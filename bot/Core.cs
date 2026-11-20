using System.Globalization;
using System.Runtime.InteropServices;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json;
using System.Text.Json.Nodes;
using System.Text.RegularExpressions;

namespace TgDesk;

/// <summary>
/// Paths and settings. Program files live next to the executables (read-only),
/// user data lives in %APPDATA%\TgDesk (or TGDESK_HOME, or .\data in portable mode).
/// config.json is written only by the bot; the desktop app changes it through the local control API.
/// </summary>
public static class Cfg
{
    public const string AppName = "TgDesk";
    public const string Version = "1.0.0";
    public const string Repo = "k1llerov/tgdesk";

    public static string Install = "";   // folder with webapp\ (program files)
    public static string Data = "";      // user data folder

    public static string WebappDir => Path.Combine(Install, "webapp");
    public static string BinDir => Path.Combine(Data, "bin");
    public static string Cloudflared => Path.Combine(BinDir, "cloudflared.exe");
    public static string ConfigPath => Path.Combine(Data, "config.json");
    public static string UsersPath => Path.Combine(Data, "users.json");
    public static string LogPath => Path.Combine(Data, "tgdesk.log");
    public static string AccessLogPath => Path.Combine(Data, "access_log.jsonl");
    public static string PidPath => Path.Combine(Data, "tgdesk.pid");
    public static string ControlTokenPath => Path.Combine(Data, "control.token");
    public static string AvatarPath => Path.Combine(Data, "avatar.jpg");
    public static string ControlToken = "";

    // ---- settings (config.json)
    public static string Token = "";
    public static int Port = 8765, LogPort = 8766, ControlPort = 8767;
    public static string Lang = "auto";            // auto | ru | en — language of the PC side (logs, overlay)
    public static string Quality = "auto";         // eco | norm | hd | auto
    public static bool Autostart = true;
    public static bool UpdateCheck = true;
    public static bool AutoUpdate = true;          // install signed updates by itself (only when nobody streams)
    public static bool GeoIp = true;              // look up city/ISP of remote IPs via ipwho.is for the access log
    public static string TunnelMode = "quick";     // quick | cloudflare | external
    public static string TunnelToken = "";         // Cloudflare named tunnel token (mode = cloudflare)
    public static string PublicUrl = "";           // https address of the Mini App (modes cloudflare / external)
    public static string DownloadDir = "", PhotoDir = "", MusicDir = "", VideoDir = "";
    public static string PicturesRoot = "", MusicRoot = "", VideosRoot = "";
    public static readonly Dictionary<string, bool> Features = Perm.All.ToDictionary(p => p, _ => true);

    static readonly byte[] Entropy = Encoding.UTF8.GetBytes("TgDesk.v1.secret");
    static readonly object L = new();

    public static bool Configured => Token != "";

    public static void Init()
    {
        Install = FindInstall();
        Data = FindData();
        Directory.CreateDirectory(Data);
        Directory.CreateDirectory(BinDir);
        PicturesRoot = Environment.GetFolderPath(Environment.SpecialFolder.MyPictures);
        MusicRoot = Environment.GetFolderPath(Environment.SpecialFolder.MyMusic);
        VideosRoot = Environment.GetFolderPath(Environment.SpecialFolder.MyVideos);
        Load();
        try
        {
            ControlToken = Convert.ToHexString(RandomNumberGenerator.GetBytes(24)).ToLowerInvariant();
            File.WriteAllText(ControlTokenPath, ControlToken);
        }
        catch { }
    }

    static string FindInstall()
    {
        var dir = new DirectoryInfo(AppContext.BaseDirectory);
        while (dir != null)
        {
            if (File.Exists(Path.Combine(dir.FullName, "webapp", "index.html"))) return dir.FullName;
            dir = dir.Parent;
        }
        return AppContext.BaseDirectory;
    }

