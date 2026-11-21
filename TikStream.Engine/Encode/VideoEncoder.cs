using FFmpeg.AutoGen;
using TikStream.Capture;
using TikStream.Core;
using TikStream.Gpu;
using D3D = Vortice.Direct3D11;
using DXGIFormat = Vortice.DXGI.Format;

namespace TikStream.Encode;

/// <summary>
/// H.264 via NVENC, input: NV12 textures from FFmpeg's D3D11 pool — the compositor renders straight into them.
/// Frames never leave VRAM.
/// </summary>
public sealed record VideoEncoderOptions(int Fps, int Kbps, string Preset, string Profile, int KeyframeSec, int BFrames, bool SpatialAq, bool LookAhead, bool ZeroFpsImpact = false, string Codec = "h264");

public sealed unsafe class VideoEncoder : IDisposable
{
    private readonly D3D.ID3D11Device _dev;
    private AVBufferRef* _hwDevice;
    private AVBufferRef* _hwFrames;
    private AVCodecContext* _ctx;
    private readonly Dictionary<(IntPtr, int), (D3D.ID3D11Texture2D Tex, D3D.ID3D11RenderTargetView Y, D3D.ID3D11RenderTargetView UV)> _views = new();

    public AVCodecContext* Context => _ctx;
    public AVRational TimeBase => _ctx->time_base;
    public string CodecName { get; }

