using System.Net.Http;
using System.Text;
using NAudio.Wave;
using NAudio.Wave.SampleProviders;
using TikStream.Core;
using Windows.Media.SpeechSynthesis;

namespace TikStream.Capture;

public sealed record TtsVoiceInfo(string Id, string DisplayName, string Language);

/// <summary>
/// Built-in TTS service with background FIFO queue supporting:
/// - "google": Classic Polish/international donation voice via Google Translate TTS (default)
/// - "streamelements": Ivona / Amazon Polly voices (Maja, Jacek, Jan, Ewa, Brian, Amy, Joey)
/// - "windows": Offline Windows WinRT SpeechSynthesizer
/// Automatically falls back (streamelements -> google -> windows) on HTTP error/429/timeout/offline.
/// All synthesis and MP3/WAV decoding runs in-memory on a dedicated background thread.
/// </summary>
public sealed class TtsService : IDisposable
{
    public const int MaxQueueSize = 20;

    private sealed record TtsItem(string Text, string? ProviderOverride, string? VoiceOverride, float? RateOverride);

    private static readonly HttpClient Http = CreateHttpClient();

    private static readonly TtsVoiceInfo[] GoogleVoices =
    {
        new("pl", "Polski (Google — Donejtowy)", "pl-PL"),
        new("en", "English (Google)", "en-US"),
        new("de", "Deutsch (Google)", "de-DE"),
        new("uk", "Українська (Google)", "uk-UA"),
        new("es", "Español (Google)", "es-ES"),
        new("fr", "Français (Google)", "fr-FR"),
    };

    private static readonly TtsVoiceInfo[] StreamElementsVoices =
    {
        new("Maja", "Maja — Ivona/Polly (Polski)", "pl-PL"),
        new("Jacek", "Jacek — Ivona/Polly (Polski)", "pl-PL"),
        new("Jan", "Jan — Ivona/Polly (Polski)", "pl-PL"),
        new("Ewa", "Ewa — Ivona/Polly (Polski)", "pl-PL"),
        new("Brian", "Brian — Ivona/Polly (English UK)", "en-GB"),
        new("Amy", "Amy — Ivona/Polly (English UK)", "en-GB"),
        new("Joey", "Joey — Ivona/Polly (English US)", "en-US"),
    };

    private readonly AudioEngine _audio;
    private readonly object _queueLock = new();
    private readonly LinkedList<TtsItem> _queue = new();
    private readonly SemaphoreSlim _signal = new(0);
    private readonly CancellationTokenSource _disposeCts = new();
    private CancellationTokenSource? _utteranceCts;
    private readonly Thread _worker;

    private volatile string _provider = "google";
    private volatile string _voiceId = "";
    private volatile float _rate = 1.0f;

    public TtsService(AudioEngine audio)
    {
        _audio = audio;
        _worker = new Thread(WorkerLoop) { IsBackground = true, Name = "TtsWorker" };
        _worker.Start();
    }

    private static HttpClient CreateHttpClient()
    {
        var client = new HttpClient { Timeout = TimeSpan.FromSeconds(5) };
        client.DefaultRequestHeaders.TryAddWithoutValidation("User-Agent",
            "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/126.0.0.0 Safari/537.36");
        return client;
    }

    public static string NormalizeProvider(string? provider)
    {
        var p = (provider ?? "").Trim().ToLowerInvariant();
        return p is "google" or "streamelements" or "windows" ? p : "google";
    }

    public void ApplySettings(AppSettings s)
    {
        _provider = NormalizeProvider(s.TtsProvider);
        _voiceId = s.TtsVoice ?? "";
        _rate = Math.Clamp(s.TtsRate <= 0f ? 1.0f : s.TtsRate, 0.5f, 2.0f);
    }

    public static List<TtsVoiceInfo> ListVoices(string? provider = null)
    {
        string p = string.IsNullOrWhiteSpace(provider) ? "google" : NormalizeProvider(provider);
        if (p == "google") return new List<TtsVoiceInfo>(GoogleVoices);
        if (p == "streamelements") return new List<TtsVoiceInfo>(StreamElementsVoices);

        var list = new List<TtsVoiceInfo>();
        try
        {
            foreach (var v in SpeechSynthesizer.AllVoices)
                list.Add(new TtsVoiceInfo(v.Id, v.DisplayName, v.Language));
        }
        catch (Exception ex) { Log.Warn("TTS ListVoices: " + ex.Message); }
        return list;
    }

