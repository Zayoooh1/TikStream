using System.Diagnostics;
using System.IO.Compression;
using System.Runtime.Intrinsics;
using System.Runtime.Intrinsics.X86;
using FFmpeg.AutoGen;
using TikStream.Core;

namespace TikStream.Capture;

/// <summary>
/// Odtwarzacz plików (MP4/MKV/MOV/WEBM/GIF…) dla źródła „Multimedia”.
/// Obraz → NV12 lub BGRA z alfą, dźwięk → 48 kHz stereo do miksera.
/// Tempo wyznacza zegar odtwarzania; po końcu pliku opcjonalnie od początku.
/// </summary>
public sealed unsafe class MediaPlayer : IDisposable
{
    private readonly string _path;
    private readonly bool _loop;
    private readonly Action<float[], int>? _audioSink;
    private readonly Thread _thread;
    private volatile bool _run = true;

    private readonly object _lock = new();
    private byte[] _front = [], _back = [], _upload = [];
    private int _w, _h;
    private bool _alpha;
    private bool _transparent;
    private long _seq;
    private HashSet<long>? _emptyVideoPts;
    private Dictionary<long, CachedBand>? _bandCache;
    private long _bandCacheBytes;
    private const long MaxBandCacheBytes = 96L * 1024 * 1024; // maks. 96 MB skompresowanego cache dla zapętlonych nakładek ProRes
    private bool _emptyMapComplete;
    private DateTime _cachedMtime;

    private readonly record struct CachedBand(int W, int H, int YMin, int BandH, byte[] CompressedBgra);

    public string Status { get; private set; } = "Otwieranie…";
    public int Width => _w;
    public int Height => _h;

    public MediaPlayer(string path, bool loop, Action<float[], int>? audioSink)
    {
        _path = path;
        _loop = loop;
        _audioSink = audioSink;
        _thread = new Thread(Run) { IsBackground = true, Name = "Media", Priority = ThreadPriority.AboveNormal };
        _thread.Start();
    }

    public bool TryConsume(ref long lastSeq, Action<IntPtr, IntPtr, int, int, bool, bool> upload)
    {
        int w, h;
        bool alpha, transparent;
        lock (_lock)
        {
            if (_seq == lastSeq || _w == 0) return false;
            lastSeq = _seq;
            w = _w;
            h = _h;
            alpha = _alpha;
            transparent = _transparent;
            if (!transparent) (_front, _upload) = (_upload, _front);
        }
        if (transparent)
        {
            upload(IntPtr.Zero, IntPtr.Zero, w, h, true, true);
            return true;
        }
        fixed (byte* p = _upload) upload((IntPtr)p, alpha ? IntPtr.Zero : (IntPtr)(p + w * h), w, h, alpha, false);
        return true;
    }

    private void Run()
    {
        while (_run)
        {
            bool ok = PlayOnce();
            if (!_run) return;
            if (!ok) { Status = "Nie można odtworzyć pliku"; Thread.Sleep(2000); continue; }
            if (!_loop) { Status = "Zakończono"; return; }
        }
    }

