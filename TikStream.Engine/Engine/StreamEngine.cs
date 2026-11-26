using System.Collections.Concurrent;
using System.Diagnostics;
using System.Numerics;
using FFmpeg.AutoGen;
using TikStream.Capture;
using TikStream.Core;
using TikStream.Encode;
using TikStream.Gpu;

namespace TikStream.Engine;

public sealed class OutputStats
{
    public string Name { get; set; } = "";
    public string Status { get; set; } = "";
    public double Kbps { get; set; }
    public long Dropped { get; set; }
    public int Queue { get; set; }
    public int Reconnects { get; set; }
}

public sealed class SourceStats
{
    public string Id { get; set; } = "";
    public string Status { get; set; } = "";
    public double Fps { get; set; }
    public double? CaptureFps { get; set; }
    public int Width { get; set; }
    public int Height { get; set; }
}

public sealed class EngineStats
{
    public double RenderFps { get; set; }
    public double RenderMs { get; set; }
    public long SkippedFrames { get; set; }
    public bool Streaming { get; set; }
    public bool Recording { get; set; }
    public bool ReplayActive { get; set; }
    public double StreamSeconds { get; set; }
    public double RecordSeconds { get; set; }
    public long EncodedFrames { get; set; }
    public double CpuPercent { get; set; }
    public string LastError { get; set; } = "";
    public string Gpu { get; set; } = "";
    public int CanvasWidth { get; set; }
    public int CanvasHeight { get; set; }
    public List<OutputStats> Outputs { get; set; } = new();
    public List<SourceStats> Sources { get; set; } = new();
}

/// <summary>
/// Core of the application. A single render thread owns the entire D3D11 context:
/// sources (Desktop Duplication, cameras, media, images) -> active scene -> NV12 -> NVENC.
/// Audio runs on a dedicated MMCSS thread. Streaming and recording start/stop independently (like OBS).
/// </summary>
public sealed unsafe class StreamEngine : IDisposable
{
    public GpuDevice Gpu { get; }
    public AudioEngine Audio { get; } = new();
    public TtsService Tts { get; }
    public NowPlayingService NowPlaying { get; } = new();
    public EngineStats Stats { get; private set; } = new();

    private Compositor _comp;
    private readonly ConcurrentQueue<Action> _cmds = new();
    private readonly Thread _renderThread;
    private readonly Thread _audioThread;
    private readonly Stopwatch _clock = Stopwatch.StartNew();
    private volatile bool _run = true;

    private AppSettings _scene;   // state used by the render thread
    private int _fps;

    private readonly Dictionary<string, RtSource> _sources = new();
    private readonly Dictionary<string, (int W, int H, byte[] Data)> _pendingPixels = new();
    private readonly MonitorCapturePool _monitors = new();
    private NRect? _selection;
    private NRect? _dualSelection;

    private IntPtr _previewHwnd;
    private int _previewW, _previewH;
    private bool _previewDirty;
    private IntPtr _dualPreviewHwnd;
    private int _dualPreviewW, _dualPreviewH;
    private bool _dualPreviewDirty;
    private volatile bool _previewEnabled = true;
    public NRect? Selection { get; set; }

    // outputs
    private VideoEncoder? _venc;
    private VideoEncoder? _horizVenc;  // dedicated horizontal h264_nvenc shared by horizontal SecondStream and h264 horizontal recording
    private VideoEncoder? _recVenc;    // dedicated recording encoder when RecordCodec != h264 (hevc/av1)
    private string? _recVideoOutputName;
    private bool _recVencIsHoriz;
    private Compositor? _dualComp;     // secondary horizontal/custom canvas for simultaneous dual-canvas streaming/recording
    private AudioEncoder? _aenc;
    private AudioEncoder? _aencHoriz;
    private volatile HashSet<string> _horizOutputs = new();      // outputs receiving _aencHoriz audio (copy-on-write for lock-free audio thread reads)
    private readonly HashSet<string> _horizVideoOutputs = new(); // outputs receiving _horizVenc video (render thread only)
    private readonly object _audioEncLock = new();
    private readonly List<(string SourceId, string Title, AudioEncoder Enc, float[] Buf)> _extraAudio = new();
    private volatile Dictionary<string, float[]>? _extraAudioMap;
    private long _encStartSamples;
    private volatile bool _encRun;
    private volatile StreamOutput[] _outputs = Array.Empty<StreamOutput>();
    private volatile ReplayBuffer? _replay;
    private long _encStartTicks;
    private long _lastPts = -1;
    private volatile bool _forceKey;
    private volatile bool _forceHorizKey;
    private long _encodedFrames;
    private long _streamStart, _recordStart;
    private string _lastError = "";
    private string _lastSceneId = "";
    private long _transStartTicks, _transDurTicks;
    private readonly System.Runtime.GCLatencyMode _prevGcMode;

    public bool IsStreaming => _outputs.Any(o => o.Name != "File" && o.Name != "FileHorizontal");
    public bool IsRecording => _outputs.Any(o => o.Name is "File" or "FileHorizontal");
    public bool IsReplayActive => _replay != null;

    public StreamEngine(AppSettings settings)
    {
        _prevGcMode = System.Runtime.GCSettings.LatencyMode;
        System.Runtime.GCSettings.LatencyMode = System.Runtime.GCLatencyMode.SustainedLowLatency;
        _scene = settings.Clone();
        _fps = settings.Fps;
        Gpu = new GpuDevice();
        _comp = new Compositor(Gpu, settings.Width, settings.Height);
        _previewEnabled = settings.PreviewEnabled;
        Native.timeBeginPeriod(1);
        ApplyPriority(settings.ProcessPriority);
        Audio.SyncDevices(settings);
        Tts = new TtsService(Audio);
        Tts.ApplySettings(settings);
        NowPlaying.SetActive(settings.Sources.Any(x => x.Type == SourceType.NowPlaying));
        _cmds.Enqueue(SyncSources);

        _renderThread = new Thread(RenderLoop) { IsBackground = true, Name = "Render", Priority = ThreadPriority.Highest };
        _renderThread.Start();
        _audioThread = new Thread(AudioLoop) { IsBackground = true, Name = "AudioPump", Priority = ThreadPriority.Highest };
        _audioThread.Start();
    }

    public int CanvasWidth => _comp.Width;
    public int CanvasHeight => _comp.Height;

    // ===================== settings =====================

    private static string Sig(IEnumerable<SourceDef> list) => System.Text.Json.JsonSerializer.Serialize(list, AppSettings.JsonOpts);

    private static HashSet<string> AudibleDeviceIds(AppSettings s) =>
        s.ActiveScene?.Items.Where(i => i.Visible || i.EffectiveDualVisible).Select(i => i.SourceId).ToHashSet() ?? new HashSet<string>();

    public void ApplySettings(AppSettings s, AppSettings previous)
    {
        var copy = s.Clone();
        bool virtualAudioChanged =
            copy.TtsVolume != previous.TtsVolume || copy.TtsMonitorVolume != previous.TtsMonitorVolume || copy.TtsMuted != previous.TtsMuted ||
            copy.TtsMonitorMode != previous.TtsMonitorMode || copy.TtsVertical != previous.TtsVertical || copy.TtsHorizontal != previous.TtsHorizontal ||
            copy.SoundboardVolume != previous.SoundboardVolume || copy.SoundboardMuted != previous.SoundboardMuted ||
            copy.SoundboardMonitorMode != previous.SoundboardMonitorMode || copy.SoundboardVertical != previous.SoundboardVertical || copy.SoundboardHorizontal != previous.SoundboardHorizontal;
        if (virtualAudioChanged ||
            Sig(copy.Sources.Where(x => !x.IsVisual || x.Type is SourceType.AlertBox or SourceType.Browser)) != Sig(previous.Sources.Where(x => !x.IsVisual || x.Type is SourceType.AlertBox or SourceType.Browser)) ||
            !AudibleDeviceIds(copy).SetEquals(AudibleDeviceIds(previous)))
            Audio.SyncDevices(copy);
        Tts.ApplySettings(copy);
        NowPlaying.SetActive(copy.Sources.Any(x => x.Type == SourceType.NowPlaying));
        var oldMedia = previous.Sources.Where(x => x.Type is SourceType.Media or SourceType.AlertBox or SourceType.Browser).ToDictionary(x => x.Id);
        foreach (var m in copy.Sources.Where(x => x.Type is SourceType.Media or SourceType.AlertBox or SourceType.Browser))
        {
            if (!oldMedia.TryGetValue(m.Id, out var old) || (m.Muted, m.BrowserMuted, m.Volume) != (old.Muted, old.BrowserMuted, old.Volume))
                Audio.SetVolume(m.Id, m.Muted || m.BrowserMuted ? 0 : m.Volume);
            Audio.SetFilters(m.Id, m);
        }
        if (copy.ProcessPriority != previous.ProcessPriority) ApplyPriority(copy.ProcessPriority);
        _cmds.Enqueue(() =>
        {
            if (_previewEnabled != copy.PreviewEnabled)
            {
                _previewEnabled = copy.PreviewEnabled;
                _previewDirty = true;
            }
            _scene = copy;
            if (_venc == null)
            {
                _fps = copy.Fps;
                if (copy.Width != _comp.Width || copy.Height != _comp.Height) RecreateCompositor(copy.Width, copy.Height);
            }
            SyncSources();
        });
    }

