using System.Diagnostics;
using System.Numerics;
using TikStream.Capture;
using TikStream.Core;
using TikStream.Gpu;
using Vortice.Direct3D11;
using Vortice.DXGI;
using ID3D11Texture2D = Vortice.Direct3D11.ID3D11Texture2D;

namespace TikStream.Engine;

/// <summary>Źródło obrazu w trakcie działania (posiada urządzenie/plik i teksturę GPU).</summary>
internal abstract class RtSource : IDisposable
{
    public SourceDef Def { get; set; } = null!;
    public string ConfigKey { get; init; } = "";
    public ID3D11ShaderResourceView? Srv { get; protected set; }
    public ID3D11ShaderResourceView? Srv2 { get; protected set; }
    public bool Yuv { get; protected set; }
    public int Width { get; protected set; }
    public int Height { get; protected set; }
    public virtual string Status => Srv != null ? $"{Width}×{Height}" : "brak obrazu";
    public double Fps { get; set; }
    public long Frames { get; protected set; }
    /// <summary>Czy kadrować (cover) do prostokąta elementu. Tekst/obraz/kolor są rozciągane.</summary>
    public virtual bool Croppable => true;
    public virtual bool PreserveAlpha => false;
    public virtual bool IsHdr => false;
    public bool Ready => Srv != null && Width > 0 && Height > 0;

    public virtual void Update(GpuDevice gpu, Compositor comp) { }
    public abstract void Dispose();

    public static string KeyOf(SourceDef d) => d.Type switch
    {
        SourceType.Screen => $"{d.Type}|{d.CaptureId}",
        SourceType.Window or SourceType.Game => $"{d.Type}|{d.CaptureId}|{d.GameMode}|{d.MatchWindowTitle}|{d.CaptureFromDesktop}",
        SourceType.Camera => $"cam|{d.CameraName}|{d.CameraWidth}|{d.CameraHeight}|{d.CameraFps}|{d.CameraFormat}",
        SourceType.Media => $"media|{d.FilePath}|{d.Loop}",
        SourceType.Color => $"color|{d.ColorArgb}",
        _ => "pixels",
    };
}

/// <summary>Dwie tekstury dynamiczne (Y + UV) dla klatek NV12 z kamerki i plików.</summary>
internal sealed unsafe class Nv12Texture : IDisposable
{
    private ID3D11Texture2D? _y, _uv;
    public ID3D11ShaderResourceView? YSrv, UVSrv;
    public int W, H;

    public void Upload(GpuDevice gpu, IntPtr y, IntPtr uv, int w, int h)
    {
        if (_y == null || W != w || H != h)
        {
            Dispose();
            Texture2DDescription D(int tw, int th, Format f) => new()
            {
                Width = (uint)tw, Height = (uint)th, MipLevels = 1, ArraySize = 1, Format = f,
                SampleDescription = new SampleDescription(1, 0), Usage = ResourceUsage.Dynamic,
                BindFlags = BindFlags.ShaderResource, CPUAccessFlags = CpuAccessFlags.Write,
            };
            _y = gpu.Device.CreateTexture2D(D(w, h, Format.R8_UNorm));
            _uv = gpu.Device.CreateTexture2D(D(w / 2, h / 2, Format.R8G8_UNorm));
            YSrv = gpu.Device.CreateShaderResourceView(_y);
            UVSrv = gpu.Device.CreateShaderResourceView(_uv);
            W = w; H = h;
        }
        Copy(gpu.Context, _y, y, w, h);
        Copy(gpu.Context, _uv!, uv, w, h / 2);
    }

    internal static void Copy(ID3D11DeviceContext ctx, ID3D11Texture2D tex, IntPtr data, int rowBytes, int rows)
    {
        var m = ctx.Map(tex, 0, MapMode.WriteDiscard);
        if (m.RowPitch == rowBytes)
            Buffer.MemoryCopy((void*)data, (void*)m.DataPointer, (long)rowBytes * rows, (long)rowBytes * rows);
        else
            for (int i = 0; i < rows; i++)
                Buffer.MemoryCopy((byte*)data + i * rowBytes, (byte*)m.DataPointer + i * m.RowPitch, m.RowPitch, rowBytes);
        ctx.Unmap(tex, 0);
    }

