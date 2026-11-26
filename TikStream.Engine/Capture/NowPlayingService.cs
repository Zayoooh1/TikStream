using System.Runtime.InteropServices.WindowsRuntime;
using TikStream.Core;
using Windows.Media.Control;

namespace TikStream.Capture;

public sealed record NowPlayingSnapshot(
    ulong Revision,
    string Title,
    string Artist,
    string Album,
    string AppId,
    bool IsPlaying,
    string ThumbnailBase64);

/// <summary>
/// Subscribes to Windows Global System Media Transport Controls (SMTC) in the background
/// and exposes the latest playing track + album art with a monotonic revision counter.
/// Never touches the render or audio thread.
/// </summary>
public sealed class NowPlayingService : IDisposable
{
    private readonly object _lock = new();
    private GlobalSystemMediaTransportControlsSessionManager? _manager;
    private GlobalSystemMediaTransportControlsSession? _currentSession;
    private Timer? _fallbackTimer;
    private volatile bool _active;
    private volatile bool _disposed;
    private bool _initStarted;
    private int _refreshing;

    private ulong _revision = 1;
    private string _title = "";
    private string _artist = "";
    private string _album = "";
    private string _appId = "";
    private bool _isPlaying;
    private string _thumbnailBase64 = "";
    private string _lastArtKey = "";

    public void SetActive(bool active)
    {
        _active = active;
        if (active)
        {
            EnsureInitialized();
            lock (_lock)
            {
                _fallbackTimer ??= new Timer(_ =>
                {
                    if (_active && !_disposed) _ = RefreshAsync();
                }, null, 3500, 4000);
            }
        }
        else
        {
            lock (_lock)
            {
                _fallbackTimer?.Dispose();
                _fallbackTimer = null;
            }
        }
    }

    public object GetState(ulong sinceRevision = 0)
    {
        if (!_initStarted && !_disposed) EnsureInitialized();
        lock (_lock)
        {
            if (sinceRevision > 0 && sinceRevision == _revision)
                return new { unchanged = true, revision = _revision };
            return new
            {
                unchanged = false,
                revision = _revision,
                title = _title,
                artist = _artist,
                album = _album,
                appId = _appId,
                isPlaying = _isPlaying,
                thumbnailBase64 = _thumbnailBase64,
            };
        }
    }

    private void EnsureInitialized()
    {
        lock (_lock)
        {
            if (_initStarted || _disposed) return;
            _initStarted = true;
        }
        _ = Task.Run(InitAsync);
    }

    private async Task InitAsync()
    {
        try
        {
            var mgr = await GlobalSystemMediaTransportControlsSessionManager.RequestAsync().AsTask().ConfigureAwait(false);
            if (_disposed) return;
            _manager = mgr;
            mgr.CurrentSessionChanged += OnCurrentSessionChanged;
            AttachSession(mgr.GetCurrentSession());
            await RefreshAsync().ConfigureAwait(false);
        }
        catch (Exception ex)
        {
            Log.Warn("NowPlaying SMTC init: " + ex.Message);
        }
    }

    private void OnCurrentSessionChanged(GlobalSystemMediaTransportControlsSessionManager sender, CurrentSessionChangedEventArgs args)
    {
        if (_disposed) return;
        try
        {
            AttachSession(sender.GetCurrentSession());
            _ = RefreshAsync();
        }
        catch { }
    }

    private void AttachSession(GlobalSystemMediaTransportControlsSession? session)
    {
        lock (_lock)
        {
            if (_currentSession != null)
            {
                try { _currentSession.MediaPropertiesChanged -= OnSessionChanged; } catch { }
                try { _currentSession.PlaybackInfoChanged -= OnPlaybackChanged; } catch { }
            }
            _currentSession = session;
            if (_currentSession != null)
            {
                try { _currentSession.MediaPropertiesChanged += OnSessionChanged; } catch { }
                try { _currentSession.PlaybackInfoChanged += OnPlaybackChanged; } catch { }
            }
        }
    }

    private void OnSessionChanged(GlobalSystemMediaTransportControlsSession sender, MediaPropertiesChangedEventArgs args)
    {
        if (!_disposed) _ = RefreshAsync();
    }

