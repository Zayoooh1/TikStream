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
    private ID3D11Texture2D? _texture;
    private long _nextResolve;
    private Task<IntPtr>? _resolving;
    private string _status = "Oczekiwanie na okno…";
    public override string Status => _status;
    public override bool IsHdr => _entry?.Capture.IsHdr ?? false;

    public override void Update(GpuDevice gpu, Compositor comp)
    {
        try { UpdateDesktop(gpu, comp); }
        catch (Exception ex)
        {
            Dispose();
            _nextResolve = Stopwatch.GetTimestamp() + Stopwatch.Frequency;
            _status = "Nie można przechwycić obrazu monitora";
            Log.Warn($"Przechwytywanie '{Def.Name}': {ex.Message}");
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
                var target = WindowCaptureRt.ResolveTarget(Def);
                if (target != IntPtr.Zero || !Native.IsWindow(_target)) SetTarget(target);
            }
            else if (_resolving == null && !string.IsNullOrEmpty(Def.CaptureId))
            {
                // Enumeracja okien i procesów potrafi zablokować się na setki ms.
                var def = Def;
                _resolving = Task.Run(() => WindowCaptureRt.ResolveTarget(def));
            }
        }

        if (_target == IntPtr.Zero || !Native.IsWindow(_target))
        {
            _status = "Oczekiwanie na wybrane okno…";
            return;
        }
        if (Native.IsIconic(_target) || WindowCaptureRt.GetForegroundWindow() != _target)
        {
            _status = Ready ? "Okno w tle • ostatnia klatka" : "Przełącz się na wybrane okno";
            return;
        }
        if (Native.DwmGetWindowAttribute(_target, Native.DWMWA_EXTENDED_FRAME_BOUNDS,
                out Native.RECT rect, 16) != 0) return;

        var monitor = Native.MonitorFromWindow(_target, 2);
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
            Srv?.Dispose();
            _texture?.Dispose();
            _texture = gpu.Device.CreateTexture2D(new Texture2DDescription
            {
                Width = (uint)width, Height = (uint)height, MipLevels = 1, ArraySize = 1,
                Format = capFmt, SampleDescription = new SampleDescription(1, 0),
                Usage = ResourceUsage.Default, BindFlags = BindFlags.ShaderResource,
            });
            Srv = gpu.Device.CreateShaderResourceView(_texture);
            Width = width;
            Height = height;
        }
        using var resource = view.Resource;
        using var image = resource.QueryInterface<ID3D11Texture2D>();
        gpu.Context.CopySubresourceRegion(_texture, 0, 0, 0, 0, image, 0, new Box(left, top, 0, right, bottom, 1));
        Frames++;
        _status = $"{Width}×{Height} • DXGI bez ramki";
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
        Srv?.Dispose();
        Srv = null;
        _texture?.Dispose();
        _texture = null;
        Width = Height = 0;
    }
}
