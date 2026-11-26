using System.Security.Cryptography;
using System.Text;
using System.Text.Json;
using System.Text.Json.Serialization;

namespace TikStream.Core;

public enum SourceType { Screen, Window, Camera, Image, Text, Color, Media, AudioInput, AudioOutput, Game, AudioApplication, ChatOverlay, Browser, AlertBox, GoalBar, NowPlaying }
public enum FitMode { Free, Fill, FitBlur }

/// <summary>Rectangle in normalized canvas coordinates (0..1, Y axis pointing down).</summary>
public struct NRect
{
    public float X { get; set; }
    public float Y { get; set; }
    public float W { get; set; }
    public float H { get; set; }
    public NRect(float x, float y, float w, float h) { X = x; Y = y; W = w; H = h; }
}

/// <summary>Source definition (OBS-style): a single device/file that can be referenced across multiple scenes.</summary>
public sealed record SourceDef
{
    public string Id { get; set; } = NewId();
    public SourceType Type { get; set; }
    public string Name { get; set; } = "";

    // display / window capture
    public string CaptureId { get; set; } = "";     // Screen: display name (\\.\DISPLAYn), Window: "process|title"
    public bool CaptureCursor { get; set; } = true;
    public string GameMode { get; set; } = "Fullscreen";
    public bool MatchWindowTitle { get; set; }
    public bool CaptureFromDesktop { get; set; }

    // camera
    public string CameraName { get; set; } = "";
    public int CameraWidth { get; set; } = 1920;
    public int CameraHeight { get; set; } = 1080;
    public int CameraFps { get; set; } = 60;
    public string CameraFormat { get; set; } = "nv12";

    // image / media
    public string FilePath { get; set; } = "";
    public bool Loop { get; set; } = true;

    // text (ColorArgb is also used by Color sources)
    public string Text { get; set; } = "My Stream!";
    public string FontName { get; set; } = "Segoe UI";
    public float FontSize { get; set; } = 72;
    public bool Bold { get; set; } = true;
    public int ColorArgb { get; set; } = unchecked((int)0xFFFFFFFF);
    public int OutlineArgb { get; set; } = unchecked((int)0xFF000000);
    public float OutlineWidth { get; set; } = 6;
    public int BackgroundArgb { get; set; }

    // audio (input / output / media audio)
    public string DeviceId { get; set; } = "";      // "" = system default device
    public float Volume { get; set; } = 1f;
    public bool Muted { get; set; }
    public string MonitorMode { get; set; } = "Off"; // "Off" | "MonitorOnly" | "MonitorAndOutput"
    [JsonIgnore(Condition = JsonIgnoreCondition.WhenWritingNull)]
    public bool? GlobalAudio { get; set; }
    [JsonIgnore(Condition = JsonIgnoreCondition.WhenWritingNull)]
    public bool? AudioVertical { get; set; }
    [JsonIgnore(Condition = JsonIgnoreCondition.WhenWritingNull)]
    public bool? AudioHorizontal { get; set; }

    // LIVE chat overlay
    public bool ShowChat { get; set; } = true;
    public bool ShowGifts { get; set; } = true;
    public bool ShowTaps { get; set; } = true;

    // browser source (off-screen Chromium)
    public string BrowserUrl { get; set; } = "";
    public int BrowserWidth { get; set; } = 800;
    public int BrowserHeight { get; set; } = 600;
    public int BrowserFps { get; set; } = 30;
    public string BrowserCss { get; set; } = "body { background-color: rgba(0, 0, 0, 0); margin: 0px auto; overflow: hidden; }";
    public bool BrowserMuted { get; set; }
    public int BrowserReloadNonce { get; set; }

