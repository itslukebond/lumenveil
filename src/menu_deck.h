// The Insert menu's Steam Deck layout, kept free of Windows types so tests/menu_deck_test.cpp covers it on the host:
// detection, the Layout setting and its live change (Portable mode), the controller's steps and focus, and the widgets.
// The layout is the signed-off 1280x800 design (a 760x768 panel: logo and layout button, tab bar with L1 / R1, one page
// of rows in fixed slots, the help box, the status line, footer), drawn at min(width / 1280, height / 800) and centred,
// in the desktop menu's look (menu_look.h) at its own sizes. Each row is one invisible button with a fixed rect drawn through the draw list, so rows never move, and a
// dimmed row keeps its place but takes no input.
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include "imgui.h"
#include "imgui_internal.h"  // ImRect, ClearActiveID
#include "menu_look.h"       // the desktop look: panel, brackets, glove, tick box, layout button
#include "menu_pad.h"        // typedValue

namespace deck {

// The Steam Deck's graphics (PCI IDs 1002:163F on the LCD model, 1002:1435 on the OLED), or SteamDeck=1 in the
// environment, on any adapter.
inline bool isSteamDeck(uint32_t vendor, uint32_t device, const char* steamDeckEnv) {
    if (steamDeckEnv && !std::strcmp(steamDeckEnv, "1")) return true;
    return vendor == 0x1002 && (device == 0x163F || device == 0x1435);
}
// What found it, for the log.
inline const char* foundBy(uint32_t vendor, uint32_t device, const char* steamDeckEnv) {
    if (vendor == 0x1002 && (device == 0x163F || device == 0x1435)) return "adapter";
    return isSteamDeck(vendor, device, steamDeckEnv) ? "SteamDeck=1" : "not found";
}
// [overlay] layout: 0 Automatic, 1 Steam Deck, 2 Desktop; anything else reads as Automatic.
inline int layoutValue(int raw) { return raw == 1 || raw == 2 ? raw : 0; }
inline bool deckLayoutOn(int setting, bool deck) { return setting == 1 ? true : setting == 2 ? false : deck; }
inline float deckScale(float w, float h) { return std::min(w / 1280.f, h / 800.f); }

// A layout change asked by the layout button: it lands on the next frame (compose applies it before drawing); asking
// for the layout shown cancels it. want: -1 nothing pending, else 1 (the Deck layout) or 0 (the desktop menu).
struct Swap {
    int want = -1;
    double at = 0;
    void flip(bool on, double now, bool current, double delay = 0) {
        if (on == current) { want = -1; return; }
        want = on ? 1 : 0; at = now + delay;
    }
    bool due(double now) const { return want >= 0 && now >= at; }
};

// A list's next index, wrapping both ways.
inline int wrapStep(int i, int n, int dir) { return n > 0 ? ((i + dir) % n + n) % n : 0; }
// A slider's next value, stopping at the ends.
inline int clampStep(int v, int lo, int hi, int step, int dir) { return std::clamp(v + dir * step, lo, hi); }
// A list of setting values: the next one after cur. A value not in the list (one only the desktop menu offers) stays
// until stepped away from: right goes to the first entry, left to the last.
inline int listStep(const int* values, int n, int cur, int dir) {
    if (!dir || n <= 0) return cur;
    for (int i = 0; i < n; ++i)
        if (values[i] == cur) return values[wrapStep(i, n, dir)];
    return dir > 0 ? values[0] : values[n - 1];
}

// The focused row of each of the three pages (-1: the layout button above the first row), and the page shown.
constexpr int kButton = -1;
struct Nav {
    int page = 0, focus[3] = {};
    void move(int rows, int dir) { focus[page] = std::clamp(focus[page] + dir, kButton, std::max(0, rows - 1)); }  // dimmed rows included
    void turn(int dir) { page = wrapStep(page, 3, dir); focus[page] = 0; }  // a new page starts on its first row
    void show(int to) { if (to != page) { page = to; focus[page] = 0; } }
};
// Where a layout change lands, once compose has set the new layout: the Deck layout keeps its page with the layout
// button focused; the desktop menu is told to focus its button (focusDesk).
inline void landFocus(bool deckOpen, Nav& nav, bool& focusDesk) {
    if (deckOpen) nav.focus[nav.page] = kButton;
    else focusDesk = true;
}

// ---- drawing ---------------------------------------------------------------------------------------------------------
// The design's geometry, in its pixels from the panel's top left.
// Fixed slots, top to bottom: the top bar (logo left, layout button right), the tabs, the rows (a heading slot and seven
// rows at a 54 px pitch), the help box (a hairline, three 26 px lines), the status line and the footer (key hints with
// the version, two trademark lines). Nothing moves between pages or states.
constexpr float kPanelW = 760, kPanelH = 768;
constexpr float kTopBar = 80, kButtonL = 430, kButtonR = 730, kButtonT = 18, kButtonB = 62;
constexpr float kTabTop = 80, kTabBottom = 132, kBodyTop = 136, kSubH = 28, kPitch = 54, kBodyEnd = kBodyTop + kSubH + 7 * kPitch;
constexpr float kHelpRule = 548, kHelpTop = 556, kHelpLine = 26, kHelpPx = 19, kStatusTop = 640, kStatusPx = 15;
constexpr float kFootTop = 674, kTradeTop = 716, kTradePx = 15, kBottomPad = 18;
constexpr float kLabelShrink = 0.95f;  // a row label longer than its slot is drawn up to 5% smaller before it is shortened
constexpr float kRowL = 20, kRowR = 740, kRowH = kPitch - 2, kCtlL = 310, kCtlR = 722, kArrowW = 56, kBoxW = 104, kTrackR = 604;
constexpr float kFlagX = 731;  // the flag slot's centre, right of the controls
static_assert(kBodyEnd <= kHelpRule - 6 && kHelpTop + 3 * kHelpLine <= kStatusTop && kStatusTop + 26 <= kFootTop &&
              kTradeTop + 2 * 17 + kBottomPad <= kPanelH, "the slots fit the panel in order");

const ImVec4 kText(0.918f, 0.929f, 0.949f, 1), kDim(0.549f, 0.580f, 0.631f, 1), kGreen(0.620f, 0.902f, 0.200f, 1),
             kAmber(1.0f, 0.75f, 0.25f, 1);

struct Look {
    ImVec2 o;                     // the panel's top left on screen
    float k = 1;                  // screen pixels per design pixel
    ImFont *label, *value, *caption;  // 26, 24 and 19 px in the design
    float y = kBodyTop;           // the body's next free line
    float sWas = 1;               // look::ctx().s outside the panel, put back by end
    static Look at(float w, float h, ImFont* label, ImFont* value, ImFont* caption) {
        Look L;
        L.k = deckScale(w, h);
        L.o = ImVec2((w - kPanelW * L.k) * 0.5f, (h - kPanelH * L.k) * 0.5f);
        L.label = label; L.value = value; L.caption = caption;
        return L;
    }
    ImVec2 p(float x, float y) const { return ImVec2(o.x + x * k, o.y + y * k); }
    float dx(float sx) const { return (sx - o.x) / k; }  // a screen x in design pixels
    float dy(float sy) const { return (sy - o.y) / k; }
};

inline ImU32 col(const ImVec4& c, float a = 1) { return ImGui::GetColorU32(ImVec4(c.x, c.y, c.z, c.w * a)); }  // style alpha applies

// Text in font f at px design pixels, first made smaller (down to shrink times px) and then shortened with "..." to fit
// maxW design pixels; align 0 left, 0.5 centred. A smaller size stays centred on the same line.
inline void text(const Look& L, ImFont* f, float px, float x, float y, float maxW, const ImVec4& c, const char* s, float align = 0,
                 float shrink = 1) {
    const float full = px * L.k, max = maxW * L.k;
    char buf[256];
    std::snprintf(buf, sizeof buf, "%s", s);
    float w = f->CalcTextSizeA(full, FLT_MAX, 0, buf).x;
    const float size = w > max ? std::max(full * shrink, full * max / w) : full;
    w = f->CalcTextSizeA(size, FLT_MAX, 0, buf).x;
    if (w > max) {
        size_t n = std::strlen(buf);
        while (n > 0 && w > max) {
            do --n; while (n > 0 && (buf[n] & 0xC0) == 0x80);  // whole UTF-8 characters
            std::snprintf(buf + n, sizeof buf - n, "...");
            w = f->CalcTextSizeA(size, FLT_MAX, 0, buf).x;
        }
    }
    const ImVec2 at = L.p(x, y);
    ImGui::GetWindowDrawList()->AddText(f, size, ImVec2(at.x + (max - w) * align, at.y + (full - size) * 0.5f), col(c), buf);
}
// A triangle pointing left (dir -1) or right (1), centred in the box.
inline void arrow(const Look& L, float x0, float y0, float x1, float y1, int dir, const ImVec4& c) {
    const ImVec2 m = L.p((x0 + x1) * 0.5f, (y0 + y1) * 0.5f);
    const float a = 8 * L.k, b = 10 * L.k;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    if (dir < 0) dl->AddTriangleFilled(ImVec2(m.x - a, m.y), ImVec2(m.x + a, m.y - b), ImVec2(m.x + a, m.y + b), col(c));
    else dl->AddTriangleFilled(ImVec2(m.x + a, m.y), ImVec2(m.x - a, m.y + b), ImVec2(m.x - a, m.y - b), col(c));
}
inline bool inside(float x, float y, float x0, float y0, float x1, float y1) { return x >= x0 && x < x1 && y >= y0 && y < y1; }

// The panel's window: fixed, undecorated, never scrolls.
inline void begin(Look& L) {
    ImGui::SetNextWindowPos(L.o, ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(kPanelW * L.k, kPanelH * L.k), ImGuiCond_Always);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0);
    ImGui::PushStyleVar(ImGuiStyleVar_DisabledAlpha, 0.42f);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0, 0, 0, 0));
    ImGui::Begin("##deck", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                                    ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoBackground);
    L.y = kBodyTop;
    L.sWas = look::ctx().s;
    look::ctx().s = L.k;  // the look's helpers draw at the Deck's scale
    // the desktop menu's panel: the gradient, the streak with its glow (reaching above the panel), the bottom line and
    // the corner brackets (22 px arms, 10 px in)
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 a = L.p(0, 0), b = L.p(kPanelW, kPanelH);
    ImGui::PushClipRect(ImVec2(a.x, a.y - 6 * L.k), b, false);
    look::panel(dl, a, b, look::kMenu);
    ImGui::PopClipRect();
    look::brackets(dl, a, b, look::kMenu, 22, 10);
}
inline void end(Look& L) {
    look::ctx().s = L.sWas;
    ImGui::End();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(4);
}

