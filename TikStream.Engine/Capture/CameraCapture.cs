using System.Runtime.InteropServices;
using System.Text;
using System.Text.RegularExpressions;
using FFmpeg.AutoGen;
using TikStream.Core;

namespace TikStream.Capture;

public sealed record CameraMode(int Width, int Height, int Fps, string Format)
{
    public string Label => $"{Width}×{Height} • {Fps} fps ({FormatName})";
    public string FormatName => Format switch { "nv12" => "NV12", "yuyv422" => "YUY2", "mjpeg" => "MJPEG", _ => Format.ToUpperInvariant() };
}

public sealed record CameraInfo(string Name, List<CameraMode> Modes);

/// <summary>
/// Kamerka przez FFmpeg/DirectShow na własnym wątku. Wynik: klatka NV12 (Y + przeplecione UV),
/// konwersja do RGB robi shader na GPU — przy surowym NV12 z kamerki procesor tylko kopiuje bajty.
/// </summary>
public sealed unsafe class CameraCapture : IDisposable
{
    private readonly string _name;
    private readonly CameraMode _mode;
    private readonly int _outputFps;
    private readonly Thread _thread;
    private volatile bool _run = true;

    private readonly object _lock = new();
    private byte[] _front = Array.Empty<byte>(), _back = Array.Empty<byte>(), _upload = Array.Empty<byte>();
    private int _w, _h;
    private long _seq;

    public string Name => _name;
    public string Status { get; private set; } = "Uruchamianie…";
    public double MeasuredFps { get; private set; }

    public CameraCapture(string name, CameraMode mode, int outputFps = 120)
    {
        _name = name;
        _mode = mode;
        _outputFps = Math.Max(1, outputFps);
        _thread = new Thread(Run) { IsBackground = true, Name = "Camera", Priority = ThreadPriority.AboveNormal };
        _thread.Start();
    }

    /// <summary>Jeśli jest nowa klatka: upload(ptrY, ptrUV, w, h) — oba plany ze stride = w.</summary>
    public bool TryConsume(ref long lastSeq, Action<IntPtr, IntPtr, int, int> upload)
    {
        int w, h;
        lock (_lock)
        {
            if (_seq == lastSeq || _w == 0) return false;
            lastSeq = _seq;
            w = _w; h = _h;
            (_front, _upload) = (_upload, _front);
        }
        // Wątek przechwytywania może publikować kolejne klatki w trakcie uploadu.
        fixed (byte* p = _upload) upload((IntPtr)p, (IntPtr)(p + w * h), w, h);
        return true;
    }

    private void Run()
    {
        var size = $"{_mode.Width}x{_mode.Height}";
        var fps = _mode.Fps.ToString();
        var primary = new Dictionary<string, string> { ["video_size"] = size, ["framerate"] = fps };
        if (_mode.Format == "mjpeg") primary["vcodec"] = "mjpeg";
        else if (!string.IsNullOrEmpty(_mode.Format)) primary["pixel_format"] = _mode.Format;

        var attempts = new List<Dictionary<string, string>>
        {
            primary,
            new() { ["video_size"] = size, ["framerate"] = fps },
            new() { ["video_size"] = size },
            new(),
        };

        while (_run)
        {
            foreach (var opts in attempts)
            {
                if (!_run) return;
                bool opened = false;
                TryRun(opts, ref opened);
                if (opened) break; // działała i padła → zacznij od najlepszego trybu
            }
            if (!_run) return;
            MeasuredFps = 0;
            Status = "Brak obrazu — ponawiam…";
            Thread.Sleep(2000);
        }
    }