    // native TikTok LIVE alert box
    public string AlertRole { get; set; } = ""; // "" (combined) | "gift" | "follow"
    public string AlertSoundPath { get; set; } = "";
    public string AlertImagePath { get; set; } = "";
    public string AlertGiftImagePath { get; set; } = "";
    public string AlertGiftSoundPath { get; set; } = "";
    public string AlertFollowImagePath { get; set; } = "";
    public string AlertFollowSoundPath { get; set; } = "";
    public float AlertVolume { get; set; } = 1.0f;
    public float AlertMonitorVolume { get; set; } = 1.0f;
    [JsonIgnore(Condition = JsonIgnoreCondition.WhenWritingNull)]
    public float? AlertGiftVolume { get; set; }
    [JsonIgnore(Condition = JsonIgnoreCondition.WhenWritingNull)]
    public float? AlertFollowVolume { get; set; }
    [JsonIgnore(Condition = JsonIgnoreCondition.WhenWritingNull)]
    public float? AlertGiftMonitorVolume { get; set; }
    [JsonIgnore(Condition = JsonIgnoreCondition.WhenWritingNull)]
    public float? AlertFollowMonitorVolume { get; set; }
    public bool AlertGiftMuted { get; set; }
    public bool AlertFollowMuted { get; set; }
    public int AlertDurationSec { get; set; } = 5;
    public bool AlertOnGift { get; set; } = true;
    public bool AlertOnFollow { get; set; } = true;

    // audio filters (noise suppression, noise gate, compressor, limiter, sync offset)
    public int SyncOffsetMs { get; set; }
    public bool NoiseSuppressEnabled { get; set; }
    public string NoiseSuppressMethod { get; set; } = "rnnoise"; // "rnnoise" | "speex"
    public int NoiseSuppressLevelDb { get; set; } = -30;
    public bool NoiseGateEnabled { get; set; }
    public float NoiseGateThresholdDb { get; set; } = -40f;
    public bool CompressorEnabled { get; set; }
    public float CompressorThresholdDb { get; set; } = -18f;
    public float CompressorRatio { get; set; } = 4f;
    public bool LimiterEnabled { get; set; }
    public float LimiterCeilingDb { get; set; } = -1f;

    // auto-ducking (sidechain from microphone)
    public bool DuckingEnabled { get; set; }
    public string DuckingSourceId { get; set; } = ""; // "" = auto (first active microphone)
    public float DuckingAmountDb { get; set; } = -12f;
    public float DuckingThresholdDb { get; set; } = -32f;
    public int DuckingAttackMs { get; set; } = 30;
    public int DuckingReleaseMs { get; set; } = 400;
    public int DuckingHoldMs { get; set; } = 150;

    // native Goal Bar overlay
    public string GoalTitle { get; set; } = "Cel: Diamenty";
    public string GoalMetric { get; set; } = "diamonds"; // "diamonds" | "gifts" | "taps" | "follows"
    public int GoalTarget { get; set; } = 1000;
    public int GoalManualOffset { get; set; }
    public bool GoalAutoResetOnStreamStart { get; set; } = true;
    public int GoalFillColorArgb { get; set; } = unchecked((int)0xFFFE2C55);
    public int GoalBgColorArgb { get; set; } = unchecked((int)0xD814161F);
    public int GoalTextColorArgb { get; set; } = unchecked((int)0xFFFFFFFF);
    public bool GoalShowPercent { get; set; } = true;

    // native Now Playing (Windows SMTC) overlay
    public bool NowPlayingHideWhenPaused { get; set; } = true;
    public bool NowPlayingShowArt { get; set; } = true;
    public string NowPlayingAppFilter { get; set; } = "";
    public int NowPlayingBgColorArgb { get; set; } = unchecked((int)0xD814161F);
    public int NowPlayingAccentColorArgb { get; set; } = unchecked((int)0xFF25F4EE);
    public int NowPlayingTextColorArgb { get; set; } = unchecked((int)0xFFFFFFFF);

    public static string NewId() => Guid.NewGuid().ToString("N");

    [JsonIgnore] public bool IsVisual => Type is not (SourceType.AudioInput or SourceType.AudioOutput or SourceType.AudioApplication);
    [JsonIgnore] public bool HasAudio => !IsVisual || Type is SourceType.Media or SourceType.AlertBox;
}

/// <summary>Source instance inside a scene: position, size, fit mode, and GPU effects.</summary>
public sealed class SceneItem
{
    public string Id { get; set; } = SourceDef.NewId();
    public string SourceId { get; set; } = "";
    public bool Visible { get; set; } = true;
    [JsonIgnore(Condition = JsonIgnoreCondition.WhenWritingNull)]
    public bool? DualVisible { get; set; }
    [JsonIgnore] public bool EffectiveDualVisible => DualVisible ?? Visible;
    public bool Locked { get; set; }
    public float X { get; set; }
    public float Y { get; set; }
    public float W { get; set; } = 1;
    public float H { get; set; } = 1;
    public FitMode Fit { get; set; } = FitMode.Free;
    public float Zoom { get; set; } = 1;
    public float PanX { get; set; }
    public float PanY { get; set; }
    public bool Mirror { get; set; }
    public float Corner { get; set; }
    public float Opacity { get; set; } = 1;

