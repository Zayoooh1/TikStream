using System.Collections.Concurrent;
using System.Text;

namespace TikStream.Core;

/// <summary>Simple thread-safe file logger + UI event. Writes on a dedicated thread so it never blocks rendering.</summary>
public static class Log
{
    private static readonly BlockingCollection<string> Queue = new(new ConcurrentQueue<string>(), 10_000);
    private static readonly Thread Writer;
    public static string FilePath { get; }
    public static event Action<string>? Line;

    static Log()
    {
        var dir = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.ApplicationData), "TikStream");
        Directory.CreateDirectory(dir);
        FilePath = Path.Combine(dir, "log.txt");
        try { if (File.Exists(FilePath) && new FileInfo(FilePath).Length > 5_000_000) File.Delete(FilePath); } catch { }
        Writer = new Thread(WriteLoop) { IsBackground = true, Name = "Log", Priority = ThreadPriority.BelowNormal };
        Writer.Start();
    }

    public static void Info(string msg) => Write("INFO", msg);
    public static void Warn(string msg) => Write("WARN", msg);
    public static void Error(string msg) => Write("ERR ", msg);
    public static void Error(string msg, Exception ex) => Write("ERR ", msg + ": " + ex);

    private static void Write(string level, string msg)
    {
        var line = $"{DateTime.Now:HH:mm:ss.fff} {level} {msg}";
        Queue.TryAdd(line);
        try { Line?.Invoke(line); } catch { }
    }

    private static void WriteLoop()
    {
        using var fs = new FileStream(FilePath, FileMode.Append, FileAccess.Write, FileShare.ReadWrite);
        using var sw = new StreamWriter(fs, new UTF8Encoding(false)) { AutoFlush = false };
        foreach (var line in Queue.GetConsumingEnumerable())
        {
            sw.WriteLine(line);
            if (Queue.Count == 0) sw.Flush();
        }
    }

    public static void Flush()
    {
        for (int i = 0; i < 50 && Queue.Count > 0; i++) Thread.Sleep(10);
    }
}
