using System.Diagnostics;
using System.Runtime.InteropServices;
using TikStream.Core;
using TikStream.Gpu;
using Vortice.Direct3D;
using Vortice.Direct3D11;
using Vortice.DXGI;
using Vortice.Mathematics;
using Vortice;

namespace TikStream.Capture;

public sealed record CaptureSource(string Kind, string Id, string Label, IntPtr Handle, bool IsPrimary = false);

/// <summary>Cursor to draw onto a captured frame (in frame pixel coordinates).</summary>
public readonly record struct CursorDraw(ID3D11ShaderResourceView Srv, int X, int Y, int W, int H);

/// <summary>
/// Display / window capture via DXGI Desktop Duplication — WITHOUT the yellow border (which WGC draws).
/// Window = cropped region of the display it resides on.
/// • Display connected to RTX -> all on GPU (zero-copy to our texture),
/// • Display connected to Intel (Optimus) -> dedicated thread on the Intel device, alternating double staging buffers
///   (no GPU stalls), uploaded to RTX.
/// </summary>
public sealed unsafe class DesktopCapture : IDisposable
{
    private readonly GpuDevice _gpu;
    private readonly CaptureSource _src;
    private readonly bool _isWindow;

    // device running the duplication (ours or Intel's)
    private ID3D11Device? _dupDevice;
    private ID3D11DeviceContext? _dupContext;
    private IDXGIOutputDuplication? _dup;
    private IntPtr _monitor;
    private RawRect _outRect;   // display coordinates on desktop (physical pixels)
    private bool _crossGpu;
    private long _nextRetryTicks;

    // output texture on RTX (RT+SR: cursor is drawn via shader)
    private ID3D11Texture2D? _copy;
    private Format _format = Format.B8G8R8A8_UNorm;
    public ID3D11ShaderResourceView? View { get; private set; }
    public RawRect DesktopBounds => _outRect;
    public ID3D11RenderTargetView? Rtv { get; private set; }
    public int Width { get; private set; }
    public int Height { get; private set; }
    public bool IsHdr => _format == Format.R16G16B16A16_Float;
    public bool Closed { get; private set; }
    public long FramesReceived { get; private set; }
    public string Label => _src.Label;
    public string Mode => _crossGpu ? "Intel→RTX copy" : "GPU zero-copy";

    // cross-GPU path
    private Thread? _thread;
    private volatile bool _run = true;
    private readonly object _lock = new();
    private byte[] _front = Array.Empty<byte>(), _back = Array.Empty<byte>();
    private int _bufW, _bufH;
    private Format _bufFmt = Format.B8G8R8A8_UNorm;
    private long _bufSeq, _consumedSeq;

    // cursor
    private readonly object _curLock = new();
    private byte[]? _curPixels;
    private int _curW, _curH, _curHotX, _curHotY;
    private long _curVersion, _curUploaded = -1;
    private int _curX, _curY;         // position relative to display
    private bool _curVisible;
    private ID3D11Texture2D? _curTex;
    private ID3D11ShaderResourceView? _curSrv;
    public bool DrawCursor { get; set; } = true;

    public DesktopCapture(GpuDevice gpu, CaptureSource src)
    {
        _gpu = gpu;
        _src = src;
        _isWindow = src.Kind == "Window";
        var mon = _isWindow ? Native.MonitorFromWindow(src.Handle, 2) : src.Handle;
        try { Init(mon); }
        catch
        {
            Dispose();
            throw;
        }
    }

    // ======================= initialization =======================

