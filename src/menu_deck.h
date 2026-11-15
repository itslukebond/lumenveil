// The Insert menu's Steam Deck layout, kept free of Windows types so tests/menu_deck_test.cpp covers it on the host:
// detection, the Layout setting, the controller's steps and focus, and the widgets. The layout is the signed-off
// 1280x800 design (a 760x768 panel: logo, tab bar with L1 / R1, one page of 66 px rows, footer), drawn at
// min(width / 1280, height / 800) and centred. Each row is one invisible button with a fixed rect drawn through the
// draw list, so rows never move, and a dimmed row keeps its place but takes no input.
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include "imgui.h"
#include "imgui_internal.h"  // ImRect, ClearActiveID
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

// The focused row of each of the three pages, and the page shown.
struct Nav {
    int page = 0, focus[3] = {};
    void move(int rows, int dir) { focus[page] = std::clamp(focus[page] + dir, 0, std::max(0, rows - 1)); }  // dimmed rows included
    void turn(int dir) { page = wrapStep(page, 3, dir); }
};
constexpr int kRows[3] = { 4, 5, 3 };  // focusable rows on Upscaling, Frame generation and Overlay

// ---- drawing ---------------------------------------------------------------------------------------------------------
// The design's geometry, in its pixels from the panel's top left.
constexpr float kPanelW = 760, kPanelH = 768;
constexpr float kTabTop = 82, kTabBottom = 142, kBodyTop = 156, kFootTop = 704;
constexpr float kRowL = 20, kRowR = 740, kRowH = 66, kCtlL = 310, kCtlR = 728, kArrowW = 56, kBoxW = 104, kTrackR = 610;

const ImVec4 kText(0.918f, 0.929f, 0.949f, 1), kDim(0.549f, 0.580f, 0.631f, 1), kGreen(0.620f, 0.902f, 0.200f, 1),
             kAmber(1.0f, 0.75f, 0.25f, 1);

struct Look {
    ImVec2 o;                     // the panel's top left on screen
    float k = 1;                  // screen pixels per design pixel
    ImFont *label, *value, *caption;  // 26, 24 and 19 px in the design
    ImVec4 acc{ 0.59f, 0.62f, 0.67f, 1 }, accHi{ 0.83f, 0.85f, 0.88f, 1 };  // this row's accent
    float y = kBodyTop;           // the body's next free line
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

// Text in font f at px design pixels, shortened with "..." to fit maxW design pixels; align 0 left, 0.5 centred.
inline void text(const Look& L, ImFont* f, float px, float x, float y, float maxW, const ImVec4& c, const char* s, float align = 0) {
    const float size = px * L.k, max = maxW * L.k;
    char buf[256];
    std::snprintf(buf, sizeof buf, "%s", s);
    float w = f->CalcTextSizeA(size, FLT_MAX, 0, buf).x;
    if (w > max) {
        size_t n = std::strlen(buf);
        while (n > 0 && w > max) {
            do --n; while (n > 0 && (buf[n] & 0xC0) == 0x80);  // whole UTF-8 characters
            std::snprintf(buf + n, sizeof buf - n, "...");
            w = f->CalcTextSizeA(size, FLT_MAX, 0, buf).x;
        }
    }
    const ImVec2 at = L.p(x, y);
    ImGui::GetWindowDrawList()->AddText(f, size, ImVec2(at.x + (max - w) * align, at.y), col(c), buf);
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
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 6 * L.k);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0);
    ImGui::PushStyleVar(ImGuiStyleVar_DisabledAlpha, 0.42f);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(22 / 255.f, 24 / 255.f, 29 / 255.f, 0.94f));
    ImGui::Begin("##deck", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                                    ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoFocusOnAppearing);
    L.y = kBodyTop;
}
inline void end(Look&) {
    ImGui::End();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(4);
}