    // edge cropping (fraction 0..0.45) and GPU video filters
    public float CropLeft { get; set; }
    public float CropRight { get; set; }
    public float CropTop { get; set; }
    public float CropBottom { get; set; }
    public float Brightness { get; set; }
    public float Contrast { get; set; } = 1;
    public float Saturation { get; set; } = 1;
    public bool ChromaKeyEnabled { get; set; }
    public int ChromaKeyArgb { get; set; } = unchecked((int)0xFF00FF00);
    public float ChromaKeySimilarity { get; set; } = 0.25f;
    public float ChromaKeySmoothness { get; set; } = 0.08f;

    // GPU drop shadow (analytical SDF for opaque sources, 9-tap alpha shadow for PNG/GIF/alerts)
    public bool ShadowEnabled { get; set; }
    public float ShadowOffsetX { get; set; } = 0.01f;
    public float ShadowOffsetY { get; set; } = 0.015f;
    public float ShadowBlur { get; set; } = 0.03f;
    public float ShadowIntensity { get; set; } = 0.65f;

    // independent layout & crop for Horizontal Dual-Canvas preview/recording
    public bool HasDualLayout { get; set; }
    public float DualX { get; set; }
    public float DualY { get; set; }
    public float DualW { get; set; } = 1;
    public float DualH { get; set; } = 1;
    public FitMode DualFit { get; set; } = FitMode.Free;
    public float DualCropLeft { get; set; }
    public float DualCropRight { get; set; }
    public float DualCropTop { get; set; }
    public float DualCropBottom { get; set; }
}

public sealed class Scene
{
    public string Id { get; set; } = SourceDef.NewId();
    public string Name { get; set; } = "Scene";
    /// <summary>Draw order: bottom to top.</summary>
    public List<SceneItem> Items { get; set; } = new();
}

public sealed record SoundboardPad
{
    public string Id { get; set; } = SourceDef.NewId();
    public string Name { get; set; } = "New Pad";
    public string FilePath { get; set; } = "";
    public float Volume { get; set; } = 1.0f;
    public string ColorHex { get; set; } = "#2563EB";
    public string Hotkey { get; set; } = "";
    public string GiftTriggerName { get; set; } = "";
    public int GiftMinDiamonds { get; set; }
}

public sealed class AppSettings
{
    // ---------- Stream ----------
    public string ServerUrl { get; set; } = "";
    public string StreamKeyProtected { get; set; } = "";
    public string TikTokUsername { get; set; } = "";
    public string KickUsername { get; set; } = "zayosh";
    public string TikTokSignerUrl { get; set; } = "https://tiktok-live-studio-api-signer1.p.rapidapi.com/";
    public string TikTokSignerKeyProtected { get; set; } = "";
    public string TikTokSessionProtected { get; set; } = "";
    public bool SecondStreamEnabled { get; set; }
    public string SecondStreamName { get; set; } = "Second Channel";
    public string SecondServerUrl { get; set; } = "";
    public string SecondStreamKeyProtected { get; set; } = "";
    public string SecondStreamCanvas { get; set; } = "vertical"; // "vertical" | "horizontal"
    public int SecondStreamVideoBitrateKbps { get; set; }        // 0 = use main VideoBitrateKbps

    // ---------- Output ----------
    public int VideoBitrateKbps { get; set; } = 6000;
    public int KeyframeSec { get; set; } = 2;
    public string NvencPreset { get; set; } = "p5";
    public string NvencProfile { get; set; } = "high";
    public int BFrames { get; set; } = 2;
    public bool SpatialAq { get; set; } = true;
    public bool LookAhead { get; set; }
    public bool NvencZeroFpsImpact { get; set; }
    public int AudioBitrateKbps { get; set; } = 160;
    public string RecordFolder { get; set; } = Environment.GetFolderPath(Environment.SpecialFolder.MyVideos);
    public string RecordFormat { get; set; } = "mkv";
    public string RecordCodec { get; set; } = "h264";
    public bool DualCanvasEnabled { get; set; }
    public bool DualCanvasPreviewEnabled { get; set; } = true;
    public string DualRecordTarget { get; set; } = "Horizontal"; // "Vertical" | "Horizontal" | "Both"
    public int DualCanvasWidth { get; set; } = 1920;
    public int DualCanvasHeight { get; set; } = 1080;
    public bool MultiTrackAudio { get; set; }
    public bool ReplayBufferEnabled { get; set; }
    public int ReplayBufferSec { get; set; } = 30;

