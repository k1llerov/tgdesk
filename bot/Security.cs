using System.Collections.Concurrent;
using System.Diagnostics;
using System.Security.Cryptography;
using System.Text;

namespace TgDesk;

/// <summary>Basic protection: IP blocking after failed logins, Host check, private file permissions.</summary>
public static class Security
{
    const int MaxFails = 8;                                        // failed attempts…
    static readonly TimeSpan Window = TimeSpan.FromMinutes(10);    // …within this time
    static readonly TimeSpan BlockFor = TimeSpan.FromMinutes(15);  // → block for

    sealed class Rec { public readonly Queue<DateTime> Fails = new(); public DateTime BlockedUntil; }
    static readonly ConcurrentDictionary<string, Rec> Recs = new();

    public static int BlockedCount => Recs.Values.Count(r => r.BlockedUntil > DateTime.UtcNow);

    public static bool IsBlocked(string? ip)
    {
        if (string.IsNullOrEmpty(ip) || !Recs.TryGetValue(ip, out var r)) return false;
        lock (r) return r.BlockedUntil > DateTime.UtcNow;
    }

    public static int RetryAfterSeconds(string ip) =>
        Recs.TryGetValue(ip, out var r) ? Math.Max(1, (int)(r.BlockedUntil - DateTime.UtcNow).TotalSeconds) : 1;

    /// <summary>Records a failed attempt; true — the IP has just been blocked.</summary>
    public static bool Fail(string? ip)
    {
        if (string.IsNullOrEmpty(ip)) return false;
        var now = DateTime.UtcNow;
        if (Recs.Count > 5000) foreach (var k in Recs.Where(p => p.Value.BlockedUntil < now && p.Value.Fails.Count == 0).Select(p => p.Key).ToList()) Recs.TryRemove(k, out _);
        var r = Recs.GetOrAdd(ip, _ => new Rec());
        lock (r)
        {
            while (r.Fails.Count > 0 && now - r.Fails.Peek() > Window) r.Fails.Dequeue();
            r.Fails.Enqueue(now);
            if (r.Fails.Count >= MaxFails && r.BlockedUntil <= now)
            {
                r.BlockedUntil = now + BlockFor;
                r.Fails.Clear();
                App.Alog?.Add("deny", L.Pc("IP заблокирован на 15 минут"), null, new ReqMeta(ip, "", "", null),
                    L.PcF("{0} неудачных попыток входа за 10 минут", MaxFails), "warn");
                return true;
            }
        }
        return false;
    }

    /// <summary>The Mini App opens only through the configured public address or from this PC (protection from Host spoofing).</summary>
    public static bool HostAllowed(string host)
    {
        host = host.ToLowerInvariant();
        var colon = host.LastIndexOf(':');
        var name = colon > 0 && !host.EndsWith(']') ? host[..colon] : host;
        if (name is "127.0.0.1" or "localhost") return true;
        if (Cfg.TunnelMode == "quick") return name.EndsWith(".trycloudflare.com");
        return Uri.TryCreate(Cfg.PublicUrl, UriKind.Absolute, out var u) && string.Equals(u.Host, name, StringComparison.OrdinalIgnoreCase);
    }

    public static bool TokenEquals(string? a, string b)
    {
        var x = Encoding.UTF8.GetBytes(a ?? "");
        var y = Encoding.UTF8.GetBytes(b);
        return x.Length == y.Length && CryptographicOperations.FixedTimeEquals(x, y);
    }

    /// <summary>Leaves access to a file only to the current user and SYSTEM (icacls).</summary>
    public static void LockDown(string path)
    {
        try
        {
            if (!File.Exists(path)) return;
            var user = Environment.UserDomainName + "\\" + Environment.UserName;
            var psi = new ProcessStartInfo("icacls.exe", $"\"{path}\" /inheritance:r /grant:r \"{user}:(F)\" \"*S-1-5-18:(F)\"")
            {
                CreateNoWindow = true, UseShellExecute = false, RedirectStandardOutput = true, RedirectStandardError = true,
            };
            using var p = Process.Start(psi)!;
            p.WaitForExit(5000);
            if (p.ExitCode != 0) Log.Warn($"Could not restrict access to {Path.GetFileName(path)} (icacls exit {p.ExitCode})");
        }
        catch (Exception e) { Log.Warn("File permissions: " + e.Message); }
    }
}