    private void Init(IntPtr monitor)
    {
        StopThread();
        _dup?.Dispose(); _dup = null;
        if (_crossGpu) { _dupContext?.Dispose(); _dupDevice?.Dispose(); }
        _dupContext = null; _dupDevice = null;

        _monitor = monitor;
        var (adapter, output) = FindOutput(_gpu.Factory, monitor);
        if (adapter == null || output == null) throw new InvalidOperationException("Display not found for source " + _src.Label);
        using (adapter)
        using (output)
        {
            var od = output.Description;
            _outRect = od.DesktopCoordinates;
            if (od.Rotation != ModeRotation.Identity && od.Rotation != ModeRotation.Unspecified)
                Log.Warn($"Display {od.DeviceName} is rotated ({od.Rotation}) — captured image may appear rotated.");

            _crossGpu = adapter.Description1.Luid != _gpu.Adapter.Description1.Luid;
            if (_crossGpu)
            {
                D3D11.D3D11CreateDevice(adapter, DriverType.Unknown, DeviceCreationFlags.BgraSupport,
                    new[] { FeatureLevel.Level_11_1, FeatureLevel.Level_11_0 }, out ID3D11Device? d, out ID3D11DeviceContext? c).CheckError();
                _dupDevice = d!; _dupContext = c!;
            }
            else
            {
                _dupDevice = _gpu.Device;
                _dupContext = _gpu.Context;
            }
            _dup = Duplicate(output, _dupDevice);
            Log.Info($"Desktop Duplication: {od.DeviceName} {_outRect.Right - _outRect.Left}x{_outRect.Bottom - _outRect.Top} on '{adapter.Description1.Description}' -> {Mode}");
        }

        if (_crossGpu)
        {
            _run = true;
            _thread = new Thread(CrossLoop) { IsBackground = true, Name = "DesktopDup", Priority = ThreadPriority.AboveNormal };
            _thread.Start();
        }
    }

    private static IDXGIOutputDuplication Duplicate(IDXGIOutput output, ID3D11Device device)
    {
        // DuplicateOutput1 also works when HDR is enabled (standard DuplicateOutput refuses)
        // Direct vtable call — Vortice's wrapper has an incorrect signature (crash 0xC0000005).
        // HRESULT IDXGIOutput5::DuplicateOutput1(IUnknown*, UINT Flags, UINT Count, const DXGI_FORMAT*, IDXGIOutputDuplication**)
        // index 26: IUnknown(3) + IDXGIObject(4) + IDXGIOutput(12) + IDXGIOutput1(4) + Output2/3/4 (1 each)
        if (output.QueryInterfaceOrNull<IDXGIOutput5>() is { } o5)
        {
            using (o5)
            {
                bool hdr = false;
                if (output.QueryInterfaceOrNull<IDXGIOutput6>() is { } o6)
                {
                    using (o6)
                        hdr = o6.Description1.ColorSpace == ColorSpaceType.RgbFullG2084NoneP2020;
                }
                var vtbl = *(void***)o5.NativePointer;
                var fn = (delegate* unmanaged[Stdcall]<IntPtr, IntPtr, uint, uint, Format*, IntPtr*, int>)vtbl[26];
                Format* fmts = stackalloc Format[2];
                uint count;
                if (hdr)
                {
                    fmts[0] = Format.R16G16B16A16_Float;
                    fmts[1] = Format.B8G8R8A8_UNorm;
                    count = 2;
                }
                else
                {
                    fmts[0] = Format.B8G8R8A8_UNorm;
                    count = 1;
                }
                IntPtr dup = IntPtr.Zero;
                int hr = fn(o5.NativePointer, device.NativePointer, 0, count, fmts, &dup);
                if (hr >= 0 && dup != IntPtr.Zero) return new IDXGIOutputDuplication(dup);
                Log.Info($"DuplicateOutput1: 0x{hr:X8}, falling back to DuplicateOutput.");
            }
        }
        using var o1 = output.QueryInterface<IDXGIOutput1>();
        return o1.DuplicateOutput(device);
    }

    private static (IDXGIAdapter1?, IDXGIOutput?) FindOutput(IDXGIFactory2 factory, IntPtr monitor)
    {
        for (uint a = 0; factory.EnumAdapters1(a, out var adapter).Success; a++)
        {
            for (uint o = 0; adapter.EnumOutputs(o, out var output).Success; o++)
            {
                if (output.Description.Monitor == monitor) return (adapter, output);
                output.Dispose();
            }
            adapter.Dispose();
        }
        return (null, null);
    }