    // ---------- Video ----------
    public int Width { get; set; } = 1080;
    public int Height { get; set; } = 1920;
    public int Fps { get; set; } = 60;
    public int SceneTransitionMs { get; set; } = 250;

    // ---------- General ----------
    public bool ConfirmStreamStop { get; set; } = true;
    public bool RecordToFile { get; set; }          // automatically record while streaming
    public bool Snapping { get; set; } = true;
    public bool SnapToSources { get; set; } = true;
    public bool ShowPostStreamSummary { get; set; } = true;
    public bool AutoSaveStreamReports { get; set; } = true;

    // ---------- Built-in TTS ----------
    public bool TtsEnabled { get; set; }
    public string TtsProvider { get; set; } = "google"; // "google" | "streamelements" | "windows"
    public string TtsVoice { get; set; } = "";
    public float TtsRate { get; set; } = 1.0f;
    public float TtsVolume { get; set; } = 1.0f;
    public float TtsMonitorVolume { get; set; } = 1.0f;
    public bool TtsMuted { get; set; }
    public string TtsMonitorMode { get; set; } = "MonitorAndOutput";
    public bool TtsVertical { get; set; } = true;
    public bool TtsHorizontal { get; set; } = true;
    public bool TtsOnGift { get; set; } = true;
    public int TtsGiftMinDiamonds { get; set; } = 10;
    public bool TtsOnFollow { get; set; }
    public bool TtsOnChat { get; set; }
    public string TtsChatPrefix { get; set; } = "!tts";
    public int TtsCooldownSec { get; set; } = 5;
    public int TtsMaxChars { get; set; } = 140;

    // ---------- Native Soundboard ----------
    public List<SoundboardPad> SoundboardPads { get; set; } = new();
    public float SoundboardVolume { get; set; } = 1.0f;
    public bool SoundboardMuted { get; set; }
    public string SoundboardMonitorMode { get; set; } = "MonitorAndOutput";
    public bool SoundboardVertical { get; set; } = true;
    public bool SoundboardHorizontal { get; set; } = true;

    // ---------- Advanced ----------
    public bool PreviewEnabled { get; set; } = true;
    public string ProcessPriority { get; set; } = "AboveNormal";
    public bool AutoReconnect { get; set; } = true;
    public int ReconnectDelaySec { get; set; } = 2;
    public int ReconnectMaxRetries { get; set; } = 25;

    // ---------- Hotkeys (action -> "Ctrl+Shift+F9") ----------
    public Dictionary<string, string> Hotkeys { get; set; } = new();

    // ---------- Scenes and sources ----------
    public bool AutoUpdateEnabled { get; set; } = true;
    public bool MinimizeToTray { get; set; }
    public List<SourceDef> Sources { get; set; } = new();
    public List<Scene> Scenes { get; set; } = new();
    public string ActiveSceneId { get; set; } = "";

    // ---------- Legacy format (loaded for migration only) ----------
    [JsonIgnore(Condition = JsonIgnoreCondition.WhenWritingNull)] public string? GameSourceKind { get; set; }
    [JsonIgnore(Condition = JsonIgnoreCondition.WhenWritingNull)] public string? GameSourceId { get; set; }
    [JsonIgnore(Condition = JsonIgnoreCondition.WhenWritingNull)] public string? GameMode { get; set; }
    [JsonIgnore(Condition = JsonIgnoreCondition.WhenWritingNull)] public string? CameraName { get; set; }
    [JsonIgnore(Condition = JsonIgnoreCondition.WhenWritingNull)] public NRect? CameraRect { get; set; }
    [JsonIgnore(Condition = JsonIgnoreCondition.WhenWritingNull)] public bool? CameraMirror { get; set; }
    [JsonIgnore(Condition = JsonIgnoreCondition.WhenWritingNull)] public string? MicDeviceId { get; set; }
    [JsonIgnore(Condition = JsonIgnoreCondition.WhenWritingNull)] public bool? DesktopAudio { get; set; }

