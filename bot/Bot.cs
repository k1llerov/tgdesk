using System.Collections.Concurrent;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json.Nodes;
using Telegram.Bot;
using Telegram.Bot.Exceptions;
using Telegram.Bot.Polling;
using Telegram.Bot.Types;
using Telegram.Bot.Types.Enums;
using Telegram.Bot.Types.ReplyMarkups;
using Message = Telegram.Bot.Types.Message;

namespace TgDesk;

/// <summary>
/// Telegram bot. Answers only users from users.json (in private chats). Strangers are ignored,
/// except for a pairing code created in the desktop app.
/// State codes: not_configured | connecting | online | bad_token | offline.
/// </summary>
public sealed class BotService : IUpdateHandler
{
    public const long MaxSend = 50 * 1024 * 1024;
    const long TgDownloadLimit = 20 * 1024 * 1024; // Bot API does not give bots files larger than 20 MB

    public TelegramBotClient Client = null!;
    public string? Username;
    public string State { get; private set; } = "not_configured";
    readonly ConcurrentDictionary<string, string> _paths = new();
    readonly Queue<string> _pathOrder = new();
    bool _announced;
    string? _menuUrl;
    DateTime _netLast = DateTime.MinValue; int _netSuppressed;
    CancellationTokenSource? _cts;
    CancellationToken _appCt;

    static HttpClient NewHttp() => new(new SocketsHttpHandler { UseProxy = true, ConnectTimeout = TimeSpan.FromSeconds(20) }) { Timeout = TimeSpan.FromMinutes(5) };

    // ------------------------------------------------------------------ start / restart
    public void Start(CancellationToken appCt)
    {
        _appCt = appCt;
        Users.Added += u => _ = OnUserAdded(u);
        Users.Removed += u => _ = OnUserRemoved(u);
        Restart();
    }

    /// <summary>(Re)connects with the current token from config.json.</summary>
    public void Restart()
    {
        _cts?.Cancel();
        Username = null;
        if (!Cfg.Configured) { State = "not_configured"; return; }
        _cts = CancellationTokenSource.CreateLinkedTokenSource(_appCt);
        _announced = false;
        var ct = _cts.Token;
        _ = Task.Run(() => RunAsync(ct), ct);
    }

    /// <summary>Checks a token with getMe before it is saved.</summary>
    public static async Task<(bool ok, string? username, string? error)> CheckToken(string token)
    {
        try
        {
            using var http = NewHttp();
            http.Timeout = TimeSpan.FromSeconds(25);
            var me = await new TelegramBotClient(token, http).GetMe();
            return (true, me.Username, null);
        }
        catch (ApiRequestException e) when (e.ErrorCode is 401 or 404) { return (false, null, "invalid"); }
        catch (Exception e) { Log.Warn("Token check: " + e.Message); return (false, null, "network"); }
    }

    async Task RunAsync(CancellationToken ct)
    {
        var http = NewHttp();
        Client = new TelegramBotClient(Cfg.Token, http);
        while (!ct.IsCancellationRequested)
        {
            try
            {
                Log.Info("Connecting to Telegram…");
                State = "connecting";
                var me = await Client.GetMe(ct);
                Username = me.Username;
                State = "online";
                Log.Info($"Connected to Telegram as @{me.Username}");
                break;
            }
            catch (OperationCanceledException) { return; }
            catch (ApiRequestException e) when (e.ErrorCode == 401)
            {
                State = "bad_token";
                Log.Error("The bot token is invalid (revoked in @BotFather?). Enter a new one in TgDesk → Settings. Retrying in 5 minutes.");
                try { await Task.Delay(TimeSpan.FromMinutes(5), ct); } catch { return; }
            }
            catch (Exception e)
            {
                State = "offline";
                Log.Warn($"No connection to Telegram ({e.Message}) — retrying in 15 seconds");
                try { await Task.Delay(15000, ct); } catch { return; }
            }
        }
        if (ct.IsCancellationRequested) return;

        _ = Task.Run(() => SetupProfile(ct), ct);
        _ = Task.Run(() => Health(ct), ct);
        Client.StartReceiving(this, new ReceiverOptions { AllowedUpdates = new[] { UpdateType.Message, UpdateType.CallbackQuery }, DropPendingUpdates = true }, ct);
        if (App.Tunnel.Url is string u) _ = Task.Run(() => OnTunnelUrl(u, ct), ct); // tunnel came up before the bot
    }

    /// <summary>Polling recovers by itself after network errors; this brings the state back to "online".</summary>
    async Task Health(CancellationToken ct)
    {
        while (!ct.IsCancellationRequested)
        {
            try { await Task.Delay(30000, ct); } catch { return; }
            if (State == "online") continue;
            try { await Client.GetMe(ct); State = "online"; Log.Info("Connection to Telegram restored"); }
            catch (ApiRequestException e) when (e.ErrorCode == 401) { State = "bad_token"; }
            catch { }
        }
    }

    bool Connected => Username != null && Client != null;