    public void Dispose()
    {
        YSrv?.Dispose(); UVSrv?.Dispose(); _y?.Dispose(); _uv?.Dispose();
        YSrv = UVSrv = null; _y = _uv = null;
    }
}

// ============================ ekran / okno ============================

internal sealed class CaptureRt : RtSource
{
    private DesktopCapture? _cap;
    private long _nextResolve;
    private string _status = "Szukanie źródła…";
    private readonly GpuDevice _gpu;
    private readonly MonitorCapturePool _monitors;
    private MonitorCapturePool.Entry? _monitor;

    public CaptureRt(GpuDevice gpu, SourceDef def, MonitorCapturePool monitors)
    {
        _gpu = gpu;
        _monitors = monitors;
        Def = def;
        TryOpen();
    }

    public override string Status => _cap == null ? _status : _cap.Closed ? "Okno zamknięte" : $"{Width}×{Height} • {_cap.Mode}{(_cap.IsHdr ? " • HDR" : "")}";
    public override bool IsHdr => _cap?.IsHdr ?? false;

    private Task<CaptureSource?>? _resolving;

    /// <summary>
    /// Szukanie monitora/okna (EnumWindows + nazwy procesów) trwa kilka–kilkanaście ms, więc robimy je w tle.
    /// Wcześniej działo się to w wątku renderującym co 2 s, gdy okno gry było zamknięte → przycięcie co 2 s.
    /// </summary>
    private void TryOpen()
    {
        _nextResolve = Stopwatch.GetTimestamp() + Stopwatch.Frequency * 2;
        if (_resolving != null) return;
        var def = Def;
        var factory = _gpu.Factory;
        _resolving = Task.Run(() =>
        {
            var all = DesktopCapture.EnumerateSources(factory);
            if (def.Type == SourceType.Screen)
                return string.IsNullOrEmpty(def.CaptureId)
                    ? all.FirstOrDefault(x => x.Kind == "Monitor" && x.IsPrimary) ?? all.FirstOrDefault(x => x.Kind == "Monitor")
                    : all.FirstOrDefault(x => x.Kind == "Monitor" && x.Id == def.CaptureId);
            return string.IsNullOrEmpty(def.CaptureId) ? null : DesktopCapture.Resolve(all, "Window", def.CaptureId);
        });
    }

    /// <summary>Otwiera przechwytywanie, gdy wyszukiwanie w tle się zakończyło (wątek renderujący).</summary>
    private void FinishOpen()
    {
        if (_resolving == null || !_resolving.IsCompleted) return;
        var task = _resolving;
        _resolving = null;
        try
        {
            var src = task.Result;
            if (src == null) { _status = Def.Type == SourceType.Screen ? "Wybrany monitor jest niedostępny" : "Okno nie jest otwarte — czekam"; return; }
            if (Def.Type == SourceType.Screen)
            {
                _monitor = _monitors.Acquire(_gpu, src);
                _cap = _monitor.Capture;
            }
            else _cap = new DesktopCapture(_gpu, src);
        }
        catch (Exception ex)
        {
            var e = ex is AggregateException ae ? ae.InnerException ?? ex : ex;
            _status = "Błąd: " + e.Message;
            Log.Warn($"Źródło '{Def.Name}': {e.Message}");
        }
    }

