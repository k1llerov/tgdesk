using System.Diagnostics;
using System.Drawing;
using System.Drawing.Drawing2D;
using System.Drawing.Imaging;
using System.Runtime.InteropServices;
using System.Text.Json.Nodes;
using Microsoft.Win32;

namespace TgDesk;

/// <summary>System functions: status, processes, power, files, autostart.</summary>
public static class Sys
{
    public static async Task<JsonObject> Status()
    {
        var cpu = await Native.CpuPercent(300);
        var (total, avail) = Native.Memory();
        var used = total - avail;
        var disks = new JsonArray();
        foreach (var d in DriveInfo.GetDrives())
        {
            try
            {
                if (!d.IsReady || d.DriveType is DriveType.CDRom or DriveType.NoRootDirectory) continue;
                disks.Add(new JsonObject
                {
                    ["device"] = d.Name, ["percent"] = Math.Round((1 - (double)d.TotalFreeSpace / d.TotalSize) * 100, 1),
                    ["free"] = d.TotalFreeSpace, ["total"] = d.TotalSize,
                });
            }
            catch { }
        }
        var ver = Environment.OSVersion.Version;
        var res = new JsonObject
        {
            ["host"] = Environment.MachineName,
            ["os"] = "Windows " + (ver.Build >= 22000 ? "11" : "10"),
            ["cpu"] = Math.Round(cpu, 1),
            ["cpu_count"] = Environment.ProcessorCount,
            ["ram"] = new JsonObject { ["percent"] = Math.Round((double)used / total * 100, 1), ["used"] = used, ["total"] = total },
            ["uptime"] = Environment.TickCount64 / 1000,
            ["disks"] = disks,
            ["battery"] = null,
        };
        try
        {
            var ps = System.Windows.Forms.SystemInformation.PowerStatus;
            if (ps.BatteryChargeStatus != System.Windows.Forms.BatteryChargeStatus.NoSystemBattery && ps.BatteryLifePercent <= 1)
                res["battery"] = new JsonObject { ["percent"] = Math.Round(ps.BatteryLifePercent * 100.0), ["plugged"] = ps.PowerLineStatus == System.Windows.Forms.PowerLineStatus.Online };
        }
        catch { }
        return res;
    }

    public static async Task<JsonArray> Processes(int limit = 40)
    {
        var first = new Dictionary<int, (TimeSpan cpu, string name, long mem)>();
        foreach (var p in Process.GetProcesses())
        {
            try { first[p.Id] = (p.TotalProcessorTime, p.ProcessName, p.WorkingSet64); } catch { try { first[p.Id] = (TimeSpan.Zero, p.ProcessName, 0); } catch { } }
            p.Dispose();
        }
        var sw = Stopwatch.StartNew();
        await Task.Delay(500);
        var (total, _) = Native.Memory();
        var rows = new List<(int pid, string name, double cpu, double mem)>();
        foreach (var p in Process.GetProcesses())
        {
            try
            {
                if (!first.TryGetValue(p.Id, out var f)) continue;
                double cpu = Math.Max(0, (p.TotalProcessorTime - f.cpu).TotalMilliseconds / sw.Elapsed.TotalMilliseconds / Environment.ProcessorCount * 100);
                rows.Add((p.Id, p.ProcessName + ".exe", Math.Round(cpu, 1), Math.Round(p.WorkingSet64 * 100.0 / total, 1)));
            }
            catch { }
            finally { p.Dispose(); }
        }
        var arr = new JsonArray();
        foreach (var r in rows.OrderByDescending(r => r.cpu).ThenByDescending(r => r.mem).Take(limit))
            arr.Add(new JsonObject { ["pid"] = r.pid, ["name"] = r.name, ["cpu"] = r.cpu, ["mem"] = r.mem });
        return arr;
    }

    static readonly HashSet<string> Protected = new(StringComparer.OrdinalIgnoreCase) { "TgDesk", "TgDeskBot", "csrss", "wininit", "winlogon", "lsass", "smss", "services", "System" };

