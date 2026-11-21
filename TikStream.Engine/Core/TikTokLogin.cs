using System.Net;
using System.Net.Http.Headers;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json;

namespace TikStream.Core;

// Sandbox only. The credentials are supplied to the local process at launch.
internal sealed class TikTokLogin
{
    private const string RedirectUri = "http://127.0.0.1:3455/callback/";
    private static readonly HttpClient Http = new(new HttpClientHandler { AllowAutoRedirect = false })
        { Timeout = TimeSpan.FromSeconds(20) };
    private CancellationTokenSource? _cancel;
    private Task<string>? _pending;
    private string? _name;
    private string? _error;
    private string? _sandboxKey;
    private string? _sandboxSecret;

    public object Configure(string key, string secret)
    {
        if (_pending != null) return new { error = "Cancel the current sign-in first." };
        if (string.IsNullOrWhiteSpace(key) || string.IsNullOrWhiteSpace(secret))
            return new { error = "Enter both fields from the Sandbox tab." };
        Clear();
        _sandboxKey = key.Trim();
        _sandboxSecret = secret.Trim();
        return new { ok = true };
    }

    public object Begin()
    {
        if (_pending != null) return new { error = "Sign-in is already in progress." };
        var key = _sandboxKey ?? Environment.GetEnvironmentVariable("TIKSTREAM_TIKTOK_SANDBOX_KEY")?.Trim();
        var secret = _sandboxSecret ?? Environment.GetEnvironmentVariable("TIKSTREAM_TIKTOK_SANDBOX_SECRET")?.Trim();
        if (string.IsNullOrEmpty(key) || string.IsNullOrEmpty(secret))
            return new { needsCredentials = true };

        var listener = new HttpListener();
        listener.Prefixes.Add(RedirectUri);
        try { listener.Start(); }
        catch (HttpListenerException)
        {
            listener.Close();
            return new { error = "Could not open callback address 127.0.0.1:3455. Check if the port is available." };
        }
        var state = Convert.ToHexString(RandomNumberGenerator.GetBytes(32));
        var verifier = Convert.ToHexString(RandomNumberGenerator.GetBytes(32));
        var challenge = Convert.ToHexStringLower(SHA256.HashData(Encoding.ASCII.GetBytes(verifier)));
        _cancel = new CancellationTokenSource(TimeSpan.FromMinutes(5));
        _error = null;
        _name = null;
        _pending = Complete(listener, key, secret, state, verifier, _cancel.Token);
        var url = "https://www.tiktok.com/v2/auth/authorize/?client_key=" + Uri.EscapeDataString(key)
            + "&scope=user.info.basic&response_type=code&redirect_uri=" + Uri.EscapeDataString(RedirectUri)
            + "&state=" + state + "&code_challenge=" + challenge + "&code_challenge_method=S256";
        return new { url };
    }

    public object Status()
    {
        if (_pending is { IsCompleted: true })
        {
            try { _name = _pending.GetAwaiter().GetResult(); }
            catch (OperationCanceledException) { _error = "Sign-in timed out."; }
            catch (InvalidOperationException ex) { _error = ex.Message; }
            catch (Exception) { _error = "Could not connect to TikTok."; }
            _pending = null;
            _cancel?.Dispose();
            _cancel = null;
        }
        return new { pending = _pending != null, name = _name, error = _error };
    }

    public void Clear()
    {
        _cancel?.Cancel();
        if (_pending is { } pending)
            _ = pending.ContinueWith(task => _ = task.Exception, TaskContinuationOptions.OnlyOnFaulted);
        _pending = null;
        _cancel?.Dispose();
        _cancel = null;
        _name = null;
        _error = null;
    }

    private static async Task<string> Complete(HttpListener listener, string key, string secret,
        string state, string verifier, CancellationToken ct)
    {
        string code;
        using (listener)
        {
            while (true)
            {
                var context = await listener.GetContextAsync().WaitAsync(ct).ConfigureAwait(false);
                var matches = context.Request.HttpMethod == "GET"
                    && context.Request.Url?.AbsolutePath == "/callback/"
                    && context.Request.QueryString["state"] == state;
                code = matches ? context.Request.QueryString["code"] ?? "" : "";
                var message = !matches ? "Invalid callback." : string.IsNullOrEmpty(code)
                    ? "Authorization declined. Return to TikStream."
                    : "Return to TikStream to finish signing in.";
                var body = Encoding.UTF8.GetBytes(message);
                using var response = context.Response;
                response.StatusCode = matches ? 200 : 400;
                response.ContentType = "text/plain; charset=utf-8";
                response.Headers["Cache-Control"] = "no-store";
                response.ContentLength64 = body.Length;
                try { await response.OutputStream.WriteAsync(body, ct).ConfigureAwait(false); }
                catch (HttpListenerException) { }
                if (matches) break;
            }
        }
        if (string.IsNullOrEmpty(code)) throw new InvalidOperationException("TikTok did not grant login authorization.");

        using var form = new FormUrlEncodedContent(new Dictionary<string, string>
        {
            ["client_key"] = key, ["client_secret"] = secret, ["code"] = code,
            ["grant_type"] = "authorization_code", ["redirect_uri"] = RedirectUri, ["code_verifier"] = verifier,
        });
        using var tokenResponse = await Http.PostAsync("https://open.tiktokapis.com/v2/oauth/token/", form, ct).ConfigureAwait(false);
        using var token = JsonDocument.Parse(await tokenResponse.Content.ReadAsStringAsync(ct).ConfigureAwait(false));
        if (!tokenResponse.IsSuccessStatusCode || !token.RootElement.TryGetProperty("access_token", out var accessToken))
            throw new InvalidOperationException("TikTok rejected the login code. Check your Sandbox client credentials and Redirect URI.");

        using var profileRequest = new HttpRequestMessage(HttpMethod.Get,
            "https://open.tiktokapis.com/v2/user/info/?fields=display_name");
        profileRequest.Headers.Authorization = new AuthenticationHeaderValue("Bearer", accessToken.GetString());
        using var profileResponse = await Http.SendAsync(profileRequest, ct).ConfigureAwait(false);
        using var profile = JsonDocument.Parse(await profileResponse.Content.ReadAsStringAsync(ct).ConfigureAwait(false));
        if (profileResponse.IsSuccessStatusCode
            && profile.RootElement.TryGetProperty("data", out var data)
            && data.TryGetProperty("user", out var user)
            && user.TryGetProperty("display_name", out var name)
            && name.GetString() is { Length: > 0 } displayName)
            return displayName;
        throw new InvalidOperationException("Failed to read profile. Verify the user.info.basic scope.");
    }
}
