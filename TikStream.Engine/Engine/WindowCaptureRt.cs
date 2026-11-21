using System.Diagnostics;
using System.Runtime.InteropServices;
using TikStream.Capture;
using TikStream.Core;
using TikStream.Gpu;
using Vortice.DXGI;
using Vortice.Direct3D11;
using Vortice.Mathematics;
using Windows.Graphics.Capture;
using Windows.Graphics.DirectX;
using Windows.Graphics.DirectX.Direct3D11;
using WinRT;
using ID3D11Texture2D = Vortice.Direct3D11.ID3D11Texture2D;

namespace TikStream.Engine;

internal sealed unsafe class WindowCaptureRt : RtSource
{
    private IntPtr _target;
    private long _nextResolve;
    private GraphicsCaptureSession? _session;
    private Direct3D11CaptureFramePool? _pool;
    private IDirect3DDevice? _device;
    private GraphicsCaptureItem? _item;
    private ID3D11Texture2D? _texture;
    private Windows.Graphics.SizeInt32 _poolSize;
    private volatile bool _closed;
    private string _status = "Oczekiwanie na okno…";
    private bool _cursor;

    public override string Status => _status;

    public override void Update(GpuDevice gpu, Compositor comp)
    {
        try { UpdateCapture(gpu); }
        catch (Exception ex)
        {
            Dispose();
            _status = "System nie udostępnia obrazu tego okna";
            _nextResolve = Stopwatch.GetTimestamp() + Stopwatch.Frequency;
            Log.Warn($"Przechwytywanie '{Def.Name}': {ex.Message}");
        }
    }

    private void UpdateCapture(GpuDevice gpu)
    {
        if (Stopwatch.GetTimestamp() >= _nextResolve)
        {
            _nextResolve = Stopwatch.GetTimestamp() + Stopwatch.Frequency;
            Resolve(gpu);
        }
        if (_pool == null) return;
        if (Native.IsIconic(_target))
        {
            _status = "Okno zminimalizowane • ostatnia dostępna klatka";
            return;
        }
        _status = Ready ? $"{Width}×{Height} • Windows Graphics Capture" : "Oczekiwanie na klatkę…";
        if (_cursor != Def.CaptureCursor && _session != null)
            _session.IsCursorCaptureEnabled = _cursor = Def.CaptureCursor;
        using var frame = _pool.TryGetNextFrame();
        if (frame == null) return;
        var size = frame.ContentSize;
        if (size.Width <= 0 || size.Height <= 0) return;
        if (size.Width != _poolSize.Width || size.Height != _poolSize.Height)
        {
            frame.Dispose();
            _pool.Recreate(_device!, DirectXPixelFormat.B8G8R8A8UIntNormalized, 2, size);
            _poolSize = size;
            return;
        }
        using var source = GetTexture(frame.Surface);
        if (_texture == null || Width != size.Width || Height != size.Height)
        {
            Srv?.Dispose();
            _texture?.Dispose();
            _texture = gpu.Device.CreateTexture2D(new Texture2DDescription
            {
                Width = (uint)size.Width, Height = (uint)size.Height, MipLevels = 1, ArraySize = 1,
                Format = Format.B8G8R8A8_UNorm, SampleDescription = new SampleDescription(1, 0),
                Usage = ResourceUsage.Default, BindFlags = BindFlags.ShaderResource,
            });
            Srv = gpu.Device.CreateShaderResourceView(_texture);
            Width = size.Width;
            Height = size.Height;
        }
        gpu.Context.CopySubresourceRegion(_texture, 0, 0, 0, 0, source, 0, new Box(0, 0, 0, Width, Height, 1));
        Frames++;
    }

    private void Resolve(GpuDevice gpu)
    {
        bool fullscreen = IsFullscreenMode(Def);
        IntPtr target = ResolveTarget(Def);
        if (fullscreen && target == IntPtr.Zero && _target != IntPtr.Zero && !_closed && Native.IsWindow(_target)) return;
        if (_pool != null && !_closed && target == _target) return;
        Dispose();
        _status = fullscreen ? "Oczekiwanie na aplikację pełnoekranową…" : "Oczekiwanie na wybrane okno…";
        if (target == IntPtr.Zero) return;
        if (Native.IsIconic(target))
        {
            _status = "Okno zminimalizowane • oczekiwanie na pierwszą klatkę";
            return;
        }
        if (!GraphicsCaptureSession.IsSupported()) throw new NotSupportedException("Windows Graphics Capture");
        using var dxgi = gpu.Device.QueryInterface<IDXGIDevice>();
        Marshal.ThrowExceptionForHR(CreateDirect3D11DeviceFromDXGIDevice(dxgi.NativePointer, out var abiDevice));
        try { _device = MarshalInterface<IDirect3DDevice>.FromAbi(abiDevice); }
        finally { Marshal.Release(abiDevice); }
        var item = _item = CreateItem(target);
        _closed = false;
        item.Closed += ItemClosed;
        _poolSize = item.Size;
        _pool = Direct3D11CaptureFramePool.CreateFreeThreaded(_device, DirectXPixelFormat.B8G8R8A8UIntNormalized, 2, _poolSize);
        _session = _pool.CreateCaptureSession(item);
        _session.IsCursorCaptureEnabled = _cursor = Def.CaptureCursor;
        ConfigureSessionCapabilities(_session);
        _session.StartCapture();
        _target = target;
    }

