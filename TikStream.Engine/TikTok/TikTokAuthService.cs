using System.Net;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json;
using TikStream.Core;

namespace TikStream.TikTok;

public sealed record TikTokCreateInfoResult(
    bool Ok,
    bool Eligible,
    string LiveStatus,
    string LastRoomId,
    string Host,
    string Message,
    string? Error = null);

public sealed record TikTokCreateRoomResult(
    bool Ok,
    string RoomId,
    string ServerUrl,
    string StreamKey,
    string FullRtmpUrl,
    string ShareUrl,
    string? Error = null);

/// <summary>
/// Manages TikTok LIVE Studio QR login, session persistence, account profile lookup,
/// and webcast/room/create_info eligibility verification.
/// All network I/O runs asynchronously off the render thread and off NativeApi's global lock.
/// </summary>
public sealed class TikTokAuthService
{
    private const string DefaultLiveStudioVersion = "1.40.2";
    private const string PassportWebSdkVersion = "2.1.9";
    private const string PassportAppKey = "884c28a44b61b78f9d837fc8b0967178";
    private const string TicketGuardPublicKey = "BHTyDu4GfY+Se8QoOlL22ARkOv4aLE5LBCk8eSiJ6K5N5m8Wg3cPeVYWVYNDrJ3hsRmaVCgNl/AKbMW67VBievw=";
    private const string Aid = "8311";

    private static readonly string[] PassportHosts =
    [
        "api16-normal-no1a.tiktokv.eu",
        "api16-normal-c-alisg.tiktokv.com",
        "api16-normal-useast8.tiktokv.us",
        "api16-normal-useast5.tiktokv.us",
    ];

    private static readonly string[] WebcastHosts =
    [
        "webcast16-normal-no1a.tiktokv.eu",
        "webcast16-normal-c-alisg.tiktokv.com",
        "webcast16-normal-useast8.tiktokv.us",
        "webcast16-normal-useast5.tiktokv.us",
    ];

    private sealed class QrFlowState : IDisposable
    {
        public readonly CancellationTokenSource Cts = new(TimeSpan.FromMinutes(5));
        public readonly CookieContainer Jar = new();
        public readonly HttpClient Http;
        public readonly ITikTokSigner? Signer;
        public readonly string DeviceId = TikTokSessionData.GenerateNumericId();
        public readonly string InstallId = TikTokSessionData.GenerateNumericId();
        public string Status = "loading"; // loading | new | scanned | confirmed | expired | error
        public string StatusText = "Generowanie kodu QR…";
        public string Token = "";
        public string QrCodeBase64 = "";
        public string PassportHost = PassportHosts[0];
        public string? Error;
        public TikTokSessionData? CompletedSession;
        public Task? PollTask;

        public QrFlowState(ITikTokSigner? signer)
        {
            Signer = signer;
            var handler = new SocketsHttpHandler
            {
                CookieContainer = Jar,
                UseCookies = true,
                AutomaticDecompression = DecompressionMethods.All,
                PooledConnectionLifetime = TimeSpan.FromMinutes(2),
            };
            Http = new HttpClient(handler) { Timeout = TimeSpan.FromSeconds(25) };
        }

        public void Dispose()
        {
            try { Cts.Cancel(); } catch { }
            Cts.Dispose();
            Http.Dispose();
        }
    }

    private readonly object _gate = new();
    private QrFlowState? _qr;

    public object BeginQrLogin(string? signerKey, string? signerUrl, Action<TikTokSessionData> onSessionConfirmed)
    {
        // QR login (/passport/web/get_qrcode & check_qrconnect) uses local Passport SDK signing (MD5+XOR5)
        // and does NOT require any external RapidAPI key.
        var candidate = new RapidApiTikTokSigner(signerKey, signerUrl);
        ITikTokSigner? signer = candidate.IsConfigured ? candidate : null;

        QrFlowState flow;
        lock (_gate)
        {
            _qr?.Dispose();
            flow = new QrFlowState(signer);
            _qr = flow;
        }

        _ = Task.Run(() => RunQrInitAsync(flow, onSessionConfirmed));
        return new { ok = true, status = "loading", statusText = flow.StatusText };
    }

    public object PollQrStatus(Action<TikTokSessionData> onSessionConfirmed)
    {
        QrFlowState? flow;
        lock (_gate) flow = _qr;
        if (flow == null)
            return new { active = false, status = "idle" };

        string status, statusText, qrB64;
        string? error;
        TikTokSessionData? done;
        bool shouldTriggerPoll = false;