    public override void Update(GpuDevice gpu, Compositor comp)
    {
        if (_cap == null || _cap.Closed)
        {
            // okno gry może zostać otwarte później — szukaj co 2 s
            if (_resolving == null && Stopwatch.GetTimestamp() >= _nextResolve)
            {
                Dispose();
                TryOpen();
            }
            FinishOpen();
            if (_cap == null) return;
        }
        if (_monitor != null)
        {
            if (_monitors.Update(_monitor, gpu)) Frames++;
            Srv = _monitor.View(Def.CaptureCursor, gpu, comp);
            Width = _cap.Width;
            Height = _cap.Height;
            return;
        }
        _cap.DrawCursor = Def.CaptureCursor;
        if (_cap.Update(gpu.Context, out var cur))
        {
            Frames++;
            if (cur is CursorDraw c && _cap.Rtv != null)
                comp.DrawOnto(_cap.Rtv, _cap.Width, _cap.Height, new DrawCmd
                {
                    Srv = c.Srv, SrcUv = new Vector4(0, 0, 1, 1), Opacity = 1, Darken = 1,
                    Dst = new NRect((float)c.X / _cap.Width, (float)c.Y / _cap.Height, (float)c.W / _cap.Width, (float)c.H / _cap.Height),
                });
        }
        Srv = _cap.View;
        Width = _cap.Width;
        Height = _cap.Height;
    }

    public override void Dispose()
    {
        if (_monitor != null) _monitors.Release(_monitor);
        else _cap?.Dispose();
        _monitor = null;
        _cap = null;
        Srv = null;
    }
}

// ============================ kamerka ============================

internal sealed class CameraRt : RtSource
{
    private readonly CameraCapture? _cam;
    private readonly Nv12Texture _tex = new();
    private long _seq;

    public CameraRt(SourceDef def, int outputFps)
    {
        Def = def;
        Yuv = true;
        if (!string.IsNullOrEmpty(def.CameraName))
            _cam = new CameraCapture(def.CameraName, new CameraMode(def.CameraWidth, def.CameraHeight, def.CameraFps, def.CameraFormat), outputFps);
    }

    public double? CaptureFps => _cam?.MeasuredFps;
    public override string Status => _cam == null ? "Nie wybrano urządzenia" : $"{_cam.Status} • {_cam.MeasuredFps:F0} fps";

    public override void Update(GpuDevice gpu, Compositor comp)
    {
        if (_cam == null) return;
        if (_cam.TryConsume(ref _seq, (y, uv, w, h) => _tex.Upload(gpu, y, uv, w, h))) Frames++;
        Srv = _tex.YSrv; Srv2 = _tex.UVSrv; Width = _tex.W; Height = _tex.H;
    }

    public override void Dispose()
    {
        var cam = _cam;
        if (cam != null) Task.Run(cam.Dispose); // zamykanie dshow potrafi trwać
        _tex.Dispose();
    }
}

// ============================ multimedia ============================

internal sealed class MediaRt : RtSource
{
    private readonly MediaPlayer? _player;
    private readonly Nv12Texture _tex = new();
    private readonly AudioEngine _audio;
    private long _seq;
    private ID3D11Texture2D? _bgra;
    private ID3D11ShaderResourceView? _bgraView;
    public override bool PreserveAlpha => !Yuv;

    public MediaRt(SourceDef def, AudioEngine audio)
    {
        Def = def;
        Yuv = true;
        _audio = audio;
        if (!string.IsNullOrEmpty(def.FilePath))
        {
            var sink = audio.RegisterPushInput(def.Id, def.Name);
            audio.SetVolume(def.Id, def.Muted ? 0 : def.Volume);
            audio.SetFilters(def.Id, def);
            _player = new MediaPlayer(def.FilePath, def.Loop, sink);
        }
    }

    public override string Status => _player?.Status ?? "Nie wybrano pliku";

    public override void Update(GpuDevice gpu, Compositor comp)
    {
        if (_player == null) return;
        if (_player.TryConsume(ref _seq, (data, uv, w, h, alpha, transparent) => Upload(gpu, data, uv, w, h, alpha, transparent))) Frames++;
    }

