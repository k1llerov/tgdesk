using System.Security.Cryptography;
using System.Text;
using System.Text.Json.Nodes;

namespace TgDesk;

/// <summary>Permissions. Each one is also a global feature switch (config.json → features).</summary>
public static class Perm
{
    public const string Screen = "screen";       // screenshots and screen streaming
    public const string Input = "input";         // mouse and keyboard control
    public const string Files = "files";         // browse, download, upload, open files
    public const string Processes = "processes"; // list and kill processes
    public const string Media = "media";         // volume and media keys
    public const string Power = "power";         // lock, sleep, restart, shut down
    public static readonly string[] All = { Screen, Input, Files, Processes, Media, Power };
    public static bool Valid(string p) => All.Contains(p);
}

public sealed class TgUser
{
    public long Id;
    public string Name = "";
    public string? Username;
    public string Role = "user";              // owner | user
    public HashSet<string> Perms = new();     // ignored for owners (they have everything)
    public string Added = "";
    public string? LastSeen;
    public string Lang = "en";

    public bool IsOwner => Role == "owner";

    public JsonObject ToJson() => new()
    {
        ["id"] = Id, ["name"] = Name, ["username"] = Username, ["role"] = Role,
        ["perms"] = new JsonArray(Perms.Select(p => (JsonNode)p).ToArray()),
        ["added"] = Added, ["last_seen"] = LastSeen, ["lang"] = Lang,
    };

    public static TgUser? FromJson(JsonNode? n)
    {
        if (n is not JsonObject o || o["id"] is not JsonValue idv || !idv.TryGetValue<long>(out var id) || id == 0) return null;
        return new TgUser
        {
            Id = id,
            Name = o["name"]?.ToString() ?? "",
            Username = o["username"]?.ToString(),
            Role = o["role"]?.ToString() == "owner" ? "owner" : "user",
            Perms = (o["perms"] as JsonArray)?.Select(x => x?.ToString() ?? "").Where(Perm.Valid).ToHashSet() ?? new(),
            Added = o["added"]?.ToString() ?? "",
            LastSeen = o["last_seen"]?.ToString(),
            Lang = o["lang"]?.ToString() == "ru" ? "ru" : "en",
        };
    }
}

/// <summary>Users allowed to control this PC (users.json). Changed only from the desktop app (local control API) and by pairing.</summary>
public static class Users
{
    static readonly object L = new();
    static List<TgUser> _list = new();
    public static event Action<TgUser>? Added;
    public static event Action<TgUser>? Removed;

    public static void Load()
    {
        lock (L)
        {
            try
            {
                if (!File.Exists(Cfg.UsersPath)) { _list = new(); return; }
                var arr = JsonNode.Parse(File.ReadAllText(Cfg.UsersPath, Encoding.UTF8)) as JsonArray ?? new JsonArray();
                _list = arr.Select(TgUser.FromJson).Where(u => u != null).Cast<TgUser>().GroupBy(u => u.Id).Select(g => g.First()).ToList();
            }
            catch (Exception e) { Log.Error("users.json is damaged", e); _list = new(); }
        }
    }

    static void Save()
    {
        var arr = new JsonArray(_list.Select(u => (JsonNode)u.ToJson()).ToArray());
        Cfg.AtomicWrite(Cfg.UsersPath, arr.ToJsonString(Json.Pretty));
        Security.LockDown(Cfg.UsersPath);
    }

    public static int Count { get { lock (L) return _list.Count; } }
    public static List<TgUser> All { get { lock (L) return _list.ToList(); } }
    public static TgUser? Get(long id) { lock (L) return _list.FirstOrDefault(u => u.Id == id); }

    public static TgUser Add(long id, string name, string? username, string role, IEnumerable<string> perms, string lang)
    {
        TgUser u;
        lock (L)
        {
            _list.RemoveAll(x => x.Id == id);
            u = new TgUser
            {
                Id = id, Name = name, Username = username, Role = role == "owner" ? "owner" : "user",
                Perms = perms.Where(Perm.Valid).ToHashSet(), Added = DateTime.Now.ToString("yyyy-MM-dd HH:mm"), Lang = lang,
            };
            _list.Add(u);
            Save();
        }
        Added?.Invoke(u);
        return u;
    }

    public static bool Update(long id, string? role, IEnumerable<string>? perms)
    {
        lock (L)
        {
            var u = _list.FirstOrDefault(x => x.Id == id);
            if (u == null) return false;
            if (role is "owner" or "user") u.Role = role;
            if (perms != null) u.Perms = perms.Where(Perm.Valid).ToHashSet();
            Save();
            return true;
        }
    }

