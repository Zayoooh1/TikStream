using System.Numerics;
using TikStream.Capture;
using TikStream.Core;
using TikStream.Gpu;
using Vortice.Direct3D11;

namespace TikStream.Engine;

// DXGI pozwala procesowi otworzyć tylko jedną duplikację danego wyjścia.
// Warstwy dzielą klatkę monitora, zachowując własne kadrowanie i opcję kursora.
internal sealed class MonitorCapturePool
{
    private readonly Dictionary<IntPtr, Entry> _entries = new();
    private long _frame;
    private bool _needsSweep;

    public Entry Acquire(GpuDevice gpu, CaptureSource source)
    {
        if (!_entries.TryGetValue(source.Handle, out var entry))
            _entries.Add(source.Handle, entry = new Entry(new DesktopCapture(gpu, source)));
        entry.Users++;
        return entry;
    }

    public bool Update(Entry entry, GpuDevice gpu) => entry.Update(_frame, gpu);

    public void Release(Entry entry)
    {
        if (--entry.Users == 0) _needsSweep = true;
    }

    public void EndFrame()
    {
        _frame++;
        if (!_needsSweep) return;
        _needsSweep = false;
        foreach (var handle in _entries.Where(pair => pair.Value.Users == 0).Select(pair => pair.Key).ToArray())
        {
            _entries[handle].Dispose();
            _entries.Remove(handle);
        }
    }

    internal sealed class Entry(DesktopCapture capture) : IDisposable
    {
        public int Users;
        public DesktopCapture Capture { get; } = capture;
        private long _frame = -1;
        private bool _updated;
        private CursorDraw? _cursor;
        private bool _cursorRendered;
        private ID3D11Texture2D? _texture;
        private ID3D11RenderTargetView? _rtv;
        private ID3D11ShaderResourceView? _view;

        public bool Update(long frame, GpuDevice gpu)
        {
            if (_frame == frame) return _updated;
            _frame = frame;
            _updated = Capture.Update(gpu.Context, out var cursor);
            if (_updated)
            {
                _cursor = cursor;
                _cursorRendered = false;
            }
            return _updated;
        }

        public ID3D11ShaderResourceView? View(bool drawCursor, GpuDevice gpu, Compositor comp)
        {
            if (Capture.View == null || !drawCursor || _cursor is not CursorDraw cursor) return Capture.View;
            if (_cursorRendered) return _view;

            using var source = Capture.View.Resource;
            using var image = source.QueryInterface<ID3D11Texture2D>();
            var desc = image.Description;
            if (_texture == null || _texture.Description.Width != desc.Width || _texture.Description.Height != desc.Height)
            {
                _rtv?.Dispose();
                _view?.Dispose();
                _texture?.Dispose();
                _texture = gpu.Device.CreateTexture2D(desc);
                _rtv = gpu.Device.CreateRenderTargetView(_texture);
                _view = gpu.Device.CreateShaderResourceView(_texture);
            }
            gpu.Context.CopyResource(_texture, image);
            int w = Capture.Width, h = Capture.Height;
            comp.DrawOnto(_rtv!, w, h, new DrawCmd
            {
                Srv = cursor.Srv, SrcUv = new Vector4(0, 0, 1, 1), Opacity = 1, Darken = 1,
                Dst = new NRect((float)cursor.X / w, (float)cursor.Y / h, (float)cursor.W / w, (float)cursor.H / h),
            });
            _cursorRendered = true;
            return _view;
        }

        public void Dispose()
        {
            Capture.Dispose();
            _rtv?.Dispose();
            _view?.Dispose();
            _texture?.Dispose();
        }
    }
}
