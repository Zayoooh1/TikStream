using System.Diagnostics;
using TikStream.Capture;
using TikStream.Core;
using TikStream.Gpu;
using Vortice.Direct3D11;
using Vortice.DXGI;
using Vortice.Mathematics;

namespace TikStream.Engine;

internal sealed class DesktopWindowRt(MonitorCapturePool monitors) : RtSource
{
    private IntPtr _target, _monitor;
    private MonitorCapturePool.Entry? _entry;
    private ID3D11Texture2D? _texture, _safeTexture;
    private ID3D11ShaderResourceView? _liveSrv, _safeSrv;
    private long _nextResolve, _lastSafeCopy;
    private Task<IntPtr>? _resolving;
    private string _status = "Waiting for window…";
    public override string Status => _status;
    public override bool IsHdr => _entry?.Capture.IsHdr ?? false;

    public override void Update(GpuDevice gpu, Compositor comp)
    {
        try { UpdateDesktop(gpu, comp); }
        catch (Exception ex)
        {
            Dispose();
            _nextResolve = Stopwatch.GetTimestamp() + Stopwatch.Frequency;
            _status = "Cannot capture display image";
            Log.Warn($"Capture '{Def.Name}': {ex.Message}");
        }
    }

    private void UpdateDesktop(GpuDevice gpu, Compositor comp)
    {
        if (_resolving is { IsCompleted: true } resolved)
        {
            _resolving = null;
            SetTarget(resolved.GetAwaiter().GetResult());
        }
        var now = Stopwatch.GetTimestamp();
        if (now >= _nextResolve)
        {
            _nextResolve = now + Stopwatch.Frequency / 2;
            if (WindowCaptureRt.IsFullscreenMode(Def))
            {
                var target = WindowCaptureRt.ResolveFullscreenTarget(_target, _monitor);
                if (target != IntPtr.Zero || !Native.IsWindow(_target)) SetTarget(target);
            }
            else if (_resolving == null && !string.IsNullOrEmpty(Def.CaptureId))
            {
                // Window and process enumeration can block for hundreds of ms.
                var def = Def;
                _resolving = Task.Run(() => WindowCaptureRt.ResolveTarget(def));
            }
        }

        if (_target == IntPtr.Zero || !Native.IsWindow(_target))
        {
            _status = "Waiting for selected window…";
            return;
        }
        if (Native.IsIconic(_target) || !Native.IsWindowVisible(_target))
        {
            if (_safeSrv != null) Srv = _safeSrv;
            _status = Ready ? "Window minimized • last frame" : "Window minimized";
            return;
        }
        if (Native.DwmGetWindowAttribute(_target, Native.DWMWA_CLOAKED, out int targetCloaked, 4) == 0 && targetCloaked != 0)
        {
            if (_safeSrv != null) Srv = _safeSrv;
            _status = Ready ? "Window hidden • last frame" : "Window hidden";
            return;
        }
        if (Native.DwmGetWindowAttribute(_target, Native.DWMWA_EXTENDED_FRAME_BOUNDS,
                out Native.RECT rect, 16) != 0) return;

        var monitor = Native.MonitorFromWindow(_target, 2);
        var fg = WindowCaptureRt.GetForegroundWindow();
        if (fg != IntPtr.Zero && fg != _target && Native.MonitorFromWindow(fg, 2) == monitor)
        {
            if (_safeSrv != null) Srv = _safeSrv;
            _status = Ready ? "Another window active on game display • last frame" : "Switch to selected window";
            return;
        }
        if (IsOccluded(_target, rect))
        {
            if (_safeSrv != null) Srv = _safeSrv;
            _status = Ready ? "Window covered • last clean frame" : "Switch to selected window";
            return;
        }
        if (monitor != _monitor)
        {
            if (_entry != null) monitors.Release(_entry);
            _entry = null;
            _monitor = IntPtr.Zero;
            var source = DesktopCapture.EnumerateSources(gpu.Factory)
                .FirstOrDefault(s => s.Kind == "Monitor" && s.Handle == monitor);
            if (source == null) return;
            _entry = monitors.Acquire(gpu, source);
            _monitor = monitor;
        }
        if (_entry == null || !monitors.Update(_entry, gpu)) return;
        var view = _entry.View(Def.CaptureCursor, gpu, comp);
        if (view == null) return;
        var bounds = _entry.Capture.DesktopBounds;
        int left = Math.Clamp(rect.Left - bounds.Left, 0, _entry.Capture.Width);
        int top = Math.Clamp(rect.Top - bounds.Top, 0, _entry.Capture.Height);
        int right = Math.Clamp(rect.Right - bounds.Left, 0, _entry.Capture.Width);
        int bottom = Math.Clamp(rect.Bottom - bounds.Top, 0, _entry.Capture.Height);
        if (right <= left || bottom <= top) return;

        int width = right - left, height = bottom - top;
        var capFmt = _entry.Capture.IsHdr ? Format.R16G16B16A16_Float : Format.B8G8R8A8_UNorm;
        if (_texture == null || width != Width || height != Height || _texture.Description.Format != capFmt)
        {
            Srv = null;
            _liveSrv?.Dispose();
            _safeSrv?.Dispose();
            _texture?.Dispose();
            _safeTexture?.Dispose();
            var desc = new Texture2DDescription
            {
                Width = (uint)width, Height = (uint)height, MipLevels = 1, ArraySize = 1,
                Format = capFmt, SampleDescription = new SampleDescription(1, 0),
                Usage = ResourceUsage.Default, BindFlags = BindFlags.ShaderResource,
            };
            _texture = gpu.Device.CreateTexture2D(desc);
            _safeTexture = gpu.Device.CreateTexture2D(desc);
            _liveSrv = gpu.Device.CreateShaderResourceView(_texture);
            _safeSrv = gpu.Device.CreateShaderResourceView(_safeTexture);
            Srv = _liveSrv;
            _lastSafeCopy = 0;
            Width = width;
            Height = height;
        }
        // Snapshot the previous clean frame into _safeTexture every ~120 ms so any window dragged onto the game display rolls back cleanly without showing its edge.
        if (_safeTexture != null && Frames > 0 && now - _lastSafeCopy >= Stopwatch.Frequency / 8)
        {
            gpu.Context.CopyResource(_safeTexture, _texture);
            _lastSafeCopy = now;
        }
        using var resource = view.Resource;
        using var image = resource.QueryInterface<ID3D11Texture2D>();
        gpu.Context.CopySubresourceRegion(_texture, 0, 0, 0, 0, image, 0, new Box(left, top, 0, right, bottom, 1));
        if (Frames == 0 && _safeTexture != null)
        {
            gpu.Context.CopyResource(_safeTexture, _texture);
            _lastSafeCopy = now;
        }
        Srv = _liveSrv;
        Frames++;
        _status = $"{Width}×{Height} • DXGI borderless";
    }