        lock (_gate)
        {
            status = flow.Status;
            statusText = flow.StatusText;
            qrB64 = flow.QrCodeBase64;
            error = flow.Error;
            done = flow.CompletedSession;
            if ((status is "new" or "scanned") && !string.IsNullOrEmpty(flow.Token) && (flow.PollTask == null || flow.PollTask.IsCompleted))
            {
                shouldTriggerPoll = true;
                flow.PollTask = Task.Run(() => RunQrPollOnceAsync(flow, onSessionConfirmed));
            }
        }

        _ = shouldTriggerPoll;
        return new
        {
            active = status is "loading" or "new" or "scanned",
            status,
            statusText,
            qrcodeB64 = qrB64,
            error,
            account = done == null ? null : FormatAccount(done),
        };
    }

    public void CancelQrLogin()
    {
        lock (_gate)
        {
            _qr?.Dispose();
            _qr = null;
        }
    }

    public static object FormatAccount(TikTokSessionData? session)
    {
        if (session == null || !session.HasSessionCookie)
            return new { loggedIn = false };
        return new
        {
            loggedIn = true,
            userId = session.UserId,
            username = session.Username,
            nickname = session.Nickname,
            displayName = session.DisplayName,
            region = session.Region,
            source = session.Source,
            webcastDomain = session.WebcastDomain,
        };
    }

    private async Task RunQrInitAsync(QrFlowState flow, Action<TikTokSessionData> onSessionConfirmed)
    {
        try
        {
            var ct = flow.Cts.Token;
            var verifyFp = "verify_" + flow.DeviceId;
            var baseParams = new Dictionary<string, string>
            {
                ["next"] = "https://www.tiktok.com",
                ["device_id"] = flow.DeviceId,
                ["aid"] = Aid,
                ["account_sdk_source"] = "web",
                ["sdk_version"] = PassportWebSdkVersion,
                ["verifyFp"] = verifyFp,
            };
            ApplyPassportSdkSignature(baseParams);

            Exception? lastEx = null;
            foreach (var host in PassportHosts[..2])
            {
                try
                {
                    var url = $"https://{host}/passport/web/get_qrcode/";
                    using var doc = await SendSignedJsonAsync(
                        flow.Http, flow.Signer, HttpMethod.Get, url, baseParams,
                        flow.DeviceId, isPassport: true, ct).ConfigureAwait(false);

                    var root = doc.RootElement;
                    if (!root.TryGetProperty("data", out var data) || data.ValueKind != JsonValueKind.Object)
                        continue;

                    var token = ReadStr(data, "token");
                    var qrB64 = ReadStr(data, "qrcode");
                    if (string.IsNullOrEmpty(token) || string.IsNullOrEmpty(qrB64))
                    {
                        var desc = ReadStr(data, "description");
                        if (!string.IsNullOrEmpty(desc))
                            lastEx = new InvalidOperationException($"TikTok ({host}): {desc}");
                        continue;
                    }

                    lock (_gate)
                    {
                        if (_qr != flow) return;
                        flow.Token = token;
                        flow.QrCodeBase64 = qrB64;
                        flow.PassportHost = host;
                        flow.Status = "new";
                        flow.StatusText = "Zeskanuj kod QR w aplikacji TikTok na telefonie.";
                    }
                    Log.Info($"TikTok QR code obtained from {host}");
                    return;
                }
                catch (Exception ex) when (ex is not OperationCanceledException)
                {
                    lastEx = ex;
                    Log.Warn($"TikTok get_qrcode failed on {host}: {ex.Message}");
                }
            }

            throw lastEx ?? new InvalidOperationException("Nie udało się pobrać kodu QR z serwerów TikTok.");
        }
        catch (OperationCanceledException)
        {
            lock (_gate)
            {
                if (_qr == flow) { flow.Status = "expired"; flow.StatusText = "Logowanie QR wygasło lub zostało anulowane."; }
            }
        }
        catch (Exception ex)
        {
            Log.Warn("TikTok QR init error: " + ex.Message);
            lock (_gate)
            {
                if (_qr == flow)
                {
                    flow.Status = "error";
                    flow.Error = ex.Message;
                    flow.StatusText = ex.Message;
                }
            }
        }
        _ = onSessionConfirmed;
    }

