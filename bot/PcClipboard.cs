namespace TgDesk;

/// <summary>
/// Text clipboard of this PC. The Windows clipboard needs an STA thread, so every call runs on a short-lived one.
/// Only plain text; the contents are never written to any log (only their length).
/// </summary>
public static class PcClipboard
{
    public const int MaxChars = 100_000;

    static T Sta<T>(Func<T> f)
    {
        T result = default!;
        Exception? error = null;
        var th = new Thread(() =>
        {
            // another program may hold the clipboard open for a moment
            for (int i = 0; ; i++)
            {
                try { result = f(); return; }
                catch (System.Runtime.InteropServices.ExternalException e) when (i < 9) { _ = e; Thread.Sleep(60); }
                catch (Exception e) { error = e; return; }
            }
        }) { IsBackground = true, Name = "tgdesk-clipboard" };
        th.SetApartmentState(ApartmentState.STA);
        th.Start();
        if (!th.Join(TimeSpan.FromSeconds(5))) throw new TimeoutException("clipboard is busy");
        if (error != null) throw error;
        return result;
    }

    /// <summary>Text on the PC clipboard ("" if there is no text), cut to <see cref="MaxChars"/>.</summary>
    public static Task<(string text, bool cut)> Get() => Task.Run(() => Sta(() =>
    {
        var s = Clipboard.ContainsText() ? Clipboard.GetText(TextDataFormat.UnicodeText) : "";
        return s.Length > MaxChars ? (s[..MaxChars], true) : (s, false);
    }));

    public static Task Set(string text)
    {
        if (string.IsNullOrEmpty(text)) throw new ArgumentException("empty text");
        if (text.Length > MaxChars) throw new ArgumentException("text is too long");
        text = text.Replace("\r\n", "\n").Replace("\n", "\r\n");   // Windows line breaks
        return Task.Run(() => Sta(() => { Clipboard.SetText(text, TextDataFormat.UnicodeText); return true; }));
    }
}
