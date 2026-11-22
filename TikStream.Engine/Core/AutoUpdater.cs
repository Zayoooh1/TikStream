using System.Diagnostics;
using System.Net.Http.Headers;
using System.Text.Json;
using System.Text.Json.Nodes;

namespace TikStream.Core;

public sealed class UpdateCheckResult
{
    public string CurrentVersion { get; set; } = "";
    public string LatestVersion { get; set; } = "";
    public bool UpdateAvailable { get; set; }
    public string ReleaseUrl { get; set; } = "";
    public string DownloadUrl { get; set; } = "";
    public long DownloadSize { get; set; }
    public string Notes { get; set; } = "";
    public string Error { get; set; } = "";
}

public sealed class UpdateDownloadStatus
{
    public bool Downloading { get; set; }
    public int Progress { get; set; }
    public string Status { get; set; } = "";
    public bool Ready { get; set; }
    public string Path { get; set; } = "";
    public string Error { get; set; } = "";
}

/// <summary>
/// Checks GitHub Releases for newer TikStream versions and downloads the MSI installer in the background.
/// </summary>
public static class AutoUpdater
{
    private const string LatestReleaseApi = "https://api.github.com/repos/Zayoooh1/TikStream/releases/latest";

    private static readonly HttpClient Http = CreateHttpClient();
    private static readonly object Lock = new();
    private static UpdateDownloadStatus _status = new();

    public static string CurrentVersion
    {
        get
        {
            var v = typeof(AutoUpdater).Assembly.GetName().Version;
            return v == null ? "1.0.0" : $"{v.Major}.{v.Minor}.{Math.Max(0, v.Build)}";
        }
    }

    private static HttpClient CreateHttpClient()
    {
        var c = new HttpClient { Timeout = TimeSpan.FromSeconds(15) };
        c.DefaultRequestHeaders.UserAgent.Add(new ProductInfoHeaderValue("TikStream", CurrentVersion));
        c.DefaultRequestHeaders.Accept.Add(new MediaTypeWithQualityHeaderValue("application/vnd.github+json"));
        return c;
    }

    public static UpdateDownloadStatus GetStatus()
    {
        lock (Lock)
        {
            return new UpdateDownloadStatus
            {
                Downloading = _status.Downloading,
                Progress = _status.Progress,
                Status = _status.Status,
                Ready = _status.Ready,
                Path = _status.Path,
                Error = _status.Error,
            };
        }
    }

    public static async Task<UpdateCheckResult> CheckAsync()
    {
        var res = new UpdateCheckResult { CurrentVersion = CurrentVersion };
        try
        {
            using var resp = await Http.GetAsync(LatestReleaseApi).ConfigureAwait(false);
            if (!resp.IsSuccessStatusCode)
            {
                res.Error = $"GitHub API returned HTTP {(int)resp.StatusCode}.";
                return res;
            }

            var json = await resp.Content.ReadAsStringAsync().ConfigureAwait(false);
            var root = JsonNode.Parse(json)?.AsObject();
            if (root == null) { res.Error = "Invalid response from GitHub."; return res; }

            var tag = root["tag_name"]?.GetValue<string>() ?? "";
            var latestStr = tag.TrimStart('v', 'V');
            res.LatestVersion = latestStr;
            res.ReleaseUrl = root["html_url"]?.GetValue<string>() ?? "https://github.com/Zayoooh1/TikStream/releases";
            res.Notes = root["body"]?.GetValue<string>() ?? "";

            if (root["assets"] is JsonArray assets)
            {
                foreach (var a in assets)
                {
                    var ao = a?.AsObject();
                    var name = ao?["name"]?.GetValue<string>() ?? "";
                    if (name.EndsWith(".msi", StringComparison.OrdinalIgnoreCase))
                    {
                        res.DownloadUrl = ao?["browser_download_url"]?.GetValue<string>() ?? "";
                        res.DownloadSize = ao?["size"]?.GetValue<long>() ?? 0;
                        break;
                    }
                }
            }

            if (TryParseVersion(latestStr, out var latestVer) && TryParseVersion(CurrentVersion, out var curVer))
                res.UpdateAvailable = latestVer > curVer;
        }
        catch (Exception ex)
        {
            Log.Warn("Update check failed: " + ex.Message);
            res.Error = ex.Message;
        }
        return res;
    }

