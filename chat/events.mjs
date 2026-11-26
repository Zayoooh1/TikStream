export function toPositiveInt(value) {
    if (typeof value === 'bigint') {
        return value > 0n && value <= BigInt(Number.MAX_SAFE_INTEGER) ? Number(value) : null;
    }
    const n = typeof value === 'number'
        ? value
        : (typeof value === 'string' && /^\d+$/.test(value.trim()) ? Number(value.trim()) : NaN);
    return Number.isSafeInteger(n) && n > 0 ? n : null;
}

export function toNonNegativeInt(value) {
    if (typeof value === 'bigint') {
        return value >= 0n && value <= BigInt(Number.MAX_SAFE_INTEGER) ? Number(value) : null;
    }
    const n = typeof value === 'number'
        ? value
        : (typeof value === 'string' && /^\d+$/.test(value.trim()) ? Number(value.trim()) : NaN);
    return Number.isSafeInteger(n) && n >= 0 ? n : null;
}

export function extractViewerCount(event) {
    return toNonNegativeInt(event?.viewerCount ?? event?.total);
}

export function extractInitialViewerCount(roomInfo) {
    return toNonNegativeInt(
        roomInfo?.viewerCount
        ?? roomInfo?.userCount
        ?? roomInfo?.user_count
        ?? roomInfo?.viewer_count
        ?? roomInfo?.stats?.viewerCount
        ?? roomInfo?.stats?.userCount
        ?? roomInfo?.stats?.user_count
        ?? roomInfo?.data?.user_count
        ?? roomInfo?.data?.viewer_count
        ?? roomInfo?.data?.viewerCount
        ?? roomInfo?.data?.userCount
        ?? roomInfo?.data?.stats?.user_count
        ?? roomInfo?.data?.stats?.viewerCount
    );
}

export function author(user) {
    const nick = user?.displayId || user?.uniqueId || user?.nickname;
    if (!nick) return null;
    const avatar = user.avatarThumb?.urlList?.[0]
        || user.avatarMedium?.urlList?.[0]
        || user.avatarLarge?.urlList?.[0]
        || user.avatarThumb?.url
        || '';
    return { nick: String(nick).slice(0, 100), avatar: String(avatar).slice(0, 1500) };
}

const TIKTOK_EMOJIS = {
    smile: '😊',
    happy: '😄',
    angry: '😠',
    cry: '😢',
    embarrassed: '😳',
    surprised: '😮',
    wronged: '🥺',
    shout: '😱',
    flushed: '😳',
    yummy: '😋',
    complacent: '😎',
    drool: '🤤',
    scream: '😱',
    weep: '😭',
    speechless: '😶',
    funnyface: '😜',
    laughwithtears: '😂',
    laughcry: '😂',
    wicked: '😈',
    facewithrollingeyes: '🙄',
    sulk: '😒',
    thinking: '🤔',
    lovely: '🥰',
    greedy: '🤑',
    wow: '🤩',
    joyful: '😁',
    hehe: '😏',
    slap: '🤦',
    tears: '🥲',
    stun: '😲',
    cute: '🥺',
    blink: '😉',
    disdain: '😒',
    astonish: '😮',
    rage: '🤬',
    cool: '😎',
    excited: '🤩',
    proud: '😌',
    smileface: '🙂',
    evil: '😈',
    angel: '😇',
    laugh: '😆',
    pride: '😤',
    nap: '😴',
    loveface: '😍',
    awkward: '😬',
    shock: '🤯',
    thanks: '🙏',
    thankyou: '🙏',
    clap: '👏',
    heart: '❤️',
    love: '❤️',
    fire: '🔥',
    rose: '🌹',
    thumbsup: '👍',
    ok: '👌',
    kiss: '😘',
    wink: '😉',
    hug: '🤗',
    party: '🥳',
    star: '⭐',
    crown: '👑',
    gg: '🎮',
    rip: '💀',
    skull: '💀',
    eyes: '👀',
    100: '💯',
    lol: '😂',
    sad: '😔',
    mad: '😡',
    shy: '🙈',
    sweat: '😅',
    sick: '🤢',
    vomit: '🤮',
    sleep: '😴',
    ghost: '👻',
    poop: '💩',
    clown: '🤡',
    monkey: '🙈',
    wave: '👋',
    pray: '🙏',
    muscle: '💪',
    trophy: '🏆',
};