    async Task Retry(Func<Task> f, CancellationToken ct, string what)
    {
        for (int i = 0; i < 20 && !ct.IsCancellationRequested; i++)
        {
            try { await f(); return; }
            catch (OperationCanceledException) { return; }
            catch (ApiRequestException e) when (e.ErrorCode is 400 or 403) { Log.Warn($"{what}: {e.Message}"); return; } // user blocked the bot, etc.
            catch (Exception e) { Log.Warn($"{what}: {e.Message} (attempt {i + 1})"); try { await Task.Delay(Math.Min(60, 3 + i * 5) * 1000, ct); } catch { return; } }
        }
    }

    CancellationToken Ct => _cts?.Token ?? _appCt;

    async Task SetupProfile(CancellationToken ct)
    {
        foreach (var lang in new[] { "en", "ru" })
        {
            string? code = lang == "en" ? null : "ru"; // English is the default for all other languages
            await Retry(async () =>
            {
                await Client.SetMyCommands(new[]
                {
                    new BotCommand { Command = "start", Description = L.T("Меню", lang) },
                    new BotCommand { Command = "app", Description = L.T("Открыть приложение", lang) },
                    new BotCommand { Command = "kill", Description = L.T("Завершить процесс", lang) },
                    new BotCommand { Command = "get", Description = L.T("Скачать файл", lang) },
                    new BotCommand { Command = "clip", Description = L.T("Буфер обмена ПК", lang) },
                }, languageCode: code, cancellationToken: ct);
                await Client.SetMyShortDescription(L.T("Пульт управления твоим ПК: экран, мышь, клавиатура, файлы.", lang), languageCode: code, cancellationToken: ct);
                await Client.SetMyDescription(L.T("TgDesk — пульт для твоего компьютера.\nНажми «TgDesk» внизу, чтобы открыть панель.", lang), languageCode: code, cancellationToken: ct);
            }, ct, "Bot description");
        }

        // optional bot avatar: put avatar.jpg into the data folder
        var path = Cfg.AvatarPath; var marker = path + ".sha";
        if (!System.IO.File.Exists(path)) return;
        var data = await System.IO.File.ReadAllBytesAsync(path, ct);
        var digest = Convert.ToHexString(SHA256.HashData(data)).ToLowerInvariant();
        if (System.IO.File.Exists(marker) && System.IO.File.ReadAllText(marker).Trim() == digest) return;
        try
        {
            using var form = new MultipartFormDataContent
            {
                { new StringContent("{\"type\":\"static\",\"photo\":\"attach://avatar\"}"), "photo" },
                { new ByteArrayContent(data), "avatar", "avatar.jpg" },
            };
            using var http = new HttpClient(new SocketsHttpHandler { UseProxy = true }) { Timeout = TimeSpan.FromSeconds(60) };
            var r = await http.PostAsync($"https://api.telegram.org/bot{Cfg.Token}/setMyProfilePhoto", form, ct);
            if (JsonNode.Parse(await r.Content.ReadAsStringAsync(ct))?["ok"]?.GetValue<bool>() == true)
            {
                System.IO.File.WriteAllText(marker, digest);
                Log.Info("Bot avatar updated");
            }
        }
        catch (Exception e) { Log.Warn("Bot avatar: " + Log.Redact(e.Message)); }
    }

    // ------------------------------------------------------------------ keyboards
    static readonly (string ru, string? perm)[] KbItems =
    {
        ("📊 Статус", null), ("📸 Скрин", Perm.Screen), ("⚙️ Процессы", Perm.Processes),
        ("📁 Файлы", Perm.Files), ("🔊 Медиа", Perm.Media), ("🔌 Питание", Perm.Power),
    };

    static ReplyMarkup Keyboard(TgUser u)
    {
        var items = KbItems.Where(i => i.perm == null || Users.Can(u, i.perm)).Select(i => new KeyboardButton(L.T(i.ru, u.Lang))).ToList();
        var rows = items.Chunk(2).Select(r => r.ToArray()).ToArray();
        return new ReplyKeyboardMarkup(rows) { ResizeKeyboard = true };
    }

    /// <summary>Maps a keyboard label in any language back to the Russian key.</summary>
    static string? KbAction(string text)
    {
        foreach (var (ru, _) in KbItems)
            if (text == ru || text == L.T(ru, "en")) return ru;
        return null;
    }

    InlineKeyboardMarkup AppButton(string url, string lang) =>
        new(InlineKeyboardButton.WithWebApp(L.T("📱 Открыть TgDesk", lang), new WebAppInfo(url)));

    // ------------------------------------------------------------------ tunnel → menu button
    public async Task OnTunnelUrl(string url, CancellationToken ct)
    {
        _menuUrl = url;
        if (!Connected) return; // called again right after the bot connects
        foreach (var u in Users.All)
            await Retry(async () =>
            {
                if (_menuUrl != url) return;
                await Client.SetChatMenuButton(u.Id, new MenuButtonWebApp { Text = "TgDesk", WebApp = new WebAppInfo(url) }, ct);
            }, ct, "Menu button");
        Log.Info("Menu button updated for all users");
        App.Alog.Add("system", L.Pc("Mini App доступен"), detail: url);
        if (_announced) return;
        _announced = true;
        foreach (var u in Users.All.Where(x => x.IsOwner))
            await Retry(async () =>
            {
                var msg = Updater.JustUpdatedFrom is string from
                    ? L.F("✅ TgDesk на <b>{0}</b> обновлён: {1} → {2}. Приложение готово.", u.Lang, Util.Esc(Environment.MachineName), Util.Esc(from), Cfg.Version)
                    : L.F("🟢 <b>{0}</b> в сети. Приложение готово.", u.Lang, Util.Esc(Environment.MachineName));
                await Client.SendMessage(u.Id, msg, ParseMode.Html, replyMarkup: AppButton(url, u.Lang), cancellationToken: ct);
            }, ct, "Online notice");
    }

