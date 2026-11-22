using System.Net;
using System.Security.Cryptography;
using System.Text.Json;
using System.Text.RegularExpressions;
using TikStream.Core;

namespace TikStream.TikTok;

public sealed class TikTokCookieEntry
{
    public string Name { get; set; } = "";
    public string Value { get; set; } = "";
    public string Domain { get; set; } = ".tiktokv.com";
    public string Path { get; set; } = "/";
}

public sealed class TikTokSessionData
{
    public string UserId { get; set; } = "";
    public string Username { get; set; } = "";
    public string Nickname { get; set; } = "";
    public string Region { get; set; } = "";
    public string PassportDomain { get; set; } = "api16-normal-no1a.tiktokv.eu";
    public string WebcastDomain { get; set; } = "webcast16-normal-no1a.tiktokv.eu";
    public string DeviceId { get; set; } = "";
    public string InstallId { get; set; } = "";
    public string Source { get; set; } = ""; // "qr" | "live_studio"
    public long UpdatedAtUnix { get; set; }
    public List<TikTokCookieEntry> Cookies { get; set; } = new();

    public bool HasSessionCookie => Cookies.Any(c =>
        !string.IsNullOrEmpty(c.Value) &&
        (string.Equals(c.Name, "sessionid", StringComparison.OrdinalIgnoreCase) ||
         string.Equals(c.Name, "sessionid_ss", StringComparison.OrdinalIgnoreCase) ||
         string.Equals(c.Name, "sid_tt", StringComparison.OrdinalIgnoreCase)));

    public string DisplayName =>
        !string.IsNullOrWhiteSpace(Nickname) && !string.IsNullOrWhiteSpace(Username)
            ? $"{Nickname} (@{Username})"
            : !string.IsNullOrWhiteSpace(Username) ? "@" + Username
            : !string.IsNullOrWhiteSpace(Nickname) ? Nickname
            : UserId;

    public static TikTokSessionData? FromJson(string? json)
    {
        if (string.IsNullOrWhiteSpace(json)) return null;
        try { return JsonSerializer.Deserialize<TikTokSessionData>(json, AppSettings.JsonOpts); }
        catch { return null; }
    }

    public string ToJson() => JsonSerializer.Serialize(this, AppSettings.JsonOpts);

    public void EnsureDeviceIds()
    {
        if (string.IsNullOrWhiteSpace(DeviceId) || DeviceId == "0")
            DeviceId = GenerateNumericId(19);
        if (string.IsNullOrWhiteSpace(InstallId) || InstallId == "0")
            InstallId = GenerateNumericId(19);
    }

    public static string GenerateNumericId(int digits = 19)
    {
        Span<byte> buf = stackalloc byte[digits];
        RandomNumberGenerator.Fill(buf);
        var chars = new char[digits];
        // Live Studio device/install IDs typically start with 7
        chars[0] = '7';
        for (int i = 1; i < digits; i++)
            chars[i] = (char)('0' + (buf[i] % 10));
        return new string(chars);
    }

    public void PopulateCookieContainer(CookieContainer jar, IEnumerable<string> targetHosts)
    {
        var hosts = targetHosts.Where(h => !string.IsNullOrWhiteSpace(h)).Distinct(StringComparer.OrdinalIgnoreCase).ToList();
        foreach (var entry in Cookies)
        {
            if (string.IsNullOrWhiteSpace(entry.Name) || string.IsNullOrEmpty(entry.Value)) continue;
            var path = string.IsNullOrWhiteSpace(entry.Path) ? "/" : entry.Path;
            foreach (var host in hosts)
            {
                try
                {
                    jar.Add(new Uri($"https://{host}/"), new Cookie(entry.Name, entry.Value, path, host));
                }
                catch { }
            }
        }
    }
}

public sealed record StudioAccountInfo(
    string UserId,
    string Username,
    string Nickname,
    string SessionId,
    string CookieDomain,
    bool IsCurrent);

/// <summary>
/// Reads saved accounts from TikTok LIVE Studio's TTStore\services.json on Windows.
/// Read-only, no registry edits, no network access.
/// </summary>
public static partial class TikTokStudioImporter
{
    private static readonly string[] AllowedDomains =
    [
        "tiktokv.com", "tiktokv.us", "tiktokv.eu",
        "tiktokw.us", "tiktokw.eu", "ttapis.com", "tiktok.com",
    ];

    [GeneratedRegex("^[a-fA-F0-9]{32}$")]
    private static partial Regex Hex32Regex();