    public void SetSelection(NRect? rect) => _cmds.Enqueue(() => _selection = rect);
    public void SetDualSelection(NRect? rect) => _cmds.Enqueue(() => _dualSelection = rect);

    private void EnsureDualSnapshot(SceneItem it)
    {
        if (it.HasDualLayout) return;
        float mainW = _comp.Width, mainH = _comp.Height;
        float dualW = _dualComp?.Width ?? Math.Clamp(_scene.DualCanvasWidth & ~1, 128, 4096);
        float dualH = _dualComp?.Height ?? Math.Clamp(_scene.DualCanvasHeight & ~1, 128, 4096);
        float ix = it.X, iy = it.Y, iw = it.W, ih = it.H;
        if (it.Fit == FitMode.Free && mainW > 0 && mainH > 0 && dualW > 0 && dualH > 0)
        {
            float aspectRatio = (mainW / mainH) / (dualW / dualH);
            float cx = ix + iw * 0.5f, cy = iy + ih * 0.5f;
            float nw = iw * aspectRatio, nh = ih;
            if (nw > 0.96f) { float k = 0.96f / nw; nw *= k; nh *= k; }
            ix = cx - nw * 0.5f; iy = cy - nh * 0.5f; iw = nw; ih = nh;
        }
        it.HasDualLayout = true;
        it.DualFit = it.Fit;
        it.DualX = ix; it.DualY = iy; it.DualW = iw; it.DualH = ih;
        it.DualCropLeft = it.CropLeft; it.DualCropRight = it.CropRight;
        it.DualCropTop = it.CropTop; it.DualCropBottom = it.CropBottom;
    }

    private static void ClampCropPair(ref float a, ref float b)
    {
        a = Math.Clamp(a, 0f, 0.98f);
        b = Math.Clamp(b, 0f, 0.98f);
        if (a + b > 0.98f)
        {
            float k = 0.98f / (a + b);
            a *= k;
            b *= k;
        }
    }

    public void SetItemTransform(string sceneId, string itemId, NRect rect, FitMode fit) => _cmds.Enqueue(() =>
    {
        var sc = _scene.Scenes.FirstOrDefault(x => x.Id == sceneId);
        var it = sc?.Items.FirstOrDefault(x => x.Id == itemId);
        if (it == null) return;
        EnsureDualSnapshot(it);
        it.X = rect.X; it.Y = rect.Y; it.W = rect.W; it.H = rect.H; it.Fit = fit;
        _selection = rect;
    });

    public void SetDualItemTransform(string sceneId, string itemId, NRect rect, FitMode fit) => _cmds.Enqueue(() =>
    {
        var sc = _scene.Scenes.FirstOrDefault(x => x.Id == sceneId);
        var it = sc?.Items.FirstOrDefault(x => x.Id == itemId);
        if (it == null) return;
        EnsureDualSnapshot(it);
        it.DualX = rect.X; it.DualY = rect.Y; it.DualW = rect.W; it.DualH = rect.H; it.DualFit = fit;
        _dualSelection = rect;
    });

    public void SetItemCrop(string sceneId, string itemId, bool dual, NRect rect, float cl, float cr, float ct, float cb) => _cmds.Enqueue(() =>
    {
        var sc = _scene.Scenes.FirstOrDefault(x => x.Id == sceneId);
        var it = sc?.Items.FirstOrDefault(x => x.Id == itemId);
        if (it == null) return;
        EnsureDualSnapshot(it);
        ClampCropPair(ref cl, ref cr);
        ClampCropPair(ref ct, ref cb);
        if (dual)
        {
            it.DualX = rect.X; it.DualY = rect.Y; it.DualW = rect.W; it.DualH = rect.H; it.DualFit = FitMode.Free;
            it.DualCropLeft = cl; it.DualCropRight = cr; it.DualCropTop = ct; it.DualCropBottom = cb;
            _dualSelection = rect;
        }
        else
        {
            it.X = rect.X; it.Y = rect.Y; it.W = rect.W; it.H = rect.H; it.Fit = FitMode.Free;
            it.CropLeft = cl; it.CropRight = cr; it.CropTop = ct; it.CropBottom = cb;
            _selection = rect;
        }
    });

    private static void ApplyPriority(string p)
    {
        try
        {
            Process.GetCurrentProcess().PriorityClass = p switch
            {
                "High" => ProcessPriorityClass.High,
                "Normal" => ProcessPriorityClass.Normal,
                _ => ProcessPriorityClass.AboveNormal,
            };
        }
        catch { }
    }

    private void RecreateCompositor(int w, int h)
    {
        Log.Info($"Canvas resolution changed to {w}×{h}");
        _comp.Dispose();
        _comp = new Compositor(Gpu, w, h);
        _previewDirty = true;
    }

    /// <summary>Creates, removes, or recreates runtime sources according to definitions (render thread).</summary>
    private void SyncSources()
    {
        var defs = _scene.Sources.Where(d => d.IsVisual).ToDictionary(d => d.Id);
        foreach (var id in _sources.Keys.Where(k => !defs.ContainsKey(k)).ToList())
        {
            _sources[id].Dispose();
            _sources.Remove(id);
        }
        foreach (var d in defs.Values)
        {
            var key = RtSource.KeyOf(d);
            if (_sources.TryGetValue(d.Id, out var rt))
            {
                if (rt.ConfigKey == key) { rt.Def = d; continue; }
                rt.Dispose();
                _sources.Remove(d.Id);
            }
            try
            {
                RtSource created = d.Type switch
                {
                    SourceType.Screen => new CaptureRt(Gpu, d, _monitors) { ConfigKey = key },
                    SourceType.Window or SourceType.Game when d.CaptureFromDesktop =>
                        new DesktopWindowRt(_monitors) { Def = d, ConfigKey = key },
                    SourceType.Window or SourceType.Game => new WindowCaptureRt(_monitors) { Def = d, ConfigKey = key },
                    SourceType.Camera => new CameraRt(d, _fps) { ConfigKey = key },
                    SourceType.Media => new MediaRt(d, Audio) { ConfigKey = key },
                    SourceType.Color => new ColorRt(d, _comp, Gpu) { ConfigKey = key },
                    SourceType.Browser => new BrowserRt(d, Audio) { ConfigKey = key },
                    _ => new PixelsRt(d) { ConfigKey = key },
                };
                if (created is PixelsRt px && _pendingPixels.Remove(d.Id, out var pp)) px.Set(Gpu, _comp, pp.W, pp.H, pp.Data);
                _sources[d.Id] = created;
            }
            catch (Exception ex) { Log.Error($"Source '{d.Name}'", ex); }
        }
    }

    public void SetPreviewWindow(IntPtr hwnd, int w, int h)
        => _cmds.Enqueue(() => { Log.Info($"Preview: window 0x{hwnd.ToInt64():X} {w}x{h}"); _previewHwnd = hwnd; _previewW = w; _previewH = h; _previewDirty = true; });

