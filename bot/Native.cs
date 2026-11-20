using System.Diagnostics;
using System.Drawing;
using System.Drawing.Drawing2D;
using System.Drawing.Imaging;
using System.Runtime.InteropServices;
using System.Text.Json.Nodes;

namespace TgDesk;

public record Mon(int Index, int Left, int Top, int Width, int Height);

public static class Native
{
    // ------------------------------------------------------------ P/Invoke
    [StructLayout(LayoutKind.Sequential)] struct MOUSEINPUT { public int dx, dy; public uint mouseData, dwFlags, time; public nuint extra; }
    [StructLayout(LayoutKind.Sequential)] struct KEYBDINPUT { public ushort vk, scan; public uint flags, time; public nuint extra; }
    [StructLayout(LayoutKind.Explicit)] struct InputUnion { [FieldOffset(0)] public MOUSEINPUT mi; [FieldOffset(0)] public KEYBDINPUT ki; }
    [StructLayout(LayoutKind.Sequential)] struct INPUT { public uint type; public InputUnion u; }
    [StructLayout(LayoutKind.Sequential)] public struct POINT { public int x, y; }
    [StructLayout(LayoutKind.Sequential)] struct CURSORINFO { public int cbSize, flags; public IntPtr hCursor; public POINT pt; }
    [StructLayout(LayoutKind.Sequential)] struct ICONINFO { public bool fIcon; public int xHotspot, yHotspot; public IntPtr hbmMask, hbmColor; }
    [StructLayout(LayoutKind.Sequential)] struct FILETIME { public uint lo, hi; public ulong V => ((ulong)hi << 32) | lo; }
    [StructLayout(LayoutKind.Sequential)] struct MEMORYSTATUSEX { public uint len, load; public ulong totalPhys, availPhys, totalPage, availPage, totalVirt, availVirt, availExt; }

