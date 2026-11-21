using FFmpeg.AutoGen;
using TikStream.Capture;
using TikStream.Core;

namespace TikStream.Encode;

/// <summary>
/// In-memory rolling replay buffer holding the last N seconds of encoded NVENC video + AAC audio packets.
/// Zero extra encoding overhead; saving snapshots packet references under lock and muxes on a background thread.
/// </summary>
public sealed unsafe class ReplayBuffer : IDisposable
{
    private readonly AVCodecParameters* _vpar;
    private readonly AVCodecParameters* _apar;
    private readonly AVRational _vtb, _atb;
    private readonly int _fps;
    private readonly long _maxDurationUs;
    private readonly object _lock = new();
    private readonly List<(IntPtr Pkt, bool IsVideo, bool IsKey, long DtsUs)> _ring = new(4096);
    private bool _disposed;

    public int DurationSec { get; }

    public ReplayBuffer(VideoEncoder venc, AudioEncoder aenc, int fps, int durationSec)
    {
        _fps = Math.Max(1, fps);
        DurationSec = Math.Clamp(durationSec, 5, 300);
        _maxDurationUs = DurationSec * 1_000_000L;
        _vpar = ffmpeg.avcodec_parameters_alloc();
        _apar = ffmpeg.avcodec_parameters_alloc();
        FF.Check(ffmpeg.avcodec_parameters_from_context(_vpar, venc.Context), "replay params(video)");
        FF.Check(ffmpeg.avcodec_parameters_from_context(_apar, aenc.Context), "replay params(audio)");
        _vtb = venc.TimeBase;
        _atb = aenc.TimeBase;
        Log.Info($"Replay buffer started ({DurationSec} s).");
    }

    public void Enqueue(AVPacket* pkt, int streamIndex)
    {
        if (streamIndex != StreamOutput.VideoIndex && streamIndex != StreamOutput.AudioIndex) return;
        var clone = ffmpeg.av_packet_clone(pkt);
        if (clone == null) return;
        clone->stream_index = streamIndex;

        bool isVideo = streamIndex == StreamOutput.VideoIndex;
        bool isKey = isVideo && (clone->flags & ffmpeg.AV_PKT_FLAG_KEY) != 0;
        var us = new AVRational { num = 1, den = 1_000_000 };
        var tb = isVideo ? _vtb : _atb;
        long dtsUs = ffmpeg.av_rescale_q(clone->dts != ffmpeg.AV_NOPTS_VALUE ? clone->dts : clone->pts, tb, us);

        lock (_lock)
        {
            if (_disposed) { ffmpeg.av_packet_free(&clone); return; }
            _ring.Add(((IntPtr)clone, isVideo, isKey, dtsUs));
            TrimLocked(dtsUs);
        }
    }

    private void TrimLocked(long latestUs)
    {
        long cutoff = latestUs - _maxDurationUs;
        if (_ring.Count < 32 || _ring[0].DtsUs >= cutoff) return;

        // Find the latest video keyframe at or before cutoff so we always keep at least DurationSec starting on a keyframe.
        int dropBefore = 0;
        for (int i = 1; i < _ring.Count; i++)
        {
            if (_ring[i].IsKey && _ring[i].DtsUs <= cutoff)
                dropBefore = i;
            else if (_ring[i].DtsUs > cutoff)
                break;
        }
        if (dropBefore <= 0) return;

        for (int i = 0; i < dropBefore; i++)
        {
            var p = (AVPacket*)_ring[i].Pkt;
            ffmpeg.av_packet_free(&p);
        }
        _ring.RemoveRange(0, dropBefore);
    }