    private void TryRun(Dictionary<string, string> optsIn, ref bool opened)
    {
        AVFormatContext* fmt = null;
        AVCodecContext* dec = null;
        SwsContext* sws = null;
        AVPacket* pkt = ffmpeg.av_packet_alloc();
        AVPacket* newer = null;
        AVFrame* frame = ffmpeg.av_frame_alloc();
        AVDictionary* opts = null;
        try
        {
            foreach (var kv in optsIn) ffmpeg.av_dict_set(&opts, kv.Key, kv.Value, 0);
            int pixels = Math.Max(640 * 480, _mode.Width * _mode.Height);
            int bufferBytes = optsIn.TryGetValue("vcodec", out var videoCodec) && videoCodec == "mjpeg"
                ? Math.Max(4 * 1024 * 1024, pixels * 2)
                : pixels * 3; // mniejszy bufor NV12 = mniejsze opóźnienie kamerki względem mikrofonu
            ffmpeg.av_dict_set(&opts, "rtbufsize", bufferBytes.ToString(System.Globalization.CultureInfo.InvariantCulture), 0);
            var ifmt = ffmpeg.av_find_input_format("dshow");
            string desc = string.Join(",", optsIn.Select(k => k.Key + "=" + k.Value));
            int r = ffmpeg.avformat_open_input(&fmt, "video=" + _name, ifmt, &opts);
            if (r < 0) { Log.Warn($"Kamerka '{_name}' [{desc}]: {FF.Err(r)}"); return; }
            int vi = ffmpeg.av_find_best_stream(fmt, AVMediaType.AVMEDIA_TYPE_VIDEO, -1, -1, null, 0);
            if (vi < 0) return;
            var par = fmt->streams[vi]->codecpar;
            var codec = ffmpeg.avcodec_find_decoder(par->codec_id);
            if (codec == null) return;
            dec = ffmpeg.avcodec_alloc_context3(codec);
            ffmpeg.avcodec_parameters_to_context(dec, par);
            // Nie buforuj przyszłych klatek w dekoderze kamery. MJPEG dopuszcza pomijanie spóźnionych pakietów.
            dec->thread_count = 1;
            dec->thread_type = 0;
            bool independent = par->codec_id is AVCodecID.AV_CODEC_ID_MJPEG or AVCodecID.AV_CODEC_ID_RAWVIDEO;
            if (ffmpeg.avcodec_open2(dec, codec, null) < 0) return;
            if (independent) newer = ffmpeg.av_packet_alloc();

            opened = true;
            string fmtName = par->codec_id == AVCodecID.AV_CODEC_ID_RAWVIDEO
                ? ffmpeg.av_get_pix_fmt_name((AVPixelFormat)par->format)
                : ffmpeg.avcodec_get_name(par->codec_id);
            Status = $"{par->width}×{par->height} {fmtName}";
            Log.Info($"Kamerka '{_name}' otwarta [{desc}] → {Status}");

            var sw = System.Diagnostics.Stopwatch.StartNew();
            int frames = 0;
            double fpsSampleStart = 0;
            double nextFrame = 0;
            double publishInterval = 1.0 / Math.Min(_mode.Fps, _outputFps);
            while (_run)
            {
                r = ffmpeg.av_read_frame(fmt, pkt);
                if (r < 0) { Log.Warn("Kamerka: odczyt przerwany: " + FF.Err(r)); return; }
                if (pkt->stream_index != vi) { ffmpeg.av_packet_unref(pkt); continue; }
                if (newer != null)
                {
                    // Zostaw najnowszy dostępny pakiet. Zegary DirectShow i aplikacji mogą się rozjeżdżać.
                    int flags = fmt->flags;
                    fmt->flags |= ffmpeg.AVFMT_FLAG_NONBLOCK;
                    for (int i = 0; i < 16 && _run && ffmpeg.av_read_frame(fmt, newer) >= 0; i++)
                    {
                        ffmpeg.av_packet_unref(pkt);
                        ffmpeg.av_packet_move_ref(pkt, newer);
                    }
                    fmt->flags = flags;
                }
                double now = sw.Elapsed.TotalSeconds;
                if (independent && now < nextFrame)
                {
                    ffmpeg.av_packet_unref(pkt);
                    continue;
                }
                r = ffmpeg.avcodec_send_packet(dec, pkt);
                ffmpeg.av_packet_unref(pkt);
                if (r < 0) continue;
                nextFrame = now + publishInterval * 0.85;
                while (ffmpeg.avcodec_receive_frame(dec, frame) == 0)
                {
                    StoreNv12(frame, ref sws);
                    ffmpeg.av_frame_unref(frame);
                    frames++;
                    double sampleEnd = sw.Elapsed.TotalSeconds;
                    if (sampleEnd - fpsSampleStart >= 1)
                    {
                        MeasuredFps = frames / (sampleEnd - fpsSampleStart);
                        frames = 0;
                        fpsSampleStart = sampleEnd;
                    }
                }
            }
        }
        finally
        {
            if (opts != null) ffmpeg.av_dict_free(&opts);
            if (sws != null) ffmpeg.sws_freeContext(sws);
            if (dec != null) ffmpeg.avcodec_free_context(&dec);
            if (fmt != null) ffmpeg.avformat_close_input(&fmt);
            ffmpeg.av_frame_free(&frame);
            ffmpeg.av_packet_free(&pkt);
            ffmpeg.av_packet_free(&newer);
        }
    }

