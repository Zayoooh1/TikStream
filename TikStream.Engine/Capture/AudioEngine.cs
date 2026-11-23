using System.Collections.Concurrent;
using NAudio.CoreAudioApi;
using NAudio.Wave;
using NAudio.Wave.SampleProviders;
using TikStream.Core;

namespace TikStream.Capture;

/// <summary>
/// Mixes all audio sources to 48 kHz stereo float.
/// The consumer (audio pump) reads at engine clock pace; buffers return silence when idle,
/// so audio never stalls when the desktop is silent (classic WASAPI loopback quirk).
/// </summary>
public sealed class AudioEngine : IDisposable
{
    public const int SampleRate = 48000;
    public const int Channels = 2;
    public static readonly WaveFormat MixFormat = WaveFormat.CreateIeeeFloatWaveFormat(SampleRate, Channels);

    private readonly MixingSampleProvider _mixer = new(MixFormat) { ReadFully = true };
    private readonly Dictionary<string, Input> _inputs = new();
    private readonly object _lock = new();
    private WasapiOut? _monitorOut;
    private BufferedWaveProvider? _monitorBuf;
    private float[] _monitorMix = new float[2048];
    private float[] _monitorTmp = new float[2048];
    private byte[] _monitorBytes = new byte[2048 * 4];

    /// <summary>Peak levels (0..1) per source — used for mixer meters.</summary>
    public IReadOnlyDictionary<string, float> Levels
    {
        get { lock (_lock) return _inputs.ToDictionary(k => k.Key, v => v.Value.Peak); }
    }

    /// <summary>True when any device has stopped or is waiting for retry (hot-plug / recovery).</summary>
    public bool NeedsSync
    {
        get
        {
            lock (_lock)
            {
                if (_inputs.Values.Any(i => i.Stopped)) return true;
                var now = DateTime.UtcNow;
                return _failed.Values.Any(f => now >= f.Retry);
            }
        }
    }

    /// <summary>Synchronizes devices (inputs/outputs) with the source list. Media sources register separately.</summary>
    private readonly Dictionary<string, (string Key, DateTime Retry)> _failed = new();

