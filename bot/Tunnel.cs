using System.Diagnostics;
using System.Security.Cryptography;
using System.Text.Json.Nodes;
using System.Text.RegularExpressions;

namespace TgDesk;

/// <summary>
/// Public HTTPS address of the Mini App.
///  quick      — Cloudflare Quick Tunnel (no account; the address changes on every start, the bot updates its menu button);
///  cloudflare — your named Cloudflare Tunnel (token from the Zero Trust dashboard + your domain);
///  external   — any HTTPS address you already route to http://127.0.0.1:WEB_PORT yourself.
/// State is a code: starting | downloading | connecting | online | offline | error | no_url.
/// </summary>
public sealed class Tunnel
{
    static readonly Regex QuickUrlRe = new(@"https://[a-z0-9-]+\.trycloudflare\.com", RegexOptions.Compiled);
    static readonly Regex HashRe = new(@"cloudflared-windows-amd64\.exe:\s*([0-9a-fA-F]{64})", RegexOptions.Compiled);
    const string ReleaseApi = "https://api.github.com/repos/cloudflare/cloudflared/releases/latest";

    public string? Url { get; private set; }
    public string State { get; private set; } = "starting";
    public string? Error { get; private set; }
    public event Action<string>? UrlChanged;
    Process? _proc;
    CancellationTokenSource? _cts;
    bool _restartRequested;

    readonly object _l = new();

    /// <summary>Starts the tunnel loop (does nothing if it is already running).</summary>
    public void Start()
    {
        lock (_l)
        {
            if (_cts is { IsCancellationRequested: false }) return;
            _cts = new CancellationTokenSource();
            var ct = _cts.Token;
            _ = Task.Run(() => Loop(ct));
        }
    }

    /// <summary>Restarts with the current settings (mode / address may have changed).</summary>
    public void Restart()
    {
        _restartRequested = true;
        Stop();
        Url = null; State = "starting"; Error = null;
        if (Cfg.Configured) Start(); // without a bot token there is nobody to serve yet
    }

    public void Stop()
    {
        _cts?.Cancel();
        try { if (_proc is { HasExited: false }) _proc.Kill(true); } catch { }
    }

    public static void KillStale()
    {
        var mine = Path.GetFullPath(Cfg.Cloudflared).ToLowerInvariant();
        foreach (var p in Process.GetProcessesByName("cloudflared"))
        {
            try
            {
                if (p.MainModule?.FileName is string f && Path.GetFullPath(f).ToLowerInvariant() == mine) { p.Kill(true); Log.Info($"Stopped a stale cloudflared pid={p.Id}"); }
            }
            catch { }
        }
    }

    public static bool ValidPublicUrl(string url) =>
        Uri.TryCreate(url, UriKind.Absolute, out var u) && u.Scheme == "https" && u.Host.Contains('.') && string.IsNullOrEmpty(u.Query);

    void SetUrl(string url)
    {
        Url = url.TrimEnd('/');
        State = "online"; Error = null;
        Log.Info("Mini App address: " + Url);
        UrlChanged?.Invoke(Url);
    }