    private void StoreNv12(AVFrame* f, ref SwsContext* sws)
    {
        int w = f->width & ~1, h = f->height & ~1;
        int size = w * h * 3 / 2;
        if (_back.Length != size) _back = new byte[size];
        fixed (byte* dst = _back)
        {
            if ((AVPixelFormat)f->format == AVPixelFormat.AV_PIX_FMT_NV12)
            {
                // surowe NV12 z kamerki: tylko kopia wierszy
                for (int y = 0; y < h; y++)
                    Buffer.MemoryCopy(f->data[0] + y * f->linesize[0], dst + y * w, w, w);
                byte* uv = dst + w * h;
                for (int y = 0; y < h / 2; y++)
                    Buffer.MemoryCopy(f->data[1] + y * f->linesize[1], uv + y * w, w, w);
            }
            else
            {
                sws = ffmpeg.sws_getCachedContext(sws, f->width, f->height, (AVPixelFormat)f->format, w, h,
                    AVPixelFormat.AV_PIX_FMT_NV12, (int)SwsFlags.SWS_FAST_BILINEAR, null, null, null);
                var dd = new byte_ptrArray4 { [0] = dst, [1] = dst + w * h };
                var ds = new int_array4 { [0] = w, [1] = w };
                ffmpeg.sws_scale(sws, f->data, f->linesize, 0, f->height, dd, ds);
            }
        }
        lock (_lock)
        {
            (_front, _back) = (_back, _front);
            _w = w; _h = h; _seq++;
        }
    }

    public void Dispose()
    {
        _run = false;
        if (!_thread.Join(3000)) Log.Warn("Wątek kamerki nie zakończył się w czasie.");
    }

    // ======================= wyliczanie kamerek i trybów =======================

    private static readonly Regex DeviceRx = new("\"([^\"]+)\"\\s+\\(video\\)", RegexOptions.Compiled);
    private static readonly Regex ModeRx = new(@"(pixel_format|vcodec)=(\S+)\s+min s=(\d+)x(\d+) fps=([\d.]+) max s=(\d+)x(\d+) fps=([\d.]+)", RegexOptions.Compiled);
    private static readonly object EnumLock = new();

    public static List<CameraInfo> Enumerate()
    {
        lock (EnumLock)
        {
            var names = new List<string>();
            var text = FF.CaptureLog(() => OpenDshow("video=dummy", ("list_devices", "true")));
            foreach (Match m in DeviceRx.Matches(text))
                if (!names.Contains(m.Groups[1].Value)) names.Add(m.Groups[1].Value);

            var result = new List<CameraInfo>();
            foreach (var n in names)
            {
                var opts = FF.CaptureLog(() => OpenDshow("video=" + n, ("list_options", "true")));
                result.Add(new CameraInfo(n, ParseModes(opts)));
            }
            return result;
        }
    }