    public VideoEncoder(GpuDevice gpu, int width, int height, VideoEncoderOptions o, bool globalHeader)
    {
        _dev = gpu.Device;
        int fps = o.Fps, kbps = o.Kbps;
        string preset = o.Preset;

        // --- FFmpeg hw device = OUR D3D11 device (same one we render on) ---
        _hwDevice = ffmpeg.av_hwdevice_ctx_alloc(AVHWDeviceType.AV_HWDEVICE_TYPE_D3D11VA);
        var devCtx = (AVHWDeviceContext*)_hwDevice->data;
        var d3dCtx = (AVD3D11VADeviceContext*)devCtx->hwctx;
        gpu.Device.AddRef(); // FFmpeg will release this reference on cleanup
        d3dCtx->device = (FFmpeg.AutoGen.ID3D11Device*)gpu.Device.NativePointer;
        FF.Check(ffmpeg.av_hwdevice_ctx_init(_hwDevice), "av_hwdevice_ctx_init");

        _hwFrames = ffmpeg.av_hwframe_ctx_alloc(_hwDevice);
        var fc = (AVHWFramesContext*)_hwFrames->data;
        fc->format = AVPixelFormat.AV_PIX_FMT_D3D11;
        fc->sw_format = AVPixelFormat.AV_PIX_FMT_NV12;
        fc->width = width;
        fc->height = height;
        fc->initial_pool_size = 0; // individual textures allocated on demand and reused by the pool
        ((AVD3D11VAFramesContext*)fc->hwctx)->BindFlags = (uint)D3D.BindFlags.RenderTarget;
        FF.Check(ffmpeg.av_hwframe_ctx_init(_hwFrames), "av_hwframe_ctx_init");

        string wantedName = o.Codec switch
        {
            "hevc" => "hevc_nvenc",
            "av1" => "av1_nvenc",
            _ => "h264_nvenc",
        };
        var codec = ffmpeg.avcodec_find_encoder_by_name(wantedName);
        if (codec == null)
        {
            Log.Warn($"Encoder '{wantedName}' not available in FFmpeg, falling back to h264_nvenc.");
            wantedName = "h264_nvenc";
            codec = ffmpeg.avcodec_find_encoder_by_name(wantedName);
        }
        if (codec == null) throw new InvalidOperationException("h264_nvenc encoder not found in FFmpeg.");
        CodecName = wantedName;

        _ctx = ffmpeg.avcodec_alloc_context3(codec);
        _ctx->width = width;
        _ctx->height = height;
        _ctx->time_base = new AVRational { num = 1, den = fps };
        _ctx->framerate = new AVRational { num = fps, den = 1 };
        _ctx->pix_fmt = AVPixelFormat.AV_PIX_FMT_D3D11;
        _ctx->hw_frames_ctx = ffmpeg.av_buffer_ref(_hwFrames);
        _ctx->gop_size = fps * Math.Clamp(o.KeyframeSec, 1, 10);
        _ctx->max_b_frames = wantedName == "av1_nvenc" ? 0 : Math.Clamp(o.BFrames, 0, 4);
        _ctx->bit_rate = kbps * 1000L;
        _ctx->rc_max_rate = kbps * 1000L;
        _ctx->rc_buffer_size = kbps * 1000;
        if (wantedName == "h264_nvenc") _ctx->profile = o.Profile == "main" ? 77 : 100;
        _ctx->color_range = AVColorRange.AVCOL_RANGE_MPEG;
        _ctx->colorspace = AVColorSpace.AVCOL_SPC_BT709;
        _ctx->color_primaries = AVColorPrimaries.AVCOL_PRI_BT709;
        _ctx->color_trc = AVColorTransferCharacteristic.AVCOL_TRC_BT709;
        _ctx->sample_aspect_ratio = new AVRational { num = 1, den = 1 };
        if (globalHeader) _ctx->flags |= ffmpeg.AV_CODEC_FLAG_GLOBAL_HEADER;

        AVDictionary* opts = null;
        string effPreset = o.ZeroFpsImpact && preset is "p5" or "p6" or "p7" ? "p4" : preset;
        bool effAq = !o.ZeroFpsImpact && o.SpatialAq;
        bool effLookAhead = !o.ZeroFpsImpact && o.LookAhead;
        ffmpeg.av_dict_set(&opts, "preset", effPreset, 0);
        ffmpeg.av_dict_set(&opts, "tune", o.ZeroFpsImpact ? "ll" : "hq", 0);
        if (o.ZeroFpsImpact)
        {
            ffmpeg.av_dict_set(&opts, "multipass", "disabled", 0);
            ffmpeg.av_dict_set(&opts, "temporal-aq", "0", 0);
        }
        if (wantedName == "h264_nvenc") ffmpeg.av_dict_set(&opts, "profile", o.Profile == "main" ? "main" : "high", 0);
        else if (wantedName == "hevc_nvenc") ffmpeg.av_dict_set(&opts, "profile", "main", 0);
        ffmpeg.av_dict_set(&opts, "rc", "cbr", 0);
        ffmpeg.av_dict_set(&opts, "spatial-aq", effAq ? "1" : "0", 0);
        if (effLookAhead) ffmpeg.av_dict_set(&opts, "rc-lookahead", "20", 0);
        if (wantedName != "av1_nvenc")
        {
            if (_ctx->max_b_frames == 0) ffmpeg.av_dict_set(&opts, "b_ref_mode", "disabled", 0);
            else ffmpeg.av_dict_set(&opts, "b_ref_mode", "middle", 0);
        }
        ffmpeg.av_dict_set(&opts, "forced-idr", "1", 0);
        int r = ffmpeg.avcodec_open2(_ctx, codec, &opts);
        if (r < 0)
        {
            Log.Warn($"{wantedName} failed to start ({FF.Err(r)}), retrying with simplified options.");
            ffmpeg.av_dict_free(&opts);
            _ctx->max_b_frames = 0;
            ffmpeg.av_dict_set(&opts, "preset", effPreset, 0);
            ffmpeg.av_dict_set(&opts, "rc", "cbr", 0);
            ffmpeg.av_dict_set(&opts, "forced-idr", "1", 0);
            r = ffmpeg.avcodec_open2(_ctx, codec, &opts);
        }
        ffmpeg.av_dict_free(&opts);
        FF.Check(r, $"avcodec_open2({wantedName})");
        Log.Info($"NVENC ({wantedName}): {width}x{height}@{fps}, {kbps} kbps CBR, preset {effPreset}, keyframe {o.KeyframeSec}s, B-frames {_ctx->max_b_frames}, AQ {effAq}, look-ahead {effLookAhead}{(o.ZeroFpsImpact ? " [Zero FPS Impact]" : "")}");
    }

