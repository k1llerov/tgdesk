// Lightweight widget layer on top of GDI+: dark theme, rounded cards, gradients, hover animation.
#pragma once
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <algorithm>
using std::min;
using std::max;
#include <gdiplus.h>
#include <string>
#include <vector>
#include <map>
#include <cmath>
using namespace Gdiplus;

namespace ui {

// ------------------------------------------------------------------ theme
inline const Color cBg(255, 14, 17, 30);
inline const Color cSide(255, 10, 12, 23);
inline const Color cCard(255, 22, 26, 45);
inline const Color cCard2(255, 31, 36, 61);
inline const Color cBorder(255, 44, 51, 84);
inline const Color cText(255, 238, 241, 255);
inline const Color cMuted(255, 142, 151, 184);
inline const Color cAcc1(255, 124, 92, 255);
inline const Color cAcc2(255, 34, 211, 238);
inline const Color cGreen(255, 52, 211, 153);
inline const Color cRed(255, 255, 77, 109);
inline const Color cAmber(255, 251, 191, 36);

inline float S = 1.0f;   // DPI scale
inline std::wstring FONT = L"Segoe UI";
inline std::wstring FONT_SEMI = L"Segoe UI Semibold";
inline std::wstring FONT_ICON = L"Segoe MDL2 Assets";
inline std::wstring FONT_MONO = L"Consolas";

inline Color alpha(Color c, int a) { return Color((BYTE)a, c.GetR(), c.GetG(), c.GetB()); }
inline Color lerp(Color a, Color b, float t) {
    t = std::clamp(t, 0.f, 1.f);
    return Color((BYTE)(a.GetA() + (b.GetA() - a.GetA()) * t), (BYTE)(a.GetR() + (b.GetR() - a.GetR()) * t),
                 (BYTE)(a.GetG() + (b.GetG() - a.GetG()) * t), (BYTE)(a.GetB() + (b.GetB() - a.GetB()) * t));
}

// ------------------------------------------------------------------ fonts
inline std::map<long, Font*> fontCache;
inline Font* font(float px, int style = 0, const std::wstring& family = L"") {
    long key = (long)(px * 4) * 64 + style * 8 + (family == FONT_ICON ? 1 : family == FONT_MONO ? 2 : family == FONT_SEMI ? 3 : 0);
    auto it = fontCache.find(key);
    if (it != fontCache.end()) return it->second;
    FontFamily ff(family.empty() ? FONT.c_str() : family.c_str());
    if (!ff.IsAvailable()) { FontFamily fb(L"Segoe UI"); fontCache[key] = new Font(&fb, px * S, style, UnitPixel); return fontCache[key]; }
    Font* f = new Font(&ff, px * S, style, UnitPixel);
    fontCache[key] = f;
    return f;
}
inline void resetFonts() { for (auto& p : fontCache) delete p.second; fontCache.clear(); }

// ------------------------------------------------------------------ geometry
inline void rrPath(GraphicsPath& p, RectF r, float rad) {
    float d = std::min(rad * 2, std::min(r.Width, r.Height));
    if (d <= 0.5f) { p.AddRectangle(r); return; }
    p.AddArc(r.X, r.Y, d, d, 180, 90);
    p.AddArc(r.X + r.Width - d, r.Y, d, d, 270, 90);
    p.AddArc(r.X + r.Width - d, r.Y + r.Height - d, d, d, 0, 90);
    p.AddArc(r.X, r.Y + r.Height - d, d, d, 90, 90);
    p.CloseFigure();
}
inline void fillRR(Graphics& g, RectF r, float rad, Color c) {
    GraphicsPath p; rrPath(p, r, rad);
    SolidBrush b(c);
    g.FillPath(&b, &p);
}
inline void strokeRR(Graphics& g, RectF r, float rad, Color c, float w = 1.f) {
    r.X += 0.5f; r.Y += 0.5f; r.Width -= 1; r.Height -= 1;
    GraphicsPath p; rrPath(p, r, rad);
    Pen pen(c, w);
    g.DrawPath(&pen, &p);
}
inline void gradRR(Graphics& g, RectF r, float rad, Color a, Color b, float angle = 0.f) {
    if (r.Width < 1 || r.Height < 1) return;
    GraphicsPath p; rrPath(p, r, rad);
    LinearGradientBrush br(r, a, b, angle, FALSE);
    g.FillPath(&br, &p);
}
inline void circle(Graphics& g, float cx, float cy, float rad, Color c) {
    SolidBrush b(c);
    g.FillEllipse(&b, cx - rad, cy - rad, rad * 2, rad * 2);
}

// ------------------------------------------------------------------ text
// h: 0 — left, 1 — center, 2 — right; v: 0 — top, 1 — center
inline void text(Graphics& g, const std::wstring& s, RectF r, float px, Color c, int style = 0, int h = 0, int v = 1,
                 const std::wstring& family = L"") {
    StringFormat sf(StringFormatFlagsNoWrap);
    sf.SetAlignment(h == 0 ? StringAlignmentNear : h == 1 ? StringAlignmentCenter : StringAlignmentFar);
    sf.SetLineAlignment(v == 0 ? StringAlignmentNear : StringAlignmentCenter);
    sf.SetTrimming(StringTrimmingEllipsisCharacter);
    SolidBrush b(c);
    g.DrawString(s.c_str(), -1, font(px, style, family), r, &sf, &b);
}
inline void icon(Graphics& g, wchar_t glyph, RectF r, float px, Color c) {
    wchar_t s[2] = {glyph, 0};
    StringFormat sf(StringFormatFlagsNoWrap);
    sf.SetAlignment(StringAlignmentCenter);
    sf.SetLineAlignment(StringAlignmentCenter);
    SolidBrush b(c);
    g.DrawString(s, -1, font(px, 0, FONT_ICON), r, &sf, &b);
}
// multi-line wrapped text, returns its height
inline float textWrap(Graphics& g, const std::wstring& s, RectF r, float px, Color c, int style = 0, bool draw = true) {
    StringFormat sf;
    sf.SetAlignment(StringAlignmentNear);
    sf.SetLineAlignment(StringAlignmentNear);
    RectF out;
    g.MeasureString(s.c_str(), -1, font(px, style), r, &sf, &out);
    if (draw) { SolidBrush b(c); g.DrawString(s.c_str(), -1, font(px, style), r, &sf, &b); }
    return out.Height;
}

// ------------------------------------------------------------------ input and animation
struct Hit { int id; RectF r; };
inline std::vector<Hit> hits;
inline int hot = 0, down = 0;
inline bool animating = false;
inline PointF off(0, 0);       // content → window offset
inline RectF viewClip;         // visible area (window coordinates)
inline std::map<int, float> hv, av;

inline void addHit(int id, RectF r) {
    RectF w(r.X + off.X, r.Y + off.Y, r.Width, r.Height);
    RectF vis;
    if (!RectF::Intersect(vis, w, viewClip)) return;
    hits.push_back({id, vis});
}
inline int hitTest(float x, float y) {
    for (int i = (int)hits.size() - 1; i >= 0; i--)
        if (hits[i].r.Contains(x, y)) return hits[i].id;
    return 0;
}
// smooth approach to the target
inline float approach(std::map<int, float>& m, int key, float target, float speed = 0.22f) {
    float& v = m[key];
    if (std::fabs(v - target) < 0.01f) { v = target; return v; }
    v += (target - v) * speed;
    animating = true;
    return v;
}
inline float hover(int id) { return approach(hv, id, (id == hot || id == down) ? 1.f : 0.f); }

// ------------------------------------------------------------------ widgets
enum BtnKind { Primary, Secondary, Danger, Ghost };

inline void button(Graphics& g, int id, RectF r, const std::wstring& label, BtnKind kind = Secondary, wchar_t glyph = 0, bool enabled = true) {
    float h = enabled ? hover(id) : 0.f;
    float pressed = (enabled && down == id && hot == id) ? 1.f : 0.f;
    float rad = 10 * S;
    Color fg = cText;
    if (!enabled) {
        fillRR(g, r, rad, alpha(cCard2, 120)); fg = alpha(cMuted, 140);
    } else if (kind == Primary) {
        Color a = lerp(cAcc1, Color(255, 150, 120, 255), h), b = lerp(Color(255, 60, 150, 225), cAcc2, h);
        if (pressed) { a = lerp(a, Color(255, 0, 0, 0), 0.2f); b = lerp(b, Color(255, 0, 0, 0), 0.2f); }
        gradRR(g, r, rad, a, b, 0.f);
        fg = Color(255, 255, 255, 255);
    } else if (kind == Danger) {
        fillRR(g, r, rad, lerp(alpha(cRed, 38), alpha(cRed, 90), h));
        strokeRR(g, r, rad, alpha(cRed, 110), 1.f);
        fg = Color(255, 255, 150, 165);
    } else if (kind == Secondary) {
        fillRR(g, r, rad, lerp(cCard2, Color(255, 44, 51, 88), h));
        strokeRR(g, r, rad, lerp(cBorder, cAcc1, h * 0.8f), 1.f);
    } else {
        fillRR(g, r, rad, alpha(cCard2, (int)(h * 255)));
        fg = lerp(cMuted, cText, h);
    }
    RectF tr = r;
    if (glyph) {
        float isz = 16 * S;
        RectF ir(r.X + 12 * S, r.Y, isz + 4 * S, r.Height);
        icon(g, glyph, ir, 15, fg);
        tr.X += 26 * S; tr.Width -= 26 * S;
        text(g, label, tr, 13.5f, fg, 0, 1, 1, FONT_SEMI);
    } else {
        text(g, label, tr, 13.5f, fg, 0, 1, 1, FONT_SEMI);
    }
    if (enabled) addHit(id, r);
}

inline void toggle(Graphics& g, int id, float x, float y, bool on) {
    float w = 44 * S, h = 24 * S;
    RectF r(x, y, w, h);
    float t = approach(av, id, on ? 1.f : 0.f, 0.3f);
    float hv_ = hover(id);
    Color off_ = lerp(Color(255, 52, 58, 92), Color(255, 66, 74, 116), hv_);
    GraphicsPath p; rrPath(p, r, h / 2);
    if (t > 0.01f) {
        LinearGradientBrush br(r, alpha(cAcc1, 255), alpha(Color(255, 60, 150, 225), 255), 0.f, FALSE);
        SolidBrush offb(off_);
        g.FillPath(&offb, &p);
        // smooth gradient fade-in
        Color c1 = lerp(off_, cAcc1, t), c2 = lerp(off_, Color(255, 50, 170, 230), t);
        LinearGradientBrush br2(r, c1, c2, 0.f, FALSE);
        g.FillPath(&br2, &p);
    } else {
        SolidBrush b(off_); g.FillPath(&b, &p);
    }
    float kx = x + 3 * S + t * (w - h);
    circle(g, kx + (h - 6 * S) / 2, y + h / 2, (h - 6 * S) / 2, Color(255, 255, 255, 255));
    addHit(id, RectF(x - 4 * S, y - 4 * S, w + 8 * S, h + 8 * S));
}

inline void segmented(Graphics& g, int idBase, RectF r, const std::vector<std::wstring>& items, int sel) {
    fillRR(g, r, 11 * S, cCard2);
    strokeRR(g, r, 11 * S, cBorder);
    float w = r.Width / items.size();
    float tx = approach(av, idBase + 900, (float)sel, 0.28f);
    RectF pill(r.X + 3 * S + tx * w, r.Y + 3 * S, w - 6 * S, r.Height - 6 * S);
    gradRR(g, pill, 9 * S, cAcc1, Color(255, 60, 150, 225), 0.f);
    for (size_t i = 0; i < items.size(); i++) {
        RectF cell(r.X + i * w, r.Y, w, r.Height);
        float h = hover(idBase + (int)i);
        bool s = (int)i == sel;
        text(g, items[i], cell, 13.f, s ? Color(255, 255, 255, 255) : lerp(cMuted, cText, h), 0, 1, 1, FONT_SEMI);
        addHit(idBase + (int)i, cell);
    }
}

inline void chip(Graphics& g, int id, RectF r, const std::wstring& label, bool selected) {
    float h = hover(id);
    float t = approach(av, id + 5000, selected ? 1.f : 0.f, 0.3f);
    Color bg = lerp(lerp(cCard2, Color(255, 44, 51, 88), h), alpha(cAcc1, 255), t * 0.85f);
    fillRR(g, r, r.Height / 2, bg);
    if (t < 0.5f) strokeRR(g, r, r.Height / 2, cBorder);
    text(g, label, r, 12.5f, lerp(cMuted, Color(255, 255, 255, 255), std::max(t, h * 0.7f)), 0, 1, 1, FONT_SEMI);
    addHit(id, r);
}

inline void card(Graphics& g, RectF r, bool glow = false) {
    fillRR(g, r, 16 * S, cCard);
    strokeRR(g, r, 16 * S, glow ? alpha(cAcc1, 140) : cBorder);
}

inline void bar(Graphics& g, RectF r, float frac, Color a, Color b) {
    frac = std::clamp(frac, 0.f, 1.f);
    fillRR(g, r, r.Height / 2, Color(255, 12, 14, 28));
    if (frac > 0.005f) gradRR(g, RectF(r.X, r.Y, std::max(r.Height, r.Width * frac), r.Height), r.Height / 2, a, b, 0.f);
}

inline void spark(Graphics& g, RectF r, const std::vector<float>& v, Color c) {
    if (v.size() < 2) return;
    std::vector<PointF> pts;
    for (size_t i = 0; i < v.size(); i++)
        pts.push_back(PointF(r.X + r.Width * i / (float)(v.size() - 1), r.Y + r.Height * (1.f - std::clamp(v[i], 0.f, 100.f) / 100.f)));
    std::vector<PointF> poly = pts;
    poly.push_back(PointF(r.X + r.Width, r.Y + r.Height));
    poly.push_back(PointF(r.X, r.Y + r.Height));
    LinearGradientBrush fill(r, alpha(c, 90), alpha(c, 0), 90.f, FALSE);
    g.FillPolygon(&fill, poly.data(), (INT)poly.size());
    Pen pen(c, 1.8f * S);
    pen.SetLineJoin(LineJoinRound);
    g.DrawLines(&pen, pts.data(), (INT)pts.size());
}

// small rounded label, returns its width
inline float badge(Graphics& g, float x, float y, const std::wstring& label, Color c, float h = 24.f) {
    RectF m;
    StringFormat sf; g.MeasureString(label.c_str(), -1, font(12, 0, FONT_SEMI), PointF(0, 0), &sf, &m);
    float w = m.Width + 20 * S, hh = h * S;
    RectF r(x, y, w, hh);
    fillRR(g, r, hh / 2, alpha(c, 38));
    text(g, label, r, 12, c, 0, 1, 1, FONT_SEMI);
    return w;
}

inline void avatar(Graphics& g, float cx, float cy, float rad, const std::wstring& letter, Color a, Color b) {
    RectF r(cx - rad, cy - rad, rad * 2, rad * 2);
    LinearGradientBrush br(r, a, b, 45.f, FALSE);
    g.FillEllipse(&br, r);
    text(g, letter, r, rad * 0.95f / S, Color(255, 255, 255, 255), 0, 1, 1, FONT_SEMI);
}

inline void statusDot(Graphics& g, float cx, float cy, float rad, Color c, bool pulse, double timeSec) {
    if (pulse) {
        float t = (float)std::fmod(timeSec, 1.6) / 1.6f;
        circle(g, cx, cy, rad + rad * 1.6f * t, alpha(c, (int)(110 * (1.f - t))));
        animating = true;
    }
    circle(g, cx, cy, rad, c);
}

} // namespace ui
