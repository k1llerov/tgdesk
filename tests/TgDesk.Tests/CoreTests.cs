using System.Security.Cryptography;
using System.Text;
using System.Text.Json;
using TgDesk;
using Xunit;

// Cfg / Users / Pairing are static: run everything in one collection, one after another.
[assembly: CollectionBehavior(DisableTestParallelization = true)]

namespace TgDesk.Tests;

public class CoreTests : IDisposable
{
    readonly string _home = Path.Combine(Path.GetTempPath(), "tgdesk-tests-" + Guid.NewGuid().ToString("N"));

    public CoreTests()
    {
        Environment.SetEnvironmentVariable("TGDESK_HOME", _home);
        Cfg.Init();
        Users.Load();
        App.Alog = new AccessLog(Cfg.AccessLogPath);
        foreach (var p in Perm.All) Cfg.Features[p] = true;
    }

    public void Dispose()
    {
        Pairing.Cancel();
        foreach (var u in Users.All) Users.Remove(u.Id);
        try { Directory.Delete(_home, true); } catch { }
    }

    [Fact]
    public void Pairing_IsSingleUse_AndLocksAfterFiveWrongCodes()
    {
        Assert.Equal(Pairing.Result.NotActive, Pairing.TryClaim("123456", 1, "A", null, "ru", out _));

        var (code, secs) = Pairing.Start("owner", Perm.All);
        Assert.Equal(6, code.Length);
        Assert.Equal(300, secs);
        var wrong = code == "000000" ? "111111" : "000000";
        for (int i = 0; i < Pairing.MaxFails - 1; i++)
            Assert.Equal(Pairing.Result.Wrong, Pairing.TryClaim(wrong, 9, "X", null, "en", out _));
        Assert.Equal(Pairing.Result.Locked, Pairing.TryClaim(wrong, 9, "X", null, "en", out _));
        Assert.Equal(Pairing.Result.NotActive, Pairing.TryClaim(code, 9, "X", null, "en", out _));

        (code, _) = Pairing.Start("owner", Perm.All);
        Assert.Equal(Pairing.Result.Ok, Pairing.TryClaim(code, 42, "Owner", "own", "ru", out var u));
        Assert.True(u!.IsOwner);
        Assert.Equal("ru", u.Lang);
        Assert.Equal(Pairing.Result.NotActive, Pairing.TryClaim(code, 43, "Y", null, "en", out _));
    }

    [Fact]
    public void Permissions_RespectUserAndGlobalFeatures()
    {
        var (code, _) = Pairing.Start("user", new[] { Perm.Media, "bogus" });
        Assert.Equal(Pairing.Result.Ok, Pairing.TryClaim(code, 50, "Guest", null, "en", out var g));
        Assert.Equal(new[] { Perm.Media }, g!.Perms.ToArray());
        Assert.True(Users.Can(g, Perm.Media));
        Assert.False(Users.Can(g, Perm.Screen));

        var owner = Users.Add(51, "Owner", null, "owner", Array.Empty<string>(), "en");
        Assert.True(Users.Can(owner, Perm.Power));

        Cfg.Features[Perm.Media] = false;
        Assert.False(Users.Can(g, Perm.Media));
        Assert.False(Users.Can(owner, Perm.Media));
        Assert.False(Users.Can(null, Perm.Media));
    }

    [Fact]
    public void Users_ArePersisted()
    {
        Users.Add(70, "A", "a", "user", new[] { Perm.Screen }, "en");
        Users.Load();
        Assert.Equal(Perm.Screen, Assert.Single(Users.Get(70)!.Perms));
        Assert.True(Users.Remove(70));
        Users.Load();
        Assert.Null(Users.Get(70));
    }

    [Fact]
    public void Token_IsEncryptedOnDisk()
    {
        const string token = "123456789:AAEfaketokenfaketokenfaketokenfake1";
        Cfg.Token = token;
        Cfg.Save();
        var raw = File.ReadAllText(Cfg.ConfigPath);
        Assert.DoesNotContain(token, raw);
        Cfg.Token = "";
        Cfg.Load();
        Assert.Equal(token, Cfg.Token);
    }

    [Fact]
    public void InitData_SignatureIsChecked()
    {
        const string token = "123456789:AAEfaketokenfaketokenfaketokenfake1";
        var user = JsonSerializer.Serialize(new { id = 5, first_name = "T", language_code = "en" });
        var data = new SortedDictionary<string, string>(StringComparer.Ordinal)
        {
            ["auth_date"] = DateTimeOffset.UtcNow.ToUnixTimeSeconds().ToString(), ["query_id"] = "q", ["user"] = user,
        };
        var check = string.Join("\n", data.Select(p => $"{p.Key}={p.Value}"));
        var secret = HMACSHA256.HashData(Encoding.UTF8.GetBytes("WebAppData"), Encoding.UTF8.GetBytes(token));
        var hash = Convert.ToHexString(HMACSHA256.HashData(secret, Encoding.UTF8.GetBytes(check))).ToLowerInvariant();
        var init = string.Join("&", data.Select(p => $"{p.Key}={Uri.EscapeDataString(p.Value)}")) + "&hash=" + hash;

        Assert.Equal(5, TgAuth.UserId(TgAuth.Validate(init, token, 3600)));
        Assert.Null(TgAuth.Validate(init, token.Replace('1', '2'), 3600));
        Assert.Null(TgAuth.Validate(init.Replace("q", "z"), token, 3600));
        Assert.Null(TgAuth.Validate("auth_date=1&hash=00", token, 3600));
    }

    [Theory]
    [InlineData("1.0.1", "1.0.0", true)]
    [InlineData("1.0.0", "1.0.0", false)]
    [InlineData("2.0", "1.9.9", true)]
    [InlineData("1.0.0-beta", "1.0.0", false)]
    public void Updater_ComparesVersions(string a, string b, bool newer) => Assert.Equal(newer, Updater.Newer(a, b));

    [Fact]
    public void Log_NeverContainsTheToken() =>
        Assert.Equal("https://api.telegram.org/bot<token>/getMe", Log.Redact("https://api.telegram.org/bot123456789:AAEfaketokenfaketokenfaketokenfake1/getMe"));

    [Fact]
    public void PublicUrl_MustBeHttps()
    {
        Assert.True(Tunnel.ValidPublicUrl("https://pc.example.com"));
        Assert.False(Tunnel.ValidPublicUrl("http://pc.example.com"));
        Assert.False(Tunnel.ValidPublicUrl("https://localhost"));
    }

    [Fact]
    public void Strings_AreTranslated()
    {
        Assert.Equal("Done", L.T("Готово", "en"));
        Assert.Equal("Готово", L.T("Готово", "ru"));
        Assert.Equal("Screen 2", L.F("Экран {0}", "en", 2));
    }
}
