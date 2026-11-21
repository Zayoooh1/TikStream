using System.Drawing;
using System.Text.Json;
using CefSharp;
using CefSharp.Enums;
using CefSharp.OffScreen;
using CefSharp.Structs;
using TikStream.Core;
using TikStream.Gpu;
using Vortice.Direct3D11;
using Vortice.DXGI;
using ID3D11Texture2D = Vortice.Direct3D11.ID3D11Texture2D;

namespace TikStream.Capture;

/// <summary>
/// Process-wide lazy CEF initializer for OBS-style off-screen Browser Sources.
/// CEF is initialized only when the first Browser Source is created, keeping startup and memory overhead at zero otherwise.
/// </summary>
public static class CefHost
{
    private static readonly object Lock = new();
    private static bool _initialized;
    private static bool _failed;
    private static string _error = "";
    private static Thread? _lifecycleThread;
    private static ManualResetEventSlim? _shutdownSignal;
    private static ManualResetEventSlim? _shutdownDone;

    public static bool EnsureInitialized(out string error)
    {
        if (_initialized) { error = ""; return true; }
        if (_failed) { error = _error; return false; }

        lock (Lock)
        {
            if (_initialized) { error = ""; return true; }
            if (_failed) { error = _error; return false; }

            // CEF requires Cef.Initialize() and Cef.Shutdown() to run on the exact same thread.
            var initDone = new ManualResetEventSlim(false);
            var shutdownSignal = new ManualResetEventSlim(false);
            var shutdownDone = new ManualResetEventSlim(false);
            bool ok = false;
            string err = "";

            var t = new Thread(() =>
            {
                try
                {
                    if (Cef.IsInitialized == true)
                    {
                        ok = true;
                        initDone.Set();
                        shutdownSignal.Wait();
                        return;
                    }

                    var engineDir = Path.GetDirectoryName(typeof(CefHost).Assembly.Location)!;
                    var subprocess = Path.Combine(engineDir, "CefSharp.BrowserSubprocess.exe");
                    if (!File.Exists(subprocess))
                    {
                        var alt = Path.Combine(engineDir, "runtimes", "win-x64", "native", "CefSharp.BrowserSubprocess.exe");
                        if (File.Exists(alt)) subprocess = alt;
                    }

                    var cacheDir = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "TikStream", "BrowserCache");
                    Directory.CreateDirectory(cacheDir);
                    var logDir = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.ApplicationData), "TikStream");
                    Directory.CreateDirectory(logDir);

                    var settings = new CefSettings
                    {
                        BrowserSubprocessPath = subprocess,
                        LocalesDirPath = Path.Combine(engineDir, "locales"),
                        ResourcesDirPath = engineDir,
                        RootCachePath = cacheDir,
                        LogSeverity = LogSeverity.Warning,
                        LogFile = Path.Combine(logDir, "cef.log"),
                        WindowlessRenderingEnabled = true,
                    };
                    settings.CefCommandLineArgs["autoplay-policy"] = "no-user-gesture-required";
                    settings.CefCommandLineArgs["disable-gpu-vsync"] = "1";
                    settings.CefCommandLineArgs["enable-begin-frame-scheduling"] = "1";
                    settings.CefCommandLineArgs["disable-background-timer-throttling"] = "1";

                    if (!Cef.Initialize(settings, performDependencyCheck: false, browserProcessHandler: null))
                    {
                        err = "Cef.Initialize returned false";
                        Log.Warn("CEF initialization failed: " + err);
                        initDone.Set();
                        return;
                    }

                    ok = true;
                    Log.Info($"CEF initialized (Chromium {Cef.ChromiumVersion}).");
                    initDone.Set();
                    shutdownSignal.Wait();
                    try { Cef.Shutdown(); }
                    catch (Exception ex) { Log.Warn("CEF shutdown: " + ex.Message); }
                }
                catch (Exception ex)
                {
                    err = ex.Message;
                    Log.Error("CEF initialization", ex);
                    initDone.Set();
                }
                finally
                {
                    shutdownDone.Set();
                }
            }) { IsBackground = true, Name = "CefLifecycle" };
            t.SetApartmentState(ApartmentState.MTA);
            t.Start();
            initDone.Wait();

            if (!ok)
            {
                _failed = true;
                _error = err;
                error = err;
                return false;
            }

            _lifecycleThread = t;
            _shutdownSignal = shutdownSignal;
            _shutdownDone = shutdownDone;
            _initialized = true;
            error = "";
            return true;
        }
    }

    public static void Shutdown()
    {
        ManualResetEventSlim? signal;
        ManualResetEventSlim? done;
        lock (Lock)
        {
            if (!_initialized) return;
            _initialized = false;
            signal = _shutdownSignal;
            done = _shutdownDone;
            _shutdownSignal = null;
            _shutdownDone = null;
            _lifecycleThread = null;
        }
        signal?.Set();
        done?.Wait(4000);
    }
}

