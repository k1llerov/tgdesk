using System.Diagnostics;
using System.Security.Cryptography;
using System.Text.Json.Nodes;

namespace TgDesk;

/// <summary>
/// Updates from GitHub Releases of k1llerov/tgdesk.
///  • every 12 hours checks the latest release (if "update_check" is on);
///  • install: downloads TgDesk-Setup-x.y.z.exe into the data folder (updates\), checks its SHA-256 against
///    SHA256SUMS.txt of the same release (that file must be signed with the author's update key, see
///    <see cref="UpdateSignature"/>), then runs it silently: "/S /update". The installer stops TgDesk,
///    replaces the program files (settings and users stay) and starts TgDesk again;
///  • "auto_update" on: does it by itself, but never while someone is streaming the screen.
/// The portable build only reports new versions.
/// State: idle | downloading | installing | error.
/// </summary>
public static class Updater
{
    public static string? LatestVersion { get; private set; }
    public static string? LatestUrl { get; private set; }
    public static bool Available => LatestVersion != null && Newer(LatestVersion, Cfg.Version);
    public static string State { get; private set; } = "idle";
    public static int Progress { get; private set; }
    public static string? Error { get; private set; }
    /// <summary>Version this bot was updated from, if it was started right after an update.</summary>
    public static string? JustUpdatedFrom { get; private set; }

    static string? _setupUrl, _sumsUrl, _sigUrl, _setupName;
    static int _busy;
    static string UpdatesDir => Path.Combine(Cfg.Data, "updates");
    static string MarkerPath => Path.Combine(Cfg.Data, "update.json");
    static string ReleasePrefix => $"https://github.com/{Cfg.Repo}/releases/download/";

    public static bool Portable => File.Exists(Path.Combine(Cfg.Install, "portable.txt"));
    /// <summary>Only signed releases are installed by the program; anything else is "download it yourself".</summary>
    public static bool CanInstall => !Portable && UpdateSignature.Configured && _setupUrl != null && _sumsUrl != null && _sigUrl != null;

    public static JsonObject? ToJson() => !Available ? null : new JsonObject
    {
        ["version"] = LatestVersion, ["url"] = LatestUrl, ["can_install"] = CanInstall,
        ["state"] = State, ["progress"] = Progress, ["error"] = Error,
    };

    public static void Start(CancellationToken ct)
    {
        AfterUpdate();
        _ = Task.Run(async () =>
        {
            try { await Task.Delay(TimeSpan.FromSeconds(30), ct); } catch { return; }
            while (!ct.IsCancellationRequested)
            {
                if (Cfg.UpdateCheck)
                {
                    await CheckNow(ct);
                    if (Available && Cfg.AutoUpdate && CanInstall)
                    {
                        // never in the middle of a screen session: wait until nobody is watching
                        for (int i = 0; i < 720 && !Remote.Live.IsEmpty; i++)
                            try { await Task.Delay(TimeSpan.FromSeconds(60), ct); } catch { return; }
                        if (Remote.Live.IsEmpty) await Install("auto", ct);
                    }
                }
                try { await Task.Delay(TimeSpan.FromHours(12), ct); } catch { return; }
            }
        }, ct);
    }

    /// <summary>Runs once at start: cleans the downloaded installer and remembers "updated from X".</summary>
    static void AfterUpdate()
    {
        try
        {
            if (File.Exists(MarkerPath))
            {
                var j = JsonNode.Parse(File.ReadAllText(MarkerPath));
                var from = j?["from"]?.ToString();
                var to = j?["to"]?.ToString();
                File.Delete(MarkerPath);
                if (from != null && to != null && !Newer(to, Cfg.Version) && Newer(Cfg.Version, from))
                {
                    JustUpdatedFrom = from;
                    Log.Info($"Updated {from} -> {Cfg.Version}");
                    App.Alog.Add("system", L.Pc("TgDesk обновлён"), detail: $"{from} → {Cfg.Version}");
                }
                else if (to != null) Log.Warn($"Update to {to} did not happen (running {Cfg.Version})");
            }
            if (Directory.Exists(UpdatesDir)) Directory.Delete(UpdatesDir, true);
        }
        catch (Exception e) { Log.Warn("Update cleanup: " + e.Message); }
    }

    static HttpClient Http(TimeSpan timeout)
    {
        var http = new HttpClient { Timeout = timeout };
        http.DefaultRequestHeaders.UserAgent.ParseAdd($"{Cfg.AppName}/{Cfg.Version}");
        return http;
    }