    private static void OpenDshow(string url, params (string, string)[] options)
    {
        AVFormatContext* fmt = null;
        AVDictionary* o = null;
        foreach (var (k, v) in options) ffmpeg.av_dict_set(&o, k, v, 0);
        ffmpeg.avformat_open_input(&fmt, url, ffmpeg.av_find_input_format("dshow"), &o);
        if (fmt != null) ffmpeg.avformat_close_input(&fmt);
        ffmpeg.av_dict_free(&o);
    }

    private static List<CameraMode> ParseModes(string text)
    {
        static int Rank(string f) => f switch { "nv12" => 0, "yuyv422" => 1, "mjpeg" => 2, _ => 3 };
        var best = new Dictionary<(int, int, int), CameraMode>();
        foreach (Match m in ModeRx.Matches(text))
        {
            string format = m.Groups[2].Value;
            int w = int.Parse(m.Groups[6].Value), h = int.Parse(m.Groups[7].Value);
            double min = double.Parse(m.Groups[5].Value, System.Globalization.CultureInfo.InvariantCulture);
            double max = double.Parse(m.Groups[8].Value, System.Globalization.CultureInfo.InvariantCulture);
            foreach (var f in new[] { 24, 25, 30, 50, 60, 90, 120, 144, 240 })
            {
                if (f < min - 0.5 || f > max + 0.5) continue;
                var key = (w, h, f);
                if (!best.TryGetValue(key, out var cur) || Rank(format) < Rank(cur.Format))
                    best[key] = new CameraMode(w, h, f, format);
            }
        }
        return best.Values.OrderByDescending(x => x.Width * x.Height).ThenByDescending(x => x.Fps).ToList();
    }
}

internal static unsafe class FF
{
    public static string Err(int code)
    {
        const int size = 512;
        var buf = stackalloc byte[size];
        ffmpeg.av_strerror(code, buf, size);
        return Marshal.PtrToStringAnsi((IntPtr)buf) ?? code.ToString();
    }

    public static void Check(int code, string what)
    {
        if (code < 0) throw new InvalidOperationException($"{what}: {Err(code)} ({code})");
    }

    private static av_log_set_callback_callback? _logCb;
    private static int _printPrefix = 1;
    private static StringBuilder? _capture;
    private static readonly object CaptureLock = new();
    private static bool _initialized;

    /// <summary>Przechwytuje komunikaty FFmpeg (np. listę urządzeń dshow) zamiast zapisywać je do logu.</summary>
    public static string CaptureLog(Action action)
    {
        lock (CaptureLock)
        {
            var sb = new StringBuilder();
            int oldLevel = ffmpeg.av_log_get_level();
            _capture = sb;
            ffmpeg.av_log_set_level(ffmpeg.AV_LOG_INFO);
            try { action(); }
            finally { ffmpeg.av_log_set_level(oldLevel); _capture = null; }
            return sb.ToString();
        }
    }

    public static void Init(string nativeDir)
    {
        if (_initialized) return;
        _initialized = true;
        ffmpeg.RootPath = nativeDir;
        Log.Info("FFmpeg " + ffmpeg.av_version_info());
        ffmpeg.avdevice_register_all();
        ffmpeg.avformat_network_init();
        ffmpeg.av_log_set_level(ffmpeg.AV_LOG_WARNING);
        _logCb = (p0, level, format, vl) =>
        {
            if (level > ffmpeg.av_log_get_level()) return;
            const int size = 2048;
            var line = stackalloc byte[size];
            int prefix = _printPrefix;
            ffmpeg.av_log_format_line(p0, level, format, vl, line, size, &prefix);
            _printPrefix = prefix;
            var s = Marshal.PtrToStringUTF8((IntPtr)line) ?? "";
            var cap = _capture;
            if (cap != null) { lock (cap) cap.Append(s); return; }
            s = s.TrimEnd();
            if (s.Contains("deprecated pixel format used", StringComparison.Ordinal)) return;
            if (s.Length > 0) Log.Info("ffmpeg: " + s);
        };
        ffmpeg.av_log_set_callback(_logCb);
    }
}