    async Task OnUserAdded(TgUser u)
    {
        if (!Connected) return;
        var ct = Ct;
        await Retry(async () =>
        {
            await Client.SendMessage(u.Id, L.F("✅ Готово! Теперь ты можешь управлять ПК <b>{0}</b>.\nКнопки внизу — быстрые команды.", u.Lang, Util.Esc(Environment.MachineName)),
                ParseMode.Html, replyMarkup: Keyboard(u), cancellationToken: ct);
            if (App.Tunnel.Url is string url)
            {
                await Client.SetChatMenuButton(u.Id, new MenuButtonWebApp { Text = "TgDesk", WebApp = new WebAppInfo(url) }, ct);
                await Client.SendMessage(u.Id, L.T("Панель управления:", u.Lang), replyMarkup: AppButton(url, u.Lang), cancellationToken: ct);
            }
        }, ct, "Welcome");
    }

    async Task OnUserRemoved(TgUser u)
    {
        if (!Connected) return;
        var ct = Ct;
        await Retry(async () =>
        {
            await Client.SetChatMenuButton(u.Id, new MenuButtonDefault(), ct);
            await Client.SendMessage(u.Id, L.T("🔒 Доступ к этому ПК отозван.", u.Lang), replyMarkup: new ReplyKeyboardRemove(), cancellationToken: ct);
        }, ct, "Access revoked notice");
    }

    /// <summary>Permissions of a user changed: send the new keyboard.</summary>
    public async Task RefreshUser(long id)
    {
        var u = Users.Get(id);
        if (u == null || !Connected) return;
        await Retry(async () =>
            await Client.SendMessage(u.Id, L.T("🔄 Права доступа обновлены.", u.Lang), replyMarkup: Keyboard(u), cancellationToken: Ct), Ct, "Permissions notice");
    }

    /// <summary>Global features changed. Keyboards are refreshed on the next /start; disabled buttons answer that the feature is off.</summary>
    public Task RefreshAllKeyboards() => Task.CompletedTask;

    // ------------------------------------------------------------------ files to the chat
    public async Task SendFileToChat(long chatId, string path, string lang)
    {
        for (int attempt = 0; attempt < 3; attempt++)
        {
            try { await SendOnce(chatId, path, lang); return; }
            catch (ApiRequestException e) when (e.ErrorCode == 400) { throw new ArgumentException(L.T("Telegram отклонил файл: ", lang) + e.Message); }
            catch (Exception e) when (e is HttpRequestException or TaskCanceledException or RequestException)
            {
                if (attempt == 2) throw new ArgumentException(L.T("Telegram не ответил вовремя. Попробуй ещё раз.", lang));
                Log.Warn($"Sending {Path.GetFileName(path)}: {e.Message}, retrying");
                await Task.Delay(2000 * (attempt + 1));
            }
        }
    }

    async Task SendOnce(long chatId, string path, string lang)
    {
        long size = new FileInfo(path).Length;
        if (size > MaxSend) throw new ArgumentException(L.F("Файл {0} — больше лимита Telegram для ботов (~50 МБ). Открой его в приложении или на ПК.", lang, Util.HumanBytes(size)));
        var name = Path.GetFileName(path); var e = Util.Ext(path);
        try
        {
            await using var f = System.IO.File.OpenRead(path);
            var file = InputFile.FromStream(f, name);
            if (e is "jpg" or "jpeg" or "png" or "webp" && size <= 10 * 1024 * 1024) { await Client.SendPhoto(chatId, file, caption: name); return; }
            if (e == "gif") { await Client.SendAnimation(chatId, file); return; }
            if (e is "mp3" or "m4a") { await Client.SendAudio(chatId, file); return; }
            if (e is "mp4" or "m4v" or "mov") { await Client.SendVideo(chatId, file, supportsStreaming: true); return; }
        }
        catch (ApiRequestException ex) when (ex.ErrorCode == 400) { Log.Warn($"Could not send {name} as media, sending as a document"); }
        await using var f2 = System.IO.File.OpenRead(path);
        await Client.SendDocument(chatId, InputFile.FromStream(f2, name));
    }

    // ------------------------------------------------------------------ updates
    public async Task HandleUpdateAsync(ITelegramBotClient bot, Update update, CancellationToken ct)
    {
        if (State != "online") State = "online";
        try
        {
            if (update.Message is { } m) await OnMessage(m, ct);
            else if (update.CallbackQuery is { } q) await OnCallback(q, ct);
        }
        catch (Exception e) when (e is not OperationCanceledException)
        {
            Log.Error("Update handler error", e);
        }
    }