    public void Enqueue(string text, bool priority = false, string? providerOverride = null, string? voiceOverride = null, float? rateOverride = null)
    {
        if (string.IsNullOrWhiteSpace(text)) return;
        var item = new TtsItem(text.Trim(), providerOverride, voiceOverride, rateOverride);
        lock (_queueLock)
        {
            if (priority)
            {
                _queue.AddFirst(item);
                while (_queue.Count > MaxQueueSize) _queue.RemoveLast();
            }
            else
            {
                if (_queue.Count >= MaxQueueSize) _queue.RemoveFirst();
                _queue.AddLast(item);
            }
        }
        try { _signal.Release(); } catch { }
    }

    public void SkipCurrent()
    {
        try { _utteranceCts?.Cancel(); } catch { }
        _audio.ClearTtsBuffer();
    }

    public void ClearQueue()
    {
        lock (_queueLock) _queue.Clear();
        SkipCurrent();
    }

    private void WorkerLoop()
    {
        SpeechSynthesizer? synth = null;
        string appliedWinVoice = "";
        try
        {
            while (!_disposeCts.IsCancellationRequested)
            {
                try { _signal.Wait(_disposeCts.Token); }
                catch (OperationCanceledException) { break; }

                TtsItem? item = null;
                lock (_queueLock)
                {
                    if (_queue.First != null)
                    {
                        item = _queue.First.Value;
                        _queue.RemoveFirst();
                    }
                }
                if (item == null || string.IsNullOrWhiteSpace(item.Text)) continue;

                using var uttCts = CancellationTokenSource.CreateLinkedTokenSource(_disposeCts.Token);
                _utteranceCts = uttCts;
                try
                {
                    string provider = NormalizeProvider(item.ProviderOverride ?? _provider);
                    string voice = item.VoiceOverride ?? _voiceId;
                    float rate = Math.Clamp(item.RateOverride ?? _rate, 0.5f, 2.0f);

                    float[]? samples = SynthesizeWithFallback(item.Text, provider, voice, rate, ref synth, ref appliedWinVoice, uttCts.Token);
                    if (samples == null || samples.Length == 0 || uttCts.IsCancellationRequested) continue;

                    _audio.ClearTtsBuffer();
                    _audio.PushTtsSamples(samples, samples.Length);

                    int durationMs = (int)((long)samples.Length * 1000L / (AudioEngine.SampleRate * AudioEngine.Channels));
                    if (durationMs > 0)
                    {
                        try { Task.Delay(durationMs + 120, uttCts.Token).Wait(uttCts.Token); }
                        catch (OperationCanceledException) { _audio.ClearTtsBuffer(); }
                    }
                }
                catch (OperationCanceledException)
                {
                    _audio.ClearTtsBuffer();
                }
                catch (Exception ex)
                {
                    Log.Warn("TTS synthesize: " + ex.Message);
                }
                finally
                {
                    if (ReferenceEquals(_utteranceCts, uttCts)) _utteranceCts = null;
                }
            }
        }
        finally
        {
            try { synth?.Dispose(); } catch { }
        }
    }

    private static float[]? SynthesizeWithFallback(
        string text,
        string provider,
        string voice,
        float rate,
        ref SpeechSynthesizer? synth,
        ref string appliedWinVoice,
        CancellationToken ct)
    {
        if (provider == "streamelements")
        {
            string seVoice = ResolveStreamElementsVoice(voice);
            var pcm = TrySynthesizeStreamElements(text, seVoice, ct);
            if (pcm != null && pcm.Length > 0) return ApplyTimeStretch(pcm, rate);
            if (ct.IsCancellationRequested) return null;

            string fallbackLang = seVoice is "Brian" or "Amy" or "Joey" ? "en" : "pl";
            Log.Warn($"TTS: StreamElements ({seVoice}) unavailable — falling back to Google TTS ({fallbackLang}).");
            pcm = TrySynthesizeGoogle(text, fallbackLang, ct);
            if (pcm != null && pcm.Length > 0) return ApplyTimeStretch(pcm, rate);
            if (ct.IsCancellationRequested) return null;

            Log.Warn("TTS: Google TTS unavailable — falling back to Windows Offline TTS.");
            return SynthesizeWindows(text, "", rate, ref synth, ref appliedWinVoice, ct);
        }

        if (provider == "google")
        {
            string lang = ResolveGoogleLang(voice);
            var pcm = TrySynthesizeGoogle(text, lang, ct);
            if (pcm != null && pcm.Length > 0) return ApplyTimeStretch(pcm, rate);
            if (ct.IsCancellationRequested) return null;

            Log.Warn($"TTS: Google TTS ({lang}) unavailable — falling back to Windows Offline TTS.");
            return SynthesizeWindows(text, "", rate, ref synth, ref appliedWinVoice, ct);
        }

        return SynthesizeWindows(text, voice, rate, ref synth, ref appliedWinVoice, ct);
    }