// The logo, 64 design pixels tall, centred in the top 76.
inline void logo(const Look& L, ImTextureID tex, float aspect) {
    const float h = 64, w = h * aspect;
    ImGui::GetWindowDrawList()->AddImage(tex, L.p((kPanelW - w) * 0.5f, 8), L.p((kPanelW + w) * 0.5f, 8 + h));
}

// The tab bar: L1, the three pages, R1. Returns the page to show: a pressed tab's, the one before (L1) or after (R1)
// page, else page.
inline int tabs(const Look& L, int page, const char* const* names) {
    static const char* const kNames[3] = { "Upscaling", "Frame generation", "Overlay" };
    if (!names) names = kNames;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddLine(L.p(kRowL, kTabBottom), L.p(kRowR, kTabBottom), col(ImVec4(1, 1, 1, 0.08f)), std::max(1.0f, L.k));
    int to = page;
    auto hit = [&](const char* id, float x0, float x1) {
        ImGui::SetCursorScreenPos(L.p(x0, kTabTop));
        return ImGui::InvisibleButton(id, ImVec2((x1 - x0) * L.k, (kTabBottom - kTabTop) * L.k), ImGuiButtonFlags_PressedOnClick);
    };
    auto glyph = [&](const char* g, float cx) {
        const float w = 46, h = 32, y = (kTabTop + kTabBottom - h) * 0.5f;
        dl->AddRectFilled(L.p(cx - w / 2, y), L.p(cx + w / 2, y + h), col(ImVec4(1, 1, 1, 0.04f)), 4 * L.k);
        dl->AddRect(L.p(cx - w / 2, y), L.p(cx + w / 2, y + h), col(ImVec4(0.42f, 0.45f, 0.50f, 1)), 4 * L.k, 0, 2 * L.k);
        text(L, L.caption, 17, cx - w / 2, y + (h - 17) * 0.5f - 1, w, ImVec4(0.81f, 0.84f, 0.87f, 1), g, 0.5f);
    };
    if (hit("##l1", kRowL, kRowL + 60)) to = wrapStep(page, 3, -1);
    glyph("L1", kRowL + 30);
    for (int i = 0; i < 3; ++i) {
        const float x0 = kRowL + 60 + 200 * i, x1 = x0 + 200;
        ImGui::PushID(i);
        if (hit("##tab", x0, x1)) to = i;
        ImGui::PopID();
        text(L, L.value, 24, x0, (kTabTop + kTabBottom - 24) * 0.5f, 200, i == page ? kText : kDim, names[i], 0.5f);
        if (i == page) dl->AddRectFilled(L.p(x0 + 24, kTabBottom - 3), L.p(x1 - 24, kTabBottom + 1), col(L.accHi));
    }
    if (hit("##r1", kRowR - 60, kRowR)) to = wrapStep(page, 3, 1);
    glyph("R1", kRowR - 30);
    return to;
}

// A heading in the body.
inline void section(Look& L, const char* name) {
    L.y += 10;
    char up[64]; size_t i = 0;
    for (; name[i] && i + 1 < sizeof up; ++i) up[i] = name[i] >= 'a' && name[i] <= 'z' ? char(name[i] - 32) : name[i];
    up[i] = 0;
    text(L, L.caption, 18, kRowL + 4, L.y + 3, kRowR - kRowL - 8, kDim, up);
    L.y += 24 + 6;
}
// One line under a row, shortened to fit.
inline void status(Look& L, const char* s, const ImVec4& c) {
    L.y -= 2;
    text(L, L.caption, 19, kRowL + 20, L.y + (30 - 19) * 0.5f - 1, kRowR - kRowL - 24, c, s);
    L.y += 30 + 6;
}

// A row's input: whether the controller's focus is on it, whether it is dimmed, and this frame's controller action
// (left / right -1 / +1, A), given only to the focused row. Its output: a press on it (focus it) and the step asked
// (-1 / +1; for a toggle, +1 flips it).
struct RowIn { bool focused = false, dim = false; int lr = 0; bool a = false; };
struct RowOut { bool press = false; int step = 0; };

