using System.Numerics;
using System.Runtime.InteropServices;
using TikStream.Core;
using Vortice.D3DCompiler;
using Vortice.Direct3D;
using Vortice.Direct3D11;
using Vortice.DXGI;
using Vortice.Mathematics;

namespace TikStream.Gpu;

public enum DrawMode { Normal, Blur }

public struct DrawCmd
{
    public ID3D11ShaderResourceView Srv;
    public Vector4 SrcUv;   // u0, v0, u1, v1 (u0>u1 = mirror)
    public NRect Dst;       // normalized destination on canvas
    public float Opacity;
    public float RadiusPx;
    public bool Opaque;     // ignore source alpha channel (games/webcams may leave garbage in alpha)
    public float Darken;    // brightness multiplier (1 = unchanged)
    public bool Yuv;        // Srv = Y plane (R8), Srv2 = UV plane (R8G8) — NV12 camera converted in shader
    public ID3D11ShaderResourceView? Srv2;
    public float Brightness;       // brightness delta (-0.5..+0.5, 0 = unchanged)
    public float ContrastDelta;    // contrast = 1 + ContrastDelta (0 = 1.0)
    public float SaturationDelta;  // saturation = 1 + SaturationDelta (0 = 1.0)
    public bool ChromaKeyEnabled;
    public Vector3 ChromaKeyRgb;   // key color (0..1)
    public float ChromaKeySimilarity;
    public float ChromaKeySmoothness;
    public bool Hdr;               // scRGB R16G16B16A16_Float texture -> HDR-to-SDR tone mapping
}