    public static string Kill(int pid, string lang)
    {
        if (pid == Environment.ProcessId) throw new ArgumentException(L.T("Это сам TgDesk — его так не остановить.", lang));
        using var p = Process.GetProcessById(pid);
        var name = p.ProcessName;
        if (Protected.Contains(name)) throw new ArgumentException(L.F("Процесс {0} защищён — его нельзя завершить удалённо.", lang, name + ".exe"));
        p.Kill();
        return L.F("Процесс {0} ({1}) завершён", lang, pid, name + ".exe");
    }

    public static void Power(string action)
    {
        switch (action)
        {
            case "lock": Native.LockWorkStation(); break;
            case "sleep": Run("rundll32.exe", "powrprof.dll,SetSuspendState 0,1,0"); break;
            case "restart": Run("shutdown", "/r /t 5"); break;
            case "shutdown": Run("shutdown", "/s /t 5"); break;
            default: throw new ArgumentException("unknown action");
        }
    }

    static void Run(string exe, string args) =>
        Process.Start(new ProcessStartInfo(exe, args) { CreateNoWindow = true, UseShellExecute = false });

    public static void OpenOnPc(string path) => Process.Start(new ProcessStartInfo(path) { UseShellExecute = true });

    public static void RevealOnPc(string path)
    {
        if (Directory.Exists(path)) OpenOnPc(path);
        else Process.Start(new ProcessStartInfo("explorer.exe", $"/select,\"{Path.GetFullPath(path)}\"") { UseShellExecute = false });
    }

    // ------------------------------------------------------------ files
    public static JsonObject ListDir(string path, string lang)
    {
        if (string.IsNullOrEmpty(path))
        {
            var home = Environment.GetFolderPath(Environment.SpecialFolder.UserProfile);
            var entries = new JsonArray();
            var seen = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
            void Q(string name, string full, string? icon)
            {
                if (Directory.Exists(full) && seen.Add(full))
                    entries.Add(new JsonObject { ["name"] = name, ["path"] = full, ["dir"] = true, ["size"] = null, ["quick"] = true, ["icon"] = icon });
            }
            Q(L.T("Изображения", lang), Cfg.PicturesRoot, "image");
            Q(L.T("Музыка", lang), Cfg.MusicRoot, "audio");
            Q(L.T("Видео", lang), Cfg.VideosRoot, "video");
            Q(L.T("Рабочий стол", lang), Environment.GetFolderPath(Environment.SpecialFolder.DesktopDirectory), null);
            Q(L.T("Загрузки", lang), Path.Combine(home, "Downloads"), null);
            Q(L.T("Документы", lang), Environment.GetFolderPath(Environment.SpecialFolder.MyDocuments), null);
            Q(L.T("Загрузки TgDesk", lang), Cfg.DownloadDir, null);
            foreach (var d in DriveInfo.GetDrives())
                if (d.IsReady) entries.Add(new JsonObject { ["name"] = d.Name.TrimEnd('\\'), ["path"] = d.Name, ["dir"] = true, ["size"] = null, ["drive"] = true });
            return new JsonObject { ["path"] = "", ["parent"] = null, ["entries"] = entries, ["truncated"] = false };
        }

        path = Path.GetFullPath(path);
        var items = new List<JsonObject>();
        foreach (var e in new DirectoryInfo(path).EnumerateFileSystemInfos())
        {
            try
            {
                bool dir = e.Attributes.HasFlag(FileAttributes.Directory);
                if (e.Name.StartsWith(".tgdesk-") && e.Name.EndsWith(".part")) continue;
                items.Add(new JsonObject
                {
                    ["name"] = e.Name, ["path"] = e.FullName, ["dir"] = dir,
                    ["size"] = dir ? null : ((FileInfo)e).Length, ["kind"] = dir ? null : Util.Kind(e.Name),
                });
            }
            catch { }
        }
        items = items.OrderBy(x => !x["dir"]!.GetValue<bool>()).ThenBy(x => x["name"]!.ToString(), StringComparer.OrdinalIgnoreCase).ToList();
        var parent = Path.GetDirectoryName(path.TrimEnd('\\')) ?? "";
        if (Path.GetPathRoot(path)?.TrimEnd('\\') == path.TrimEnd('\\')) parent = "";
        return new JsonObject
        {
            ["path"] = path, ["parent"] = parent,
            ["entries"] = new JsonArray(items.Take(1000).Select(x => (JsonNode)x).ToArray()),
            ["truncated"] = items.Count > 1000,
        };
    }