// The logo, 48 design pixels tall, at the top bar's left.
inline void logo(const Look& L, ImTextureID tex, float aspect) {
    const float h = 48, w = h * aspect;
    ImGui::GetWindowDrawList()->AddImage(tex, L.p(30, (kTopBar - h) * 0.5f), L.p(30 + w, (kTopBar + h) * 0.5f));
}
// The layout button at the top bar's right: a tap on a half picks it; with the controller's focus on it (focused),
// left / right pick a half and A the other layout. Returns the layout to switch to (0 Desktop, 1 Portable) or -1.
inline int layoutButton(const Look& L, bool portable, bool focused, int lr, bool a) {
    ImGui::SetCursorScreenPos(L.p(kButtonL, kButtonT));
    const bool pressed = ImGui::InvisibleButton("##layout", ImVec2((kButtonR - kButtonL) * L.k, (kButtonB - kButtonT) * L.k), ImGuiButtonFlags_PressedOnClick);
    look::layoutLook(ImGui::GetWindowDrawList(), L.p(kButtonL, kButtonT), L.p(kButtonR, kButtonB), portable, focused, L.value, 22 * L.k, L.k);
    return look::layoutPick(portable, pressed, ImGui::GetIO().MouseClickedPos[0].x, L.p((kButtonL + kButtonR) * 0.5f, 0).x,
                            focused ? lr : 0, focused && a);
}

