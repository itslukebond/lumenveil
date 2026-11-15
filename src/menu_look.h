// The desktop Insert menu's and the FPS counter's look, after FFXV's own menus: a near-black panel fading to the right
// with a blue light streak along the top and thin corner brackets, section names in spaced capitals, a pointing glove
// cursor on the selected row, options as ‹ value ›, square toggles that animate, hairline sliders with a diamond knob, and the
// counter's frame time graph, and the layout button and page tabs. Dear ImGui's own widgets keep focus, navigation, clicks, lists and typing; this draws
// around them. Free of Windows types so tests/menu_look_test.cpp covers it on the host. Sizes are in ctx().s units;
// colours are sRGB and always go through ImGui::GetColorU32, so a disabled row fades with it.
#pragma once
#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstring>
#include "imgui.h"
#include "imgui_internal.h"  // the nav rectangle and cursor, item flags, RenderTextEllipsis
#include "menu_pad.h"

namespace look {

constexpr ImVec4 rgba(int r, int g, int b, float a) { return ImVec4(r / 255.f, g / 255.f, b / 255.f, a); }
constexpr ImVec4 kInk = rgba(0xee, 0xf1, 0xf4, 1), kInk2 = rgba(0xb9, 0xc2, 0xcc, 1), kDim = rgba(0x85, 0x90, 0x9c, 1),
                 kRule = rgba(205, 215, 228, .22f), kStreak = rgba(0x4f, 0x8d, 0xff, 1), kStreakHi = rgba(0xe4, 0xef, 0xff, 1),
                 kSel = rgba(110, 160, 255, .20f), kGo = rgba(0x9f, 0xd4, 0x7a, 1), kInfo = rgba(0x8d, 0x99, 0xa6, 1),
                 kClear = ImVec4(0, 0, 0, 0), kWhite = ImVec4(1, 1, 1, 1);
inline ImU32 col(const ImVec4& c) { return ImGui::GetColorU32(c); }
inline ImVec4 alpha(ImVec4 c, float a) { c.w *= a; return c; }

// The scale, the small font (notes, units, the counter's lines) and the row being drawn. Present thread only.
struct Ctx {
    float s = 1;
    ImFont* fontSmall = nullptr;  // not "small": a Windows header defines it
    bool rowSel = false;  // the row opened last is the selected one
    float widgetX = 0;    // where the current row's widget starts (screen x)
    float bob = 0;        // the glove's sway this frame (0..2 design px), set once a frame from the clock, never in row()
};
inline Ctx& ctx() { static Ctx c; return c; }
inline ImFont* smallFont() { return ctx().fontSmall ? ctx().fontSmall : ImGui::GetFont(); }

// ---- pure helpers ------------------------------------------------------------------------------------------------
// The value d steps away from i among n, wrapping at both ends.
inline int stepWrap(int i, int d, int n) { return n > 0 ? ((i + d) % n + n) % n : 0; }

// A toggle's inner square and pulse ring at `age` seconds after the value changed.
struct Anim { float scale, alpha; };
inline Anim toggleInner(bool on, float age) {
    if (on) {  // grows in from 0.2 with a slight overshoot (ease-out-back) over 0.18 s, fading in over 0.12 s
        const float t = std::clamp(age / 0.18f, 0.f, 1.f), u = t - 1, c1 = 1.70158f, c3 = c1 + 1;
        const float back = 1 + c3 * u * u * u + c1 * u * u;
        const float f = std::clamp(age / 0.12f, 0.f, 1.f);
        return { 0.2f + 0.8f * back, 1 - (1 - f) * (1 - f) };
    }
    const float t = std::clamp(age / 0.12f, 0.f, 1.f), e = 1 - (1 - t) * (1 - t);  // shrinks out, ease-out
    return { 1 - 0.8f * e, 1 - e };
}
inline Anim pulse(float age) {  // 1 to 1.9 and 0.9 to 0 over 0.42 s, ease-out
    const float t = std::clamp(age / 0.42f, 0.f, 1.f), e = 1 - (1 - t) * (1 - t);
    return { 1 + 0.9f * e, 0.9f * (1 - e) };
}
// Seconds since toggle `id` last changed, kept in `st`. The first sight of an id (the menu opening) reads as long ago.
constexpr float kStill = 1000;
inline float toggleAge(ImGuiStorage& st, ImGuiID id, bool on, float now) {
    const ImGuiID kv = ImHashData(&id, sizeof id, 0x76a1u), kt = ImHashData(&id, sizeof id, 0x7713u);
    const int seen = st.GetInt(kv, 0), want = on ? 2 : 1;
    if (!seen) { st.SetInt(kv, want); st.SetFloat(kt, now - kStill); return kStill; }
    if (seen != want) { st.SetInt(kv, want); st.SetFloat(kt, now); }
    return now - st.GetFloat(kt, now - kStill);
}

// The glove's sway at t seconds: 0 to 2 px and back every 1.1 s, eased at both turns.
inline float bobX(double t) { return float(1 - std::cos(6.283185307179586 * t / 1.1)); }

// The graph's vertical scale eased toward `want` with a 0.2 s time constant, the same whatever the frame rate.
inline float easeScale(float top, float want, float dt) { return top + (want - top) * (1 - std::exp(-dt / 0.2f)); }

// Presented frames over the last 2.1 s, one sample per presented frame: a rendered frame that produced n frames adds n
// samples of interval / n, evenly spaced in time. Written and read on the same thread.
struct FrameRing {
    static constexpr int kCap = 1024;
    static constexpr double kKeep = 2.1;
    double t[kCap] = {};
    float v[kCap] = {};
    int head = 0, count = 0;  // head: the oldest
    double prev = 0;
    bool started = false;
    void record(double now, unsigned shown) {
        if (!started) { prev = now; started = true; return; }
        if (!shown) return;
        const double dt = now - prev;
        for (unsigned k = 1; k <= shown; ++k) {
            const int i = (head + count) % kCap;
            t[i] = prev + dt * k / shown; v[i] = float(dt * 1000 / shown);
            if (count < kCap) ++count; else head = (head + 1) % kCap;
        }
        prev = now;
        while (count && t[head] < now - kKeep) { head = (head + 1) % kCap; --count; }
    }
    int size() const { return count; }
    double time(int i) const { return t[(head + i) % kCap]; }  // 0 = oldest
    float ms(int i) const { return v[(head + i) % kCap]; }
};
// x of a sample taken at t, redrawn at now: its age over a 2 s window, now at the right edge.
inline float ageX(double now, double t, float left, float width) { return float(left + width - (now - t) / 2.0 * width); }
// The frame time under a number: 1000 / fps, one decimal; nothing before the first count.
inline void msText(char* buf, size_t n, float fps) {
    if (fps > 0) std::snprintf(buf, n, "%.1f ms", 1000.0 / fps);
    else if (n) *buf = 0;
}
// The counter window's width: the fixed number slots or the widest info line, plus 18 px padding a side, at least 230.
inline float counterWidth(float s, float numbersW, float widestInfo) {
    return std::max(230 * s, std::max(numbersW, widestInfo) + 36 * s);
}

// ---- drawing ---------------------------------------------------------------------------------------------------------
struct Stop { float at; ImVec4 c; };
// A horizontal gradient through `stops` (0..1 across x0..x1), alpha from a top/bottom factor for vertical fades.
inline void hgrad(ImDrawList* dl, float x0, float x1, float y0, float y1, const Stop* stops, int n, float topK = 1, float botK = 1) {
    for (int i = 0; i + 1 < n; ++i) {
        const float a = x0 + (x1 - x0) * stops[i].at, b = x0 + (x1 - x0) * stops[i + 1].at;
        if (b <= a) continue;
        const ImU32 lt = col(alpha(stops[i].c, topK)), rt = col(alpha(stops[i + 1].c, topK)),
                    lb = col(alpha(stops[i].c, botK)), rb = col(alpha(stops[i + 1].c, botK));
        dl->AddRectFilledMultiColor(ImVec2(a, y0), ImVec2(b, y1), lt, rt, rb, lb);
    }
}
inline void fadeRule(ImDrawList* dl, float x0, float x1, float y, const ImVec4& c, float th = 1) {
    const Stop st[] = { { 0, c }, { 1, alpha(c, 0) } };
    hgrad(dl, x0, x1, y, y + th, st, 2);
}
inline void diamond(ImDrawList* dl, ImVec2 c, float half, const ImVec4& fill, float glow) {
    if (glow > 0) {
        const ImVec4 g = rgba(150, 190, 255, 0.35f * fill.w);
        dl->AddQuadFilled(ImVec2(c.x, c.y - half - glow), ImVec2(c.x + half + glow, c.y), ImVec2(c.x, c.y + half + glow),
                          ImVec2(c.x - half - glow, c.y), col(g));
    }
    dl->AddQuadFilled(ImVec2(c.x, c.y - half), ImVec2(c.x + half, c.y), ImVec2(c.x, c.y + half), ImVec2(c.x - half, c.y), col(fill));
}
// ‹ (dir -1) or › (dir 1), 4 x 8 at the scale (or at `scale` when given), 1.5 thick: the point toward dir.
inline void chevron(ImDrawList* dl, ImVec2 c, int dir, const ImVec4& colr, float scale = 0) {
    const float s = scale > 0 ? scale : ctx().s, w = -2 * s * dir, h = 4 * s;
    const ImVec2 pts[3] = { ImVec2(c.x + w, c.y - h), ImVec2(c.x - w, c.y), ImVec2(c.x + w, c.y + h) };
    dl->AddPolyline(pts, 3, col(colr), ImDrawFlags_None, 1.5f * s);
}
// Text with extra space after every character (spaced capitals); returns its width.
inline float spacedText(ImDrawList* dl, ImFont* f, float size, ImVec2 p, const char* text, float tracking, const ImVec4& c) {
    float x = p.x;
    for (const char* t = text; *t;) {
        unsigned ch = 0;
        const int n = ImTextCharFromUtf8(&ch, t, nullptr);
        if (n <= 0) break;
        if (dl) dl->AddText(f, size, ImVec2(x, p.y), col(c), t, t + n);
        x += f->CalcTextSizeA(size, FLT_MAX, 0, t, t + n).x + tracking * size;
        t += n;
    }
    return x - p.x - (x > p.x ? tracking * size : 0);
}

// Our own pointing glove, drawn from rounded rectangles and one line on a 40 x 24 grid scaled by `unit` from top left
// `o`: cuff, palm, index finger, two curled fingers and thumb, each filled then outlined, then the line across the cuff.
inline void glove(ImDrawList* dl, ImVec2 o, float unit) {
    struct Part { float x, y, w, h, r; ImVec4 c; };
    static const Part parts[] = {
        { 1.5f, 6.5f, 6.5f, 12, 1.4f, rgba(0xcf, 0xd8, 0xe4, 1) },   // cuff
        { 6.5f, 5, 15, 15.5f, 5.5f, rgba(0xff, 0xff, 0xff, 1) },     // palm
        { 15.5f, 6, 23, 5.4f, 2.7f, rgba(0xff, 0xff, 0xff, 1) },     // index finger
        { 15.5f, 11.6f, 10, 4.2f, 2.1f, rgba(0xf2, 0xf5, 0xfa, 1) }, // curled finger
        { 15, 15.6f, 9, 4.2f, 2.1f, rgba(0xe6, 0xeb, 0xf2, 1) },     // curled finger
        { 9, 3.6f, 10.5f, 4.6f, 2.3f, rgba(0xff, 0xff, 0xff, 1) },   // thumb
    };
    const ImU32 outline = col(rgba(0x26, 0x30, 0x3d, 1));
    for (const Part& q : parts) {
        const ImVec2 a(o.x + q.x * unit, o.y + q.y * unit), b(o.x + (q.x + q.w) * unit, o.y + (q.y + q.h) * unit);
        dl->AddRectFilled(a, b, col(q.c), q.r * unit);
        dl->AddRect(a, b, outline, q.r * unit, 0, 1.1f * unit);
    }
    dl->AddLine(ImVec2(o.x + 3.4f * unit, o.y + 9 * unit), ImVec2(o.x + 3.4f * unit, o.y + 16 * unit), col(rgba(0x9a, 0xa7, 0xb8, 1)), unit);
}

// The panel: a near-black gradient fading to the right, the light streak with its glow along the top, and for the menu
// a faint line along the bottom. Square corners.
enum Kind { kMenu, kCounter };
// The panel's fill ends at fillEnd (default b.x), so a scrollbar Begin drew before the panel stays visible; the streak,
// its glow and the bottom line still span a..b.
inline void panel(ImDrawList* dl, ImVec2 a, ImVec2 b, Kind k, float fillEnd = FLT_MAX) {
    const float s = ctx().s;
    const Stop menuBg[] = { { 0, rgba(4, 6, 9, .90f) }, { .65f, rgba(4, 6, 9, .86f) }, { 1, rgba(4, 6, 9, .74f) } };
    const Stop ctrBg[] = { { 0, rgba(4, 6, 9, .78f) }, { .70f, rgba(4, 6, 9, .66f) }, { 1, rgba(4, 6, 9, .42f) } };
    const float fx = std::min(fillEnd, b.x);
    if (fx < b.x) {  // the scrollbar's gutter: the fill's gradient as it would be over the whole width, cut short
        const float t = (fx - a.x) / (b.x - a.x);
        const Stop cut[] = { { 0, rgba(4, 6, 9, .90f) }, { std::min(.65f / t, 1.f), rgba(4, 6, 9, .86f) }, { 1, rgba(4, 6, 9, .86f - .12f * std::max(0.f, t - .65f) / .35f) } };
        if (k == kMenu) hgrad(dl, a.x, fx, a.y, b.y, cut, 3);
        else hgrad(dl, a.x, fx, a.y, b.y, ctrBg, 3);
    } else hgrad(dl, a.x, b.x, a.y, b.y, k == kMenu ? menuBg : ctrBg, 3);
    const ImVec4 tail = rgba(79, 141, 255, k == kMenu ? .25f : .2f);
    const Stop menuSt[] = { { 0, alpha(kStreak, 0) }, { .18f, kStreak }, { .34f, kStreakHi }, { .52f, kStreak }, { .80f, tail }, { 1, alpha(tail, 0) } };
    const Stop ctrSt[] = { { 0, alpha(kStreak, 0) }, { .15f, kStreak }, { .32f, kStreakHi }, { .52f, kStreak }, { .85f, tail }, { 1, alpha(tail, 0) } };
    const Stop* st = k == kMenu ? menuSt : ctrSt;
    // the glow: the streak's blue, faded out over 3 px above and below
    const ImVec4 glow = rgba(79, 141, 255, k == kMenu ? .55f : .5f);
    const Stop gl[] = { { 0, alpha(glow, 0) }, { .1f, glow }, { .9f, glow }, { 1, alpha(glow, 0) } };
    hgrad(dl, a.x, b.x, a.y - 3 * s, a.y, gl, 4, 0, 0.45f);
    hgrad(dl, a.x, b.x, a.y + 2 * s, a.y + 5 * s, gl, 4, 0.45f, 0);
    hgrad(dl, a.x, b.x, a.y, a.y + 2 * s, st, 6);
    if (k == kMenu) {
        const ImVec4 line = rgba(160, 190, 230, .35f);
        const Stop bot[] = { { 0, alpha(line, 0) }, { .30f, line }, { .90f, alpha(line, 0) }, { 1, alpha(line, 0) } };
        hgrad(dl, a.x, b.x, b.y - s, b.y, bot, 4);
    }
}
// Where the menu's panel fill ends: the left edge of the window's scrollbar when it has one, else its right edge.
inline float panelEnd(ImGuiWindow* win) {
    return win->ScrollbarY ? ImGui::GetWindowScrollbarRect(win, ImGuiAxis_Y).Min.x : win->Pos.x + win->Size.x;
}
// Thin corner brackets: the menu's four (16 px arms, 8 px in, or armPx / inPx when given), the counter's top left and
// bottom right (9 px, 4 px in).
inline void brackets(ImDrawList* dl, ImVec2 a, ImVec2 b, Kind k, float armPx = 0, float inPx = 0) {
    const float s = ctx().s, arm = (armPx > 0 ? armPx : k == kMenu ? 16 : 9) * s, in = (inPx > 0 ? inPx : k == kMenu ? 8 : 4) * s, th = s;
    const ImU32 c = col(rgba(205, 215, 228, .55f));
    auto corner = [&](float x, float y, float dx, float dy) {  // (x, y) the corner, (dx, dy) toward the inside
        dl->AddRectFilled(ImVec2(std::min(x, x + dx * arm), dy > 0 ? y : y - th), ImVec2(std::max(x, x + dx * arm), dy > 0 ? y + th : y), c);
        dl->AddRectFilled(ImVec2(dx > 0 ? x : x - th, std::min(y, y + dy * arm)), ImVec2(dx > 0 ? x + th : x, std::max(y, y + dy * arm)), c);
    };
    corner(a.x + in, a.y + in + (k == kCounter ? s : 0), 1, 1);
    corner(b.x - in, b.y - in, -1, -1);
    if (k == kMenu) { corner(b.x - in, a.y + in, -1, 1); corner(a.x + in, b.y - in, 1, -1); }
}

// ---- widgets ---------------------------------------------------------------------------------------------------------
// A section heading (the role of ImGui's SeparatorText): spaced capitals at 12 px in ink2, 18 px above and 6 px below,
// then a rule that fades out toward the right edge.
inline void SeparatorText(const char* label) {
    const float s = ctx().s, sp = ImGui::GetStyle().ItemSpacing.y, size = 12 * s;
    char up[64]; size_t n = 0;
    for (const char* p = label; *p && n + 1 < sizeof up; ++p) up[n++] = char(*p >= 'a' && *p <= 'z' ? *p - 32 : *p);
    up[n] = 0;
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + std::max(0.f, 18 * s - sp));
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float avail = ImGui::GetContentRegionAvail().x;
    ImGui::Dummy(ImVec2(avail, size));
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float w = spacedText(dl, smallFont(), size, p, up, 0.22f, kInk2);
    fadeRule(dl, p.x + w + 12 * s, p.x + avail, std::floor(p.y + size * 0.55f), kRule);
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() - sp + 6 * s);
}