// The row's background, focus bar and label; the row's button is added by rowButton after the row's own controls.
inline void rowFrame(const Look& L, const char* label, const RowIn& in) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float y = L.y;
    if (in.focused) {
        dl->AddRectFilled(L.p(kRowL, y), L.p(kRowR, y + kRowH), col(L.acc, 0.26f), 4 * L.k);
        dl->AddRectFilled(L.p(kRowL, y + 8), L.p(kRowL + 5, y + kRowH - 8), col(L.accHi), 2 * L.k, ImDrawFlags_RoundCornersRight);
    } else dl->AddRectFilled(L.p(kRowL, y), L.p(kRowR, y + kRowH), col(ImVec4(1, 1, 1, 0.035f)), 4 * L.k);
    text(L, L.label, 26, kRowL + 20, y + (kRowH - 26) * 0.5f - 1, kCtlL - kRowL - 24, kText, label);
}
// The whole row as one button (last, so the row's own controls take a press first). True on the frame it is pressed;
// the item is the row, so GetItemRect gives the row's rect.
inline bool rowButton(const Look& L, const char* id) {
    ImGui::SetCursorScreenPos(L.p(kRowL, L.y));
    return ImGui::InvisibleButton(id, ImVec2((kRowR - kRowL) * L.k, kRowH * L.k), ImGuiButtonFlags_PressedOnClick);
}
// Where the press that is still held began, in design pixels.
inline ImVec2 pressAt(const Look& L) { const ImVec2 m = ImGui::GetIO().MouseClickedPos[0]; return ImVec2(L.dx(m.x), L.dy(m.y)); }

// ◀ value ▶: the arrows step -1 / +1, a press elsewhere on the row +1; left / right step, A steps +1.
inline RowOut cycler(Look& L, const char* id, const char* label, const char* value, RowIn in) {
    RowOut out;
    ImGui::BeginDisabled(in.dim);
    rowFrame(L, label, in);
    const float y = L.y, t = y + (kRowH - 52) * 0.5f;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    for (float x0 : { kCtlL, kCtlR - kArrowW }) dl->AddRectFilled(L.p(x0, t), L.p(x0 + kArrowW, t + 52), col(L.acc, 0.30f), 4 * L.k);
    arrow(L, kCtlL, t, kCtlL + kArrowW, t + 52, -1, kText);
    arrow(L, kCtlR - kArrowW, t, kCtlR, t + 52, 1, kText);
    text(L, L.value, 24, kCtlL + kArrowW + 10, y + (kRowH - 24) * 0.5f - 1, kCtlR - kCtlL - 2 * kArrowW - 20, kText, value, 0.5f);
    if (rowButton(L, id)) {
        out.press = true;
        const ImVec2 at = pressAt(L);
        out.step = inside(at.x, at.y, kCtlL, t, kCtlL + kArrowW, t + 52) ? -1 : 1;
    }
    ImGui::EndDisabled();
    if (!in.dim && in.focused && !out.press) out.step = in.lr ? in.lr : in.a ? 1 : 0;
    L.y += kRowH + 4;
    return out;
}