    [DllImport("user32.dll")] static extern uint SendInput(uint n, INPUT[] inputs, int size);
    [DllImport("user32.dll")] static extern bool SetCursorPos(int x, int y);
    [DllImport("user32.dll")] static extern bool GetCursorPos(out POINT p);
    [DllImport("user32.dll")] static extern bool GetCursorInfo(ref CURSORINFO ci);
    [DllImport("user32.dll")] static extern bool GetIconInfo(IntPtr h, out ICONINFO ii);
    [DllImport("user32.dll")] static extern bool DrawIconEx(IntPtr hdc, int x, int y, IntPtr hIcon, int cx, int cy, uint step, IntPtr brush, uint flags);
    [DllImport("user32.dll")] static extern void keybd_event(byte vk, byte scan, uint flags, nuint extra);
    [DllImport("user32.dll")] public static extern bool LockWorkStation();
    [DllImport("user32.dll")] public static extern int GetSystemMetrics(int i);
    [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
    [DllImport("gdi32.dll")] static extern bool DeleteObject(IntPtr h);
    [DllImport("kernel32.dll")] static extern bool GetSystemTimes(out FILETIME idle, out FILETIME kernel, out FILETIME user);
    [DllImport("kernel32.dll")] static extern bool GlobalMemoryStatusEx(ref MEMORYSTATUSEX m);

    [DllImport("user32.dll")] static extern IntPtr GetDC(IntPtr hwnd);
    [DllImport("user32.dll")] static extern int ReleaseDC(IntPtr hwnd, IntPtr dc);
    [DllImport("gdi32.dll")] static extern bool BitBlt(IntPtr dst, int x, int y, int w, int h, IntPtr src, int sx, int sy, uint rop);

    /// <summary>Copies a screen area into a bitmap (SRCCOPY | CAPTUREBLT — including layered windows).</summary>
    public static void CopyScreen(Bitmap bmp, int x, int y, int w, int h)
    {
        using var g = Graphics.FromImage(bmp);
        var dst = g.GetHdc();
        var src = GetDC(IntPtr.Zero);
        try
        {
            if (!BitBlt(dst, 0, 0, w, h, src, x, y, 0x00CC0020 | 0x40000000))
                throw new InvalidOperationException("BitBlt failed (screen locked?)");
        }
        finally { ReleaseDC(IntPtr.Zero, src); g.ReleaseHdc(dst); }
    }

    // ------------------------------------------------------------ system
    public static (ulong total, ulong avail) Memory()
    {
        var m = new MEMORYSTATUSEX { len = (uint)Marshal.SizeOf<MEMORYSTATUSEX>() };
        GlobalMemoryStatusEx(ref m);
        return (m.totalPhys, m.availPhys);
    }

    static (ulong idle, ulong total) CpuTimes()
    {
        GetSystemTimes(out var i, out var k, out var u);
        return (i.V, k.V + u.V); // kernel time already includes idle
    }

    public static async Task<double> CpuPercent(int ms = 300)
    {
        var a = CpuTimes();
        await Task.Delay(ms);
        var b = CpuTimes();
        double total = b.total - a.total, idle = b.idle - a.idle;
        return total <= 0 ? 0 : Math.Clamp((1 - idle / total) * 100, 0, 100);
    }

    // ------------------------------------------------------------ monitors
    public static List<Mon> Monitors()
    {
        var v = System.Windows.Forms.SystemInformation.VirtualScreen;
        var list = new List<Mon> { new(0, v.Left, v.Top, v.Width, v.Height) };
        int i = 1;
        foreach (var s in System.Windows.Forms.Screen.AllScreens)
            list.Add(new Mon(i++, s.Bounds.Left, s.Bounds.Top, s.Bounds.Width, s.Bounds.Height));
        return list;
    }

    public static (int x, int y) CursorPos() { GetCursorPos(out var p); return (p.x, p.y); }

    // ------------------------------------------------------------ input
    const uint KEYUP = 2, EXT = 1, UNI = 4, WHEEL = 0x800, HWHEEL = 0x1000;

    static INPUT K(ushort vk, ushort scan = 0, uint flags = 0) =>
        new() { type = 1, u = new InputUnion { ki = new KEYBDINPUT { vk = vk, scan = scan, flags = flags } } };

    static INPUT M(uint flags, int data = 0) =>
        new() { type = 0, u = new InputUnion { mi = new MOUSEINPUT { dwFlags = flags, mouseData = unchecked((uint)data) } } };

    static void Send(List<INPUT> seq)
    {
        if (seq.Count == 0) return;
        SendInput((uint)seq.Count, seq.ToArray(), Marshal.SizeOf<INPUT>());
    }

    static readonly Dictionary<string, (uint down, uint up)> Buttons = new()
    {
        ["left"] = (0x2, 0x4), ["right"] = (0x8, 0x10), ["middle"] = (0x20, 0x40),
    };

    public static void MoveAbs(Mon m, double nx, double ny)
    {
        var x = m.Left + Math.Clamp(nx, 0, 1) * (m.Width - 1);
        var y = m.Top + Math.Clamp(ny, 0, 1) * (m.Height - 1);
        SetCursorPos((int)Math.Round(x), (int)Math.Round(y));
    }

    public static void MoveRel(double dx, double dy, Mon virt)
    {
        var (x, y) = CursorPos();
        x = Math.Clamp(x + (int)Math.Round(dx), virt.Left, virt.Left + virt.Width - 1);
        y = Math.Clamp(y + (int)Math.Round(dy), virt.Top, virt.Top + virt.Height - 1);
        SetCursorPos(x, y);
    }

    public static void Button(string name, string action)
    {
        if (!Buttons.TryGetValue(name, out var b)) throw new ArgumentException("bad button");
        switch (action)
        {
            case "down": Send(new() { M(b.down) }); break;
            case "up": Send(new() { M(b.up) }); break;
            case "click": Send(new() { M(b.down), M(b.up) }); break;
            case "dbl": Send(new() { M(b.down), M(b.up), M(b.down), M(b.up) }); break;
            default: throw new ArgumentException("bad action");
        }
    }

    public static void Scroll(int dy, int dx)
    {
        var s = new List<INPUT>();
        if (dy != 0) s.Add(M(WHEEL, dy));
        if (dx != 0) s.Add(M(HWHEEL, dx));
        Send(s);
    }

    static readonly Dictionary<string, ushort> Keys = new()
    {
        ["enter"] = 0x0D, ["esc"] = 0x1B, ["tab"] = 0x09, ["backspace"] = 0x08, ["delete"] = 0x2E, ["insert"] = 0x2D,
        ["space"] = 0x20, ["left"] = 0x25, ["up"] = 0x26, ["right"] = 0x27, ["down"] = 0x28,
        ["home"] = 0x24, ["end"] = 0x23, ["pgup"] = 0x21, ["pgdn"] = 0x22,
        ["win"] = 0x5B, ["ctrl"] = 0x11, ["alt"] = 0x12, ["shift"] = 0x10,
        ["printscreen"] = 0x2C, ["capslock"] = 0x14, ["apps"] = 0x5D,
    };
    static readonly Dictionary<string, ushort> Mods = new() { ["ctrl"] = 0x11, ["alt"] = 0x12, ["shift"] = 0x10, ["win"] = 0x5B };
    static readonly HashSet<ushort> Extended = new() { 0x25, 0x26, 0x27, 0x28, 0x2D, 0x2E, 0x24, 0x23, 0x21, 0x22, 0x5B, 0x5C, 0x5D };

    static ushort KeyVk(string key)
    {
        key = key.ToLowerInvariant();
        if (Keys.TryGetValue(key, out var v)) return v;
        if (key.Length >= 2 && key[0] == 'f' && int.TryParse(key[1..], out var n) && n is >= 1 and <= 12) return (ushort)(0x6F + n);
        if (key.Length == 1 && char.IsAsciiLetterOrDigit(key[0])) return (ushort)char.ToUpperInvariant(key[0]);
        throw new ArgumentException("unknown key: " + key);
    }

    public static void Press(string key, IEnumerable<string>? mods)
    {
        var vk = KeyVk(key);
        var mv = (mods ?? Array.Empty<string>()).Where(Mods.ContainsKey).Select(m => Mods[m]).Distinct().ToList();
        uint Ext(ushort x) => Extended.Contains(x) ? EXT : 0;
        var seq = new List<INPUT>();
        foreach (var m in mv) seq.Add(K(m, 0, Ext(m)));
        seq.Add(K(vk, 0, Ext(vk))); seq.Add(K(vk, 0, Ext(vk) | KEYUP));
        for (int i = mv.Count - 1; i >= 0; i--) seq.Add(K(mv[i], 0, Ext(mv[i]) | KEYUP));
        Send(seq);
    }

    public static void TypeText(string text)
    {
        var seq = new List<INPUT>();
        foreach (var ch in text.Length > 2000 ? text[..2000] : text)
        {
            if (ch == '\r') continue;
            if (ch == '\n') { seq.Add(K(0x0D)); seq.Add(K(0x0D, 0, KEYUP)); continue; }
            seq.Add(K(0, ch, UNI)); seq.Add(K(0, ch, UNI | KEYUP));
        }
        Send(seq);
    }

    public static void Media(string action)
    {
        byte vk = action switch { "volup" => 0xAF, "voldown" => 0xAE, "mute" => 0xAD, "playpause" => 0xB3, "next" => 0xB0, "prev" => 0xB1, _ => throw new ArgumentException("unknown action") };
        keybd_event(vk, 0, 0, 0);
        keybd_event(vk, 0, 2, 0);
    }

    public static readonly string[] MediaActions = { "volup", "voldown", "mute", "playpause", "next", "prev" };

    public static void HandleInput(JsonObject ev, Mon mon, Mon virt)
    {
        string t = ev["type"]?.ToString() ?? "";
        double D(string k) => ev[k] is JsonNode n ? n.GetValue<double>() : 0;
        switch (t)
        {
            case "move": MoveAbs(mon, D("nx"), D("ny")); break;
            case "rel": MoveRel(D("dx"), D("dy"), virt); break;
            case "btn":
                if (ev.ContainsKey("nx") && ev.ContainsKey("ny")) MoveAbs(mon, D("nx"), D("ny"));
                Button(ev["button"]?.ToString() ?? "left", ev["action"]?.ToString() ?? "click");
                break;
            case "scroll": Scroll((int)D("dy"), (int)D("dx")); break;
            case "key": Press(ev["key"]?.ToString() ?? "", (ev["mods"] as JsonArray)?.Select(x => x?.ToString() ?? "")); break;
            case "text": TypeText(ev["text"]?.ToString() ?? ""); break;
            default: throw new ArgumentException("unknown event: " + t);
        }
    }

    // ------------------------------------------------------------ cursor on the frame
    public static void DrawCursor(Graphics g, Mon m, double scale)
    {
        var ci = new CURSORINFO { cbSize = Marshal.SizeOf<CURSORINFO>() };
        if (!GetCursorInfo(ref ci) || (ci.flags & 1) == 0 || ci.hCursor == IntPtr.Zero) return;
        int hx = 0, hy = 0;
        if (GetIconInfo(ci.hCursor, out var ii))
        {
            hx = ii.xHotspot; hy = ii.yHotspot;
            if (ii.hbmMask != IntPtr.Zero) DeleteObject(ii.hbmMask);
            if (ii.hbmColor != IntPtr.Zero) DeleteObject(ii.hbmColor);
        }
        int baseSize = Math.Max(16, GetSystemMetrics(13)); // SM_CXCURSOR
        int size = (int)Math.Max(22, baseSize * scale * 1.5);
        double k = (double)size / baseSize;
        int x = (int)Math.Round((ci.pt.x - m.Left) * scale - hx * k);
        int y = (int)Math.Round((ci.pt.y - m.Top) * scale - hy * k);
        var hdc = g.GetHdc();
        try { DrawIconEx(hdc, x, y, ci.hCursor, size, size, 0, IntPtr.Zero, 3); }
        finally { g.ReleaseHdc(hdc); }
    }
}

/// <summary>Screen capture for one session: reuses buffers and skips unchanged frames.</summary>
public sealed class Grabber : IDisposable
{
    Bitmap? _full, _small;
    byte[] _buf = Array.Empty<byte>();
    ulong _lastKey;
    DateTime _lastSent = DateTime.MinValue;
    readonly MemoryStream _ms = new();
    static readonly ImageCodecInfo Jpeg = ImageCodecInfo.GetImageEncoders().First(c => c.FormatID == ImageFormat.Jpeg.Guid);