    public void SyncDevices(AppSettings settings)
    {
        var audibleV = settings.ActiveScene?.Items.Where(i => i.Visible).Select(i => i.SourceId).ToHashSet();
        var audibleH = settings.ActiveScene?.Items.Where(i => i.EffectiveDualVisible).Select(i => i.SourceId).ToHashSet();
        lock (_lock)
        {
            var wanted = settings.Sources.Where(d => !d.IsVisual).ToDictionary(d => d.Id);
            foreach (var id in _inputs.Where(kv => kv.Value.IsDevice && !wanted.ContainsKey(kv.Key)).Select(kv => kv.Key).ToList())
            {
                _inputs[id].Dispose(_mixer);
                _inputs.Remove(id);
            }
            foreach (var d in wanted.Values)
            {
                string target = d.DeviceId;
                if (d.Type != SourceType.AudioApplication && string.IsNullOrEmpty(target))
                    target = DefaultDeviceId(d.Type == SourceType.AudioInput ? DataFlow.Capture : DataFlow.Render);
                string key = d.Type + "|" + target;
                if (_inputs.TryGetValue(d.Id, out var cur) && (cur.Key != key || cur.Stopped))
                {
                    cur.Dispose(_mixer);
                    _inputs.Remove(d.Id);
                    cur = null;
                }
                // unavailable device (e.g. unplugged headset) at most every 5 s, not on every settings change
                if (cur == null && (!_failed.TryGetValue(d.Id, out var f) || f.Key != key || DateTime.UtcNow >= f.Retry))
                {
                    try
                    {
                        var cap = CreateCapture(d.Type, target);
                        if (cap != null) { _inputs[d.Id] = Input.FromDevice(d.Name, key, cap, _mixer, d.Type == SourceType.AudioInput ? 16 : 35); _failed.Remove(d.Id); }
                        else _failed[d.Id] = (key, DateTime.UtcNow.AddSeconds(5));
                    }
                    catch (Exception ex)
                    {
                        if (!_failed.ContainsKey(d.Id)) Log.Warn($"Audio '{d.Name}': device unavailable ({ex.Message.Trim()})");
                        _failed[d.Id] = (key, DateTime.UtcNow.AddSeconds(5));
                    }
                }
                if (_inputs.TryGetValue(d.Id, out var inp))
                {
                    bool onV = !d.Muted && (d.GlobalAudio == true ? (d.AudioVertical ?? true) : audibleV?.Contains(d.Id) == true);
                    bool onH = !d.Muted && (d.GlobalAudio == true ? (d.AudioHorizontal ?? true) : audibleH?.Contains(d.Id) == true);
                    inp.SetVolume((onV || onH) ? d.Volume : 0);
                    inp.SetRouting(onV, onH);
                    inp.SetMonitorMode(d.MonitorMode);
                    inp.SetFilters(d.SyncOffsetMs, d.NoiseSuppressEnabled, d.NoiseSuppressMethod, d.NoiseSuppressLevelDb,
                        d.NoiseGateEnabled, d.NoiseGateThresholdDb, d.CompressorEnabled, d.CompressorThresholdDb, d.CompressorRatio, d.LimiterEnabled, d.LimiterCeilingDb);
                }
            }
            foreach (var d in settings.Sources.Where(x => x.Type == SourceType.Browser))
            {
                if (_inputs.TryGetValue(d.Id, out var inp))
                {
                    bool onV = !d.Muted && !d.BrowserMuted && (audibleV?.Contains(d.Id) == true);
                    bool onH = !d.Muted && !d.BrowserMuted && (audibleH?.Contains(d.Id) == true);
                    inp.SetVolume((onV || onH) ? d.Volume : 0);
                    inp.SetRouting(onV, onH);
                    inp.SetMonitorMode(string.IsNullOrEmpty(d.MonitorMode) ? "MonitorAndOutput" : d.MonitorMode);
                    inp.SetFilters(d.SyncOffsetMs, d.NoiseSuppressEnabled, d.NoiseSuppressMethod, d.NoiseSuppressLevelDb,
                        d.NoiseGateEnabled, d.NoiseGateThresholdDb, d.CompressorEnabled, d.CompressorThresholdDb, d.CompressorRatio, d.LimiterEnabled, d.LimiterCeilingDb);
                }
            }
            foreach (var d in settings.Sources.Where(x => x.Type == SourceType.Media))
            {
                if (_inputs.TryGetValue(d.Id, out var inp))
                {
                    bool onV = !d.Muted && (audibleV?.Contains(d.Id) == true);
                    bool onH = !d.Muted && (audibleH?.Contains(d.Id) == true);
                    inp.SetRouting(onV, onH);
                }
            }
            foreach (var d in settings.Sources.Where(x => x.Type == SourceType.AlertBox))
            {
                if (!_inputs.TryGetValue(d.Id, out var inp))
                {
                    inp = Input.FromPush(d.Name, _mixer, isAlert: true);
                    _inputs[d.Id] = inp;
                }
                bool onV = !d.Muted && (audibleV?.Contains(d.Id) != false);
                bool onH = !d.Muted && (audibleH?.Contains(d.Id) != false);
                inp.SetVolume((onV || onH) ? d.Volume : 0);
                inp.SetRouting(onV, onH);
                inp.SetMonitorMode(string.IsNullOrEmpty(d.MonitorMode) ? "MonitorAndOutput" : d.MonitorMode);
                inp.SetFilters(d.SyncOffsetMs, d.NoiseSuppressEnabled, d.NoiseSuppressMethod, d.NoiseSuppressLevelDb,
                    d.NoiseGateEnabled, d.NoiseGateThresholdDb, d.CompressorEnabled, d.CompressorThresholdDb, d.CompressorRatio, d.LimiterEnabled, d.LimiterCeilingDb);
            }
            var alertIds = settings.Sources.Where(x => x.Type == SourceType.AlertBox).Select(x => x.Id).ToHashSet();
            foreach (var id in _inputs.Where(kv => kv.Value.IsAlert && !alertIds.Contains(kv.Key)).Select(kv => kv.Key).ToList())
            {
                _inputs[id].Dispose(_mixer);
                _inputs.Remove(id);
            }
        }
    }