    private async Task RunQrPollOnceAsync(QrFlowState flow, Action<TikTokSessionData> onSessionConfirmed)
    {
        try
        {
            var ct = flow.Cts.Token;
            var verifyFp = "verify_" + flow.DeviceId;
            var query = new Dictionary<string, string>
            {
                ["next"] = "https://www.tiktok.com",
                ["token"] = flow.Token,
                ["multi_login"] = "1",
                ["device_id"] = flow.DeviceId,
                ["aid"] = Aid,
                ["account_sdk_source"] = "web",
                ["sdk_version"] = PassportWebSdkVersion,
                ["verifyFp"] = verifyFp,
            };
            ApplyPassportSdkSignature(query);

            var hosts = new List<string> { flow.PassportHost };
            foreach (var h in PassportHosts)
                if (!hosts.Contains(h, StringComparer.OrdinalIgnoreCase)) hosts.Add(h);

            string bestStatus = "";
            string matchedHost = flow.PassportHost;

            foreach (var host in hosts)
            {
                try
                {
                    var url = $"https://{host}/passport/web/check_qrconnect/";
                    using var doc = await SendSignedJsonAsync(
                        flow.Http, flow.Signer, HttpMethod.Get, url, query,
                        flow.DeviceId, isPassport: true, ct).ConfigureAwait(false);

                    var root = doc.RootElement;
                    if (!root.TryGetProperty("data", out var data) || data.ValueKind != JsonValueKind.Object)
                        continue;

                    var st = ReadStr(data, "status");
                    if (string.IsNullOrEmpty(st)) st = ReadStr(data, "qr_status");
                    if (string.IsNullOrEmpty(st)) st = ReadStr(data, "state");
                    if (string.IsNullOrEmpty(st)) continue;

                    bestStatus = st;
                    matchedHost = host;
                    if (st is "confirmed" or "scanned" or "expired")
                        break;
                }
                catch (Exception ex) when (ex is not OperationCanceledException)
                {
                    Log.Warn($"TikTok check_qrconnect failed on {host}: {ex.Message}");
                }
            }

            if (bestStatus == "scanned")
            {
                lock (_gate)
                {
                    if (_qr == flow && flow.Status != "confirmed")
                    {
                        flow.Status = "scanned";
                        flow.StatusText = "Kod zeskanowany — potwierdź logowanie na telefonie.";
                    }
                }
            }
            else if (bestStatus == "expired")
            {
                lock (_gate)
                {
                    if (_qr == flow)
                    {
                        flow.Status = "expired";
                        flow.StatusText = "Kod QR wygasł. Kliknij ponownie, aby wygenerować nowy.";
                    }
                }
            }
            else if (bestStatus == "confirmed")
            {
                var session = await FinalizeConfirmedSessionAsync(flow, matchedHost, ct).ConfigureAwait(false);
                onSessionConfirmed(session);
                lock (_gate)
                {
                    if (_qr == flow)
                    {
                        flow.CompletedSession = session;
                        flow.Status = "confirmed";
                        flow.StatusText = "Zalogowano jako " + session.DisplayName;
                    }
                }
                Log.Info($"TikTok QR login confirmed: {session.DisplayName} (uid={session.UserId}, host={matchedHost})");
            }
        }
        catch (OperationCanceledException) { }
        catch (Exception ex)
        {
            Log.Warn("TikTok QR poll error: " + ex.Message);
            lock (_gate)
            {
                if (_qr == flow)
                {
                    flow.Status = "error";
                    flow.Error = ex.Message;
                    flow.StatusText = ex.Message;
                }
            }
        }
    }