// A tick box with On / Off beside it: a press, left / right or A flips it (step +1).
inline RowOut toggle(Look& L, const char* id, const char* label, bool on, RowIn in) {
    RowOut out;
    ImGui::BeginDisabled(in.dim);
    rowFrame(L, label, in);
    const float y = L.y, t = y + (kRowH - 52) * 0.5f;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(L.p(kCtlL, t), L.p(kCtlL + 52, t + 52), col(ImVec4(1, 1, 1, 0.08f)), 4 * L.k);
    if (on) {
        const ImVec2 pts[3] = { L.p(kCtlL + 14, t + 27), L.p(kCtlL + 22, t + 36), L.p(kCtlL + 39, t + 16) };
        dl->AddPolyline(pts, 3, col(L.accHi), ImDrawFlags_None, 4 * L.k);
    }
    text(L, L.value, 22, kCtlL + 52 + 16, y + (kRowH - 22) * 0.5f - 1, 80, kDim, on ? "On" : "Off");
    if (rowButton(L, id)) { out.press = true; out.step = 1; }
    ImGui::EndDisabled();
    if (!in.dim && in.focused && !out.press && (in.lr || in.a)) out.step = 1;
    L.y += kRowH + 4;
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
    const float f = hi > lo ? float(v - lo) / float(hi - lo) : 0, gx = kCtlL + (kTrackR - kCtlL) * f, cy = y + kRowH * 0.5f;
    dl->AddRectFilled(L.p(kCtlL, cy - 5), L.p(kTrackR, cy + 5), col(ImVec4(1, 1, 1, 0.12f)), 3 * L.k);
    dl->AddRectFilled(L.p(kCtlL, cy - 5), L.p(gx, cy + 5), col(L.acc), 3 * L.k);
    dl->AddRectFilled(L.p(gx - 12, cy - 22), L.p(gx + 12, cy + 22), col(L.accHi), 4 * L.k);
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
        dl->AddRectFilled(L.p(bx, t), L.p(kCtlR, t + 52), col(ImVec4(0, 0, 0, 0.25f)), 4 * L.k);
        dl->AddRect(L.p(bx, t), L.p(kCtlR, t + 52), col(ImVec4(0.29f, 0.32f, 0.36f, 1)), 4 * L.k, 0, std::max(1.0f, L.k));
        char s[24];
        std::snprintf(s, sizeof s, "%d%s", v, unit);
        text(L, L.value, 24, bx + 4, y + (kRowH - 24) * 0.5f - 1, kBoxW - 8, kText, s, 0.5f);
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
    if (!in.dim && in.focused && typing.id != rid) {
        if (in.lr) v = clampStep(v, lo, hi, step, in.lr);
        else if (in.a) typing = Typing{ rid, true, false, 0 };
    }
    ImGui::PopID();
    L.y += kRowH + 4;
    if (outp) *outp = out;
    return v != before;
}

// The footer: the controller's buttons and what they do, and a note on the right.
inline void footer(const Look& L, const char* note) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddLine(L.p(0, kFootTop), L.p(kPanelW, kFootTop), col(ImVec4(1, 1, 1, 0.08f)), std::max(1.0f, L.k));
    const float cy = kFootTop + 32, gh = 32;
    float x = 24;
    auto badge = [&](const char* g, float w, bool round) {
        const ImVec2 a = L.p(x, cy - gh / 2), b = L.p(x + w, cy + gh / 2);
        dl->AddRectFilled(a, b, col(ImVec4(1, 1, 1, 0.04f)), round ? gh * 0.5f * L.k : 4 * L.k);
        dl->AddRect(a, b, col(ImVec4(0.42f, 0.45f, 0.50f, 1)), round ? gh * 0.5f * L.k : 4 * L.k, 0, 2 * L.k);
        if (g) text(L, L.caption, 17, x, cy - 17 * 0.5f - 1, w, ImVec4(0.81f, 0.84f, 0.87f, 1), g, 0.5f);
        else { arrow(L, x + 4, cy - 12, x + w / 2, cy + 12, -1, kText); arrow(L, x + w / 2, cy - 12, x + w - 4, cy + 12, 1, kText); }
        x += w + 8;
    };
    auto word = [&](const char* s) {
        text(L, L.caption, 19, x, cy - 19 * 0.5f - 1, 160, kDim, s);
        x += L.caption->CalcTextSizeA(19 * L.k, FLT_MAX, 0, s).x / L.k + 22;
    };
    badge("A", 34, true); word("Select");
    badge("B", 34, true); word("Close");
    badge(nullptr, 62, false); word("Change");
    const float nw = L.caption->CalcTextSizeA(16 * L.k, FLT_MAX, 0, note).x / L.k;
    text(L, L.caption, 16, kPanelW - 24 - nw, cy - 8 - 1, nw + 1, kDim, note);
}

}  // namespace deck