export function replaceTikTokEmotes(text) {
    return String(text ?? '').replace(/\[([a-z0-9_ -]{1,32})\]/gi, (match, rawKey) => {
        const key = rawKey.toLowerCase().replace(/[\s_-]+/g, '');
        return TIKTOK_EMOJIS[key] ?? match;
    });
}

export function extractChatText(event) {
    const raw = event?.content ?? event?.comment ?? '';
    const trimmed = replaceTikTokEmotes(raw).trim();
    if (trimmed) return trimmed.slice(0, 500);
    const emotes = Array.isArray(event?.emotes) ? event.emotes : (Array.isArray(event?.emoteList) ? event.emoteList : []);
    if (emotes.length > 0) return emotes.length === 1 ? '✨' : '✨'.repeat(Math.min(emotes.length, 5));
    return '';
}

function numericCode(value) {
    return Number.isInteger(value) && value >= 100 && value <= 5999 ? value : undefined;
}

export function classifyError(error) {
    const ctor = typeof error?.constructor?.name === 'string' && error.constructor.name !== 'Error' && error.constructor.name !== 'Object'
        ? error.constructor.name : '';
    const name = ctor || (typeof error?.name === 'string' ? error.name : '');
    const httpStatus = numericCode(error?.response?.status) ?? numericCode(error?.status) ?? numericCode(error?.statusCode)
        ?? numericCode(error?.cause?.response?.status) ?? numericCode(error?.cause?.status);
    const rawCode = error?.code ?? error?.cause?.code;
    const sysCode = typeof rawCode === 'string' && /^[A-Z0-9_]{2,24}$/.test(rawCode) ? rawCode : undefined;
    const code = httpStatus ?? numericCode(rawCode) ?? sysCode;

    if (name === 'UserOfflineError') return { state: 'offline' };
    if (name === 'SignatureRateLimitError' || httpStatus === 429) {
        return { state: 'rate_limited', ...(code !== undefined ? { code } : {}) };
    }
    if (name === 'SignAPIError' || name === 'SignatureMissingTokensError' || name === 'PremiumFeatureError') {
        return { state: 'sign_error', ...(code !== undefined ? { code } : {}) };
    }
    if (name === 'ConnectTimeoutError' || name === 'AuthenticatedWebSocketConnectionError'
        || sysCode === 'ETIMEDOUT' || sysCode === 'ECONNRESET' || sysCode === 'ECONNREFUSED' || sysCode === 'ENOTFOUND') {
        return { state: 'ws_error', ...(code !== undefined ? { code } : {}) };
    }
    if (name === 'InvalidUniqueIdError' || name === 'InvalidRequestError'
        || name === 'InvalidResponseError' || name === 'InvalidResponseCompositeError'
        || httpStatus === 403 || httpStatus === 404) {
        return { state: 'room_error', ...(code !== undefined ? { code } : {}) };
    }
    return { state: 'unavailable', ...(code !== undefined ? { code } : {}) };
}

