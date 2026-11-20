using System.Collections.Concurrent;
using System.Diagnostics;
using System.Net.WebSockets;
using System.Text;
using System.Text.Json.Nodes;
using Microsoft.AspNetCore.Http;

namespace TgDesk;

/// <summary>Screen streaming and remote input over WebSocket (protocol of webapp/index.html).</summary>
public static class Remote
{
    // width, JPEG quality, fps
    static readonly Dictionary<string, (int w, int q, int fps)> Quality = new()
    {
        ["eco"] = (960, 45, 6), ["norm"] = (1280, 60, 10), ["hd"] = (1920, 75, 15),
    };
    static readonly string[] AutoLevels = { "eco", "norm", "hd" };

    public static readonly ConcurrentDictionary<string, (WebSocket ws, CancellationTokenSource cts)> Live = new();

    static readonly ConcurrentDictionary<string, long> SidUid = new();

    /// <summary>Disconnects viewers who lost access (user removed, permission or feature turned off).</summary>
    public static void KickUnauthorized()
    {
        foreach (var (sid, uid) in SidUid)
            if (!Users.Can(Users.Get(uid), Perm.Screen)) Kick(sid);
    }

    public static bool Kick(string sid)
    {
        if (!Live.TryGetValue(sid, out var s)) return false;
        try { s.cts.Cancel(); s.ws.Abort(); } catch { }
        return true;
    }

    public static string DefaultQuality() => Cfg.Quality; // "auto" is the best choice for a phone on mobile data

    static async Task<string?> ReceiveText(WebSocket ws, CancellationToken ct)
    {
        var buf = new byte[16 * 1024];
        using var ms = new MemoryStream();
        while (true)
        {
            var r = await ws.ReceiveAsync(buf, ct);
            if (r.MessageType == WebSocketMessageType.Close) return null;
            if (ms.Length + r.Count > 1 << 20) throw new InvalidDataException("message too big");
            ms.Write(buf, 0, r.Count);
            if (r.EndOfMessage)
            {
                if (r.MessageType == WebSocketMessageType.Text) return Encoding.UTF8.GetString(ms.GetBuffer(), 0, (int)ms.Length);
                ms.SetLength(0); // binary messages are ignored
            }
        }
    }