    private static async Task<TikTokSessionData> FinalizeConfirmedSessionAsync(QrFlowState flow, string passportHost, CancellationToken ct)
    {
        var session = new TikTokSessionData
        {
            DeviceId = flow.DeviceId,
            InstallId = flow.InstallId,
            PassportDomain = passportHost,
            WebcastDomain = MapPassportToWebcastHost(passportHost),
            Source = "qr",
            UpdatedAtUnix = DateTimeOffset.UtcNow.ToUnixTimeSeconds(),
        };

        // Extract cookies collected across passport hosts
        var seen = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
        foreach (var host in new[] { passportHost }.Concat(PassportHosts).Distinct(StringComparer.OrdinalIgnoreCase))
        {
            foreach (Cookie c in flow.Jar.GetCookies(new Uri($"https://{host}/")))
            {
                if (string.IsNullOrEmpty(c.Name) || string.IsNullOrEmpty(c.Value)) continue;
                if (!seen.Add(c.Name)) continue;
                session.Cookies.Add(new TikTokCookieEntry
                {
                    Name = c.Name,
                    Value = c.Value,
                    Domain = string.IsNullOrEmpty(c.Domain) ? ".tiktokv.com" : c.Domain,
                    Path = string.IsNullOrEmpty(c.Path) ? "/" : c.Path,
                });
            }
        }

        if (!session.HasSessionCookie)
            throw new InvalidOperationException("TikTok potwierdził QR, ale nie zwrócił ciasteczka sesji (sessionid/sid_tt).");

        // Fetch account profile via passport/account/info/v2/
        try
        {
            var verifyFp = "verify_" + flow.DeviceId;
            var infoParams = new Dictionary<string, string>
            {
                ["aid"] = Aid,
                ["device_id"] = flow.DeviceId,
                ["account_sdk_source"] = "web",
                ["sdk_version"] = PassportWebSdkVersion,
                ["verifyFp"] = verifyFp,
            };
            ApplyPassportSdkSignature(infoParams);

            var url = $"https://{passportHost}/passport/account/info/v2/";
            using var doc = await SendSignedJsonAsync(
                flow.Http, flow.Signer, HttpMethod.Get, url, infoParams,
                flow.DeviceId, isPassport: true, ct).ConfigureAwait(false);

            if (doc.RootElement.TryGetProperty("data", out var data) && data.ValueKind == JsonValueKind.Object)
            {
                session.UserId = ReadStr(data, "user_id_str");
                if (string.IsNullOrEmpty(session.UserId)) session.UserId = ReadStr(data, "user_id");
                session.Username = ReadStr(data, "username");
                if (string.IsNullOrEmpty(session.Username)) session.Username = ReadStr(data, "screen_name");
                session.Nickname = ReadStr(data, "name");
                if (string.IsNullOrEmpty(session.Nickname)) session.Nickname = ReadStr(data, "screen_name");
                session.Region = ReadStr(data, "region");
            }
        }
        catch (Exception ex)
        {
            Log.Warn("TikTok account/info/v2 lookup warning: " + ex.Message);
        }

        return session;
    }

    public static async Task<TikTokCreateInfoResult> CheckCreateInfoAsync(
        TikTokSessionData session,
        string? signerKey,
        string? signerUrl,
        CancellationToken ct = default)
    {
        if (!session.HasSessionCookie)
            return new TikTokCreateInfoResult(false, false, "", "", "", "", "Brak aktywnej sesji TikTok. Zaloguj się najpierw przez QR lub LIVE Studio.");

        var candidate = new RapidApiTikTokSigner(signerKey, signerUrl);
        ITikTokSigner? signer = candidate.IsConfigured ? candidate : null;

        session.EnsureDeviceIds();
        var jar = new CookieContainer();
        var candidateHosts = new List<string>();
        if (!string.IsNullOrWhiteSpace(session.WebcastDomain))
            candidateHosts.Add(session.WebcastDomain);
        foreach (var h in WebcastHosts)
            if (!candidateHosts.Contains(h, StringComparer.OrdinalIgnoreCase)) candidateHosts.Add(h);

        session.PopulateCookieContainer(jar, candidateHosts);

        using var handler = new SocketsHttpHandler
        {
            CookieContainer = jar,
            UseCookies = true,
            AutomaticDecompression = DecompressionMethods.All,
        };
        using var http = new HttpClient(handler) { Timeout = TimeSpan.FromSeconds(25) };

        var query = BuildStudioParams(session.DeviceId, session.InstallId, session.Region);
        query["last_time_hashtag_id"] = "5";
        query["live_studio"] = "1";

        string? lastError = null;
        foreach (var host in candidateHosts)
        {
            try
            {
                var url = $"https://{host}/webcast/room/create_info/";
                using var doc = await SendSignedJsonAsync(
                    http, signer, HttpMethod.Get, url, query,
                    session.DeviceId, isPassport: false, ct).ConfigureAwait(false);

                var root = doc.RootElement;
                var statusCode = ReadStr(root, "status_code");
                var hasData = root.TryGetProperty("data", out var data) && data.ValueKind == JsonValueKind.Object;

                if (statusCode != "0" && !string.IsNullOrEmpty(statusCode))
                {
                    var prompt = hasData ? ReadStr(data, "prompts") : "";
                    if (string.IsNullOrEmpty(prompt) && hasData) prompt = ReadStr(data, "message");
                    if (string.IsNullOrEmpty(prompt)) prompt = ReadStr(root, "message");
                    lastError = $"TikTok status_code={statusCode}" + (string.IsNullOrEmpty(prompt) ? "" : $": {prompt}");
                    Log.Warn($"TikTok create_info rejected on {host}: {lastError}");
                    // 20003 / 20006 / 4003088 etc. are account-level responses, no need to retry other regions if clear message
                    if (statusCode is "20003" or "4003088" or "4003025" or "4014257")
                        return new TikTokCreateInfoResult(false, false, statusCode, "", host, prompt, lastError);
                    continue;
                }

                string liveStatus = hasData ? ReadStr(data, "live_status") : "";
                string lastRoomId = hasData ? ReadStr(data, "last_room_id_str") : "";
                if (string.IsNullOrEmpty(lastRoomId) && hasData) lastRoomId = ReadStr(data, "last_room_id");

                session.WebcastDomain = host;
                string msg = liveStatus == "2"
                    ? $"Konto ma uprawnienia LIVE (wykryto otwarty pokój {lastRoomId}) — host: {host}."
                    : $"Konto gotowe do transmisji TikTok LIVE — host: {host}.";

                Log.Info($"TikTok create_info OK on {host} (live_status={liveStatus}, last_room_id={lastRoomId})");
                return new TikTokCreateInfoResult(true, true, liveStatus, lastRoomId, host, msg);
            }
            catch (Exception ex) when (ex is not OperationCanceledException)
            {
                lastError = ex.Message;
                Log.Warn($"TikTok create_info failed on {host}: {ex.Message}");
            }
        }

        return new TikTokCreateInfoResult(false, false, "", "", "", "", lastError ?? "Nie udało się połączyć z webcast/room/create_info/.");
    }

