using System.Diagnostics;
using System.Runtime.InteropServices;
using NAudio.CoreAudioApi;
using NAudio.CoreAudioApi.Interfaces;
using NAudio.Wasapi.CoreAudioApi.Interfaces;
using NAudio.Wave;
using TikStream.Core;

namespace TikStream.Capture;

internal sealed unsafe class ApplicationAudioCapture(string executable) : IWaveIn
{
    private const int SurroundChannels = 8;
    private const int StereoChannels = 2;
    private const uint SurroundMask = 0x63F; // FL, FR, FC, LFE, BL, BR, SL, SR
    private const uint StereoMask = 0x3;     // FL, FR
    private const float Fold = 0.70710678f;
    private readonly CancellationTokenSource _shutdown = new();
    private Task? _worker;
    public WaveFormat WaveFormat { get; set; } = AudioEngine.MixFormat;
    public event EventHandler<WaveInEventArgs>? DataAvailable;
    public event EventHandler<StoppedEventArgs>? RecordingStopped;

    public void StartRecording() => _worker ??= Task.Run(() => Run(_shutdown.Token));
    public void StopRecording() => _shutdown.Cancel();

    private void Run(CancellationToken stop)
    {
        string? lastFailure = null;
        try
        {
            while (!stop.IsCancellationRequested)
            {
                var apps = Process.GetProcessesByName(executable);
                try
                {
                    var app = SelectTargetProcess(apps);
                    if (app != null)
                    {
                        try { Record(app, stop); }
                        catch (Exception ex) when (!stop.IsCancellationRequested &&
                            ex is COMException or TimeoutException or InvalidOperationException or System.ComponentModel.Win32Exception)
                        {
                            if (lastFailure != ex.Message)
                                Log.Warn($"Application audio '{executable}': {ex.Message} Retrying capture.");
                            lastFailure = ex.Message;
                        }
                    }
                }
                finally { foreach (var app in apps) app.Dispose(); }
                stop.WaitHandle.WaitOne(1000);
            }
        }
        catch (OperationCanceledException) when (stop.IsCancellationRequested) { }
        catch (Exception ex) { RecordingStopped?.Invoke(this, new StoppedEventArgs(ex)); return; }
        RecordingStopped?.Invoke(this, new StoppedEventArgs());
    }

    private void Record(Process app, CancellationToken stop)
    {
        var completion = new Activation();
        var parameters = new ProcessActivation { Type = 1, Pid = (uint)app.Id };
        var props = new ActivationBlob { Variant = 65, Size = sizeof(ProcessActivation), Data = (IntPtr)(&parameters) };
        Marshal.ThrowExceptionForHR(ActivateAudioInterfaceAsync("VAD\\Process_Loopback",
            typeof(IAudioClient).GUID, ref props, completion, out var operation));
        IAudioClient nativeClient;
        try { nativeClient = completion.Result.Task.WaitAsync(TimeSpan.FromSeconds(5), stop).GetAwaiter().GetResult(); }
        finally { GC.KeepAlive(operation); GC.KeepAlive(completion); }
        using var client = new AudioClient(nativeClient);
        using var dataReady = new AutoResetEvent(false);
        int captureChannels = InitializeLoopback(nativeClient);
        Marshal.ThrowExceptionForHR(nativeClient.SetEventHandle(dataReady.SafeWaitHandle.DangerousGetHandle()));
        var capture = client.AudioCaptureClient;
        var waits = new WaitHandle[] { stop.WaitHandle, dataReady };
        byte[] buffer = [];
        float[] surround = [], stereo = [];
        Marshal.ThrowExceptionForHR(nativeClient.Start());
        try
        {
            while (!stop.IsCancellationRequested && !app.HasExited)
            {
                if (WaitHandle.WaitAny(waits, 500) != 1) continue;
                while (!stop.IsCancellationRequested && capture.GetNextPacketSize() > 0)
                {
                    var pointer = capture.GetBuffer(out int frames, out var flags);
                    int bytes = frames * WaveFormat.BlockAlign;
                    try
                    {
                        if (bytes <= 0) continue;
                        // Packet size comes from GetBuffer, not from the virtual device's GetBufferSize.
                        if (buffer.Length < bytes) buffer = new byte[bytes];
                        if ((flags & AudioClientBufferFlags.Silent) != 0) Array.Clear(buffer, 0, bytes);
                        else if (captureChannels == StereoChannels)
                        {
                            Marshal.Copy(pointer, buffer, 0, bytes);
                        }
                        else
                        {
                            int samples = frames * captureChannels;
                            if (surround.Length < samples) surround = new float[samples];
                            if (stereo.Length < frames * 2) stereo = new float[frames * 2];
                            Marshal.Copy(pointer, surround, 0, samples);
                            Downmix(surround, stereo, frames);
                            Buffer.BlockCopy(stereo, 0, buffer, 0, bytes);
                        }
                    }
                    finally { capture.ReleaseBuffer(frames); }
                    DataAvailable?.Invoke(this, new WaveInEventArgs(buffer, bytes));
                }
            }
        }
        finally { nativeClient.Stop(); }
    }

