using System.Runtime.InteropServices;
using System.Text.Json;
using System.Text.Json.Nodes;
using TikStream.Capture;
using TikStream.Core;
using TikStream.Engine;

namespace TikStream;

/// <summary>
/// Engine API for the Qt (C++) UI, loaded in-process via hostfxr.
/// Contract: Call(method, json) -> json (UTF-8, freed via FreeString).
/// Image/text source pixels are uploaded through a dedicated function without JSON encoding.
/// </summary>
public static unsafe class NativeApi
{
    private static volatile StreamEngine? _engine;
    private static AppSettings _settings = new();
    private static Timer? _saveTimer;
    private static readonly object Lock = new();
    private static readonly TikTokLogin TikTok = new();
    private static JsonSerializerOptions Json => AppSettings.JsonOpts;

    [UnmanagedCallersOnly]
    public static IntPtr Call(IntPtr method, IntPtr args)
    {
        string m = Marshal.PtrToStringUTF8(method) ?? "";
        string a = Marshal.PtrToStringUTF8(args) ?? "";
        if (string.IsNullOrWhiteSpace(a)) a = "{}";
        string result;
        try
        {
            // Read-only queries and render-queue commands do not wait on settings saves or output operations.
            if (m is "listCameras" or "listAudioDevices" or "levels" or "stats" or "setItemTransform" or "setSelection"
                  or "checkUpdate" or "startUpdate" or "updateStatus" or "installUpdate" or "appVersion"
                  or "browserFrame" or "browserInteract" or "playAlertSound")
                result = Dispatch(m, a);
            else lock (Lock) result = Dispatch(m, a);
        }
        catch (Exception ex)
        {
            Log.Error($"API {m}", ex);
            result = JsonSerializer.Serialize(new { error = ex.Message });
        }
        return Marshal.StringToCoTaskMemUTF8(result);
    }

    [UnmanagedCallersOnly]
    public static void FreeString(IntPtr p) => Marshal.FreeCoTaskMem(p);

    [UnmanagedCallersOnly]
    public static void SetSourcePixels(IntPtr idUtf8, int w, int h, int stride, byte* bgraPremul)
    {
        try
        {
            var id = Marshal.PtrToStringUTF8(idUtf8) ?? "";
            var data = new byte[Math.Max(0, w * h * 4)];
            if (bgraPremul != null)
                fixed (byte* dst = data)
                    for (int y = 0; y < h; y++)
                        Buffer.MemoryCopy(bgraPremul + (long)y * stride, dst + (long)y * w * 4, w * 4, w * 4);
            _engine?.SetSourcePixels(id, w, h, data);
        }
        catch (Exception ex) { Log.Error("SetSourcePixels", ex); }
    }

    private static string Ok(object? extra = null) => JsonSerializer.Serialize(extra ?? new { ok = true }, Json);

    private static NRect ReadRect(JsonObject j)
        => new(j["x"]!.GetValue<float>(), j["y"]!.GetValue<float>(), j["w"]!.GetValue<float>(), j["h"]!.GetValue<float>());