/// <summary>
/// Entire frame is composited on the GPU: layers -> BGRA canvas -> (shader) NV12 directly into encoder texture.
/// Zero CPU copies, zero color conversion on the NVENC side.
/// </summary>
public sealed class Compositor : IDisposable
{
    private const string Hlsl = @"
cbuffer P : register(b0)
{
    float4 dst;   // x,y,w,h (0..1)
    float4 src;   // u0,v0,u1,v1
    float4 p0;    // opacity, radiusPx, opaque, darken
    float4 p1;    // dstWpx, dstHpx, stepU, stepV
    float4 p2;    // brightness, contrast, saturation, chromaEnabled
    float4 p3;    // chromaR, chromaG, chromaB, chromaSimilarity
    float4 p4;    // chromaSmoothness, hdr, 0, 0
};
Texture2D tex : register(t0);
SamplerState smp : register(s0);

struct VO { float4 pos : SV_Position; float2 uv : TEXCOORD0; float2 local : TEXCOORD1; };

VO VS(uint id : SV_VertexID)
{
    float2 t = float2(id & 1, id >> 1);
    float2 p = dst.xy + t * dst.zw;
    VO o;
    o.pos = float4(p.x * 2 - 1, 1 - p.y * 2, 0, 1);
    o.uv = lerp(src.xy, src.zw, t);
    o.local = t * p1.xy;
    return o;
}

Texture2D tex2 : register(t1);

// NV12 (BT.709, TV range) -> RGB or regular BGRA texture
float4 SampleSrc(float2 uv, float yuv)
{
    if (yuv > 0.5)
    {
        float y = (tex.SampleLevel(smp, uv, 0).r - 16.0 / 255.0) * (255.0 / 219.0);
        float2 c = (tex2.SampleLevel(smp, uv, 0).rg - 128.0 / 255.0) * (255.0 / 224.0);
        return float4(saturate(float3(y + 1.5748 * c.y, y - 0.1873 * c.x - 0.4681 * c.y, y + 1.8556 * c.x)), 1);
    }
    return tex.SampleLevel(smp, uv, 0);
}

float2 RgbToCbCr(float3 rgb)
{
    float cb = dot(rgb, float3(-0.114572, -0.385428, 0.5));
    float cr = dot(rgb, float3(0.5, -0.454153, -0.045847));
    return float2(cb, cr);
}

// scRGB (linear 16-bit float, 1.0 = 80 nits) -> sRGB SDR (ACES filmic + gamma 2.2)
float3 ToneMapHdrToSdr(float3 rgb)
{
    float3 x = max(rgb, 0.0) * 0.35;
    float3 mapped = (x * (2.51 * x + 0.03)) / (x * (2.43 * x + 0.59) + 0.14);
    return pow(saturate(mapped), 1.0 / 2.2);
}

float4 PSLayer(VO i) : SV_Target
{
    float4 c = SampleSrc(i.uv, p1.z);
    if (p0.z > 0.5) c.a = 1;
    if (p4.y > 0.5) c.rgb = ToneMapHdrToSdr(c.rgb);
    if (c.a > 0.001)
    {
        float3 rgb = c.rgb / c.a;
        if (p2.w > 0.5)
        {
            float dist = distance(RgbToCbCr(rgb), RgbToCbCr(p3.xyz));
            float sim = max(0.001, p3.w);
            float sm = max(0.001, p4.x);
            float mask = smoothstep(sim, sim + sm, dist);
            // subtle background edge despill
            float spill = smoothstep(sim, sim + sm * 2.5, dist);
            float luma = dot(rgb, float3(0.2126, 0.7152, 0.0722));
            rgb = lerp(luma.xxx, rgb, lerp(0.35, 1.0, spill));
            c.a *= mask;
        }
        if (abs(p2.x) > 0.001 || abs(p2.y - 1.0) > 0.001 || abs(p2.z - 1.0) > 0.001)
        {
            rgb = (rgb - 0.5) * p2.y + 0.5 + p2.x;
            float luma = dot(rgb, float3(0.2126, 0.7152, 0.0722));
            rgb = lerp(luma.xxx, rgb, p2.z);
        }
        c.rgb = saturate(rgb) * c.a;
    }
    c.rgb *= p0.w;
    float r = p0.y;
    if (r > 0.5)
    {
        float2 hs = p1.xy * 0.5;
        float2 q = abs(i.local - hs) - (hs - r);
        float d = length(max(q, 0)) + min(max(q.x, q.y), 0) - r;
        c *= saturate(0.5 - d);
    }
    return c * p0.x;
}

float4 PSBlur(VO i) : SV_Target
{
    float3 acc = 0; float ws = 0;
    [unroll] for (int y = -3; y <= 3; y++)
    [unroll] for (int x = -3; x <= 3; x++)
    {
        float w = exp(-(x * x + y * y) / 6.0);
        acc += SampleSrc(i.uv + float2(x, y) * p1.zw, p0.z).rgb * w;
        ws += w;
    }
    float3 col = acc / ws;
    if (p4.y > 0.5) col = ToneMapHdrToSdr(col);
    return float4(col, 1);
}

// BT.709, limited range (TV) — standard for H.264 streaming
float4 PSY(VO i) : SV_Target
{
    float3 c = tex.Sample(smp, i.uv).rgb;
    float y = dot(c, float3(0.2126, 0.7152, 0.0722));
    return float4(16.0 / 255.0 + y * (219.0 / 255.0), 0, 0, 1);
}

float4 PSUV(VO i) : SV_Target
{
    float3 c = tex.Sample(smp, i.uv).rgb; // bilinear sample at 2x2 block center = average of 4 pixels
    float u = dot(c, float3(-0.114572, -0.385428, 0.5));
    float v = dot(c, float3(0.5, -0.454153, -0.045847));
    return float4(128.0 / 255.0 + u * (224.0 / 255.0), 128.0 / 255.0 + v * (224.0 / 255.0), 0, 1);
}
";

    [StructLayout(LayoutKind.Sequential)]
    private struct Cb
    {
        public Vector4 Dst, Src, P0, P1, P2, P3, P4;
    }

    private readonly ID3D11Device _dev;
    private readonly ID3D11DeviceContext _ctx;
    private readonly ID3D11VertexShader _vs;
    private readonly ID3D11PixelShader _psLayer, _psBlur, _psY, _psUV;
    private readonly ID3D11Buffer _cb;
    private readonly ID3D11SamplerState _linear;
    private readonly ID3D11BlendState _alphaBlend, _opaque;
    private readonly ID3D11RasterizerState _raster;

    public int Width { get; }
    public int Height { get; }
    public ID3D11Texture2D Canvas { get; }
    public ID3D11RenderTargetView CanvasRtv { get; }
    public ID3D11ShaderResourceView CanvasSrv { get; }

