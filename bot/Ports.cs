using System.Net;
using System.Net.Sockets;

namespace TgDesk;

/// <summary>
/// The default ports (8765/8766/8767) may be taken by another program on someone's PC. Instead of crashing,
/// the bot moves to free ports and saves them to config.json; the desktop app re-reads them from there.
/// The web port is moved only in Quick Tunnel mode: with an own tunnel or address it is part of the user's
/// routing (http://127.0.0.1:8765), so there it is reported instead.
/// </summary>
public static class Ports
{
    public static bool IsFree(int port)
    {
        try
        {
            var l = new TcpListener(IPAddress.Loopback, port);
            l.Start();
            l.Stop();
            return true;
        }
        catch (SocketException) { return false; }
    }

    static int FindFree(int preferred, ISet<int> taken)
    {
        for (int p = preferred + 100; p < preferred + 2000; p++)
            if (!taken.Contains(p) && IsFree(p)) return p;
        // let Windows pick any free port
        var l = new TcpListener(IPAddress.Loopback, 0);
        l.Start();
        int any = ((IPEndPoint)l.LocalEndpoint).Port;
        l.Stop();
        return any;
    }

    public static void EnsureFree()
    {
        var taken = new HashSet<int>();
        bool changed = false;
        int Pick(int port, string name, bool movable)
        {
            if (!taken.Contains(port) && IsFree(port)) { taken.Add(port); return port; }
            if (!movable)
            {
                Log.Error($"Port {port} ({name}) is used by another program. Free it or route your tunnel to another port in config.json → ports.web");
                taken.Add(port);
                return port;
            }
            int np = FindFree(port, taken);
            Log.Warn($"Port {port} ({name}) is used by another program — using {np}");
            taken.Add(np);
            changed = true;
            return np;
        }
        Cfg.Port = Pick(Cfg.Port, "web", Cfg.TunnelMode == "quick");
        Cfg.LogPort = Pick(Cfg.LogPort, "log", true);
        Cfg.ControlPort = Pick(Cfg.ControlPort, "control", true);
        if (changed) Cfg.Save();
    }
}