    public static async Task CheckNow(CancellationToken ct = default)
    {
        try
        {
            using var http = Http(TimeSpan.FromSeconds(20));
            var j = JsonNode.Parse(await http.GetStringAsync($"https://api.github.com/repos/{Cfg.Repo}/releases/latest", ct));
            var tag = j?["tag_name"]?.ToString()?.TrimStart('v', 'V');
            var url = j?["html_url"]?.ToString();
            if (string.IsNullOrEmpty(tag) || url == null || !url.StartsWith($"https://github.com/{Cfg.Repo}/")) return;
            string? Asset(string name)
            {
                var a = (j?["assets"] as JsonArray)?.FirstOrDefault(x => x?["name"]?.ToString() == name);
                var u = a?["browser_download_url"]?.ToString();
                return u != null && u.StartsWith(ReleasePrefix) ? u : null;
            }
            _setupName = $"TgDesk-Setup-{tag}.exe";
            _setupUrl = Asset(_setupName);
            _sumsUrl = Asset("SHA256SUMS.txt");
            _sigUrl = Asset("SHA256SUMS.txt.sig");
            LatestVersion = tag; LatestUrl = url;
            if (Available) Log.Info($"Update available: {tag}" + (CanInstall ? "" : " (manual install only)"));
        }
        catch (Exception e) when (e is not OperationCanceledException) { Log.Warn("Update check failed: " + e.Message); }
    }

    /// <summary>Downloads, verifies and starts the installer. <paramref name="by"/> is who asked: "auto", "app" or a user name.</summary>
    public static async Task<(bool ok, string? error)> Install(string by, CancellationToken ct = default)
    {
        if (!Available) return (false, "no_update");
        if (!CanInstall) return (false, Portable ? "portable" : "no_installer");
        if (Interlocked.Exchange(ref _busy, 1) == 1) return (false, "busy");
        try
        {
            State = "downloading"; Progress = 0; Error = null;
            Log.Info($"Downloading update {LatestVersion} ({by})");
            Directory.CreateDirectory(UpdatesDir);
            var target = Path.Combine(UpdatesDir, _setupName!);
            var part = target + ".part";
            using var http = Http(TimeSpan.FromMinutes(20));

            var sums = await http.GetStringAsync(_sumsUrl, ct);
            // authenticity: SHA256SUMS.txt must be signed by the author's update key (built into this program)
            var sig = await http.GetStringAsync(_sigUrl, ct);
            if (!UpdateSignature.Verify(sums, sig))
                throw new InvalidOperationException("SHA256SUMS.txt signature is not valid — the update was rejected");
            var expected = sums.Split('\n').Select(l => l.Trim().Split(' ', 2, StringSplitOptions.RemoveEmptyEntries))
                .FirstOrDefault(p => p.Length == 2 && p[1].Trim().TrimStart('*') == _setupName)?[0];
            if (expected == null || expected.Length != 64) throw new InvalidOperationException("SHA256SUMS.txt has no checksum for " + _setupName);

            using (var resp = await http.GetAsync(_setupUrl, HttpCompletionOption.ResponseHeadersRead, ct))
            {
                resp.EnsureSuccessStatusCode();
                long total = resp.Content.Headers.ContentLength ?? 0, done = 0;
                await using var src = await resp.Content.ReadAsStreamAsync(ct);
                await using var dst = File.Create(part);
                var buf = new byte[1 << 16]; int n;
                while ((n = await src.ReadAsync(buf, ct)) > 0)
                {
                    await dst.WriteAsync(buf.AsMemory(0, n), ct);
                    done += n;
                    if (total > 0) Progress = (int)(done * 100 / total);
                }
            }
            string hash;
            await using (var f = File.OpenRead(part)) hash = Convert.ToHexString(await SHA256.HashDataAsync(f, ct)).ToLowerInvariant();
            if (!string.Equals(hash, expected, StringComparison.OrdinalIgnoreCase))
            {
                File.Delete(part);
                throw new InvalidOperationException("checksum mismatch — the download was rejected");
            }
            File.Move(part, target, true);
            Progress = 100;

            State = "installing";
            Cfg.AtomicWrite(MarkerPath, new JsonObject { ["from"] = Cfg.Version, ["to"] = LatestVersion, ["by"] = by }.ToJsonString());
            App.Alog.Add("system", L.Pc("Устанавливается обновление"), detail: $"{Cfg.Version} → {LatestVersion} ({by})");
            Log.Info($"Update {LatestVersion} verified (SHA-256), starting the installer");
            // the installer stops this process, replaces the files and starts TgDesk again
            Process.Start(new ProcessStartInfo(target, "/S /update") { UseShellExecute = true, WorkingDirectory = UpdatesDir });
            return (true, null);
        }
        catch (Exception e) when (e is not OperationCanceledException)
        {
            State = "error"; Error = e.Message;
            Log.Error("Update failed", e);
            return (false, e.Message);
        }
        finally { Interlocked.Exchange(ref _busy, 0); }
    }

    public static bool Newer(string a, string b)
    {
        static int[] P(string s) => s.Split('-')[0].Split('.').Select(x => int.TryParse(x, out var n) ? n : 0).Concat(new[] { 0, 0, 0 }).Take(3).ToArray();
        var x = P(a); var y = P(b);
        for (int i = 0; i < 3; i++) if (x[i] != y[i]) return x[i] > y[i];
        return false;
    }
}