    public static async Task<TikTokCreateRoomResult> CreateRoomAsync(
        TikTokSessionData session,
        string title,
        string category,
        string? signerKey,
        string? signerUrl,
        CancellationToken ct = default)
    {
        if (!session.HasSessionCookie)
            return new TikTokCreateRoomResult(false, "", "", "", "", "", "No active TikTok session. Sign in via QR code or LIVE Studio import first.");

        var candidate = new RapidApiTikTokSigner(signerKey, signerUrl);
        ITikTokSigner? signer = candidate.IsConfigured ? candidate : null;

        session.EnsureDeviceIds();
        var jar = new CookieContainer();
        var candidateHosts = new List<string>();
        if (!string.IsNullOrWhiteSpace(session.WebcastDomain))
            candidateHosts.Add(session.WebcastDomain);
        foreach (var h in WebcastHosts)
            if (!candidateHosts.Contains(h, StringComparer.OrdinalIgnoreCase)) candidateHosts.Add(h);
        session.PopulateCookieContainer(jar, candidateHosts);

        using var handler = new SocketsHttpHandler
        {
            CookieContainer = jar,
            UseCookies = true,
            AutomaticDecompression = DecompressionMethods.All,
        };
        using var http = new HttpClient(handler) { Timeout = TimeSpan.FromSeconds(25) };

        var query = BuildStudioParams(session.DeviceId, session.InstallId, session.Region);
        query["live_studio"] = "1";

        string safeTitle = string.IsNullOrWhiteSpace(title) ? "LIVE" : title.Trim();
        if (safeTitle.Length > 64) safeTitle = safeTitle[..64];
        var (hashtagId, gameTagId) = ResolveCategoryTag(category);

        var form = new Dictionary<string, string>
        {
            ["title"] = safeTitle,
            ["live_studio"] = "1",
            ["gen_replay"] = "false",
            ["chat_sub_only_auth"] = "2",
            ["chat_l2"] = "1",
            ["gift_auth"] = "1",
            ["star_comment_switch"] = "true",
            ["multi_stream_scene"] = "0",
            ["multi_stream_source"] = "1",
            ["hashtag_id"] = hashtagId,
            ["game_tag_id"] = gameTagId,
            ["create_source"] = "0",
        };

        string? lastError = null;
        foreach (var host in candidateHosts)
        {
            try
            {
                var url = $"https://{host}/webcast/room/create/";
                using var doc = await SendSignedJsonAsync(
                    http, signer, HttpMethod.Post, url, query,
                    session.DeviceId, isPassport: false, ct, formBody: form).ConfigureAwait(false);

                var root = doc.RootElement;
                var statusCode = ReadStr(root, "status_code");
                var hasData = root.TryGetProperty("data", out var data) && data.ValueKind == JsonValueKind.Object;

                if (statusCode != "0" && !string.IsNullOrEmpty(statusCode))
                {
                    var prompt = hasData ? ReadStr(data, "prompts") : "";
                    if (string.IsNullOrEmpty(prompt) && hasData) prompt = ReadStr(data, "message");
                    if (string.IsNullOrEmpty(prompt)) prompt = ReadStr(root, "message");
                    lastError = $"TikTok status_code={statusCode}" + (string.IsNullOrEmpty(prompt) ? "" : $": {prompt}");
                    Log.Warn($"TikTok room/create rejected on {host}: {lastError}");
                    if (statusCode is "20003" or "4003088" or "4003025" or "4014257")
                        return new TikTokCreateRoomResult(false, "", "", "", "", "", lastError);
                    continue;
                }

                if (!hasData)
                {
                    lastError = "TikTok room/create returned empty data.";
                    continue;
                }

                string roomId = ReadStr(data, "id_str");
                if (string.IsNullOrEmpty(roomId)) roomId = ReadStr(data, "id");
                string shareUrl = ReadStr(data, "share_url");
                string pushUrl = "";
                if (data.TryGetProperty("stream_url", out var su) && su.ValueKind == JsonValueKind.Object)
                {
                    pushUrl = ReadStr(su, "rtmp_push_url");
                    if (string.IsNullOrEmpty(pushUrl) && su.TryGetProperty("push_urls", out var pushArr) && pushArr.ValueKind == JsonValueKind.Array)
                    {
                        foreach (var item in pushArr.EnumerateArray())
                        {
                            var cand = item.ValueKind == JsonValueKind.String ? (item.GetString() ?? "").Trim() : "";
                            if (!string.IsNullOrEmpty(cand)) { pushUrl = cand; break; }
                        }
                    }
                }
                if (string.IsNullOrEmpty(pushUrl))
                {
                    lastError = "TikTok created the room but did not return an RTMP push URL.";
                    continue;
                }
                if (!pushUrl.StartsWith("rtmp://", StringComparison.OrdinalIgnoreCase)
                    && !pushUrl.StartsWith("rtmps://", StringComparison.OrdinalIgnoreCase))
                {
                    lastError = "TikTok returned an unsupported RTMP push URL format.";
                    Log.Warn($"TikTok room/create returned unsupported URL on {host} (room_id={roomId})");
                    continue;
                }

                var (serverUrl, streamKey) = SplitRtmpUrl(pushUrl);
                if (string.IsNullOrWhiteSpace(serverUrl) || string.IsNullOrWhiteSpace(streamKey))
                {
                    lastError = "TikTok created the room but did not return a valid RTMP server URL and stream key.";
                    Log.Warn($"TikTok room/create missing stream key in push URL on {host} (room_id={roomId}, target={MaskRtmpUrl(pushUrl)})");
                    continue;
                }
                session.WebcastDomain = host;
                Log.Info($"TikTok room/create OK on {host} (room_id={roomId}, target={MaskRtmpUrl(pushUrl)})");
                return new TikTokCreateRoomResult(true, roomId, serverUrl, streamKey, pushUrl, shareUrl);
            }
            catch (Exception ex) when (ex is not OperationCanceledException)
            {
                lastError = ex.Message;
                Log.Warn($"TikTok room/create failed on {host}: {ex.Message}");
            }
        }

        return new TikTokCreateRoomResult(false, "", "", "", "", "", lastError ?? "Failed to create TikTok LIVE room.");
    }