    public static bool Remove(long id)
    {
        TgUser? u;
        lock (L)
        {
            u = _list.FirstOrDefault(x => x.Id == id);
            if (u == null) return false;
            _list.Remove(u);
            Save();
        }
        Removed?.Invoke(u);
        return true;
    }

    /// <summary>Remember the display name / language the user has now (Telegram profile may change).</summary>
    public static void Touch(long id, string? name, string? username, string? langCode)
    {
        lock (L)
        {
            var u = _list.FirstOrDefault(x => x.Id == id);
            if (u == null) return;
            bool changed = false;
            if (!string.IsNullOrEmpty(name) && u.Name != name) { u.Name = name; changed = true; }
            if (u.Username != username) { u.Username = username; changed = true; }
            if (langCode != null) { var l = Cfg.TgLang(langCode); if (u.Lang != l) { u.Lang = l; changed = true; } }
            var now = DateTime.Now.ToString("yyyy-MM-dd HH:mm");
            if (u.LastSeen == null || u.LastSeen.Length < 13 || u.LastSeen[..13] != now[..13]) { u.LastSeen = now; changed = true; } // at most once an hour
            if (changed) Save();
        }
    }

    /// <summary>A permission is granted when the feature is on globally and the user is an owner or has the permission.</summary>
    public static bool Can(TgUser? u, string perm) =>
        u != null && Cfg.Features.GetValueOrDefault(perm, false) && (u.IsOwner || u.Perms.Contains(perm));

    public static IEnumerable<string> Effective(TgUser u) => Perm.All.Where(p => Can(u, p));
}

/// <summary>
/// Adding a user: the desktop app asks for a one-time 6-digit code (valid 5 minutes),
/// the person sends it to the bot. Five wrong codes cancel the pairing. With no active pairing
/// the bot ignores strangers completely.
/// </summary>
public static class Pairing
{
    public const int Lifetime = 300;
    public const int MaxFails = 5;

    sealed class P { public string Code = ""; public DateTime Exp; public string Role = "user"; public HashSet<string> Perms = new(); public int Fails; }
    static P? _cur;
    static readonly object L = new();
    public static JsonObject? LastPaired;   // for the desktop app: who was added last
    static int _seq;

    public static (string code, int seconds) Start(string role, IEnumerable<string> perms)
    {
        lock (L)
        {
            _cur = new P
            {
                Code = RandomNumberGenerator.GetInt32(0, 1_000_000).ToString("D6"),
                Exp = DateTime.UtcNow.AddSeconds(Lifetime),
                Role = role == "owner" ? "owner" : "user",
                Perms = perms.Where(Perm.Valid).ToHashSet(),
            };
            Log.Info($"Pairing started (role {_cur.Role})");
            return (_cur.Code, Lifetime);
        }
    }

    public static void Cancel() { lock (L) _cur = null; }

    public static bool Active { get { lock (L) return _cur != null && _cur.Exp > DateTime.UtcNow; } }

    public static JsonObject State()
    {
        lock (L)
        {
            if (_cur == null || _cur.Exp <= DateTime.UtcNow) return new JsonObject { ["active"] = false };
            return new JsonObject
            {
                ["active"] = true, ["code"] = _cur.Code, ["role"] = _cur.Role,
                ["expires_in"] = (int)(_cur.Exp - DateTime.UtcNow).TotalSeconds,
            };
        }
    }

    public enum Result { NotActive, Wrong, Locked, Ok }

    /// <summary>Checks a code sent by a Telegram user. On success the user is added.</summary>
    public static Result TryClaim(string code, long id, string name, string? username, string? langCode, out TgUser? user)
    {
        user = null;
        P cur;
        lock (L)
        {
            if (_cur == null || _cur.Exp <= DateTime.UtcNow) { _cur = null; return Result.NotActive; }
            code = new string((code ?? "").Where(char.IsDigit).ToArray());
            if (!Security.TokenEquals(code, _cur.Code))
            {
                if (++_cur.Fails >= MaxFails) { _cur = null; Log.Warn("Pairing cancelled: too many wrong codes"); return Result.Locked; }
                return Result.Wrong;
            }
            cur = _cur;
            _cur = null;
        }
        user = Users.Add(id, name, username, cur.Role, cur.Perms, Cfg.TgLang(langCode));
        lock (L) LastPaired = new JsonObject { ["seq"] = ++_seq, ["id"] = id, ["name"] = name, ["username"] = username, ["role"] = user.Role };
        Log.Info($"User {id} added by pairing (role {user.Role})");
        return Result.Ok;
    }
}