    public static string DefaultDeviceId(DataFlow flow)
    {
        using var en = new MMDeviceEnumerator();
        if (!en.HasDefaultAudioEndpoint(flow, Role.Multimedia)) return "";
        using var device = en.GetDefaultAudioEndpoint(flow, Role.Multimedia);
        return device.ID;
    }

    private static IWaveIn? CreateCapture(SourceType type, string target)
    {
        if (string.IsNullOrWhiteSpace(target)) return null;
        if (type == SourceType.AudioApplication) return new ApplicationAudioCapture(target);
        using var en = new MMDeviceEnumerator();
        using var device = en.GetDevice(target);
        if (device.State != DeviceState.Active) return null;
        if (type == SourceType.AudioOutput) return new WasapiLoopbackCapture(device);
        try { return new WasapiCapture(device, true, 10); }
        catch { return new WasapiCapture(device, false, 20); }
    }

    /// <summary>Programmatically fed input (media file audio): 48 kHz interleaved stereo float.</summary>
    public Action<float[], int> RegisterPushInput(string id, string name)
    {
        lock (_lock)
        {
            if (_inputs.Remove(id, out var old)) old.Dispose(_mixer);
            var inp = Input.FromPush(name, _mixer);
            _inputs[id] = inp;
            return inp.Push;
        }
    }

    public void SetVolume(string id, float v)
    {
        lock (_lock) if (_inputs.TryGetValue(id, out var i)) i.SetVolume(v);
    }

    public void SetFilters(string id, SourceDef d)
    {
        lock (_lock)
            if (_inputs.TryGetValue(id, out var i))
            {
                i.SetMonitorMode(d.MonitorMode);
                i.SetFilters(d.SyncOffsetMs, d.NoiseSuppressEnabled, d.NoiseSuppressMethod, d.NoiseSuppressLevelDb,
                    d.NoiseGateEnabled, d.NoiseGateThresholdDb, d.CompressorEnabled, d.CompressorThresholdDb, d.CompressorRatio,
                    d.LimiterEnabled, d.LimiterCeilingDb);
            }
    }

    public void Unregister(string id)
    {
        lock (_lock) if (_inputs.Remove(id, out var i)) i.Dispose(_mixer);
    }

    /// <summary>Lists currently registered audio inputs (Id, Name) for multi-track MKV recording.</summary>
    public List<(string Id, string Name)> GetActiveInputs()
    {
        lock (_lock) return _inputs.Select(kv => (kv.Key, kv.Value.Name)).ToList();
    }

    /// <summary>Reads exactly <paramref name="frames"/> interleaved stereo frames into <paramref name="buffer"/> (Vertical mix), optionally producing <paramref name="horizBuffer"/> (Horizontal mix) and per-source tapped samples.</summary>
    public void Read(float[] buffer, int frames, Dictionary<string, float[]>? perSource = null, float[]? horizBuffer = null)
    {
        int samples = frames * Channels;
        bool hasMonitor = false;
        lock (_lock)
        {
            foreach (var i in _inputs.Values) i.TrimLatency();
            int read = _mixer.Read(buffer, 0, samples);
            if (read < samples) Array.Clear(buffer, read, samples - read);

            bool splitRouting = false;
            foreach (var inp in _inputs.Values)
            {
                if (!inp.RouteVertical || !inp.RouteHorizontal) { splitRouting = true; break; }
            }
            if (splitRouting)
            {
                if (_monitorTmp.Length < samples) { _monitorMix = new float[samples]; _monitorTmp = new float[samples]; _monitorBytes = new byte[samples * 4]; }
                Array.Clear(buffer, 0, samples);
                if (horizBuffer != null) Array.Clear(horizBuffer, 0, samples);
                foreach (var inp in _inputs.Values)
                {
                    if (inp.IsMonitorOnly) continue;
                    if (!inp.RouteVertical && (horizBuffer == null || !inp.RouteHorizontal)) continue;
                    inp.CopyLastSamples(_monitorTmp, samples);
                    if (inp.RouteVertical)
                    {
                        for (int k = 0; k < samples; k++) buffer[k] += _monitorTmp[k];
                    }
                    if (horizBuffer != null && inp.RouteHorizontal)
                    {
                        for (int k = 0; k < samples; k++) horizBuffer[k] += _monitorTmp[k];
                    }
                }
            }
            else if (horizBuffer != null)
            {
                Array.Copy(buffer, 0, horizBuffer, 0, samples);
            }

            if (perSource != null)
            {
                foreach (var (id, buf) in perSource)
                {
                    if (_inputs.TryGetValue(id, out var inp)) inp.CopyLastSamples(buf, samples);
                    else Array.Clear(buf, 0, Math.Min(buf.Length, samples));
                }
            }
            foreach (var inp in _inputs.Values)
            {
                if (!inp.IsMonitored) continue;
                if (!hasMonitor)
                {
                    if (_monitorMix.Length < samples) { _monitorMix = new float[samples]; _monitorTmp = new float[samples]; _monitorBytes = new byte[samples * 4]; }
                    Array.Clear(_monitorMix, 0, samples);
                    hasMonitor = true;
                }
                inp.CopyLastSamples(_monitorTmp, samples);
                for (int k = 0; k < samples; k++) _monitorMix[k] += _monitorTmp[k];
            }
            if (hasMonitor) FeedMonitorLocked(samples);
            else if (_monitorOut != null) StopMonitorLocked();
        }
        for (int i = 0; i < samples; i++)
        {
            float v = buffer[i];
            if (v > 1f) buffer[i] = 1f; else if (v < -1f) buffer[i] = -1f;
        }
        if (horizBuffer != null)
        {
            for (int i = 0; i < samples; i++)
            {
                float v = horizBuffer[i];
                if (v > 1f) horizBuffer[i] = 1f; else if (v < -1f) horizBuffer[i] = -1f;
            }
        }
    }