    private static Process? SelectTargetProcess(Process[] apps)
    {
        if (apps.Length <= 1) return apps.FirstOrDefault();
        var pids = new HashSet<uint>(apps.Select(p => (uint)p.Id));
        uint activePid = 0, anySessionPid = 0;
        try
        {
            using var en = new MMDeviceEnumerator();
            foreach (var dev in en.EnumerateAudioEndPoints(DataFlow.Render, DeviceState.Active))
            {
                using (dev)
                {
                    var sessions = dev.AudioSessionManager.Sessions;
                    for (int i = 0; i < sessions.Count; i++)
                    {
                        var s = sessions[i];
                        uint pid = s.GetProcessID;
                        if (!pids.Contains(pid)) continue;
                        if (s.State == AudioSessionState.AudioSessionStateActive) { activePid = pid; break; }
                        if (anySessionPid == 0) anySessionPid = pid;
                    }
                }
                if (activePid != 0) break;
            }
        }
        catch { }
        uint chosen = activePid != 0 ? activePid : anySessionPid;
        if (chosen != 0)
        {
            var match = apps.FirstOrDefault(p => (uint)p.Id == chosen);
            if (match != null) return match;
        }
        return apps.FirstOrDefault(p => p.MainWindowHandle != IntPtr.Zero) ?? apps.FirstOrDefault();
    }

    private static void Downmix(float[] input, float[] output, int frames)
    {
        for (int i = 0; i < frames; i++)
        {
            int offset = i * SurroundChannels;
            float center = input[offset + 2] * Fold;
            output[i * 2] = input[offset] + center + (input[offset + 4] + input[offset + 6]) * Fold;
            output[i * 2 + 1] = input[offset + 1] + center + (input[offset + 5] + input[offset + 7]) * Fold;
        }
    }

    private int InitializeLoopback(IAudioClient client)
    {
        // NAudio passes a pointer to a GUID. Process loopback sessions do not require a session GUID.
        var pointer = Marshal.GetComInterfaceForObject(client, typeof(IAudioClient));
        try
        {
            var initialize = (delegate* unmanaged[Stdcall]<IntPtr, int, uint, long, long, SurroundFormat*, IntPtr, int>)
                (*(IntPtr**)pointer)[3];
            var flags = AudioClientStreamFlags.Loopback | AudioClientStreamFlags.EventCallback | AudioClientStreamFlags.AutoConvertPcm;

            var format8 = MakeFormat(SurroundChannels, SurroundMask);
            int hr = initialize(pointer, (int)AudioClientShareMode.Shared, (uint)flags, 0, 0, &format8, IntPtr.Zero);
            if (hr >= 0) return SurroundChannels;

            var format2 = MakeFormat(StereoChannels, StereoMask);
            int hr2 = initialize(pointer, (int)AudioClientShareMode.Shared, (uint)flags, 0, 0, &format2, IntPtr.Zero);
            if (hr2 >= 0)
            {
                Log.Info($"Application audio '{executable}': fallback loopback 8ch (0x{hr:X8}) -> 2ch stereo.");
                return StereoChannels;
            }
            Marshal.ThrowExceptionForHR(hr);
            return SurroundChannels;
        }
        finally { Marshal.Release(pointer); }
    }

    private static SurroundFormat MakeFormat(ushort channels, uint mask)
    {
        ushort blockAlign = (ushort)(channels * sizeof(float));
        return new SurroundFormat
        {
            Tag = 0xFFFE,
            Channels = channels,
            SampleRate = AudioEngine.SampleRate,
            BytesPerSecond = (uint)(AudioEngine.SampleRate * blockAlign),
            BlockAlign = blockAlign,
            BitsPerSample = 32,
            ExtraSize = 22,
            ValidBits = 32,
            ChannelMask = mask,
            SubFormat = new Guid("00000003-0000-0010-8000-00aa00389b71"),
        };
    }

    public void Dispose()
    {
        _shutdown.Cancel();
        if (_worker == null) _shutdown.Dispose();
        else _ = _worker.ContinueWith(_ => _shutdown.Dispose(), TaskScheduler.Default);
    }

    [ComVisible(true)]
    private sealed class Activation : IActivateAudioInterfaceCompletionHandler
    {
        public readonly TaskCompletionSource<IAudioClient> Result = new(TaskCreationOptions.RunContinuationsAsynchronously);
        public void ActivateCompleted(IActivateAudioInterfaceAsyncOperation operation)
        {
            try
            {
                operation.GetActivateResult(out var result, out var client);
                Marshal.ThrowExceptionForHR(result);
                Result.TrySetResult((IAudioClient)client);
            }
            catch (Exception ex) { Result.TrySetException(ex); }
        }
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct ProcessActivation { public uint Type, Pid, Mode; }

    [StructLayout(LayoutKind.Sequential, Pack = 2)]
    private struct SurroundFormat
    {
        public ushort Tag, Channels;
        public uint SampleRate, BytesPerSecond;
        public ushort BlockAlign, BitsPerSample, ExtraSize, ValidBits;
        public uint ChannelMask;
        public Guid SubFormat;
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct ActivationBlob
    {
        public ushort Variant, Reserved1, Reserved2, Reserved3;
        public int Size;
        public IntPtr Data;
    }

    [DllImport("Mmdevapi.dll", CharSet = CharSet.Unicode, ExactSpelling = true)]
    private static extern int ActivateAudioInterfaceAsync(string path, [MarshalAs(UnmanagedType.LPStruct)] Guid iid,
        ref ActivationBlob parameters, IActivateAudioInterfaceCompletionHandler callback, out IActivateAudioInterfaceAsyncOperation operation);
}