    public void SetDualPreviewWindow(IntPtr hwnd, int w, int h)
        => _cmds.Enqueue(() => { _dualPreviewHwnd = hwnd; _dualPreviewW = w; _dualPreviewH = h; _dualPreviewDirty = true; });

    /// <summary>Image/text pixels (premultiplied BGRA) are rasterized by the Qt UI.</summary>
    public void SetSourcePixels(string id, int w, int h, byte[] bgra)
    {
        _cmds.Enqueue(() =>
        {
            if (_sources.TryGetValue(id, out var rt) && rt is PixelsRt px) px.Set(Gpu, _comp, w, h, bgra);
            else _pendingPixels[id] = (w, h, bgra);
        });
    }

    public void RequestKeyframe() => _forceKey = true;
    public void RequestHorizKeyframe() => _forceHorizKey = true;
    public void ClearError() => _lastError = "";

    private void AddHorizAudioOutput(string name)
    {
        var next = new HashSet<string>(_horizOutputs) { name };
        _horizOutputs = next;
    }

    private bool RemoveHorizAudioOutput(string name)
    {
        if (!_horizOutputs.Contains(name)) return false;
        var next = new HashSet<string>(_horizOutputs);
        next.Remove(name);
        _horizOutputs = next;
        return true;
    }

    private Compositor EnsureDualCompositor()
    {
        int rw = Math.Clamp(_scene.DualCanvasWidth & ~1, 128, 4096);
        int rh = Math.Clamp(_scene.DualCanvasHeight & ~1, 128, 4096);
        if (_dualComp == null || _dualComp.Width != rw || _dualComp.Height != rh)
        {
            _dualComp?.Dispose();
            _dualComp = new Compositor(Gpu, rw, rh);
            if (_dualPreviewHwnd != IntPtr.Zero) _dualPreviewDirty = true;
        }
        return _dualComp;
    }

    private VideoEncoder EnsureHorizVideoEncoder(int bitrateKbps)
    {
        if (_horizVenc != null) return _horizVenc;
        var dc = EnsureDualCompositor();
        var s = _scene;
        int kbps = bitrateKbps > 0 ? Math.Clamp(bitrateKbps, 1000, 20000) : s.VideoBitrateKbps;
        _horizVenc = new VideoEncoder(Gpu, dc.Width, dc.Height, new VideoEncoderOptions(
            _fps, kbps, s.NvencPreset, s.NvencProfile, s.KeyframeSec, s.BFrames, s.SpatialAq, s.LookAhead, s.NvencZeroFpsImpact, "h264"), globalHeader: true);
        _forceHorizKey = true;
        return _horizVenc;
    }

    private bool HasHorizVideoEncoder => _horizVenc != null || (_recVenc != null && _recVencIsHoriz);

    public BrowserCapture? GetBrowser(string sourceId) =>
        _sources.TryGetValue(sourceId, out var rt) && rt is BrowserRt br ? br.Capture : null;

    // ===================== streaming / recording =====================

    public Task<string?> StartStreamAsync(string? overrideRtmpUrl = null) => RunOnRender(() =>
    {
        if (IsStreaming) return;
        bool hasOverride = !string.IsNullOrWhiteSpace(overrideRtmpUrl);
        var url = hasOverride ? overrideRtmpUrl!.Trim() : _scene.BuildRtmpUrl();
        if (url.Length == 0 || (!hasOverride && string.IsNullOrWhiteSpace(_scene.StreamKey)))
            throw new InvalidOperationException("Missing server URL or stream key (Settings → Stream).");
        if (hasOverride && !url.StartsWith("rtmp://", StringComparison.OrdinalIgnoreCase)
            && !url.StartsWith("rtmps://", StringComparison.OrdinalIgnoreCase)
            && !url.StartsWith("srt://", StringComparison.OrdinalIgnoreCase))
            throw new InvalidOperationException("TikTok LIVE returned an invalid RTMP server URL.");
        EnsureEncoders();
        var opts = new OutputOptions(_scene.AutoReconnect, _scene.ReconnectDelaySec, _scene.ReconnectMaxRetries);

        bool startSecond = false;
        string url2 = "";
        string name2 = "";
        bool secondHoriz = false;
        VideoEncoder? secondVenc = null;
        if (_scene.SecondStreamEnabled)
        {
            url2 = _scene.BuildSecondRtmpUrl();
            if (url2.Length > 0 && !string.IsNullOrWhiteSpace(_scene.SecondStreamKey))
            {
                name2 = string.IsNullOrWhiteSpace(_scene.SecondStreamName) ? "Second Channel" : _scene.SecondStreamName.Trim();
                if (name2 is "Stream" or "File" or "FileHorizontal") name2 = "Stream 2";
                secondHoriz = string.Equals(_scene.SecondStreamCanvas, "horizontal", StringComparison.OrdinalIgnoreCase);
                secondVenc = secondHoriz
                    ? EnsureHorizVideoEncoder(_scene.SecondStreamVideoBitrateKbps > 0 ? _scene.SecondStreamVideoBitrateKbps : _scene.VideoBitrateKbps)
                    : _venc!;
                startSecond = true;
            }
        }

        AddOutput(new StreamOutput("Stream", url, _venc!, _aenc!, _fps, opts, RequestKeyframe));
        if (startSecond)
        {
            AddHorizAudioOutput(name2);
            if (secondHoriz)
            {
                _horizVideoOutputs.Add(name2);
                _forceHorizKey = true;
            }
            AddOutput(new StreamOutput(name2, url2, secondVenc!, _aencHoriz ?? _aenc!, _fps, opts,
                secondHoriz ? RequestHorizKeyframe : RequestKeyframe));
        }
        _streamStart = _clock.ElapsedTicks;
        if (_scene.RecordToFile && !IsRecording) StartRecordInternal();
    });

    public Task<string?> StopStreamAsync() => RunOnRender(() =>
    {
        foreach (var name in _outputs.Where(x => x.Name is not ("File" or "FileHorizontal")).Select(x => x.Name).ToArray())
            RemoveOutput(name, "stopped");
    });
    public Task<string?> StartRecordAsync() => RunOnRender(() => { if (!IsRecording) StartRecordInternal(); });
    public Task<string?> StopRecordAsync() => RunOnRender(() =>
    {
        RemoveOutput("File", "stopped");
        RemoveOutput("FileHorizontal", "stopped");
    });

    public Task<string?> StartReplayAsync() => RunOnRender(() =>
    {
        if (_replay != null) return;
        EnsureEncoders();
        _replay = new ReplayBuffer(_venc!, _aenc!, _fps, _scene.ReplayBufferSec);
        _forceKey = true;
    });

    public Task<string?> StopReplayAsync() => RunOnRender(() =>
    {
        var r = _replay;
        if (r == null) return;
        _replay = null;
        if (_outputs.Length == 0) TeardownEncoders();
        Task.Run(r.Dispose);
    });

    public Task<(string? Path, string? Error)> SaveReplayAsync()
    {
        var r = _replay;
        if (r == null) return Task.FromResult<(string?, string?)>((null, "Replay buffer is not running."));
        var folder = string.IsNullOrWhiteSpace(_scene.RecordFolder) ? Environment.GetFolderPath(Environment.SpecialFolder.MyVideos) : _scene.RecordFolder;
        Directory.CreateDirectory(folder);
        var ext = _scene.RecordFormat == "mp4" ? "mp4" : "mkv";
        var path = Path.Combine(folder, $"Replay {DateTime.Now:yyyy-MM-dd HH-mm-ss}.{ext}");
        return r.SaveAsync(path).ContinueWith<(string?, string?)>(t =>
            t.IsFaulted ? (null, t.Exception?.GetBaseException().Message ?? "Failed to save replay.") : (t.Result, null));
    }

