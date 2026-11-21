using System.Runtime.InteropServices;
using NAudio.Wave;
using TikStream.Core;

namespace TikStream.Capture;

/// <summary>
/// Tłumienie hałasu 48 kHz stereo float (ramki po 480 próbek = 10 ms):
/// • RNNoise 0.2 (sieć neuronowa Xiph.org — wysoka jakość, jak w OBS Studio)
/// • SpeexDSP 1.2.1 (niskie użycie CPU, regulowany poziom tłumienia w dB)
/// Zero alokacji w metodzie Read; przy wyłączeniu czysty passthrough.
/// </summary>
internal sealed unsafe class NoiseSuppressorProvider : ISampleProvider, IDisposable
{
    private const int FrameSize = 480; // 10 ms @ 48 kHz
    private const int SpeexSetDenoise = 0;
    private const int SpeexSetNoiseSuppress = 18;

    private readonly ISampleProvider _source;
    private readonly bool _monoInput;
    private readonly object _lock = new();
    private volatile bool _enabled;
    private volatile bool _muted;
    private string _method = "rnnoise";
    private int _speexLevelDb = -30;

    private IntPtr _rnnL, _rnnR;
    private IntPtr _spxL, _spxR;
    private bool _rnnoiseUnavailable, _speexUnavailable;

    // Bufory reszty (FIFO) i ramki 480 próbek (zero alokacji per Read)
    private readonly float[] _inL = new float[FrameSize];
    private readonly float[] _inR = new float[FrameSize];
    private readonly float[] _outL = new float[FrameSize];
    private readonly float[] _outR = new float[FrameSize];
    private readonly short[] _spxBuf = new short[FrameSize];
    private readonly float[] _pending = new float[FrameSize * 8];
    private int _inFill;
    private int _pendRead, _pendWrite;

    public WaveFormat WaveFormat => _source.WaveFormat;

    static NoiseSuppressorProvider()
    {
        NativeLibrary.SetDllImportResolver(typeof(NoiseSuppressorProvider).Assembly, ResolveNative);
    }

    private static IntPtr ResolveNative(string libraryName, System.Reflection.Assembly assembly, DllImportSearchPath? searchPath)
    {
        if (libraryName is not ("librnnoise-0" or "libspeexdsp-1")) return IntPtr.Zero;
        string file = libraryName + ".dll";
        string? asmDir = Path.GetDirectoryName(assembly.Location);
        string[] candidates =
        {
            asmDir != null ? Path.Combine(asmDir, "native", file) : "",
            asmDir != null ? Path.Combine(asmDir, file) : "",
            Path.Combine(AppContext.BaseDirectory, "engine", "native", file),
            Path.Combine(AppContext.BaseDirectory, "native", file),
            Path.Combine(AppContext.BaseDirectory, file),
        };
        foreach (var path in candidates)
            if (!string.IsNullOrEmpty(path) && File.Exists(path) && NativeLibrary.TryLoad(path, out var handle)) return handle;
        return NativeLibrary.TryLoad(file, assembly, searchPath, out var fallback) ? fallback : IntPtr.Zero;
    }

    public NoiseSuppressorProvider(ISampleProvider source, bool monoInput)
    {
        _source = source;
        _monoInput = monoInput;
    }

    public void SetMuted(bool muted) => _muted = muted;

    public void Configure(bool enabled, string? method, int speexLevelDb)
    {
        string norm = string.Equals(method, "speex", StringComparison.OrdinalIgnoreCase) ? "speex" : "rnnoise";
        int level = Math.Clamp(speexLevelDb, -60, 0);
        lock (_lock)
        {
            bool changed = _enabled != enabled || _method != norm || _speexLevelDb != level;
            _enabled = enabled;
            _method = norm;
            _speexLevelDb = level;
            if (!changed) return;

            _inFill = 0;
            _pendRead = _pendWrite = 0;
            if (!enabled) return;

            if (norm == "rnnoise") EnsureRnnoise();
            else EnsureSpeex(level);
        }
    }

