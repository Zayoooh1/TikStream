using System.Net.Http.Headers;
using System.Text;
using System.Text.Json;
using TikStream.Core;

namespace TikStream.TikTok;

public sealed record TikTokSignatureResult(
    string XKhronos,
    string XLadon,
    string XArgus,
    string? XGorgon = null);

public interface ITikTokSigner
{
    bool IsConfigured { get; }
    Task<TikTokSignatureResult> SignRequestAsync(
        long timestampSec,
        string aid,
        IDictionary<string, string>? queryParams,
        string? xSsStub,
        string? deviceId,
        CancellationToken ct = default);
}

/// <summary>
/// Hosted signer client compatible with the RapidAPI TikTok LIVE Studio signer
/// (or a custom self-hosted endpoint with the same /signatures contract).
/// Never transmits session cookies to the signer service.
/// </summary>
public sealed class RapidApiTikTokSigner : ITikTokSigner
{
    public const string DefaultBaseUrl = "https://tiktok-live-studio-api-signer1.p.rapidapi.com/";
    private const int DefaultLicenseId = 1877999593;

    private static readonly HttpClient Http = new(new SocketsHttpHandler
    {
        PooledConnectionLifetime = TimeSpan.FromMinutes(2),
        AutomaticDecompression = System.Net.DecompressionMethods.All,
    })
    {
        Timeout = TimeSpan.FromSeconds(25),
    };

    private readonly string _baseUrl;
    private readonly string _apiKey;
    private readonly string _host;
    private readonly bool _isRapidApi;

    public RapidApiTikTokSigner(string? apiKey, string? baseUrl = null)
    {
        var rawUrl = string.IsNullOrWhiteSpace(baseUrl) ? DefaultBaseUrl : baseUrl.Trim();
        if (!rawUrl.EndsWith('/')) rawUrl += "/";
        _baseUrl = rawUrl;
        _apiKey = (apiKey ?? "").Trim();
        _host = Uri.TryCreate(_baseUrl, UriKind.Absolute, out var u) ? u.Host : "tiktok-live-studio-api-signer1.p.rapidapi.com";
        _isRapidApi = _host.Contains("rapidapi.com", StringComparison.OrdinalIgnoreCase);
    }

    public bool IsConfigured => !_isRapidApi || _apiKey.Length > 0;

    public async Task<TikTokSignatureResult> SignRequestAsync(
        long timestampSec,
        string aid,
        IDictionary<string, string>? queryParams,
        string? xSsStub,
        string? deviceId,
        CancellationToken ct = default)
    {
        if (!IsConfigured)
            throw new InvalidOperationException("Brak klucza RapidAPI Signera. Wpisz klucz w Ustawienia → Stream → Konto TikTok LIVE.");

        var payload = new Dictionary<string, object?>
        {
            ["timestamp"] = timestampSec,
            ["aid"] = string.IsNullOrEmpty(aid) ? "8311" : aid,
            ["device_id"] = deviceId ?? "",
            ["license_id"] = DefaultLicenseId,
            ["params"] = queryParams is { Count: > 0 } ? (object)queryParams : "",
            ["stub"] = xSsStub ?? "",
        };

        var jsonBody = JsonSerializer.Serialize(payload);
        var targetUri = new Uri(new Uri(_baseUrl), "signatures");

        using var req = new HttpRequestMessage(HttpMethod.Post, targetUri)
        {
            Content = new StringContent(jsonBody, Encoding.UTF8, "application/json"),
        };
        req.Headers.Accept.Add(new MediaTypeWithQualityHeaderValue("application/json"));
        if (_isRapidApi)
        {
            req.Headers.TryAddWithoutValidation("X-RapidAPI-Key", _apiKey);
            req.Headers.TryAddWithoutValidation("X-RapidAPI-Host", _host);
        }

        HttpResponseMessage resp;
        try
        {
            resp = await Http.SendAsync(req, ct).ConfigureAwait(false);
        }
        catch (TaskCanceledException) when (!ct.IsCancellationRequested)
        {
            throw new InvalidOperationException("Przekroczono czas oczekiwania na usługę signera (timeout).");
        }
        catch (HttpRequestException ex)
        {
            throw new InvalidOperationException("Błąd połączenia z usługą signera: " + ex.Message);
        }

        using (resp)
        {
            var text = await resp.Content.ReadAsStringAsync(ct).ConfigureAwait(false);
            if (!resp.IsSuccessStatusCode)
            {
                string detail = ExtractErrorDetail(text);
                Log.Warn($"TikTok signer HTTP {(int)resp.StatusCode}: {detail}");
                if ((int)resp.StatusCode is 401 or 403)
                    throw new InvalidOperationException($"Signer odrzucił klucz API (HTTP {(int)resp.StatusCode}): {detail}");
                if ((int)resp.StatusCode == 429)
                    throw new InvalidOperationException($"Przekroczono limit zapytań RapidAPI (HTTP 429): {detail}");
                throw new InvalidOperationException($"Błąd signera HTTP {(int)resp.StatusCode}: {detail}");
            }

            using var doc = JsonDocument.Parse(text);
            var root = doc.RootElement;
            if (root.ValueKind != JsonValueKind.Object)
                throw new InvalidOperationException("Nieprawidłowa odpowiedź signera (brak obiektu JSON).");

            if (root.TryGetProperty("success", out var okProp) && okProp.ValueKind == JsonValueKind.False)
                throw new InvalidOperationException("Signer zwrócił błąd: " + ExtractErrorDetail(text));

            var headersObj = root.TryGetProperty("headers", out var nested) && nested.ValueKind == JsonValueKind.Object
                ? nested
                : root;

            string khronos = ReadString(headersObj, "x-khronos");
            string ladon = ReadString(headersObj, "x-ladon");
            string argus = ReadString(headersObj, "x-argus");
            string gorgon = ReadString(headersObj, "x-gorgon");

            if (string.IsNullOrEmpty(ladon) || string.IsNullOrEmpty(argus))
                throw new InvalidOperationException("Odpowiedź signera nie zawiera pełnych nagłówków x-argus / x-ladon.");

            if (string.IsNullOrEmpty(khronos))
                khronos = timestampSec.ToString();

            return new TikTokSignatureResult(khronos, ladon, argus, string.IsNullOrEmpty(gorgon) ? null : gorgon);
        }
    }

    private static string ReadString(JsonElement obj, string name)
    {
        if (!obj.TryGetProperty(name, out var prop)) return "";
        return prop.ValueKind switch
        {
            JsonValueKind.String => (prop.GetString() ?? "").Trim(),
            JsonValueKind.Number => prop.GetRawText().Trim(),
            _ => "",
        };
    }

    private static string ExtractErrorDetail(string body)
    {
        try
        {
            using var doc = JsonDocument.Parse(body);
            var root = doc.RootElement;
            if (root.ValueKind == JsonValueKind.Object)
            {
                foreach (var k in new[] { "error", "message", "detail" })
                {
                    if (root.TryGetProperty(k, out var p) && p.ValueKind == JsonValueKind.String)
                    {
                        var s = p.GetString()?.Trim();
                        if (!string.IsNullOrEmpty(s)) return s;
                    }
                }
            }
        }
        catch { }
        return string.IsNullOrWhiteSpace(body) ? "brak szczegółów" : body.Trim()[..Math.Min(body.Trim().Length, 240)];
    }
}