// Spaced capitals: text upper-cased with extra space after each letter, in font f at px design pixels; returns the width
// in design pixels. Measures only when dl is null.
inline float caps(const Look& L, ImDrawList* dl, ImFont* f, float px, float x, float y, const char* name, float tracking, const ImVec4& c) {
    char up[64]; size_t i = 0;
    for (; name[i] && i + 1 < sizeof up; ++i) up[i] = name[i] >= 'a' && name[i] <= 'z' ? char(name[i] - 32) : name[i];
    up[i] = 0;
    return look::spacedText(dl, f, px * L.k, L.p(x, y), up, tracking, c) / L.k;
}
// A key badge (square, 2 px corners) with its name in 15 px, 28 high, at least 40 wide; returns its width.
inline float badge(const Look& L, float x, float y, const char* name) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float tw = L.caption->CalcTextSizeA(15 * L.k, FLT_MAX, 0, name).x / L.k, w = std::max(40.f, tw + 14);
    dl->AddRect(L.p(x, y), L.p(x + w, y + 28), col(ImVec4(205 / 255.f, 215 / 255.f, 228 / 255.f, .5f)), 2 * L.k, 0, std::max(1.0f, L.k));
    text(L, L.caption, 15, x, y + (28 - 15) * 0.5f - 1, w, look::kWhite, name, 0.5f);
    return w;
}

