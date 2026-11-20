using System.Collections.Concurrent;
using System.Text;
using System.Text.Json.Nodes;
using System.Text.RegularExpressions;
using Microsoft.AspNetCore.Http;

namespace TgDesk;

public class ApiError : Exception
{
    public int Status;
    public ApiError(int status, string msg) : base(msg) { Status = status; }
}

/// <summary>Authenticated request: Telegram user from initData + TgDesk user record + language.</summary>
public sealed record Ctx(JsonObject J, TgUser U, string Lang);

public static class App
{
    public static AccessLog Alog = null!;
    public static StreamOverlay? Overlay;
    public static Tunnel Tunnel = new();
    public static BotService Bot = new();
    public static DateTime Started = DateTime.Now;
    public static IHostApplicationLifetime? Life;
    public static void EnsureTunnel() => Tunnel.Start();

    public static (JsonObject? user, string reason) DenyReason(string? init, string? perm = null)
    {
        if (string.IsNullOrEmpty(init)) return (null, L.Pc("Попытка входа без подписи Telegram"));
        var u = TgAuth.Validate(init);
        if (u == null) return (null, L.Pc("Неверная или просроченная подпись"));
        var tu = Users.Get(TgAuth.UserId(u));
        if (tu == null) return (u, L.Pc("Чужой Telegram-аккаунт"));
        return (u, L.PcF("Нет прав: {0}", L.Pc(PermTitle(perm ?? ""))));
    }

    public static string PermTitle(string p) => p switch
    {
        Perm.Screen => "экран", Perm.Input => "мышь и клавиатура", Perm.Files => "файлы", Perm.Processes => "процессы",
        Perm.Media => "звук и медиа", Perm.Power => "питание", _ => p,
    };

    // ---- tokens for media (<img>/<video> cannot send headers)
    static readonly ConcurrentDictionary<string, (JsonObject user, DateTime exp)> MediaTokens = new();

    public static string NewMediaToken(JsonObject user)
    {
        var now = DateTime.UtcNow;
        foreach (var k in MediaTokens.Where(p => p.Value.exp < now).Select(p => p.Key).ToList()) MediaTokens.TryRemove(k, out _);
        var t = Convert.ToBase64String(System.Security.Cryptography.RandomNumberGenerator.GetBytes(24)).Replace('+', '-').Replace('/', '_').TrimEnd('=');
        MediaTokens[t] = (user, now.AddHours(12));
        return t;
    }

    public static JsonObject MediaAuth(HttpContext c)
    {
        if (MediaTokens.TryGetValue(c.Request.Query["t"].ToString(), out var v) && v.exp > DateTime.UtcNow && Users.Can(Users.Get(TgAuth.UserId(v.user)), Perm.Files)) return v.user;
        var meta = ReqMeta.From(c.Request);
        Alog.Deny(L.Pc("Медиа без действующего токена"), meta, null, c.Request.Path);
        Security.Fail(meta.Ip);
        throw new ApiError(401, "unauthorized");
    }

    /// <summary>Checks the Telegram signature and that the user exists (and has <paramref name="perm"/>, if given).</summary>
    public static Ctx Auth(HttpContext c, string? perm)
    {
        var init = c.Request.Headers["X-Init-Data"].ToString();
        var user = TgAuth.Validate(init);
        var tu = user == null ? null : Users.Get(TgAuth.UserId(user));
        if (user == null || tu == null)
        {
            var (u, reason) = DenyReason(init);
            var meta = ReqMeta.From(c.Request);
            Alog.Deny(reason, meta, u, c.Request.Path);
            Security.Fail(meta.Ip);
            throw new ApiError(401, "unauthorized");
        }
        var lang = Cfg.TgLang(user["language_code"]?.ToString());
        Users.Touch(tu.Id, TgAuth.FmtUser(user).Split(" (@")[0], user["username"]?.ToString(), user["language_code"]?.ToString());
        if (perm != null && !Users.Can(tu, perm))
        {
            Alog.Deny(L.PcF("Нет прав: {0}", L.Pc(PermTitle(perm))), ReqMeta.From(c.Request), user, c.Request.Path);
            throw new ApiError(403, L.T("Эта функция отключена для тебя на ПК", lang));
        }
        return new Ctx(user, tu, lang);
    }