    private bool PlayOnce()
    {
        if (!File.Exists(_path)) { Status = "Brak pliku"; return false; }
        AVFormatContext* fmt = null;
        AVCodecContext* vdec = null, adec = null;
        SwsContext* sws = null;
        SwrContext* swr = null;
        AVPacket* pkt = ffmpeg.av_packet_alloc();
        AVFrame* frame = ffmpeg.av_frame_alloc();
        try
        {
            if (ffmpeg.avformat_open_input(&fmt, _path, null, null) < 0) return false;
            if (ffmpeg.avformat_find_stream_info(fmt, null) < 0) return false;
            int vi = ffmpeg.av_find_best_stream(fmt, AVMediaType.AVMEDIA_TYPE_VIDEO, -1, -1, null, 0);
            int ai = _audioSink != null ? ffmpeg.av_find_best_stream(fmt, AVMediaType.AVMEDIA_TYPE_AUDIO, -1, -1, null, 0) : -1;
            if (vi < 0 && ai < 0) return false;
            if (vi >= 0) vdec = OpenDecoder(fmt->streams[vi], 4);
            if (ai >= 0) adec = OpenDecoder(fmt->streams[ai], 1);
            if (ai >= 0 && adec != null)
            {
                AVChannelLayout outLayout;
                ffmpeg.av_channel_layout_default(&outLayout, 2);
                FF.Check(ffmpeg.swr_alloc_set_opts2(&swr, &outLayout, AVSampleFormat.AV_SAMPLE_FMT_FLT, AudioEngine.SampleRate,
                    &adec->ch_layout, adec->sample_fmt, adec->sample_rate, 0, null), "swr_alloc_set_opts2");
                FF.Check(ffmpeg.swr_init(swr), "swr_init");
            }
            Status = $"Odtwarzanie • {Path.GetFileName(_path)}";

            var clock = Stopwatch.StartNew();
            double vtb = vi >= 0 ? ffmpeg.av_q2d(fmt->streams[vi]->time_base) : 0;
            var streamAlpha = vi >= 0 ? fmt->streams[vi]->codecpar->alpha_mode : AVAlphaMode.AVALPHA_MODE_UNSPECIFIED;
            double atb = ai >= 0 ? ffmpeg.av_q2d(fmt->streams[ai]->time_base) : 0;
            double startPts = double.NaN;
            float[] abuf = new float[16384];

            bool intraOnly = _loop && vdec != null && vdec->codec_id == AVCodecID.AV_CODEC_ID_PRORES;
            if (intraOnly)
            {
                var mtime = File.GetLastWriteTimeUtc(_path);
                if (mtime != _cachedMtime)
                {
                    _cachedMtime = mtime;
                    _emptyVideoPts = new HashSet<long>();
                    _bandCache = new Dictionary<long, CachedBand>();
                    _bandCacheBytes = 0;
                    _emptyMapComplete = false;
                }
            }

            while (_run)
            {
                int r = ffmpeg.av_read_frame(fmt, pkt);
                if (r < 0) break; // koniec pliku
                AVCodecContext* dec = pkt->stream_index == vi ? vdec : pkt->stream_index == ai ? adec : null;
                if (dec == null) { ffmpeg.av_packet_unref(pkt); continue; }
                bool isVideo = dec == vdec;
                if (isVideo && intraOnly && _emptyMapComplete && pkt->pts != ffmpeg.AV_NOPTS_VALUE && (pkt->flags & ffmpeg.AV_PKT_FLAG_KEY) != 0)
                {
                    if (_emptyVideoPts != null && _emptyVideoPts.Contains(pkt->pts))
                    {
                        double t = pkt->pts * vtb;
                        if (double.IsNaN(startPts)) startPts = t;
                        double wait = t - startPts - clock.Elapsed.TotalSeconds;
                        if (wait > 0.002) Thread.Sleep(TimeSpan.FromSeconds(Math.Min(wait, 0.5)));
                        PublishTransparent(vdec->width, vdec->height);
                        ffmpeg.av_packet_unref(pkt);
                        continue;
                    }
                    if (_bandCache != null && _bandCache.TryGetValue(pkt->pts, out var cached))
                    {
                        double t = pkt->pts * vtb;
                        if (double.IsNaN(startPts)) startPts = t;
                        double wait = t - startPts - clock.Elapsed.TotalSeconds;
                        if (wait > 0.002) Thread.Sleep(TimeSpan.FromSeconds(Math.Min(wait, 0.5)));
                        PublishCachedBand(cached);
                        ffmpeg.av_packet_unref(pkt);
                        continue;
                    }
                }
                ffmpeg.avcodec_send_packet(dec, pkt);
                ffmpeg.av_packet_unref(pkt);
                while (_run && ffmpeg.avcodec_receive_frame(dec, frame) == 0)
                {
                    long ts = frame->best_effort_timestamp;
                    double t = ts == ffmpeg.AV_NOPTS_VALUE ? clock.Elapsed.TotalSeconds : ts * (isVideo ? vtb : atb);
                    if (double.IsNaN(startPts)) startPts = t;
                    double due = t - startPts;
                    // dźwięk wysyłamy ~60 ms przed czasem (bufor miksera), obraz dokładnie na czas
                    double wait = due - clock.Elapsed.TotalSeconds - (isVideo ? 0 : 0.06);
                    if (wait > 0.002) Thread.Sleep(TimeSpan.FromSeconds(Math.Min(wait, 0.5)));
                    if (isVideo)
                    {
                        bool cacheThis = intraOnly && !_emptyMapComplete && ts != ffmpeg.AV_NOPTS_VALUE;
                        bool empty = StoreVideo(frame, streamAlpha, ref sws, cacheThis ? ts : ffmpeg.AV_NOPTS_VALUE);
                        if (empty && cacheThis) _emptyVideoPts?.Add(ts);
                    }
                    else if (swr != null) PushAudio(swr, frame, ref abuf);
                    ffmpeg.av_frame_unref(frame);
                }
            }
            if (_run && intraOnly && !_emptyMapComplete) _emptyMapComplete = true;
            return true;
        }
        catch (Exception ex) { Log.Warn($"Multimedia '{_path}': {ex.Message}"); return false; }
        finally
        {
            if (sws != null) ffmpeg.sws_freeContext(sws);
            if (swr != null) ffmpeg.swr_free(&swr);
            if (vdec != null) ffmpeg.avcodec_free_context(&vdec);
            if (adec != null) ffmpeg.avcodec_free_context(&adec);
            if (fmt != null) ffmpeg.avformat_close_input(&fmt);
            ffmpeg.av_frame_free(&frame);
            ffmpeg.av_packet_free(&pkt);
        }
    }

