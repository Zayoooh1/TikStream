using System.Collections.Concurrent;
using FFmpeg.AutoGen;
using TikStream.Capture;
using TikStream.Core;

namespace TikStream.Encode;

/// <summary>
/// Single output (RTMP stream or local file) with its own thread and packet queue.
/// Network I/O never blocks rendering: on congestion, packets are dropped until the next keyframe;
/// on disconnect, it reconnects automatically and resumes from a fresh keyframe.
/// </summary>
public sealed record OutputOptions(bool Reconnect, int DelaySec, int MaxRetries);

public sealed unsafe class StreamOutput : IDisposable
{
    public const int VideoIndex = 0, AudioIndex = 1;

    private readonly string _url;
    private readonly string _name;
    private readonly bool _isNetwork;
    private readonly bool _reconnect;
    private readonly OutputOptions _opt;
    private int _attempts; // consecutive failed attempts since last successful connection
    private readonly AVCodecParameters* _vpar;
    private readonly AVCodecParameters* _apar;
    private readonly AVRational _vtb, _atb;
    private readonly List<(string Title, IntPtr Par, AVRational Tb)> _extraAudio = new();
    private readonly int _fps;
    private readonly Action _requestKeyframe;

    private readonly BlockingCollection<IntPtr> _queue = new(new ConcurrentQueue<IntPtr>());
    private readonly Thread _thread;
    private volatile bool _running = true;
    private volatile bool _abort; // hard I/O abort — only when shutdown takes too long
    private int _queuedVideo;

    private AVFormatContext* _oc;
    private AVIOInterruptCB_callback? _interrupt;

    public string Name => _name;
    public string Status { get; private set; } = "Connecting…";
    public bool Connected { get; private set; }
    public bool Failed { get; private set; }
    public long BytesWritten { get; private set; }
    public long DroppedPackets { get; private set; }
    public int Reconnects { get; private set; }
    public int QueueLength => _queue.Count;

    public StreamOutput(string name, string url, VideoEncoder venc, AudioEncoder aenc, int fps, OutputOptions opt, Action requestKeyframe,
        IReadOnlyList<(string Title, AudioEncoder Enc)>? extraAudioTracks = null)
    {
        _name = name;
        _url = url;
        _isNetwork = url.StartsWith("rtmp", StringComparison.OrdinalIgnoreCase) || url.StartsWith("srt", StringComparison.OrdinalIgnoreCase);
        _opt = opt;
        _reconnect = opt.Reconnect && _isNetwork;
        _fps = fps;
        _requestKeyframe = requestKeyframe;
        _vpar = ffmpeg.avcodec_parameters_alloc();
        _apar = ffmpeg.avcodec_parameters_alloc();
        FF.Check(ffmpeg.avcodec_parameters_from_context(_vpar, venc.Context), "params(video)");
        FF.Check(ffmpeg.avcodec_parameters_from_context(_apar, aenc.Context), "params(audio)");
        _vtb = venc.TimeBase;
        _atb = aenc.TimeBase;
        if (extraAudioTracks != null)
        {
            foreach (var (title, enc) in extraAudioTracks)
            {
                var ep = ffmpeg.avcodec_parameters_alloc();
                FF.Check(ffmpeg.avcodec_parameters_from_context(ep, enc.Context), "params(extra audio)");
                _extraAudio.Add((title, (IntPtr)ep, enc.TimeBase));
            }
        }
        _thread = new Thread(Run) { IsBackground = true, Name = "Output:" + name, Priority = ThreadPriority.AboveNormal };
        _thread.Start();
    }

    /// <summary>Queues a reference-counted copy of a packet. Caller retains ownership of the original.</summary>
    public void Enqueue(AVPacket* pkt)
    {
        if (!_running) return;
        var clone = ffmpeg.av_packet_clone(pkt);
        if (clone == null) return;
        if (clone->stream_index == VideoIndex) Interlocked.Increment(ref _queuedVideo);
        try { _queue.Add((IntPtr)clone); }
        catch (InvalidOperationException) { ffmpeg.av_packet_free(&clone); } // output already closing
    }