/// <summary>
/// Off-screen Chromium browser source (OBS-style) delivering premultiplied BGRA32 frames with full alpha transparency.
/// Runs CEF initialization and page loading off the render thread so rendering is never blocked.
/// </summary>
public sealed unsafe class BrowserCapture : IDisposable
{
    private readonly object _frameLock = new();
    private byte[]? _frameBuf;
    private int _frameW, _frameH;
    private long _frameVersion;
    private long _uploadedVersion;

    private readonly object _stateLock = new();
    private ChromiumWebBrowser? _browser;
    private string _url;
    private int _width, _height, _fps;
    private string _css;
    private bool _muted;
    private int _reloadNonce;
    private volatile bool _disposed;
    private volatile string _status = "Starting browser…";

    public string Status => _status;
    public long Frames => Volatile.Read(ref _frameVersion);

    public BrowserCapture(string url, int width, int height, int fps, string css, bool muted, int reloadNonce)
    {
        _url = NormalizeUrl(url);
        _width = Math.Clamp(width, 64, 3840);
        _height = Math.Clamp(height, 64, 2160);
        _fps = Math.Clamp(fps, 1, 60);
        _css = css ?? "";
        _muted = muted;
        _reloadNonce = reloadNonce;

        Task.Run(InitAsync);
    }

    private static readonly string PlaceholderUrl = "data:text/html;charset=utf-8," + Uri.EscapeDataString("""
        <!DOCTYPE html>
        <html>
        <head><meta charset="utf-8"><style>
        *{box-sizing:border-box;margin:0;padding:0}
        html,body{width:100%;height:100%;background:radial-gradient(circle at 25% 20%,#232535 0%,#14151c 70%);font-family:'Segoe UI',system-ui,sans-serif;color:#eef0f7;display:flex;align-items:center;justify-content:center;overflow:hidden}
        .card{width:86%;max-width:620px;padding:36px 32px;border-radius:18px;background:rgba(24,26,36,0.88);border:1px solid rgba(254,44,85,0.45);box-shadow:0 20px 50px rgba(0,0,0,0.45);text-align:center}
        .badge{width:64px;height:64px;margin:0 auto 18px;border-radius:16px;background:#1b1c23;border:3px solid #fe2c55;display:flex;align-items:center;justify-content:center;font-size:36px;font-weight:800;color:#fff;text-shadow:3px 3px 0 #fe2c55}
        h1{font-size:24px;font-weight:700;margin-bottom:10px;letter-spacing:0.2px}
        p{font-size:15px;line-height:1.55;color:#a9afc4}
        .pill{display:inline-block;margin-top:16px;padding:6px 14px;border-radius:999px;background:rgba(37,244,238,0.12);border:1px solid rgba(37,244,238,0.35);color:#25f4ee;font-size:13px;font-weight:600}
        </style></head>
        <body><div class="card">
        <div class="badge">T</div>
        <h1>TikStream Browser Source</h1>
        <p>Enter a widget, alert, or webpage URL (StreamElements, Streamlabs, TikFinity) or pick a local HTML file in the source properties.</p>
        <div class="pill">Transparent background supported</div>
        </div></body></html>
        """);

    private static string NormalizeUrl(string? url)
    {
        var u = (url ?? "").Trim();
        if (string.IsNullOrEmpty(u)) return PlaceholderUrl;
        if (u.Contains("://", StringComparison.Ordinal) || u.StartsWith("about:", StringComparison.OrdinalIgnoreCase) || u.StartsWith("data:", StringComparison.OrdinalIgnoreCase))
            return u;
        if (File.Exists(u)) return new Uri(Path.GetFullPath(u)).AbsoluteUri;
        return "https://" + u;
    }

