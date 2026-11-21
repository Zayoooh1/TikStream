using System.Security.Cryptography;
using System.Text;
using System.Text.Json;
using System.Text.Json.Serialization;

namespace TikStream.Core;

public enum SourceType { Screen, Window, Camera, Image, Text, Color, Media, AudioInput, AudioOutput, Game, AudioApplication, ChatOverlay, Browser }
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
    [JsonIgnore(Condition = JsonIgnoreCondition.WhenWritingNull)]
    public bool? GlobalAudio { get; set; }

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

    public static string NewId() => Guid.NewGuid().ToString("N");

    [JsonIgnore] public bool IsVisual => Type is not (SourceType.AudioInput or SourceType.AudioOutput or SourceType.AudioApplication);
    [JsonIgnore] public bool HasAudio => !IsVisual || Type == SourceType.Media;
}

/// <summary>Source instance inside a scene: position, size, fit mode, and GPU effects.</summary>
public sealed class SceneItem
{
    public string Id { get; set; } = SourceDef.NewId();
    public string SourceId { get; set; } = "";
    public bool Visible { get; set; } = true;
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
}

public sealed class Scene
{
    public string Id { get; set; } = SourceDef.NewId();
    public string Name { get; set; } = "Scene";
    /// <summary>Draw order: bottom to top.</summary>
    public List<SceneItem> Items { get; set; } = new();
}

public sealed class AppSettings
{
    // ---------- Stream ----------
    public string ServerUrl { get; set; } = "";
    public string StreamKeyProtected { get; set; } = "";
    public string TikTokUsername { get; set; } = "";
    public bool SecondStreamEnabled { get; set; }
    public string SecondStreamName { get; set; } = "Second Channel";
    public string SecondServerUrl { get; set; } = "";
    public string SecondStreamKeyProtected { get; set; } = "";

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
        var key = (streamKey ?? "").Trim();
        if (server.Length == 0) return "";
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
    }
}