    private void StartRecordInternal()
    {
        EnsureEncoders();
        var folder = string.IsNullOrWhiteSpace(_scene.RecordFolder) ? Environment.GetFolderPath(Environment.SpecialFolder.MyVideos) : _scene.RecordFolder;
        Directory.CreateDirectory(folder);
        var ext = _scene.RecordFormat == "mp4" ? "mp4" : "mkv";
        var path = Path.Combine(folder, $"TikStream {DateTime.Now:yyyy-MM-dd HH-mm-ss}.{ext}");

        List<(string Title, AudioEncoder Enc)>? extraForOutput = null;
        if (_scene.MultiTrackAudio && ext == "mkv")
        {
            var inputs = Audio.GetActiveInputs();
            if (inputs.Count > 0)
            {
                var created = new List<(string SourceId, string Title, AudioEncoder Enc, float[] Buf)>();
                var map = new Dictionary<string, float[]>();
                extraForOutput = new List<(string Title, AudioEncoder Enc)>();
                try
                {
                    foreach (var (id, name) in inputs.Take(6))
                    {
                        var enc = new AudioEncoder(_scene.AudioBitrateKbps, globalHeader: true);
                        var buf = new float[1024 * AudioEngine.Channels];
                        created.Add((id, name, enc, buf));
                        map[id] = buf;
                        extraForOutput.Add((name, enc));
                    }
                    lock (_audioEncLock)
                    {
                        DisposeExtraAudioLocked();
                        _extraAudio.AddRange(created);
                        _extraAudioMap = map;
                    }
                    Log.Info($"Multi-track MKV recording: Full Mix + {created.Count} separate audio track(s).");
                }
                catch
                {
                    foreach (var x in created) x.Enc.Dispose();
                    throw;
                }
            }
        }

        var s = _scene;
        string target = s.DualCanvasEnabled ? (s.DualRecordTarget ?? "Horizontal") : "Vertical";
        bool recordVertical = target is "Vertical" or "Both";
        bool recordHorizontal = s.DualCanvasEnabled && target is "Horizontal" or "Both";

        if (recordVertical)
        {
            var vertVenc = _venc!;
            if (!recordHorizontal && s.RecordCodec is "hevc" or "av1")
            {
                _recVenc?.Dispose();
                _recVenc = new VideoEncoder(Gpu, _comp.Width, _comp.Height, new VideoEncoderOptions(
                    _fps, s.VideoBitrateKbps, s.NvencPreset, s.NvencProfile, s.KeyframeSec, s.BFrames, s.SpatialAq, s.LookAhead, s.NvencZeroFpsImpact, s.RecordCodec), globalHeader: true);
                _recVideoOutputName = "File";
                _recVencIsHoriz = false;
                vertVenc = _recVenc;
            }
            AddOutput(new StreamOutput("File", path, vertVenc, _aenc!, _fps, new OutputOptions(false, 0, 0), RequestKeyframe, extraForOutput));
        }

        if (recordHorizontal)
        {
            var dc = EnsureDualCompositor();
            var hName = recordVertical ? "FileHorizontal" : "File";
            var hPath = recordVertical ? Path.Combine(folder, $"TikStream {DateTime.Now:yyyy-MM-dd HH-mm-ss} (Horizontal).{ext}") : path;
            VideoEncoder hVenc;
            Action hReqKey;
            if (s.RecordCodec is "hevc" or "av1")
            {
                _recVenc?.Dispose();
                _recVenc = new VideoEncoder(Gpu, dc.Width, dc.Height, new VideoEncoderOptions(
                    _fps, s.VideoBitrateKbps, s.NvencPreset, s.NvencProfile, s.KeyframeSec, s.BFrames, s.SpatialAq, s.LookAhead, s.NvencZeroFpsImpact, s.RecordCodec), globalHeader: true);
                _recVideoOutputName = hName;
                _recVencIsHoriz = true;
                hVenc = _recVenc;
                hReqKey = RequestHorizKeyframe;
            }
            else
            {
                // Share the horizontal h264_nvenc session with horizontal SecondStream if active.
                hVenc = EnsureHorizVideoEncoder(s.VideoBitrateKbps);
                _horizVideoOutputs.Add(hName);
                _forceHorizKey = true;
                hReqKey = RequestHorizKeyframe;
            }
            AddHorizAudioOutput(hName);
            AddOutput(new StreamOutput(hName, hPath, hVenc, _aencHoriz ?? _aenc!, _fps, new OutputOptions(false, 0, 0), hReqKey, recordVertical ? null : extraForOutput));
        }
        _recordStart = _clock.ElapsedTicks;
    }

    private void DisposeExtraAudioLocked(StreamOutput? flushTo = null)
    {
        _extraAudioMap = null;
        for (int i = 0; i < _extraAudio.Count; i++)
        {
            var enc = _extraAudio[i].Enc;
            int streamIdx = StreamOutput.AudioIndex + 1 + i;
            if (flushTo != null)
            {
                try { enc.Flush(p => DeliverTo(p, streamIdx, flushTo)); }
                catch (Exception ex) { Log.Warn("Extra audio flush: " + ex.Message); }
            }
            enc.Dispose();
        }
        _extraAudio.Clear();
    }

    private Task<string?> RunOnRender(Action a)
    {
        var tcs = new TaskCompletionSource<string?>();
        _cmds.Enqueue(() =>
        {
            try { a(); tcs.SetResult(null); }
            catch (Exception ex)
            {
                Log.Error("Output command", ex);
                _lastError = ex.Message;
                if (_outputs.Length == 0 && _replay == null) TeardownEncoders();
                tcs.SetResult(ex.Message);
            }
        });
        return tcs.Task;
    }

    private void EnsureEncoders()
    {
        if (_venc != null) return;
        var s = _scene;
        _fps = s.Fps;
        if (s.Width != _comp.Width || s.Height != _comp.Height) RecreateCompositor(s.Width, s.Height);
        _venc = new VideoEncoder(Gpu, _comp.Width, _comp.Height, new VideoEncoderOptions(
            _fps, s.VideoBitrateKbps, s.NvencPreset, s.NvencProfile, s.KeyframeSec, s.BFrames, s.SpatialAq, s.LookAhead, s.NvencZeroFpsImpact), globalHeader: true);
        var aenc = new AudioEncoder(s.AudioBitrateKbps, globalHeader: true);
        var aencHoriz = new AudioEncoder(s.AudioBitrateKbps, globalHeader: true);
        _encStartTicks = _clock.ElapsedTicks;
        _lastPts = -1;
        _encodedFrames = 0;
        _forceKey = true;
        lock (_audioEncLock)
        {
            _encStartSamples = (long)((double)_encStartTicks * AudioEngine.SampleRate / Stopwatch.Frequency);
            _aenc = aenc;
            _aencHoriz = aencHoriz;
            _encRun = true;
        }
        Log.Info("Encoders started.");
    }

    private void AddOutput(StreamOutput o)
    {
        _outputs = _outputs.Append(o).ToArray();
        _forceKey = true;
        Log.Info("=== START: " + o.Name);
    }

    private void RemoveOutput(string name, string reason)
    {
        var o = _outputs.FirstOrDefault(x => x.Name == name);
        if (o == null) return;
        _outputs = _outputs.Where(x => x != o).ToArray();
        bool wasHorizAudio = RemoveHorizAudioOutput(name);
        bool wasHorizVideo = _horizVideoOutputs.Remove(name);
        bool wasRecVideo = _recVenc != null && name == _recVideoOutputName;

        if (wasRecVideo && _recVenc != null)
        {
            try { _recVenc.Flush(p => DeliverTo(p, StreamOutput.VideoIndex, o)); } catch { }
            _recVenc.Dispose();
            _recVenc = null;
            _recVideoOutputName = null;
            _recVencIsHoriz = false;
        }
        if (wasHorizVideo && _horizVideoOutputs.Count == 0 && _horizVenc != null)
        {
            try { _horizVenc.Flush(p => DeliverTo(p, StreamOutput.VideoIndex, o)); } catch { }
            _horizVenc.Dispose();
            _horizVenc = null;
        }
        if (!HasHorizVideoEncoder && _dualPreviewHwnd == IntPtr.Zero && _dualComp != null)
        {
            _dualComp.Dispose();
            _dualComp = null;
        }
        if (name == "File")
        {
            lock (_audioEncLock) DisposeExtraAudioLocked(flushTo: o);
        }
        if (_outputs.Length == 0 && _replay == null)
            TeardownEncoders(flushTo: o, flushToHorizAudio: wasHorizAudio, flushMainVideo: !wasHorizVideo && !wasRecVideo);
        Task.Run(o.Dispose);
        Log.Info($"=== STOP: {name} ({reason})");
    }