    /// <summary>Snapshots buffered packets and writes them to <paramref name="path"/> on a background thread.</summary>
    public Task<string> SaveAsync(string path)
    {
        List<(IntPtr Pkt, bool IsVideo)> snapshot;
        long offsetUs;

        lock (_lock)
        {
            if (_disposed) throw new InvalidOperationException("Replay buffer is not running.");
            int start = _ring.FindIndex(e => e.IsKey);
            if (start < 0) throw new InvalidOperationException("Replay buffer does not have a complete keyframe yet — wait a moment and try again.");
            offsetUs = _ring[start].DtsUs;
            snapshot = new List<(IntPtr, bool)>(_ring.Count - start);
            for (int i = start; i < _ring.Count; i++)
            {
                var c = ffmpeg.av_packet_clone((AVPacket*)_ring[i].Pkt);
                if (c != null) snapshot.Add(((IntPtr)c, _ring[i].IsVideo));
            }
        }

        // Copy codec parameters for background muxing so Dispose() cannot free them mid-save.
        var vpar = ffmpeg.avcodec_parameters_alloc();
        var apar = ffmpeg.avcodec_parameters_alloc();
        ffmpeg.avcodec_parameters_copy(vpar, _vpar);
        ffmpeg.avcodec_parameters_copy(apar, _apar);
        var vtb = _vtb;
        var atb = _atb;
        int fps = _fps;

        return Task.Run(() =>
        {
            try
            {
                WriteFile(path, snapshot, offsetUs, vpar, apar, vtb, atb, fps);
                Log.Info($"Replay saved: {path} ({snapshot.Count} packets)");
                return path;
            }
            finally
            {
                foreach (var (ptr, _) in snapshot)
                {
                    var p = (AVPacket*)ptr;
                    ffmpeg.av_packet_free(&p);
                }
                var vp = vpar;
                var ap = apar;
                ffmpeg.avcodec_parameters_free(&vp);
                ffmpeg.avcodec_parameters_free(&ap);
            }
        });
    }

    private static void WriteFile(string path, List<(IntPtr Pkt, bool IsVideo)> packets, long offsetUs,
        AVCodecParameters* vpar, AVCodecParameters* apar, AVRational vtb, AVRational atb, int fps)
    {
        Directory.CreateDirectory(Path.GetDirectoryName(path)!);
        AVFormatContext* oc = null;
        FF.Check(ffmpeg.avformat_alloc_output_context2(&oc, null, null, path), "replay alloc_output");
        try
        {
            var vs = ffmpeg.avformat_new_stream(oc, null);
            ffmpeg.avcodec_parameters_copy(vs->codecpar, vpar);
            vs->codecpar->codec_tag = 0;
            vs->time_base = vtb;
            vs->avg_frame_rate = new AVRational { num = fps, den = 1 };

            var aus = ffmpeg.avformat_new_stream(oc, null);
            ffmpeg.avcodec_parameters_copy(aus->codecpar, apar);
            aus->codecpar->codec_tag = 0;
            aus->time_base = atb;

            if ((oc->oformat->flags & ffmpeg.AVFMT_NOFILE) == 0)
                FF.Check(ffmpeg.avio_open(&oc->pb, path, ffmpeg.AVIO_FLAG_WRITE), "replay avio_open");

            AVDictionary* mo = null;
            if (path.EndsWith(".mp4", StringComparison.OrdinalIgnoreCase))
                ffmpeg.av_dict_set(&mo, "movflags", "+frag_keyframe+empty_moov+default_base_moof", 0);
            int hr = ffmpeg.avformat_write_header(oc, &mo);
            ffmpeg.av_dict_free(&mo);
            FF.Check(hr, "replay write_header");

            var us = new AVRational { num = 1, den = 1_000_000 };
            for (int i = 0; i < packets.Count; i++)
            {
                var pkt = (AVPacket*)packets[i].Pkt;
                var srcTb = packets[i].IsVideo ? vtb : atb;
                long off = ffmpeg.av_rescale_q(offsetUs, us, srcTb);
                if (pkt->pts != ffmpeg.AV_NOPTS_VALUE) pkt->pts -= off;
                if (pkt->dts != ffmpeg.AV_NOPTS_VALUE) pkt->dts -= off;
                if (pkt->dts != ffmpeg.AV_NOPTS_VALUE && pkt->dts < 0) continue;
                var st = oc->streams[pkt->stream_index];
                ffmpeg.av_packet_rescale_ts(pkt, srcTb, st->time_base);
                ffmpeg.av_interleaved_write_frame(oc, pkt);
            }

            ffmpeg.av_write_trailer(oc);
        }
        finally
        {
            if (oc != null)
            {
                if ((oc->oformat->flags & ffmpeg.AVFMT_NOFILE) == 0 && oc->pb != null)
                    ffmpeg.avio_closep(&oc->pb);
                ffmpeg.avformat_free_context(oc);
            }
        }
    }

    public void Dispose()
    {
        lock (_lock)
        {
            if (_disposed) return;
            _disposed = true;
            foreach (var e in _ring)
            {
                var p = (AVPacket*)e.Pkt;
                ffmpeg.av_packet_free(&p);
            }
            _ring.Clear();
        }
        var v = _vpar; ffmpeg.avcodec_parameters_free(&v);
        var a = _apar; ffmpeg.avcodec_parameters_free(&a);
        Log.Info("Replay buffer stopped.");
    }
}