    private static AVCodecContext* OpenDecoder(AVStream* st, int threads)
    {
        bool hasAlphaMeta = ffmpeg.av_dict_get(st->metadata, "alpha_mode", null, 0) != null;
        // Dla zwykłych plików wideo (bez alfy) próbujemy najpierw sprzętowego dekodera NVIDIA CUVID (NVDEC),
        // co zdejmuje dekodowanie H.264/HEVC/VP9/AV1 z CPU. Przy błędzie fallback na CPU.
        if (st->codecpar->codec_type == AVMediaType.AVMEDIA_TYPE_VIDEO && !hasAlphaMeta)
        {
            string? cuvidName = st->codecpar->codec_id switch
            {
                AVCodecID.AV_CODEC_ID_H264 => "h264_cuvid",
                AVCodecID.AV_CODEC_ID_HEVC => "hevc_cuvid",
                AVCodecID.AV_CODEC_ID_VP9 => "vp9_cuvid",
                AVCodecID.AV_CODEC_ID_AV1 => "av1_cuvid",
                _ => null,
            };
            if (cuvidName != null)
            {
                var hwCodec = ffmpeg.avcodec_find_decoder_by_name(cuvidName);
                if (hwCodec != null)
                {
                    var hwCtx = ffmpeg.avcodec_alloc_context3(hwCodec);
                    ffmpeg.avcodec_parameters_to_context(hwCtx, st->codecpar);
                    if (ffmpeg.avcodec_open2(hwCtx, hwCodec, null) >= 0) return hwCtx;
                    ffmpeg.avcodec_free_context(&hwCtx);
                }
            }
        }

        var codec = ffmpeg.avcodec_find_decoder(st->codecpar->codec_id);
        if (hasAlphaMeta)
        {
            // Matroska/WebM niesie alfę jako dodatkowy blok VP8/VP9.
            var alphaDecoder = st->codecpar->codec_id switch
            {
                AVCodecID.AV_CODEC_ID_VP8 => ffmpeg.avcodec_find_decoder_by_name("libvpx"),
                AVCodecID.AV_CODEC_ID_VP9 => ffmpeg.avcodec_find_decoder_by_name("libvpx-vp9"),
                _ => null,
            };
            if (alphaDecoder != null) codec = alphaDecoder;
        }
        if (codec == null) return null;
        var ctx = ffmpeg.avcodec_alloc_context3(codec);
        ffmpeg.avcodec_parameters_to_context(ctx, st->codecpar);
        ctx->thread_count = threads;
        if (ffmpeg.avcodec_open2(ctx, codec, null) < 0) { ffmpeg.avcodec_free_context(&ctx); return null; }
        return ctx;
    }

    private void PublishCachedBand(in CachedBand b)
    {
        int size = b.W * b.H * 4;
        int stride = b.W * 4;
        if (_back.Length != size) _back = new byte[size];
        if (b.YMin > 0) Array.Clear(_back, 0, b.YMin * stride);
        int bottomStart = (b.YMin + b.BandH) * stride;
        if (bottomStart < size) Array.Clear(_back, bottomStart, size - bottomStart);
        using (var ms = new MemoryStream(b.CompressedBgra, writable: false))
        using (var ds = new DeflateStream(ms, CompressionMode.Decompress))
        {
            int offset = b.YMin * stride;
            int remaining = b.BandH * stride;
            while (remaining > 0)
            {
                int r = ds.Read(_back, offset, remaining);
                if (r <= 0) break;
                offset += r;
                remaining -= r;
            }
        }
        lock (_lock)
        {
            (_front, _back) = (_back, _front);
            _w = b.W; _h = b.H; _alpha = true; _transparent = false; _seq++;
        }
    }

    private void TryCacheBand(long pts, int w, int h, int yMin, int bandH)
    {
        if (pts == ffmpeg.AV_NOPTS_VALUE || _bandCache == null || _bandCacheBytes >= MaxBandCacheBytes) return;
        int stride = w * 4;
        using var ms = new MemoryStream(bandH * stride / 8);
        using (var ds = new DeflateStream(ms, CompressionLevel.Fastest, leaveOpen: true))
            ds.Write(_back, yMin * stride, bandH * stride);
        var bytes = ms.ToArray();
        if (_bandCacheBytes + bytes.Length > MaxBandCacheBytes) return;
        _bandCache[pts] = new CachedBand(w, h, yMin, bandH, bytes);
        _bandCacheBytes += bytes.Length;
    }