    public static async Task FinishRoomAsync(
        TikTokSessionData session,
        string roomId,
        string? signerKey,
        string? signerUrl,
        CancellationToken ct = default)
    {
        if (!session.HasSessionCookie || string.IsNullOrWhiteSpace(roomId)) return;
        try
        {
            var candidate = new RapidApiTikTokSigner(signerKey, signerUrl);
            ITikTokSigner? signer = candidate.IsConfigured ? candidate : null;

            session.EnsureDeviceIds();
            var jar = new CookieContainer();
            string host = !string.IsNullOrWhiteSpace(session.WebcastDomain) ? session.WebcastDomain : WebcastHosts[0];
            session.PopulateCookieContainer(jar, new[] { host });

            using var handler = new SocketsHttpHandler
            {
                CookieContainer = jar,
                UseCookies = true,
                AutomaticDecompression = DecompressionMethods.All,
            };
            using var http = new HttpClient(handler) { Timeout = TimeSpan.FromSeconds(15) };

            var query = BuildStudioParams(session.DeviceId, session.InstallId, session.Region);
            query["room_id"] = roomId;
            query["reason"] = "1";
            var url = $"https://{host}/webcast/room/finish_abnormal/";
            using var doc = await SendSignedJsonAsync(
                http, signer, HttpMethod.Post, url, query,
                session.DeviceId, isPassport: false, ct,
                formBody: new Dictionary<string, string> { ["room_id"] = roomId, ["reason"] = "1" }).ConfigureAwait(false);
            Log.Info($"TikTok room/finish_abnormal sent for room {roomId}.");
        }
        catch (Exception ex)
        {
            Log.Warn("TikTok room finish warning: " + ex.Message);
        }
    }