    private void InitAsync()
    {
        if (_disposed) return;
        if (!CefHost.EnsureInitialized(out var err))
        {
            _status = "Browser error: " + err;
            return;
        }
        if (_disposed) return;

        try
        {
            var bs = new BrowserSettings
            {
                WindowlessFrameRate = _fps,
                BackgroundColor = 0, // ARGB(0,0,0,0) = transparent canvas
            };

            var browser = new ChromiumWebBrowser(_url, bs, requestContext: null, automaticallyCreateBrowser: false);
            browser.RenderHandler = new RawBgraRenderHandler(browser, OnRawPaint);
            browser.Size = new System.Drawing.Size(_width, _height);
            browser.BrowserInitialized += (_, _) =>
            {
                if (_disposed) return;
                try
                {
                    var host = browser.GetBrowserHost();
                    if (host != null)
                    {
                        host.WindowlessFrameRate = _fps;
                        host.SetAudioMuted(_muted);
                    }
                }
                catch { }
            };
            browser.FrameLoadEnd += (_, e) =>
            {
                if (_disposed || !e.Frame.IsMain) return;
                _status = $"{_width}×{_height} • {_fps} fps";
                InjectCss(browser);
            };
            browser.LoadError += (_, e) =>
            {
                if (_disposed || !e.Frame.IsMain || e.ErrorCode == CefErrorCode.Aborted) return;
                _status = $"Load error: {e.ErrorCode}";
            };

            lock (_stateLock)
            {
                if (_disposed) { browser.Dispose(); return; }
                _browser = browser;
            }

            browser.CreateBrowser();
            _status = "Loading…";
        }
        catch (Exception ex)
        {
            _status = "Browser error: " + ex.Message;
            Log.Error("BrowserCapture init", ex);
        }
    }

    public void Apply(string url, int width, int height, int fps, string css, bool muted, int reloadNonce)
    {
        var normUrl = NormalizeUrl(url);
        int w = Math.Clamp(width, 64, 3840);
        int h = Math.Clamp(height, 64, 2160);
        int f = Math.Clamp(fps, 1, 60);
        var c = css ?? "";

        ChromiumWebBrowser? b;
        bool urlChanged, sizeChanged, fpsChanged, cssChanged, muteChanged, reloadRequested;

        lock (_stateLock)
        {
            urlChanged = normUrl != _url;
            sizeChanged = w != _width || h != _height;
            fpsChanged = f != _fps;
            cssChanged = c != _css;
            muteChanged = muted != _muted;
            reloadRequested = reloadNonce != _reloadNonce;

            _url = normUrl;
            _width = w;
            _height = h;
            _fps = f;
            _css = c;
            _muted = muted;
            _reloadNonce = reloadNonce;
            b = _browser;
        }

        if (b == null || !b.IsBrowserInitialized) return;

        try
        {
            if (sizeChanged) b.Size = new System.Drawing.Size(w, h);
            if (fpsChanged) { var hst = b.GetBrowserHost(); if (hst != null) hst.WindowlessFrameRate = f; }
            if (muteChanged) b.GetBrowserHost()?.SetAudioMuted(muted);
            if (urlChanged) { _status = "Loading…"; b.LoadUrl(normUrl); }
            else if (reloadRequested) { _status = "Reloading…"; b.Reload(ignoreCache: true); }
            else if (cssChanged) InjectCss(b);
            if (sizeChanged || fpsChanged) _status = $"{w}×{h} • {f} fps";
        }
        catch (Exception ex) { Log.Warn("BrowserCapture apply: " + ex.Message); }
    }

    private void InjectCss(ChromiumWebBrowser browser)
    {
        string css, url;
        lock (_stateLock) { css = _css; url = _url; }
        if (string.IsNullOrWhiteSpace(css) || url == PlaceholderUrl) return;
        try
        {
            var jsCss = JsonSerializer.Serialize(css);
            var script = $"(function(){{var id='tikstream-custom-css';var s=document.getElementById(id);if(!s){{s=document.createElement('style');s.id=id;(document.head||document.documentElement).appendChild(s);}}s.textContent={jsCss};}})();";
            browser.ExecuteScriptAsync(script);
        }
        catch { }
    }

    private void OnRawPaint(IntPtr buffer, int width, int height)
    {
        if (_disposed || buffer == IntPtr.Zero || width <= 0 || height <= 0) return;
        int bytes = checked(width * height * 4);
        lock (_frameLock)
        {
            if (_frameBuf == null || _frameBuf.Length < bytes) _frameBuf = new byte[bytes];
            fixed (byte* dst = _frameBuf)
                Buffer.MemoryCopy((void*)buffer, dst, bytes, bytes);
            _frameW = width;
            _frameH = height;
            Interlocked.Increment(ref _frameVersion);
        }
    }

    /// <summary>Uploads the latest painted BGRA frame to a dynamic D3D11 texture (render thread).</summary>
    public bool TryUpload(GpuDevice gpu, ref ID3D11Texture2D? tex, ref ID3D11ShaderResourceView? srv, ref int outW, ref int outH)
    {
        long ver = Volatile.Read(ref _frameVersion);
        if (ver == _uploadedVersion || ver == 0) return false;

        lock (_frameLock)
        {
            if (_frameBuf == null || _frameW <= 0 || _frameH <= 0) return false;
            int w = _frameW, h = _frameH;
            if (tex == null || outW != w || outH != h)
            {
                srv?.Dispose();
                tex?.Dispose();
                tex = gpu.Device.CreateTexture2D(new Texture2DDescription
                {
                    Width = (uint)w,
                    Height = (uint)h,
                    MipLevels = 1,
                    ArraySize = 1,
                    Format = Format.B8G8R8A8_UNorm,
                    SampleDescription = new SampleDescription(1, 0),
                    Usage = ResourceUsage.Dynamic,
                    BindFlags = BindFlags.ShaderResource,
                    CPUAccessFlags = CpuAccessFlags.Write,
                });
                srv = gpu.Device.CreateShaderResourceView(tex);
                outW = w;
                outH = h;
            }
            fixed (byte* p = _frameBuf)
                TikStream.Engine.Nv12Texture.Copy(gpu.Context, tex, (IntPtr)p, w * 4, h);
            _uploadedVersion = _frameVersion;
            return true;
        }
    }

