using System.Net;
using TgDesk;

Cfg.Init();
Native.SetProcessDPIAware();
AppDomain.CurrentDomain.UnhandledException += (_, e) => Log.Error("Unhandled exception", e.ExceptionObject as Exception);
TaskScheduler.UnobservedTaskException += (_, e) => { Log.Error("Unobserved task exception", e.Exception); e.SetObserved(); };

using var mutex = new Mutex(true, "Local\\TgDeskBot_Singleton", out var created);
if (!created) { Log.Info("TgDesk bot is already running — exit"); return; }

Tunnel.KillStale();
Ports.EnsureFree();
File.WriteAllText(Cfg.PidPath, Environment.ProcessId.ToString());
Users.Load();
App.Alog = new AccessLog(Cfg.AccessLogPath);
if (!Cfg.Configured) Log.Warn("Bot token is not set — open the TgDesk app to finish the setup");
foreach (var p in new[] { Cfg.ConfigPath, Cfg.UsersPath, Cfg.ControlTokenPath, Cfg.AccessLogPath }) Security.LockDown(p);

var builder = WebApplication.CreateBuilder(new WebApplicationOptions { ContentRootPath = Cfg.Data, Args = args });
builder.Logging.ClearProviders();
builder.WebHost.ConfigureKestrel(o =>
{
    o.AddServerHeader = false;
    o.Limits.MaxRequestBodySize = null;
    o.Listen(IPAddress.Loopback, Cfg.Port);
    o.Listen(IPAddress.Loopback, Cfg.LogPort);
    o.Listen(IPAddress.Loopback, Cfg.ControlPort);
});
var app = builder.Build();
App.Life = app.Lifetime;
app.UseWebSockets(new WebSocketOptions { KeepAliveInterval = TimeSpan.FromSeconds(20) });

// One process, three ports (all on 127.0.0.1):
//  WEB     — the Mini App, reachable from the internet only through the tunnel;
//  LOG     — access log web page, this PC only;
//  CONTROL — API for the desktop app, this PC only + random token from control.token.
app.Use(async (ctx, next) =>
{
    int lp = ctx.Connection.LocalPort;
    var path = ctx.Request.Path.Value ?? "/";
    var host = ctx.Request.Host.Value ?? "";
    bool localHost = host == $"127.0.0.1:{lp}" || host == $"localhost:{lp}";
    var h = ctx.Response.Headers;
    h["X-Content-Type-Options"] = "nosniff";
    h["Referrer-Policy"] = "no-referrer";
    if (path.StartsWith("/api/") || path.StartsWith("/m/")) h["Cache-Control"] = "no-store";
    if (lp == Cfg.LogPort)
    {
        if (!localHost || ctx.Request.Headers.ContainsKey("Cf-Connecting-Ip")) { ctx.Response.StatusCode = 403; await ctx.Response.WriteAsync("local only"); return; }
        h["X-Frame-Options"] = "DENY";
        ctx.Request.Path = "/_log" + (path == "/" ? "" : path);
    }
    else if (lp == Cfg.ControlPort)
    {
        if (!localHost || ctx.Request.Headers.ContainsKey("Cf-Connecting-Ip") || ctx.Request.Headers.ContainsKey("Origin")
            || !Security.TokenEquals(ctx.Request.Headers["X-Control-Token"], Cfg.ControlToken))
        { ctx.Response.StatusCode = 403; await ctx.Response.WriteAsync("forbidden"); return; }
        ctx.Request.Path = "/_ctl" + path;
    }
    else
    {
        // the public port: internal paths are closed, foreign Host and blocked IPs are refused
        if (path.StartsWith("/_")) { ctx.Response.StatusCode = 404; return; }
        var meta = ReqMeta.From(ctx.Request);
        if (Security.IsBlocked(meta.Ip))
        {
            ctx.Response.StatusCode = 429;
            ctx.Response.Headers["Retry-After"] = Security.RetryAfterSeconds(meta.Ip).ToString();
            return;
        }
        if (!Security.HostAllowed(host))
        {
            App.Alog.Deny(L.Pc("Запрос с чужим Host"), meta, null, host);
            Security.Fail(meta.Ip);
            ctx.Response.StatusCode = 421;
            return;
        }
    }
    try { await next(); }
    catch (Exception e) when (e is not OperationCanceledException)
    {
        Log.Error($"Unhandled error {ctx.Request.Method} {path}", e);
        if (!ctx.Response.HasStarted) { ctx.Response.StatusCode = 500; await ctx.Response.WriteAsync("{\"error\":\"internal error\"}"); }
    }
});

app.UseRouting(); // routing strictly after the per-port path rewrite
Routes.Map(app);
Routes.MapLogs(app);
Routes.MapControl(app);

await app.StartAsync();
Log.Info($"TgDesk bot {Cfg.Version} started, pid {Environment.ProcessId}, data: {Cfg.Data}");
Log.Info($"Web 127.0.0.1:{Cfg.Port}; log http://127.0.0.1:{Cfg.LogPort}/; control :{Cfg.ControlPort}");
App.Alog.Add("system", L.Pc("Бот запущен"), detail: $"{Environment.MachineName}, pid {Environment.ProcessId}");

App.Overlay = new StreamOverlay();
try { Sys.ApplyAutostartOnStart(); } catch (Exception e) { Log.Error("Autostart setup failed", e); }

var cts = new CancellationTokenSource();
App.Tunnel.UrlChanged += url => _ = Task.Run(() => App.Bot.OnTunnelUrl(url, cts.Token));
App.Bot.Start(cts.Token);
if (Cfg.Configured) App.EnsureTunnel();   // without a token there is nobody to serve yet
else if (Cfg.TunnelMode != "external")
    // the components TgDesk needs are fetched into its own folder while the user is still in the setup wizard
    _ = Task.Run(async () =>
    {
        try { await Tunnel.EnsureCloudflared(cts.Token); }
        catch (Exception e) when (e is not OperationCanceledException) { Log.Warn("cloudflared pre-download failed (will retry when the tunnel starts): " + e.Message); }
    });
Updater.Start(cts.Token);

await app.WaitForShutdownAsync();

cts.Cancel();
App.Tunnel.Stop();
Tunnel.KillStale();
try { File.Delete(Cfg.PidPath); } catch { }
Log.Info("TgDesk bot stopped");