    // small background blur buffers — computed at low resolution for near-zero GPU cost
    public const int BlurSlots = 4;
    private const int BgW = 144, BgH = 256;
    private readonly ID3D11Texture2D[] _bg = new ID3D11Texture2D[BlurSlots];
    private readonly ID3D11RenderTargetView[] _bgRtv = new ID3D11RenderTargetView[BlurSlots];
    private readonly ID3D11ShaderResourceView[] _bgSrv = new ID3D11ShaderResourceView[BlurSlots];
    public ID3D11ShaderResourceView BgSrv(int slot) => _bgSrv[slot];

    private readonly ID3D11Texture2D _white;
    public ID3D11ShaderResourceView WhiteSrv { get; }

    // preview
    private IDXGISwapChain1? _swap;
    private ID3D11RenderTargetView? _swapRtv;
    private IntPtr _swapHwnd;
    private int _swapW, _swapH;
    private int _presentErrors;

    public Compositor(GpuDevice gpu, int width, int height)
    {
        _dev = gpu.Device;
        _ctx = gpu.Context;
        Width = width;
        Height = height;

        _vs = _dev.CreateVertexShader(Compile("VS", "vs_5_0").Span);
        _psLayer = _dev.CreatePixelShader(Compile("PSLayer", "ps_5_0").Span);
        _psBlur = _dev.CreatePixelShader(Compile("PSBlur", "ps_5_0").Span);
        _psY = _dev.CreatePixelShader(Compile("PSY", "ps_5_0").Span);
        _psUV = _dev.CreatePixelShader(Compile("PSUV", "ps_5_0").Span);

        _cb = _dev.CreateBuffer(new BufferDescription((uint)Marshal.SizeOf<Cb>(), BindFlags.ConstantBuffer, ResourceUsage.Dynamic, CpuAccessFlags.Write));
        _linear = _dev.CreateSamplerState(SamplerDescription.LinearClamp);
        _alphaBlend = _dev.CreateBlendState(BlendDescription.AlphaBlend); // premultiplied: One / InvSrcAlpha
        _opaque = _dev.CreateBlendState(BlendDescription.Opaque);
        _raster = _dev.CreateRasterizerState(RasterizerDescription.CullNone);

        Canvas = CreateRt(width, height, Format.B8G8R8A8_UNorm);
        CanvasRtv = _dev.CreateRenderTargetView(Canvas);
        CanvasSrv = _dev.CreateShaderResourceView(Canvas);

        for (int i = 0; i < BlurSlots; i++)
        {
            _bg[i] = CreateRt(BgW, BgH, Format.B8G8R8A8_UNorm);
            _bgRtv[i] = _dev.CreateRenderTargetView(_bg[i]);
            _bgSrv[i] = _dev.CreateShaderResourceView(_bg[i]);
        }

        _white = CreateImmutable(1, 1, new byte[] { 255, 255, 255, 255 });
        WhiteSrv = _dev.CreateShaderResourceView(_white);
    }

    private static ReadOnlyMemory<byte> Compile(string entry, string profile)
        => Compiler.Compile(Hlsl, entry, "compositor.hlsl", profile, ShaderFlags.OptimizationLevel3);

    private ID3D11Texture2D CreateRt(int w, int h, Format f) => _dev.CreateTexture2D(new Texture2DDescription
    {
        Width = (uint)w, Height = (uint)h, MipLevels = 1, ArraySize = 1, Format = f,
        SampleDescription = new SampleDescription(1, 0), Usage = ResourceUsage.Default,
        BindFlags = BindFlags.RenderTarget | BindFlags.ShaderResource,
    });

    public unsafe ID3D11Texture2D CreateImmutable(int w, int h, byte[] bgraPremul)
    {
        fixed (byte* p = bgraPremul)
        {
            var init = new SubresourceData((IntPtr)p, (uint)(w * 4), 0);
            return _dev.CreateTexture2D(new Texture2DDescription
            {
                Width = (uint)w, Height = (uint)h, MipLevels = 1, ArraySize = 1, Format = Format.B8G8R8A8_UNorm,
                SampleDescription = new SampleDescription(1, 0), Usage = ResourceUsage.Immutable,
                BindFlags = BindFlags.ShaderResource,
            }, new[] { init });
        }
    }