    private bool Open()
    {
        AVFormatContext* oc = null;
        string? fmtName = _isNetwork ? (_url.StartsWith("srt", StringComparison.OrdinalIgnoreCase) ? "mpegts" : "flv") : null;
        int r = ffmpeg.avformat_alloc_output_context2(&oc, null, fmtName, _url);
        if (r < 0 || oc == null) { Status = "Format error: " + FF.Err(r); return false; }

        _interrupt = _ => _abort ? 1 : 0; // abort stalled connect/write
        oc->interrupt_callback.callback = _interrupt;

        var vs = ffmpeg.avformat_new_stream(oc, null);
        ffmpeg.avcodec_parameters_copy(vs->codecpar, _vpar);
        vs->codecpar->codec_tag = 0;
        vs->time_base = _vtb;
        vs->avg_frame_rate = new AVRational { num = _fps, den = 1 };
        var aus = ffmpeg.avformat_new_stream(oc, null);
        ffmpeg.avcodec_parameters_copy(aus->codecpar, _apar);
        aus->codecpar->codec_tag = 0;
        aus->time_base = _atb;
        if (!_isNetwork && _extraAudio.Count > 0)
            ffmpeg.av_dict_set(&aus->metadata, "title", "Full Mix", 0);
        foreach (var (title, epPtr, tb) in _extraAudio)
        {
            var es = ffmpeg.avformat_new_stream(oc, null);
            ffmpeg.avcodec_parameters_copy(es->codecpar, (AVCodecParameters*)epPtr);
            es->codecpar->codec_tag = 0;
            es->time_base = tb;
            ffmpeg.av_dict_set(&es->metadata, "title", title, 0);
        }

        if ((oc->oformat->flags & ffmpeg.AVFMT_NOFILE) == 0)
        {
            AVDictionary* o = null;
            if (_isNetwork)
            {
                ffmpeg.av_dict_set(&o, "rw_timeout", "15000000", 0); // 15 s without progress = disconnected
                ffmpeg.av_dict_set(&o, "tcp_nodelay", "1", 0);
            }
            r = ffmpeg.avio_open2(&oc->pb, _url, ffmpeg.AVIO_FLAG_WRITE, &oc->interrupt_callback, &o);
            ffmpeg.av_dict_free(&o);
            if (r < 0)
            {
                Status = "Cannot connect: " + FF.Err(r);
                ffmpeg.avformat_free_context(oc);
                return false;
            }
        }

        AVDictionary* mo = null;
        if (!_isNetwork) ffmpeg.av_dict_set(&mo, "flush_packets", "1", 0);
        // Fragmented MP4: remains playable even if the app crashes
        if (_url.EndsWith(".mp4", StringComparison.OrdinalIgnoreCase)) ffmpeg.av_dict_set(&mo, "movflags", "+frag_keyframe+empty_moov+default_base_moof", 0);
        r = ffmpeg.avformat_write_header(oc, &mo);
        ffmpeg.av_dict_free(&mo);
        if (r < 0)
        {
            Status = "Header error: " + FF.Err(r);
            ffmpeg.avio_closep(&oc->pb);
            ffmpeg.avformat_free_context(oc);
            return false;
        }
        _oc = oc;
        Connected = true;
        Status = _isNetwork ? "Live" : "Recording";
        Log.Info($"Output '{_name}' opened ({(_isNetwork ? "network" : _url)})");
        return true;
    }

    private void Close(bool writeTrailer)
    {
        if (_oc == null) return;
        var oc = _oc;
        _oc = null;
        Connected = false;
        if (writeTrailer) ffmpeg.av_write_trailer(oc);
        if ((oc->oformat->flags & ffmpeg.AVFMT_NOFILE) == 0) ffmpeg.avio_closep(&oc->pb);
        ffmpeg.avformat_free_context(oc);
    }