    /// <summary>Returns a JPEG, or null if the picture has not changed (and no keepalive is due yet).</summary>
    public byte[]? Capture(int monIndex, int maxW, int quality, bool force = false)
    {
        var mons = Native.Monitors();
        if (monIndex < 0 || monIndex >= mons.Count) monIndex = mons.Count > 1 ? 1 : 0;
        var m = mons[monIndex];
        if (_full == null || _full.Width != m.Width || _full.Height != m.Height)
        {
            _full?.Dispose();
            _full = new Bitmap(m.Width, m.Height, PixelFormat.Format32bppRgb);
        }
        Native.CopyScreen(_full, m.Left, m.Top, m.Width, m.Height);

        double scale = _full.Width > maxW ? (double)maxW / _full.Width : 1.0;
        int w = Math.Max(1, (int)Math.Round(_full.Width * scale)), h = Math.Max(1, (int)Math.Round(_full.Height * scale));
        if (_small == null || _small.Width != w || _small.Height != h)
        {
            _small?.Dispose();
            _small = new Bitmap(w, h, PixelFormat.Format32bppRgb);
        }
        using (var g = Graphics.FromImage(_small))
        {
            g.InterpolationMode = scale < 1 ? InterpolationMode.Bilinear : InterpolationMode.NearestNeighbor;
            g.PixelOffsetMode = PixelOffsetMode.Half;
            g.CompositingMode = CompositingMode.SourceCopy;
            g.DrawImage(_full, new Rectangle(0, 0, w, h), 0, 0, _full.Width, _full.Height, GraphicsUnit.Pixel);
        }

        // change key: content + cursor position
        var bd = _small.LockBits(new Rectangle(0, 0, w, h), ImageLockMode.ReadOnly, PixelFormat.Format32bppRgb);
        ulong key = 1469598103934665603UL;
        try
        {
            int bytes = Math.Abs(bd.Stride) * h;
            if (_buf.Length < bytes) _buf = new byte[bytes];
            Marshal.Copy(bd.Scan0, _buf, 0, bytes);
            var span = System.Runtime.InteropServices.MemoryMarshal.Cast<byte, ulong>(_buf.AsSpan(0, bytes & ~7));
            foreach (var v in span) key = (key ^ v) * 1099511628211UL;
        }
        finally { _small.UnlockBits(bd); }
        var (cx, cy) = Native.CursorPos();
        key = (key ^ (ulong)(cx * 73856093 ^ cy * 19349663)) * 1099511628211UL;
        var now = DateTime.UtcNow;
        if (!force && key == _lastKey && (now - _lastSent).TotalSeconds < 2) return null;
        _lastKey = key; _lastSent = now;

        using (var g = Graphics.FromImage(_small)) Native.DrawCursor(g, m, scale);
        _ms.SetLength(0);
        using var ep = new EncoderParameters(1);
        ep.Param[0] = new EncoderParameter(System.Drawing.Imaging.Encoder.Quality, (long)quality);
        _small.Save(_ms, Jpeg, ep);
        return _ms.ToArray();
    }