    [JsonIgnore]
    public string StreamKey
    {
        get => UnprotectSecret(StreamKeyProtected);
        set => StreamKeyProtected = ProtectSecret(value);
    }

    [JsonIgnore]
    public string SecondStreamKey
    {
        get => UnprotectSecret(SecondStreamKeyProtected);
        set => SecondStreamKeyProtected = ProtectSecret(value);
    }

    [JsonIgnore]
    public string TikTokSignerKey
    {
        get => UnprotectSecret(TikTokSignerKeyProtected);
        set => TikTokSignerKeyProtected = ProtectSecret(value);
    }

    [JsonIgnore]
    public string TikTokSessionJson
    {
        get => UnprotectSecret(TikTokSessionProtected);
        set => TikTokSessionProtected = ProtectSecret(value);
    }

    private static string UnprotectSecret(string protectedB64)
    {
        if (string.IsNullOrEmpty(protectedB64)) return "";
        try { return Encoding.UTF8.GetString(ProtectedData.Unprotect(Convert.FromBase64String(protectedB64), null, DataProtectionScope.CurrentUser)); }
        catch { return ""; }
    }

    private static string ProtectSecret(string plain) =>
        string.IsNullOrEmpty(plain) ? "" : Convert.ToBase64String(ProtectedData.Protect(Encoding.UTF8.GetBytes(plain), null, DataProtectionScope.CurrentUser));

    public static readonly JsonSerializerOptions JsonOpts = new()
    {
        WriteIndented = true,
        PropertyNamingPolicy = JsonNamingPolicy.CamelCase,
        PropertyNameCaseInsensitive = true,
        Converters = { new JsonStringEnumConverter() },
    };