    public static void Audit(HttpContext c, Ctx x, string cat, string action, string? detail = null) =>
        Alog.Add(cat, action, x.J, ReqMeta.From(c.Request), detail);
}

public static class Routes
{
    static IResult J(JsonNode n, int status = 200) => Results.Content(n.ToJsonString(Json.Relaxed), "application/json; charset=utf-8", Encoding.UTF8, status);
    static IResult Err(string msg, int status) => J(new JsonObject { ["error"] = msg }, status);

    static async Task<JsonObject> Body(HttpContext c)
    {
        try
        {
            using var ms = new MemoryStream();
            var buf = new byte[8192]; int n;
            while ((n = await c.Request.Body.ReadAsync(buf)) > 0) { ms.Write(buf, 0, n); if (ms.Length > 1 << 20) throw new ArgumentException("request too large"); }
            return JsonNode.Parse(ms.ToArray()) as JsonObject ?? throw new ArgumentException("JSON object expected");
        }
        catch (System.Text.Json.JsonException) { throw new ArgumentException("bad JSON"); }
    }

    static async Task<IResult> Guard(HttpContext c, string? perm, Func<Ctx, Task<IResult>> f)
    {
        Ctx x;
        try { x = App.Auth(c, perm); }
        catch (ApiError e) when (e.Status == 401) { return Results.Text("unauthorized", statusCode: 401); }
        catch (ApiError e) { return Err(e.Message, e.Status); }
        try { return await f(x); }
        catch (ApiError e) { return Err(e.Message, e.Status); }
        catch (FileNotFoundException) { return Err(L.T("Не найдено", x.Lang), 404); }
        catch (DirectoryNotFoundException) { return Err(L.T("Не найдено", x.Lang), 404); }
        catch (UnauthorizedAccessException) { return Err(L.T("Недостаточно прав", x.Lang), 403); }
        catch (System.ComponentModel.Win32Exception) { return Err(L.T("Недостаточно прав", x.Lang), 403); }
        catch (ArgumentException e) { return Err(e.Message, 400); }
        catch (Exception e) { Log.Error("API error " + c.Request.Path, e); return Err(e.Message, 500); }
    }

    static string Q(HttpContext c, string k) => c.Request.Query[k].ToString();
    static Task Run(HttpContext c, Task<IResult> t) => RunAsync(c, t);
    static async Task RunAsync(HttpContext c, Task<IResult> t) => await (await t).ExecuteAsync(c);