    /// <summary>Region to copy, in pixels relative to the display. Null = nothing to copy (minimized window, etc.).</summary>
    private RawRect? CropRect()
    {
        int mw = _outRect.Right - _outRect.Left, mh = _outRect.Bottom - _outRect.Top;
        if (!_isWindow) return new RawRect(0, 0, mw, mh);
        if (!Native.IsWindow(_src.Handle)) { Closed = true; return null; }
        if (Native.IsIconic(_src.Handle)) return null;
        if (Native.DwmGetWindowAttribute(_src.Handle, Native.DWMWA_EXTENDED_FRAME_BOUNDS, out Native.RECT r, sizeof(Native.RECT)) != 0)
            return null;
        int l = Math.Clamp(r.Left - _outRect.Left, 0, mw), t = Math.Clamp(r.Top - _outRect.Top, 0, mh);
        int rr = Math.Clamp(r.Right - _outRect.Left, 0, mw), b = Math.Clamp(r.Bottom - _outRect.Top, 0, mh);
        if (rr - l < 16 || b - t < 16) return null;
        return new RawRect(l, t, rr & ~1, b & ~1);
    }

    // ======================= render thread =======================

    /// <summary>Call every frame from the render thread. Returns true when a new frame arrived.</summary>
    public bool Update(ID3D11DeviceContext ctx, out CursorDraw? cursor)
    {
        cursor = null;
        if (Closed) return false;

        // window moved to another display -> switch duplication
        if (_isWindow && Native.IsWindow(_src.Handle))
        {
            var mon = Native.MonitorFromWindow(_src.Handle, 2);
            if (mon != _monitor) { try { Init(mon); } catch (Exception ex) { Log.Warn("Display switch: " + ex.Message); } return false; }
        }

        bool got = _crossGpu ? ConsumeCross(ctx) : AcquireSameGpu(ctx);
        if (got && DrawCursor) cursor = PrepareCursor(ctx);
        return got;
    }

    private bool AcquireSameGpu(ID3D11DeviceContext ctx)
    {
        if (_dup == null) { TryRecreate(); return false; }
        var res = _dup.AcquireNextFrame(0, out var info, out var resource);
        if (res == Vortice.DXGI.ResultCode.WaitTimeout) return false;
        if (res.Failure)
        {
            HandleLost(res);
            return false;
        }
        try
        {
            UpdatePointer(_dup, info);
            var crop = CropRect();
            if (crop == null) return false;
            var c = crop.Value;
            using var tex = resource!.QueryInterface<ID3D11Texture2D>();
            EnsureCopy(c.Right - c.Left, c.Bottom - c.Top, tex.Description.Format);
            ctx.CopySubresourceRegion(_copy!, 0, 0, 0, 0, tex, 0, new Box(c.Left, c.Top, 0, c.Right, c.Bottom, 1));
            FramesReceived++;
            return true;
        }
        finally
        {
            resource?.Dispose();
            _dup.ReleaseFrame();
        }
    }

    private void HandleLost(SharpGen.Runtime.Result res)
    {
        // ACCESS_LOST: mode change, UAC, fullscreen game switched mode -> recreate quickly
        Log.Warn($"Desktop Duplication lost ({res.Code:X8}) — recreating.");
        _dup?.Dispose(); _dup = null;
        _nextRetryTicks = Stopwatch.GetTimestamp() + Stopwatch.Frequency / 20; // 50 ms first retry
    }

    private void TryRecreate()
    {
        if (Stopwatch.GetTimestamp() < _nextRetryTicks) return;
        _nextRetryTicks = Stopwatch.GetTimestamp() + Stopwatch.Frequency / 4; // 250 ms subsequent retries
        try
        {
            var (adapter, output) = FindOutput(_gpu.Factory, _monitor);
            if (adapter == null || output == null) return;
            using (adapter) using (output) _dup = Duplicate(output, _dupDevice!);
            Log.Info("Desktop Duplication restored.");
        }
        catch (Exception ex) { Log.Warn("Restoring duplication: " + ex.Message); }
    }

    private static int BytesPerPixel(Format f) => f == Format.R16G16B16A16_Float ? 8 : 4;