    private void FeedMonitorLocked(int samples)
    {
        try
        {
            if (_monitorOut == null)
            {
                _monitorBuf = new BufferedWaveProvider(MixFormat) { BufferDuration = TimeSpan.FromMilliseconds(200), DiscardOnBufferOverflow = true, ReadFully = true };
                _monitorOut = new WasapiOut(AudioClientShareMode.Shared, 15);
                _monitorOut.Init(_monitorBuf);
                _monitorOut.Play();
            }
            for (int i = 0; i < samples; i++)
            {
                float v = _monitorMix[i];
                if (v > 1f) _monitorMix[i] = 1f; else if (v < -1f) _monitorMix[i] = -1f;
            }
            int maxMonBytes = MixFormat.AverageBytesPerSecond / 40; // keep monitoring latency <= 25 ms
            if (_monitorBuf!.BufferedBytes > maxMonBytes)
            {
                int excess = _monitorBuf.BufferedBytes - maxMonBytes;
                excess -= excess % MixFormat.BlockAlign;
                if (excess > 0)
                {
                    if (_monitorBytes.Length < excess) _monitorBytes = new byte[excess];
                    _monitorBuf.Read(_monitorBytes, 0, excess);
                }
            }
            int byteCount = samples * 4;
            if (_monitorBytes.Length < byteCount) _monitorBytes = new byte[byteCount];
            Buffer.BlockCopy(_monitorMix, 0, _monitorBytes, 0, byteCount);
            _monitorBuf.AddSamples(_monitorBytes, 0, byteCount);
        }
        catch (Exception ex)
        {
            Log.Warn("Audio monitoring: " + ex.Message);
            StopMonitorLocked();
        }
    }

    private void StopMonitorLocked()
    {
        try { _monitorOut?.Stop(); } catch { }
        try { _monitorOut?.Dispose(); } catch { }
        _monitorOut = null;
        _monitorBuf = null;
    }

