import { setTimeout as delay } from 'node:timers/promises';

const rawChannel = (process.argv[2] ?? '').trim().toLowerCase();
const channel = rawChannel.replace(/^https?:\/\/(www\.)?kick\.com\//i, '').replace(/^@/, '').replace(/\/.*$/, '');
if (!/^[a-z0-9_-]{1,32}$/i.test(channel)) process.exit(2);

const HEADERS = {
    'User-Agent': 'Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/131.0.0.0 Safari/537.36',
    'Accept': 'application/json',
};

const emit = event => process.stdout.write(`${JSON.stringify(event)}\n`);
const status = (state, extra = {}) => emit({ type: 'status', platform: 'kick', state, ...extra });

function cleanKickContent(raw) {
    return String(raw ?? '')
        .replace(/\[emote:\d+:([^\]]+)\]/g, ':$1:')
        .trim()
        .slice(0, 500);
}

async function resolveChannelId(slug) {
    const res = await fetch(`https://kick.com/api/v2/channels/${encodeURIComponent(slug)}`, {
        headers: HEADERS,
        signal: AbortSignal.timeout(10000),
    });
    if (res.status === 404) return { error: 'not_found' };
    if (!res.ok) return { error: `http_${res.status}` };
    const data = await res.json();
    const id = data?.id;
    if (!id) return { error: 'no_channel' };
    return { id };
}

for (;;) {
    status('connecting');
    try {
        const resolved = await resolveChannelId(channel);
        if (!resolved.id) {
            status(resolved.error === 'not_found' ? 'not_found' : 'unavailable', { code: resolved.error });
            await delay(15000);
            continue;
        }

        const channelId = resolved.id;
        const seen = new Set();
        const seenOrder = [];
        let firstPoll = true;
        let connected = false;
        let failStreak = 0;

        for (;;) {
            try {
                const res = await fetch(`https://kick.com/api/v2/channels/${channelId}/messages`, {
                    headers: HEADERS,
                    signal: AbortSignal.timeout(8000),
                });
                if (!res.ok) {
                    if (++failStreak >= 4) {
                        status('unavailable', { code: `http_${res.status}` });
                        break;
                    }
                    await delay(3000);
                    continue;
                }
                failStreak = 0;
                const json = await res.json();
                const list = Array.isArray(json?.data?.messages) ? json.data.messages : [];
                if (!connected) {
                    connected = true;
                    status('connected');
                }

                // API returns newest-first; iterate oldest-first for natural chat order
                const fresh = [];
                for (let i = list.length - 1; i >= 0; i--) {
                    const m = list[i];
                    const id = String(m?.id ?? '');
                    if (!id || seen.has(id)) continue;
                    seen.add(id);
                    seenOrder.push(id);
                    if (seenOrder.length > 400) seen.delete(seenOrder.shift());
                    if (firstPoll) continue; // skip old history before connecting
                    const nick = String(m?.sender?.username ?? '').trim().slice(0, 100);
                    const text = cleanKickContent(m?.content);
                    const color = String(m?.sender?.identity?.color ?? '').trim().slice(0, 16);
                    if (nick && text) fresh.push({ platform: 'kick', nick, text, color });
                }
                firstPoll = false;
                if (fresh.length) emit({ type: 'messages', messages: fresh });
            } catch {
                if (++failStreak >= 4) {
                    status('disconnected');
                    break;
                }
            }
            await delay(1500);
        }
    } catch {
        status('unavailable');
    }
    await delay(10000);
}