    private void Upload(GpuDevice gpu, IntPtr data, IntPtr uv, int w, int h, bool alpha, bool transparent)
    {
        if (transparent)
        {
            Srv = null;
            Srv2 = null;
            Yuv = false;
            Width = w;
            Height = h;
            return;
        }
        if (alpha)
        {
            if (_bgra == null || _bgra.Description.Width != w || _bgra.Description.Height != h)
            {
                _bgraView?.Dispose();
                _bgra?.Dispose();
                _bgra = gpu.Device.CreateTexture2D(new Texture2DDescription
                {
                    Width = (uint)w, Height = (uint)h, MipLevels = 1, ArraySize = 1,
                    Format = Format.B8G8R8A8_UNorm, SampleDescription = new SampleDescription(1, 0),
                    Usage = ResourceUsage.Dynamic, BindFlags = BindFlags.ShaderResource, CPUAccessFlags = CpuAccessFlags.Write,
                });
                _bgraView = gpu.Device.CreateShaderResourceView(_bgra);
            }
            Nv12Texture.Copy(gpu.Context, _bgra, data, w * 4, h);
            Srv = _bgraView;
            Srv2 = null;
        }
        else
        {
            _tex.Upload(gpu, data, uv, w, h);
            Srv = _tex.YSrv;
            Srv2 = _tex.UVSrv;
        }
        Yuv = !alpha;
        Width = w;
        Height = h;
    }

    public override void Dispose()
    {
        _player?.Dispose();
        _audio.Unregister(Def.Id);
        _tex.Dispose();
        _bgraView?.Dispose();
        _bgra?.Dispose();
    }
}

// ============================ obraz / tekst (piksele z UI) ============================

internal sealed unsafe class PixelsRt : RtSource
{
    private ID3D11Texture2D? _tex;
    public override bool Croppable => false;
    public override bool PreserveAlpha => true;
    public override string Status => Srv != null ? $"{Width}×{Height} px" : (Def.Type == SourceType.Image ? "Nie wybrano pliku" : "—");

    public PixelsRt(SourceDef def) { Def = def; }

    public void Set(GpuDevice gpu, Compositor comp, int w, int h, byte[] bgra)
    {
        if (w <= 0 || h <= 0)
        {
            Srv?.Dispose(); _tex?.Dispose();
            Srv = null; _tex = null; Width = Height = 0;
            return;
        }
        if (_tex == null || Width != w || Height != h)
        {
            Srv?.Dispose(); _tex?.Dispose();
            _tex = gpu.Device.CreateTexture2D(new Texture2DDescription
            {
                Width = (uint)w, Height = (uint)h, MipLevels = 1, ArraySize = 1,
                Format = Format.B8G8R8A8_UNorm, SampleDescription = new SampleDescription(1, 0),
                Usage = ResourceUsage.Dynamic, BindFlags = BindFlags.ShaderResource, CPUAccessFlags = CpuAccessFlags.Write,
            });
            Srv = gpu.Device.CreateShaderResourceView(_tex);
            Width = w; Height = h;
        }
        fixed (byte* p = bgra) Nv12Texture.Copy(gpu.Context, _tex, (IntPtr)p, w * 4, h);
    }

    public override void Dispose() { Srv?.Dispose(); _tex?.Dispose(); }
}

// ============================ kolor ============================

internal sealed class ColorRt : RtSource
{
    private readonly ID3D11Texture2D _tex;
    public override bool Croppable => false;
    public override string Status => "Jednolity kolor";

    public ColorRt(SourceDef def, Compositor comp, GpuDevice gpu)
    {
        Def = def;
        uint c = unchecked((uint)def.ColorArgb);
        byte a = (byte)(c >> 24), r = (byte)(c >> 16), g = (byte)(c >> 8), b = (byte)c;
        _tex = comp.CreateImmutable(1, 1, new[] { (byte)(b * a / 255), (byte)(g * a / 255), (byte)(r * a / 255), a });
        Srv = gpu.Device.CreateShaderResourceView(_tex);
        Width = Height = 1;
    }

    public override void Dispose() { Srv?.Dispose(); _tex.Dispose(); }
}