    private void TeardownEncoders(StreamOutput? flushTo = null, bool flushToHorizAudio = false, bool flushMainVideo = true)
    {
        AudioEncoder? aenc, aencHoriz;
        lock (_audioEncLock)
        {
            _encRun = false;
            aenc = _aenc;
            _aenc = null;
            aencHoriz = _aencHoriz;
            _aencHoriz = null;
            DisposeExtraAudioLocked(flushTo);
        }
        if (flushTo != null && _venc != null && aenc != null)
        {
            try
            {
                if (flushMainVideo) _venc.Flush(p => DeliverTo(p, StreamOutput.VideoIndex, flushTo));
                var flushAenc = (flushToHorizAudio && aencHoriz != null) ? aencHoriz : aenc;
                flushAenc.Flush(p => DeliverTo(p, StreamOutput.AudioIndex, flushTo));
            }
            catch (Exception ex) { Log.Warn("Encoder flush: " + ex.Message); }
        }
        _venc?.Dispose(); _venc = null;
        _horizVenc?.Dispose(); _horizVenc = null;
        _recVenc?.Dispose(); _recVenc = null;
        _recVideoOutputName = null;
        _recVencIsHoriz = false;
        _horizVideoOutputs.Clear();
        _horizOutputs = new HashSet<string>();
        if (_dualPreviewHwnd == IntPtr.Zero && _dualComp != null)
        {
            _dualComp.Dispose();
            _dualComp = null;
        }
        aenc?.Dispose();
        aencHoriz?.Dispose();
    }

    private void DeliverAudio(IntPtr p, bool horizontal)
    {
        var pkt = (AVPacket*)p;
        pkt->stream_index = StreamOutput.AudioIndex;
        var horizSet = _horizOutputs;
        foreach (var o in _outputs)
        {
            bool isHoriz = horizSet.Contains(o.Name);
            if (isHoriz == horizontal) o.Enqueue(pkt);
        }
        if (!horizontal) _replay?.Enqueue(pkt, StreamOutput.AudioIndex);
        ffmpeg.av_packet_free(&pkt);
    }

    private void Deliver(IntPtr p, int streamIndex)
    {
        var pkt = (AVPacket*)p;
        pkt->stream_index = streamIndex;
        bool isVideo = streamIndex == StreamOutput.VideoIndex;
        foreach (var o in _outputs)
        {
            if (isVideo)
            {
                if (_horizVideoOutputs.Contains(o.Name)) continue;
                if (_recVenc != null && o.Name == _recVideoOutputName) continue;
            }
            o.Enqueue(pkt);
        }
        _replay?.Enqueue(pkt, streamIndex);
        ffmpeg.av_packet_free(&pkt);
    }

    private void DeliverHorizVideo(IntPtr p)
    {
        var pkt = (AVPacket*)p;
        pkt->stream_index = StreamOutput.VideoIndex;
        foreach (var o in _outputs)
        {
            if (_horizVideoOutputs.Contains(o.Name)) o.Enqueue(pkt);
        }
        ffmpeg.av_packet_free(&pkt);
    }

    private void DeliverToFile(IntPtr p, int streamIndex, string targetName = "File")
    {
        var pkt = (AVPacket*)p;
        pkt->stream_index = streamIndex;
        foreach (var o in _outputs)
        {
            if (o.Name == targetName) { o.Enqueue(pkt); break; }
        }
        ffmpeg.av_packet_free(&pkt);
    }

    private static void DeliverTo(IntPtr p, int streamIndex, StreamOutput o)
    {
        var pkt = (AVPacket*)p;
        pkt->stream_index = streamIndex;
        o.Enqueue(pkt);
        ffmpeg.av_packet_free(&pkt);
    }

    /// <summary>Audio pump: runs at 10 ms (480-sample) cadence for low mic latency and 1:1 RNNoise alignment, encoding 1024-sample AAC frames when active.</summary>
    private void AudioLoop()
    {
        uint idx = 0;
        var mm = Native.AvSetMmThreadCharacteristicsW("Pro Audio", ref idx);
        using var waiter = new PreciseWaiter();
        try
        {
            const int step = 480;     // 10.0 ms @ 48 kHz (exact RNNoise/Speex frame size)
            const int encFrame = 1024; // AAC-LC frame size
            const int ch = AudioEngine.Channels;
            var stepBuf = new float[step * ch];
            var stepBufHoriz = new float[step * ch];
            var encRing = new float[(encFrame + step) * ch];
            var encRingHoriz = new float[(encFrame + step) * ch];
            var encOut = new float[encFrame * ch];
            var encOutHoriz = new float[encFrame * ch];
            int encFill = 0;

            Dictionary<string, float[]>? lastExtraMap = null;
            var extraRings = new List<float[]>();
            var extraOut = new float[encFrame * ch];

            long freq = Stopwatch.Frequency;
            long produced = (long)((double)_clock.ElapsedTicks * AudioEngine.SampleRate / freq);
            while (_run)
            {
                long target = (long)((double)_clock.ElapsedTicks * AudioEngine.SampleRate / freq);
                if (target - produced > AudioEngine.SampleRate)
                {
                    produced = target - step;
                    encFill = 0;
                }
                while (produced + step <= target && _run)
                {
                    bool encoding = _encRun;
                    var extraMap = encoding ? _extraAudioMap : null;
                    Audio.Read(stepBuf, step, extraMap, encoding && _horizOutputs.Count > 0 ? stepBufHoriz : null);
                    produced += step;

                    if (!encoding)
                    {
                        encFill = 0;
                        continue;
                    }

                    Array.Copy(stepBuf, 0, encRing, encFill * ch, step * ch);
                    if (_horizOutputs.Count > 0)
                        Array.Copy(stepBufHoriz, 0, encRingHoriz, encFill * ch, step * ch);

                    lock (_audioEncLock)
                    {
                        if (!ReferenceEquals(extraMap, lastExtraMap))
                        {
                            lastExtraMap = extraMap;
                            extraRings.Clear();
                            if (extraMap != null)
                            {
                                for (int i = 0; i < _extraAudio.Count; i++)
                                    extraRings.Add(new float[(encFrame + step) * ch]);
                            }
                        }
                        if (extraMap != null && extraRings.Count == _extraAudio.Count)
                        {
                            for (int i = 0; i < _extraAudio.Count; i++)
                                Array.Copy(_extraAudio[i].Buf, 0, extraRings[i], encFill * ch, step * ch);
                        }

                        encFill += step;

                        while (encFill >= encFrame)
                        {
                            long pts = (produced - encFill) - _encStartSamples;
                            var aenc = _aenc;
                            var aencHoriz = _aencHoriz;
                            if (aenc != null && _encRun && pts >= 0)
                            {
                                Array.Copy(encRing, 0, encOut, 0, encFrame * ch);
                                aenc.Encode(encOut, pts, p => DeliverAudio(p, horizontal: false));
                                if (aencHoriz != null && _horizOutputs.Count > 0)
                                {
                                    Array.Copy(encRingHoriz, 0, encOutHoriz, 0, encFrame * ch);
                                    aencHoriz.Encode(encOutHoriz, pts, p => DeliverAudio(p, horizontal: true));
                                }
                                if (extraMap != null && extraRings.Count == _extraAudio.Count && _extraAudio.Count > 0)
                                {
                                    for (int i = 0; i < _extraAudio.Count; i++)
                                    {
                                        Array.Copy(extraRings[i], 0, extraOut, 0, encFrame * ch);
                                        int streamIdx = StreamOutput.AudioIndex + 1 + i;
                                        _extraAudio[i].Enc.Encode(extraOut, pts, p => DeliverToFile(p, streamIdx));
                                    }
                                }
                            }
                            int rem = encFill - encFrame;
                            if (rem > 0)
                            {
                                Array.Copy(encRing, encFrame * ch, encRing, 0, rem * ch);
                                if (_horizOutputs.Count > 0)
                                    Array.Copy(encRingHoriz, encFrame * ch, encRingHoriz, 0, rem * ch);
                                if (extraMap != null && extraRings.Count == _extraAudio.Count)
                                {
                                    for (int i = 0; i < extraRings.Count; i++)
                                        Array.Copy(extraRings[i], encFrame * ch, extraRings[i], 0, rem * ch);
                                }
                            }
                            encFill = rem;
                        }
                    }
                }
                long nextSample = produced + step;
                long nextTick = (long)((double)nextSample * freq / AudioEngine.SampleRate);
                waiter.WaitUntil(_clock, nextTick, spinFinish: false);
            }
        }
        catch (Exception ex) { Log.Error("Audio thread", ex); }
        finally { if (mm != IntPtr.Zero) Native.AvRevertMmThreadCharacteristics(mm); }
    }

