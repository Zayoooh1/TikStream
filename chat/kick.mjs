import { setTimeout as delay } from 'node:timers/promises';
import { toNonNegativeInt } from './events.mjs';

const rawChannel = (process.argv[2] ?? '').trim().toLowerCase();
const channel = rawChannel.replace(/^https?:\/\/(www\.)?kick\.com\//i, '').replace(/^@/, '').replace(/\/.*$/, '');
if (!/^[a-z0-9_-]{1,32}$/i.test(channel)) process.exit(2);

const HEADERS = {
    'User-Agent': 'Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/131.0.0.0 Safari/537.36',
    'Accept': 'application/json',
};

// Verified Twitch global emotes (static-cdn.jtvnw.net)
const TWITCH_GLOBAL_EMOTES = {
    Kappa: '25', KappaPride: '55338', KappaRoss: '70433', KappaClaus: '74510', Keepo: '1902',
    PogChamp: '305954156', LUL: '425618', Kreygasm: '41', SeemsGood: '64138', HeyGuys: '30259',
    DansGame: '33', WutFace: '28087', NotLikeThis: '58765', VoHiYo: '81274', ResidentSleeper: '245',
    SwiftRage: '34', '4Head': '354', EleGiggle: '4339', BabyRage: '22639', FailFish: '360',
    OpieOP: '100590', TriHard: '120232', FrankerZ: '65', MrDestructoid: '28', SMOrc: '52',
    OSFrog: '81248', BloodTrail: '69', PJSalt: '36', BrokeBack: '4057', CoolStoryBob: '123171',
    MingLee: '68856', OhMyDog: '81103', Jebaited: '114836', CurseLit: '116625', VoteYea: '106293',
    VoteNay: '106294', PowerUpL: '425688', PowerUpR: '425671', GlitchCat: '304486301',
    GlitchNRG: '304489128', DoritosChip: '102242', CoolCat: '58127', SeriousSloth: '81249',
    KomodoHype: '81273', SabaPing: '160402', TPFufun: '508650', PinkMercy: '1003187',
    FootGoal: '303121417',
    ':)': '1', ':(': '2', ':D': '3', '>:(': '4', ':z': '5', 'o_O': '6', 'B)': '7', ':o': '8',
    '<3': '9', ':\\': '10', ';)': '11', ':P': '12', ';P': '13', 'R)': '14',
};

const emit = event => process.stdout.write(`${JSON.stringify(event)}\n`);
const status = (state, extra = {}) => emit({ type: 'status', platform: 'kick', state, ...extra });

async function fetchJsonSafe(url, timeoutMs = 6000) {
    try {
        const res = await fetch(url, { headers: HEADERS, signal: AbortSignal.timeout(timeoutMs) });
        if (!res.ok) return null;
        return await res.json();
    } catch {
        return null;
    }
}

function buildBaseEmotes() {
    const map = new Map();
    for (const [name, id] of Object.entries(TWITCH_GLOBAL_EMOTES)) {
        map.set(name, `https://static-cdn.jtvnw.net/emoticons/v2/${id}/default/dark/2.0`);
    }
    return map;
}

async function loadGlobalThirdPartyEmotes(map) {
    const [ffz, bttv, stv] = await Promise.all([
        fetchJsonSafe('https://api.frankerfacez.com/v1/set/global'),
        fetchJsonSafe('https://api.betterttv.net/3/cached/emotes/global'),
        fetchJsonSafe('https://7tv.io/v3/emote-sets/global'),
    ]);

    if (ffz?.sets && Array.isArray(ffz.default_sets)) {
        for (const setId of ffz.default_sets) {
            for (const e of ffz.sets[setId]?.emoticons ?? []) {
                const u = e?.urls?.['2'] || e?.urls?.['1'];
                if (e?.name && u) map.set(e.name, u.startsWith('//') ? `https:${u}` : u);
            }
        }
    }
    if (Array.isArray(bttv)) {
        for (const e of bttv) {
            if (e?.code && e?.id) {
                const ext = e.imageType || 'png';
                map.set(e.code, `https://cdn.betterttv.net/emote/${e.id}/2x.${ext}`);
            }
        }
    }
    if (Array.isArray(stv?.emotes)) {
        for (const e of stv.emotes) {
            if (e?.name && e?.id) {
                map.set(e.name, `https://cdn.7tv.app/emote/${e.id}/2x.webp`);
            }
        }
    }
}

async function loadKickChannel7tvEmotes(map, userId) {
    if (!userId) return;
    const data = await fetchJsonSafe(`https://7tv.io/v3/users/kick/${encodeURIComponent(userId)}`);
    const emotes = data?.emote_set?.emotes;
    if (!Array.isArray(emotes)) return;
    for (const e of emotes) {
        if (e?.name && e?.id) {
            map.set(e.name, `https://cdn.7tv.app/emote/${e.id}/2x.webp`);
        }
    }
}

function pushText(parts, value) {
    if (!value) return;
    const prev = parts[parts.length - 1];
    if (prev && prev.t === 's') prev.v += value;
    else parts.push({ t: 's', v: value });
}

function parseKickMessage(raw, emoteMap) {
    const str = String(raw ?? '').trim().slice(0, 500);
    if (!str) return { text: '', parts: null };

    const parts = [];
    const textPieces = [];
    let hasEmote = false;
    const kickRe = /\[emote:(\d+):([^\]]+)\]/g;
    let last = 0;

    const processPlain = segment => {
        if (!segment) return;
        const tokens = segment.split(/(\s+)/);
        for (const tok of tokens) {
            if (!tok) continue;
            if (/^\s+$/.test(tok)) {
                pushText(parts, tok);
                textPieces.push(tok);
                continue;
            }
            const url = emoteMap.get(tok);
            if (url) {
                parts.push({ t: 'e', name: tok, url });
                textPieces.push(tok);
                hasEmote = true;
            } else {
                pushText(parts, tok);
                textPieces.push(tok);
            }
        }
    };

    for (let m; (m = kickRe.exec(str)) !== null;) {
        if (m.index > last) processPlain(str.slice(last, m.index));
        const id = m[1];
        const name = m[2].trim() || 'emote';
        parts.push({ t: 'e', name, url: `https://files.kick.com/emotes/${id}/fullsize` });
        textPieces.push(name);
        hasEmote = true;
        last = m.index + m[0].length;
    }
    if (last < str.length) processPlain(str.slice(last));

    const text = textPieces.join('').trim().slice(0, 500);
    return { text, parts: hasEmote ? parts : null };
}