    public Task HandleErrorAsync(ITelegramBotClient bot, Exception ex, HandleErrorSource source, CancellationToken ct)
    {
        if (ex is HttpRequestException or TaskCanceledException or RequestException && ex is not ApiRequestException)
        {
            var now = DateTime.UtcNow;
            State = "offline";
            if ((now - _netLast).TotalSeconds >= 60)
            {
                Log.Warn($"No connection to Telegram: {ex.Message}" + (_netSuppressed > 0 ? $" (same errors suppressed: {_netSuppressed})" : ""));
                _netLast = now; _netSuppressed = 0;
            }
            else _netSuppressed++;
        }
        else if (ex is ApiRequestException { ErrorCode: 401 }) { State = "bad_token"; Log.Error("Telegram rejected the token (401)"); }
        else if (ex is ApiRequestException { ErrorCode: 409 }) { Log.Error("Another program is using this bot token (409 Conflict). Each PC needs its own bot."); }
        else Log.Error("Telegram error", ex);
        return Task.CompletedTask;
    }

    static JsonObject UserJson(User? u) => u == null ? new JsonObject() : new JsonObject
    {
        ["id"] = u.Id, ["first_name"] = u.FirstName, ["last_name"] = u.LastName, ["username"] = u.Username, ["language_code"] = u.LanguageCode,
    };

    static string DisplayName(User u) => string.Join(" ", new[] { u.FirstName, u.LastName }.Where(x => !string.IsNullOrEmpty(x)));

    /// <summary>Known user → record (and the action is logged). Stranger → null (logged as denied, no answer).</summary>
    TgUser? Known(User? u, string what)
    {
        var short_ = what.Length > 200 ? what[..200] : what;
        var tu = u == null ? null : Users.Get(u.Id);
        if (tu != null)
        {
            Users.Touch(u!.Id, DisplayName(u), u.Username, u.LanguageCode);
            App.Alog.Add("chat", L.Pc("Команда в чате"), UserJson(u), null, short_, device: L.Pc("Telegram-чат"));
            return tu;
        }
        App.Alog.Deny(L.Pc("Чужой аккаунт пишет боту"), null, u == null ? null : UserJson(u), short_, L.Pc("Telegram-чат"));
        return null;
    }

    /// <summary>"/clip text" puts the text on the PC clipboard, "/clip" sends the PC clipboard text back.</summary>
    async Task ClipCmd(long chat, string arg, string lang, CancellationToken ct)
    {
        try
        {
            if (arg.Trim() != "")
            {
                if (arg.Length > PcClipboard.MaxChars) { await Client.SendMessage(chat, L.T("Слишком длинный текст", lang), cancellationToken: ct); return; }
                await PcClipboard.Set(arg);
                await Client.SendMessage(chat, L.T("📋 Скопировано в буфер ПК", lang), cancellationToken: ct);
                return;
            }
            var (text, _) = await PcClipboard.Get();
            if (text.Trim() == "") { await Client.SendMessage(chat, L.T("📋 В буфере ПК нет текста.\nЧтобы положить текст: /clip текст", lang), cancellationToken: ct); return; }
            // a Telegram message holds 4096 characters; the rest goes as a file
            if (text.Length <= 4000)
                await Client.SendMessage(chat, "<pre>" + Util.Esc(text) + "</pre>", ParseMode.Html, cancellationToken: ct);
            else
            {
                using var ms = new MemoryStream(System.Text.Encoding.UTF8.GetBytes(text));
                await Client.SendDocument(chat, InputFile.FromStream(ms, "clipboard.txt"), caption: L.T("📋 Буфер обмена ПК", lang), cancellationToken: ct);
            }
        }
        catch (Exception e) when (e is not OperationCanceledException)
        {
            Log.Warn("Clipboard command failed: " + e.Message);
            await Client.SendMessage(chat, L.T("⚠️ Буфер обмена ПК сейчас занят, попробуй ещё раз.", lang), cancellationToken: ct);
        }
    }

    async Task<bool> Need(TgUser u, string perm, long chat, CancellationToken ct)
    {
        if (Users.Can(u, perm)) return true;
        await Client.SendMessage(chat, L.T("⛔ Эта функция отключена для тебя на ПК.", u.Lang), cancellationToken: ct);
        return false;
    }