// The tab bar: L1, the three pages, R1. Returns the page to show: a pressed tab's, the one before (L1) or after (R1)
// page, else page.
inline int tabs(const Look& L, int page, const char* const* names) {
    static const char* const kNames[3] = { "Upscaling", "Frame generation", "Overlay" };
    if (!names) names = kNames;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(L.p(30, kTabBottom - 1), L.p(kPanelW - 30, kTabBottom), col(ImVec4(205 / 255.f, 215 / 255.f, 228 / 255.f, .12f)));
    int to = page;
    auto hit = [&](const char* id, float x0, float x1) {
        ImGui::SetCursorScreenPos(L.p(x0, kTabTop));
        return ImGui::InvisibleButton(id, ImVec2((x1 - x0) * L.k, (kTabBottom - kTabTop) * L.k), ImGuiButtonFlags_PressedOnClick);
    };
    auto glyph = [&](const char* g, float cx) {
        const float w = std::max(40.f, L.caption->CalcTextSizeA(15 * L.k, FLT_MAX, 0, g).x / L.k + 14);
        badge(L, cx - w / 2, (kTabTop + kTabBottom - 28) * 0.5f, g);
    };
    // the names in 17 px spaced capitals, a name's spacing narrowed only as far as it needs to fit its tab
    auto tracking = [&](const char* name) {
        const float w = caps(L, nullptr, L.caption, 17, 0, 0, name, 0, kText), n = float(std::strlen(name) - 1);
        return n > 0 && w + n * 0.2f * 17 > 192 ? std::max(0.f, (192 - w) / (n * 17)) : 0.2f;
    };
    if (hit("##l1", kRowL, kRowL + 60)) to = wrapStep(page, 3, -1);
    glyph("L1", kRowL + 30);
    for (int i = 0; i < 3; ++i) {
        const float x0 = kRowL + 60 + 200 * i, x1 = x0 + 200;
        ImGui::PushID(i);
        if (hit("##tab", x0, x1)) to = i;
        ImGui::PopID();
        const float track = tracking(names[i]), w = caps(L, nullptr, L.caption, 17, 0, 0, names[i], track, kText), cx = (x0 + x1) * 0.5f;
        caps(L, dl, L.caption, 17, cx - w * 0.5f, (kTabTop + kTabBottom - 17) * 0.5f - 1, names[i], track, i == page ? look::kWhite : look::kDim);
        if (i == page) {  // a short streak under the active tab, with its glow
            const look::Stop st[] = { { 0, look::alpha(look::kStreak, 0) }, { .2f, look::kStreak }, { .5f, look::kStreakHi }, { .8f, look::kStreak }, { 1, look::alpha(look::kStreak, 0) } };
            const look::Stop gl[] = { { 0, look::rgba(79, 141, 255, 0) }, { .5f, look::rgba(79, 141, 255, .6f) }, { 1, look::rgba(79, 141, 255, 0) } };
            const ImVec2 a = L.p(cx - 28, kTabBottom - 1), b = L.p(cx + 28, kTabBottom + 1);
            look::hgrad(dl, a.x - 4 * L.k, b.x + 4 * L.k, a.y - 4 * L.k, a.y, gl, 3, 0, 0.6f);
            look::hgrad(dl, a.x - 4 * L.k, b.x + 4 * L.k, b.y, b.y + 4 * L.k, gl, 3, 0.6f, 0);
            look::hgrad(dl, a.x, b.x, a.y, b.y, st, 5);
        }
    }
    if (hit("##r1", kRowR - 60, kRowR)) to = wrapStep(page, 3, 1);
    glyph("R1", kRowR - 30);
    return to;
}