    public static void Map(WebApplication app)
    {
        app.MapGet("/", (HttpContext c) =>
        {
            c.Response.Headers.CacheControl = "no-cache";
            return Results.File(Path.Combine(Cfg.WebappDir, "index.html"), "text/html; charset=utf-8");
        });

        app.MapPost("/api/hello", c => Run(c, Guard(c, null, async x =>
        {
            App.Audit(c, x, "auth", L.Pc("Открыл приложение"));
            return J(new JsonObject
            {
                ["ok"] = true, ["mt"] = App.NewMediaToken(x.J), ["quality"] = Remote.DefaultQuality(),
                ["role"] = x.U.Role, ["perms"] = new JsonArray(Users.Effective(x.U).Select(p => (JsonNode)p).ToArray()),
                ["version"] = Cfg.Version, ["host"] = Environment.MachineName,
            });
        })));

        app.MapGet("/api/status", c => Run(c, Guard(c, null, async x => J(await Sys.Status()))));

        app.MapGet("/api/processes", c => Run(c, Guard(c, Perm.Processes, async x =>
        {
            App.Audit(c, x, "view", L.Pc("Смотрит процессы"));
            return J(new JsonObject { ["procs"] = await Sys.Processes() });
        })));

        app.MapPost("/api/kill", c => Run(c, Guard(c, Perm.Processes, async x =>
        {
            var b = await Body(c);
            int pid = b["pid"]?.GetValue<int>() ?? 0;
            string msg;
            try { msg = await Task.Run(() => Sys.Kill(pid, x.Lang)); }
            catch (ArgumentException e) when (e.Message.Contains("not running")) { throw new ApiError(404, L.T("Нет такого процесса", x.Lang)); }
            App.Audit(c, x, "control", L.Pc("Завершил процесс"), $"pid {pid}");
            return J(new JsonObject { ["ok"] = true, ["msg"] = msg });
        })));

        app.MapGet("/api/screenshot", c => Run(c, Guard(c, Perm.Screen, async x =>
        {
            byte[] data;
            try { data = await Task.Run(() => Grabber.Snapshot(false)); }
            catch (Exception ex) { Log.Warn("Screenshot failed: " + ex.Message); throw new ArgumentException(L.T("Экран недоступен — ПК заблокирован или спит", x.Lang)); }
            App.Audit(c, x, "view", L.Pc("Снимок экрана"));
            c.Response.Headers.CacheControl = "no-store";
            return Results.File(data, "image/jpeg");
        })));

        app.MapPost("/api/media", c => Run(c, Guard(c, Perm.Media, async x =>
        {
            var b = await Body(c);
            var action = b["action"]?.ToString() ?? "";
            if (!Native.MediaActions.Contains(action)) throw new ArgumentException("unknown action");
            await Task.Run(() => Native.Media(action));
            var names = new Dictionary<string, string> { ["volup"] = "Громче", ["voldown"] = "Тише", ["mute"] = "Без звука", ["playpause"] = "Play/Pause", ["next"] = "Следующий трек", ["prev"] = "Предыдущий трек" };
            App.Audit(c, x, "control", L.Pc("Медиа"), L.Pc(names[action]));
            return J(new JsonObject { ["ok"] = true });
        })));

        app.MapPost("/api/power", c => Run(c, Guard(c, Perm.Power, async x =>
        {
            var b = await Body(c);
            var action = b["action"]?.ToString() ?? "";
            var titles = new Dictionary<string, string> { ["lock"] = "Заблокировал ПК", ["sleep"] = "Отправил ПК в сон", ["restart"] = "Перезагрузка ПК", ["shutdown"] = "Выключение ПК" };
            var labels = new Dictionary<string, string> { ["lock"] = "ПК заблокирован", ["sleep"] = "Ухожу в сон", ["restart"] = "Перезагрузка через 5 сек", ["shutdown"] = "Выключение через 5 сек" };
            if (!titles.ContainsKey(action)) throw new ArgumentException("unknown action");
            App.Audit(c, x, "power", L.Pc(titles[action]));
            _ = Task.Run(async () => { await Task.Delay(1000); Sys.Power(action); });
            return J(new JsonObject { ["ok"] = true, ["msg"] = L.T(labels[action], x.Lang) });
        })));

        // ---- clipboard: writing is "typing" (input); reading also needs the screen permission — whoever may read
        //      the clipboard could see it on the screen anyway. The text itself never goes to the access log.
        app.MapGet("/api/clipboard", c => Run(c, Guard(c, Perm.Input, async x =>
        {
            if (!Users.Can(x.U, Perm.Screen)) throw new ApiError(403, L.T("Эта функция отключена для тебя на ПК", x.Lang));
            var (text, cut) = await PcClipboard.Get();
            App.Audit(c, x, "control", L.Pc("Взял текст из буфера ПК"), L.PcF("{0} симв.", text.Length));
            return J(new JsonObject { ["text"] = text, ["cut"] = cut });
        })));

        app.MapPost("/api/clipboard", c => Run(c, Guard(c, Perm.Input, async x =>
        {
            var text = (await Body(c))["text"]?.ToString() ?? "";
            if (text.Length == 0) throw new ArgumentException(L.T("Пустой текст", x.Lang));
            if (text.Length > PcClipboard.MaxChars) throw new ArgumentException(L.T("Слишком длинный текст", x.Lang));
            await PcClipboard.Set(text);
            App.Audit(c, x, "control", L.Pc("Положил текст в буфер ПК"), L.PcF("{0} симв.", text.Length));
            return J(new JsonObject { ["ok"] = true, ["msg"] = L.T("Скопировано в буфер ПК", x.Lang) });
        })));

        app.MapGet("/api/ls", c => Run(c, Guard(c, Perm.Files, async x =>
        {
            var path = Q(c, "path");
            App.Audit(c, x, "files", L.Pc("Открыл папку"), path == "" ? L.Pc("Быстрый доступ") : path);
            return J(await Task.Run(() => Sys.ListDir(path, x.Lang)));
        })));

        app.MapPost("/api/send", c => Run(c, Guard(c, Perm.Files, async x =>
        {
            var b = await Body(c);
            var path = b["path"]?.ToString() ?? "";
            if (!File.Exists(path)) throw new FileNotFoundException(path);
            long size = new FileInfo(path).Length;
            if (size > BotService.MaxSend) throw new ArgumentException(L.F("Файл {0} — больше лимита Telegram (~50 МБ)", x.Lang, Util.HumanBytes(size)));
            await App.Bot.SendFileToChat(x.U.Id, path, x.Lang);
            App.Audit(c, x, "files", L.Pc("Прислал файл в чат"), $"{path} ({Util.HumanBytes(size)})");
            return J(new JsonObject { ["ok"] = true, ["msg"] = L.T("Файл отправлен в чат", x.Lang) });
        })));

        app.MapPost("/api/open", c => Run(c, Guard(c, Perm.Files, async x =>
        {
            var path = (await Body(c))["path"]?.ToString() ?? "";
            if (!File.Exists(path) && !Directory.Exists(path)) throw new FileNotFoundException(path);
            await Task.Run(() => Sys.OpenOnPc(path));
            App.Audit(c, x, "files", L.Pc("Открыл на ПК"), path);
            return J(new JsonObject { ["ok"] = true, ["msg"] = L.T("Открыто на ПК", x.Lang) });
        })));

        app.MapPost("/api/reveal", c => Run(c, Guard(c, Perm.Files, async x =>
        {
            var path = (await Body(c))["path"]?.ToString() ?? "";
            if (!File.Exists(path) && !Directory.Exists(path)) throw new FileNotFoundException(path);
            await Task.Run(() => Sys.RevealOnPc(path));
            App.Audit(c, x, "files", L.Pc("Показал в папке на ПК"), path);
            return J(new JsonObject { ["ok"] = true, ["msg"] = L.T("Папка открыта на ПК", x.Lang) });
        })));

        app.MapPost("/api/mkdir", c => Run(c, Guard(c, Perm.Files, async x =>
        {
            var b = await Body(c);
            var parent = b["dir"]?.ToString() ?? "";
            var name = Util.Sanitize(b["name"]?.ToString(), L.T("Новая папка", x.Lang));
            if (!Directory.Exists(parent)) throw new DirectoryNotFoundException(parent);
            var path = Path.Combine(parent, name);
            if (Directory.Exists(path) || File.Exists(path)) throw new ArgumentException(L.T("Такая папка уже есть", x.Lang));
            Directory.CreateDirectory(path);
            App.Audit(c, x, "files", L.Pc("Создал папку"), path);
            return J(new JsonObject { ["ok"] = true, ["path"] = path });
        })));

        app.MapPost("/api/upload", c => Run(c, Guard(c, Perm.Files, x => Upload(c, x))));
        app.MapPost("/api/upload_cancel", c => Run(c, Guard(c, Perm.Files, async x =>
        {
            var id = (await Body(c))["id"]?.ToString() ?? "";
            if (Uploads.TryRemove(id, out var st)) { try { File.Delete(st.tmp); } catch { } }
            return J(new JsonObject { ["ok"] = true });
        })));

        // Autostart of the PC is a setting of this PC: only owners may change it from the phone.
        app.MapMethods("/api/autostart", new[] { "GET", "POST" }, c => Run(c, Guard(c, null, async x =>
        {
            if (c.Request.Method == "POST")
            {
                if (!x.U.IsOwner) throw new ApiError(403, L.T("Только владелец может менять эту настройку", x.Lang));
                var en = (await Body(c))["enabled"]?.GetValue<bool>() ?? false;
                Cfg.Autostart = en; Cfg.Save(); Sys.AutostartSet(en);
                App.Audit(c, x, "control", en ? L.Pc("Автозапуск включён") : L.Pc("Автозапуск выключен"));
            }
            return J(new JsonObject { ["enabled"] = Sys.AutostartGet(), ["can_change"] = x.U.IsOwner });
        })));

        app.MapGet("/ws", Remote.Handle);

        app.MapGet("/m/file", c => MediaFile(c));
        app.MapGet("/m/thumb", async c =>
        {
            try { App.MediaAuth(c); } catch (ApiError e) { c.Response.StatusCode = e.Status; return; }
            var path = Q(c, "path");
            if (!File.Exists(path) || Util.Kind(path) != "image") { c.Response.StatusCode = 404; return; }
            try
            {
                int.TryParse(Q(c, "s"), out var s); if (s == 0) s = 256;
                var data = await Task.Run(() => Sys.Thumb(path, s));
                c.Response.ContentType = "image/jpeg";
                c.Response.Headers.CacheControl = "private, max-age=3600";
                await c.Response.Body.WriteAsync(data);
            }
            catch { c.Response.StatusCode = 404; }
        });
    }

