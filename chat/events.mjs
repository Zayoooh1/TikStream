export function author(user) {
    const nick = user?.uniqueId || user?.nickname;
    if (!nick) return null;
    const avatar = user.avatarThumb?.urlList?.[0] || user.avatarLarge?.urlList?.[0] || '';
    return { nick: String(nick).slice(0, 100), avatar: avatar.slice(0, 1500) };
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
        like(event) {
            const total = event.totalLikeCount;
            if (Number.isSafeInteger(total) && total > 0) {
                if (taps && !taps.partial && total <= taps.count) return null;
                taps = { count: total, partial: false };
            } else {
                const delta = event.likeCount;
                if ((taps && !taps.partial) || !Number.isSafeInteger(delta) || delta <= 0) return null;
                taps = { count: (taps?.count ?? 0) + delta, partial: true };
            }
            return taps;
        },
        gift(event, now = Date.now()) {
            const details = event.giftDetails;
            const streak = details?.giftType === 1 || (!details?.giftType && details?.combo === true);
            const sender = author(event.user);
            if (!sender) return null;
            const id = event.common?.msgId;
            if (id && id !== '0') {
                if (gifts.has(id)) return null;
                gifts.add(id);
                if (gifts.size > 512) gifts.delete(gifts.values().next().value);
            }
            const name = (details?.giftName || (event.giftId ? `Gift #${event.giftId}` : 'Gift')).slice(0, 180);
            const senderId = event.user?.id && event.user.id !== '0' ? String(event.user.id) : sender.nick;
            const key = JSON.stringify([senderId, event.giftId || details?.id || name]);
            let count = Math.max(1, event.repeatCount || event.comboCount || event.groupCount || 1);
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
            return { ...sender, kind: 'gift', senderId, text: `🎁 ${name} ×${total}` };
        },
    };
}