    async Task OnMessage(Message m, CancellationToken ct)
    {
        if (m.Chat.Type != ChatType.Private || m.From == null) return; // groups and channels are ignored
        var text = m.Text?.Trim() ?? "";
        long chat = m.Chat.Id;

        if (Users.Get(m.From.Id) == null)
        {
            await Stranger(m, text, ct);
            return;
        }
        var what = m.Text ?? (L.Pc("файл: ") + (m.Document?.FileName ?? L.Pc("медиа")));
        // clipboard text is private: only its length goes to the access log
        if (text.StartsWith("/clip", StringComparison.OrdinalIgnoreCase) && text.Length > 6) what = "/clip " + L.PcF("{0} симв.", text.Length - 6);
        var u = Known(m.From, what)!;
        var lang = u.Lang;

        if (text.StartsWith('/'))
        {
            var parts = text.Split(' ', 2, StringSplitOptions.RemoveEmptyEntries);
            var cmd = parts[0].Split('\n')[0].Split('@')[0].ToLowerInvariant();
            var arg = parts.Length > 1 ? parts[1].Trim() : "";
            switch (cmd)
            {
                case "/start":
                    await Client.SendMessage(chat, L.F("🖥 TgDesk на <b>{0}</b>. Кнопки ниже — быстрые команды.", lang, Util.Esc(Environment.MachineName)), ParseMode.Html, replyMarkup: Keyboard(u), cancellationToken: ct);
                    await AppCmd(chat, lang, ct); break;
                case "/app": await AppCmd(chat, lang, ct); break;
                case "/kill": if (await Need(u, Perm.Processes, chat, ct)) await KillCmd(chat, arg, lang, ct); break;
                case "/get": if (await Need(u, Perm.Files, chat, ct)) await GetCmd(chat, arg, lang, ct); break;
                case "/clip":
                {
                    // keep the text exactly as typed, line breaks included ("/clip\nline 1\nline 2" works too)
                    int sp = text.IndexOfAny(new[] { ' ', '\n', '\r' });
                    var clip = sp < 0 ? "" : text[(sp + 1)..];
                    if (!await Need(u, Perm.Input, chat, ct)) break;
                    if (clip.Trim() == "" && !await Need(u, Perm.Screen, chat, ct)) break;
                    await ClipCmd(chat, clip, lang, ct);
                    break;
                }
            }
            return;
        }

        switch (KbAction(text))
        {
            case "📊 Статус": await StatusCmd(chat, lang, ct); return;
            case "📸 Скрин": if (await Need(u, Perm.Screen, chat, ct)) await ScreenCmd(chat, lang, ct); return;
            case "⚙️ Процессы": if (await Need(u, Perm.Processes, chat, ct)) await ProcCmd(chat, lang, ct); return;
            case "📁 Файлы": if (await Need(u, Perm.Files, chat, ct)) await FilesHelp(chat, lang, ct); return;
            case "🔊 Медиа": if (await Need(u, Perm.Media, chat, ct)) await MediaCmd(chat, lang, ct); return;
            case "🔌 Питание": if (await Need(u, Perm.Power, chat, ct)) await PowerCmd(chat, lang, ct); return;
        }

        if (m.Photo != null || m.Video != null || m.Animation != null || m.VideoNote != null || m.Audio != null || m.Voice != null || m.Document != null)
            if (await Need(u, Perm.Files, chat, ct)) await SaveIncoming(m, u, ct);
    }

    /// <summary>Only a pairing code is accepted from strangers: "/start pair_123456", "/pair 123456".</summary>
    async Task Stranger(Message m, string text, CancellationToken ct)
    {
        var u = m.From!;
        string? code = null;
        var parts = text.Split(' ', 2, StringSplitOptions.RemoveEmptyEntries);
        if (parts.Length == 2)
        {
            var cmd = parts[0].Split('@')[0].ToLowerInvariant();
            var arg = parts[1].Trim();
            if (cmd == "/start" && arg.StartsWith("pair_")) code = arg[5..];
            else if (cmd is "/pair" or "/start") code = arg;
        }
        if (code == null || code.Length > 12 || !code.All(char.IsDigit))
        {
            Known(u, text.Length > 0 ? text : L.Pc("медиа")); // logs the denied attempt, no answer at all
            return;
        }
        var lang = Cfg.TgLang(u.LanguageCode);
        var meta = new ReqMeta("", "", "", null);
        switch (Pairing.TryClaim(code, u.Id, DisplayName(u), u.Username, u.LanguageCode, out var added))
        {
            case Pairing.Result.Ok:
                App.Alog.Add("auth", L.Pc("Новый пользователь подключён"), UserJson(u), null, added!.IsOwner ? L.Pc("владелец") : L.Pc("пользователь"), device: L.Pc("Telegram-чат"));
                break; // the welcome message is sent by OnUserAdded
            case Pairing.Result.Wrong:
                App.Alog.Deny(L.Pc("Неверный код привязки"), meta, UserJson(u), null, L.Pc("Telegram-чат"));
                await Client.SendMessage(m.Chat.Id, L.T("❌ Неверный код. Проверь цифры в приложении TgDesk на ПК.", lang), cancellationToken: ct);
                break;
            case Pairing.Result.Locked:
                App.Alog.Deny(L.Pc("Привязка отменена: слишком много неверных кодов"), meta, UserJson(u), null, L.Pc("Telegram-чат"));
                await Client.SendMessage(m.Chat.Id, L.T("⛔ Слишком много неверных попыток. Создай новый код в приложении TgDesk на ПК.", lang), cancellationToken: ct);
                break;
            default:
                App.Alog.Deny(L.Pc("Код привязки без активной привязки"), meta, UserJson(u), null, L.Pc("Telegram-чат"));
                await Client.SendMessage(m.Chat.Id, L.T("⌛ Код недействителен или истёк. Создай новый в приложении TgDesk на ПК: «Пользователи» → «Добавить».", lang), cancellationToken: ct);
                break;
        }
    }

    async Task AppCmd(long chat, string lang, CancellationToken ct)
    {
        var url = App.Tunnel.Url;
        if (url == null) { await Client.SendMessage(chat, L.T("⏳ Приложение ещё поднимается, попробуй через минуту.", lang), cancellationToken: ct); return; }
        await Client.SendMessage(chat, L.T("Панель управления:", lang), replyMarkup: AppButton(url, lang), cancellationToken: ct);
    }