    // ===================== render loop =====================

    private void RenderLoop()
    {
        uint idx = 0;
        var mm = Native.AvSetMmThreadCharacteristicsW("Games", ref idx);
        using var waiter = new PreciseWaiter();
        long freq = Stopwatch.Frequency;
        long next = _clock.ElapsedTicks;
        long frames = 0, skipped = 0, statFrames = 0;
        double renderAcc = 0;
        long statStart = _clock.ElapsedTicks;
        var bytesAtStat = new Dictionary<string, long>();
        var framesAtStat = new Dictionary<string, long>();
        var proc = Process.GetCurrentProcess();
        var cpuAtStat = proc.TotalProcessorTime;

        while (_run)
        {
            bool encoding = _venc != null;
            bool minimized = false;
            if (!encoding && _previewHwnd != IntPtr.Zero)
            {
                var root = Native.GetAncestor(_previewHwnd, Native.GA_ROOT);
                if (root != IntPtr.Zero && Native.IsIconic(root)) minimized = true;
            }
            int targetFps = encoding
                ? Math.Max(1, _fps)
                : ((!_previewEnabled || _previewHwnd == IntPtr.Zero || minimized) ? 5 : Math.Max(1, _fps));
            long interval = freq / targetFps;
            waiter.WaitUntil(_clock, next, spinFinish: encoding);
            long now = _clock.ElapsedTicks;
            long frameTime = next;
            if (now - next > interval * 2)
            {
                skipped += (now - next) / interval;
                next = now;
                frameTime = now;
            }
            next += interval;

            while (_cmds.TryDequeue(out var cmd))
            {
                try { cmd(); } catch (Exception ex) { Log.Error("Engine command", ex); }
            }
            if (!_run) break;

            long t0 = _clock.ElapsedTicks;
            try { RenderFrame(frameTime, interval, frames); }
            catch (Exception ex)
            {
                Log.Error("Frame", ex);
                _lastError = "Render error: " + ex.Message;
                foreach (var o in _outputs.ToArray()) RemoveOutput(o.Name, "render error");
                Thread.Sleep(100);
            }
            renderAcc += (_clock.ElapsedTicks - t0) * 1000.0 / freq;
            frames++; statFrames++;

            long statElapsed = _clock.ElapsedTicks - statStart;
            if (statElapsed >= freq)
            {
                double sec = (double)statElapsed / freq;
                proc.Refresh();
                var cpu = proc.TotalProcessorTime;
                var st = new EngineStats
                {
                    RenderFps = statFrames / sec,
                    RenderMs = renderAcc / Math.Max(1, statFrames),
                    SkippedFrames = skipped,
                    Streaming = IsStreaming,
                    Recording = IsRecording,
                    ReplayActive = IsReplayActive,
                    StreamSeconds = IsStreaming ? (double)(_clock.ElapsedTicks - _streamStart) / freq : 0,
                    RecordSeconds = IsRecording ? (double)(_clock.ElapsedTicks - _recordStart) / freq : 0,
                    EncodedFrames = _encodedFrames,
                    CpuPercent = (cpu - cpuAtStat).TotalSeconds / sec / Environment.ProcessorCount * 100,
                    LastError = _lastError,
                    Gpu = Gpu.AdapterName,
                    CanvasWidth = _comp.Width,
                    CanvasHeight = _comp.Height,
                };
                cpuAtStat = cpu;
                foreach (var o in _outputs)
                {
                    bytesAtStat.TryGetValue(o.Name, out var prev);
                    long cur = o.BytesWritten;
                    st.Outputs.Add(new OutputStats
                    {
                        Name = o.Name,
                        Status = o.Status,
                        Kbps = (cur - prev) * 8 / 1000.0 / sec,
                        Dropped = o.DroppedPackets,
                        Queue = o.QueueLength,
                        Reconnects = o.Reconnects,
                    });
                    bytesAtStat[o.Name] = cur;
                }
                foreach (var k in bytesAtStat.Keys.Where(k => _outputs.All(o => o.Name != k)).ToList()) bytesAtStat.Remove(k);
                foreach (var (id, rt) in _sources)
                {
                    framesAtStat.TryGetValue(id, out var pf);
                    rt.Fps = (rt.Frames - pf) / sec;
                    framesAtStat[id] = rt.Frames;
                    st.Sources.Add(new SourceStats { Id = id, Status = rt.Status, Fps = rt.Fps, CaptureFps = (rt as CameraRt)?.CaptureFps, Width = rt.Width, Height = rt.Height });
                }
                Stats = st;
                statFrames = 0; renderAcc = 0; statStart = _clock.ElapsedTicks;

                foreach (var o in _outputs.Where(o => o.Failed).ToArray())
                {
                    _lastError = $"{o.Name}: {o.Status}";
                    RemoveOutput(o.Name, o.Status);
                }
            }
        }

        foreach (var o in _outputs.ToArray()) RemoveOutput(o.Name, "application shutdown");
        var r = _replay;
        if (r != null)
        {
            _replay = null;
            TeardownEncoders();
            r.Dispose();
        }
        if (mm != IntPtr.Zero) Native.AvRevertMmThreadCharacteristics(mm);
    }

    private readonly List<DrawCmd> _drawList = new(32);
    private readonly List<DrawCmd> _dualDrawList = new(32);
    private readonly HashSet<string> _activeVisibleIds = new();