    private void PublishTransparent(int sw, int sh)
    {
        double s = Math.Min(1.0, 1920.0 / Math.Max(sw, sh));
        int w = (int)(sw * s) & ~1, h = (int)(sh * s) & ~1;
        if (w < 2 || h < 2) return;
        lock (_lock)
        {
            if (_transparent && _w == w && _h == h) return;
            _w = w; _h = h; _alpha = true; _transparent = true; _seq++;
        }
    }

    private bool StoreVideo(AVFrame* f, AVAlphaMode streamAlpha, ref SwsContext* sws, long cachePts)
    {
        // pliki większe niż 1080p skalujemy w dół — na pionowym płótnie i tak nie będzie widać różnicy
        int sw = f->width, sh = f->height;
        double s = Math.Min(1.0, 1920.0 / Math.Max(sw, sh));
        int w = (int)(sw * s) & ~1, h = (int)(sh * s) & ~1;
        if (w < 2 || h < 2) return false;
        var format = (AVPixelFormat)f->format;
        var desc = ffmpeg.av_pix_fmt_desc_get(format);
        bool alpha = desc != null && (desc->flags & ffmpeg.AV_PIX_FMT_FLAG_ALPHA) != 0;
        var alphaMode = f->alpha_mode == AVAlphaMode.AVALPHA_MODE_UNSPECIFIED ? streamAlpha : f->alpha_mode;
        bool premultiply = alpha && alphaMode != AVAlphaMode.AVALPHA_MODE_PREMULTIPLIED;
        int yMin = 0, yMax = sh - 1;
        if (premultiply && format == AVPixelFormat.AV_PIX_FMT_YUVA444P12LE)
        {
            if (!AlphaBounds12Bit(f, out yMin, out yMax))
            {
                PublishTransparent(sw, sh);
                return true;
            }
        }
        int size = alpha ? w * h * 4 : w * h * 3 / 2;
        if (_back.Length != size) _back = new byte[size];
        if (premultiply && format == AVPixelFormat.AV_PIX_FMT_YUVA444P12LE && w == sw && h == sh && (yMin > 0 || yMax < sh - 1))
        {
            int stride = w * 4;
            int bandH = yMax - yMin + 1;
            if (yMin > 0) Array.Clear(_back, 0, yMin * stride);
            if (yMax < h - 1) Array.Clear(_back, (yMax + 1) * stride, (h - 1 - yMax) * stride);
            sws = ffmpeg.sws_getCachedContext(sws, sw, bandH, format, w, bandH, AVPixelFormat.AV_PIX_FMT_BGRA,
                (int)SwsFlags.SWS_BILINEAR, null, null, null);
            if (sws == null) throw new InvalidOperationException("Nie można przekonwertować formatu obrazu.");
            var sd = new byte_ptrArray4
            {
                [0] = f->data[0] + (nint)yMin * f->linesize[0],
                [1] = f->data[1] + (nint)yMin * f->linesize[1],
                [2] = f->data[2] + (nint)yMin * f->linesize[2],
                [3] = f->data[3] + (nint)yMin * f->linesize[3],
            };
            fixed (byte* dst = _back)
            {
                byte* bandDst = dst + (nint)yMin * stride;
                var dd = new byte_ptrArray4 { [0] = bandDst };
                var ds = new int_array4 { [0] = stride };
                FF.Check(ffmpeg.sws_scale(sws, sd, f->linesize, 0, bandH, dd, ds), "sws_scale");
                Premultiply(bandDst, bandH * stride);
            }
            if (cachePts != ffmpeg.AV_NOPTS_VALUE) TryCacheBand(cachePts, w, h, yMin, bandH);
        }
        else
        {
            sws = ffmpeg.sws_getCachedContext(sws, sw, sh, format, w, h, alpha ? AVPixelFormat.AV_PIX_FMT_BGRA : AVPixelFormat.AV_PIX_FMT_NV12,
                (int)SwsFlags.SWS_BILINEAR, null, null, null);
            if (sws == null) throw new InvalidOperationException("Nie można przekonwertować formatu obrazu.");
            fixed (byte* dst = _back)
            {
                var dd = new byte_ptrArray4 { [0] = dst, [1] = alpha ? null : dst + w * h };
                var ds = new int_array4 { [0] = alpha ? w * 4 : w, [1] = alpha ? 0 : w };
                FF.Check(ffmpeg.sws_scale(sws, f->data, f->linesize, 0, sh, dd, ds), "sws_scale");
                if (premultiply) Premultiply(dst, size);
            }
            if (alpha && cachePts != ffmpeg.AV_NOPTS_VALUE) TryCacheBand(cachePts, w, h, 0, h);
        }
        lock (_lock)
        {
            (_front, _back) = (_back, _front);
            _w = w; _h = h; _alpha = alpha; _transparent = false; _seq++;
        }
        return false;
    }