    public static string? StartDownload(string downloadUrl)
    {
        if (!IsTrustedUrl(downloadUrl))
            return "Untrusted update download URL.";

        lock (Lock)
        {
            if (_status.Downloading) return null;
            _status = new UpdateDownloadStatus { Downloading = true, Progress = 0, Status = "Connecting…" };
        }

        _ = Task.Run(async () =>
        {
            var target = Path.Combine(Path.GetTempPath(), "TikStreamSetup-update.msi");
            try
            {
                using var dlClient = new HttpClient { Timeout = TimeSpan.FromMinutes(15) };
                dlClient.DefaultRequestHeaders.UserAgent.Add(new ProductInfoHeaderValue("TikStream", CurrentVersion));
                using var resp = await dlClient.GetAsync(downloadUrl, HttpCompletionOption.ResponseHeadersRead).ConfigureAwait(false);
                resp.EnsureSuccessStatusCode();

                long total = resp.Content.Headers.ContentLength ?? -1;
                await using var src = await resp.Content.ReadAsStreamAsync().ConfigureAwait(false);
                await using (var dst = new FileStream(target, FileMode.Create, FileAccess.Write, FileShare.None, 81920, useAsync: true))
                {
                    var buf = new byte[81920];
                    long readTotal = 0;
                    int read;
                    while ((read = await src.ReadAsync(buf).ConfigureAwait(false)) > 0)
                    {
                        await dst.WriteAsync(buf.AsMemory(0, read)).ConfigureAwait(false);
                        readTotal += read;
                        int pct = total > 0 ? (int)Math.Clamp(readTotal * 100 / total, 0, 100) : 0;
                        string mb = $"{readTotal / 1024.0 / 1024.0:F1}" + (total > 0 ? $" / {total / 1024.0 / 1024.0:F1} MB" : " MB");
                        lock (Lock)
                        {
                            _status.Progress = pct;
                            _status.Status = $"Downloading… {mb}";
                        }
                    }
                }

                lock (Lock)
                {
                    _status.Downloading = false;
                    _status.Progress = 100;
                    _status.Ready = true;
                    _status.Path = target;
                    _status.Status = "Download complete.";
                }
                Log.Info($"Update downloaded: {target}");
            }
            catch (Exception ex)
            {
                Log.Error("Update download", ex);
                lock (Lock)
                {
                    _status.Downloading = false;
                    _status.Ready = false;
                    _status.Error = ex.Message;
                    _status.Status = "Download failed.";
                }
            }
        });

        return null;
    }

    public static string? LaunchInstaller()
    {
        string path;
        lock (Lock)
        {
            if (!_status.Ready || string.IsNullOrEmpty(_status.Path) || !File.Exists(_status.Path))
                return "Update installer is not ready.";
            path = _status.Path;
        }

        try
        {
            var exePath = Environment.ProcessPath;
            if (string.IsNullOrEmpty(exePath) || !File.Exists(exePath))
                exePath = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.ProgramFiles), "TikStream", "TikStream.exe");

            Process.Start(new ProcessStartInfo
            {
                FileName = "cmd.exe",
                Arguments = $"/c msiexec.exe /i \"{path}\" /passive && start \"\" \"{exePath}\"",
                UseShellExecute = false,
                CreateNoWindow = true,
            });
            Log.Info($"Launched update installer with auto-relaunch: {path} -> {exePath}");
            return null;
        }
        catch (Exception ex)
        {
            Log.Error("Launch update installer", ex);
            return ex.Message;
        }
    }

    private static bool IsTrustedUrl(string url)
    {
        if (!Uri.TryCreate(url, UriKind.Absolute, out var uri)) return false;
        if (!string.Equals(uri.Scheme, "https", StringComparison.OrdinalIgnoreCase)) return false;
        return uri.Host.Equals("github.com", StringComparison.OrdinalIgnoreCase) &&
               uri.AbsolutePath.StartsWith("/Zayoooh1/TikStream/", StringComparison.OrdinalIgnoreCase);
    }

    private static bool TryParseVersion(string s, out Version v)
    {
        var clean = s.Split('-', '+')[0].Trim();
        return Version.TryParse(clean, out v!);
    }
}