    private void BuildDualDrawList(Compositor dualComp, Scene? scene)
    {
        _dualDrawList.Clear();
        if (scene == null) return;
        float mainW = _comp.Width, mainH = _comp.Height;
        float dualW = dualComp.Width, dualH = dualComp.Height;
        if (mainW <= 0 || mainH <= 0 || dualW <= 0 || dualH <= 0) return;

        float aspectRatio = (mainW / mainH) / (dualW / dualH);
        int blurSlot = 0;
        var full = new Vector4(0, 0, 1, 1);

        foreach (var item in scene.Items)
        {
            if (!item.EffectiveDualVisible || !_sources.TryGetValue(item.SourceId, out var rt) || !rt.Ready) continue;
            float sw = rt.Width, sh = rt.Height;
            var cmd = new DrawCmd { Srv = rt.Srv!, Srv2 = rt.Srv2, Yuv = rt.Yuv, Hdr = rt.IsHdr, Opacity = Math.Clamp(item.Opacity, 0, 1), Darken = 1, Opaque = rt.Croppable && !rt.PreserveAlpha };
            float zoom = Math.Clamp(item.Zoom, 1f, 4f);

            FitMode fit = item.HasDualLayout ? item.DualFit : item.Fit;
            float ix = item.HasDualLayout ? item.DualX : item.X;
            float iy = item.HasDualLayout ? item.DualY : item.Y;
            float iw = item.HasDualLayout ? item.DualW : item.W;
            float ih = item.HasDualLayout ? item.DualH : item.H;

            switch (fit)
            {
                case FitMode.Fill:
                    cmd.SrcUv = CoverCrop(sw, sh, dualW, dualH, zoom, item.PanX, item.PanY);
                    cmd.Dst = new NRect(0, 0, 1, 1);
                    break;
                case FitMode.FitBlur:
                    if (!rt.PreserveAlpha && blurSlot < Compositor.BlurSlots)
                    {
                        int slot = blurSlot++;
                        dualComp.RenderBlurBackground(slot, rt.Srv!, rt.Yuv ? rt.Srv2 : null, CoverCrop(sw, sh, dualW, dualH, 1, 0, 0), rt.IsHdr);
                        _dualDrawList.Add(new DrawCmd { Srv = dualComp.BgSrv(slot), SrcUv = full, Dst = new NRect(0, 0, 1, 1), Opacity = cmd.Opacity, Opaque = true, Darken = 0.55f });
                    }
                    float sc = Math.Min(dualW / sw, dualH / sh);
                    float dw = sw * sc / dualW, dh = sh * sc / dualH;
                    cmd.SrcUv = CoverCrop(sw, sh, sw, sh, zoom, item.PanX, item.PanY);
                    cmd.Dst = new NRect((1 - dw) / 2, (1 - dh) / 2, dw, dh);
                    break;
                default:
                    if (!item.HasDualLayout)
                    {
                        float cx = ix + iw * 0.5f;
                        float cy = iy + ih * 0.5f;
                        float nw = iw * aspectRatio;
                        float nh = ih;
                        if (nw > 0.96f) { float k = 0.96f / nw; nw *= k; nh *= k; }
                        ix = cx - nw * 0.5f; iy = cy - nh * 0.5f; iw = nw; ih = nh;
                    }
                    cmd.Dst = new NRect(ix, iy, iw, ih);
                    cmd.SrcUv = rt.Croppable ? CoverCrop(sw, sh, iw * dualW, ih * dualH, zoom, item.PanX, item.PanY) : full;
                    cmd.RadiusPx = Math.Min(iw * dualW, ih * dualH) * Math.Clamp(item.Corner, 0, 0.5f);
                    break;
            }

            float cl = item.HasDualLayout ? item.DualCropLeft : item.CropLeft;
            float cr = item.HasDualLayout ? item.DualCropRight : item.CropRight;
            float ct = item.HasDualLayout ? item.DualCropTop : item.CropTop;
            float cb = item.HasDualLayout ? item.DualCropBottom : item.CropBottom;
            ClampCropPair(ref cl, ref cr);
            ClampCropPair(ref ct, ref cb);
            if (cl > 0 || cr > 0 || ct > 0 || cb > 0)
            {
                float uSpan = cmd.SrcUv.Z - cmd.SrcUv.X;
                float vSpan = cmd.SrcUv.W - cmd.SrcUv.Y;
                cmd.SrcUv = new Vector4(cmd.SrcUv.X + uSpan * cl, cmd.SrcUv.Y + vSpan * ct, cmd.SrcUv.Z - uSpan * cr, cmd.SrcUv.W - vSpan * cb);
            }
            cmd.Brightness = Math.Clamp(item.Brightness, -0.5f, 0.5f);
            cmd.ContrastDelta = Math.Clamp(item.Contrast, 0.5f, 2.0f) - 1f;
            cmd.SaturationDelta = Math.Clamp(item.Saturation, 0f, 2.0f) - 1f;
            if (item.ChromaKeyEnabled)
            {
                uint argb = unchecked((uint)item.ChromaKeyArgb);
                cmd.ChromaKeyEnabled = true;
                cmd.ChromaKeyRgb = new Vector3(((argb >> 16) & 0xFF) / 255f, ((argb >> 8) & 0xFF) / 255f, (argb & 0xFF) / 255f);
                cmd.ChromaKeySimilarity = Math.Clamp(item.ChromaKeySimilarity, 0.01f, 0.6f);
                cmd.ChromaKeySmoothness = Math.Clamp(item.ChromaKeySmoothness, 0.01f, 0.3f);
            }
            if (item.ShadowEnabled)
            {
                cmd.ShadowEnabled = true;
                cmd.ShadowOffsetX = Math.Clamp(item.ShadowOffsetX, -0.1f, 0.1f);
                cmd.ShadowOffsetY = Math.Clamp(item.ShadowOffsetY, -0.1f, 0.1f);
                cmd.ShadowBlur = Math.Clamp(item.ShadowBlur, 0.002f, 0.12f);
                cmd.ShadowIntensity = Math.Clamp(item.ShadowIntensity, 0f, 1f);
            }
            if (item.Mirror) cmd.SrcUv = new Vector4(cmd.SrcUv.Z, cmd.SrcUv.Y, cmd.SrcUv.X, cmd.SrcUv.W);
            _dualDrawList.Add(cmd);
        }
    }

