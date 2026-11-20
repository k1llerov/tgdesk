using System.Drawing;
using System.Runtime.InteropServices;
using System.Windows.Forms;

namespace TgDesk;

/// <summary>
/// "Screen is being streamed" badge in the top-left corner of the PC. It is ALWAYS shown while somebody watches the screen
/// and cannot be turned off: TgDesk must never be usable for covert monitoring. It does not take focus, lets clicks through
/// and is excluded from the stream itself (so it does not cover the picture for the viewer).
/// </summary>
public sealed class StreamOverlay
{
    [DllImport("user32.dll")] static extern int GetWindowLong(IntPtr h, int i);
    [DllImport("user32.dll")] static extern int SetWindowLong(IntPtr h, int i, int v);
    [DllImport("user32.dll")] static extern bool SetWindowDisplayAffinity(IntPtr h, uint a);
    [DllImport("dwmapi.dll")] static extern int DwmSetWindowAttribute(IntPtr h, int attr, ref int v, int size);

    readonly Dictionary<string, (string who, DateTime t)> _sessions = new();
    readonly object _l = new();
    OverlayForm? _form;
    Thread? _thread;
    readonly ManualResetEventSlim _ready = new();

    // Failures of the badge must not break streaming (they are logged).
    public void Start(string sid, string who)
    {
        try
        {
            lock (_l) _sessions[sid] = (who, DateTime.Now);
            EnsureThread();
            _ready.Wait(3000);
            Poke();
        }
        catch (Exception ex) { Log.Warn("Overlay: " + ex.Message, "overlay"); }
    }

    public void End(string sid)
    {
        try { lock (_l) _sessions.Remove(sid); Poke(); }
        catch (Exception ex) { Log.Warn("Overlay: " + ex.Message, "overlay"); }
    }

    void Poke()
    {
        var f = _form;
        if (f != null && f.IsHandleCreated) f.BeginInvoke(new Action(() => f.Refresh_()));
    }

    void EnsureThread()
    {
        lock (_l)
        {
            if (_thread != null) return;
            _thread = new Thread(() =>
            {
                try
                {
                    _form = new OverlayForm(this);
                    _ = _form.Handle; // create the window now; it is shown only while a stream is active
                    _ready.Set();
                    Application.Run();
                }
                catch (Exception ex) { Log.Error("Overlay crashed", ex, "overlay"); _ready.Set(); }
            }) { IsBackground = true, Name = "tgdesk-overlay" };
            _thread.SetApartmentState(ApartmentState.STA);
            _thread.Start();
        }
    }

    sealed class OverlayForm : Form
    {
        readonly StreamOverlay _o;
        readonly Label _title = new(), _timer = new(), _sub = new();
        readonly System.Windows.Forms.Timer _tick = new() { Interval = 50 };
        readonly Panel _dot = new();
        double _target = 0, _alpha = 0;
        int _n = 0;
        bool _shown;

        protected override bool ShowWithoutActivation => true;

        protected override CreateParams CreateParams
        {
            get
            {
                var cp = base.CreateParams;
                cp.ExStyle |= 0x00000080 | 0x08000000 | 0x00000020 | 0x00080000; // toolwindow, noactivate, transparent, layered
                return cp;
            }
        }

        public OverlayForm(StreamOverlay o)
        {
            _o = o;
            FormBorderStyle = FormBorderStyle.None;
            ShowInTaskbar = false;
            TopMost = true;
            StartPosition = FormStartPosition.Manual;
            BackColor = Color.FromArgb(20, 24, 38);
            Opacity = 0;
            AutoSize = true; AutoSizeMode = AutoSizeMode.GrowAndShrink;
            Padding = new Padding(12, 8, 12, 8);
            float s = DeviceDpi / 96f;
            Location = new Point((int)(16 * s), (int)(16 * s));

            _dot.Size = new Size((int)(11 * s), (int)(11 * s)); _dot.BackColor = Color.FromArgb(255, 77, 109);
            _dot.Location = new Point(Padding.Left, Padding.Top + (int)(4 * s));
            _dot.Paint += (_, e) => { };
            var rgn = new System.Drawing.Drawing2D.GraphicsPath(); rgn.AddEllipse(0, 0, _dot.Width, _dot.Height); _dot.Region = new Region(rgn);

            _title.Text = L.Pc("TgDesk · трансляция экрана"); _title.ForeColor = Color.White; _title.AutoSize = true;
            _title.Font = new Font("Segoe UI", 10f, FontStyle.Bold);
            _title.Location = new Point(Padding.Left + (int)(20 * s), Padding.Top);
            _timer.ForeColor = Color.FromArgb(34, 211, 238); _timer.AutoSize = true; _timer.Font = new Font("Segoe UI Semibold", 10f);
            _timer.Location = new Point(_title.Right + (int)(150 * s), Padding.Top);
            _sub.ForeColor = Color.FromArgb(154, 163, 189); _sub.AutoSize = true; _sub.Font = new Font("Segoe UI", 9f);
            _sub.Location = new Point(Padding.Left + (int)(20 * s), Padding.Top + (int)(24 * s));
            Controls.AddRange(new Control[] { _dot, _title, _timer, _sub });
            _tick.Tick += (_, _) => Tick();
            _tick.Start();
        }

        protected override void OnHandleCreated(EventArgs e)
        {
            base.OnHandleCreated(e);
            try
            {
                SetWindowDisplayAffinity(Handle, 0x11); // WDA_EXCLUDEFROMCAPTURE
                int pref = 2; DwmSetWindowAttribute(Handle, 33, ref pref, sizeof(int)); // rounded corners
            }
            catch { }
        }

        public void Refresh_()
        {
            bool any; lock (_o._l) any = _o._sessions.Count > 0;
            _target = any ? 0.94 : 0;
            if (any && !_shown)
            {
                _shown = true;
                Show();
                TopMost = true;
            }
        }

        void Tick()
        {
            if (Math.Abs(_alpha - _target) > 0.02)
            {
                _alpha = Math.Clamp(_alpha + (_target > _alpha ? 0.12 : -0.08), 0, 0.94);
                Opacity = _alpha;
            }
            else if (_target == 0 && _shown) { Opacity = 0; _alpha = 0; Hide(); _shown = false; }

            List<(string who, DateTime t)> list;
            lock (_o._l) list = _o._sessions.Values.ToList();
            if (list.Count == 0) return;
            var sec = (int)(DateTime.Now - list.Min(x => x.t)).TotalSeconds;
            _timer.Text = (sec >= 3600 ? $"{sec / 3600}:" : "") + $"{sec / 60 % 60:00}:{sec % 60:00}";
            _sub.Text = list.Count == 1 ? list[0].who : L.PcF("Сеансов: {0}", list.Count) + "\n" + string.Join("\n", list.Select(x => x.who));
            _timer.Left = _title.Right + 10;
            _n++;
            _dot.BackColor = (_n / 12) % 2 == 0 ? Color.FromArgb(255, 77, 109) : Color.FromArgb(122, 42, 58);
        }
    }
}