    private static (string HashtagId, string GameTagId) ResolveCategoryTag(string? category)
    {
        var c = (category ?? "").Trim().ToLowerInvariant();
        if (c.Contains("just chatting") || c.Contains("chat") || c.Contains("irl")) return ("1", "0");
        if (c.Contains("music")) return ("3", "0");
        if (c.Contains("fortnite")) return ("5", "76");
        if (c.Contains("valorant")) return ("5", "187");
        if (c.Contains("counter-strike") || c.Contains("cs2") || c.Contains("cs:go")) return ("5", "124");
        if (c.Contains("league of legends") || c == "lol") return ("5", "5");
        if (c.Contains("minecraft")) return ("5", "4");
        if (c.Contains("gta") || c.Contains("grand theft")) return ("5", "75");
        if (c.Contains("call of duty") || c.Contains("warzone")) return ("5", "128");
        if (c.Contains("apex")) return ("5", "134");
        if (c.Contains("fc ") || c.Contains("fifa")) return ("5", "147");
        return ("5", "0"); // default: Gaming
    }

    private static (string ServerUrl, string StreamKey) SplitRtmpUrl(string fullUrl)
    {
        var trimmed = fullUrl.Trim();
        int q = trimmed.IndexOf('?');
        string withoutQuery = q >= 0 ? trimmed[..q] : trimmed;
        int lastSlash = withoutQuery.LastIndexOf('/');
        if (lastSlash <= 8) return (trimmed, "");
        return (trimmed[..lastSlash], trimmed[(lastSlash + 1)..]);
    }

    private static string MaskRtmpUrl(string fullUrl)
    {
        var trimmed = (fullUrl ?? "").Trim();
        int q = trimmed.IndexOf('?');
        string withoutQuery = q >= 0 ? trimmed[..q] : trimmed;
        int lastSlash = withoutQuery.LastIndexOf('/');
        if (lastSlash <= 8)
            return Uri.TryCreate(withoutQuery, UriKind.Absolute, out var uri) ? $"{uri.Scheme}://{uri.Host}/***" : "***";
        return withoutQuery[..lastSlash] + "/***";
    }

    private static async Task<JsonDocument> SendSignedJsonAsync(
        HttpClient http,
        ITikTokSigner? signer,
        HttpMethod method,
        string baseUrl,
        Dictionary<string, string> queryParams,
        string deviceId,
        bool isPassport,
        CancellationToken ct,
        Dictionary<string, string>? formBody = null)
    {
        long ts = DateTimeOffset.UtcNow.ToUnixTimeSeconds();
        TikTokSignatureResult? sig = null;
        if (signer is { IsConfigured: true })
            sig = await signer.SignRequestAsync(ts, Aid, queryParams, formBody != null ? BuildQueryString(formBody) : null, deviceId, ct).ConfigureAwait(false);

        var qs = BuildQueryString(queryParams);
        var fullUrl = string.IsNullOrEmpty(qs) ? baseUrl : $"{baseUrl}?{qs}";

        using var req = new HttpRequestMessage(method, fullUrl);
        if (formBody != null && method == HttpMethod.Post)
            req.Content = new FormUrlEncodedContent(formBody);
        string browserVer = BuildLiveStudioBrowserVersion(DefaultLiveStudioVersion);
        req.Headers.TryAddWithoutValidation("User-Agent", "Mozilla/" + browserVer);
        req.Headers.TryAddWithoutValidation("Accept", isPassport ? "application/json, text/javascript" : "application/json, text/plain, */*");
        req.Headers.TryAddWithoutValidation("Accept-Language", "en-US");
        req.Headers.TryAddWithoutValidation("sdk_aid", Aid);
        req.Headers.TryAddWithoutValidation("sec-ch-ua", "\"Not.A/Brand\";v=\"99\", \"Chromium\";v=\"136\"");
        req.Headers.TryAddWithoutValidation("sec-ch-ua-mobile", "?0");
        req.Headers.TryAddWithoutValidation("sec-ch-ua-platform", "\"Windows\"");
        req.Headers.TryAddWithoutValidation("tt-ticket-guard-public-key", TicketGuardPublicKey);
        req.Headers.TryAddWithoutValidation("tt-ticket-guard-version", "2");
        req.Headers.TryAddWithoutValidation("tt-ticket-guard-web-version", "1");
        req.Headers.TryAddWithoutValidation("tt-ticket-guard-iteration-version", "0");
        if (isPassport)
            req.Headers.TryAddWithoutValidation("Referer", "https://www.tiktok.com/ucenter_web/live_studio/login");

        if (sig != null)
        {
            req.Headers.TryAddWithoutValidation("x-khronos", sig.XKhronos);
            req.Headers.TryAddWithoutValidation("x-ladon", sig.XLadon);
            req.Headers.TryAddWithoutValidation("x-argus", sig.XArgus);
            if (!string.IsNullOrEmpty(sig.XGorgon))
                req.Headers.TryAddWithoutValidation("x-gorgon", sig.XGorgon);
        }
        else
        {
            req.Headers.TryAddWithoutValidation("x-khronos", ts.ToString());
        }

        using var resp = await http.SendAsync(req, ct).ConfigureAwait(false);
        var body = await resp.Content.ReadAsStringAsync(ct).ConfigureAwait(false);
        if (!resp.IsSuccessStatusCode)
            throw new InvalidOperationException($"TikTok HTTP {(int)resp.StatusCode}: {body[..Math.Min(body.Length, 200)]}");

        try { return JsonDocument.Parse(body); }
        catch (JsonException)
        {
            throw new InvalidOperationException("TikTok nie zwrócił poprawnego JSON (możliwa blokada TLS/anti-bot lub wygasła sesja).");
        }
    }