    private void RenderFrame(long frameTime, long interval, long frameNo)
    {
        var ctx = Gpu.Context;
        var s = _scene;
        float W = _comp.Width, H = _comp.Height;

        // 1. update sources (pause hidden Media/Browser sources to save CPU when not visible in active scene)
        _activeVisibleIds.Clear();
        var activeSc = s.ActiveScene;
        if (activeSc != null)
        {
            foreach (var it in activeSc.Items)
            {
                if (it.Visible || (_dualComp != null && it.EffectiveDualVisible))
                    _activeVisibleIds.Add(it.SourceId);
            }
        }
        foreach (var (sid, rt) in _sources)
        {
            bool vis = _activeVisibleIds.Contains(sid);
            rt.SetSceneVisible(vis);
            if (vis || rt is not (MediaRt or BrowserRt))
                rt.Update(Gpu, _comp);
        }
        _monitors.EndFrame();

        // 2. active scene items, bottom to top
        _drawList.Clear();
        int blurSlot = 0;
        var scene = s.ActiveScene;
        if (scene != null && !string.IsNullOrEmpty(_lastSceneId) && scene.Id != _lastSceneId && s.SceneTransitionMs > 0)
        {
            _comp.CapturePreviousCanvas();
            _transStartTicks = frameTime;
            _transDurTicks = Math.Clamp(s.SceneTransitionMs, 10, 5000) * Stopwatch.Frequency / 1000;
        }
        _lastSceneId = scene?.Id ?? "";
        if (scene != null)
        {
            foreach (var item in scene.Items)
            {
                if (!item.Visible || !_sources.TryGetValue(item.SourceId, out var rt) || !rt.Ready) continue;
                float sw = rt.Width, sh = rt.Height;
                var full = new Vector4(0, 0, 1, 1);
                var cmd = new DrawCmd { Srv = rt.Srv!, Srv2 = rt.Srv2, Yuv = rt.Yuv, Hdr = rt.IsHdr, Opacity = Math.Clamp(item.Opacity, 0, 1), Darken = 1, Opaque = rt.Croppable && !rt.PreserveAlpha };
                float zoom = Math.Clamp(item.Zoom, 1f, 4f);

                switch (item.Fit)
                {
                    case FitMode.Fill:
                        cmd.SrcUv = CoverCrop(sw, sh, W, H, zoom, item.PanX, item.PanY);
                        cmd.Dst = new NRect(0, 0, 1, 1);
                        break;
                    case FitMode.FitBlur:
                        if (!rt.PreserveAlpha && blurSlot < Compositor.BlurSlots)
                        {
                            int slot = blurSlot++;
                            _comp.RenderBlurBackground(slot, rt.Srv!, rt.Yuv ? rt.Srv2 : null, CoverCrop(sw, sh, W, H, 1, 0, 0), rt.IsHdr);
                            _drawList.Add(new DrawCmd { Srv = _comp.BgSrv(slot), SrcUv = full, Dst = new NRect(0, 0, 1, 1), Opacity = cmd.Opacity, Opaque = true, Darken = 0.55f });
                        }
                        // fit completely (contain) and center
                        float sc = Math.Min(W / sw, H / sh);
                        float dw = sw * sc / W, dh = sh * sc / H;
                        cmd.SrcUv = CoverCrop(sw, sh, sw, sh, zoom, item.PanX, item.PanY);
                        cmd.Dst = new NRect((1 - dw) / 2, (1 - dh) / 2, dw, dh);
                        break;
                    default:
                        cmd.Dst = new NRect(item.X, item.Y, item.W, item.H);
                        cmd.SrcUv = rt.Croppable ? CoverCrop(sw, sh, item.W * W, item.H * H, zoom, item.PanX, item.PanY) : full;
                        cmd.RadiusPx = Math.Min(item.W * W, item.H * H) * Math.Clamp(item.Corner, 0, 0.5f);
                        break;
                }
                if (item.CropLeft > 0 || item.CropRight > 0 || item.CropTop > 0 || item.CropBottom > 0)
                {
                    float cl = item.CropLeft, cr = item.CropRight, ct = item.CropTop, cb = item.CropBottom;
                    ClampCropPair(ref cl, ref cr);
                    ClampCropPair(ref ct, ref cb);
                    float uSpan = cmd.SrcUv.Z - cmd.SrcUv.X;
                    float vSpan = cmd.SrcUv.W - cmd.SrcUv.Y;
                    cmd.SrcUv = new Vector4(
                        cmd.SrcUv.X + uSpan * cl,
                        cmd.SrcUv.Y + vSpan * ct,
                        cmd.SrcUv.Z - uSpan * cr,
                        cmd.SrcUv.W - vSpan * cb);
                }
                cmd.Brightness = Math.Clamp(item.Brightness, -0.5f, 0.5f);
                cmd.ContrastDelta = Math.Clamp(item.Contrast, 0.5f, 2.0f) - 1f;
                cmd.SaturationDelta = Math.Clamp(item.Saturation, 0f, 2.0f) - 1f;
                if (item.ChromaKeyEnabled)
                {
                    uint argb = unchecked((uint)item.ChromaKeyArgb);
                    cmd.ChromaKeyEnabled = true;
                    cmd.ChromaKeyRgb = new Vector3(((argb >> 16) & 0xFF) / 255f, ((argb >> 8) & 0xFF) / 255f, (argb & 0xFF) / 255f);
                    cmd.ChromaKeySimilarity = Math.Clamp(item.ChromaKeySimilarity, 0.01f, 0.6f);
                    cmd.ChromaKeySmoothness = Math.Clamp(item.ChromaKeySmoothness, 0.01f, 0.3f);
                }
                if (item.ShadowEnabled)
                {
                    cmd.ShadowEnabled = true;
                    cmd.ShadowOffsetX = Math.Clamp(item.ShadowOffsetX, -0.1f, 0.1f);
                    cmd.ShadowOffsetY = Math.Clamp(item.ShadowOffsetY, -0.1f, 0.1f);
                    cmd.ShadowBlur = Math.Clamp(item.ShadowBlur, 0.002f, 0.12f);
                    cmd.ShadowIntensity = Math.Clamp(item.ShadowIntensity, 0f, 1f);
                }
                if (item.Mirror) cmd.SrcUv = new Vector4(cmd.SrcUv.Z, cmd.SrcUv.Y, cmd.SrcUv.X, cmd.SrcUv.W);
                _drawList.Add(cmd);
            }
        }

        _comp.RenderCanvas(System.Runtime.InteropServices.CollectionsMarshal.AsSpan(_drawList));
        if (_transDurTicks > 0)
        {
            double t = (frameTime - _transStartTicks) / (double)_transDurTicks;
            if (t >= 0 && t < 1.0) _comp.BlendPreviousCanvas((float)(1.0 - t));
            else if (t >= 1.0) _transDurTicks = 0;
        }

        // 3. encode — straight into NVENC texture
        bool dualRendered = false;
        if (_venc != null)
        {
            // PTS is derived from the scheduled frame time so late wakeups never duplicate timestamps.
            long elapsed = frameTime - _encStartTicks + interval / 2;
            long pts = elapsed < 0 ? -1 : elapsed / interval;
            if (pts > _lastPts)
            {
                var frame = _venc.AcquireFrame(out var y, out var uvRtv);
                _comp.ConvertToNv12(y, uvRtv);
                ctx.Flush();
                bool key = _forceKey;
                _forceKey = false;
                _venc.Encode(frame, pts, key, p => Deliver(p, StreamOutput.VideoIndex));

                bool horizKey = _forceHorizKey;
                _forceHorizKey = false;
                if (HasHorizVideoEncoder && _dualComp != null)
                {
                    BuildDualDrawList(_dualComp, scene);
                    _dualComp.RenderCanvas(System.Runtime.InteropServices.CollectionsMarshal.AsSpan(_dualDrawList));
                    dualRendered = true;
                }
                if (_horizVenc != null && _dualComp != null)
                {
                    var hFrame = _horizVenc.AcquireFrame(out var hy, out var huv);
                    _dualComp.ConvertToNv12(hy, huv);
                    ctx.Flush();
                    _horizVenc.Encode(hFrame, pts, horizKey || key, DeliverHorizVideo);
                }
                if (_recVenc != null && _recVideoOutputName != null)
                {
                    var recComp = _recVencIsHoriz ? _dualComp! : _comp;
                    var rFrame = _recVenc.AcquireFrame(out var ry, out var ruv);
                    recComp.ConvertToNv12(ry, ruv);
                    ctx.Flush();
                    string recOutName = _recVideoOutputName;
                    _recVenc.Encode(rFrame, pts, (_recVencIsHoriz ? horizKey : false) || key, p => DeliverToFile(p, StreamOutput.VideoIndex, recOutName));
                }
                _lastPts = pts;
                _encodedFrames++;
            }
        }

        // 4. preview (main + optional horizontal Dual-Canvas dock preview)
        if (_previewDirty)
        {
            _previewDirty = false;
            try { _comp.SetPreviewTarget(_previewEnabled ? _previewHwnd : IntPtr.Zero, _previewW, _previewH); }
            catch (Exception ex) { Log.Warn("Preview: " + ex.Message); }
        }
        if (_previewEnabled && _previewHwnd != IntPtr.Zero)
            _comp.PresentPreview(_selection);

        bool wantDualPreview = _previewEnabled && _dualPreviewHwnd != IntPtr.Zero;
        if (wantDualPreview)
        {
            int dw = Math.Clamp(s.DualCanvasWidth & ~1, 128, 4096);
            int dh = Math.Clamp(s.DualCanvasHeight & ~1, 128, 4096);
            if (_dualComp == null || (!HasHorizVideoEncoder && (_dualComp.Width != dw || _dualComp.Height != dh)))
            {
                _dualComp?.Dispose();
                _dualComp = new Compositor(Gpu, dw, dh);
                _dualPreviewDirty = true;
                dualRendered = false;
            }
            if (_dualPreviewDirty)
            {
                _dualPreviewDirty = false;
                try { _dualComp.SetPreviewTarget(_dualPreviewHwnd, _dualPreviewW, _dualPreviewH); }
                catch (Exception ex) { Log.Warn("Dual preview: " + ex.Message); }
            }
            if (!dualRendered)
            {
                BuildDualDrawList(_dualComp, scene);
                _dualComp.RenderCanvas(System.Runtime.InteropServices.CollectionsMarshal.AsSpan(_dualDrawList));
            }
            _dualComp.PresentPreview(_dualSelection);
        }
        else if (_dualPreviewDirty)
        {
            _dualPreviewDirty = false;
            try { _dualComp?.SetPreviewTarget(IntPtr.Zero, 0, 0); } catch { }
            if (!HasHorizVideoEncoder && _dualComp != null)
            {
                _dualComp.Dispose();
                _dualComp = null;
            }
        }
    }

    private static Vector4 CoverCrop(float sw, float sh, float dw, float dh, float zoom, float panX, float panY)
    {
        if (sw <= 0 || sh <= 0 || dw <= 0 || dh <= 0) return new Vector4(0, 0, 1, 1);
        float srcAspect = sw / sh, dstAspect = dw / dh;
        float cw = 1, ch = 1;
        if (srcAspect > dstAspect) cw = dstAspect / srcAspect; else ch = srcAspect / dstAspect;
        cw /= zoom; ch /= zoom;
        float cx = 0.5f + Math.Clamp(panX, -1, 1) * (1 - cw) / 2;
        float cy = 0.5f + Math.Clamp(panY, -1, 1) * (1 - ch) / 2;
        return new Vector4(cx - cw / 2, cy - ch / 2, cx + cw / 2, cy + ch / 2);
    }

    public void Dispose()
    {
        _run = false;
        _renderThread.Join(10000);
        _audioThread.Join(2000);
        foreach (var rt in _sources.Values) rt.Dispose();
        _sources.Clear();
        CefHost.Shutdown();
        Tts.Dispose();
        NowPlaying.Dispose();
        Audio.Dispose();
        _dualComp?.Dispose();
        _comp.Dispose();
        Gpu.Dispose();
        Native.timeEndPeriod(1);
        System.Runtime.GCSettings.LatencyMode = _prevGcMode;
    }
}