    // ---------------------------------------------------------- media files
    static readonly ConcurrentDictionary<(string, string), DateTime> MediaSeen = new();

    static async Task MediaFile(HttpContext c)
    {
        JsonObject user;
        try { user = App.MediaAuth(c); } catch (ApiError e) { c.Response.StatusCode = e.Status; return; }
        var path = Q(c, "path");
        if (!File.Exists(path)) { c.Response.StatusCode = 404; return; }
        var rng = c.Request.Headers.Range.ToString();
        if (Q(c, "log") == "1" && (rng == "" || rng.StartsWith("bytes=0-")))
        {
            var meta = ReqMeta.From(c.Request);
            var key = (path, meta.Ip); var now = DateTime.UtcNow;
            if (!MediaSeen.TryGetValue(key, out var last) || (now - last).TotalSeconds > 60)
            {
                MediaSeen[key] = now;
                App.Alog.Add("files", L.Pc("Смотрит/слушает в приложении"), user, meta, path);
            }
        }
        c.Response.Headers.CacheControl = "private, max-age=600";
        await Results.File(path, Util.Mime(path), enableRangeProcessing: true).ExecuteAsync(c);
    }

    // ---------------------------------------------------------- chunked upload
    static readonly ConcurrentDictionary<string, (string tmp, long received, long total, DateTime t)> Uploads = new();
    static readonly Regex UploadId = new("^[A-Za-z0-9_-]{8,64}$", RegexOptions.Compiled);
    const long MaxUpload = 8L * 1024 * 1024 * 1024;

