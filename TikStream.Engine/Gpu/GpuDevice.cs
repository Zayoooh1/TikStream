using TikStream.Core;
using Vortice.Direct3D;
using Vortice.Direct3D11;
using Vortice.DXGI;

namespace TikStream.Gpu;

/// <summary>
/// D3D11 device explicitly created on the NVIDIA adapter (important on Optimus laptops —
/// Windows defaults to Intel UHD, whereas NVENC is only available on RTX).
/// </summary>
public sealed class GpuDevice : IDisposable
{
    public IDXGIFactory2 Factory { get; }
    public IDXGIAdapter1 Adapter { get; }
    public ID3D11Device Device { get; }
    public ID3D11DeviceContext Context { get; }
    public string AdapterName { get; }
    public bool IsNvidia { get; }

    public GpuDevice()
    {
        Factory = DXGI.CreateDXGIFactory1<IDXGIFactory2>();

        IDXGIAdapter1? best = null;
        ulong bestMem = 0;
        bool bestNv = false;
        for (uint i = 0; Factory.EnumAdapters1(i, out var a).Success; i++)
        {
            var d = a.Description1;
            if ((d.Flags & AdapterFlags.Software) != 0) { a.Dispose(); continue; }
            bool nv = d.VendorId == 0x10DE;
            ulong mem = (ulong)d.DedicatedVideoMemory;
            Log.Info($"GPU {i}: {d.Description} (vendor 0x{d.VendorId:X4}, VRAM {mem / (1024 * 1024)} MB)");
            if (best == null || (nv && !bestNv) || (nv == bestNv && mem > bestMem))
            {
                best?.Dispose();
                best = a; bestMem = mem; bestNv = nv;
            }
            else a.Dispose();
        }
        if (best == null) throw new InvalidOperationException("No graphics adapter found.");

        Adapter = best;
        AdapterName = best.Description1.Description;
        IsNvidia = bestNv;
        if (!IsNvidia) Log.Warn("NVIDIA GPU not found — NVENC may not work.");

        var levels = new[] { FeatureLevel.Level_11_1, FeatureLevel.Level_11_0 };
        D3D11.D3D11CreateDevice(Adapter, DriverType.Unknown, DeviceCreationFlags.BgraSupport | DeviceCreationFlags.VideoSupport,
            levels, out ID3D11Device? dev, out ID3D11DeviceContext? ctx).CheckError();
        Device = dev!;
        Context = ctx!;

        using (var mt = Device.QueryInterface<ID3D11Multithread>())
            mt.SetMultithreadProtected(true);

        // GPU priority: compositor must not queue behind the game, otherwise frames drop.
        try
        {
            using var dxgiDev = Device.QueryInterface<IDXGIDevice>();
            dxgiDev.SetGPUThreadPriority(7);
            Log.Info("GPU thread priority set to +7.");
        }
        catch (Exception ex) { Log.Warn("SetGPUThreadPriority: " + ex.Message); }
        try
        {
            bool hags = Native.IsHagsEnabled();
            // When HAGS is enabled, REALTIME priority can degrade smoothness (OBS fix 9de6b18d) -> use HIGH instead.
            int desired = hags ? Native.D3DKMT_SCHEDULINGPRIORITYCLASS_HIGH : Native.D3DKMT_SCHEDULINGPRIORITYCLASS_REALTIME;
            string desiredName = hags ? "HIGH" : "REALTIME";
            int hr = Native.D3DKMTSetProcessSchedulingPriorityClass(Native.GetCurrentProcess(), desired);
            if (hr != 0 && !hags)
            {
                hr = Native.D3DKMTSetProcessSchedulingPriorityClass(Native.GetCurrentProcess(), Native.D3DKMT_SCHEDULINGPRIORITYCLASS_HIGH);
                desiredName = "HIGH";
            }
            Log.Info(hr == 0
                ? $"Process GPU scheduling priority: {desiredName} (HAGS={(hags ? "ON" : "OFF")})."
                : $"Process GPU priority {desiredName} unavailable (0x{hr:X8}, HAGS={(hags ? "ON" : "OFF")}) — run as administrator for best smoothness.");
        }
        catch (Exception ex) { Log.Warn("D3DKMTSetProcessSchedulingPriorityClass: " + ex.Message); }

        Log.Info($"D3D11 device on: {AdapterName}, feature level {Device.FeatureLevel}");
    }

    public void Dispose()
    {
        Context.ClearState();
        Context.Flush();
        Context.Dispose();
        Device.Dispose();
        Adapter.Dispose();
        Factory.Dispose();
    }
}