    public static async Task Handle(HttpContext c)
    {
        if (!c.WebSockets.IsWebSocketRequest) { c.Response.StatusCode = 400; return; }
        using var ws = await c.WebSockets.AcceptWebSocketAsync();
        var sendLock = new SemaphoreSlim(1, 1);
        using var cts = CancellationTokenSource.CreateLinkedTokenSource(c.RequestAborted);
        var ct = cts.Token;

        async Task SendJson(JsonObject o)
        {
            var b = Encoding.UTF8.GetBytes(o.ToJsonString(Json.Relaxed));
            await sendLock.WaitAsync(ct);
            try { if (ws.State == WebSocketState.Open) await ws.SendAsync(b, WebSocketMessageType.Text, true, ct); }
            finally { sendLock.Release(); }
        }

        JsonObject hello = new();
        try
        {
            using var t = CancellationTokenSource.CreateLinkedTokenSource(ct);
            t.CancelAfter(15000);
            var first = await ReceiveText(ws, t.Token);
            hello = JsonNode.Parse(first ?? "{}") as JsonObject ?? new();
        }
        catch { }

        var meta = ReqMeta.From(c.Request) with { Client = (hello["client"]?.ToString() ?? "").Length > 40 ? hello["client"]!.ToString()[..40] : hello["client"]?.ToString() ?? "" };
        var init = hello["initData"]?.ToString();
        var user = TgAuth.Validate(init);
        var tu = user == null ? null : Users.Get(TgAuth.UserId(user));
        if (user == null || !Users.Can(tu, Perm.Screen))
        {
            var (u, reason) = App.DenyReason(init, Perm.Screen);
            App.Alog.Deny(reason, meta, u, L.Pc("Трансляция экрана"));
            Security.Fail(meta.Ip);
            try
            {
                await SendJson(new JsonObject { ["type"] = "error", ["error"] = "unauthorized" });
                await ws.CloseAsync((WebSocketCloseStatus)4401, "unauthorized", CancellationToken.None);
            }
            catch { }
            return;
        }

        var lang = Cfg.TgLang(user["language_code"]?.ToString());
        bool canInput = Users.Can(tu, Perm.Input);
        bool inputDeniedSent = false;
        var mons = Native.Monitors();
        int mon = mons.Count > 2 ? 1 : (mons.Count > 1 ? 1 : 0);
        string q = hello["quality"]?.ToString() is string hq && (Quality.ContainsKey(hq) || hq == "auto") ? hq : DefaultQuality();
        bool on = true;
        int autoLevel = 1;
        var ack = new SemaphoreSlim(0, 1);
        void SetAck() { if (ack.CurrentCount == 0) { try { ack.Release(); } catch (SemaphoreFullException) { } } }
        SetAck();

        var sid = Guid.NewGuid().ToString("N")[..12];
        var started = DateTime.Now;
        long frames = 0, bytes = 0, inputs = 0;
        var evStart = App.Alog.Add("stream", L.Pc("Начал трансляцию"), user, meta, L.PcF("монитор {0}, качество {1}", mon, q), ev: "start");
        var who = string.Join(" · ", new[] { TgAuth.FmtUser(user).Split(" (@")[0], evStart["device"]?.ToString(), meta.Ip }.Where(x => !string.IsNullOrEmpty(x)));
        App.Alog.SessionStart(sid, who, meta.Ip);
        Live[sid] = (ws, cts);
        SidUid[sid] = TgAuth.UserId(user);
        App.Overlay?.Start(sid, who); // the badge is always shown — it cannot be disabled

        await SendJson(new JsonObject
        {
            ["type"] = "hello",
            ["host"] = Environment.MachineName,
            ["mon"] = mon,
            ["monitors"] = new JsonArray(mons.Where(m => m.Index > 0 || mons.Count > 2).Select(m => (JsonNode)new JsonObject
            {
                ["index"] = m.Index, ["width"] = m.Width, ["height"] = m.Height,
                ["label"] = m.Index == 0 ? L.T("Все экраны", lang) : L.F("Экран {0}", lang, m.Index),
            }).ToArray()),
            ["input"] = canInput,
        });

        var grabber = new Grabber();
        DateTime sentAt = default;
        double rtt = 0; int good = 0;

        var sender = Task.Run(async () =>
        {
            try
            {
                while (!ct.IsCancellationRequested && ws.State == WebSocketState.Open)
                {
                    await ack.WaitAsync(4000, ct);
                    if (!on) continue;
                    // auto quality: adapt to the link latency (tunnel + mobile network)
                    string eff = q == "auto" ? AutoLevels[autoLevel] : q;
                    var (w, qual, fps) = Quality[eff];
                    var sw = Stopwatch.StartNew();
                    byte[]? frame;
                    try { frame = await Task.Run(() => grabber.Capture(mon, w, qual), ct); }
                    catch (OperationCanceledException) { break; }
                    catch (Exception ex)
                    {
                        Log.Warn("Frame capture failed: " + ex.Message);
                        await SendJson(new JsonObject { ["type"] = "error", ["error"] = L.T("Экран недоступен (заблокирован?)", lang) });
                        await Task.Delay(1500, ct);
                        SetAck();
                        continue;
                    }
                    if (frame == null) { await Task.Delay(70, ct); SetAck(); continue; } // picture unchanged
                    await sendLock.WaitAsync(ct);
                    try { await ws.SendAsync(frame, WebSocketMessageType.Binary, true, ct); }
                    finally { sendLock.Release(); }
                    sentAt = DateTime.UtcNow;
                    frames++; bytes += frame.Length;
                    var rest = 1000.0 / fps - sw.Elapsed.TotalMilliseconds;
                    if (rest > 0) await Task.Delay((int)rest, ct);
                }
            }
            catch (OperationCanceledException) { }
            catch (WebSocketException) { }
            catch (Exception ex) { Log.Error("Frame sender failed", ex); }
        });

        try
        {
            while (!ct.IsCancellationRequested && ws.State == WebSocketState.Open)
            {
                var text = await ReceiveText(ws, ct);
                if (text == null) break;
                JsonObject? ev;
                try { ev = JsonNode.Parse(text) as JsonObject; } catch { continue; }
                if (ev == null) continue;
                var t = ev["type"]?.ToString();
                if (t == "ack")
                {
                    if (sentAt != default && q == "auto")
                    {
                        var ms = (DateTime.UtcNow - sentAt).TotalMilliseconds;
                        rtt = rtt == 0 ? ms : rtt * 0.7 + ms * 0.3;
                        if (rtt > 450 && autoLevel > 0) { autoLevel--; good = 0; rtt = 0; }
                        else if (rtt < 140 && ++good > 25 && autoLevel < 2) { autoLevel++; good = 0; rtt = 0; }
                    }
                    SetAck();
                }
                else if (t == "config")
                {
                    if (ev["mon"] != null && int.TryParse(ev["mon"]!.ToString(), out var m) && m >= 0 && m < mons.Count) { mon = m; mons = Native.Monitors(); grabber.ForceNext(); }
                    if (ev["quality"]?.ToString() is string nq && (Quality.ContainsKey(nq) || nq == "auto")) { q = nq; grabber.ForceNext(); }
                    if (ev["on"] != null) on = ev["on"]!.GetValue<bool>();
                    grabber.ForceNext();
                    SetAck();
                }
                else
                {
                    // permissions may change while streaming: check on every event
                    canInput = Users.Can(Users.Get(TgAuth.UserId(user)), Perm.Input);
                    if (!canInput)
                    {
                        if (!inputDeniedSent) { inputDeniedSent = true; await SendJson(new JsonObject { ["type"] = "error", ["error"] = L.T("Нет прав на управление мышью и клавиатурой", lang) }); }
                        continue;
                    }
                    inputs++;
                    try { Native.HandleInput(ev, mons[Math.Min(mon, mons.Count - 1)], mons[0]); }
                    catch (Exception ex) { await SendJson(new JsonObject { ["type"] = "error", ["error"] = L.T("Ввод: ", lang) + ex.Message }); }
                }
            }
        }
        catch (OperationCanceledException) { }
        catch (WebSocketException) { }
        catch (Exception ex) { Log.Warn("Stream: " + ex.Message); }
        finally
        {
            cts.Cancel();
            try { await sender; } catch { }
            try
            {
                if (ws.State is WebSocketState.CloseReceived or WebSocketState.Open)
                    await ws.CloseOutputAsync(WebSocketCloseStatus.NormalClosure, "bye", CancellationToken.None);
            }
            catch { }
            grabber.Dispose();
            Live.TryRemove(sid, out _);
            SidUid.TryRemove(sid, out _);
            App.Alog.SessionEnd(sid);
            App.Overlay?.End(sid);
            var secs = (int)(DateTime.Now - started).TotalSeconds;
            App.Alog.Add("stream", L.Pc("Завершил трансляцию"), user, meta,
                L.PcF("длительность {0}, кадров {1}, трафик {2}, действий мышью/клавиатурой {3}", $"{secs / 60}:{secs % 60:00}", frames, Util.HumanBytes(bytes), inputs), ev: "end");
        }
    }
}