    private static string ResolveGoogleLang(string voice)
    {
        var v = (voice ?? "").Trim().ToLowerInvariant();
        return v is "pl" or "en" or "de" or "uk" or "es" or "fr" ? v : "pl";
    }

    private static string ResolveStreamElementsVoice(string voice)
    {
        var v = (voice ?? "").Trim();
        foreach (var info in StreamElementsVoices)
            if (string.Equals(info.Id, v, StringComparison.OrdinalIgnoreCase))
                return info.Id;
        return "Maja";
    }

    private static float[]? TrySynthesizeGoogle(string text, string lang, CancellationToken ct)
    {
        try
        {
            var chunks = SplitTextChunks(text, 180);
            if (chunks.Count == 0) return null;

            var combined = new List<float>(AudioEngine.SampleRate * AudioEngine.Channels * 4);
            int maxSamples = AudioEngine.SampleRate * AudioEngine.Channels * 20;

            foreach (var chunk in chunks)
            {
                if (ct.IsCancellationRequested || combined.Count >= maxSamples) break;
                string url = $"https://translate.google.com/translate_tts?ie=UTF-8&client=tw-ob&tl={Uri.EscapeDataString(lang)}&q={Uri.EscapeDataString(chunk)}";
                var mp3 = DownloadBytes(url, ct);
                if (mp3 == null || mp3.Length == 0) return null;
                if (!DecodeMp3Into(mp3, combined, maxSamples, ct)) return null;
            }
            return combined.Count > 0 ? combined.ToArray() : null;
        }
        catch (OperationCanceledException) { return null; }
        catch (Exception ex)
        {
            Log.Warn("TTS Google error: " + ex.Message);
            return null;
        }
    }

    private static float[]? TrySynthesizeStreamElements(string text, string voice, CancellationToken ct)
    {
        try
        {
            var chunks = SplitTextChunks(text, 280);
            if (chunks.Count == 0) return null;

            var combined = new List<float>(AudioEngine.SampleRate * AudioEngine.Channels * 4);
            int maxSamples = AudioEngine.SampleRate * AudioEngine.Channels * 20;

            foreach (var chunk in chunks)
            {
                if (ct.IsCancellationRequested || combined.Count >= maxSamples) break;
                string url = $"https://api.streamelements.com/kappa/v2/speech?voice={Uri.EscapeDataString(voice)}&text={Uri.EscapeDataString(chunk)}";
                var mp3 = DownloadBytes(url, ct);
                if (mp3 == null || mp3.Length == 0) return null;
                if (!DecodeMp3Into(mp3, combined, maxSamples, ct)) return null;
            }
            return combined.Count > 0 ? combined.ToArray() : null;
        }
        catch (OperationCanceledException) { return null; }
        catch (Exception ex)
        {
            Log.Warn("TTS StreamElements error: " + ex.Message);
            return null;
        }
    }

    private static byte[]? DownloadBytes(string url, CancellationToken ct)
    {
        using var reqCts = CancellationTokenSource.CreateLinkedTokenSource(ct);
        reqCts.CancelAfter(TimeSpan.FromSeconds(5));
        using var resp = Http.GetAsync(url, HttpCompletionOption.ResponseContentRead, reqCts.Token).GetAwaiter().GetResult();
        if (!resp.IsSuccessStatusCode)
        {
            Log.Warn($"TTS HTTP {(int)resp.StatusCode} ({resp.ReasonPhrase})");
            return null;
        }
        var data = resp.Content.ReadAsByteArrayAsync(reqCts.Token).GetAwaiter().GetResult();
        return data.Length > 0 ? data : null;
    }