    /// <summary>Acquires a free texture from the pool and returns Y and UV render target views.</summary>
    public AVFrame* AcquireFrame(out D3D.ID3D11RenderTargetView yRtv, out D3D.ID3D11RenderTargetView uvRtv)
    {
        var f = ffmpeg.av_frame_alloc();
        int r = ffmpeg.av_hwframe_get_buffer(_hwFrames, f, 0);
        if (r < 0) { ffmpeg.av_frame_free(&f); throw new InvalidOperationException("av_hwframe_get_buffer: " + FF.Err(r)); }
        var texPtr = (IntPtr)f->data[0];
        int index = (int)(long)f->data[1];
        if (!_views.TryGetValue((texPtr, index), out var v))
        {
            var tex = new D3D.ID3D11Texture2D(texPtr);
            tex.AddRef(); // our wrapper holds its own reference
            var arr = tex.Description.ArraySize > 1;
            D3D.RenderTargetViewDescription Desc(DXGIFormat fmt) => arr
                ? new D3D.RenderTargetViewDescription(D3D.RenderTargetViewDimension.Texture2DArray, fmt, 0, (uint)index, 1)
                : new D3D.RenderTargetViewDescription(D3D.RenderTargetViewDimension.Texture2D, fmt);
            var y = _dev.CreateRenderTargetView(tex, Desc(DXGIFormat.R8_UNorm));
            var uv = _dev.CreateRenderTargetView(tex, Desc(DXGIFormat.R8G8_UNorm));
            v = (tex, y, uv);
            _views[(texPtr, index)] = v;
            if (_views.Count > 32) Log.Warn($"NVENC texture pool grew to {_views.Count} textures");
        }
        yRtv = v.Y;
        uvRtv = v.UV;
        return f;
    }

    /// <summary>Submits a frame to the encoder (takes ownership) and drains ready packets.</summary>
    public void Encode(AVFrame* frame, long pts, bool forceKeyframe, Action<IntPtr> onPacket)
    {
        if (frame != null)
        {
            frame->pts = pts;
            frame->pict_type = forceKeyframe ? AVPictureType.AV_PICTURE_TYPE_I : AVPictureType.AV_PICTURE_TYPE_NONE;
            if (forceKeyframe) frame->flags |= ffmpeg.AV_FRAME_FLAG_KEY;
        }
        int r = ffmpeg.avcodec_send_frame(_ctx, frame);
        if (frame != null) ffmpeg.av_frame_free(&frame);
        if (r < 0 && r != ffmpeg.AVERROR_EOF) { Log.Warn("NVENC send_frame: " + FF.Err(r)); return; }
        Drain(onPacket);
    }

    private void Drain(Action<IntPtr> onPacket)
    {
        while (true)
        {
            var pkt = ffmpeg.av_packet_alloc();
            int r = ffmpeg.avcodec_receive_packet(_ctx, pkt);
            if (r < 0) { ffmpeg.av_packet_free(&pkt); return; }
            onPacket((IntPtr)pkt);
        }
    }

    public void Flush(Action<IntPtr> onPacket) => Encode(null, 0, false, onPacket);

    public void Dispose()
    {
        foreach (var v in _views.Values) { v.Y.Dispose(); v.UV.Dispose(); v.Tex.Dispose(); }
        _views.Clear();
        fixed (AVCodecContext** c = &_ctx) ffmpeg.avcodec_free_context(c);
        fixed (AVBufferRef** f = &_hwFrames) ffmpeg.av_buffer_unref(f);
        fixed (AVBufferRef** d = &_hwDevice) ffmpeg.av_buffer_unref(d);
    }
}