// The heading in the rows' heading slot: 16 px spaced capitals at the slot's foot, then a rule fading out toward the
// right; takes the slot's 28 px.
inline void subHeading(Look& L, const char* name) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float w = caps(L, dl, L.caption, 16, 30, L.y + kSubH - 16 - 6, name, 0.22f, look::kInk2);
    look::fadeRule(dl, L.p(30 + w + 12, 0).x, L.p(kPanelW - 30, 0).x, std::floor(L.p(0, L.y + kSubH - 14).y), look::kRule, std::max(1.0f, L.k));
    L.y += kSubH;
}
// A row's flag in its slot at the right end: a small diamond, amber or green.
inline void flag(const Look& L, float rowY, const ImVec4& c) {
    look::diamond(ImGui::GetWindowDrawList(), L.p(kFlagX, rowY + kRowH * 0.5f), 5 * L.k, c, 0);
}

// A row's input: whether the controller's focus is on it, whether it is dimmed, and this frame's controller action
// (left / right -1 / +1, A), given only to the focused row. Its output: a press on it (focus it) and the step asked
// (-1 / +1; for a toggle, +1 flips it).
struct RowIn { bool focused = false, dim = false; int lr = 0; bool a = false; };
struct RowOut { bool press = false; int step = 0; bool focus = false; };  // focus: a tap on a dimmed row

// The row's background, focus bar and label; the row's button is added by rowButton after the row's own controls.
// The focused row: the desktop's blue band fading to the right, its glowing underline (to 85 % of the row) and the
// pointing glove 2 px into the row, swaying by look::ctx().bob. Then the label, 48 px in.
inline void rowFrame(const Look& L, const char* label, const RowIn& in) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float y = L.y, k = L.k;
    if (in.focused) {
        const ImVec2 a = L.p(kRowL, y), b = L.p(kRowR, y + kRowH);
        const look::Stop band[] = { { 0, look::kSel }, { .55f, look::rgba(110, 160, 255, .07f) }, { 1, look::rgba(110, 160, 255, 0) } };
        look::hgrad(dl, a.x, b.x, a.y, b.y, band, 3);
        const float ux = a.x + (b.x - a.x) * 0.85f, uy = b.y - k;
        const ImVec4 glow = look::rgba(120, 170, 255, .7f);
        const look::Stop gl[] = { { 0, look::alpha(glow, .5f) }, { 1, look::alpha(glow, 0) } };
        look::hgrad(dl, a.x, ux, uy - 4 * k, uy, gl, 2, 0, 0.6f);
        look::hgrad(dl, a.x, ux, uy + k, uy + 4 * k, gl, 2, 0.6f, 0);
        look::fadeRule(dl, a.x, ux, uy, look::rgba(0xa9, 0xc8, 0xff, 1), k);
        look::glove(dl, L.p(kRowL + 2 + look::ctx().bob, y + kRowH * 0.5f - 10.8f), 0.9f * k);
    }
    text(L, L.label, 26, kRowL + 48, y + (kRowH - 26) * 0.5f - 1, kCtlL - kRowL - 52, look::kInk, label, 0, kLabelShrink);
}
// The whole row as one button (last, so the row's own controls take a press first). True on the frame it is pressed;
// the item is the row, so GetItemRect gives the row's rect.
inline bool rowButton(const Look& L, const char* id) {
    ImGui::SetCursorScreenPos(L.p(kRowL, L.y));
    return ImGui::InvisibleButton(id, ImVec2((kRowR - kRowL) * L.k, kRowH * L.k), ImGuiButtonFlags_PressedOnClick);
}
// A tap on a dimmed row (whose controls are disabled) only focuses it, so the help box can say why it is dimmed.
inline bool dimTap(const Look& L) {
    const ImVec2 m = ImGui::GetIO().MousePos;
    const float x = L.dx(m.x), y = L.dy(m.y);
    return ImGui::IsMouseClicked(0) && ImGui::IsWindowHovered() && x >= kRowL && x < kRowR && y >= L.y && y < L.y + kRowH;
}
// Where the press that is still held began, in design pixels.
inline ImVec2 pressAt(const Look& L) { const ImVec2 m = ImGui::GetIO().MouseClickedPos[0]; return ImVec2(L.dx(m.x), L.dy(m.y)); }