    private static string Dispatch(string method, string args)
    {
        var j = JsonNode.Parse(args) as JsonObject ?? new JsonObject();
        switch (method)
        {
            case "init":
            {
                if (_engine != null) return Ok(new { ok = true, gpu = _engine.Gpu.AdapterName, canvasWidth = _engine.CanvasWidth, canvasHeight = _engine.CanvasHeight });
                var dir = Path.GetDirectoryName(typeof(NativeApi).Assembly.Location)!;
                FF.Init(Path.Combine(dir, "native"));
                Log.Info($"TikStream Engine, .NET {Environment.Version}");
                _settings = AppSettings.Load();
                _settings.Save();
                _engine = new StreamEngine(_settings);
                return Ok(new { ok = true, gpu = _engine.Gpu.AdapterName, canvasWidth = _engine.CanvasWidth, canvasHeight = _engine.CanvasHeight });
            }

            case "getSettings":
            {
                var node = JsonSerializer.SerializeToNode(_settings, Json)!.AsObject();
                node.Remove("streamKeyProtected");
                node.Remove("secondStreamKeyProtected");
                node["streamKey"] = _settings.StreamKey;
                node["secondStreamKey"] = _settings.SecondStreamKey;
                return node.ToJsonString();
            }

            case "setSettings":
            {
                var s = JsonSerializer.Deserialize<AppSettings>(args, Json) ?? new AppSettings();
                s.StreamKey = j["streamKey"]?.GetValue<string>() ?? _settings.StreamKey;
                s.SecondStreamKey = j["secondStreamKey"]?.GetValue<string>() ?? _settings.SecondStreamKey;
                if (s.Scenes.Count == 0) s.Scenes = _settings.Scenes; // guard against empty state
                s.NormalizeVideo();
                Engine().ApplySettings(s, _settings);
                _settings = s;
                ScheduleSave();
                return Ok();
            }

            case "listSources":
                return JsonSerializer.Serialize(
                    DesktopCapture.EnumerateSources(Engine().Gpu.Factory).Select(x => new { kind = x.Kind, id = x.Id, label = x.Label, isPrimary = x.IsPrimary }), Json);

            case "listCameras":
                return JsonSerializer.Serialize(CameraCapture.Enumerate().Select(c => new
                {
                    name = c.Name,
                    modes = c.Modes.Select(md => new { width = md.Width, height = md.Height, fps = md.Fps, format = md.Format, label = md.Label }),
                }), Json);

            case "listAudioDevices":
            {
                var (ins, outs) = AudioEngine.EnumerateDevices();
                return JsonSerializer.Serialize(new
                {
                    inputs = ins.Select(x => new { id = x.Id, name = x.Name }),
                    outputs = outs.Select(x => new { id = x.Id, name = x.Name }),
                    defaultInput = AudioEngine.DefaultDeviceId(NAudio.CoreAudioApi.DataFlow.Capture),
                    defaultOutput = AudioEngine.DefaultDeviceId(NAudio.CoreAudioApi.DataFlow.Render),
                    needsSync = _engine?.Audio.NeedsSync == true,
                }, Json);
            }

            case "refreshAudioDevices":
                Engine().Audio.SyncDevices(_settings);
                return Ok();

            case "listAudioApplications":
            {
                var names = new SortedSet<string>(StringComparer.CurrentCultureIgnoreCase);
                try
                {
                    using var en = new NAudio.CoreAudioApi.MMDeviceEnumerator();
                    foreach (var dev in en.EnumerateAudioEndPoints(NAudio.CoreAudioApi.DataFlow.Render, NAudio.CoreAudioApi.DeviceState.Active))
                    {
                        using (dev)
                        {
                            var sessions = dev.AudioSessionManager.Sessions;
                            for (int i = 0; i < sessions.Count; i++)
                            {
                                uint pid = sessions[i].GetProcessID;
                                if (pid == 0 || pid == Environment.ProcessId) continue;
                                try
                                {
                                    using var p = System.Diagnostics.Process.GetProcessById((int)pid);
                                    string name = p.ProcessName;
                                    if (string.Equals(name, "audiodg", StringComparison.OrdinalIgnoreCase) ||
                                        string.Equals(name, "SteelSeriesSonar", StringComparison.OrdinalIgnoreCase))
                                        continue;
                                    names.Add(name);
                                }
                                catch { }
                            }
                        }
                    }
                }
                catch { }
                return JsonSerializer.Serialize(names, Json);
            }

            case "setPreview":
                Engine().SetPreviewWindow(new IntPtr(j["hwnd"]!.GetValue<long>()), j["w"]!.GetValue<int>(), j["h"]!.GetValue<int>());
                return Ok();

            case "setDualPreview":
                Engine().SetDualPreviewWindow(new IntPtr(j["hwnd"]!.GetValue<long>()), j["w"]!.GetValue<int>(), j["h"]!.GetValue<int>());
                return Ok();

            case "setSelection":
                Engine().SetSelection(j.ContainsKey("x") ? ReadRect(j) : null);
                return Ok();

            case "setItemTransform":
                Engine().SetItemTransform(j["sceneId"]!.GetValue<string>(), j["itemId"]!.GetValue<string>(), ReadRect(j),
                    Enum.Parse<FitMode>(j["fit"]!.GetValue<string>()));
                return Ok();

            case "beginTikTokLogin": return Ok(TikTok.Begin());
            case "configureTikTokSandbox":
                return Ok(TikTok.Configure(j["key"]?.GetValue<string>() ?? "", j["secret"]?.GetValue<string>() ?? ""));
            case "tiktokLoginStatus": return Ok(TikTok.Status());
            case "clearTikTokLogin": TikTok.Clear(); return Ok();
            case "startStream": return Result(Engine().StartStreamAsync());
            case "stopStream": return Result(Engine().StopStreamAsync());
            case "startRecord": return Result(Engine().StartRecordAsync());
            case "stopRecord": return Result(Engine().StopRecordAsync());
            case "startReplay": return Result(Engine().StartReplayAsync());
            case "stopReplay": return Result(Engine().StopReplayAsync());
            case "saveReplay":
            {
                var task = Engine().SaveReplayAsync();
                if (!task.Wait(30000)) return JsonSerializer.Serialize(new { error = "Replay save timed out" }, Json);
                var (path, err) = task.Result;
                return err == null ? Ok(new { ok = true, path }) : JsonSerializer.Serialize(new { error = err }, Json);
            }

            case "levels":
                return JsonSerializer.Serialize(Engine().Audio.Levels, Json);

            case "stats":
                return JsonSerializer.Serialize(Engine().Stats, Json);

            case "clearError":
                Engine().ClearError();
                return Ok();

            case "logPath":
                return Ok(new { path = Log.FilePath });

            case "appVersion":
                return Ok(new { version = AutoUpdater.CurrentVersion });

            case "checkUpdate":
            {
                var t = AutoUpdater.CheckAsync();
                if (!t.Wait(15000)) return JsonSerializer.Serialize(new { currentVersion = AutoUpdater.CurrentVersion, error = "Update check timed out." }, Json);
                return JsonSerializer.Serialize(t.Result, Json);
            }

            case "startUpdate":
            {
                var err = AutoUpdater.StartDownload(j["url"]?.GetValue<string>() ?? "");
                return err == null ? Ok() : JsonSerializer.Serialize(new { error = err }, Json);
            }

            case "updateStatus":
                return JsonSerializer.Serialize(AutoUpdater.GetStatus(), Json);

            case "installUpdate":
            {
                var err = AutoUpdater.LaunchInstaller();
                return err == null ? Ok() : JsonSerializer.Serialize(new { error = err }, Json);
            }

            case "browserFrame":
            {
                var id = j["id"]?.GetValue<string>() ?? "";
                var bmp = Engine().GetBrowser(id)?.SnapshotBmp();
                return bmp == null ? Ok(new { ok = false }) : Ok(new { ok = true, bmp = Convert.ToBase64String(bmp) });
            }

            case "browserInteract":
            {
                var id = j["id"]?.GetValue<string>() ?? "";
                var b = Engine().GetBrowser(id);
                if (b == null) return Ok(new { ok = false });
                var kind = j["kind"]?.GetValue<string>() ?? "";
                if (kind == "move") b.SendMouseMove(j["x"]?.GetValue<int>() ?? 0, j["y"]?.GetValue<int>() ?? 0, j["leave"]?.GetValue<bool>() ?? false);
                else if (kind == "click") b.SendMouseClick(j["x"]?.GetValue<int>() ?? 0, j["y"]?.GetValue<int>() ?? 0, j["button"]?.GetValue<int>() ?? 0, j["up"]?.GetValue<bool>() ?? false, j["clicks"]?.GetValue<int>() ?? 1);
                else if (kind == "wheel") b.SendMouseWheel(j["x"]?.GetValue<int>() ?? 0, j["y"]?.GetValue<int>() ?? 0, j["dx"]?.GetValue<int>() ?? 0, j["dy"]?.GetValue<int>() ?? 0);
                else if (kind == "key") b.SendKey(j["ev"]?.GetValue<string>() ?? "down", j["vk"]?.GetValue<int>() ?? 0, j["ch"]?.GetValue<int>() ?? 0);
                return Ok();
            }

            case "playAlertSound":
            {
                var sourceId = j["sourceId"]?.GetValue<string>() ?? "";
                var path = j["path"]?.GetValue<string>() ?? "";
                var vol = j["volume"]?.GetValue<float>() ?? 1f;
                Engine().Audio.PlayAlertSound(sourceId, path, vol);
                return Ok();
            }

            case "shutdown":
                TikTok.Clear();
                _saveTimer?.Dispose();
                _settings.Save();
                _engine?.Dispose();
                _engine = null;
                Log.Flush();
                return Ok();

            default:
                throw new InvalidOperationException("Unknown method: " + method);
        }
    }

    private static string Result(Task<string?> t)
    {
        var err = t.Wait(15000) ? t.Result : "Operation timed out";
        return err == null ? Ok() : JsonSerializer.Serialize(new { error = err }, Json);
    }

    private static StreamEngine Engine() => _engine ?? throw new InvalidOperationException("Engine is not initialized.");

    private static void ScheduleSave()
    {
        _saveTimer ??= new Timer(_ => { lock (Lock) _settings.Save(); });
        _saveTimer.Change(1000, Timeout.Infinite);
    }
}