    private static bool DecodeMp3Into(byte[] mp3Bytes, List<float> dst, int maxSamples, CancellationToken ct)
    {
        using var mem = new MemoryStream(mp3Bytes, writable: false);
        using var reader = new Mp3FileReader(mem);
        ISampleProvider sp = reader.ToSampleProvider();
        if (sp.WaveFormat.Channels == 1) sp = new MonoToStereoSampleProvider(sp);
        else if (sp.WaveFormat.Channels > 2) sp = new MultiplexingSampleProvider(new[] { sp }, 2);
        if (sp.WaveFormat.SampleRate != AudioEngine.SampleRate)
            sp = new WdlResamplingSampleProvider(sp, AudioEngine.SampleRate);

        var tmp = new float[4096];
        int n;
        while (!ct.IsCancellationRequested && (n = sp.Read(tmp, 0, tmp.Length)) > 0 && dst.Count < maxSamples)
        {
            for (int i = 0; i < n && dst.Count < maxSamples; i++) dst.Add(tmp[i]);
        }
        return !ct.IsCancellationRequested;
    }

    /// <summary>
    /// Pitch-preserving WSOLA (Waveform Similarity Overlap-Add) time-stretching for 48 kHz interleaved stereo TTS audio.
    /// Changes speaking speed (0.5x..2.0x) without altering voice pitch. Runs on TtsWorker thread.
    /// </summary>
    private static float[] ApplyTimeStretch(float[] stereoPcm, float rate)
    {
        rate = Math.Clamp(rate, 0.5f, 2.0f);
        if (MathF.Abs(rate - 1.0f) < 0.01f) return stereoPcm;

        int inFrames = stereoPcm.Length / AudioEngine.Channels;
        const int winLen = 1920;      // 40 ms @ 48 kHz
        const int synHop = winLen / 2; // 20 ms (50% Hann overlap -> unity gain)
        const int searchRadius = 384;  // +-8 ms alignment search
        if (inFrames <= winLen + synHop) return stereoPcm;

        // Extract mono (TTS is mono duplicated to stereo)
        var mono = new float[inFrames];
        for (int i = 0; i < inFrames; i++)
            mono[i] = 0.5f * (stereoPcm[i * 2] + stereoPcm[i * 2 + 1]);

        // Precompute periodic Hann window (w[i] + w[i + synHop] == 1.0)
        var hann = new float[winLen];
        for (int i = 0; i < winLen; i++)
            hann[i] = 0.5f * (1f - MathF.Cos(2f * MathF.PI * i / winLen));

        int estOutFrames = Math.Max(winLen, (int)MathF.Ceiling(inFrames / rate) + winLen);
        var outMono = new float[estOutFrames];

        // First frame: keep leading half unattenuated, fade out trailing half
        for (int i = 0; i < synHop; i++) outMono[i] = mono[i];
        for (int i = synHop; i < winLen; i++) outMono[i] = mono[i] * hann[i];

        int prevBestPos = 0;
        int outPos = synHop;
        int step = 1;

        while (true)
        {
            int nom = (int)MathF.Round(step * synHop * rate);
            if (nom + winLen > inFrames) break;
            if (outPos + winLen > outMono.Length) Array.Resize(ref outMono, outMono.Length + winLen * 8);

            int refStart = Math.Min(prevBestPos + synHop, inFrames - synHop);
            int minCand = Math.Max(0, nom - searchRadius);
            int maxCand = Math.Min(inFrames - winLen, nom + searchRadius);

            int bestPos = Math.Clamp(nom, minCand, maxCand);
            float bestScore = float.NegativeInfinity;

            // Normalized cross-correlation at stride 4 (12 kHz effective) for fast, accurate pitch alignment
            for (int cand = minCand; cand <= maxCand; cand += 2)
            {
                float dot = 0f, eRef = 1e-9f, eCand = 1e-9f;
                for (int i = 0; i < synHop; i += 4)
                {
                    float a = mono[refStart + i];
                    float b = mono[cand + i];
                    dot += a * b;
                    eRef += a * a;
                    eCand += b * b;
                }
                float score = dot / MathF.Sqrt(eRef * eCand);
                if (score > bestScore)
                {
                    bestScore = score;
                    bestPos = cand;
                }
            }

            for (int i = 0; i < winLen; i++)
                outMono[outPos + i] += mono[bestPos + i] * hann[i];

            prevBestPos = bestPos;
            outPos += synHop;
            step++;
        }

        int finalFrames = Math.Min(outPos + synHop, outMono.Length);
        var result = new float[finalFrames * AudioEngine.Channels];
        for (int i = 0; i < finalFrames; i++)
        {
            float s = Math.Clamp(outMono[i], -1f, 1f);
            result[i * 2] = s;
            result[i * 2 + 1] = s;
        }
        return result;
    }