// Opens a row at the cursor: it is the selected row when it holds the nav rectangle while the nav cursor shows, else
// when the mouse is over it. The selected row gets a blue band fading to the right, the pointing glove at its left
// (swaying by ctx().bob) and a glowing underline, drawn now (behind the row's widgets), where the row is: nothing moves between rows.
inline void row() {
    ImGuiContext& g = *GImGui;
    ImGuiWindow* w = g.CurrentWindow;
    const float s = ctx().s, sp = ImGui::GetStyle().ItemSpacing.y;
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const ImRect r(p.x, p.y - sp * 0.5f, p.x + ImGui::GetContentRegionAvail().x, p.y + ImGui::GetFrameHeight() + sp * 0.5f);
    bool sel;
    if (g.NavCursorVisible && g.NavWindow == w && g.NavId) {
        const ImRect n = ImGui::WindowRectRelToAbs(w, w->NavRectRel[0]);
        const float cy = (n.Min.y + n.Max.y) * 0.5f;
        sel = cy >= r.Min.y && cy < r.Max.y;
    } else sel = ImGui::IsWindowHovered() && r.Contains(ImGui::GetIO().MousePos);
    ctx().rowSel = sel;
    if (!sel) return;
    ImDrawList* dl = w->DrawList;
    const Stop band[] = { { 0, kSel }, { .55f, rgba(110, 160, 255, .07f) }, { 1, rgba(110, 160, 255, 0) } };
    hgrad(dl, r.Min.x, r.Max.x, r.Min.y, r.Max.y, band, 3);
    const float ux = r.Min.x + (r.Max.x - r.Min.x) * 0.8f, uy = r.Max.y - s;
    const ImVec4 under = rgba(0xa9, 0xc8, 0xff, 1), glow = rgba(120, 170, 255, .7f);
    const Stop gl[] = { { 0, alpha(glow, .5f) }, { 1, alpha(glow, 0) } };
    hgrad(dl, r.Min.x, ux, uy - 3 * s, uy, gl, 2, 0, 0.6f);
    hgrad(dl, r.Min.x, ux, uy + s, uy + 3 * s, gl, 2, 0.6f, 0);
    fadeRule(dl, r.Min.x, ux, uy, under, s);
    glove(dl, ImVec2(r.Min.x - 6 * s + ctx().bob * s, (r.Min.y + r.Max.y) * 0.5f - 7.2f * s), 0.6f * s);
}

