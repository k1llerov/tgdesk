using System.Collections.Concurrent;
using System.Net;
using System.Net.Sockets;
using System.Text;
using System.Text.Json;
using System.Text.Json.Nodes;
using System.Text.RegularExpressions;
using Microsoft.AspNetCore.Http;

namespace TgDesk;

public record ReqMeta(string Ip, string Ua, string Client, string? Country)
{
    public static ReqMeta From(HttpRequest r)
    {
        var h = r.Headers;
        string ip = h["Cf-Connecting-Ip"].ToString();
        if (ip == "") ip = h["X-Forwarded-For"].ToString().Split(',')[0].Trim();
        if (ip == "") ip = r.HttpContext.Connection.RemoteIpAddress?.MapToIPv4().ToString() ?? "";
        var c = h["Cf-Ipcountry"].ToString();
        return new ReqMeta(ip, h["User-Agent"].ToString(), h["X-Client"].ToString(), c == "" ? null : c);
    }
}

/// <summary>Access log: who, when, from where (IP, place) and from which device (access_log.jsonl).</summary>
public class AccessLog
{
    const long MaxFile = 5 * 1024 * 1024;
    readonly string _path;
    readonly object _l = new();
    readonly LinkedList<JsonObject> _items = new();
    int _nextId = 1;
    readonly Dictionary<string, (string? place, string? isp)> _geo = new();
    readonly Dictionary<string, List<JsonObject>> _pending = new();
    readonly HashSet<string> _seenIps = new();
    readonly Dictionary<string, JsonObject> _active = new();
    readonly Dictionary<string, DateTime> _denySeen = new();
    static readonly HttpClient Http = new(new HttpClientHandler { UseProxy = true }) { Timeout = TimeSpan.FromSeconds(6) };

    static readonly Dictionary<string, string> TgClients = new()
    {
        ["android"] = "Telegram Android", ["android_x"] = "Telegram X", ["ios"] = "Telegram iOS", ["tdesktop"] = "Telegram Desktop",
        ["macos"] = "Telegram macOS", ["weba"] = "Telegram Web A", ["webk"] = "Telegram Web K", ["web"] = "Telegram Web", ["unigram"] = "Unigram",
    };

    public AccessLog(string path)
    {
        _path = path;
        if (!File.Exists(path)) return;
        try
        {
            foreach (var line in File.ReadAllLines(path, Encoding.UTF8))
            {
                try
                {
                    if (JsonNode.Parse(line) is not JsonObject e) continue;
                    _items.AddLast(e);
                    if (_items.Count > 3000) _items.RemoveFirst();
                    _nextId = Math.Max(_nextId, (e["id"]?.GetValue<int>() ?? 0) + 1);
                    var ip = e["ip"]?.ToString();
                    if (!string.IsNullOrEmpty(ip))
                    {
                        _seenIps.Add(ip);
                        if (e["place"] != null && !IsLocal(ip)) _geo[ip] = (e["place"]?.ToString(), e["isp"]?.ToString());
                    }
                }
                catch { }
            }
        }
        catch { }
    }

    public static bool IsLocal(string ip)
    {
        if (!IPAddress.TryParse(ip, out var a)) return false;
        if (IPAddress.IsLoopback(a)) return true;
        if (a.AddressFamily == AddressFamily.InterNetworkV6) return a.IsIPv6LinkLocal || a.IsIPv6SiteLocal || (a.GetAddressBytes()[0] & 0xFE) == 0xFC;
        var b = a.GetAddressBytes();
        return b[0] == 10 || (b[0] == 172 && b[1] >= 16 && b[1] <= 31) || (b[0] == 192 && b[1] == 168) || (b[0] == 169 && b[1] == 254);
    }