    public void ForceNext() => _lastKey = 0;

    public void Dispose() { _full?.Dispose(); _small?.Dispose(); _ms.Dispose(); }

    /// <summary>Single snapshot of all screens (for the Screenshot tab and the chat).</summary>
    public static byte[] Snapshot(bool hq)
    {
        var v = Native.Monitors()[0];
        using var full = new Bitmap(v.Width, v.Height, PixelFormat.Format32bppRgb);
        Native.CopyScreen(full, v.Left, v.Top, v.Width, v.Height);
        int lim = hq ? 2560 : 1920;
        double sc = Math.Min(1.0, (double)lim / Math.Max(v.Width, v.Height));
        int w = (int)(v.Width * sc), h = (int)(v.Height * sc);
        using var outB = new Bitmap(w, h, PixelFormat.Format24bppRgb);
        using (var g = Graphics.FromImage(outB))
        {
            g.InterpolationMode = InterpolationMode.HighQualityBilinear;
            g.DrawImage(full, new Rectangle(0, 0, w, h));
        }
        using var ms = new MemoryStream();
        using var ep = new EncoderParameters(1);
        ep.Param[0] = new EncoderParameter(System.Drawing.Imaging.Encoder.Quality, hq ? 90L : 80L);
        outB.Save(ms, Jpeg, ep);
        return ms.ToArray();
    }
}