    public void Dispose()
    {
        if (_disposed) return;
        _disposed = true;
        ChromiumWebBrowser? b;
        lock (_stateLock) { b = _browser; _browser = null; }
        if (b != null)
        {
            try { b.Dispose(); } catch { }
        }
    }

    public void SendMouseMove(int x, int y, bool mouseLeave = false)
    {
        ChromiumWebBrowser? b; lock (_stateLock) b = _browser;
        b?.GetBrowserHost()?.SendMouseMoveEvent(new MouseEvent(x, y, CefEventFlags.None), mouseLeave);
    }

    public void SendMouseClick(int x, int y, int button, bool mouseUp, int clickCount)
    {
        ChromiumWebBrowser? b; lock (_stateLock) b = _browser;
        var host = b?.GetBrowserHost();
        if (host == null) return;
        var btn = button switch { 1 => MouseButtonType.Middle, 2 => MouseButtonType.Right, _ => MouseButtonType.Left };
        host.SendFocusEvent(true);
        host.SendMouseClickEvent(new MouseEvent(x, y, CefEventFlags.None), btn, mouseUp, Math.Max(1, clickCount));
    }

    public void SendMouseWheel(int x, int y, int deltaX, int deltaY)
    {
        ChromiumWebBrowser? b; lock (_stateLock) b = _browser;
        b?.GetBrowserHost()?.SendMouseWheelEvent(new MouseEvent(x, y, CefEventFlags.None), deltaX, deltaY);
    }

    public void SendKey(string kind, int keyCode, int charCode)
    {
        ChromiumWebBrowser? b; lock (_stateLock) b = _browser;
        var host = b?.GetBrowserHost();
        if (host == null) return;
        var ev = new KeyEvent
        {
            Type = kind switch { "up" => KeyEventType.KeyUp, "char" => KeyEventType.Char, _ => KeyEventType.RawKeyDown },
            WindowsKeyCode = kind == "char" ? charCode : keyCode,
            NativeKeyCode = keyCode,
            FocusOnEditableField = true,
        };
        host.SendKeyEvent(ev);
    }

    /// <summary>Returns a lightweight 32-bit BGRA BMP snapshot of the current page for the Interact dialog.</summary>
    public byte[]? SnapshotBmp()
    {
        lock (_frameLock)
        {
            if (_frameBuf == null || _frameW <= 0 || _frameH <= 0) return null;
            int w = _frameW, h = _frameH;
            int dataSize = w * h * 4;
            var bmp = new byte[54 + dataSize];
            bmp[0] = (byte)'B'; bmp[1] = (byte)'M';
            BitConverter.TryWriteBytes(bmp.AsSpan(2, 4), bmp.Length);
            BitConverter.TryWriteBytes(bmp.AsSpan(10, 4), 54);
            BitConverter.TryWriteBytes(bmp.AsSpan(14, 4), 40);
            BitConverter.TryWriteBytes(bmp.AsSpan(18, 4), w);
            BitConverter.TryWriteBytes(bmp.AsSpan(22, 4), -h); // top-down DIB
            BitConverter.TryWriteBytes(bmp.AsSpan(26, 2), (short)1);
            BitConverter.TryWriteBytes(bmp.AsSpan(28, 2), (short)32);
            BitConverter.TryWriteBytes(bmp.AsSpan(34, 4), dataSize);
            Buffer.BlockCopy(_frameBuf, 0, bmp, 54, dataSize);
            return bmp;
        }
    }

    private sealed class RawBgraRenderHandler : DefaultRenderHandler
    {
        private readonly Action<IntPtr, int, int> _onPaint;

        public RawBgraRenderHandler(ChromiumWebBrowser browser, Action<IntPtr, int, int> onPaint) : base(browser)
        {
            _onPaint = onPaint;
        }

        public override void OnPaint(PaintElementType type, Rect dirtyRect, IntPtr buffer, int width, int height)
        {
            if (type == PaintElementType.View) _onPaint(buffer, width, height);
        }
    }
}
