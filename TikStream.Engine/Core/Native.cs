using System.Runtime.InteropServices;
using System.Text;

namespace TikStream.Core;

internal static partial class Native
{
    // ---------- High-resolution waitable timer (Win10 1803+) ----------
    private const uint CREATE_WAITABLE_TIMER_HIGH_RESOLUTION = 0x00000002;
    private const uint TIMER_ALL_ACCESS = 0x1F0003;

    [DllImport("kernel32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
    public static extern IntPtr CreateWaitableTimerExW(IntPtr attrs, string? name, uint flags, uint access);

    [DllImport("kernel32.dll", SetLastError = true)]
    public static extern bool SetWaitableTimer(IntPtr timer, ref long dueTime, int period, IntPtr cb, IntPtr arg, bool resume);

    [DllImport("kernel32.dll", SetLastError = true)]
    public static extern uint WaitForSingleObject(IntPtr handle, uint ms);

    [DllImport("kernel32.dll", SetLastError = true)]
    public static extern bool CloseHandle(IntPtr handle);

    public static IntPtr CreateHighResTimer()
    {
        var h = CreateWaitableTimerExW(IntPtr.Zero, null, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
        if (h == IntPtr.Zero) h = CreateWaitableTimerExW(IntPtr.Zero, null, 0, TIMER_ALL_ACCESS);
        return h;
    }

    [DllImport("winmm.dll")] public static extern uint timeBeginPeriod(uint p);
    [DllImport("winmm.dll")] public static extern uint timeEndPeriod(uint p);

    // ---------- MMCSS (multimedia thread priority) ----------
    [DllImport("avrt.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    public static extern IntPtr AvSetMmThreadCharacteristicsW(string task, ref uint index);

    [DllImport("avrt.dll", SetLastError = true)]
    public static extern bool AvRevertMmThreadCharacteristics(IntPtr handle);

    // ---------- Process GPU scheduling priority (full effect when elevated) ----------
    [DllImport("gdi32.dll")]
    public static extern int D3DKMTSetProcessSchedulingPriorityClass(IntPtr process, int priorityClass);
    public const int D3DKMT_SCHEDULINGPRIORITYCLASS_HIGH = 4;
    public const int D3DKMT_SCHEDULINGPRIORITYCLASS_REALTIME = 5;

    /// <summary>Checks the registry to see whether Hardware-Accelerated GPU Scheduling (HAGS) is enabled (HwSchMode == 2).</summary>
    public static bool IsHagsEnabled()
    {
        try
        {
            using var key = Microsoft.Win32.Registry.LocalMachine.OpenSubKey(@"SYSTEM\CurrentControlSet\Control\GraphicsDrivers");
            return key?.GetValue("HwSchMode") is int v && v == 2;
        }
        catch { return false; }
    }

    [DllImport("kernel32.dll")] public static extern IntPtr GetCurrentProcess();

    // ---------- Windows ----------
    public delegate bool EnumWindowsProc(IntPtr hWnd, IntPtr lParam);

    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumWindowsProc cb, IntPtr lParam);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr hWnd);
    [DllImport("user32.dll")] public static extern bool IsWindow(IntPtr hWnd);
    [DllImport("user32.dll")] public static extern bool IsIconic(IntPtr hWnd);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern int GetWindowTextW(IntPtr hWnd, StringBuilder sb, int max);
    [DllImport("user32.dll")] public static extern int GetWindowTextLengthW(IntPtr hWnd);
    [DllImport("user32.dll")] public static extern IntPtr GetWindow(IntPtr hWnd, uint cmd);
    [DllImport("user32.dll")] public static extern long GetWindowLongPtrW(IntPtr hWnd, int index);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr hWnd, out uint pid);
    [DllImport("user32.dll")] public static extern IntPtr MonitorFromPoint(System.Drawing.Point pt, uint flags);
    [DllImport("dwmapi.dll")] public static extern int DwmGetWindowAttribute(IntPtr hwnd, int attr, out int value, int size);

    public const uint GW_HWNDPREV = 3;
    public const uint GW_OWNER = 4;
    public const int GWL_EXSTYLE = -20;
    public const long WS_EX_TOOLWINDOW = 0x80;
    public const int DWMWA_CLOAKED = 14;

    public static string GetWindowTitle(IntPtr h)
    {
        int len = GetWindowTextLengthW(h);
        if (len <= 0) return "";
        var sb = new StringBuilder(len + 1);
        GetWindowTextW(h, sb, sb.Capacity);
        return sb.ToString();
    }

    // ---------- Desktop Duplication ----------
    [StructLayout(LayoutKind.Sequential)]
    public struct RECT { public int Left, Top, Right, Bottom; }

    public const int DWMWA_EXTENDED_FRAME_BOUNDS = 9;

    [DllImport("dwmapi.dll")] public static extern int DwmGetWindowAttribute(IntPtr hwnd, int attr, out RECT value, int size);
    [DllImport("user32.dll")] public static extern IntPtr MonitorFromWindow(IntPtr hwnd, uint flags);
}

/// <summary>Precise wait until a target timestamp: high-resolution timer + short spin finish.</summary>
internal sealed class PreciseWaiter : IDisposable
{
    private readonly IntPtr _timer = Native.CreateHighResTimer();

    public void WaitUntil(System.Diagnostics.Stopwatch clock, long targetTicks)
    {
        long freq = System.Diagnostics.Stopwatch.Frequency;
        while (true)
        {
            long remaining = targetTicks - clock.ElapsedTicks;
            if (remaining <= 0) return;
            double ms = remaining * 1000.0 / freq;
            if (ms > 0.5 && _timer != IntPtr.Zero)
            {
                long due = -(long)((ms - 0.2) * 10_000);
                if (Native.SetWaitableTimer(_timer, ref due, 0, IntPtr.Zero, IntPtr.Zero, false))
                    Native.WaitForSingleObject(_timer, 0xFFFFFFFF);
                else Thread.Sleep(1);
            }
            else if (ms > 0.2) Thread.Yield();
            else Thread.SpinWait(50);
        }
    }

    public void Dispose()
    {
        if (_timer != IntPtr.Zero) Native.CloseHandle(_timer);
    }
}