    public static string ParseDevice(string ua, string client = "")
    {
        var parts = new List<string>();
        var m = Regex.Match(ua ?? "", @"Android ([\d.]+);\s*([^;)]+?)(?:\s+Build/|;|\))");
        if (m.Success)
        {
            var model = m.Groups[2].Value.Trim();
            if (model != "" && model is not ("K" or "wv" or "Linux")) parts.Add(model);
            parts.Add("Android " + m.Groups[1].Value);
        }
        else if ((ua ?? "").Contains("iPhone") || (ua ?? "").Contains("iPad"))
        {
            parts.Add(ua!.Contains("iPad") ? "iPad" : "iPhone");
            var v = Regex.Match(ua, @"OS (\d+)[_.](\d+)");
            parts.Add(v.Success ? $"iOS {v.Groups[1].Value}.{v.Groups[2].Value}" : "iOS");
        }
        else if ((ua ?? "").Contains("Windows NT")) parts.Add("Windows");
        else if ((ua ?? "").Contains("Macintosh") || (ua ?? "").Contains("Mac OS X")) parts.Add("macOS");
        else if ((ua ?? "").Contains("Linux")) parts.Add("Linux");
        if (!string.IsNullOrEmpty(client))
        {
            var plat = client.Split(' ')[0];
            var name = TgClients.TryGetValue(plat.ToLowerInvariant(), out var n) ? n : (plat != "" && plat != "browser" ? "Telegram " + plat : "");
            if (name != "") parts.Add(name);
        }
        if (parts.Count > 0) return string.Join(" · ", parts);
        return string.IsNullOrEmpty(ua) ? L.Pc("неизвестно") : ua[..Math.Min(60, ua.Length)];
    }

    void Write(JsonObject e)
    {
        try
        {
            lock (_l)
            {
                if (File.Exists(_path) && new FileInfo(_path).Length > MaxFile) File.Move(_path, _path + ".1", true);
                File.AppendAllText(_path, e.ToJsonString(Json.Relaxed) + "\n", new UTF8Encoding(false));
            }
        }
        catch (Exception ex) { Log.Error("Cannot write the access log", ex, "access"); }
    }

    public JsonObject Add(string cat, string action, JsonObject? user = null, ReqMeta? meta = null, string? detail = null, string level = "info", string? device = null, string? ev = null)
    {
        string? ip = string.IsNullOrEmpty(meta?.Ip) ? null : meta!.Ip;
        var e = new JsonObject
        {
            ["id"] = 0,
            ["ts"] = DateTimeOffset.UtcNow.ToUnixTimeMilliseconds() / 1000.0,
            ["time"] = DateTime.Now.ToString("yyyy-MM-dd HH:mm:ss"),
            ["cat"] = cat,
            ["action"] = action,
            ["detail"] = detail,
            ["level"] = level,
            ["user"] = user == null ? null : TgAuth.FmtUser(user),
            ["uid"] = user?["id"]?.DeepClone(),
            ["ip"] = ip,
            ["device"] = device ?? ((meta != null && (meta.Ua != "" || meta.Client != "")) ? ParseDevice(meta.Ua, meta.Client) : null),
            ["place"] = null,
            ["isp"] = null,
            ["new_ip"] = false,
            ["ev"] = ev,
        };
        bool lookup = false;
        lock (_l)
        {
            e["id"] = _nextId++;
            if (ip != null)
            {
                if (IsLocal(ip)) e["place"] = L.Pc("Этот ПК / локальная сеть");
                else
                {
                    if (!_seenIps.Contains(ip)) e["new_ip"] = true;
                    if (_geo.TryGetValue(ip, out var g)) { e["place"] = g.place; e["isp"] = g.isp; }
                    else lookup = true;
                }
                _seenIps.Add(ip);
            }
            if (e["place"] == null && meta?.Country != null) e["place"] = meta.Country;
            _items.AddLast(e);
            if (_items.Count > 3000) _items.RemoveFirst();
            if (lookup)
            {
                if (_pending.TryGetValue(ip!, out var list)) list.Add(e);
                else { _pending[ip!] = new List<JsonObject> { e }; _ = Task.Run(() => Resolve(ip!)); }
            }
        }
        Log.Info($"[{cat}] {action} {detail} | {e["uid"]?.ToString() ?? "-"} | {ip ?? "-"} | {e["device"] ?? "-"}", "access");
        if (!lookup) Write(e);
        return e;
    }