    private static void ConfigureSessionCapabilities(GraphicsCaptureSession session)
    {
        const string cls = "Windows.Graphics.Capture.GraphicsCaptureSession";
        try
        {
            if (Windows.Foundation.Metadata.ApiInformation.IsPropertyPresent(cls, "IsBorderRequired"))
                session.IsBorderRequired = false;
        }
        catch { }
        try
        {
            if (Windows.Foundation.Metadata.ApiInformation.IsPropertyPresent(cls, "MinUpdateInterval"))
                session.MinUpdateInterval = TimeSpan.FromMilliseconds(4);
        }
        catch { }
        try
        {
            if (Windows.Foundation.Metadata.ApiInformation.IsPropertyPresent(cls, "DirtyRegionMode"))
                session.DirtyRegionMode = GraphicsCaptureDirtyRegionMode.ReportAndRender;
        }
        catch { }
    }

    private void ItemClosed(GraphicsCaptureItem sender, object args) => _closed = true;

    internal static bool IsFullscreenMode(SourceDef def) => def.Type == SourceType.Game && def.GameMode == "Fullscreen";

    internal static IntPtr ResolveTarget(SourceDef def)
    {
        if (IsFullscreenMode(def)) return FullscreenWindow();
        if (string.IsNullOrEmpty(def.CaptureId)) return IntPtr.Zero;
        var windows = DesktopCapture.EnumerateWindows();
        var exact = windows.FirstOrDefault(s => s.Id == def.CaptureId);
        return (def.MatchWindowTitle ? exact : exact ?? DesktopCapture.Resolve(windows, "Window", def.CaptureId))
            ?.Handle ?? IntPtr.Zero;
    }

    private static IntPtr FullscreenWindow()
    {
        var hwnd = GetForegroundWindow();
        if (hwnd == IntPtr.Zero || Native.IsIconic(hwnd)) return IntPtr.Zero;
        Native.GetWindowThreadProcessId(hwnd, out uint pid);
        if (pid == Environment.ProcessId) return IntPtr.Zero;
        var monitor = Native.MonitorFromWindow(hwnd, 2);
        var info = new MonitorInfo { Size = (uint)sizeof(MonitorInfo) };
        if (!GetMonitorInfoW(monitor, ref info) ||
            Native.DwmGetWindowAttribute(hwnd, Native.DWMWA_EXTENDED_FRAME_BOUNDS, out Native.RECT rect, sizeof(Native.RECT)) != 0)
            return IntPtr.Zero;
        var bounds = info.Monitor;
        return rect.Left <= bounds.Left && rect.Top <= bounds.Top && rect.Right >= bounds.Right && rect.Bottom >= bounds.Bottom
            ? hwnd : IntPtr.Zero;
    }

    private static GraphicsCaptureItem CreateItem(IntPtr hwnd)
    {
        const string type = "Windows.Graphics.Capture.GraphicsCaptureItem";
        Marshal.ThrowExceptionForHR(WindowsCreateString(type, type.Length, out var hstring));
        try
        {
            var interop = new Guid("3628E81B-3CAC-4C60-B7F4-23CE0E0C3356");
            Marshal.ThrowExceptionForHR(RoGetActivationFactory(hstring, ref interop, out var factory));
            try
            {
                var iid = new Guid("79C3F95B-31F7-4EC2-A464-632EF5D30760");
                var create = (delegate* unmanaged[Stdcall]<IntPtr, IntPtr, Guid*, IntPtr*, int>)(*(IntPtr**)factory)[3];
                IntPtr raw;
                Marshal.ThrowExceptionForHR(create(factory, hwnd, &iid, &raw));
                try { return GraphicsCaptureItem.FromAbi(raw); }
                finally { Marshal.Release(raw); }
            }
            finally { Marshal.Release(factory); }
        }
        finally { WindowsDeleteString(hstring); }
    }

    private static ID3D11Texture2D GetTexture(IDirect3DSurface surface)
    {
        var marshaler = MarshalInterface<IDirect3DSurface>.CreateMarshaler(surface);
        try
        {
            var iid = new Guid("A9B3D012-3DF2-4EE3-B8D1-8695F457D3C1");
            Marshal.ThrowExceptionForHR(Marshal.QueryInterface(MarshalInterface<IDirect3DSurface>.GetAbi(marshaler), in iid, out var access));
            try
            {
                var textureIid = typeof(ID3D11Texture2D).GUID;
                var get = (delegate* unmanaged[Stdcall]<IntPtr, Guid*, IntPtr*, int>)(*(IntPtr**)access)[3];
                IntPtr raw;
                Marshal.ThrowExceptionForHR(get(access, &textureIid, &raw));
                return new ID3D11Texture2D(raw);
            }
            finally { Marshal.Release(access); }
        }
        finally { MarshalInterface<IDirect3DSurface>.DisposeMarshaler(marshaler); }
    }

    public override void Dispose()
    {
        if (_item != null) _item.Closed -= ItemClosed;
        _item = null;
        _session?.Dispose();
        _pool?.Dispose();
        _device?.Dispose();
        Srv?.Dispose();
        _texture?.Dispose();
        _session = null;
        _pool = null;
        _device = null;
        _texture = null;
        Srv = null;
        _target = IntPtr.Zero;
        Width = Height = 0;
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct MonitorInfo { public uint Size; public Native.RECT Monitor, Work; public uint Flags; }
    [DllImport("user32.dll")] internal static extern IntPtr GetForegroundWindow();
    [DllImport("user32.dll")] private static extern bool GetMonitorInfoW(IntPtr monitor, ref MonitorInfo info);
    [DllImport("combase.dll", CharSet = CharSet.Unicode)] private static extern int WindowsCreateString(string value, int length, out IntPtr result);
    [DllImport("combase.dll")] private static extern int WindowsDeleteString(IntPtr hstring);
    [DllImport("combase.dll")] private static extern int RoGetActivationFactory(IntPtr hstring, ref Guid iid, out IntPtr factory);
    [DllImport("d3d11.dll")] private static extern int CreateDirect3D11DeviceFromDXGIDevice(IntPtr dxgi, out IntPtr inspectable);
}