    /// <summary>
    /// Plays an alert sound file (.mp3/.wav). When routed through the AlertBox's registered mixer channel,
    /// its volume, mute, level meter, filters, and headphone monitoring are all controlled directly from the Audio Mixer.
    /// </summary>
    public void PlayAlertSound(string sourceId, string path, float volume = 1f)
    {
        if (string.IsNullOrWhiteSpace(path) || !File.Exists(path)) return;
        float vol = Math.Clamp(volume, 0f, 2f);
        Task.Run(() =>
        {
            try
            {
                float[] samples;
                using (var reader = new AudioFileReader(path))
                {
                    ISampleProvider sp = reader;
                    if (sp.WaveFormat.Channels == 1) sp = new MonoToStereoSampleProvider(sp);
                    else if (sp.WaveFormat.Channels > 2) sp = new MultiplexingSampleProvider(new[] { sp }, 2);
                    if (sp.WaveFormat.SampleRate != SampleRate) sp = new WdlResamplingSampleProvider(sp, SampleRate);
                    if (Math.Abs(vol - 1f) > 0.001f) sp = new VolumeSampleProvider(sp) { Volume = vol };

                    var pcm = new List<float>(SampleRate * Channels * 4);
                    var tmp = new float[4096];
                    int n;
                    int maxSamples = SampleRate * Channels * 15;
                    while ((n = sp.Read(tmp, 0, tmp.Length)) > 0 && pcm.Count < maxSamples)
                    {
                        for (int i = 0; i < n && pcm.Count < maxSamples; i++) pcm.Add(tmp[i]);
                    }
                    samples = pcm.ToArray();
                }
                if (samples.Length == 0) return;

                Input? target = null;
                lock (_lock)
                {
                    if (!string.IsNullOrEmpty(sourceId)) _inputs.TryGetValue(sourceId, out target);
                }
                if (target != null)
                {
                    target.Push(samples, samples.Length);
                    return;
                }

                // Fallback when no dedicated AlertBox channel is registered
                lock (_lock) _mixer.AddMixerInput(new OneShotSampleProvider(samples, MixFormat));
                using var localReader = new AudioFileReader(path) { Volume = Math.Clamp(vol, 0f, 1f) };
                using var wo = new WasapiOut(AudioClientShareMode.Shared, 60);
                using var done = new ManualResetEventSlim(false);
                wo.PlaybackStopped += (_, _) => done.Set();
                wo.Init(localReader);
                wo.Play();
                done.Wait(TimeSpan.FromSeconds(15));
            }
            catch (Exception ex) { Log.Warn("Alert sound: " + ex.Message); }
        });
    }

    private sealed class OneShotSampleProvider(float[] samples, WaveFormat format) : ISampleProvider
    {
        private int _pos;
        public WaveFormat WaveFormat => format;
        public int Read(float[] buffer, int offset, int count)
        {
            int avail = samples.Length - _pos;
            if (avail <= 0) return 0;
            int n = Math.Min(count, avail);
            Array.Copy(samples, _pos, buffer, offset, n);
            _pos += n;
            return n;
        }
    }

    public void Dispose()
    {
        lock (_lock)
        {
            StopMonitorLocked();
            foreach (var i in _inputs.Values) i.Dispose(_mixer);
            _inputs.Clear();
        }
    }

    public static (List<(string Id, string Name)> Inputs, List<(string Id, string Name)> Outputs) EnumerateDevices()
    {
        var ins = new List<(string, string)>();
        var outs = new List<(string, string)>();
        try
        {
            using var en = new MMDeviceEnumerator();
            foreach (var d in en.EnumerateAudioEndPoints(DataFlow.Capture, DeviceState.Active)) ins.Add((d.ID, d.FriendlyName));
            foreach (var d in en.EnumerateAudioEndPoints(DataFlow.Render, DeviceState.Active)) outs.Add((d.ID, d.FriendlyName));
        }
        catch (Exception ex) { Log.Warn("Audio device list: " + ex.Message); }
        return (ins, outs);
    }

    private sealed class Input
    {
        private readonly IWaveIn? _capture;
        private readonly BufferedWaveProvider _buffer;
        private readonly NoiseSuppressorProvider _noiseSuppressor;
        private readonly DynamicsProvider _dynamics;
        private readonly VolumeSampleProvider _volume;
        private readonly LimiterProvider _limiter;
        private readonly DelayProvider _delay;
        private readonly TapSampleProvider _tap;
        private readonly ISampleProvider _chainOut;
        private readonly bool _trim;
        private readonly int _maxBufferMs;
        private volatile int _lastPacketBytes;
        private byte[] _trimBuf = new byte[4096];
        public string Name { get; }
        public string Key { get; private init; } = "";
        public bool IsDevice => _capture != null;
        public bool IsAlert { get; private init; }
        public bool IsMonitored { get; private set; }
        public bool IsMonitorOnly => _tap.SuppressOutput;
        public bool RouteVertical { get; private set; } = true;
        public bool RouteHorizontal { get; private set; } = true;
        public volatile bool Stopped;
        public float Peak;