    async Task StatusCmd(long chat, string lang, CancellationToken ct)
    {
        var d = await Sys.Status();
        var up = TimeSpan.FromSeconds(d["uptime"]!.GetValue<long>());
        var sb = new StringBuilder();
        sb.AppendLine($"🖥 <b>{Util.Esc(d["host"]!.ToString())}</b> — {Util.Esc(d["os"]!.ToString())}");
        sb.AppendLine($"🧠 CPU: {d["cpu"]!.GetValue<double>():0}%");
        var ram = d["ram"]!;
        sb.AppendLine($"💾 RAM: {ram["percent"]!.GetValue<double>():0}% ({Util.HumanBytes(ram["used"]!.GetValue<double>())} / {Util.HumanBytes(ram["total"]!.GetValue<double>())})");
        sb.AppendLine(L.T("⏱ Аптайм: ", lang) + (up.Days > 0 ? up.Days + L.T(" д. ", lang) : "") + $"{up.Hours}:{up.Minutes:00}:{up.Seconds:00}");
        sb.AppendLine().AppendLine(L.T("💿 Диски:", lang));
        foreach (var x in d["disks"]!.AsArray())
            sb.AppendLine("  " + L.F("{0} — {1}% занято, {2} свободно", lang, Util.Esc(x!["device"]!.ToString()), x["percent"]!.GetValue<double>().ToString("0"), Util.HumanBytes(x["free"]!.GetValue<double>())));
        await Client.SendMessage(chat, sb.ToString(), ParseMode.Html, cancellationToken: ct);
    }

    async Task ScreenCmd(long chat, string lang, CancellationToken ct)
    {
        try
        {
            var data = await Task.Run(() => Grabber.Snapshot(true), ct);
            using var ms = new MemoryStream(data);
            await Client.SendPhoto(chat, InputFile.FromStream(ms, "screen.jpg"), cancellationToken: ct);
        }
        catch (ApiRequestException e) { await Client.SendMessage(chat, L.T("⚠️ Не удалось отправить: ", lang) + e.Message, cancellationToken: ct); }
        catch (Exception e) when (e is not OperationCanceledException) { await Client.SendMessage(chat, L.T("⚠️ Экран недоступен — ПК заблокирован или спит.", lang), cancellationToken: ct); }
    }

    async Task ProcCmd(long chat, string lang, CancellationToken ct)
    {
        var rows = await Sys.Processes(12);
        var body = string.Join("\n", rows.Select(r => $"{r!["pid"],6}  {Trunc(r["name"]!.ToString(), 22),-22} cpu {r["cpu"]!.GetValue<double>(),4:0}%  ram {r["mem"]!.GetValue<double>(),4:0}%"));
        await Client.SendMessage(chat, L.T("⚙️ Топ процессов по CPU:", lang) + $"\n<pre>{Util.Esc(body)}</pre>\n" + L.T("Завершить процесс: /kill &lt;PID&gt;", lang), ParseMode.Html, cancellationToken: ct);
    }

    static string Trunc(string s, int n) => s.Length <= n ? s : s[..n];

    async Task KillCmd(long chat, string arg, string lang, CancellationToken ct)
    {
        if (arg == "") { await Client.SendMessage(chat, L.T("Использование: /kill <PID>", lang), cancellationToken: ct); return; }
        if (!int.TryParse(arg.Split(' ')[0], out var pid)) { await Client.SendMessage(chat, L.T("PID должен быть числом.", lang), cancellationToken: ct); return; }
        try { await Client.SendMessage(chat, "✅ " + await Task.Run(() => Sys.Kill(pid, lang)), cancellationToken: ct); }
        catch (ArgumentException e) when (e.Message.Contains("not running")) { await Client.SendMessage(chat, L.T("Нет такого процесса.", lang), cancellationToken: ct); }
        catch (ArgumentException e) { await Client.SendMessage(chat, e.Message, cancellationToken: ct); }
        catch (Exception) { await Client.SendMessage(chat, L.T("⚠️ Недостаточно прав.", lang), cancellationToken: ct); }
    }

    async Task FilesHelp(long chat, string lang, CancellationToken ct) =>
        await Client.SendMessage(chat, L.F("📁 <b>Файлы</b>\n• Пришли фото, видео, музыку или голосовое — разложу по папкам Изображения/Видео/Музыка → TgDesk\n  (для оригинального качества фото отправляй «как файл»)\n• Остальные файлы — в <code>{0}</code>\n• Скачать с ПК: <code>/get &lt;полный путь&gt;</code> (до ~50 МБ)\n• Удобнее — через приложение, вкладка «Файлы».", lang, Util.Esc(Cfg.DownloadDir)),
            ParseMode.Html, cancellationToken: ct);