    private void SetCommon()
    {
        _ctx.IASetPrimitiveTopology(PrimitiveTopology.TriangleStrip);
        _ctx.IASetInputLayout(null);
        _ctx.VSSetShader(_vs);
        _ctx.VSSetConstantBuffer(0, _cb);
        _ctx.PSSetConstantBuffer(0, _cb);
        _ctx.PSSetSampler(0, _linear);
        _ctx.RSSetState(_raster);
    }

    private unsafe void UpdateCb(in Cb cb)
    {
        var m = _ctx.Map(_cb, 0, MapMode.WriteDiscard);
        *(Cb*)m.DataPointer = cb;
        _ctx.Unmap(_cb, 0);
    }

    private void DrawQuad(ID3D11ShaderResourceView srv, Vector4 dst, Vector4 src, Vector4 p0, Vector4 p1,
        Vector4? p2 = null, Vector4? p3 = null, Vector4? p4 = null)
    {
        UpdateCb(new Cb
        {
            Dst = dst, Src = src, P0 = p0, P1 = p1,
            P2 = p2 ?? new Vector4(0, 1, 1, 0),
            P3 = p3 ?? Vector4.Zero,
            P4 = p4 ?? Vector4.Zero,
        });
        _ctx.PSSetShaderResource(0, srv);
        _ctx.Draw(4, 0);
    }

    /// <summary>Renders a blurred background from a texture region into small buffer <paramref name="slot"/> (output in <see cref="BgSrv"/>).</summary>
    public void RenderBlurBackground(int slot, ID3D11ShaderResourceView srcSrv, ID3D11ShaderResourceView? uvSrv, Vector4 srcUv, bool hdr = false)
    {
        SetCommon();
        _ctx.OMSetRenderTargets(_bgRtv[slot]);
        _ctx.RSSetViewport(new Viewport(0, 0, BgW, BgH));
        _ctx.OMSetBlendState(_opaque);
        _ctx.PSSetShader(_psBlur);
        _ctx.PSSetShaderResource(1, uvSrv!);
        float stepU = (srcUv.Z - srcUv.X) / BgW * 1.5f;
        float stepV = (srcUv.W - srcUv.Y) / BgH * 1.5f;
        DrawQuad(srcSrv, new Vector4(0, 0, 1, 1), srcUv, new Vector4(0, 0, uvSrv != null ? 1 : 0, 0), new Vector4(BgW, BgH, stepU, stepV),
            p4: new Vector4(0, hdr ? 1 : 0, 0, 0));
        _ctx.PSSetShaderResource(0, null!);
        _ctx.PSSetShaderResource(1, null!);
        _ctx.OMSetRenderTargets((ID3D11RenderTargetView)null!);
    }

    /// <summary>Composites layers onto the canvas.</summary>
    public void RenderCanvas(ReadOnlySpan<DrawCmd> cmds)
    {
        SetCommon();
        _ctx.OMSetRenderTargets(CanvasRtv);
        _ctx.RSSetViewport(new Viewport(0, 0, Width, Height));
        _ctx.ClearRenderTargetView(CanvasRtv, new Color4(0, 0, 0, 1));
        _ctx.OMSetBlendState(_alphaBlend);
        _ctx.PSSetShader(_psLayer);
        foreach (ref readonly var c in cmds) DrawLayer(c, Width, Height);
        _ctx.PSSetShaderResource(0, null!);
        _ctx.PSSetShaderResource(1, null!);
    }

    private void DrawLayer(in DrawCmd c, float targetW, float targetH)
    {
        _ctx.PSSetShaderResource(1, c.Yuv ? c.Srv2! : null!);
        DrawQuad(c.Srv,
            new Vector4(c.Dst.X, c.Dst.Y, c.Dst.W, c.Dst.H),
            c.SrcUv,
            new Vector4(c.Opacity, c.RadiusPx, c.Opaque ? 1 : 0, c.Darken <= 0 ? 1 : c.Darken),
            new Vector4(c.Dst.W * targetW, c.Dst.H * targetH, c.Yuv ? 1 : 0, 0),
            new Vector4(c.Brightness, 1f + c.ContrastDelta, 1f + c.SaturationDelta, c.ChromaKeyEnabled ? 1 : 0),
            new Vector4(c.ChromaKeyRgb.X, c.ChromaKeyRgb.Y, c.ChromaKeyRgb.Z, c.ChromaKeySimilarity),
            new Vector4(c.ChromaKeySmoothness, c.Hdr ? 1 : 0, 0, 0));
    }