        public void SetRouting(bool vertical, bool horizontal)
        {
            RouteVertical = vertical;
            RouteHorizontal = horizontal;
        }

        public void SetMonitorMode(string? mode)
        {
            IsMonitored = mode is "MonitorOnly" or "MonitorAndOutput";
            _tap.SuppressOutput = mode == "MonitorOnly";
        }

        private Input(string name, IWaveIn? capture, WaveFormat fmt, bool trim, MixingSampleProvider mixer, int maxBufferMs = 35)
        {
            Name = name;
            _capture = capture;
            _trim = trim;
            _maxBufferMs = Math.Clamp(maxBufferMs, 12, 100);
            _buffer = new BufferedWaveProvider(fmt)
            {
                BufferDuration = TimeSpan.FromSeconds(trim ? 1 : 16),
                DiscardOnBufferOverflow = true,
                ReadFully = true,
            };
            bool monoInput = fmt.Channels == 1;
            ISampleProvider sp = _buffer.ToSampleProvider();
            if (sp.WaveFormat.Channels == 1) sp = new MonoToStereoSampleProvider(sp);
            else if (sp.WaveFormat.Channels > 2) sp = new MultiplexingSampleProvider(new[] { sp }, 2);
            if (sp.WaveFormat.SampleRate != SampleRate) sp = new WdlResamplingSampleProvider(sp, SampleRate);
            _noiseSuppressor = new NoiseSuppressorProvider(sp, monoInput);
            _dynamics = new DynamicsProvider(_noiseSuppressor);
            _volume = new VolumeSampleProvider(_dynamics);
            _limiter = new LimiterProvider(_volume);
            _delay = new DelayProvider(_limiter);
            var meter = new MeteringSampleProvider(_delay, SampleRate / 60);
            meter.StreamVolume += (_, e) => Peak = e.MaxSampleValues.Length > 0 ? e.MaxSampleValues.Max() : 0;
            _tap = new TapSampleProvider(meter);
            _chainOut = _tap;
            mixer.AddMixerInput(_chainOut);
            if (capture != null)
            {
                capture.DataAvailable += (_, e) =>
                {
                    if (e.BytesRecorded > 0) _lastPacketBytes = e.BytesRecorded;
                    _buffer.AddSamples(e.Buffer, 0, e.BytesRecorded);
                };
                capture.RecordingStopped += (_, e) => {
                    Stopped = true;
                    if (e.Exception != null) Log.Error($"Audio '{name}' stopped", e.Exception);
                };
                try { capture.StartRecording(); }
                catch
                {
                    mixer.RemoveMixerInput(_chainOut);
                    capture.Dispose();
                    throw;
                }
                Log.Info($"Audio '{name}': {capture.WaveFormat}");
            }
        }

        public static Input FromDevice(string name, string key, IWaveIn cap, MixingSampleProvider mixer, int maxBufferMs = 35)
            => new(name, cap, cap.WaveFormat, true, mixer, maxBufferMs) { Key = key };

        public static Input FromPush(string name, MixingSampleProvider mixer, bool isAlert = false)
            => new(name, null, MixFormat, false, mixer) { Key = "push", IsAlert = isAlert };

        public void SetVolume(float v)
        {
            _volume.Volume = v;
            _noiseSuppressor.SetMuted(v <= 0f);
        }

        public void SetFilters(int syncOffsetMs, bool nsOn, string? nsMethod, int nsLevelDb, bool gateOn, float gateDb, bool compOn, float compDb, float compRatio, bool limOn, float limDb)
        {
            _delay.Configure(syncOffsetMs);
            _noiseSuppressor.Configure(nsOn, nsMethod, nsLevelDb);
            _dynamics.Configure(gateOn, gateDb, compOn, compDb, compRatio);
            _limiter.Configure(limOn, limDb);
        }

        public void Push(float[] samples, int count)
        {
            var bytes = new byte[count * 4];
            Buffer.BlockCopy(samples, 0, bytes, 0, bytes.Length);
            _buffer.AddSamples(bytes, 0, bytes.Length);
        }