export function liveEvents() {
    let roomId;
    let taps = null;
    let giftTotalCount = 0;
    let giftTotalDiamonds = 0;
    const gifts = new Set();
    const follows = new Map();
    const series = new Map();
    const bursts = new Map();

    function remember(map, key, value) {
        map.delete(key);
        map.set(key, value);
        if (map.size > 512) map.delete(map.keys().next().value);
    }

    return {
        enterRoom(id) {
            if (!id || id === roomId) return false;
            roomId = id;
            taps = null;
            giftTotalCount = 0;
            giftTotalDiamonds = 0;
            gifts.clear();
            follows.clear();
            series.clear();
            bursts.clear();
            return true;
        },
        follow(event, now = Date.now()) {
            const sender = author(event.user);
            if (!sender) return null;
            const key = event.user?.id && event.user.id !== '0' ? String(event.user.id) : sender.nick;
            const prev = follows.get(key);
            if (prev && now - prev < 30000) return null;
            remember(follows, key, now);
            return { ...sender, kind: 'follow', text: 'just followed!' };
        },
        seedLikes(rawTotal) {
            const total = toPositiveInt(rawTotal);
            if (!total) return null;
            if (taps && total <= taps.count) return null;
            taps = { count: total, partial: false };
            return taps;
        },
        like(event) {
            const total = toPositiveInt(event?.total ?? event?.totalLikeCount);
            const delta = toPositiveInt(event?.count ?? event?.likeCount);
            if (total) {
                const next = taps ? Math.max(taps.count, total) : total;
                if (taps && !taps.partial && next <= taps.count) {
                    if (!delta) return null;
                    taps = { count: taps.count + delta, partial: false };
                    return taps;
                }
                taps = { count: next, partial: false };
                return taps;
            }
            if (delta) {
                taps = { count: (taps?.count ?? 0) + delta, partial: !(taps && !taps.partial) };
                return taps;
            }
            return null;
        },
        gift(event, now = Date.now()) {
            const details = event.gift ?? event.giftDetails ?? event.extendedGiftInfo;
            const gType = details?.type ?? details?.giftType;
            const streak = gType === 1 || (!gType && details?.combo === true);
            const sender = author(event.user);
            if (!sender) return null;
            const id = event.common?.msgId;
            if (id && id !== '0') {
                if (gifts.has(id)) return null;
                gifts.add(id);
                if (gifts.size > 512) gifts.delete(gifts.values().next().value);
            }
            const name = String(details?.name || details?.giftName || (event.giftId ? `Gift #${event.giftId}` : 'Gift')).slice(0, 180);
            const senderId = event.user?.id && event.user.id !== '0' ? String(event.user.id) : sender.nick;
            const key = JSON.stringify([senderId, event.giftId || details?.id || name]);
            let count = Math.max(1, toPositiveInt(event.repeatCount) || toPositiveInt(event.comboCount) || toPositiveInt(event.groupCount) || 1);
            let continuing = false;
            if (streak) {
                const group = event.groupId && event.groupId !== '0' ? event.groupId : '';
                const seriesKey = JSON.stringify([key, group]);
                const previous = series.get(seriesKey);
                continuing = Boolean(previous && (group || (!previous.finished && now - previous.time <= 6000)));
                const progress = continuing ? Math.max(previous.count, count) : count;
                remember(series, seriesKey, { count: progress, finished: Boolean(event.repeatEnd), time: now });
                count = progress - (continuing ? previous.count : 0);
                if (!count) return null;
            }
            const previous = bursts.get(key);
            const total = (previous && (continuing || now - previous.time <= 6000) ? previous.count : 0) + count;
            remember(bursts, key, { count: total, time: now });
            const unitDiamonds = toPositiveInt(details?.diamondCount ?? details?.diamond_count ?? event?.diamondCount) || 0;
            giftTotalCount += count;
            giftTotalDiamonds += unitDiamonds * count;
            return {
                ...sender,
                kind: 'gift',
                senderId,
                giftName: name,
                giftCount: count,
                diamonds: unitDiamonds * count,
                text: `🎁 ${name} ×${total}`,
                totalGifts: giftTotalCount,
                totalDiamonds: giftTotalDiamonds,
            };
        },
        giftStats() {
            return { count: giftTotalCount, diamonds: giftTotalDiamonds };
        },
    };
}