    /// <summary>TGDESK_HOME → portable (portable.txt next to TgDesk.exe) → %APPDATA%\TgDesk. Same rules as the desktop app.</summary>
    static string FindData()
    {
        var env = Environment.GetEnvironmentVariable("TGDESK_HOME");
        if (!string.IsNullOrWhiteSpace(env)) return Path.GetFullPath(env);
        if (File.Exists(Path.Combine(Install, "portable.txt"))) return Path.Combine(Install, "data");
        return Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.ApplicationData), AppName);
    }

    public static void Load()
    {
        lock (L)
        {
            JsonObject j;
            try { j = File.Exists(ConfigPath) ? JsonNode.Parse(File.ReadAllText(ConfigPath, Encoding.UTF8)) as JsonObject ?? new() : new(); }
            catch (Exception e) { Log.Error("config.json is damaged, using defaults", e); j = new(); }

            Token = Unprotect(S(j, "token_enc"));
            Lang = S(j, "lang") is "ru" or "en" ? S(j, "lang") : "auto";
            Quality = S(j, "quality") is "eco" or "norm" or "hd" or "auto" ? S(j, "quality") : "auto";
            Autostart = j["autostart"]?.GetValue<bool>() ?? Sys.AutostartGet(); // first run: keep what the installer chose
            UpdateCheck = j["update_check"]?.GetValue<bool>() ?? true;
            AutoUpdate = j["auto_update"]?.GetValue<bool>() ?? true;
            GeoIp = j["geoip"]?.GetValue<bool>() ?? true;
            var t = j["tunnel"] as JsonObject ?? new JsonObject();
            TunnelMode = S(t, "mode") is "cloudflare" or "external" ? S(t, "mode") : "quick";
            TunnelToken = Unprotect(S(t, "token_enc"));
            PublicUrl = S(t, "public_url");
            var p = j["ports"] as JsonObject ?? new JsonObject();
            Port = Int(p["web"], 8765); LogPort = Int(p["log"], 8766); ControlPort = Int(p["control"], 8767);
            var f = j["features"] as JsonObject ?? new JsonObject();
            foreach (var k in Perm.All) Features[k] = f[k]?.GetValue<bool>() ?? true;
            var d = j["folders"] as JsonObject ?? new JsonObject();
            var home = Environment.GetFolderPath(Environment.SpecialFolder.UserProfile);
            DownloadDir = Or(S(d, "downloads"), Path.Combine(home, "Downloads", AppName));
            PhotoDir = Or(S(d, "photos"), Path.Combine(PicturesRoot, AppName));
            MusicDir = Or(S(d, "music"), Path.Combine(MusicRoot, AppName));
            VideoDir = Or(S(d, "videos"), Path.Combine(VideosRoot, AppName));
        }
    }

    public static void Save()
    {
        lock (L)
        {
            JsonObject old;
            try { old = File.Exists(ConfigPath) ? JsonNode.Parse(File.ReadAllText(ConfigPath, Encoding.UTF8)) as JsonObject ?? new() : new(); }
            catch { old = new(); }
            var j = new JsonObject
            {
                ["version"] = 1,
                ["token_enc"] = Protect(Token),
                ["lang"] = Lang,
                ["quality"] = Quality,
                ["autostart"] = Autostart,
                ["update_check"] = UpdateCheck,
                ["auto_update"] = AutoUpdate,
                ["geoip"] = GeoIp,
                ["tunnel"] = new JsonObject { ["mode"] = TunnelMode, ["token_enc"] = Protect(TunnelToken), ["public_url"] = PublicUrl },
                ["ports"] = new JsonObject { ["web"] = Port, ["log"] = LogPort, ["control"] = ControlPort },
                ["features"] = new JsonObject(Features.Select(kv => KeyValuePair.Create(kv.Key, (JsonNode?)kv.Value))),
                ["folders"] = old["folders"]?.DeepClone() ?? new JsonObject(),
            };
            AtomicWrite(ConfigPath, j.ToJsonString(Json.Pretty));
            Security.LockDown(ConfigPath);
        }
    }

    public static void AtomicWrite(string path, string text)
    {
        var tmp = path + ".tmp";
        File.WriteAllText(tmp, text, new UTF8Encoding(false));
        File.Move(tmp, path, true);
    }

    static string S(JsonObject o, string k) => o[k] is JsonValue v && v.TryGetValue<string>(out var s) ? s : "";
    static string Or(string a, string b) => string.IsNullOrWhiteSpace(a) ? b : a;
    static int Int(JsonNode? n, int d) => n is JsonValue v && v.TryGetValue<int>(out var x) && x > 0 && x < 65536 ? x : d;

    /// <summary>Secrets are encrypted with Windows DPAPI: only this Windows user on this PC can decrypt them.</summary>
    public static string Protect(string plain)
    {
        if (string.IsNullOrEmpty(plain)) return "";
        var enc = ProtectedData.Protect(Encoding.UTF8.GetBytes(plain), Entropy, DataProtectionScope.CurrentUser);
        return Convert.ToBase64String(enc);
    }

    public static string Unprotect(string b64)
    {
        if (string.IsNullOrEmpty(b64)) return "";
        try { return Encoding.UTF8.GetString(ProtectedData.Unprotect(Convert.FromBase64String(b64), Entropy, DataProtectionScope.CurrentUser)); }
        catch (Exception e) { Log.Warn("Cannot decrypt a stored secret (copied from another PC or user?): " + e.Message); return ""; }
    }

    [DllImport("kernel32.dll")] static extern ushort GetUserDefaultUILanguage();

    /// <summary>Effective PC language: setting or Windows UI language.</summary>
    public static string PcLang => Lang != "auto" ? Lang : SystemLang();

    public static string SystemLang()
    {
        try
        {
            int primary = GetUserDefaultUILanguage() & 0x3FF;
            return primary is 0x19 or 0x22 or 0x23 ? "ru" : "en"; // Russian, Ukrainian, Belarusian → ru
        }
        catch { return "en"; }
    }

    /// <summary>Language for a Telegram user by their client language code.</summary>
    public static string TgLang(string? code)
    {
        code = (code ?? "").ToLowerInvariant();
        return code.StartsWith("ru") || code.StartsWith("uk") || code.StartsWith("be") || code.StartsWith("kk") ? "ru" : "en";
    }
}