    /// <summary>Returns true if any visible, non-cloaked, non-click-through window above <paramref name="target"/> overlaps its bounds (including windows dragged across monitor edges).</summary>
    private static bool IsOccluded(IntPtr target, in Native.RECT targetRect)
    {
        if (targetRect.Right <= targetRect.Left || targetRect.Bottom <= targetRect.Top) return false;
        const long WS_EX_TRANSPARENT = 0x00000020L;
        IntPtr cur = Native.GetWindow(target, Native.GW_HWNDPREV);
        int guard = 0;
        while (cur != IntPtr.Zero && ++guard < 512)
        {
            if (Native.IsWindowVisible(cur) && !Native.IsIconic(cur))
            {
                long exStyle = Native.GetWindowLongPtrW(cur, Native.GWL_EXSTYLE);
                if ((exStyle & (Native.WS_EX_TOOLWINDOW | WS_EX_TRANSPARENT)) == 0 &&
                    (Native.DwmGetWindowAttribute(cur, Native.DWMWA_CLOAKED, out int cloaked, 4) != 0 || cloaked == 0) &&
                    Native.DwmGetWindowAttribute(cur, Native.DWMWA_EXTENDED_FRAME_BOUNDS, out Native.RECT r, 16) == 0)
                {
                    // Check virtual-desktop intersection directly (no MonitorFromWindow filter, so windows dragged from another monitor are caught on the very first pixel).
                    if (r.Right > targetRect.Left && r.Left < targetRect.Right &&
                        r.Bottom > targetRect.Top && r.Top < targetRect.Bottom &&
                        (r.Right - r.Left) >= 16 && (r.Bottom - r.Top) >= 16)
                        return true;
                }
            }
            cur = Native.GetWindow(cur, Native.GW_HWNDPREV);
        }
        return false;
    }

    private void SetTarget(IntPtr target)
    {
        if (target == _target) return;
        Dispose();
        _target = target;
    }

    public override void Dispose()
    {
        _resolving = null;
        if (_entry != null) monitors.Release(_entry);
        _entry = null;
        _target = _monitor = IntPtr.Zero;
        Srv = null;
        _liveSrv?.Dispose();
        _liveSrv = null;
        _safeSrv?.Dispose();
        _safeSrv = null;
        _texture?.Dispose();
        _texture = null;
        _safeTexture?.Dispose();
        _safeTexture = null;
        Width = Height = 0;
    }
}