    private void Run()
    {
        bool waitKey = true;
        long offsetUs = 0;
        var us = new AVRational { num = 1, den = 1_000_000 };
        var nextAttempt = DateTime.MinValue;

        while (_running || _queue.Count > 0)
        {
            if (_oc == null)
            {
                if (!_running) break;
                if (DateTime.UtcNow >= nextAttempt)
                {
                    if (Open())
                    {
                        waitKey = true; _attempts = 0;
                        _requestKeyframe();
                    }
                    else
                    {
                        Log.Warn($"Output '{_name}': {Status}");
                        if (!_reconnect || (_opt.MaxRetries > 0 && _attempts >= _opt.MaxRetries)) { Failed = true; break; }
                        Reconnects++; _attempts++;
                        nextAttempt = DateTime.UtcNow.AddSeconds(Math.Max(1, _opt.DelaySec));
                        Status += $" — attempt {_attempts}/{_opt.MaxRetries}";
                    }
                }
                // disconnected: drain queue so memory does not grow
                while (_queue.TryTake(out var junk)) FreePacket(junk, countDrop: true);
                Thread.Sleep(100);
                continue;
            }

            if (!_queue.TryTake(out var p, 200)) continue;
            var pkt = (AVPacket*)p;
            bool isVideo = pkt->stream_index == VideoIndex;
            if (isVideo) Interlocked.Decrement(ref _queuedVideo);

            // Network congestion: >3 s of video queued -> drop backlog and wait for fresh keyframe
            if (_isNetwork && Volatile.Read(ref _queuedVideo) > _fps * 3)
            {
                Log.Warn($"Output '{_name}': network congestion, dropping queued frames");
                FreePacket(p, true);
                while (_queue.TryTake(out var junk)) FreePacket(junk, true);
                waitKey = true;
                _requestKeyframe();
                continue;
            }

            if (waitKey)
            {
                if (isVideo && (pkt->flags & ffmpeg.AV_PKT_FLAG_KEY) != 0)
                {
                    offsetUs = ffmpeg.av_rescale_q(pkt->dts, _vtb, us);
                    waitKey = false;
                }
                else { FreePacket(p, false); continue; }
            }

            if (pkt->stream_index < 0 || pkt->stream_index >= (int)_oc->nb_streams) { FreePacket(p, false); continue; }
            var srcTb = isVideo ? _vtb
                : pkt->stream_index == AudioIndex ? _atb
                : _extraAudio[pkt->stream_index - AudioIndex - 1].Tb;
            long off = ffmpeg.av_rescale_q(offsetUs, us, srcTb);
            pkt->pts -= off;
            pkt->dts -= off;
            if (pkt->dts < 0) { FreePacket(p, false); continue; }

            var st = _oc->streams[pkt->stream_index];
            ffmpeg.av_packet_rescale_ts(pkt, srcTb, st->time_base);
            int size = pkt->size;
            int r = ffmpeg.av_interleaved_write_frame(_oc, pkt);
            ffmpeg.av_packet_free(&pkt);
            if (r < 0)
            {
                Log.Warn($"Output '{_name}': write error {FF.Err(r)}");
                Status = "Disconnected: " + FF.Err(r);
                Close(false);
                if (!_running) break;
                if (!_reconnect) { Failed = true; break; }
                Reconnects++;
                nextAttempt = DateTime.UtcNow.AddSeconds(Math.Max(1, _opt.DelaySec));
                continue;
            }
            BytesWritten += size;
        }

        Close(true);
        while (_queue.TryTake(out var rest)) FreePacket(rest, false);
        if (!Failed) Status = "Stopped";
        Log.Info($"Output '{_name}' closed. Sent {BytesWritten / 1024 / 1024} MB, dropped {DroppedPackets} packets.");
    }

    private void FreePacket(IntPtr p, bool countDrop)
    {
        var pkt = (AVPacket*)p;
        if (countDrop) DroppedPackets++;
        ffmpeg.av_packet_free(&pkt);
    }

    public void Dispose()
    {
        _running = false;
        _queue.CompleteAdding();
        if (!_thread.Join(_isNetwork ? 4000 : 15000))
        {
            _abort = true;
            if (!_thread.Join(4000)) Log.Warn($"Output '{_name}' did not close in time.");
        }
        var v = _vpar; ffmpeg.avcodec_parameters_free(&v);
        var a = _apar; ffmpeg.avcodec_parameters_free(&a);
        foreach (var (_, epPtr, _) in _extraAudio)
        {
            var ep = (AVCodecParameters*)epPtr;
            ffmpeg.avcodec_parameters_free(&ep);
        }
        _extraAudio.Clear();
    }
}