    private void EnsureRnnoise()
    {
        if (_rnnL != IntPtr.Zero || _rnnoiseUnavailable) return;
        try
        {
            _rnnL = rnnoise_create(IntPtr.Zero);
            if (!_monoInput) _rnnR = rnnoise_create(IntPtr.Zero);
            if (_rnnL == IntPtr.Zero || (!_monoInput && _rnnR == IntPtr.Zero))
            {
                DestroyRnnoise();
                _rnnoiseUnavailable = true;
                Log.Warn("RNNoise: nie udało się utworzyć stanu dekodera.");
            }
        }
        catch (Exception ex)
        {
            _rnnoiseUnavailable = true;
            Log.Warn("RNNoise niedostępny: " + ex.Message);
        }
    }

    private void EnsureSpeex(int levelDb)
    {
        if (_speexUnavailable) return;
        try
        {
            if (_spxL == IntPtr.Zero)
            {
                _spxL = speex_preprocess_state_init(FrameSize, AudioEngine.SampleRate);
                int on = 1;
                speex_preprocess_ctl(_spxL, SpeexSetDenoise, ref on);
                if (!_monoInput)
                {
                    _spxR = speex_preprocess_state_init(FrameSize, AudioEngine.SampleRate);
                    on = 1;
                    speex_preprocess_ctl(_spxR, SpeexSetDenoise, ref on);
                }
            }
            int lvl = levelDb;
            speex_preprocess_ctl(_spxL, SpeexSetNoiseSuppress, ref lvl);
            if (!_monoInput && _spxR != IntPtr.Zero)
            {
                lvl = levelDb;
                speex_preprocess_ctl(_spxR, SpeexSetNoiseSuppress, ref lvl);
            }
        }
        catch (Exception ex)
        {
            _speexUnavailable = true;
            Log.Warn("SpeexDSP niedostępny: " + ex.Message);
        }
    }

    public int Read(float[] buffer, int offset, int count)
    {
        int read = _source.Read(buffer, offset, count);
        if (!_enabled || _muted || read <= 0) return read;

        lock (_lock)
        {
            if (!_enabled) return read;
            bool useRnn = _method == "rnnoise";
            if (useRnn && _rnnL == IntPtr.Zero) return read;
            if (!useRnn && _spxL == IntPtr.Zero) return read;

            // Przepuść wszystkie dostarczone próbki przez bloki 480 próbek (10 ms)
            int framesIn = read / 2;
            for (int i = 0; i < framesIn; i++)
            {
                _inL[_inFill] = buffer[offset + i * 2];
                _inR[_inFill] = buffer[offset + i * 2 + 1];
                _inFill++;
                if (_inFill == FrameSize)
                {
                    if (useRnn) ProcessRnnoiseFrame();
                    else ProcessSpeexFrame();
                    _inFill = 0;
                    EnqueuePending();
                }
            }

            // Zwróć dokładnie tyle próbek, ile przeczytano (początkowe 10 ms to cisza rozruchowa)
            int outIdx = 0;
            while (outIdx < read && _pendRead < _pendWrite)
                buffer[offset + outIdx++] = _pending[_pendRead++];
            if (_pendRead == _pendWrite) _pendRead = _pendWrite = 0;
            while (outIdx < read) buffer[offset + outIdx++] = 0f;
            return read;
        }
    }

    private void ProcessRnnoiseFrame()
    {
        bool silent = true;
        if (_monoInput)
        {
            for (int i = 0; i < FrameSize; i++)
            {
                float v = _inL[i];
                if (v != 0f) silent = false;
                _inL[i] = v * 32768f;
            }
            if (silent)
            {
                Array.Clear(_outL);
                Array.Clear(_outR);
                return;
            }
            fixed (float* inL = _inL, outL = _outL)
                rnnoise_process_frame(_rnnL, outL, inL);
            for (int i = 0; i < FrameSize; i++)
                _outL[i] = _outR[i] = _outL[i] * (1f / 32768f);
            return;
        }

        for (int i = 0; i < FrameSize; i++)
        {
            float l = _inL[i], r = _inR[i];
            if (l != 0f || r != 0f) silent = false;
            _inL[i] = l * 32768f;
            _inR[i] = r * 32768f;
        }
        if (silent)
        {
            Array.Clear(_outL);
            Array.Clear(_outR);
            return;
        }
        fixed (float* inL = _inL, outL = _outL, inR = _inR, outR = _outR)
        {
            rnnoise_process_frame(_rnnL, outL, inL);
            rnnoise_process_frame(_rnnR, outR, inR);
        }
        for (int i = 0; i < FrameSize; i++)
        {
            _outL[i] *= (1f / 32768f);
            _outR[i] *= (1f / 32768f);
        }
    }