    async Task Loop(CancellationToken ct)
    {
        if (Cfg.TunnelMode == "external")
        {
            if (ValidPublicUrl(Cfg.PublicUrl)) SetUrl(Cfg.PublicUrl);
            else { State = "no_url"; Log.Warn("External mode, but the public address is empty or not https"); }
            return;
        }
        if (Cfg.TunnelMode == "cloudflare" && (Cfg.TunnelToken == "" || !ValidPublicUrl(Cfg.PublicUrl)))
        {
            State = "no_url"; Log.Warn("Cloudflare mode needs a tunnel token and an https address");
            return;
        }

        int backoff = 5;
        while (!ct.IsCancellationRequested)
        {
            try
            {
                if (!File.Exists(Cfg.Cloudflared)) { State = "downloading"; await EnsureCloudflared(ct); }
                State = "connecting";
                var psi = new ProcessStartInfo(Cfg.Cloudflared)
                {
                    RedirectStandardError = true, RedirectStandardOutput = true, UseShellExecute = false, CreateNoWindow = true,
                };
                if (Cfg.TunnelMode == "cloudflare")
                {
                    psi.Arguments = "tunnel --no-autoupdate run";
                    psi.Environment["TUNNEL_TOKEN"] = Cfg.TunnelToken; // not on the command line: other programs can read command lines
                }
                else psi.Arguments = $"tunnel --no-autoupdate --url http://127.0.0.1:{Cfg.Port}";
                _proc = Process.Start(psi)!;
                _ = _proc.StandardOutput.ReadToEndAsync();
                bool got = false;
                string? line;
                while ((line = await _proc.StandardError.ReadLineAsync(ct)) != null)
                {
                    if (got) continue;
                    if (Cfg.TunnelMode == "cloudflare")
                    {
                        if (!line.Contains("Registered tunnel connection", StringComparison.OrdinalIgnoreCase)) continue;
                        got = true; backoff = 5;
                        SetUrl(Cfg.PublicUrl);
                    }
                    else
                    {
                        var m = QuickUrlRe.Match(line);
                        if (!m.Success) continue;
                        got = true; backoff = 5;
                        SetUrl(m.Value);
                    }
                }
                await _proc.WaitForExitAsync(ct);
                Log.Warn($"cloudflared exited with code {_proc.ExitCode}");
            }
            catch (OperationCanceledException) { break; }
            catch (Exception ex) { Log.Error("Tunnel error", ex); Error = ex.Message; }
            if (ct.IsCancellationRequested) break;
            Url = null; State = Error != null ? "error" : "offline";
            if (_restartRequested) { _restartRequested = false; backoff = 1; }
            try { await Task.Delay(TimeSpan.FromSeconds(backoff), ct); } catch { break; }
            backoff = Math.Min(backoff * 2, 120);
        }
    }

    static readonly SemaphoreSlim DownloadLock = new(1, 1);

    /// <summary>
    /// Makes sure cloudflared is in the TgDesk bin folder: downloads it once (also ahead of time, right after
    /// the first start, so the Mini App comes up without waiting). Safe to call from several places at once.
    /// </summary>
    public static async Task EnsureCloudflared(CancellationToken ct)
    {
        await DownloadLock.WaitAsync(ct);
        try { if (!File.Exists(Cfg.Cloudflared)) await Download(ct); }
        finally { DownloadLock.Release(); }
    }

    /// <summary>
    /// Downloads cloudflared from the official GitHub release of Cloudflare and checks it
    /// against the SHA-256 published in the release notes. A file that does not match is deleted.
    /// </summary>
    static async Task Download(CancellationToken ct)
    {
        Log.Info("Downloading cloudflared from github.com/cloudflare/cloudflared…");
        using var http = new HttpClient { Timeout = TimeSpan.FromMinutes(5) };
        http.DefaultRequestHeaders.UserAgent.ParseAdd($"{Cfg.AppName}/{Cfg.Version}");
        var rel = JsonNode.Parse(await http.GetStringAsync(ReleaseApi, ct))!;
        var tag = rel["tag_name"]?.ToString();
        var m = HashRe.Match(rel["body"]?.ToString() ?? "");
        if (!m.Success) throw new InvalidOperationException("cloudflared release has no published SHA-256 for the Windows build");
        var asset = (rel["assets"] as JsonArray)?.FirstOrDefault(a => a?["name"]?.ToString() == "cloudflared-windows-amd64.exe");
        var url = asset?["browser_download_url"]?.ToString();
        if (url == null || !url.StartsWith("https://github.com/cloudflare/cloudflared/releases/download/"))
            throw new InvalidOperationException("cloudflared Windows build not found in the release");

        var part = Cfg.Cloudflared + ".part";
        await using (var src = await http.GetStreamAsync(url, ct))
        await using (var dst = File.Create(part))
            await src.CopyToAsync(dst, ct);
        string hash;
        await using (var f = File.OpenRead(part))
            hash = Convert.ToHexString(await SHA256.HashDataAsync(f, ct)).ToLowerInvariant();
        if (!string.Equals(hash, m.Groups[1].Value, StringComparison.OrdinalIgnoreCase))
        {
            File.Delete(part);
            throw new InvalidOperationException("cloudflared checksum mismatch — the download was rejected");
        }
        File.Move(part, Cfg.Cloudflared, true);
        Log.Info($"cloudflared {tag} downloaded, SHA-256 verified");
    }
}