        /// <summary>Device clock drifts relative to ours — adapt cap to device packet size (wired / 2.4 GHz / Bluetooth) so latency stays minimal without choppy audio.</summary>
        public void TrimLatency()
        {
            if (!_trim) return;
            int minBytes = _buffer.WaveFormat.AverageBytesPerSecond * _maxBufferMs / 1000;
            int pktCap = Math.Min(_buffer.WaveFormat.AverageBytesPerSecond / 12, _lastPacketBytes * 2); // cap <= 83 ms even on bursty Bluetooth HFP
            int max = Math.Max(minBytes, pktCap);
            int excess = _buffer.BufferedBytes - max;
            if (excess <= 0) return;
            excess -= excess % _buffer.WaveFormat.BlockAlign;
            if (excess <= 0) return;
            if (_trimBuf.Length < excess) _trimBuf = new byte[excess];
            _buffer.Read(_trimBuf, 0, excess);
        }

        public void CopyLastSamples(float[] dst, int count) => _tap.CopyLast(dst, count);

        public void Dispose(MixingSampleProvider mixer)
        {
            mixer.RemoveMixerInput(_chainOut);
            _noiseSuppressor.Dispose();
            if (_capture == null) return;
            try { _capture.StopRecording(); } catch { }
            _capture.Dispose();
        }
    }

    /// <summary>Noise gate (4 dB hysteresis) + compressor (RMS/peak envelope) on 48 kHz stereo float, zero allocations.</summary>
    private sealed class DynamicsProvider(ISampleProvider source) : ISampleProvider
    {
        private volatile bool _gateOn, _compOn;
        private float _gateOpenLin = 0.01f, _gateCloseLin = 0.0063f;
        private float _compThreshDb = -18f, _compSlope = 0.75f;
        private float _gateGain = 1f, _compEnv;
        private int _holdLeft;
        private const int HoldSamples = SampleRate * 80 / 1000;
        private const float GateAttack = 1f / (SampleRate * 0.005f);
        private const float GateRelease = 1f / (SampleRate * 0.150f);
        private static readonly float CompAttackCoeff = MathF.Exp(-1f / (SampleRate * 0.010f));
        private static readonly float CompReleaseCoeff = MathF.Exp(-1f / (SampleRate * 0.100f));

        public WaveFormat WaveFormat => source.WaveFormat;

        public void Configure(bool gateOn, float gateDb, bool compOn, float compDb, float compRatio)
        {
            _gateOn = gateOn;
            _gateOpenLin = MathF.Pow(10f, Math.Clamp(gateDb, -100f, 0f) / 20f);
            _gateCloseLin = MathF.Pow(10f, Math.Clamp(gateDb - 4f, -104f, -4f) / 20f);
            _compOn = compOn;
            _compThreshDb = Math.Clamp(compDb, -100f, 0f);
            float ratio = Math.Clamp(compRatio, 1f, 20f);
            _compSlope = 1f - 1f / ratio;
        }

        public int Read(float[] buffer, int offset, int count)
        {
            int read = source.Read(buffer, offset, count);
            bool gateOn = _gateOn, compOn = _compOn;
            if (!gateOn && !compOn) return read;
            for (int i = 0; i + 1 < read; i += 2)
            {
                float l = buffer[offset + i], r = buffer[offset + i + 1];
                float level = MathF.Max(MathF.Abs(l), MathF.Abs(r));
                float gain = 1f;

                if (gateOn)
                {
                    if (level >= _gateOpenLin) { _holdLeft = HoldSamples; _gateGain = MathF.Min(1f, _gateGain + GateAttack); }
                    else if (level >= _gateCloseLin && _holdLeft > 0) { _holdLeft--; _gateGain = MathF.Min(1f, _gateGain + GateAttack); }
                    else if (_holdLeft > 0) _holdLeft--;
                    else _gateGain = MathF.Max(0f, _gateGain - GateRelease);
                    gain *= _gateGain;
                }

                if (compOn)
                {
                    float coeff = level > _compEnv ? CompAttackCoeff : CompReleaseCoeff;
                    _compEnv = coeff * _compEnv + (1f - coeff) * level;
                    if (_compEnv > 1e-6f)
                    {
                        float envDb = 20f * MathF.Log10(_compEnv);
                        if (envDb > _compThreshDb)
                        {
                            float reductionDb = (envDb - _compThreshDb) * _compSlope;
                            gain *= MathF.Pow(10f, -reductionDb / 20f);
                        }
                    }
                }

                buffer[offset + i] = l * gain;
                buffer[offset + i + 1] = r * gain;
            }
            return read;
        }
    }