// ◀ value ▶: the arrows step -1 / +1, a press elsewhere on the row +1; left / right step, A steps +1. A resolved part
// (rest) follows the value dim after a dot.
inline RowOut cycler(Look& L, const char* id, const char* label, const char* value, RowIn in, const char* rest = nullptr) {
    RowOut out;
    ImGui::BeginDisabled(in.dim);
    rowFrame(L, label, in);
    const float y = L.y, t = y + (kRowH - 52) * 0.5f;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    // ‹ and › centred in their slots, white on the focused row, then the value centred between them
    const ImVec4 ar = in.focused ? look::kWhite : look::kDim;
    look::chevron(dl, L.p(kCtlL + kArrowW * 0.5f, y + kRowH * 0.5f), -1, ar, 1.75f * L.k);
    look::chevron(dl, L.p(kCtlR - kArrowW * 0.5f, y + kRowH * 0.5f), 1, ar, 1.75f * L.k);
    {
        char tail[48] = "";
        if (rest && *rest) std::snprintf(tail, sizeof tail, " \xC2\xB7 %s", rest);
        const float slot = kCtlR - kCtlL - 2 * kArrowW, mw = L.value->CalcTextSizeA(24 * L.k, FLT_MAX, 0, value).x / L.k,
                    tw = L.value->CalcTextSizeA(24 * L.k, FLT_MAX, 0, tail).x / L.k, x = kCtlL + kArrowW + std::max(0.f, (slot - mw - tw) * 0.5f);
        text(L, L.value, 24, x, y + (kRowH - 24) * 0.5f - 1, slot - (x - kCtlL - kArrowW), look::kInk, value);
        if (*tail) text(L, L.value, 24, x + mw, y + (kRowH - 24) * 0.5f - 1, std::max(0.f, slot - (x + mw - kCtlL - kArrowW)), kDim, tail);
    }
    if (rowButton(L, id)) {
        out.press = true;
        const ImVec2 at = pressAt(L);
        out.step = inside(at.x, at.y, kCtlL, t, kCtlL + kArrowW, t + 52) ? -1 : 1;
    }
    ImGui::EndDisabled();
    if (in.dim && dimTap(L)) out.focus = true;
    if (!in.dim && in.focused && !out.press) out.step = in.lr ? in.lr : in.a ? 1 : 0;
    L.y += kRowH + 2;
    return out;
}

// A tick box with On / Off beside it: a press, left / right or A flips it (step +1).
inline RowOut toggle(Look& L, const char* id, const char* label, bool on, RowIn in) {
    RowOut out;
    ImGui::BeginDisabled(in.dim);
    rowFrame(L, label, in);
    const float y = L.y, k = L.k, cy = std::floor(L.p(0, y + kRowH * 0.5f).y);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    // the desktop's square box at 28 (inner 14): the inner square grows in with a pulse ring when on, shrinks out when off
    const float age = look::toggleAge(*ImGui::GetStateStorage(), ImGui::GetID(id), on, float(ImGui::GetTime()));
    const float half = 14 * k;
    const ImVec2 c(L.p(kCtlL, 0).x + half, cy);
    dl->AddRect(ImVec2(c.x - half, c.y - half), ImVec2(c.x + half, c.y + half), col(on ? look::rgba(0xcf, 0xe0, 0xff, 1) : look::rgba(220, 230, 240, .7f)), 0, 0, std::max(1.0f, k));
    const look::Anim inner = look::toggleInner(on, age);
    if (inner.alpha > 0.001f) {
        const float q = 7 * k * inner.scale;
        dl->AddRectFilled(ImVec2(c.x - q - 3 * k, c.y - q - 3 * k), ImVec2(c.x + q + 3 * k, c.y + q + 3 * k), col(look::rgba(150, 190, 255, .25f * inner.alpha)));
        dl->AddRectFilled(ImVec2(c.x - q, c.y - q), ImVec2(c.x + q, c.y + q), col(look::alpha(look::kWhite, inner.alpha)));
    }
    if (on) {
        const look::Anim p = look::pulse(age);
        if (p.alpha > 0.001f) {
            const float r = (half + k) * p.scale;
            dl->AddRect(ImVec2(c.x - r, c.y - r), ImVec2(c.x + r, c.y + r), col(look::rgba(140, 185, 255, p.alpha)), 0, 0, std::max(1.0f, k));
        }
    }
    text(L, L.value, 22, kCtlL + 28 + 16, y + (kRowH - 22) * 0.5f - 1, 60, look::kInk2, on ? "On" : "Off");
    if (rowButton(L, id)) { out.press = true; out.step = 1; }
    ImGui::EndDisabled();
    if (in.dim && dimTap(L)) out.focus = true;
    if (!in.dim && in.focused && !out.press && (in.lr || in.a)) out.step = 1;
    L.y += kRowH + 2;
    return out;
}

