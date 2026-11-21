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

    private IntPtr _previewHwnd;
    private int _previewW, _previewH;
    private bool _previewDirty;
    private volatile bool _previewEnabled = true;
    public NRect? Selection { get; set; }

    // outputs
    private VideoEncoder? _venc;
    private VideoEncoder? _recVenc;    // dedicated encoder when RecordCodec != h264 or DualCanvas is enabled
    private Compositor? _dualComp;     // secondary horizontal/custom canvas for simultaneous dual-canvas recording
    private AudioEncoder? _aenc;
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
    private long _encodedFrames;
    private long _streamStart, _recordStart;
    private string _lastError = "";
    private string _lastSceneId = "";
    private long _transStartTicks, _transDurTicks;
    private readonly System.Runtime.GCLatencyMode _prevGcMode;

    public bool IsStreaming => _outputs.Any(o => o.Name != "File");
    public bool IsRecording => _outputs.Any(o => o.Name == "File");
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
        s.ActiveScene?.Items.Where(i => i.Visible).Select(i => i.SourceId).ToHashSet() ?? new HashSet<string>();

    public void ApplySettings(AppSettings s, AppSettings previous)
    {
        var copy = s.Clone();
        if (Sig(copy.Sources.Where(x => !x.IsVisual)) != Sig(previous.Sources.Where(x => !x.IsVisual)) ||
            !AudibleDeviceIds(copy).SetEquals(AudibleDeviceIds(previous)))
            Audio.SyncDevices(copy);
        var oldMedia = previous.Sources.Where(x => x.Type == SourceType.Media).ToDictionary(x => x.Id);
        foreach (var m in copy.Sources.Where(x => x.Type == SourceType.Media))
        {
            if (!oldMedia.TryGetValue(m.Id, out var old) || (m.Muted, m.Volume) != (old.Muted, old.Volume))
                Audio.SetVolume(m.Id, m.Muted ? 0 : m.Volume);
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

    public void SetItemTransform(string sceneId, string itemId, NRect rect, FitMode fit) => _cmds.Enqueue(() =>
    {
        var sc = _scene.Scenes.FirstOrDefault(x => x.Id == sceneId);
        var it = sc?.Items.FirstOrDefault(x => x.Id == itemId);
        if (it == null) return;
        it.X = rect.X; it.Y = rect.Y; it.W = rect.W; it.H = rect.H; it.Fit = fit;
        _selection = rect;
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
                    SourceType.Window or SourceType.Game => new WindowCaptureRt { Def = d, ConfigKey = key },
                    SourceType.Camera => new CameraRt(d, _fps) { ConfigKey = key },
                    SourceType.Media => new MediaRt(d, Audio) { ConfigKey = key },
                    SourceType.Color => new ColorRt(d, _comp, Gpu) { ConfigKey = key },
                    SourceType.Browser => new BrowserRt(d) { ConfigKey = key },
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
    public void ClearError() => _lastError = "";

    public BrowserCapture? GetBrowser(string sourceId) =>
        _sources.TryGetValue(sourceId, out var rt) && rt is BrowserRt br ? br.Capture : null;

    // ===================== streaming / recording =====================

    public Task<string?> StartStreamAsync() => RunOnRender(() =>
    {
        if (IsStreaming) return;
        var url = _scene.BuildRtmpUrl();
        if (url.Length == 0 || string.IsNullOrWhiteSpace(_scene.StreamKey))
            throw new InvalidOperationException("Missing server URL or stream key (Settings → Stream).");
        EnsureEncoders();
        var opts = new OutputOptions(_scene.AutoReconnect, _scene.ReconnectDelaySec, _scene.ReconnectMaxRetries);
        AddOutput(new StreamOutput("Stream", url, _venc!, _aenc!, _fps, opts, RequestKeyframe));
        if (_scene.SecondStreamEnabled)
        {
            var url2 = _scene.BuildSecondRtmpUrl();
            if (url2.Length > 0 && !string.IsNullOrWhiteSpace(_scene.SecondStreamKey))
            {
                var name2 = string.IsNullOrWhiteSpace(_scene.SecondStreamName) ? "Second Channel" : _scene.SecondStreamName.Trim();
                if (name2 == "Stream" || name2 == "File") name2 = "Stream 2";
                AddOutput(new StreamOutput(name2, url2, _venc!, _aenc!, _fps, opts, RequestKeyframe));
            }
        }
        _streamStart = _clock.ElapsedTicks;
        if (_scene.RecordToFile && !IsRecording) StartRecordInternal();
    });

    public Task<string?> StopStreamAsync() => RunOnRender(() =>
    {
        foreach (var name in _outputs.Where(x => x.Name != "File").Select(x => x.Name).ToArray())
            RemoveOutput(name, "stopped");
    });
    public Task<string?> StartRecordAsync() => RunOnRender(() => { if (!IsRecording) StartRecordInternal(); });
    public Task<string?> StopRecordAsync() => RunOnRender(() => RemoveOutput("File", "stopped"));

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

        var recVenc = _venc!;
        var s = _scene;
        bool needDedicatedRecVenc = s.DualCanvasEnabled || (s.RecordCodec is "hevc" or "av1");
        if (needDedicatedRecVenc)
        {
            int rw = s.DualCanvasEnabled ? Math.Clamp(s.DualCanvasWidth & ~1, 128, 4096) : _comp.Width;
            int rh = s.DualCanvasEnabled ? Math.Clamp(s.DualCanvasHeight & ~1, 128, 4096) : _comp.Height;
            if (s.DualCanvasEnabled)
            {
                _dualComp?.Dispose();
                _dualComp = new Compositor(Gpu, rw, rh);
            }
            _recVenc?.Dispose();
            _recVenc = new VideoEncoder(Gpu, rw, rh, new VideoEncoderOptions(
                _fps, s.VideoBitrateKbps, s.NvencPreset, s.NvencProfile, s.KeyframeSec, s.BFrames, s.SpatialAq, s.LookAhead, s.NvencZeroFpsImpact, s.RecordCodec), globalHeader: true);
            recVenc = _recVenc;
        }

        AddOutput(new StreamOutput("File", path, recVenc, _aenc!, _fps, new OutputOptions(false, 0, 0), RequestKeyframe, extraForOutput));
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
        _encStartTicks = _clock.ElapsedTicks;
        _lastPts = -1;
        _encodedFrames = 0;
        _forceKey = true;
        lock (_audioEncLock)
        {
            _encStartSamples = (long)((double)_encStartTicks * AudioEngine.SampleRate / Stopwatch.Frequency);
            _aenc = aenc;
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
        if (name == "File")
        {
            if (_recVenc != null)
            {
                try { _recVenc.Flush(p => DeliverTo(p, StreamOutput.VideoIndex, o)); } catch { }
                _recVenc.Dispose();
                _recVenc = null;
            }
            _dualComp?.Dispose();
            _dualComp = null;
            lock (_audioEncLock) DisposeExtraAudioLocked(flushTo: o);
        }
        if (_outputs.Length == 0 && _replay == null) TeardownEncoders(flushTo: o);
        Task.Run(o.Dispose);
        Log.Info($"=== STOP: {name} ({reason})");
    }

    private void TeardownEncoders(StreamOutput? flushTo = null)
    {
        AudioEncoder? aenc;
        lock (_audioEncLock)
        {
            _encRun = false;
            aenc = _aenc;
            _aenc = null;
            DisposeExtraAudioLocked(flushTo);
        }
        if (flushTo != null && _venc != null && aenc != null)
        {
            try
            {
                _venc.Flush(p => DeliverTo(p, StreamOutput.VideoIndex, flushTo));
                aenc.Flush(p => DeliverTo(p, StreamOutput.AudioIndex, flushTo));
            }
            catch (Exception ex) { Log.Warn("Encoder flush: " + ex.Message); }
        }
        _venc?.Dispose(); _venc = null;
        aenc?.Dispose();
    }

    private void Deliver(IntPtr p, int streamIndex)
    {
        var pkt = (AVPacket*)p;
        pkt->stream_index = streamIndex;
        bool dedicatedFileVideo = streamIndex == StreamOutput.VideoIndex && _recVenc != null;
        foreach (var o in _outputs)
        {
            if (dedicatedFileVideo && o.Name == "File") continue;
            o.Enqueue(pkt);
        }
        _replay?.Enqueue(pkt, streamIndex);
        ffmpeg.av_packet_free(&pkt);
    }

    private void DeliverToFile(IntPtr p, int streamIndex)
    {
        var pkt = (AVPacket*)p;
        pkt->stream_index = streamIndex;
        foreach (var o in _outputs)
        {
            if (o.Name == "File") { o.Enqueue(pkt); break; }
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

    /// <summary>Audio pump: runs continuously (for mixer meters), encodes only while an output is active.</summary>
    private void AudioLoop()
    {
        uint idx = 0;
        var mm = Native.AvSetMmThreadCharacteristicsW("Pro Audio", ref idx);
        try
        {
            const int n = 1024;
            var buf = new float[n * AudioEngine.Channels];
            long freq = Stopwatch.Frequency;
            long produced = (long)((double)_clock.ElapsedTicks * AudioEngine.SampleRate / freq);
            while (_run)
            {
                long target = (long)((double)_clock.ElapsedTicks * AudioEngine.SampleRate / freq);
                if (target - produced > AudioEngine.SampleRate) produced = target - n;
                while (produced + n <= target && _run)
                {
                    var extraMap = _extraAudioMap;
                    Audio.Read(buf, n, extraMap);
                    lock (_audioEncLock)
                    {
                        var aenc = _aenc;
                        if (aenc != null && _encRun)
                        {
                            long pts = produced - _encStartSamples;
                            if (pts >= 0)
                            {
                                aenc.Encode(buf, pts, p => Deliver(p, StreamOutput.AudioIndex));
                                if (extraMap != null && _extraAudio.Count > 0)
                                {
                                    for (int i = 0; i < _extraAudio.Count; i++)
                                    {
                                        int streamIdx = StreamOutput.AudioIndex + 1 + i;
                                        _extraAudio[i].Enc.Encode(_extraAudio[i].Buf, pts, p => DeliverToFile(p, streamIdx));
                                    }
                                }
                            }
                        }
                    }
                    produced += n;
                }
                Thread.Sleep(5);
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
            long interval = freq / Math.Max(1, _fps);
            waiter.WaitUntil(_clock, next);
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

    private void RenderFrame(long frameTime, long interval, long frameNo)
    {
        var ctx = Gpu.Context;
        var s = _scene;
        float W = _comp.Width, H = _comp.Height;

        // 1. update all sources (including inactive scenes so scene switching is instant)
        foreach (var rt in _sources.Values) rt.Update(Gpu, _comp);
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
                    float cl = Math.Clamp(item.CropLeft, 0f, 0.45f);
                    float cr = Math.Clamp(item.CropRight, 0f, 0.45f);
                    float ct = Math.Clamp(item.CropTop, 0f, 0.45f);
                    float cb = Math.Clamp(item.CropBottom, 0f, 0.45f);
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
                if (_recVenc != null)
                {
                    var recComp = _dualComp ?? _comp;
                    if (_dualComp != null)
                    {
                        _dualComp.RenderCanvas(System.Runtime.InteropServices.CollectionsMarshal.AsSpan(_drawList));
                    }
                    var rFrame = _recVenc.AcquireFrame(out var ry, out var ruv);
                    recComp.ConvertToNv12(ry, ruv);
                    ctx.Flush();
                    _recVenc.Encode(rFrame, pts, key, p => DeliverToFile(p, StreamOutput.VideoIndex));
                }
                _lastPts = pts;
                _encodedFrames++;
            }
        }

        // 4. preview
        if (_previewDirty)
        {
            _previewDirty = false;
            try { _comp.SetPreviewTarget(_previewEnabled ? _previewHwnd : IntPtr.Zero, _previewW, _previewH); }
            catch (Exception ex) { Log.Warn("Preview: " + ex.Message); }
        }
        if (_previewEnabled && _previewHwnd != IntPtr.Zero)
            _comp.PresentPreview(_selection);
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
        Audio.Dispose();
        _comp.Dispose();
        Gpu.Dispose();
        Native.timeEndPeriod(1);
        System.Runtime.GCSettings.LatencyMode = _prevGcMode;
    }
}