    private static Dictionary<string, string> BuildStudioParams(string deviceId, string installId, string? priorityRegion)
    {
        var ver = DefaultLiveStudioVersion;
        var browserVer = BuildLiveStudioBrowserVersion(ver);
        var p = new Dictionary<string, string>
        {
            ["aid"] = Aid,
            ["app_name"] = "tiktok_live_studio",
            ["device_id"] = string.IsNullOrEmpty(deviceId) ? "0" : deviceId,
            ["install_id"] = string.IsNullOrEmpty(installId) ? "0" : installId,
            ["channel"] = "studio",
            ["version_code"] = ver,
            ["device_platform"] = "windows",
            ["timezone_name"] = TimeZoneInfo.Local.Id,
            ["screen_width"] = "1920",
            ["screen_height"] = "1080",
            ["browser_language"] = "en-US",
            ["browser_platform"] = "Win32",
            ["browser_name"] = "Mozilla",
            ["browser_version"] = browserVer,
            ["language"] = "en",
            ["app_language"] = "en",
            ["webcast_language"] = "en",
            ["webcast_sdk_version"] = ver.Replace(".", ""),
            ["live_mode"] = "6",
        };
        if (!string.IsNullOrWhiteSpace(priorityRegion))
            p["priority_region"] = priorityRegion.Trim().ToLowerInvariant();
        return p;
    }

    private static string BuildLiveStudioBrowserVersion(string version) =>
        $"5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) TikTokLIVEStudio/{version} Chrome/136.0.0.0 Electron/36.0.0 Safari/537.36";

    private static void ApplyPassportSdkSignature(Dictionary<string, string> queryParams)
    {
        queryParams.Remove("sign");
        queryParams.Remove("qs");

        var sortedKeys = queryParams.Keys.OrderBy(k => k, StringComparer.Ordinal).Take(10).ToList();
        var paramString = string.Join("&", sortedKeys.Select(k => $"{k}={queryParams[k]}"));
        var baseStr = $"{paramString}&&app_key={PassportAppKey}";

        var md5Bytes = MD5.HashData(Encoding.UTF8.GetBytes(baseStr));
        var sign = Convert.ToHexStringLower(md5Bytes);

        var keyBytes = Encoding.UTF8.GetBytes(string.Join(",", sortedKeys));
        var qsBuilder = new StringBuilder(keyBytes.Length * 2);
        foreach (var b in keyBytes)
            qsBuilder.Append(((byte)(b ^ 5)).ToString("x2"));

        queryParams["sign"] = sign;
        queryParams["qs"] = qsBuilder.ToString();
    }

    private static string BuildQueryString(IDictionary<string, string> queryParams) =>
        string.Join("&", queryParams.Select(kv => $"{Uri.EscapeDataString(kv.Key)}={Uri.EscapeDataString(kv.Value ?? "")}"));

    private static string MapPassportToWebcastHost(string passportHost)
    {
        if (passportHost.Contains("no1a", StringComparison.OrdinalIgnoreCase)) return "webcast16-normal-no1a.tiktokv.eu";
        if (passportHost.Contains("useast8", StringComparison.OrdinalIgnoreCase)) return "webcast16-normal-useast8.tiktokv.us";
        if (passportHost.Contains("useast5", StringComparison.OrdinalIgnoreCase)) return "webcast16-normal-useast5.tiktokv.us";
        return "webcast16-normal-c-alisg.tiktokv.com";
    }

    private static string ReadStr(JsonElement obj, string key)
    {
        if (!obj.TryGetProperty(key, out var p)) return "";
        return p.ValueKind switch
        {
            JsonValueKind.String => (p.GetString() ?? "").Trim(),
            JsonValueKind.Number => p.GetRawText().Trim(),
            _ => "",
        };
    }
}