/// <summary>Technical log tgdesk.log (English, rotated at 1 MB).</summary>
public static class Log
{
    static readonly object L = new();

    static void Write(string level, string name, string msg)
    {
        lock (L)
        {
            try
            {
                var p = Cfg.LogPath;
                if (string.IsNullOrEmpty(Cfg.Data)) return;
                if (File.Exists(p) && new FileInfo(p).Length > 1_000_000) File.Move(p, p + ".1", true);
                File.AppendAllText(p, $"{DateTime.Now:yyyy-MM-dd HH:mm:ss,fff} {level} {name}: {Redact(msg)}\r\n", Encoding.UTF8);
            }
            catch { }
        }
    }

    // never write a bot token to the log, even inside exception messages (Bot API URLs contain it)
    static readonly Regex TokenRe = new(@"\d{6,12}:[A-Za-z0-9_-]{30,}", RegexOptions.Compiled);
    public static string Redact(string s) => TokenRe.Replace(s, "<token>");

    public static void Info(string msg, string name = "tgdesk") => Write("INFO", name, msg);
    public static void Warn(string msg, string name = "tgdesk") => Write("WARNING", name, msg);
    public static void Error(string msg, Exception? ex = null, string name = "tgdesk") => Write("ERROR", name, ex == null ? msg : $"{msg}: {ex}");
}

public static class Util
{
    public static string HumanBytes(double n)
    {
        string[] u = { "B", "KB", "MB", "GB", "TB" };
        int i = 0;
        while (n >= 1024 && i < u.Length - 1) { n /= 1024; i++; }
        return n.ToString("0.0", CultureInfo.InvariantCulture) + " " + u[i];
    }

    static readonly Regex Bad = new("[<>:\"/\\\\|?*\\x00-\\x1f]", RegexOptions.Compiled);
    static readonly HashSet<string> Reserved = new(new[] { "CON", "PRN", "AUX", "NUL" }
        .Concat(Enumerable.Range(1, 9).Select(i => "COM" + i)).Concat(Enumerable.Range(1, 9).Select(i => "LPT" + i)));

    public static string Sanitize(string? name, string def = "file")
    {
        name = Bad.Replace(Path.GetFileName((name ?? "").Replace('/', '\\')), "_").Trim(' ', '.');
        if (name.Length == 0) name = def;
        if (name.Length > 150)
        {
            var e = Path.GetExtension(name);
            name = Path.GetFileNameWithoutExtension(name)[..(150 - e.Length)] + e;
        }
        if (Reserved.Contains(name.Split('.')[0].ToUpperInvariant())) name = "_" + name;
        return name;
    }

    public static string UniquePath(string folder, string name)
    {
        name = Sanitize(name);
        var root = Path.GetFileNameWithoutExtension(name);
        var ext = Path.GetExtension(name);
        var path = Path.Combine(folder, name);
        for (int i = 1; File.Exists(path) || Directory.Exists(path); i++) path = Path.Combine(folder, $"{root} ({i}){ext}");
        return path;
    }

    public static string Ext(string name) => Path.GetExtension(name).TrimStart('.').ToLowerInvariant();