    /// <summary>Fast brickwall limiter after volume fader — prevents clipping.</summary>
    private sealed class LimiterProvider(ISampleProvider source) : ISampleProvider
    {
        private volatile bool _enabled;
        private float _ceiling = 0.89125f;
        private float _gain = 1f;
        private static readonly float ReleaseCoeff = MathF.Exp(-1f / (SampleRate * 0.050f));

        public WaveFormat WaveFormat => source.WaveFormat;

        public void Configure(bool enabled, float ceilingDb)
        {
            _enabled = enabled;
            _ceiling = MathF.Pow(10f, Math.Clamp(ceilingDb, -100f, 0f) / 20f);
        }

        public int Read(float[] buffer, int offset, int count)
        {
            int read = source.Read(buffer, offset, count);
            if (!_enabled) return read;
            float ceiling = _ceiling;
            for (int i = 0; i + 1 < read; i += 2)
            {
                float l = buffer[offset + i], r = buffer[offset + i + 1];
                float peak = MathF.Max(MathF.Abs(l), MathF.Abs(r));
                float target = peak > ceiling ? ceiling / peak : 1f;
                _gain = target < _gain ? target : ReleaseCoeff * _gain + (1f - ReleaseCoeff) * target;
                buffer[offset + i] = l * _gain;
                buffer[offset + i + 1] = r * _gain;
            }
            return read;
        }
    }

    /// <summary>Fixed A/V sync delay (0..500 ms) using a ring buffer, zero allocations in Read.</summary>
    private sealed class DelayProvider(ISampleProvider source) : ISampleProvider
    {
        private const int MaxMs = 500;
        private readonly float[] _ring = new float[(SampleRate * MaxMs / 1000 + 16) * Channels];
        private int _writePos;
        private volatile int _delaySamples;

        public WaveFormat WaveFormat => source.WaveFormat;

        public void Configure(int delayMs)
        {
            int ms = Math.Clamp(delayMs, 0, MaxMs);
            _delaySamples = (ms * SampleRate / 1000) * Channels;
        }

        public int Read(float[] buffer, int offset, int count)
        {
            int read = source.Read(buffer, offset, count);
            int delay = _delaySamples;
            if (delay <= 0 || read <= 0) return read;
            int len = _ring.Length;
            for (int i = 0; i < read; i++)
            {
                _ring[_writePos] = buffer[offset + i];
                int rPos = _writePos - delay;
                if (rPos < 0) rPos += len;
                buffer[offset + i] = _ring[rPos];
                if (++_writePos >= len) _writePos = 0;
            }
            return read;
        }
    }

    /// <summary>Captures the most recently mixed block per input for multi-track MKV recording and audio monitoring, zero allocations.</summary>
    private sealed class TapSampleProvider(ISampleProvider source) : ISampleProvider
    {
        private float[] _last = new float[2048];
        private int _lastCount;
        public volatile bool SuppressOutput;
        public WaveFormat WaveFormat => source.WaveFormat;

        public int Read(float[] buffer, int offset, int count)
        {
            int read = source.Read(buffer, offset, count);
            if (_last.Length < count) _last = new float[count];
            if (read > 0) Array.Copy(buffer, offset, _last, 0, read);
            if (read < count) Array.Clear(_last, read, count - read);
            _lastCount = count;
            if (SuppressOutput && read > 0) Array.Clear(buffer, offset, read);
            return read;
        }

        public void CopyLast(float[] dst, int count)
        {
            int n = Math.Min(count, Math.Min(dst.Length, _lastCount));
            if (n > 0) Array.Copy(_last, 0, dst, 0, n);
            if (n < count && n < dst.Length) Array.Clear(dst, n, Math.Min(count, dst.Length) - n);
            for (int i = 0; i < Math.Min(count, dst.Length); i++)
            {
                float v = dst[i];
                if (v > 1f) dst[i] = 1f; else if (v < -1f) dst[i] = -1f;
            }
        }
    }
}