    static async Task<IResult> Upload(HttpContext c, Ctx x)
    {
        foreach (var kv in Uploads.Where(p => (DateTime.UtcNow - p.Value.t).TotalHours > 2).ToList())
        {
            if (Uploads.TryRemove(kv.Key, out var old)) { try { File.Delete(old.tmp); } catch { } }
        }
        var folder = Q(c, "dir"); var uid = Q(c, "id");
        if (!long.TryParse(Q(c, "total"), out var total)) total = -1;
        if (!long.TryParse(Q(c, "offset"), out var offset)) offset = -1;
        if (!UploadId.IsMatch(uid) || total < 0 || offset < 0) throw new ArgumentException("bad upload params");
        if (!Directory.Exists(folder)) throw new DirectoryNotFoundException(folder);
        if (total > MaxUpload) throw new ArgumentException(L.T("Файл слишком большой", x.Lang));
        var name = Util.Sanitize(Q(c, "name"), "upload");
        var tmp = Path.Combine(folder, $".tgdesk-{uid}.part");
        Uploads.TryGetValue(uid, out var st);
        if (offset == 0 || st.tmp == null)
        {
            if (offset != 0) throw new ArgumentException(L.T("Загрузка не найдена, начни заново", x.Lang));
            File.WriteAllBytes(tmp, Array.Empty<byte>());
            st = (tmp, 0, total, DateTime.UtcNow);
        }
        if (offset > st.received) throw new ArgumentException(L.T("Пропущен кусок файла", x.Lang));
        long n = 0;
        await using (var f = new FileStream(tmp, FileMode.Open, FileAccess.Write, FileShare.None, 1 << 16, true))
        {
            f.Seek(offset, SeekOrigin.Begin);
            var buf = new byte[1 << 20]; int r;
            while ((r = await c.Request.Body.ReadAsync(buf)) > 0)
            {
                if (offset + n + r > total) throw new ArgumentException(L.T("Данных больше, чем заявлено", x.Lang));
                await f.WriteAsync(buf.AsMemory(0, r));
                n += r;
            }
            f.SetLength(offset + n);
        }
        st.received = offset + n; st.t = DateTime.UtcNow;
        Uploads[uid] = st;
        if (st.received >= total)
        {
            Uploads.TryRemove(uid, out _);
            var final = Util.UniquePath(folder, name);
            File.Move(tmp, final);
            App.Audit(c, x, "files", L.Pc("Загрузил с телефона"), $"{final} ({Util.HumanBytes(total)})");
            return J(new JsonObject { ["done"] = true, ["path"] = final, ["name"] = Path.GetFileName(final) });
        }
        return J(new JsonObject { ["done"] = false, ["received"] = st.received });
    }