    // ------------------------------------------------------------ thumbnails
    static readonly Dictionary<string, byte[]> Thumbs = new();
    static readonly Queue<string> ThumbOrder = new();
    static readonly object ThumbL = new();
    static readonly ImageCodecInfo Jpeg = ImageCodecInfo.GetImageEncoders().First(c => c.FormatID == ImageFormat.Jpeg.Guid);

    public static byte[] Thumb(string path, int size)
    {
        size = Math.Clamp(size, 32, 800);
        var fi = new FileInfo(path);
        var key = $"{path}|{fi.LastWriteTimeUtc.Ticks}|{fi.Length}|{size}";
        lock (ThumbL) if (Thumbs.TryGetValue(key, out var c)) return c;
        using var src = Image.FromFile(path);
        foreach (var pi in src.PropertyItems)
        {
            if (pi.Id != 0x0112 || pi.Value is not { Length: > 0 }) continue;
            switch (pi.Value[0])
            {
                case 3: src.RotateFlip(RotateFlipType.Rotate180FlipNone); break;
                case 6: src.RotateFlip(RotateFlipType.Rotate90FlipNone); break;
                case 8: src.RotateFlip(RotateFlipType.Rotate270FlipNone); break;
            }
            break;
        }
        double k = Math.Min(1.0, (double)size / Math.Max(src.Width, src.Height));
        int w = Math.Max(1, (int)(src.Width * k)), h = Math.Max(1, (int)(src.Height * k));
        using var bmp = new Bitmap(w, h, PixelFormat.Format24bppRgb);
        using (var g = Graphics.FromImage(bmp))
        {
            g.Clear(Color.FromArgb(20, 22, 32));
            g.InterpolationMode = InterpolationMode.HighQualityBilinear;
            g.DrawImage(src, 0, 0, w, h);
        }
        using var ms = new MemoryStream();
        using var ep = new EncoderParameters(1);
        ep.Param[0] = new EncoderParameter(System.Drawing.Imaging.Encoder.Quality, 78L);
        bmp.Save(ms, Jpeg, ep);
        var data = ms.ToArray();
        lock (ThumbL)
        {
            Thumbs[key] = data; ThumbOrder.Enqueue(key);
            while (ThumbOrder.Count > 400) Thumbs.Remove(ThumbOrder.Dequeue());
        }
        return data;
    }

    // ------------------------------------------------------------ autostart
    const string RunKey = @"Software\Microsoft\Windows\CurrentVersion\Run";
    const string RunName = "TgDesk";

    /// <summary>What to run at Windows sign-in: the desktop app (in the tray), or the bot itself if the app is missing.</summary>
    public static string LaunchCommand()
    {
        var mgr = Path.Combine(Cfg.Install, "TgDesk.exe");
        if (File.Exists(mgr)) return $"\"{mgr}\" --tray";
        return $"\"{Environment.ProcessPath}\"";
    }

    public static bool AutostartGet()
    {
        using var k = Registry.CurrentUser.OpenSubKey(RunKey);
        return k?.GetValue(RunName) != null;
    }

    public static void AutostartSet(bool on)
    {
        using var k = Registry.CurrentUser.OpenSubKey(RunKey, true)!;
        if (on) k.SetValue(RunName, LaunchCommand());
        else k.DeleteValue(RunName, false);
    }

    /// <summary>Keeps the Run key in sync with the setting (and with the current install path).</summary>
    public static void ApplyAutostartOnStart()
    {
        if (Cfg.Autostart) { AutostartSet(true); Log.Info("Autostart: " + LaunchCommand()); }
        else if (AutostartGet()) { AutostartSet(false); Log.Info("Autostart disabled"); }
    }
}