    private void EnsureCopy(int w, int h, Format fmt)
    {
        if (_copy != null && w == Width && h == Height && fmt == _format) return;
        Rtv?.Dispose(); View?.Dispose(); _copy?.Dispose();
        _format = fmt;
        _copy = _gpu.Device.CreateTexture2D(new Texture2DDescription
        {
            Width = (uint)w, Height = (uint)h, MipLevels = 1, ArraySize = 1, Format = fmt,
            SampleDescription = new SampleDescription(1, 0), Usage = ResourceUsage.Default,
            BindFlags = BindFlags.ShaderResource | BindFlags.RenderTarget,
        });
        View = _gpu.Device.CreateShaderResourceView(_copy);
        Rtv = _gpu.Device.CreateRenderTargetView(_copy);
        Width = w; Height = h;
    }

    private bool ConsumeCross(ID3D11DeviceContext ctx)
    {
        lock (_lock)
        {
            if (_bufSeq == _consumedSeq || _bufW == 0) return false;
            _consumedSeq = _bufSeq;
            EnsureCopy(_bufW, _bufH, _bufFmt);
            fixed (byte* p = _front)
                ctx.UpdateSubresource(_copy!, 0, null, (IntPtr)p, (uint)(_bufW * BytesPerPixel(_bufFmt)), 0);
        }
        FramesReceived++;
        return true;
    }

    // ======================= Intel -> RTX thread =======================

    private void CrossLoop()
    {
        ID3D11Texture2D? staging = null;
        try
        {
            while (_run)
            {
                if (_dup == null) { TryRecreate(); Thread.Sleep(50); continue; }
                var res = _dup.AcquireNextFrame(20, out var info, out var resource);
                if (res == Vortice.DXGI.ResultCode.WaitTimeout) continue;
                if (res.Failure) { HandleLost(res); continue; }
                try
                {
                    UpdatePointer(_dup, info);
                    var crop = CropRect();
                    if (crop == null) continue;
                    var c = crop.Value;
                    int w = c.Right - c.Left, h = c.Bottom - c.Top;
                    using var tex = resource!.QueryInterface<ID3D11Texture2D>();
                    var fmt = tex.Description.Format;
                    var st = staging;
                    if (st == null || st.Description.Width != w || st.Description.Height != h || st.Description.Format != fmt)
                    {
                        st?.Dispose();
                        st = staging = _dupDevice!.CreateTexture2D(new Texture2DDescription
                        {
                            Width = (uint)w, Height = (uint)h, MipLevels = 1, ArraySize = 1, Format = fmt,
                            SampleDescription = new SampleDescription(1, 0), Usage = ResourceUsage.Staging,
                            CPUAccessFlags = CpuAccessFlags.Read,
                        });
                    }
                    _dupContext!.CopySubresourceRegion(st, 0, 0, 0, 0, tex, 0, new Box(c.Left, c.Top, 0, c.Right, c.Bottom, 1));
                }
                finally
                {
                    resource?.Dispose();
                    _dup?.ReleaseFrame();
                }

                // Desktop Duplication does not emit another frame while the desktop is static.
                // Publish the current frame right here on the capture thread without waiting for the next one.
                var s = staging!;
                var sfmt = s.Description.Format;
                int sw = (int)s.Description.Width, sh = (int)s.Description.Height, rowBytes = sw * BytesPerPixel(sfmt);
                var m = _dupContext!.Map(s, 0, MapMode.Read);
                try
                {
                    int size = rowBytes * sh;
                    if (_back.Length != size) _back = new byte[size];
                    fixed (byte* dst = _back)
                    {
                        byte* src = (byte*)m.DataPointer;
                        if (m.RowPitch == rowBytes) Buffer.MemoryCopy(src, dst, size, size);
                        else for (int y = 0; y < sh; y++)
                            Buffer.MemoryCopy(src + y * m.RowPitch, dst + y * rowBytes, rowBytes, rowBytes);
                    }
                }
                finally { _dupContext.Unmap(s, 0); }
                lock (_lock)
                {
                    (_front, _back) = (_back, _front);
                    _bufW = sw; _bufH = sh; _bufFmt = sfmt; _bufSeq++;
                }
            }
        }
        catch (Exception ex) { Log.Error("Desktop Duplication thread", ex); }
        finally { staging?.Dispose(); }
    }