    private void ProcessSpeexFrame()
    {
        RunSpeexChannel(_spxL, _inL, _outL);
        if (_monoInput) Array.Copy(_outL, _outR, FrameSize);
        else RunSpeexChannel(_spxR, _inR, _outR);
    }

    private void RunSpeexChannel(IntPtr state, float[] input, float[] output)
    {
        for (int i = 0; i < FrameSize; i++)
            _spxBuf[i] = (short)Math.Clamp((int)(input[i] * 32767f), -32768, 32767);
        fixed (short* p = _spxBuf) speex_preprocess_run(state, p);
        for (int i = 0; i < FrameSize; i++)
            output[i] = _spxBuf[i] * (1f / 32768f);
    }

    private void EnqueuePending()
    {
        int needed = FrameSize * 2;
        if (_pendWrite + needed > _pending.Length)
        {
            int rem = _pendWrite - _pendRead;
            if (rem > 0 && _pendRead > 0) Array.Copy(_pending, _pendRead, _pending, 0, rem);
            _pendRead = 0;
            _pendWrite = rem;
            if (_pendWrite + needed > _pending.Length)
            {
                // Nadmiar — odrzuć najstarsze próbki, żeby nie przekroczyć bufora
                _pendRead = _pendWrite = 0;
            }
        }
        for (int i = 0; i < FrameSize; i++)
        {
            _pending[_pendWrite++] = _outL[i];
            _pending[_pendWrite++] = _outR[i];
        }
    }

    private void DestroyRnnoise()
    {
        if (_rnnL != IntPtr.Zero) { rnnoise_destroy(_rnnL); _rnnL = IntPtr.Zero; }
        if (_rnnR != IntPtr.Zero) { rnnoise_destroy(_rnnR); _rnnR = IntPtr.Zero; }
    }

    private void DestroySpeex()
    {
        if (_spxL != IntPtr.Zero) { speex_preprocess_state_destroy(_spxL); _spxL = IntPtr.Zero; }
        if (_spxR != IntPtr.Zero) { speex_preprocess_state_destroy(_spxR); _spxR = IntPtr.Zero; }
    }

    public void Dispose()
    {
        lock (_lock)
        {
            _enabled = false;
            DestroyRnnoise();
            DestroySpeex();
        }
    }

    [DllImport("librnnoise-0", CallingConvention = CallingConvention.Cdecl)]
    private static extern IntPtr rnnoise_create(IntPtr model);

    [DllImport("librnnoise-0", CallingConvention = CallingConvention.Cdecl)]
    private static extern void rnnoise_destroy(IntPtr st);

    [DllImport("librnnoise-0", CallingConvention = CallingConvention.Cdecl)]
    private static extern float rnnoise_process_frame(IntPtr st, float* outBuf, float* inBuf);

    [DllImport("libspeexdsp-1", CallingConvention = CallingConvention.Cdecl)]
    private static extern IntPtr speex_preprocess_state_init(int frameSize, int samplingRate);

    [DllImport("libspeexdsp-1", CallingConvention = CallingConvention.Cdecl)]
    private static extern void speex_preprocess_state_destroy(IntPtr st);

    [DllImport("libspeexdsp-1", CallingConvention = CallingConvention.Cdecl)]
    private static extern int speex_preprocess_ctl(IntPtr st, int request, ref int ptr);

    [DllImport("libspeexdsp-1", CallingConvention = CallingConvention.Cdecl)]
    private static extern int speex_preprocess_run(IntPtr st, short* x);
}
