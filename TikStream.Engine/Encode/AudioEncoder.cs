using FFmpeg.AutoGen;
using TikStream.Capture;

namespace TikStream.Encode;

/// <summary>AAC-LC 48 kHz stereo (TikTok/RTMP requirement).</summary>
public sealed unsafe class AudioEncoder : IDisposable
{
    private AVCodecContext* _ctx;
    private AVFrame* _frame;
    public AVCodecContext* Context => _ctx;
    public AVRational TimeBase => _ctx->time_base;
    public int FrameSize { get; }

    public AudioEncoder(int kbps, bool globalHeader)
    {
        var codec = ffmpeg.avcodec_find_encoder(AVCodecID.AV_CODEC_ID_AAC);
        if (codec == null) throw new InvalidOperationException("AAC encoder not found.");
        _ctx = ffmpeg.avcodec_alloc_context3(codec);
        _ctx->sample_fmt = AVSampleFormat.AV_SAMPLE_FMT_FLTP;
        _ctx->sample_rate = AudioEngine.SampleRate;
        AVChannelLayout layout;
        ffmpeg.av_channel_layout_default(&layout, AudioEngine.Channels);
        _ctx->ch_layout = layout;
        _ctx->bit_rate = kbps * 1000L;
        _ctx->time_base = new AVRational { num = 1, den = AudioEngine.SampleRate };
        if (globalHeader) _ctx->flags |= ffmpeg.AV_CODEC_FLAG_GLOBAL_HEADER;
        FF.Check(ffmpeg.avcodec_open2(_ctx, codec, null), "avcodec_open2(aac)");
        FrameSize = _ctx->frame_size > 0 ? _ctx->frame_size : 1024;

        _frame = ffmpeg.av_frame_alloc();
        _frame->nb_samples = FrameSize;
        _frame->format = (int)AVSampleFormat.AV_SAMPLE_FMT_FLTP;
        _frame->sample_rate = AudioEngine.SampleRate;
        ffmpeg.av_channel_layout_copy(&_frame->ch_layout, &_ctx->ch_layout);
        FF.Check(ffmpeg.av_frame_get_buffer(_frame, 0), "av_frame_get_buffer(audio)");
    }

    /// <summary>Encodes exactly <see cref="FrameSize"/> frames from an interleaved buffer.</summary>
    public void Encode(float[] interleaved, long pts, Action<IntPtr> onPacket)
    {
        FF.Check(ffmpeg.av_frame_make_writable(_frame), "av_frame_make_writable");
        var l = (float*)_frame->data[0];
        var r = (float*)_frame->data[1];
        for (int i = 0; i < FrameSize; i++)
        {
            l[i] = interleaved[i * 2];
            r[i] = interleaved[i * 2 + 1];
        }
        _frame->pts = pts;
        if (ffmpeg.avcodec_send_frame(_ctx, _frame) < 0) return;
        Drain(onPacket);
    }

    private void Drain(Action<IntPtr> onPacket)
    {
        while (true)
        {
            var pkt = ffmpeg.av_packet_alloc();
            if (ffmpeg.avcodec_receive_packet(_ctx, pkt) < 0) { ffmpeg.av_packet_free(&pkt); return; }
            onPacket((IntPtr)pkt);
        }
    }

    public void Flush(Action<IntPtr> onPacket)
    {
        ffmpeg.avcodec_send_frame(_ctx, null);
        Drain(onPacket);
    }

    public void Dispose()
    {
        fixed (AVFrame** f = &_frame) ffmpeg.av_frame_free(f);
        fixed (AVCodecContext** c = &_ctx) ffmpeg.avcodec_free_context(c);
    }
}