    private void StopThread()
    {
        _run = false;
        _thread?.Join(2000);
        _thread = null;
    }

    // ======================= cursor =======================

    private void UpdatePointer(IDXGIOutputDuplication dup, OutduplFrameInfo info)
    {
        if (info.LastMouseUpdateTime != 0)
        {
            lock (_curLock)
            {
                _curVisible = info.PointerPosition.Visible;
                _curX = info.PointerPosition.Position.X;
                _curY = info.PointerPosition.Position.Y;
            }
        }
        if (info.PointerShapeBufferSize == 0) return;
        var buf = new byte[info.PointerShapeBufferSize];
        fixed (byte* p = buf)
        {
            if (dup.GetFramePointerShape((uint)buf.Length, (IntPtr)p, out _, out var si).Failure) return;
            var (pix, w, h) = ConvertPointer(buf, si);
            lock (_curLock)
            {
                _curPixels = pix; _curW = w; _curH = h;
                _curHotX = si.HotSpot.X; _curHotY = si.HotSpot.Y;
                _curVersion++;
            }
        }
    }

    /// <summary>Cursor shape -> premultiplied BGRA (supports all 3 DXGI cursor types).</summary>
    private static (byte[] pix, int w, int h) ConvertPointer(byte[] buf, OutduplPointerShapeInfo si)
    {
        int w = (int)si.Width, pitch = (int)si.Pitch;
        if (si.Type == 1) // MONOCHROME: AND mask above XOR mask, 1 bit/pixel
        {
            int h = (int)si.Height / 2;
            var px = new byte[w * h * 4];
            for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++)
            {
                int bit = 0x80 >> (x & 7);
                bool and = (buf[y * pitch + x / 8] & bit) != 0;
                bool xor = (buf[(y + h) * pitch + x / 8] & bit) != 0;
                int o = (y * w + x) * 4;
                if (and && !xor) continue;                                  // transparent
                byte v = (!and && xor) ? (byte)255 : (byte)0;               // white / black (invert -> black)
                px[o] = px[o + 1] = px[o + 2] = v; px[o + 3] = 255;
            }
            return (px, w, h);
        }
        else
        {
            int h = (int)si.Height;
            var px = new byte[w * h * 4];
            for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++)
            {
                int s = y * pitch + x * 4, o = (y * w + x) * 4;
                byte b = buf[s], g = buf[s + 1], r = buf[s + 2], a = buf[s + 3];
                if (si.Type == 4) // MASKED_COLOR: a=0 -> color, a=0xFF -> XOR with background
                {
                    if (a == 0) { px[o] = b; px[o + 1] = g; px[o + 2] = r; px[o + 3] = 255; }
                    else if ((b | g | r) != 0) { px[o] = px[o + 1] = px[o + 2] = 0; px[o + 3] = 255; }
                }
                else // COLOR: BGRA with alpha
                {
                    px[o] = (byte)(b * a / 255); px[o + 1] = (byte)(g * a / 255); px[o + 2] = (byte)(r * a / 255); px[o + 3] = a;
                }
            }
            return (px, w, h);
        }
    }

    private CursorDraw? PrepareCursor(ID3D11DeviceContext ctx)
    {
        byte[]? pix; int w, h, hx, hy, x, y; long ver; bool vis;
        lock (_curLock)
        {
            pix = _curPixels; w = _curW; h = _curH; hx = _curHotX; hy = _curHotY;
            x = _curX; y = _curY; ver = _curVersion; vis = _curVisible;
        }
        if (!vis || pix == null || w == 0 || h == 0) return null;
        if (ver != _curUploaded)
        {
            _curSrv?.Dispose(); _curTex?.Dispose();
            fixed (byte* p = pix)
            {
                _curTex = _gpu.Device.CreateTexture2D(new Texture2DDescription
                {
                    Width = (uint)w, Height = (uint)h, MipLevels = 1, ArraySize = 1, Format = Format.B8G8R8A8_UNorm,
                    SampleDescription = new SampleDescription(1, 0), Usage = ResourceUsage.Immutable, BindFlags = BindFlags.ShaderResource,
                }, new[] { new SubresourceData((IntPtr)p, (uint)(w * 4), 0) });
            }
            _curSrv = _gpu.Device.CreateShaderResourceView(_curTex);
            _curUploaded = ver;
        }
        // cursor position relative to display -> relative to cropped region
        var crop = _isWindow ? CropRect() : new RawRect(0, 0, Width, Height);
        if (crop == null) return null;
        int cx = x - crop.Value.Left, cy = y - crop.Value.Top;
        if (cx + w < 0 || cy + h < 0 || cx > Width || cy > Height) return null;
        return new CursorDraw(_curSrv!, cx, cy, w, h);
    }

    public void Dispose()
    {
        StopThread();
        _dup?.Dispose();
        if (_crossGpu) { _dupContext?.Dispose(); _dupDevice?.Dispose(); }
        _curSrv?.Dispose(); _curTex?.Dispose();
        Rtv?.Dispose(); View?.Dispose(); _copy?.Dispose();
    }

    // ======================= source enumeration =======================

    public static List<CaptureSource> EnumerateSources(IDXGIFactory2 factory)
    {
        var list = new List<CaptureSource>();
        int n = 1;
        var monitors = new HashSet<IntPtr>();
        for (uint a = 0; factory.EnumAdapters1(a, out var adapter).Success; a++)
        {
            using (adapter)
            {
                string gpu = adapter.Description1.VendorId switch { 0x10DE => "NVIDIA", 0x8086 => "Intel", 0x1002 => "AMD", _ => "GPU" };
                for (uint o = 0; adapter.EnumOutputs(o, out var output).Success; o++)
                {
                    using (output)
                    {
                        var d = output.Description;
                        if (!d.AttachedToDesktop || d.Monitor == IntPtr.Zero || !monitors.Add(d.Monitor)) continue;
                        var r = d.DesktopCoordinates;
                        bool primary = r.Left == 0 && r.Top == 0;
                        list.Add(new CaptureSource("Monitor", d.DeviceName,
                            $"Display {n++} • {r.Right - r.Left}×{r.Bottom - r.Top}{(primary ? " • primary" : "")} • {gpu}", d.Monitor, primary));
                    }
                }
            }
        }

        list.AddRange(EnumerateWindows());
        return list;
    }

    public static List<CaptureSource> EnumerateWindows()
    {
        var list = new List<CaptureSource>();
        uint myPid = (uint)Environment.ProcessId;
        Native.EnumWindows((h, _) =>
        {
            if (!Native.IsWindowVisible(h) && !Native.IsIconic(h)) return true;
            var title = Native.GetWindowTitle(h);
            if (string.IsNullOrWhiteSpace(title)) return true;
            Native.GetWindowThreadProcessId(h, out uint pid);
            if (pid == myPid) return true;
            string proc = "";
            try { using var process = Process.GetProcessById((int)pid); proc = process.ProcessName; } catch { }
            if (proc == "explorer" && title == "Program Manager") return true;
            var minimized = Native.IsIconic(h) ? " • minimized" : "";
            list.Add(new CaptureSource("Window", proc + "|" + title, $"{title} [{proc}]{minimized}", h));
            return true;
        }, IntPtr.Zero);
        return list;
    }

    public static CaptureSource? Resolve(List<CaptureSource> sources, string kind, string id)
    {
        var exact = sources.FirstOrDefault(s => s.Kind == kind && s.Id == id);
        if (exact != null) return exact;
        if (kind == "Window")
        {
            var proc = id.Split('|')[0];
            return sources.FirstOrDefault(s => s.Kind == "Window" && s.Id.Split('|')[0] == proc);
        }
        return null;
    }
}