function extractKickViewers(data) {
    if (!data?.livestream || data.livestream.is_live === false) return null;
    return toNonNegativeInt(data.livestream.viewer_count);
}

async function resolveChannelInfo(slug) {
    const res = await fetch(`https://kick.com/api/v2/channels/${encodeURIComponent(slug)}`, {
        headers: HEADERS,
        signal: AbortSignal.timeout(10000),
    });
    if (res.status === 404) return { error: 'not_found' };
    if (!res.ok) return { error: `http_${res.status}` };
    const data = await res.json();
    const id = data?.id;
    if (!id) return { error: 'no_channel' };
    return { id, userId: data?.user_id ?? null, viewers: extractKickViewers(data) };
}

const emoteMap = buildBaseEmotes();
// Load global 7TV/BTTV/FFZ emotes asynchronously without blocking chat startup
loadGlobalThirdPartyEmotes(emoteMap).catch(() => {});

for (;;) {
    status('connecting');
    try {
        const resolved = await resolveChannelInfo(channel);
        if (!resolved.id) {
            status(resolved.error === 'not_found' ? 'not_found' : 'unavailable', { code: resolved.error });
            await delay(15000);
            continue;
        }

        const channelId = resolved.id;
        if (resolved.userId) loadKickChannel7tvEmotes(emoteMap, resolved.userId).catch(() => {});

        const seen = new Set();
        const seenOrder = [];
        let firstPoll = true;
        let connected = false;
        let failStreak = 0;
        let lastViewers = undefined;
        let viewerPollTicks = 0;
        let pollActive = true;
        const emitKickViewers = count => {
            if (count === lastViewers) return;
            lastViewers = count;
            emit({ type: 'viewers', platform: 'kick', count });
        };

        for (;;) {
            try {
                const res = await fetch(`https://kick.com/api/v2/channels/${channelId}/messages`, {
                    headers: HEADERS,
                    signal: AbortSignal.timeout(8000),
                });
                if (!res.ok) {
                    if (++failStreak >= 4) {
                        pollActive = false;
                        if (lastViewers !== null && lastViewers !== undefined) emitKickViewers(null);
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
                    if (resolved.viewers !== null) emitKickViewers(resolved.viewers);
                } else if (++viewerPollTicks >= 20) {
                    viewerPollTicks = 0;
                    fetchJsonSafe(`https://kick.com/api/v2/channels/${encodeURIComponent(channel)}`, 6000)
                        .then(info => {
                            if (pollActive && info?.id) emitKickViewers(extractKickViewers(info));
                        })
                        .catch(() => {});
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
                    const parsed = parseKickMessage(m?.content, emoteMap);
                    const color = String(m?.sender?.identity?.color ?? '').trim().slice(0, 16);
                    if (nick && parsed.text) {
                        const msg = { platform: 'kick', nick, text: parsed.text, color };
                        if (parsed.parts) msg.parts = parsed.parts;
                        fresh.push(msg);
                    }
                }
                firstPoll = false;
                if (fresh.length) emit({ type: 'messages', messages: fresh });
            } catch {
                if (++failStreak >= 4) {
                    pollActive = false;
                    if (lastViewers !== null && lastViewers !== undefined) emitKickViewers(null);
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
