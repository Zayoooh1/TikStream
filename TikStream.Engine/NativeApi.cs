using System.Runtime.InteropServices;
using System.Text.Json;
using System.Text.Json.Nodes;
using TikStream.Capture;
using TikStream.Core;
using TikStream.Engine;
using TikStream.TikTok;

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
    private static readonly TikTokAuthService TikTokAuth = new();
    private static string _activeTikTokRoomId = "";
    private static string _activeTikTokRtmpUrl = "";
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
                  or "setDualItemTransform" or "setDualSelection" or "setItemCrop"
                  or "checkUpdate" or "startUpdate" or "updateStatus" or "installUpdate" or "appVersion"
                  or "browserFrame" or "browserInteract" or "playAlertSound"
                  or "listTtsVoices" or "ttsSpeak" or "ttsSkip" or "ttsClear"
                  or "playSoundboardPad" or "stopSoundboard" or "getNowPlaying"
                  or "tiktokAccountStatus" or "tiktokQrBegin" or "tiktokQrPoll" or "tiktokQrCancel"
                  or "tiktokImportStudio" or "tiktokLogout" or "tiktokCheckCreateInfo" or "tiktokCreateRoom" or "tiktokFinishRoom")
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
                node.Remove("tikTokSignerKeyProtected");
                node.Remove("tikTokSessionProtected");
                node["streamKey"] = _settings.StreamKey;
                node["secondStreamKey"] = _settings.SecondStreamKey;
                node["tikTokSignerKey"] = _settings.TikTokSignerKey;
                return node.ToJsonString();
            }

            case "setSettings":
            {
                var s = JsonSerializer.Deserialize<AppSettings>(args, Json) ?? new AppSettings();
                s.StreamKey = j["streamKey"]?.GetValue<string>() ?? _settings.StreamKey;
                s.SecondStreamKey = j["secondStreamKey"]?.GetValue<string>() ?? _settings.SecondStreamKey;
                s.TikTokSignerKey = j["tikTokSignerKey"]?.GetValue<string>() ?? _settings.TikTokSignerKey;
                s.TikTokSessionProtected = _settings.TikTokSessionProtected;
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

            case "setDualSelection":
                Engine().SetDualSelection(j.ContainsKey("x") ? ReadRect(j) : null);
                return Ok();

            case "setItemTransform":
                Engine().SetItemTransform(j["sceneId"]!.GetValue<string>(), j["itemId"]!.GetValue<string>(), ReadRect(j),
                    Enum.Parse<FitMode>(j["fit"]!.GetValue<string>()));
                return Ok();

            case "setDualItemTransform":
                Engine().SetDualItemTransform(j["sceneId"]!.GetValue<string>(), j["itemId"]!.GetValue<string>(), ReadRect(j),
                    Enum.Parse<FitMode>(j["fit"]!.GetValue<string>()));
                return Ok();

            case "setItemCrop":
                Engine().SetItemCrop(j["sceneId"]!.GetValue<string>(), j["itemId"]!.GetValue<string>(),
                    j["dual"]?.GetValue<bool>() ?? false, ReadRect(j),
                    j["cl"]!.GetValue<float>(), j["cr"]!.GetValue<float>(), j["ct"]!.GetValue<float>(), j["cb"]!.GetValue<float>());
                return Ok();

            case "beginTikTokLogin": return Ok(TikTok.Begin());
            case "configureTikTokSandbox":
                return Ok(TikTok.Configure(j["key"]?.GetValue<string>() ?? "", j["secret"]?.GetValue<string>() ?? ""));
            case "tiktokLoginStatus": return Ok(TikTok.Status());
            case "clearTikTokLogin": TikTok.Clear(); return Ok();

            case "tiktokAccountStatus":
            {
                TikTokSessionData? sess;
                string signerKey, signerUrl;
                lock (Lock)
                {
                    sess = TikTokSessionData.FromJson(_settings.TikTokSessionJson);
                    signerKey = _settings.TikTokSignerKey;
                    signerUrl = _settings.TikTokSignerUrl;
                }
                var signerConfigured = new RapidApiTikTokSigner(signerKey, signerUrl).IsConfigured;
                return Ok(new
                {
                    account = TikTokAuthService.FormatAccount(sess),
                    signerConfigured,
                });
            }

            case "tiktokQrBegin":
            {
                string signerKey = j["signerKey"]?.GetValue<string>() ?? "";
                string signerUrl = j["signerUrl"]?.GetValue<string>() ?? "";
                lock (Lock)
                {
                    if (string.IsNullOrWhiteSpace(signerKey)) signerKey = _settings.TikTokSignerKey;
                    if (string.IsNullOrWhiteSpace(signerUrl)) signerUrl = _settings.TikTokSignerUrl;
                }
                return Ok(TikTokAuth.BeginQrLogin(signerKey, signerUrl, SaveTikTokSession));
            }

            case "tiktokQrPoll":
                return Ok(TikTokAuth.PollQrStatus(SaveTikTokSession));

            case "tiktokQrCancel":
                TikTokAuth.CancelQrLogin();
                return Ok();

            case "tiktokImportStudio":
            {
                var accounts = TikTokStudioImporter.ReadAccounts();
                var chosen = accounts[0];
                var sess = TikTokStudioImporter.ToSession(chosen);
                SaveTikTokSession(sess);
                return Ok(new
                {
                    ok = true,
                    account = TikTokAuthService.FormatAccount(sess),
                    totalFound = accounts.Count,
                });
            }

            case "tiktokLogout":
                TikTokAuth.CancelQrLogin();
                lock (Lock)
                {
                    _activeTikTokRtmpUrl = "";
                    FinishActiveTikTokRoomLocked();
                    _settings.TikTokSessionJson = "";
                    ScheduleSave();
                }
                return Ok();

            case "tiktokCheckCreateInfo":
            {
                TikTokSessionData? sess;
                string signerKey = j["signerKey"]?.GetValue<string>() ?? "";
                string signerUrl = j["signerUrl"]?.GetValue<string>() ?? "";
                lock (Lock)
                {
                    sess = TikTokSessionData.FromJson(_settings.TikTokSessionJson);
                    if (string.IsNullOrWhiteSpace(signerKey)) signerKey = _settings.TikTokSignerKey;
                    if (string.IsNullOrWhiteSpace(signerUrl)) signerUrl = _settings.TikTokSignerUrl;
                }
                if (sess == null || !sess.HasSessionCookie)
                    return JsonSerializer.Serialize(new { ok = false, error = "Brak zalogowanego konta TikTok." }, Json);
                var task = TikTokAuthService.CheckCreateInfoAsync(sess, signerKey, signerUrl);
                if (!task.Wait(30000))
                    return JsonSerializer.Serialize(new { ok = false, error = "Przekroczono czas oczekiwania na webcast/room/create_info/." }, Json);
                var res = task.Result;
                if (res.Ok) SaveTikTokSession(sess); // persist discovered webcast host
                return JsonSerializer.Serialize(res, Json);
            }

            case "tiktokCreateRoom":
            {
                TikTokSessionData? sess;
                string signerKey, signerUrl;
                string title = j["title"]?.GetValue<string>() ?? "";
                string category = j["category"]?.GetValue<string>() ?? "";
                lock (Lock)
                {
                    sess = TikTokSessionData.FromJson(_settings.TikTokSessionJson);
                    signerKey = _settings.TikTokSignerKey;
                    signerUrl = _settings.TikTokSignerUrl;
                }
                if (sess == null || !sess.HasSessionCookie)
                    return JsonSerializer.Serialize(new { ok = false, error = "No active TikTok account session." }, Json);
                var task = TikTokAuthService.CreateRoomAsync(sess, title, category, signerKey, signerUrl);
                if (!task.Wait(30000))
                    return JsonSerializer.Serialize(new { ok = false, error = "Timed out waiting for TikTok webcast/room/create/." }, Json);
                var res = task.Result;
                if (res.Ok && (string.IsNullOrWhiteSpace(res.FullRtmpUrl) || string.IsNullOrWhiteSpace(res.ServerUrl) || string.IsNullOrWhiteSpace(res.StreamKey)))
                    res = res with { Ok = false, Error = "TikTok created the room but did not return a valid RTMP server URL and stream key." };
                if (res.Ok)
                {
                    // Keep the dynamic room RTMP URL in session memory only — never overwrite manual settings or write one-time keys to settings.json.
                    lock (Lock)
                    {
                        _activeTikTokRoomId = res.RoomId;
                        _activeTikTokRtmpUrl = res.FullRtmpUrl;
                    }
                    SaveTikTokSession(sess);
                }
                else
                {
                    lock (Lock)
                    {
                        _activeTikTokRtmpUrl = "";
                        if (_engine?.IsStreaming != true) _activeTikTokRoomId = "";
                    }
                }
                return JsonSerializer.Serialize(res, Json);
            }

            case "tiktokFinishRoom":
            {
                lock (Lock)
                {
                    _activeTikTokRtmpUrl = "";
                    FinishActiveTikTokRoomLocked();
                }
                return Ok();
            }
            case "startStream":
            {
                var overrideUrl = ResolveStreamOverrideUrl(j, out bool usedTikTokRoom);
                if ((j["useTikTokRoom"]?.GetValue<bool>() ?? false) && string.IsNullOrWhiteSpace(overrideUrl))
                    return JsonSerializer.Serialize(new { error = "TikTok LIVE room did not provide an active RTMP URL. Please try again." }, Json);
                var err = RunTask(Engine().StartStreamAsync(overrideUrl));
                if (err != null && usedTikTokRoom)
                {
                    _activeTikTokRtmpUrl = "";
                    FinishActiveTikTokRoomLocked();
                }
                return err == null ? Ok() : JsonSerializer.Serialize(new { error = err }, Json);
            }
            case "stopStream":
            {
                var err = RunTask(Engine().StopStreamAsync());
                _activeTikTokRtmpUrl = "";
                FinishActiveTikTokRoomLocked();
                return err == null ? Ok() : JsonSerializer.Serialize(new { error = err }, Json);
            }
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

            case "listTtsVoices":
            {
                var provider = j["provider"]?.GetValue<string>();
                return JsonSerializer.Serialize(TtsService.ListVoices(provider), Json);
            }

            case "ttsSpeak":
            {
                var text = j["text"]?.GetValue<string>() ?? "";
                var priority = j["priority"]?.GetValue<bool>() ?? false;
                var provider = j["provider"]?.GetValue<string>();
                var voice = j["voice"]?.GetValue<string>();
                float? rate = j["rate"] != null ? j["rate"]!.GetValue<float>() : null;
                float? streamVol = j["volume"] != null ? j["volume"]!.GetValue<float>() : null;
                float? monVol = j["monitorVolume"] != null ? j["monitorVolume"]!.GetValue<float>() : null;
                var monMode = j["monitorMode"]?.GetValue<string>();
                if (streamVol.HasValue || monVol.HasValue || !string.IsNullOrEmpty(monMode))
                    Engine().Audio.SetTtsPreviewGains(streamVol, monVol, monMode);
                Engine().Tts.Enqueue(text, priority, provider, voice, rate);
                return Ok();
            }

            case "ttsSkip":
                Engine().Tts.SkipCurrent();
                return Ok();

            case "ttsClear":
                Engine().Tts.ClearQueue();
                return Ok();

            case "playSoundboardPad":
            {
                var padId = j["id"]?.GetValue<string>() ?? "";
                var path = j["path"]?.GetValue<string>() ?? "";
                var vol = j["volume"]?.GetValue<float>() ?? 1f;
                if (string.IsNullOrWhiteSpace(path) && !string.IsNullOrEmpty(padId))
                {
                    lock (Lock)
                    {
                        var pad = _settings.SoundboardPads.FirstOrDefault(p => p.Id == padId);
                        if (pad != null) { path = pad.FilePath; vol = pad.Volume; }
                    }
                }
                Engine().Audio.PlaySoundboardPad(path, vol);
                return Ok();
            }

            case "stopSoundboard":
                Engine().Audio.StopSoundboard();
                return Ok();

            case "getNowPlaying":
            {
                ulong sinceRev = j["sinceRevision"]?.GetValue<ulong>() ?? 0;
                return JsonSerializer.Serialize(Engine().NowPlaying.GetState(sinceRev), Json);
            }

            case "shutdown":
                TikTok.Clear();
                TikTokAuth.CancelQrLogin();
                _activeTikTokRtmpUrl = "";
                FinishActiveTikTokRoomLocked();
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

    private static string? ResolveStreamOverrideUrl(JsonObject j, out bool usedTikTokRoom)
    {
        usedTikTokRoom = false;
        var explicitRtmp = j["rtmpUrl"]?.GetValue<string>()?.Trim();
        if (!string.IsNullOrEmpty(explicitRtmp))
            return explicitRtmp;

        var explicitServer = j["serverUrl"]?.GetValue<string>()?.Trim() ?? "";
        var explicitKey = j["streamKey"]?.GetValue<string>()?.Trim().TrimStart('/') ?? "";
        if (explicitServer.Length > 0 && explicitKey.Length > 0)
            return explicitServer.TrimEnd('/') + "/" + explicitKey;

        bool requestTikTokRoom = j["useTikTokRoom"]?.GetValue<bool>() ?? false;
        bool hasManual = !string.IsNullOrWhiteSpace(_settings.ServerUrl) && !string.IsNullOrWhiteSpace(_settings.StreamKey);
        if ((requestTikTokRoom || !hasManual) && !string.IsNullOrWhiteSpace(_activeTikTokRtmpUrl))
        {
            usedTikTokRoom = true;
            return _activeTikTokRtmpUrl;
        }
        return null;
    }

    private static void FinishActiveTikTokRoomLocked()
    {
        var roomId = _activeTikTokRoomId;
        _activeTikTokRoomId = "";
        if (string.IsNullOrEmpty(roomId)) return;
        var sess = TikTokSessionData.FromJson(_settings.TikTokSessionJson);
        var signerKey = _settings.TikTokSignerKey;
        var signerUrl = _settings.TikTokSignerUrl;
        if (sess != null && sess.HasSessionCookie)
            _ = Task.Run(() => TikTokAuthService.FinishRoomAsync(sess, roomId, signerKey, signerUrl));
    }

    private static void SaveTikTokSession(TikTokSessionData session)
    {
        lock (Lock)
        {
            _settings.TikTokSessionJson = session.ToJson();
            if (!string.IsNullOrWhiteSpace(session.Username))
                _settings.TikTokUsername = session.Username;
            ScheduleSave();
        }
    }

    private static string? RunTask(Task<string?> t) => t.Wait(15000) ? t.Result : "Operation timed out";

    private static string Result(Task<string?> t)
    {
        var err = RunTask(t);
        return err == null ? Ok() : JsonSerializer.Serialize(new { error = err }, Json);
    }

    private static StreamEngine Engine() => _engine ?? throw new InvalidOperationException("Engine is not initialized.");

    private static void ScheduleSave()
    {
        _saveTimer ??= new Timer(_ => { lock (Lock) _settings.Save(); });
        _saveTimer.Change(1000, Timeout.Infinite);
    }
}