    /// <summary>Draws a layer onto an arbitrary render target (e.g. cursor overlay onto a captured frame).</summary>
    public void DrawOnto(ID3D11RenderTargetView rtv, int w, int h, in DrawCmd cmd)
    {
        SetCommon();
        _ctx.OMSetRenderTargets(rtv);
        _ctx.RSSetViewport(new Viewport(0, 0, w, h));
        _ctx.OMSetBlendState(_alphaBlend);
        _ctx.PSSetShader(_psLayer);
        DrawLayer(cmd, w, h);
        _ctx.PSSetShaderResource(0, null!);
        _ctx.OMSetRenderTargets((ID3D11RenderTargetView)null!);
    }

    /// <summary>Converts the canvas to NV12 directly in the encoder texture (two passes: Y and UV).</summary>
    public void ConvertToNv12(ID3D11RenderTargetView yRtv, ID3D11RenderTargetView uvRtv)
    {
        SetCommon();
        _ctx.OMSetBlendState(_opaque);
        var full = new Vector4(0, 0, 1, 1);

        _ctx.OMSetRenderTargets(yRtv);
        _ctx.RSSetViewport(new Viewport(0, 0, Width, Height));
        _ctx.PSSetShader(_psY);
        DrawQuad(CanvasSrv, full, full, Vector4.Zero, Vector4.Zero);

        _ctx.OMSetRenderTargets(uvRtv);
        _ctx.RSSetViewport(new Viewport(0, 0, Width / 2, Height / 2));
        _ctx.PSSetShader(_psUV);
        DrawQuad(CanvasSrv, full, full, Vector4.Zero, Vector4.Zero);

        _ctx.PSSetShaderResource(0, null!);
        _ctx.OMSetRenderTargets((ID3D11RenderTargetView)null!);
    }

    // ---------------- Preview ----------------

    public void SetPreviewTarget(IntPtr hwnd, int w, int h)
    {
        w = Math.Max(w, 16); h = Math.Max(h, 16);
        if (hwnd == _swapHwnd && _swap != null)
        {
            if (w == _swapW && h == _swapH) return;
            _swapRtv?.Dispose(); _swapRtv = null;
            _swap.ResizeBuffers(2, (uint)w, (uint)h, Format.B8G8R8A8_UNorm, SwapChainFlags.None).CheckError();
        }
        else
        {
            ReleasePreview();
            if (hwnd == IntPtr.Zero) return;
            using var factory = GetFactory();
            _swap = factory.CreateSwapChainForHwnd(_dev, hwnd, new SwapChainDescription1
            {
                Width = (uint)w, Height = (uint)h, Format = Format.B8G8R8A8_UNorm, BufferCount = 2,
                BufferUsage = Usage.RenderTargetOutput, SwapEffect = SwapEffect.FlipDiscard,
                SampleDescription = new SampleDescription(1, 0), Scaling = Scaling.Stretch, AlphaMode = AlphaMode.Ignore,
            });
            factory.MakeWindowAssociation(hwnd, WindowAssociationFlags.IgnoreAll);
            _swapHwnd = hwnd;
        }
        using var back = _swap!.GetBuffer<ID3D11Texture2D>(0);
        _swapRtv = _dev.CreateRenderTargetView(back);
        _swapW = w; _swapH = h;
        Log.Info($"Preview: swapchain {w}x{h}");
    }

    private IDXGIFactory2 GetFactory()
    {
        using var dxgiDev = _dev.QueryInterface<IDXGIDevice>();
        using var adapter = dxgiDev.GetParent<IDXGIAdapter>();
        return adapter.GetParent<IDXGIFactory2>();
    }