// The number box being typed in (one at a time): the slider row's id, set by A or a press on the box; wait counts the
// frames ImGui takes to give the box the keyboard.
struct Typing { ImGuiID id = 0; bool start = false, active = false; int wait = 0; };
// B: leave the box without changing the value.
inline void leaveTyping(Typing& t) {
    if (t.active && GImGui->ActiveId) ImGui::ClearActiveID();
    t = Typing{};
}

// A track with a grab and a number box: a press on the track sets the value there and a drag follows (clamped);
// left / right step it by step; A or a press on the box puts the box into typing, where Enter sets the typed value.
// True when v changed.
inline bool slider(Look& L, const char* id, const char* label, int& v, int lo, int hi, int step, const char* unit, RowIn in,
                   Typing& typing, RowOut* outp) {
    RowOut out;
    const int before = v;
    ImGui::PushID(id);
    const ImGuiID rid = ImGui::GetID("##row");
    if (in.dim && typing.id == rid) leaveTyping(typing);  // dimmed while its box was open: the box lets go
    ImGui::BeginDisabled(in.dim);
    rowFrame(L, label, in);
    const float y = L.y, t = y + (kRowH - 52) * 0.5f, bx = kCtlR - kBoxW;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float f = hi > lo ? std::clamp(float(v - lo) / float(hi - lo), 0.f, 1.f) : 0, gx = kCtlL + (kTrackR - kCtlL) * f, cy = y + kRowH * 0.5f;
    {  // the hairline track, its fill and the diamond knob (an 18 px square turned 45 degrees)
        const float sy = std::floor(L.p(0, cy).y), x0 = L.p(kCtlL, 0).x, x1 = L.p(kTrackR, 0).x, kx = L.p(gx, 0).x;
        dl->AddRectFilled(ImVec2(x0, sy - L.k), ImVec2(x1, sy + L.k), col(look::rgba(205, 215, 228, .25f)));
        if (kx > x0) {
            const look::Stop fill[] = { { 0, look::rgba(0x6f, 0x9d, 0xff, 1) }, { 1, look::rgba(0xe4, 0xef, 0xff, 1) } };
            look::hgrad(dl, x0, kx, sy - L.k, sy + L.k, fill, 2);
        }
        look::diamond(dl, ImVec2(kx, sy), 12.73f * L.k, look::kWhite, 3 * L.k);
    }
    // the box: typed in, or showing the value
    if (typing.id == rid && !in.dim) {
        ImGui::SetCursorScreenPos(L.p(bx, t));
        ImGui::SetWindowFontScale(24 * L.k / L.value->FontSize);
        ImGui::PushFont(L.value);
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(10 * L.k, (52 - 24) * 0.5f * L.k));
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 4 * L.k);
        ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0, 0, 0, 0.25f));
        ImGui::SetNextItemWidth(kBoxW * L.k);
        if (typing.start) { ImGui::SetKeyboardFocusHere(); typing.start = false; }
        char buf[16];
        std::snprintf(buf, sizeof buf, "%d", v);
        const bool enter = ImGui::InputText("##box", buf, sizeof buf, ImGuiInputTextFlags_CharsDecimal | ImGuiInputTextFlags_EnterReturnsTrue |
                                            ImGuiInputTextFlags_AutoSelectAll);
        const bool active = ImGui::IsItemActive();
        if (enter) { v = pad::typedValue(buf, lo, hi, v); typing = Typing{}; }
        else if (ImGui::IsItemDeactivated() || (!active && typing.active) || (!active && ++typing.wait > 4)) typing = Typing{};
        else typing.active = active;
        ImGui::PopStyleColor();
        ImGui::PopStyleVar(2);
        ImGui::PopFont();
        ImGui::SetWindowFontScale(1);
    } else {
        // the number box, 104 x 44 in the 52 high place it takes presses in
        dl->AddRectFilled(L.p(bx, t + 4), L.p(kCtlR, t + 48), col(look::rgba(255, 255, 255, .04f)), 2 * L.k);
        dl->AddRect(L.p(bx, t + 4), L.p(kCtlR, t + 48), col(look::kRule), 2 * L.k, 0, std::max(1.0f, L.k));
        char s[24];
        std::snprintf(s, sizeof s, "%d%s", v, unit);
        text(L, L.value, 22, bx + 4, y + (kRowH - 22) * 0.5f - 1, kBoxW - 8, look::kInk, s, 0.5f);
        ImGui::SetCursorScreenPos(L.p(bx, t));
        if (ImGui::InvisibleButton("##num", ImVec2(kBoxW * L.k, 52 * L.k), ImGuiButtonFlags_PressedOnClick)) {
            out.press = true;
            typing = Typing{ rid, true, false, 0 };
        }
    }
    // the track: a press there sets the value, and while it is held the value follows the pointer
    auto setFrom = [&](float x) {
        const float steps = (x - kCtlL) / (kTrackR - kCtlL) * float(hi - lo) / float(step);
        v = std::clamp(lo + int(std::lround(steps)) * step, lo, hi);
    };
    if (rowButton(L, "##row")) out.press = true;
    if (ImGui::IsItemActive()) {
        const ImVec2 at = pressAt(L);
        if (inside(at.x, at.y, kCtlL - 12, t, kTrackR + 12, t + 52)) setFrom(L.dx(ImGui::GetIO().MousePos.x));
        else if (ImGui::IsItemActivated()) typing = Typing{ rid, true, false, 0 };  // a press on the label does what A does
    }
    ImGui::EndDisabled();
    if (in.dim && dimTap(L)) out.focus = true;
    if (!in.dim && in.focused && typing.id != rid) {
        if (in.lr) v = clampStep(v, lo, hi, step, in.lr);
        else if (in.a) typing = Typing{ rid, true, false, 0 };
    }
    ImGui::PopID();
    L.y += kRowH + 2;
    if (outp) *outp = out;
    return v != before;
}