    static readonly HashSet<string> Img = new() { "jpg", "jpeg", "png", "gif", "webp", "bmp" };
    static readonly HashSet<string> Aud = new() { "mp3", "m4a", "aac", "ogg", "oga", "opus", "wav", "flac" };
    static readonly HashSet<string> Vid = new() { "mp4", "m4v", "webm", "mov", "mkv", "avi", "wmv" };

    public static string Kind(string name)
    {
        var e = Ext(name);
        return Img.Contains(e) ? "image" : Aud.Contains(e) ? "audio" : Vid.Contains(e) ? "video" : "other";
    }

    static readonly Dictionary<string, string> Mimes = new()
    {
        ["jpg"] = "image/jpeg", ["jpeg"] = "image/jpeg", ["png"] = "image/png", ["gif"] = "image/gif", ["webp"] = "image/webp", ["bmp"] = "image/bmp",
        ["mp3"] = "audio/mpeg", ["m4a"] = "audio/mp4", ["aac"] = "audio/aac", ["ogg"] = "audio/ogg", ["oga"] = "audio/ogg", ["opus"] = "audio/ogg",
        ["wav"] = "audio/wav", ["flac"] = "audio/flac",
        ["mp4"] = "video/mp4", ["m4v"] = "video/mp4", ["webm"] = "video/webm", ["mov"] = "video/quicktime",
        ["mkv"] = "video/x-matroska", ["avi"] = "video/x-msvideo", ["wmv"] = "video/x-ms-wmv",
    };

    public static string Mime(string name) => Mimes.TryGetValue(Ext(name), out var m) ? m : "application/octet-stream";

    public static string Esc(string? s) => System.Net.WebUtility.HtmlEncode(s ?? "");
}

/// <summary>Validation of Telegram Mini App initData signature.</summary>
public static class TgAuth
{
    public static JsonObject? Validate(string? initData, int maxAge = 12 * 3600) => Validate(initData, Cfg.Token, maxAge);

    public static JsonObject? Validate(string? initData, string botToken, int maxAge)
    {
        if (string.IsNullOrEmpty(initData) || string.IsNullOrEmpty(botToken)) return null;
        var pairs = new SortedDictionary<string, string>(StringComparer.Ordinal);
        string? received = null;
        foreach (var part in initData.Split('&'))
        {
            var i = part.IndexOf('=');
            var k = Uri.UnescapeDataString((i < 0 ? part : part[..i]).Replace('+', ' '));
            var v = i < 0 ? "" : Uri.UnescapeDataString(part[(i + 1)..].Replace('+', ' '));
            if (k == "hash") received = v; else pairs[k] = v;
        }
        if (received == null) return null;
        var check = string.Join("\n", pairs.Select(p => $"{p.Key}={p.Value}"));
        var secret = HMACSHA256.HashData(Encoding.UTF8.GetBytes("WebAppData"), Encoding.UTF8.GetBytes(botToken));
        var calc = Convert.ToHexString(HMACSHA256.HashData(secret, Encoding.UTF8.GetBytes(check))).ToLowerInvariant();
        if (!CryptographicOperations.FixedTimeEquals(Encoding.ASCII.GetBytes(calc), Encoding.ASCII.GetBytes(received.ToLowerInvariant()))) return null;
        try
        {
            if (!pairs.TryGetValue("auth_date", out var ad) || DateTimeOffset.UtcNow.ToUnixTimeSeconds() - long.Parse(ad) > maxAge) return null;
            return JsonNode.Parse(pairs.GetValueOrDefault("user", "{}")) as JsonObject;
        }
        catch { return null; }
    }

    public static long UserId(JsonObject? u) => u?["id"] is JsonNode n ? n.GetValue<long>() : 0;

    public static string FmtUser(JsonObject? u)
    {
        if (u == null) return "";
        var name = string.Join(" ", new[] { u["first_name"]?.ToString(), u["last_name"]?.ToString() }.Where(x => !string.IsNullOrEmpty(x)));
        if (name == "") name = L.Pc("без имени");
        var un = u["username"]?.ToString();
        return string.IsNullOrEmpty(un) ? name : $"{name} (@{un})";
    }
}

public static class Json
{
    public static readonly JsonSerializerOptions Relaxed = new() { Encoder = System.Text.Encodings.Web.JavaScriptEncoder.UnsafeRelaxedJsonEscaping };
    public static readonly JsonSerializerOptions Pretty = new() { WriteIndented = true, Encoder = System.Text.Encodings.Web.JavaScriptEncoder.UnsafeRelaxedJsonEscaping };
}