    public static List<StudioAccountInfo> ReadAccounts()
    {
        var appData = Environment.GetFolderPath(Environment.SpecialFolder.ApplicationData);
        if (string.IsNullOrWhiteSpace(appData))
            throw new InvalidOperationException("Nie udało się zlokalizować folderu %APPDATA%.");

        var servicesPath = Path.Combine(appData, "TikTok LIVE Studio", "TTStore", "services.json");
        if (!File.Exists(servicesPath))
            throw new InvalidOperationException("Nie znaleziono zapisanych kont TikTok LIVE Studio (TTStore\\services.json). Zaloguj się najpierw w LIVE Studio lub użyj logowania kodem QR.");

        string raw;
        try { raw = File.ReadAllText(servicesPath); }
        catch (IOException ex) { throw new InvalidOperationException("Nie udało się odczytać pliku LIVE Studio (zamknij LIVE Studio i spróbuj ponownie): " + ex.Message); }

        using var doc = JsonDocument.Parse(raw);
        var root = doc.RootElement;

        if (!root.TryGetProperty("AccountService", out var acctSvc) ||
            !acctSvc.TryGetProperty("state", out var acctState) ||
            !acctState.TryGetProperty("accountList", out var list) ||
            list.ValueKind != JsonValueKind.Array)
            throw new InvalidOperationException("Plik TTStore\\services.json nie zawiera obsługiwanej listy kont LIVE Studio.");

        string currentUserId = "";
        if (root.TryGetProperty("UserService", out var userSvc) &&
            userSvc.TryGetProperty("state", out var userState) &&
            userState.TryGetProperty("userInfo", out var userInfo) &&
            userInfo.ValueKind == JsonValueKind.Object)
        {
            currentUserId = ReadJsonString(userInfo, "user_id_str");
            if (string.IsNullOrEmpty(currentUserId))
                currentUserId = ReadJsonString(userInfo, "user_id");
        }

        var result = new List<StudioAccountInfo>();
        var seen = new HashSet<string>(StringComparer.Ordinal);
        foreach (var item in list.EnumerateArray())
        {
            if (item.ValueKind != JsonValueKind.Object) continue;
            var userId = ReadJsonString(item, "userId");
            var token = ReadJsonString(item, "sessionId");
            if (string.IsNullOrEmpty(userId) || !userId.All(char.IsAsciiDigit) || !Hex32Regex().IsMatch(token))
                continue;
            if (!seen.Add(userId)) continue;

            var domain = NormalizeCookieDomain(ReadJsonString(item, "domain"));
            var username = ReadJsonString(item, "displayId");
            var nickname = ReadJsonString(item, "nickname");
            result.Add(new StudioAccountInfo(userId, username, nickname, token, domain, userId == currentUserId));
        }

        if (result.Count == 0)
            throw new InvalidOperationException("TikTok LIVE Studio nie ma zapisanych aktywnych sesji. Zaloguj się w LIVE Studio lub użyj logowania kodem QR.");

        // Put currently active LIVE Studio account first
        result.Sort((a, b) => b.IsCurrent.CompareTo(a.IsCurrent));
        return result;
    }

    public static TikTokSessionData ToSession(StudioAccountInfo account)
    {
        var session = new TikTokSessionData
        {
            UserId = account.UserId,
            Username = account.Username,
            Nickname = account.Nickname,
            PassportDomain = PickPassportHost(account.CookieDomain),
            WebcastDomain = PickWebcastHost(account.CookieDomain),
            Source = "live_studio",
            UpdatedAtUnix = DateTimeOffset.UtcNow.ToUnixTimeSeconds(),
            Cookies =
            [
                new TikTokCookieEntry { Name = "sessionid", Value = account.SessionId, Domain = account.CookieDomain },
                new TikTokCookieEntry { Name = "sessionid_ss", Value = account.SessionId, Domain = account.CookieDomain },
                new TikTokCookieEntry { Name = "sid_tt", Value = account.SessionId, Domain = account.CookieDomain },
            ],
        };
        session.EnsureDeviceIds();
        return session;
    }

    private static string NormalizeCookieDomain(string origin)
    {
        if (string.IsNullOrWhiteSpace(origin)) return ".tiktokv.com";
        var withScheme = origin.Contains("://", StringComparison.Ordinal) ? origin : "https://" + origin;
        if (!Uri.TryCreate(withScheme, UriKind.Absolute, out var uri)) return ".tiktokv.com";
        var host = uri.Host.ToLowerInvariant();
        foreach (var d in AllowedDomains)
            if (host == d || host.EndsWith("." + d, StringComparison.Ordinal))
                return "." + d;
        return ".tiktokv.com";
    }

    private static string PickPassportHost(string cookieDomain) =>
        cookieDomain.EndsWith(".tiktokv.eu", StringComparison.OrdinalIgnoreCase) ? "api16-normal-no1a.tiktokv.eu"
        : cookieDomain.EndsWith(".tiktokv.us", StringComparison.OrdinalIgnoreCase) ? "api16-normal-useast8.tiktokv.us"
        : "api16-normal-c-alisg.tiktokv.com";

    private static string PickWebcastHost(string cookieDomain) =>
        cookieDomain.EndsWith(".tiktokv.eu", StringComparison.OrdinalIgnoreCase) ? "webcast16-normal-no1a.tiktokv.eu"
        : cookieDomain.EndsWith(".tiktokv.us", StringComparison.OrdinalIgnoreCase) ? "webcast16-normal-useast8.tiktokv.us"
        : "webcast16-normal-c-alisg.tiktokv.com";

    private static string ReadJsonString(JsonElement obj, string key)
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