    /// <summary>Draws the canvas into the preview window (aspect-preserving) + selected layer border. Never blocks.</summary>
    public void PresentPreview(NRect? selection)
    {
        if (_swap == null || _swapRtv == null) return;
        SetCommon();
        _ctx.OMSetRenderTargets(_swapRtv);
        _ctx.ClearRenderTargetView(_swapRtv, new Color4(0.063f, 0.067f, 0.086f, 1)); // #101116 = preview background in UI
        var vp = PreviewViewport(_swapW, _swapH, Width, Height);
        _ctx.RSSetViewport(new Viewport(vp.X, vp.Y, vp.Width, vp.Height));
        _ctx.OMSetBlendState(_opaque);
        _ctx.PSSetShader(_psLayer);
        var full = new Vector4(0, 0, 1, 1);
        DrawQuad(CanvasSrv, full, full, new Vector4(1, 0, 1, 1), new Vector4(vp.Width, vp.Height, 0, 0));

        if (selection is NRect s)
        {
            _ctx.OMSetBlendState(_alphaBlend);
            float tx = 2f / vp.Width, ty = 2f / vp.Height;
            var p0 = new Vector4(0.9f, 0, 1, 1);
            DrawQuad(WhiteSrv, new Vector4(s.X, s.Y, s.W, ty), full, p0, Vector4.Zero);
            DrawQuad(WhiteSrv, new Vector4(s.X, s.Y + s.H - ty, s.W, ty), full, p0, Vector4.Zero);
            DrawQuad(WhiteSrv, new Vector4(s.X, s.Y, tx, s.H), full, p0, Vector4.Zero);
            DrawQuad(WhiteSrv, new Vector4(s.X + s.W - tx, s.Y, tx, s.H), full, p0, Vector4.Zero);
            // corner resize handles — white square with dark border
            float hw = 10f / vp.Width, hh = 10f / vp.Height, bw = 2f / vp.Width, bh = 2f / vp.Height;
            foreach (var (cx, cy) in new[] { (s.X, s.Y), (s.X + s.W, s.Y), (s.X, s.Y + s.H), (s.X + s.W, s.Y + s.H) })
            {
                DrawQuad(WhiteSrv, new Vector4(cx - hw / 2 - bw, cy - hh / 2 - bh, hw + 2 * bw, hh + 2 * bh), full, new Vector4(1, 0, 1, 0.12f), Vector4.Zero);
                DrawQuad(WhiteSrv, new Vector4(cx - hw / 2, cy - hh / 2, hw, hh), full, new Vector4(1, 0, 1, 1), Vector4.Zero);
            }
        }
        _ctx.PSSetShaderResource(0, null!);
        var pr = _swap.Present(0, PresentFlags.DoNotWait); // DXGI_ERROR_WAS_STILL_DRAWING = skip preview frame
        if (pr.Failure && pr.Code != unchecked((int)0x887A000A) && _presentErrors++ < 5) Log.Warn($"Preview present: 0x{pr.Code:X8}");
    }

    public static System.Drawing.RectangleF PreviewViewport(int vw, int vh, int cw, int ch)
    {
        float s = Math.Min((float)vw / cw, (float)vh / ch);
        float w = cw * s, h = ch * s;
        return new System.Drawing.RectangleF((vw - w) / 2, (vh - h) / 2, w, h);
    }

    public void ReleasePreview()
    {
        _swapRtv?.Dispose(); _swapRtv = null;
        _swap?.Dispose(); _swap = null;
        _swapHwnd = IntPtr.Zero;
    }

    public void Dispose()
    {
        ReleasePreview();
        WhiteSrv.Dispose(); _white.Dispose();
        for (int i = 0; i < BlurSlots; i++) { _bgSrv[i].Dispose(); _bgRtv[i].Dispose(); _bg[i].Dispose(); }
        CanvasSrv.Dispose(); CanvasRtv.Dispose(); Canvas.Dispose();
        _raster.Dispose(); _opaque.Dispose(); _alphaBlend.Dispose(); _linear.Dispose(); _cb.Dispose();
        _psUV.Dispose(); _psY.Dispose(); _psBlur.Dispose(); _psLayer.Dispose(); _vs.Dispose();
    }
}