    public static string DefaultPath =>
        Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.ApplicationData), "TikStream", "settings.json");

    public static AppSettings Load()
    {
        AppSettings s = new();
        try
        {
            if (File.Exists(DefaultPath))
                s = JsonSerializer.Deserialize<AppSettings>(File.ReadAllText(DefaultPath), JsonOpts) ?? new AppSettings();
        }
        catch (Exception ex) { Log.Warn("Failed to load settings: " + ex.Message); }
        s.Normalize();
        return s;
    }

    public void Save()
    {
        try
        {
            Directory.CreateDirectory(Path.GetDirectoryName(DefaultPath)!);
            var tmp = DefaultPath + ".tmp";
            File.WriteAllText(tmp, JsonSerializer.Serialize(this, JsonOpts));
            File.Move(tmp, DefaultPath, true);
        }
        catch (Exception ex) { Log.Warn("Failed to save settings: " + ex.Message); }
    }

    public AppSettings Clone() => JsonSerializer.Deserialize<AppSettings>(JsonSerializer.Serialize(this, JsonOpts), JsonOpts)!;

    public string BuildRtmpUrl() => CombineRtmp(ServerUrl, StreamKey);
    public string BuildSecondRtmpUrl() => CombineRtmp(SecondServerUrl, SecondStreamKey);

    private static string CombineRtmp(string serverUrl, string streamKey)
    {
        var server = (serverUrl ?? "").Trim();
        var key = (streamKey ?? "").Trim().TrimStart('/');
        if (server.Length == 0) return "";
        // Kick / AWS IVS ingest URLs require the /app RTMP application path (e.g. rtmps://<id>.global-contribute.live-video.net:443/app/<key>)
        if (Uri.TryCreate(server, UriKind.Absolute, out var uri) &&
            uri.Host.EndsWith(".live-video.net", StringComparison.OrdinalIgnoreCase) &&
            (string.IsNullOrEmpty(uri.AbsolutePath) || uri.AbsolutePath == "/"))
        {
            int port = uri.Port > 0 ? uri.Port : (uri.Scheme.Equals("rtmps", StringComparison.OrdinalIgnoreCase) ? 443 : 1935);
            server = $"{uri.Scheme}://{uri.Host}:{port}/app";
        }
        return key.Length == 0 ? server : server.TrimEnd('/') + "/" + key;
    }

    public Scene? ActiveScene => Scenes.FirstOrDefault(x => x.Id == ActiveSceneId) ?? Scenes.FirstOrDefault();
    public SourceDef? FindSource(string id) => Sources.FirstOrDefault(x => x.Id == id);

    /// <summary>Fills missing defaults and migrates settings from legacy versions.</summary>
    public void Normalize()
    {
        if (Scenes.Count == 0)
        {
            var scene = new Scene { Name = "Scene 1" };
            bool hasLegacy = !string.IsNullOrEmpty(GameSourceKind) || !string.IsNullOrEmpty(GameSourceId)
                || !string.IsNullOrEmpty(CameraName) || !string.IsNullOrEmpty(MicDeviceId) || DesktopAudio.HasValue;
            if (hasLegacy)
            {
                // migration from legacy settings format
                var cap = new SourceDef
                {
                    Type = GameSourceKind == "Window" ? SourceType.Window : SourceType.Screen,
                    Name = GameSourceKind == "Window" ? "Game Window" : "Display Capture",
                    CaptureId = GameSourceKind is "Window" or "Monitor" ? GameSourceId ?? "" : "",
                };
                Sources.Add(cap);
                scene.Items.Add(new SceneItem { SourceId = cap.Id, Fit = GameMode == "Fill" ? FitMode.Fill : FitMode.FitBlur });
                if (!string.IsNullOrEmpty(CameraName))
                {
                    var cam = new SourceDef { Type = SourceType.Camera, Name = "Camera", CameraName = CameraName };
                    Sources.Add(cam);
                    var r = CameraRect ?? new NRect(0.55f, 0.03f, 0.42f, 0.133f);
                    scene.Items.Add(new SceneItem { SourceId = cam.Id, X = r.X, Y = r.Y, W = r.W, H = r.H, Mirror = CameraMirror ?? true, Corner = 0.12f });
                }
                if (!Sources.Any(x => x.Type == SourceType.AudioOutput) && DesktopAudio == true)
                    Sources.Add(new SourceDef { Type = SourceType.AudioOutput, Name = "Desktop Audio" });
                if (!Sources.Any(x => x.Type == SourceType.AudioInput) && !string.IsNullOrEmpty(MicDeviceId))
                    Sources.Add(new SourceDef { Type = SourceType.AudioInput, Name = "Microphone", DeviceId = MicDeviceId });
            }
            Scenes.Add(scene);
            ActiveSceneId = scene.Id;
        }
        GameSourceKind = GameSourceId = GameMode = CameraName = MicDeviceId = null;
        CameraRect = null; CameraMirror = null; DesktopAudio = null;

        if (Scenes.All(x => x.Id != ActiveSceneId)) ActiveSceneId = Scenes[0].Id;
        // Older versions could insert duplicate items during reordering.
        var sourceIds = Sources.Select(s => s.Id).ToHashSet();
        foreach (var sc in Scenes)
        {
            var itemIds = new HashSet<string>();
            sc.Items.RemoveAll(i => !sourceIds.Contains(i.SourceId) || !itemIds.Add(i.Id));
        }
        // Older settings treated the first device of each type as global.
        foreach (var type in new[] { SourceType.AudioInput, SourceType.AudioOutput })
        {
            var devices = Sources.Where(s => s.Type == type).ToList();
            var legacyGlobal = devices.FirstOrDefault();
            foreach (var device in devices.Where(s => s.GlobalAudio == null))
            {
                bool inScene = Scenes.Any(sc => sc.Items.Any(i => i.SourceId == device.Id));
                device.GlobalAudio = !inScene && device == legacyGlobal;
                if (device.GlobalAudio == false && !inScene)
                    ActiveScene!.Items.Add(new SceneItem { SourceId = device.Id });
            }
        }
        NormalizeVideo();
    }

    public void NormalizeVideo()
    {
        if (Width <= 0 || Height <= 0) { Width = 1080; Height = 1920; }
        Width = Math.Clamp(Width, 128, 4096) & ~1;
        Height = Math.Clamp(Height, 128, 4096) & ~1;
        Fps = Math.Clamp(Fps, 1, 120);
        SecondStreamCanvas = string.Equals(SecondStreamCanvas, "horizontal", StringComparison.OrdinalIgnoreCase)
            ? "horizontal"
            : "vertical";
        SecondStreamVideoBitrateKbps = SecondStreamVideoBitrateKbps > 0
            ? Math.Clamp(SecondStreamVideoBitrateKbps, 1000, 20000)
            : 0;
    }
}