    private static bool AlphaBounds12Bit(AVFrame* frame, out int yMin, out int yMax)
    {
        // Na całkowicie przezroczystej klatce premultiplikacja i tak zeruje BGRA.
        // Dla niepustych klatek wyznaczamy najwęższe pasmo wierszy z niezerową alfą.
        int h = frame->height, w = frame->width;
        yMin = h; yMax = -1;
        for (int y = 0; y < h; y++)
        {
            var alpha = new ReadOnlySpan<ushort>(frame->data[3] + (nint)y * frame->linesize[3], w);
            if (!alpha.ContainsAnyExcept((ushort)0)) continue;
            yMin = y;
            break;
        }
        if (yMin == h) return false;
        for (int y = h - 1; y >= yMin; y--)
        {
            var alpha = new ReadOnlySpan<ushort>(frame->data[3] + (nint)y * frame->linesize[3], w);
            if (!alpha.ContainsAnyExcept((ushort)0)) continue;
            yMax = y;
            break;
        }
        return yMax >= yMin;
    }

    private static void Premultiply(byte* pixels, int size)
    {
        // Kompozytor miesza tekstury BGRA z premultiplikowanym RGB.
        int i = 0;
        if (Avx2.IsSupported)
        {
            var shuffle = Vector128.Create((byte)3, 3, 3, 3, 7, 7, 7, 7, 11, 11, 11, 11, 15, 15, 15, 15);
            var alphaIndices = Vector256.Create(shuffle, shuffle);
            var alphaMask = Vector256.Create(0xFF000000u).AsByte();
            var zero = Vector256<byte>.Zero;
            for (; i <= size - Vector256<byte>.Count; i += Vector256<byte>.Count)
            {
                var bgra = Vector256.Load(pixels + i);
                var alpha = Avx2.Shuffle(bgra, alphaIndices);
                var low = ScaleByAlpha(Avx2.UnpackLow(bgra, zero).AsUInt16(), Avx2.UnpackLow(alpha, zero).AsUInt16());
                var high = ScaleByAlpha(Avx2.UnpackHigh(bgra, zero).AsUInt16(), Avx2.UnpackHigh(alpha, zero).AsUInt16());
                var scaled = Avx2.PackUnsignedSaturate(low.AsInt16(), high.AsInt16());
                Avx2.BlendVariable(scaled, bgra, alphaMask).Store(pixels + i);
            }
        }
        for (; i < size; i += 4)
        {
            int a = pixels[i + 3];
            pixels[i] = (byte)((pixels[i] * a + 127) / 255);
            pixels[i + 1] = (byte)((pixels[i + 1] * a + 127) / 255);
            pixels[i + 2] = (byte)((pixels[i + 2] * a + 127) / 255);
        }
    }

    private static Vector256<ushort> ScaleByAlpha(Vector256<ushort> color, Vector256<ushort> alpha)
    {
        // Dzielenie z zaokrągleniem przez 255 w zakresie iloczynu dwóch bajtów.
        var biased = Avx2.Add(Avx2.MultiplyLow(color, alpha), Vector256.Create((ushort)128));
        return Avx2.ShiftRightLogical(Avx2.Add(biased, Avx2.ShiftRightLogical(biased, 8)), 8);
    }

    private void PushAudio(SwrContext* swr, AVFrame* f, ref float[] buf)
    {
        int maxOut = (int)ffmpeg.av_rescale_rnd(ffmpeg.swr_get_delay(swr, f->sample_rate) + f->nb_samples,
            AudioEngine.SampleRate, f->sample_rate, AVRounding.AV_ROUND_UP);
        if (buf.Length < maxOut * 2) buf = new float[maxOut * 2];
        int got;
        fixed (float* p = buf)
        {
            byte* outp = (byte*)p;
            got = ffmpeg.swr_convert(swr, &outp, maxOut, f->extended_data, f->nb_samples);
        }
        if (got > 0) _audioSink!(buf, got * 2);
    }

    public void Dispose()
    {
        _run = false;
        _thread.Join(3000);
    }
}