// The footer: a faint rule at kFootTop; the controller's buttons in square badges with what they do (B says Back while a
// number box is being typed in), the version right-aligned on that line; then the trademark lines at 15 px.
inline void footer(const Look& L, const char* version, bool typing) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(L.p(30, kFootTop), L.p(kPanelW - 30, kFootTop + 1), col(ImVec4(205 / 255.f, 215 / 255.f, 228 / 255.f, .12f)));
    const float ky = kFootTop + 8;
    float x = 30;
    auto key = [&](const char* name, const char* word) {
        x += badge(L, x, ky, name) + 10;
        text(L, L.caption, 19, x, ky + (28 - 19) * 0.5f - 1, 200, look::kInk2, word);
        x += L.caption->CalcTextSizeA(19 * L.k, FLT_MAX, 0, word).x / L.k + 26;
    };
    key("A", "Change"); key("L1 R1", "Page"); key("B", typing ? "Back" : "Close");
    const float vw = L.caption->CalcTextSizeA(17 * L.k, FLT_MAX, 0, version).x / L.k;
    text(L, L.caption, 17, kPanelW - 30 - vw - 1, ky + (28 - 17) * 0.5f - 1, vw + 1, look::kDim, version);
    // the trademarks, wrapped in two lines
    static const char* const kTm = "Uses NVIDIA DLSS, NVIDIA Reflex and NVIDIA Streamline, AMD FSR and Intel XeSS. NVIDIA, DLSS and "
                                   "Reflex are trademarks of NVIDIA Corporation, FSR of AMD and XeSS of Intel.";
    look::wrapText(dl, L.caption, kTradePx * L.k, L.p(30, kTradeTop), (kPanelW - 60) * L.k, 17 * L.k, kTm, look::kDim, 2);
}

}  // namespace deck