    async Task GetCmd(long chat, string arg, string lang, CancellationToken ct)
    {
        if (arg == "") { await Client.SendMessage(chat, L.T("Использование: /get <путь к файлу>", lang), cancellationToken: ct); return; }
        var path = arg.Trim('"');
        if (!System.IO.File.Exists(path)) { await Client.SendMessage(chat, L.T("Файл не найден.", lang), cancellationToken: ct); return; }
        long size = new FileInfo(path).Length;
        if (size > MaxSend) { await Client.SendMessage(chat, L.F("Файл {0} — больше лимита Telegram (~50 МБ)", lang, Util.HumanBytes(size)), cancellationToken: ct); return; }
        try { await SendFileToChat(chat, path, lang); }
        catch (ArgumentException e) { await Client.SendMessage(chat, "⚠️ " + e.Message, cancellationToken: ct); }
    }

    string Remember(string path)
    {
        var key = Convert.ToHexString(RandomNumberGenerator.GetBytes(4)).ToLowerInvariant();
        lock (_pathOrder)
        {
            _paths[key] = path; _pathOrder.Enqueue(key);
            while (_pathOrder.Count > 500) _paths.TryRemove(_pathOrder.Dequeue(), out _);
        }
        return key;
    }

    async Task SaveIncoming(Message m, TgUser u, CancellationToken ct)
    {
        var lang = u.Lang;
        var stamp = DateTime.Now.ToString("yyyy-MM-dd_HH-mm-ss");
        string fileId, name, kind; long? fsize;
        if (m.Photo is { Length: > 0 } ph) { var p = ph[^1]; (fileId, name, kind, fsize) = (p.FileId, $"photo_{stamp}.jpg", "image", p.FileSize); }
        else if (m.Video is { } v) (fileId, name, kind, fsize) = (v.FileId, v.FileName ?? $"video_{stamp}.mp4", "video", v.FileSize);
        else if (m.Animation is { } an) (fileId, name, kind, fsize) = (an.FileId, an.FileName ?? $"gif_{stamp}.mp4", "video", an.FileSize);
        else if (m.VideoNote is { } vn) (fileId, name, kind, fsize) = (vn.FileId, $"circle_{stamp}.mp4", "video", vn.FileSize);
        else if (m.Audio is { } au)
        {
            var guess = string.Join(" - ", new[] { au.Performer, au.Title }.Where(x => !string.IsNullOrEmpty(x)));
            (fileId, name, kind, fsize) = (au.FileId, au.FileName ?? (guess != "" ? guess + ".mp3" : $"audio_{stamp}.mp3"), "audio", au.FileSize);
        }
        else if (m.Voice is { } vo) (fileId, name, kind, fsize) = (vo.FileId, $"voice_{stamp}.ogg", "audio", vo.FileSize);
        else if (m.Document is { } doc) { name = doc.FileName ?? $"file_{stamp}"; (fileId, kind, fsize) = (doc.FileId, Util.Kind(name), doc.FileSize); }
        else return;

        if (fsize > TgDownloadLimit)
        {
            await Client.SendMessage(m.Chat.Id, L.F("⚠️ Файл {0} — Telegram не даёт ботам скачивать файлы больше 20 МБ.\nЗагрузи его через приложение: «Файлы» → открой папку → «⬆ Загрузить».", lang, Util.HumanBytes(fsize.Value)), cancellationToken: ct);
            return;
        }
        var folder = kind switch { "image" => Cfg.PhotoDir, "audio" => Cfg.MusicDir, "video" => Cfg.VideoDir, _ => Cfg.DownloadDir };
        string dest;
        try
        {
            Directory.CreateDirectory(folder);
            dest = Util.UniquePath(folder, name);
            await using var fs = System.IO.File.Create(dest);
            await Client.GetInfoAndDownloadFile(fileId, fs, ct);
        }
        catch (Exception e) when (e is not OperationCanceledException)
        {
            await Client.SendMessage(m.Chat.Id, L.T("⚠️ Не удалось сохранить: ", lang) + e.Message, cancellationToken: ct);
            return;
        }
        App.Alog.Add("files", L.Pc("Загрузил через чат"), UserJson(m.From), null, dest, device: L.Pc("Telegram-чат"));
        var key = Remember(dest);
        var kb = new InlineKeyboardMarkup(new[] { InlineKeyboardButton.WithCallbackData(L.T("▶ Открыть на ПК", lang), "o:" + key), InlineKeyboardButton.WithCallbackData(L.T("📂 Показать в папке", lang), "r:" + key) });
        await Client.SendMessage(m.Chat.Id, L.T("✅ Сохранено", lang) + $"\n<code>{Util.Esc(dest)}</code>", ParseMode.Html, replyMarkup: kb, cancellationToken: ct);
    }

    async Task MediaCmd(long chat, string lang, CancellationToken ct)
    {
        var kb = new InlineKeyboardMarkup(new[]
        {
            new[] { InlineKeyboardButton.WithCallbackData(L.T("🔉 Тише", lang), "m:voldown"), InlineKeyboardButton.WithCallbackData("🔇 Mute", "m:mute"), InlineKeyboardButton.WithCallbackData(L.T("🔊 Громче", lang), "m:volup") },
            new[] { InlineKeyboardButton.WithCallbackData("⏮", "m:prev"), InlineKeyboardButton.WithCallbackData("⏯", "m:playpause"), InlineKeyboardButton.WithCallbackData("⏭", "m:next") },
        });
        await Client.SendMessage(chat, L.T("🎚 Звук и медиа:", lang), replyMarkup: kb, cancellationToken: ct);
    }