    async Task Resolve(string ip)
    {
        (string?, string?)? info = null;
        if (Cfg.GeoIp) try
        {
            var d = JsonNode.Parse(await Http.GetStringAsync($"https://ipwho.is/{Uri.EscapeDataString(ip)}?lang={Cfg.PcLang}"));
            if (d?["success"]?.GetValue<bool>() == true)
            {
                var place = string.Join(", ", new[] { d["city"]?.ToString(), d["region"]?.ToString(), d["country_code"]?.ToString() }.Where(x => !string.IsNullOrEmpty(x)));
                var c = d["connection"];
                info = (place == "" ? null : place, c?["isp"]?.ToString() ?? c?["org"]?.ToString());
            }
        }
        catch { }
        List<JsonObject> list;
        lock (_l)
        {
            if (info != null) _geo[ip] = info.Value;
            list = _pending.Remove(ip, out var l) ? l : new();
            foreach (var e in list)
            {
                if (info != null) { e["place"] = info.Value.Item1 ?? e["place"]?.ToString(); e["isp"] = info.Value.Item2; }
            }
        }
        foreach (var e in list) Write(e);
    }

    /// <summary>Access denied (one record per IP + reason per 10 seconds).</summary>
    public JsonObject? Deny(string reason, ReqMeta? meta = null, JsonObject? user = null, string? detail = null, string? device = null)
    {
        var key = $"{meta?.Ip}|{reason}|{user?["id"]}";
        lock (_l)
        {
            var now = DateTime.UtcNow;
            if (_denySeen.TryGetValue(key, out var t) && (now - t).TotalSeconds < 10) return null;
            if (_denySeen.Count > 1000) foreach (var k in _denySeen.Where(p => (now - p.Value).TotalSeconds > 60).Select(p => p.Key).ToList()) _denySeen.Remove(k);
            _denySeen[key] = now;
        }
        return Add("deny", reason, user, meta, detail, "warn", device);
    }

    // streaming sessions
    public void SessionStart(string sid, string who, string? ip)
    {
        lock (_l) _active[sid] = new JsonObject { ["sid"] = sid, ["who"] = who, ["ip"] = ip, ["started"] = DateTimeOffset.UtcNow.ToUnixTimeMilliseconds() / 1000.0 };
    }

    public void SessionEnd(string sid) { lock (_l) _active.Remove(sid); }

    public JsonArray ActiveList()
    {
        var now = DateTimeOffset.UtcNow.ToUnixTimeMilliseconds() / 1000.0;
        var arr = new JsonArray();
        lock (_l)
            foreach (var v in _active.Values)
            {
                var c = (JsonObject)v.DeepClone();
                c["seconds"] = (int)(now - v["started"]!.GetValue<double>());
                arr.Add(c);
            }
        return arr;
    }

    public JsonArray Query(int after = 0, int limit = 500)
    {
        lock (_l)
        {
            var res = _items.Where(e => (e["id"]?.GetValue<int>() ?? 0) > after).TakeLast(limit);
            return new JsonArray(res.Select(e => (JsonNode)e.DeepClone()).ToArray());
        }
    }

    public JsonArray Updates()
    {
        lock (_l)
            return new JsonArray(_items.TakeLast(80).Select(e => (JsonNode)new JsonObject
            {
                ["id"] = e["id"]?.DeepClone(), ["place"] = e["place"]?.DeepClone(), ["isp"] = e["isp"]?.DeepClone(),
            }).ToArray());
    }

    public JsonObject Stats()
    {
        var today = DateTime.Now.ToString("yyyy-MM-dd");
        lock (_l)
        {
            var t = _items.Where(e => (e["time"]?.ToString() ?? "").StartsWith(today)).ToList();
            return new JsonObject
            {
                ["today"] = t.Count,
                ["ips"] = t.Select(e => e["ip"]?.ToString()).Where(x => !string.IsNullOrEmpty(x)).Distinct().Count(),
                ["denied"] = t.Count(e => e["cat"]?.ToString() == "deny"),
                ["streams"] = t.Count(e => e["cat"]?.ToString() == "stream" && e["ev"]?.ToString() == "start"),
            };
        }
    }

    public string ToCsv()
    {
        var sb = new StringBuilder("﻿");
        string Q(string? s) => "\"" + (s ?? "").Replace("\"", "\"\"") + "\"";
        sb.AppendLine(L.Pc("Время;Тип;Событие;Детали;Пользователь;ID;IP;Место;Провайдер;Устройство"));
        lock (_l)
            foreach (var e in _items)
                sb.AppendLine(string.Join(";", new[] { "time", "cat", "action", "detail", "user", "uid", "ip", "place", "isp", "device" }.Select(k => Q(e[k]?.ToString()))));
        return sb.ToString();
    }
}