    private static List<string> SplitTextChunks(string text, int maxLen)
    {
        var result = new List<string>();
        var remaining = text.Trim();
        while (remaining.Length > 0)
        {
            if (remaining.Length <= maxLen)
            {
                result.Add(remaining);
                break;
            }
            int cut = -1;
            // Prefer splitting at sentence punctuation first, then comma/semicolon, then whitespace
            for (int i = maxLen; i >= maxLen / 2; i--)
            {
                char c = remaining[i - 1];
                if (c is '.' or '!' or '?') { cut = i; break; }
            }
            if (cut < 0)
            {
                for (int i = maxLen; i >= maxLen / 2; i--)
                {
                    char c = remaining[i - 1];
                    if (c is ',' or ';' or ':') { cut = i; break; }
                }
            }
            if (cut < 0)
            {
                for (int i = maxLen; i >= 1; i--)
                {
                    if (char.IsWhiteSpace(remaining[i - 1])) { cut = i; break; }
                }
            }
            if (cut <= 0) cut = maxLen;
            string part = remaining[..cut].Trim();
            if (part.Length > 0) result.Add(part);
            remaining = remaining[cut..].Trim();
        }
        return result;
    }

    private static float[]? SynthesizeWindows(
        string text,
        string desiredVoice,
        float rate,
        ref SpeechSynthesizer? synth,
        ref string appliedVoice,
        CancellationToken ct)
    {
        synth ??= new SpeechSynthesizer();
        if (!string.Equals(desiredVoice, appliedVoice, StringComparison.Ordinal))
        {
            appliedVoice = desiredVoice;
            if (string.IsNullOrEmpty(desiredVoice))
            {
                synth.Voice = SpeechSynthesizer.DefaultVoice;
            }
            else
            {
                var match = SpeechSynthesizer.AllVoices.FirstOrDefault(v =>
                    string.Equals(v.Id, desiredVoice, StringComparison.OrdinalIgnoreCase) ||
                    string.Equals(v.DisplayName, desiredVoice, StringComparison.OrdinalIgnoreCase));
                synth.Voice = match ?? SpeechSynthesizer.DefaultVoice;
            }
        }
        try { synth.Options.SpeakingRate = Math.Clamp(rate, 0.5f, 2.0f); } catch { }

        using var synthStream = synth.SynthesizeTextToStreamAsync(text).AsTask(ct).GetAwaiter().GetResult();
        if (ct.IsCancellationRequested) return null;

        using var netStream = synthStream.AsStreamForRead();
        using var mem = new MemoryStream();
        netStream.CopyTo(mem);
        if (mem.Length == 0 || ct.IsCancellationRequested) return null;
        mem.Position = 0;

        using var reader = new WaveFileReader(mem);
        ISampleProvider sp = reader.ToSampleProvider();
        if (sp.WaveFormat.Channels == 1) sp = new MonoToStereoSampleProvider(sp);
        else if (sp.WaveFormat.Channels > 2) sp = new MultiplexingSampleProvider(new[] { sp }, 2);
        if (sp.WaveFormat.SampleRate != AudioEngine.SampleRate)
            sp = new WdlResamplingSampleProvider(sp, AudioEngine.SampleRate);

        var pcm = new List<float>(AudioEngine.SampleRate * AudioEngine.Channels * 4);
        var tmp = new float[4096];
        int maxSamples = AudioEngine.SampleRate * AudioEngine.Channels * 20;
        int n;
        while (!ct.IsCancellationRequested && (n = sp.Read(tmp, 0, tmp.Length)) > 0 && pcm.Count < maxSamples)
        {
            for (int i = 0; i < n && pcm.Count < maxSamples; i++) pcm.Add(tmp[i]);
        }
        return pcm.Count > 0 ? pcm.ToArray() : null;
    }

    public void Dispose()
    {
        try { _disposeCts.Cancel(); } catch { }
        try { _utteranceCts?.Cancel(); } catch { }
        try { _signal.Release(); } catch { }
        try { _worker.Join(1500); } catch { }
        _disposeCts.Dispose();
        _signal.Dispose();
    }
}