// The ‹ value › frame of an option row: ‹ and › in 22 px slots at the ends (mouse only: never navigation stops), and a
// combo over the middle with the value centred (cut with an ellipsis when too long). Returns -1 or 1 on a click on an
// arrow, or on left / right (keyboard or D-pad, once per press) while the combo has the focus and its list is closed,
// else 0. *open is BeginCombo's answer: the caller lists the values, then calls ImGui::EndCombo.
inline int optionFrame(const char* id, const char* value, bool* open, const char* rest = nullptr) {
    const float s = ctx().s, a = 22 * s, w = ImGui::CalcItemWidth(), h = ImGui::GetFrameHeight();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const bool disabled = (GImGui->CurrentItemFlags & ImGuiItemFlags_Disabled) != 0;
    int d = 0;
    ImGui::PushID(id);
    ImGui::PushItemFlag(ImGuiItemFlags_NoNav, true);
    if (ImGui::InvisibleButton("##prev", ImVec2(a, h), ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_NoNavFocus)) d = -1;
    const bool hotPrev = ImGui::IsItemHovered();
    ImGui::SetCursorScreenPos(ImVec2(p.x + w - a, p.y));
    if (ImGui::InvisibleButton("##next", ImVec2(a, h), ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_NoNavFocus)) d = 1;
    const bool hotNext = ImGui::IsItemHovered();
    ImGui::PopItemFlag();
    ImGui::PopID();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec4 dim = ctx().rowSel ? kWhite : kDim;
    chevron(dl, ImVec2(p.x + a * 0.5f, p.y + h * 0.5f), -1, hotPrev ? kWhite : dim);
    chevron(dl, ImVec2(p.x + w - a * 0.5f, p.y + h * 0.5f), 1, hotNext ? kWhite : dim);
    // the value, centred in the middle slot, drawn before the combo (whose own frame is transparent)
    const ImVec2 v0(p.x + a, p.y), v1(p.x + w - a, p.y + h);
    char tail[48] = "";
    if (rest && *rest) std::snprintf(tail, sizeof tail, " \xC2\xB7 %s", rest);  // " · DLSS", dim
    const float pad = ImGui::GetStyle().FramePadding.x, mw = ImGui::CalcTextSize(value).x, tw = mw + ImGui::CalcTextSize(tail).x;
    const float tx = std::max(v0.x + pad, (v0.x + v1.x - tw) * 0.5f), ty = p.y + ImGui::GetStyle().FramePadding.y;
    ImGui::PushStyleColor(ImGuiCol_Text, kInk);
    ImGui::RenderTextEllipsis(dl, ImVec2(tx, ty), ImVec2(v1.x - pad, v1.y), v1.x - pad, v1.x - pad, value, nullptr, nullptr);
    ImGui::PopStyleColor();
    if (*tail) dl->AddText(ImVec2(tx + mw, ty), col(kDim), tail);
    ImGui::SetCursorScreenPos(v0);
    ImGui::SetNextItemWidth(v1.x - v0.x);
    *open = ImGui::BeginCombo(id, "", ImGuiComboFlags_NoArrowButton | ImGuiComboFlags_HeightLarge);
    if (!*open && !disabled && ImGui::IsItemFocused() && !ImGui::IsItemActive()) d = d ? d : pad::padLeftRight(false);
    return disabled ? 0 : d;
}
// ImGui::Combo's contract drawn as ‹ value ›: true when *cur changed (a pick from the list, or a step with wrap-around).
inline bool option(const char* id, int* cur, const char* const items[], int n, const char* shown = nullptr, const char* rest = nullptr) {
    bool open = false, changed = false;
    const int d = optionFrame(id, shown ? shown : *cur >= 0 && *cur < n ? items[*cur] : "", &open, rest);
    if (open) {
        for (int i = 0; i < n; ++i) {
            if (ImGui::Selectable(items[i], i == *cur) && i != *cur) { *cur = i; changed = true; }
            if (i == *cur) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
    if (d && n > 1) { *cur = stepWrap(*cur, d, n); changed = true; }
    return changed;
}

// Called right after a tick box (and its pad tick): draws the square box, the inner square that grows in with a glow
// ring pulsing outward when switched on and shrinks out when switched off, and the word On or Off beside it.
inline void toggleLook(bool on) {
    const float s = ctx().s;
    const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
    const float age = toggleAge(*ImGui::GetStateStorage(), ImGui::GetItemID(), on, float(ImGui::GetTime()));
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float half = 8 * s, cy = std::floor((a.y + b.y) * 0.5f);
    const ImVec2 c(a.x + half, cy);
    dl->AddRect(ImVec2(c.x - half, c.y - half), ImVec2(c.x + half, c.y + half), col(on ? rgba(0xcf, 0xe0, 0xff, 1) : rgba(220, 230, 240, .7f)), 0, 0, s);
    const Anim in = toggleInner(on, age);
    if (in.alpha > 0.001f) {
        const float q = 4 * s * in.scale;
        dl->AddRectFilled(ImVec2(c.x - q - 2 * s, c.y - q - 2 * s), ImVec2(c.x + q + 2 * s, c.y + q + 2 * s), col(rgba(150, 190, 255, .25f * in.alpha)));
        dl->AddRectFilled(ImVec2(c.x - q, c.y - q), ImVec2(c.x + q, c.y + q), col(alpha(kWhite, in.alpha)));
    }
    if (on) {
        const Anim p = pulse(age);
        if (p.alpha > 0.001f) {
            const float r = (half + s) * p.scale;
            dl->AddRect(ImVec2(c.x - r, c.y - r), ImVec2(c.x + r, c.y + r), col(rgba(140, 185, 255, p.alpha)), 0, 0, s);
        }
    }
    const char* word = on ? "On" : "Off";
    dl->AddText(ImVec2(c.x + half + 10 * s, cy - ImGui::GetFontSize() * 0.5f), col(kInk2), word);
}

// Right after an item: it takes the keyboard and controller focus (without being pressed), with the nav cursor shown
// when `cursor`, and is scrolled into view.
inline void focusItem(bool cursor) {
    ImGuiContext& g = *GImGui;
    ImGui::SetWindowFocus();
    ImGui::SetFocusID(g.LastItemData.ID, g.CurrentWindow);
    g.NavInitRequest = false; g.NavInitResult.ID = 0;  // a window just appearing asks for its first item: not this time
    ImGui::SetNavCursorVisible(cursor);
    if (cursor) g.NavHighlightItemUnderNav = true;
    ImGui::ScrollToItem();
}
// Called right after the desktop layout button: when the desktop menu has just taken over from the Portable layout
// (pending), the button takes the focus, its nav cursor shown when a controller has been used (cursor).
inline void takeFocus(bool& pending, bool cursor) {
    if (!pending) return;
    pending = false;
    focusItem(cursor);
}

// A typed slider's look, called right after pad::typedSlider: the hairline track with its fill and diamond knob over
// the slider (from the row's widget start to the box), and the box's frame (blue while typed in).
inline void sliderLook(int v, int lo, int hi) {
    const float s = ctx().s;
    const ImVec2 b0 = ImGui::GetItemRectMin(), b1 = ImGui::GetItemRectMax();
    const bool typing = ImGui::IsItemActive();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    // the box
    dl->AddRectFilled(b0, b1, col(rgba(255, 255, 255, .04f)), 2 * s);
    dl->AddRect(b0, b1, col(typing ? rgba(0x8f, 0xb4, 0xff, 1) : kRule), 2 * s, 0, s);
    // the track, where the slider puts its grab's centre
    const float grab = ImGui::GetStyle().GrabMinSize, x0 = ctx().widgetX + 2 + grab * 0.5f,
                x1 = b0.x - ImGui::GetStyle().ItemSpacing.x - 2 - grab * 0.5f, cy = std::floor((b0.y + b1.y) * 0.5f);
    if (x1 <= x0) return;
    const float t = hi > lo ? std::clamp(float(v - lo) / float(hi - lo), 0.f, 1.f) : 0, kx = x0 + (x1 - x0) * t;
    dl->AddRectFilled(ImVec2(x0, cy - s), ImVec2(x1, cy + s), col(rgba(205, 215, 228, .25f)));
    if (kx > x0) {
        const Stop fill[] = { { 0, rgba(0x6f, 0x9d, 0xff, 1) }, { 1, rgba(0xe4, 0xef, 0xff, 1) } };
        hgrad(dl, x0, kx, cy - s, cy + s, fill, 2);
    }
    diamond(dl, ImVec2(kx, cy), 7.07f * s, kWhite, 2 * s);
}

// Text smaller than the small font (px design pixels, times the menu's scale) in the current text colour, wrapped at
// the content region's width with lines 1.5 times the size apart; it takes its height in the layout, as TextWrapped
// does.
inline void fineText(const char* t, float px) {
    ImFont* f = smallFont();
    const float size = px * ctx().s, w = std::max(1.0f, ImGui::GetContentRegionAvail().x), step = std::round(size * 1.5f);
    const ImVec2 at = ImGui::GetCursorScreenPos();
    const char* end = t + std::strlen(t);
    int lines = 0;
    for (const char* p = t; p < end; ++lines) {
        const char* e = f->CalcWordWrapPositionA(size / f->FontSize, p, end, w);
        if (e == p) ++e;  // a word wider than the line: at least one character, so the loop always moves
        ImGui::GetWindowDrawList()->AddText(f, size, ImVec2(at.x, at.y + lines * step), ImGui::GetColorU32(ImGuiCol_Text), p, e);
        p = e;
        while (p < end && (*p == ' ' || *p == '\n')) ++p;
    }
    ImGui::Dummy(ImVec2(w, lines ? (lines - 1) * step + size : 0));
}
// A key hint in the footer: the key in a small bordered badge, then its word. Returns the x after the word.
inline float keyHint(ImDrawList* dl, float x, float y, float h, const char* badge, const char* word) {
    const float s = ctx().s, size = 11 * s;
    ImFont* f = smallFont();
    const float tw = f->CalcTextSizeA(size, FLT_MAX, 0, badge).x, bw = std::max(20 * s, tw + 10 * s);
    dl->AddRect(ImVec2(x, y), ImVec2(x + bw, y + h), col(rgba(205, 215, 228, .5f)), 2 * s, 0, s);
    dl->AddText(f, size, ImVec2(x + (bw - tw) * 0.5f, y + (h - size) * 0.5f), col(kWhite), badge);
    x += bw + 6 * s;
    dl->AddText(f, f->FontSize, ImVec2(x, y + (h - f->FontSize) * 0.5f), col(kInk2), word);
    return x + f->CalcTextSizeA(f->FontSize, FLT_MAX, 0, word).x;
}

// Text wrapped at width w in font f at `size`, one line every `step`, at most maxLines lines (the rest is left out).
// Returns the lines it needs, drawn or not; measures only when dl is null.
inline int wrapText(ImDrawList* dl, ImFont* f, float size, ImVec2 p, float w, float step, const char* t, const ImVec4& c, int maxLines) {
    const char* end = t + std::strlen(t);
    int n = 0;
    while (t < end) {
        const char* brk = f->CalcWordWrapPositionA(size / f->FontSize, t, end, w);
        if (brk == t) brk = t + 1;  // a word wider than the line: at least one character
        if (dl && n < maxLines) dl->AddText(f, size, ImVec2(p.x, p.y + n * step), col(c), t, brk);
        ++n;
        t = brk;
        while (t < end && (*t == ' ' || *t == '\n')) ++t;
    }
    return n;
}

// ---- the layout button and the page tabs (both layouts) ----------------------------------------------------------------
// The two icons, drawn with the draw list in a box of height h at x (top y): a monitor on its stand, and a handheld with
// its screen and two buttons. Returns the width used.
inline float monitorIcon(ImDrawList* dl, float x, float y, float h, const ImVec4& c, float th) {
    const float w = h * 1.25f, sh = h * 0.72f;
    dl->AddRect(ImVec2(x, y), ImVec2(x + w, y + sh), col(c), 0, 0, th);
    dl->AddLine(ImVec2(x + w * 0.5f, y + sh), ImVec2(x + w * 0.5f, y + h - th), col(c), th);
    dl->AddLine(ImVec2(x + w * 0.25f, y + h - th * 0.5f), ImVec2(x + w * 0.75f, y + h - th * 0.5f), col(c), th);
    return w;
}
inline float handheldIcon(ImDrawList* dl, float x, float y, float h, const ImVec4& c, float th) {
    const float w = h * 1.6f, t = y + h * 0.18f, b = y + h * 0.82f;
    dl->AddRect(ImVec2(x, t), ImVec2(x + w, b), col(c), 2 * th, 0, th);
    dl->AddRect(ImVec2(x + w * 0.3f, t + h * 0.14f), ImVec2(x + w * 0.7f, b - h * 0.14f), col(c), 0, 0, th);
    const float cy = (t + b) * 0.5f, r = std::max(1.0f, h * 0.07f);
    dl->AddCircleFilled(ImVec2(x + w * 0.15f, cy), r, col(c));
    dl->AddCircleFilled(ImVec2(x + w * 0.85f, cy), r, col(c));
    return w;
}
// The two-part layout button in a..b: Desktop left, Portable right, square (2 px corners); the current layout's half lit
// (the selection band, ink text and a 1 px #a9c8ff underline), the other dim. k: screen pixels per design pixel; f, px:
// the words' font and size; focused: the keyboard or controller focus is on it (a brighter outline).
inline void layoutLook(ImDrawList* dl, ImVec2 a, ImVec2 b, bool portable, bool focused, ImFont* f, float px, float k) {
    const float mid = std::floor((a.x + b.x) * 0.5f), h = b.y - a.y, ih = std::round(h * 0.36f);
    const ImVec4 edge = focused ? rgba(0xa9, 0xc8, 0xff, .9f) : rgba(205, 215, 228, .35f);
    for (int i = 0; i < 2; ++i) {
        const bool lit = (i == 1) == portable;
        const float x0 = i ? mid : a.x, x1 = i ? b.x : mid;
        if (lit) {
            dl->AddRectFilled(ImVec2(x0, a.y), ImVec2(x1, b.y), col(kSel), 2 * k, i ? ImDrawFlags_RoundCornersRight : ImDrawFlags_RoundCornersLeft);
            dl->AddRectFilled(ImVec2(x0, b.y - k), ImVec2(x1, b.y), col(rgba(0xa9, 0xc8, 0xff, 1)));
        }
        const ImVec4 c = lit ? kInk : kDim;
        const char* word = i ? "Portable" : "Desktop";
        const float th = std::max(1.0f, 1.2f * k), tw = f->CalcTextSizeA(px, FLT_MAX, 0, word).x;
        const float iw = i ? ih * 1.6f : ih * 1.25f, gap = 8 * k, w = iw + gap + tw;
        const float x = std::floor((x0 + x1 - w) * 0.5f), iy = std::floor(a.y + (h - ih) * 0.5f);
        if (i) handheldIcon(dl, x, iy, ih, c, th); else monitorIcon(dl, x, iy, ih, c, th);
        dl->AddText(f, px, ImVec2(x + iw + gap, std::floor(a.y + (h - px) * 0.5f)), col(c), word);
    }
    dl->AddRect(a, b, col(edge), 2 * k, 0, std::max(1.0f, k));
    dl->AddRectFilled(ImVec2(mid, a.y + 4 * k), ImVec2(mid + std::max(1.0f, k), b.y - 4 * k), col(kRule));
}
// What a press or a key on the layout button asks for: the layout (0 Desktop, 1 Portable) to switch to, or -1 for
// nothing. A press on a half picks it; left / right pick that side's half; A, Space or Enter (activate) picks the other
// layout. Asking for the layout already shown is nothing.
inline int layoutPick(bool portable, bool pressed, float pressX, float midX, int leftRight, bool activate) {
    int want = -1;
    if (pressed) want = pressX >= midX ? 1 : 0;
    else if (leftRight) want = leftRight > 0 ? 1 : 0;
    else if (activate) want = portable ? 0 : 1;
    return want == int(portable) ? -1 : want;
}
// The desktop menu's layout button at the cursor, size w x h: one navigable item over both halves (so Up from the first
// row lands on it), clicked by the mouse, A, Space or Enter; left / right while it has the focus pick a half. Returns
// the layout to switch to, or -1.
inline int layoutButton(const char* id, bool portable, bool enabled, float w, float h) {
    ImGui::BeginDisabled(!enabled);
    const ImVec2 a = ImGui::GetCursorScreenPos(), b(a.x + w, a.y + h);
    const bool clicked = ImGui::InvisibleButton(id, ImVec2(w, h), ImGuiButtonFlags_EnableNav);
    const bool mouse = clicked && ImGui::IsMouseReleased(ImGuiMouseButton_Left);  // a click, not A / Space / Enter
    const bool focused = ImGui::IsItemFocused() && GImGui->NavCursorVisible;
    const int lr = enabled && ImGui::IsItemFocused() ? pad::padLeftRight(false) : 0;
    layoutLook(ImGui::GetWindowDrawList(), a, b, portable, focused, ImGui::GetFont(), ImGui::GetFontSize(), ctx().s);
    ImGui::EndDisabled();
    if (!enabled) return -1;
    return layoutPick(portable, mouse, ImGui::GetIO().MousePos.x, std::floor((a.x + b.x) * 0.5f), lr, clicked && !mouse);
}
// The desktop menu's page tabs across w from the cursor, h high: the names in spaced capitals, the current one in ink
// with a short static underline. Never navigation stops (a click or L1 / R1 turns pages). Returns the page to
// show: a clicked tab's, else page.
inline int tabs(int page, const char* const* names, int n, float w, float h) {
    const float s = ctx().s;
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImFont* f = smallFont();
    const float size = 12 * s, cw = w / n;
    int to = page;
    ImGui::PushItemFlag(ImGuiItemFlags_NoNav, true);
    for (int i = 0; i < n; ++i) {
        const float x0 = p.x + cw * i;
        ImGui::SetCursorScreenPos(ImVec2(x0, p.y));
        ImGui::PushID(i);
        if (ImGui::InvisibleButton("##tab", ImVec2(cw, h), ImGuiButtonFlags_PressedOnClick | ImGuiButtonFlags_NoNavFocus)) to = i;
        const bool hot = ImGui::IsItemHovered();
        ImGui::PopID();
        char up[48]; size_t k = 0;
        for (; names[i][k] && k + 1 < sizeof up; ++k) up[k] = names[i][k] >= 'a' && names[i][k] <= 'z' ? char(names[i][k] - 32) : names[i][k];
        up[k] = 0;
        const float tw = spacedText(nullptr, f, size, ImVec2(0, 0), up, 0.2f, kInk);
        const float tx = std::floor(x0 + (cw - tw) * 0.5f), ty = std::floor(p.y + (h - size) * 0.5f - 2 * s);
        spacedText(dl, f, size, ImVec2(tx, ty), up, 0.2f, i == page ? kInk : hot ? kInk2 : kDim);
        if (i == page) dl->AddRectFilled(ImVec2(tx, p.y + h - 2 * s), ImVec2(tx + tw, p.y + h), col(rgba(0xa9, 0xc8, 0xff, 1)));
    }
    ImGui::PopItemFlag();
    dl->AddRectFilled(ImVec2(p.x, p.y + h), ImVec2(p.x + w, p.y + h + s), col(rgba(205, 215, 228, .12f)));
    ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + h + s));
    ImGui::Dummy(ImVec2(w, 0));
    return to;
}

}  // namespace look