    private void OnPlaybackChanged(GlobalSystemMediaTransportControlsSession sender, PlaybackInfoChangedEventArgs args)
    {
        if (!_disposed) _ = RefreshAsync();
    }

    private async Task RefreshAsync()
    {
        if (_disposed || Interlocked.Exchange(ref _refreshing, 1) != 0) return;
        try
        {
            var mgr = _manager;
            if (mgr == null) return;
            var session = mgr.GetCurrentSession();
            if (!ReferenceEquals(session, _currentSession))
                AttachSession(session);

            if (session == null)
            {
                UpdateSnapshot("", "", "", "", false, null, "");
                return;
            }

            string appId = session.SourceAppUserModelId ?? "";
            bool isPlaying = false;
            try
            {
                var pb = session.GetPlaybackInfo();
                isPlaying = pb?.PlaybackStatus == GlobalSystemMediaTransportControlsSessionPlaybackStatus.Playing;
            }
            catch { }

            string title = "", artist = "", album = "";
            string? thumbB64 = null;
            string artKey = "";
            try
            {
                var props = await session.TryGetMediaPropertiesAsync().AsTask().ConfigureAwait(false);
                if (props != null)
                {
                    title = (props.Title ?? "").Trim();
                    artist = (props.Artist ?? "").Trim();
                    album = (props.AlbumTitle ?? "").Trim();
                    artKey = $"{appId}|{title}|{artist}|{album}";

                    bool needArt;
                    lock (_lock) needArt = !string.Equals(_lastArtKey, artKey, StringComparison.Ordinal);
                    if (needArt)
                    {
                        thumbB64 = await ReadThumbnailBase64Async(props.Thumbnail).ConfigureAwait(false);
                    }
                }
            }
            catch { }

            UpdateSnapshot(title, artist, album, appId, isPlaying, thumbB64, artKey);
        }
        catch { }
        finally
        {
            Interlocked.Exchange(ref _refreshing, 0);
        }
    }

    private static async Task<string> ReadThumbnailBase64Async(Windows.Storage.Streams.IRandomAccessStreamReference? thumbRef)
    {
        if (thumbRef == null) return "";
        try
        {
            using var stream = await thumbRef.OpenReadAsync().AsTask().ConfigureAwait(false);
            if (stream == null || stream.Size == 0 || stream.Size > 2 * 1024 * 1024) return "";
            using var netStream = stream.AsStreamForRead();
            using var mem = new MemoryStream((int)Math.Min(stream.Size, 512 * 1024));
            await netStream.CopyToAsync(mem).ConfigureAwait(false);
            if (mem.Length == 0 || mem.Length > 2 * 1024 * 1024) return "";
            return Convert.ToBase64String(mem.ToArray());
        }
        catch
        {
            return "";
        }
    }

    private void UpdateSnapshot(string title, string artist, string album, string appId, bool isPlaying, string? newThumbB64, string artKey)
    {
        lock (_lock)
        {
            string thumb = newThumbB64 ?? _thumbnailBase64;
            if (string.IsNullOrEmpty(title) && string.IsNullOrEmpty(artist)) thumb = "";
            bool changed = title != _title || artist != _artist || album != _album ||
                           appId != _appId || isPlaying != _isPlaying || thumb != _thumbnailBase64;
            _lastArtKey = artKey;
            if (!changed) return;
            _title = title;
            _artist = artist;
            _album = album;
            _appId = appId;
            _isPlaying = isPlaying;
            _thumbnailBase64 = thumb;
            _revision++;
        }
    }

    public void Dispose()
    {
        _disposed = true;
        _active = false;
        lock (_lock)
        {
            _fallbackTimer?.Dispose();
            _fallbackTimer = null;
            if (_currentSession != null)
            {
                try { _currentSession.MediaPropertiesChanged -= OnSessionChanged; } catch { }
                try { _currentSession.PlaybackInfoChanged -= OnPlaybackChanged; } catch { }
                _currentSession = null;
            }
            if (_manager != null)
            {
                try { _manager.CurrentSessionChanged -= OnCurrentSessionChanged; } catch { }
                _manager = null;
            }
        }
    }
}