    // =====================================================================================
    //  Access log web page (127.0.0.1:LOG_PORT) — opens only on this PC
    // =====================================================================================
    public static void MapLogs(WebApplication app)
    {
        app.MapGet("/_log", (HttpContext c) => { c.Response.Headers.CacheControl = "no-cache"; return Results.File(Path.Combine(Cfg.WebappDir, "logs.html"), "text/html; charset=utf-8"); });
        app.MapGet("/_log/api/log", (HttpContext c) =>
        {
            int.TryParse(Q(c, "after"), out var after);
            return J(new JsonObject
            {
                ["host"] = Environment.MachineName,
                ["lang"] = Cfg.PcLang,
                ["items"] = App.Alog.Query(after),
                ["updates"] = App.Alog.Updates(),
                ["stats"] = App.Alog.Stats(),
                ["active"] = App.Alog.ActiveList(),
            });
        });
        app.MapGet("/_log/api/log.csv", () => Results.File(Encoding.UTF8.GetBytes(App.Alog.ToCsv()), "text/csv; charset=utf-8", "tgdesk_access_log.csv"));
    }

    // =====================================================================================
    //  Control API for the desktop app (127.0.0.1:CONTROL_PORT, X-Control-Token required)
    // =====================================================================================
    public static void MapControl(WebApplication app)
    {
        app.MapGet("/_ctl/state", () =>
        {
            var pairing = Pairing.State();
            if (pairing["active"]?.GetValue<bool>() == true && App.Bot.Username != null)
                pairing["link"] = $"https://t.me/{App.Bot.Username}?start=pair_{pairing["code"]}";
            return J(new JsonObject
            {
                ["version"] = Cfg.Version,
                ["pid"] = Environment.ProcessId,
                ["host"] = Environment.MachineName,
                ["uptime"] = (int)(DateTime.Now - App.Started).TotalSeconds,
                ["configured"] = Cfg.Configured,
                ["lang"] = Cfg.Lang,
                ["pc_lang"] = Cfg.PcLang,
                ["tunnel_url"] = App.Tunnel.Url,
                ["tunnel_state"] = App.Tunnel.State,
                ["tunnel_error"] = App.Tunnel.Error,
                ["tunnel"] = new JsonObject { ["mode"] = Cfg.TunnelMode, ["public_url"] = Cfg.PublicUrl, ["has_token"] = Cfg.TunnelToken != "" },
                ["bot_username"] = App.Bot.Username,
                ["bot_state"] = App.Bot.State,
                ["autostart"] = Sys.AutostartGet(),
                ["quality"] = Cfg.Quality,
                ["update_check"] = Cfg.UpdateCheck,
                ["geoip"] = Cfg.GeoIp,
                ["auto_update"] = Cfg.AutoUpdate,
                ["portable"] = Updater.Portable,
                ["update"] = Updater.ToJson(),
                ["features"] = new JsonObject(Cfg.Features.Select(kv => KeyValuePair.Create(kv.Key, (JsonNode?)kv.Value))),
                ["users"] = new JsonArray(Users.All.Select(u => (JsonNode)u.ToJson()).ToArray()),
                ["pairing"] = pairing,
                ["last_paired"] = Pairing.LastPaired?.DeepClone(),
                ["blocked_ips"] = Security.BlockedCount,
                ["sessions"] = App.Alog.ActiveList(),
                ["stats"] = App.Alog.Stats(),
                ["ports"] = new JsonObject { ["web"] = Cfg.Port, ["log"] = Cfg.LogPort, ["control"] = Cfg.ControlPort },
                ["dirs"] = new JsonObject { ["data"] = Cfg.Data, ["install"] = Cfg.Install, ["downloads"] = Cfg.DownloadDir },
            });
        });

        app.MapGet("/_ctl/status", async () => J(await Sys.Status()));

        app.MapGet("/_ctl/log", (HttpContext c) =>
        {
            int.TryParse(Q(c, "after"), out var after);
            return J(new JsonObject { ["items"] = App.Alog.Query(after, 300), ["updates"] = App.Alog.Updates(), ["stats"] = App.Alog.Stats() });
        });

        app.MapGet("/_ctl/syslog", (HttpContext c) =>
        {
            int.TryParse(Q(c, "lines"), out var n); if (n <= 0) n = 200;
            string[] lines;
            try
            {
                using var fs = new FileStream(Cfg.LogPath, FileMode.Open, FileAccess.Read, FileShare.ReadWrite);
                using var sr = new StreamReader(fs, Encoding.UTF8);
                lines = sr.ReadToEnd().Split('\n');
            }
            catch { lines = Array.Empty<string>(); }
            return Results.Text(string.Join("\n", lines.Skip(Math.Max(0, lines.Length - n))).Replace("\r", ""), "text/plain; charset=utf-8");
        });

        // ---- first-run setup: bot token (checked with getMe before it is saved)
        app.MapPost("/_ctl/setup/token", async (HttpContext c) =>
        {
            var token = ((await Body(c))["token"]?.ToString() ?? "").Trim();
            if (!Regex.IsMatch(token, @"^\d{5,15}:[A-Za-z0-9_-]{30,64}$")) return J(new JsonObject { ["ok"] = false, ["error"] = "format" });
            var (ok, username, error) = await BotService.CheckToken(token);
            if (!ok) return J(new JsonObject { ["ok"] = false, ["error"] = error });
            bool changed = token != Cfg.Token;
            Cfg.Token = token;
            Cfg.Save();
            if (changed)
            {
                Log.Info($"Bot token set (@{username})");
                App.Alog.Add("system", L.Pc("Токен бота изменён"), detail: "@" + username);
                App.Bot.Restart();
                App.EnsureTunnel();
            }
            return J(new JsonObject { ["ok"] = true, ["username"] = username });
        });

        // ---- users and pairing
        app.MapPost("/_ctl/pair/start", async (HttpContext c) =>
        {
            var b = await Body(c);
            var role = b["role"]?.ToString() == "owner" ? "owner" : "user";
            var perms = (b["perms"] as JsonArray)?.Select(x => x?.ToString() ?? "") ?? Perm.All;
            if (!Cfg.Configured) return J(new JsonObject { ["ok"] = false, ["error"] = "not_configured" });
            var (code, secs) = Pairing.Start(role, perms);
            return J(new JsonObject
            {
                ["ok"] = true, ["code"] = code, ["expires_in"] = secs,
                ["link"] = App.Bot.Username == null ? null : $"https://t.me/{App.Bot.Username}?start=pair_{code}",
            });
        });
        app.MapPost("/_ctl/pair/cancel", () => { Pairing.Cancel(); return J(new JsonObject { ["ok"] = true }); });

        app.MapPost("/_ctl/users/update", async (HttpContext c) =>
        {
            var b = await Body(c);
            long id = b["id"]?.GetValue<long>() ?? 0;
            var perms = (b["perms"] as JsonArray)?.Select(x => x?.ToString() ?? "").ToList();
            var ok = Users.Update(id, b["role"]?.ToString(), perms);
            if (ok)
            {
                App.Alog.Add("system", L.Pc("Права пользователя изменены"), detail: id.ToString());
                Remote.KickUnauthorized();
                _ = App.Bot.RefreshUser(id);
            }
            return J(new JsonObject { ["ok"] = ok });
        });

        app.MapPost("/_ctl/users/remove", async (HttpContext c) =>
        {
            long id = (await Body(c))["id"]?.GetValue<long>() ?? 0;
            var u = Users.Get(id);
            var ok = Users.Remove(id);
            if (ok)
            {
                App.Alog.Add("system", L.Pc("Пользователь удалён"), detail: $"{u?.Name} ({id})");
                Remote.KickUnauthorized();
            }
            return J(new JsonObject { ["ok"] = ok });
        });

        // ---- settings (everything the app may change)
        app.MapPost("/_ctl/settings", async (HttpContext c) =>
        {
            var b = await Body(c);
            bool tunnelChanged = false;
            if (b["quality"]?.ToString() is string q && (q is "eco" or "norm" or "hd" or "auto")) Cfg.Quality = q;
            if (b["lang"]?.ToString() is string l && (l is "auto" or "ru" or "en")) Cfg.Lang = l;
            if (b["autostart"] is JsonValue av && av.TryGetValue<bool>(out var auto)) { Cfg.Autostart = auto; Sys.AutostartSet(auto); }
            if (b["update_check"] is JsonValue uv && uv.TryGetValue<bool>(out var upd)) { Cfg.UpdateCheck = upd; if (upd) _ = Updater.CheckNow(); }
            if (b["geoip"] is JsonValue gv && gv.TryGetValue<bool>(out var geo)) Cfg.GeoIp = geo;
            if (b["auto_update"] is JsonValue au && au.TryGetValue<bool>(out var autoUpd)) Cfg.AutoUpdate = autoUpd;
            if (b["features"] is JsonObject f)
            {
                foreach (var (k, v) in f)
                    if (Perm.Valid(k) && v is JsonValue fv && fv.TryGetValue<bool>(out var on)) Cfg.Features[k] = on;
                Remote.KickUnauthorized();
                _ = App.Bot.RefreshAllKeyboards();
            }
            if (b["tunnel"] is JsonObject t)
            {
                var mode = t["mode"]?.ToString();
                var url = (t["public_url"]?.ToString() ?? Cfg.PublicUrl).Trim().TrimEnd('/');
                if (mode is "quick" or "cloudflare" or "external")
                {
                    if (mode != "quick" && !Tunnel.ValidPublicUrl(url)) return J(new JsonObject { ["ok"] = false, ["error"] = "bad_url" }, 400);
                    if (t["token"]?.ToString() is string tok && tok.Trim() != "") Cfg.TunnelToken = tok.Trim();
                    if (mode == "cloudflare" && Cfg.TunnelToken == "") return J(new JsonObject { ["ok"] = false, ["error"] = "no_token" }, 400);
                    tunnelChanged = mode != Cfg.TunnelMode || url != Cfg.PublicUrl || t["token"] != null;
                    Cfg.TunnelMode = mode; Cfg.PublicUrl = mode == "quick" ? Cfg.PublicUrl : url;
                }
            }
            Cfg.Save();
            if (tunnelChanged) { App.Alog.Add("system", L.Pc("Адрес Mini App изменён"), detail: Cfg.TunnelMode); App.Tunnel.Restart(); }
            return J(new JsonObject { ["ok"] = true });
        });

        // ---- updates: check now / download + verify + install (the installer restarts TgDesk)
        app.MapPost("/_ctl/update/check", async () =>
        {
            await Updater.CheckNow();
            return J(new JsonObject { ["ok"] = true, ["update"] = Updater.ToJson() });
        });
        app.MapPost("/_ctl/update/install", () =>
        {
            if (!Updater.Available) return J(new JsonObject { ["ok"] = false, ["error"] = "no_update" });
            if (!Updater.CanInstall) return J(new JsonObject { ["ok"] = false, ["error"] = Updater.Portable ? "portable" : "no_installer" });
            _ = Task.Run(() => Updater.Install("app"));
            return J(new JsonObject { ["ok"] = true });
        });

        app.MapPost("/_ctl/tunnel/restart", () => { App.Tunnel.Restart(); return J(new JsonObject { ["ok"] = true }); });

        app.MapPost("/_ctl/kick", async (HttpContext c) =>
        {
            var sid = (await Body(c))["sid"]?.ToString() ?? "";
            if (sid == "*") { foreach (var k in Remote.Live.Keys) Remote.Kick(k); return J(new JsonObject { ["ok"] = true }); }
            return J(new JsonObject { ["ok"] = Remote.Kick(sid) });
        });

        app.MapPost("/_ctl/shutdown", () =>
        {
            _ = Task.Run(async () => { await Task.Delay(300); App.Life?.StopApplication(); });
            return J(new JsonObject { ["ok"] = true });
        });
    }
}