    async Task PowerCmd(long chat, string lang, CancellationToken ct)
    {
        var kb = new InlineKeyboardMarkup(new[]
        {
            new[] { InlineKeyboardButton.WithCallbackData(L.T("🔒 Блокировка", lang), "p:lock") },
            new[] { InlineKeyboardButton.WithCallbackData(L.T("🌙 Сон", lang), "p:sleep") },
            new[] { InlineKeyboardButton.WithCallbackData(L.T("🔄 Перезагрузка", lang), "p:restart") },
            new[] { InlineKeyboardButton.WithCallbackData(L.T("⛔ Выключение", lang), "p:shutdown") },
        });
        await Client.SendMessage(chat, L.T("🔌 Питание — выбери действие:", lang), replyMarkup: kb, cancellationToken: ct);
    }

    static readonly Dictionary<string, string> PowerLabels = new() { ["sleep"] = "усыпить", ["restart"] = "перезагрузить", ["shutdown"] = "выключить" };

    async Task OnCallback(CallbackQuery q, CancellationToken ct)
    {
        var u = q.Message?.Chat.Type == ChatType.Private ? Known(q.From, "button " + q.Data) : null;
        if (u == null)
        {
            await Client.AnswerCallbackQuery(q.Id, cancellationToken: ct);
            return;
        }
        var lang = u.Lang;
        var data = q.Data ?? ""; var i = data.IndexOf(':');
        if (i < 0) { await Client.AnswerCallbackQuery(q.Id, cancellationToken: ct); return; }
        string kind = data[..i], arg = data[(i + 1)..];
        var perm = kind switch { "m" => Perm.Media, "p" or "pc" => Perm.Power, _ => Perm.Files };
        if (!Users.Can(u, perm)) { await Client.AnswerCallbackQuery(q.Id, L.T("⛔ Эта функция отключена для тебя на ПК.", lang), true, cancellationToken: ct); return; }
        var chat = q.Message!.Chat.Id; var mid = q.Message.MessageId;
        switch (kind)
        {
            case "m":
                if (Native.MediaActions.Contains(arg)) { Native.Media(arg); await Client.AnswerCallbackQuery(q.Id, L.T("Готово", lang), cancellationToken: ct); }
                else await Client.AnswerCallbackQuery(q.Id, cancellationToken: ct);
                break;
            case "p":
                await Client.AnswerCallbackQuery(q.Id, cancellationToken: ct);
                if (arg == "lock") { Sys.Power("lock"); App.Alog.Add("power", L.Pc("Заблокировал ПК"), UserJson(q.From), device: L.Pc("Telegram-чат")); await Client.EditMessageText(chat, mid, L.T("🔒 ПК заблокирован.", lang), cancellationToken: ct); break; }
                if (!PowerLabels.ContainsKey(arg)) break;
                var label = L.T(PowerLabels[arg], lang);
                await Client.EditMessageText(chat, mid, L.F("Точно {0} ПК?", lang, label), replyMarkup: new InlineKeyboardMarkup(new[]
                {
                    InlineKeyboardButton.WithCallbackData(L.F("✅ Да, {0}", lang, label), "pc:" + arg), InlineKeyboardButton.WithCallbackData(L.T("❌ Отмена", lang), "pc:cancel"),
                }), cancellationToken: ct);
                break;
            case "pc":
                await Client.AnswerCallbackQuery(q.Id, cancellationToken: ct);
                if (arg == "cancel") { await Client.EditMessageText(chat, mid, L.T("Отменено.", lang), cancellationToken: ct); break; }
                var texts = new Dictionary<string, string> { ["sleep"] = "🌙 Ухожу в сон…", ["restart"] = "🔄 Перезагрузка через 5 секунд…", ["shutdown"] = "⛔ Выключение через 5 секунд…" };
                if (!texts.ContainsKey(arg)) break;
                var titles = new Dictionary<string, string> { ["sleep"] = "Отправил ПК в сон", ["restart"] = "Перезагрузка ПК", ["shutdown"] = "Выключение ПК" };
                App.Alog.Add("power", L.Pc(titles[arg]), UserJson(q.From), device: L.Pc("Telegram-чат"));
                await Client.EditMessageText(chat, mid, L.T(texts[arg], lang), cancellationToken: ct);
                Sys.Power(arg);
                break;
            case "o":
            case "r":
                if (!_paths.TryGetValue(arg, out var path) || (!System.IO.File.Exists(path) && !Directory.Exists(path)))
                {
                    await Client.AnswerCallbackQuery(q.Id, L.T("Файл не найден (возможно, перемещён)", lang), true, cancellationToken: ct);
                    break;
                }
                if (kind == "o") { Sys.OpenOnPc(path); await Client.AnswerCallbackQuery(q.Id, L.T("Открыто на ПК", lang), cancellationToken: ct); }
                else { Sys.RevealOnPc(path); await Client.AnswerCallbackQuery(q.Id, L.T("Папка открыта на ПК", lang), cancellationToken: ct); }
                App.Alog.Add("files", kind == "o" ? L.Pc("Открыл на ПК") : L.Pc("Показал в папке на ПК"), UserJson(q.From), null, path, device: L.Pc("Telegram-чат"));
                break;
        }
    }
}
