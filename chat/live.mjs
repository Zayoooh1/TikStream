import { setTimeout as delay } from 'node:timers/promises';
import { TikTokLiveConnection, WebcastEvent, ControlEvent } from 'tiktok-live-connector';
import { author, classifyError, extractChatText, liveEvents } from './events.mjs';

const account = process.argv[2];
if (!/^[a-z\d_.]{1,24}$/i.test(account ?? '')) process.exit(2);

const emit = event => process.stdout.write(`${JSON.stringify(event)}\n`);
const status = (state, extra = {}) => emit({ type: 'status', state, ...extra });
let pending = [];
let flush;
let blocked = false;
const events = liveEvents();
let taps;
let tapsFlush;
process.stdout.on('drain', () => { blocked = false; scheduleFlush(); scheduleTaps(); });
function scheduleTaps() {
    if (!taps || blocked || tapsFlush) return;
    tapsFlush = setTimeout(() => {
        tapsFlush = undefined;
        if (blocked || !taps) return;
        const payload = taps;
        taps = undefined;
        blocked = !emit({ type: 'taps', ...payload });
    }, 250);
}

function scheduleFlush() {
    if (!pending.length || blocked || flush) return;
    flush = setTimeout(() => {
        flush = undefined;
        if (blocked) return;
        const messages = pending;
        pending = [];
        blocked = !emit({ type: 'messages', messages });
    }, 45);
}

function enqueue(message) {
    if (pending.length === 128) pending.shift();
    pending.push(message);
    scheduleFlush();
}

for (;;) {
    status('connecting');
    const client = new TikTokLiveConnection(account, {
        processInitialData: false,
        fetchRoomInfoOnConnect: true,
        webClientOptions: { timeout: { request: 10000 } },
        wsClientOptions: { handshakeTimeout: 10000 },
        ...(process.env.TIKSTREAM_EULER_API_KEY ? { signApiKey: process.env.TIKSTREAM_EULER_API_KEY } : {}),
    });
    client.on(ControlEvent.ERROR, err => {
        const msg = err?.info || err?.exception?.message || err?.message || String(err ?? '');
        if (msg) process.stderr.write(`[tiktok-chat] ${msg.slice(0, 240)}\n`);
    });
    const updateRoom = id => {
        if (!events.enterRoom(id)) return;
        pending = pending.filter(message => message.kind !== 'gift');
        emit({ type: 'clearGifts' });
        emit({ type: 'giftStats', count: 0, diamonds: 0 });
        taps = { count: null };
        scheduleTaps();
    };
    client.on(WebcastEvent.CHAT, event => {
        const sender = author(event.user);
        const text = extractChatText(event);
        if (sender && text) enqueue({ platform: 'tiktok', ...sender, text });
    });
    if (WebcastEvent.EMOTE) {
        client.on(WebcastEvent.EMOTE, event => {
            const sender = author(event.user);
            const text = extractChatText(event);
            if (sender && text) enqueue({ platform: 'tiktok', ...sender, text });
        });
    }
    client.on(WebcastEvent.GIFT, event => {
        updateRoom(client.roomId || event.common?.roomId);
        const gift = events.gift(event);
        if (gift) {
            enqueue(gift);
            emit({ type: 'giftStats', count: gift.totalGifts, diamonds: gift.totalDiamonds });
        }
    });
    if (WebcastEvent.FOLLOW) {
        client.on(WebcastEvent.FOLLOW, event => {
            updateRoom(client.roomId || event.common?.roomId);
            const flw = events.follow(event);
            if (flw) enqueue(flw);
        });
    }
    if (WebcastEvent.SOCIAL) {
        client.on(WebcastEvent.SOCIAL, event => {
            const dt = String(event.displayType || event.common?.displayText?.key || '').toLowerCase();
            if (!dt.includes('follow')) return;
            updateRoom(client.roomId || event.common?.roomId);
            const flw = events.follow(event);
            if (flw) enqueue(flw);
        });
    }
    client.on(WebcastEvent.LIKE, event => {
        updateRoom(client.roomId || event.common?.roomId);
        const latest = events.like(event);
        if (latest) {
            taps = latest;
            scheduleTaps();
        }
    });
    let streamEnded = false;
    client.on(WebcastEvent.STREAM_END, () => { streamEnded = true; });
    const disconnected = new Promise(resolve => client.once(ControlEvent.DISCONNECTED, resolve));
    try {
        const connection = await client.connect();
        updateRoom(connection.roomId);
        const initialLikes = connection?.roomInfo?.likeCount
            ?? connection?.roomInfo?.stats?.likeCount
            ?? connection?.roomInfo?.like_count
            ?? connection?.roomInfo?.data?.like_count;
        const seeded = events.seedLikes(initialLikes);
        if (seeded) {
            taps = seeded;
            scheduleTaps();
        }
        status('connected');
        const disconnectEvent = await disconnected;
        if (streamEnded) {
            status('offline');
        } else {
            const wsCode = Number.isInteger(disconnectEvent?.code) ? disconnectEvent.code : undefined;
            status('disconnected', wsCode !== undefined ? { code: wsCode } : {});
        }
    } catch (error) {
        const classified = classifyError(error);
        status(classified.state, classified.code !== undefined ? { code: classified.code } : {});
    } finally {
        await client.disconnect().catch(() => {});
        client.removeAllListeners();
    }
    await delay(30000);
}
