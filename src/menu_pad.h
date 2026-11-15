// Controller support for the Insert menu, kept free of Windows types so tests/menu_pad_test.cpp covers it on the host.
// The game's controller reads (any thread) publish each pad's state here without a lock; the present thread merges
// them once a frame, times the open/close combo and feeds Dear ImGui's controller navigation.
#pragma once
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include "imgui.h"
#include "imgui_internal.h"  // ImGuiContext::ActiveIdSource: tells a slider adjusted with A from one dragged with the mouse

namespace pad {

// Controller button bits as XInput reports them (menu.cpp checks them against Xinput.h)
constexpr uint16_t kUp = 0x0001, kDown = 0x0002, kLeft = 0x0004, kRight = 0x0008, kL3 = 0x0040, kR3 = 0x0080,
                   kL1 = 0x0100, kR1 = 0x0200, kA = 0x1000, kB = 0x2000;  // L1 / R1: the Steam Deck layout's pages
constexpr uint64_t kFreshMs = 250;    // a pad not read for longer than this is left out
constexpr uint64_t kHoldMs = 500;     // how long both sticks are held in to open or close the menu
constexpr int kStickDeadzone = 7849;  // the left stick's dead zone, as XInput defines it

// One frame's view of the controllers: buttons held now, buttons pressed since the last frame, and the left stick of the
// pad pushed furthest.
struct PadView {
    uint16_t buttons = 0, edges = 0;
    int16_t lx = 0, ly = 0;
    bool connected = false, comboDown = false;  // comboDown: L3 and R3 held together on one pad
};

struct PadSlots {
    struct Slot {
        std::atomic<uint64_t> state{ 0 };  // buttons | lx << 16 | ly << 32 | connected << 48
        std::atomic<uint64_t> tick{ 0 };
        std::atomic<uint16_t> edges{ 0 }, latch{ 0 };
    };
    static_assert(std::atomic<uint64_t>::is_always_lock_free && std::atomic<uint16_t>::is_always_lock_free,
                  "the game's controller reads must never wait");
    Slot s[4];

    static uint64_t pack(uint16_t b, int16_t lx, int16_t ly) {
        return uint64_t(b) | uint64_t(uint16_t(lx)) << 16 | uint64_t(uint16_t(ly)) << 32 | uint64_t(1) << 48;
    }
    // A read of pad i by the game: ok false when no pad is connected there.
    void record(unsigned i, bool ok, uint16_t b, int16_t lx, int16_t ly, uint64_t now) {
        if (i >= 4) return;
        Slot& x = s[i];
        if (!ok) { x.state.store(0); x.edges.store(0); x.latch.store(0); return; }
        const uint64_t prev = x.state.exchange(pack(b, lx, ly));
        x.edges.fetch_or(uint16_t(b & ~uint16_t(prev)));
        x.tick.store(now);
    }
    // The buttons the game may see from pad i while the menu is closed: one held when the menu closed stays hidden
    // until it is released.
    uint16_t hide(unsigned i, uint16_t b) {
        if (i >= 4) return b;
        const uint16_t held = s[i].latch.fetch_and(b) & b;
        return uint16_t(b & ~held);
    }
    void latchAll() { for (Slot& x : s) x.latch.store(0xFFFF); }

    PadView merge(uint64_t now) {
        PadView v;
        int best = -1;
        for (Slot& x : s) {
            const uint64_t st = x.state.load();
            const uint16_t e = x.edges.exchange(0);
            if (!(st >> 48 & 1) || x.tick.load() + kFreshMs < now) continue;
            const uint16_t b = uint16_t(st);
            const int16_t lx = int16_t(uint16_t(st >> 16)), ly = int16_t(uint16_t(st >> 32));
            v.connected = true;
            v.buttons |= b; v.edges |= e;
            if ((b & (kL3 | kR3)) == (kL3 | kR3)) v.comboDown = true;
            const int mag = std::abs(int(lx)) + std::abs(int(ly));
            if (mag > best) { best = mag; v.lx = lx; v.ly = ly; }
        }
        return v;
    }
};

// How long a slot that answered "not connected" is answered from the cache before XInput is asked again. Slot 0 is
// where the first controller goes, so it is re-asked soonest.
constexpr uint64_t kSlotRecheckMs[4] = { 1000, 2000, 3000, 4000 };

// Controller slots with nothing in them. Asking XInput about an empty slot is slow, so after it answers "not connected"
// the game gets that answer from us until the slot's re-ask time; a connected slot is asked on every read, so a
// disconnect shows at once. Called from the game's controller reads on any thread, without a lock. The counts feed one
// log line, 30 s after the first read.
struct EmptySlots {
    std::atomic<uint64_t> until[4] = {};  // re-ask time per slot; 0: ask
    std::atomic<uint32_t> asked[4] = {}, cached[4] = {};
    std::atomic<uint64_t> first{ 0 };     // tick of the first read; 0: none yet
    std::atomic<bool> logged{ false };
    static_assert(std::atomic<uint32_t>::is_always_lock_free && std::atomic<bool>::is_always_lock_free,
                  "the game's controller reads must never wait");

    // True when slot i is answered "not connected" from the cache for this read.
    bool skip(unsigned i, uint64_t now) {
        uint64_t none = 0;
        first.compare_exchange_strong(none, now);
        if (i >= 4 || now >= until[i].load()) return false;
        cached[i].fetch_add(1);
        return true;
    }
    // XInput's answer for slot i: empty starts a new window from this read; connected clears it.
    void answer(unsigned i, bool empty, uint64_t now) {
        if (i >= 4) return;
        until[i].store(empty ? now + kSlotRecheckMs[i] : 0);
        asked[i].fetch_add(1);
    }
    // True exactly once: on the first call at least 30 s after the first read.
    bool logDue(uint64_t now) {
        const uint64_t f = first.load();
        return f && now >= f + 30000 && !logged.exchange(true);
    }
};

// A value typed into a setting's box, clamped to lo..hi: a number with optional spaces around it, a sign, a fraction
// (rounded to the nearest whole) and a % sign after it. Anything else, or nothing, gives keep.
inline int typedValue(const char* text, int lo, int hi, int keep) {
    const char* p = text;
    while (*p == ' ') ++p;
    const bool neg = *p == '-';
    if (*p == '-' || *p == '+') ++p;
    double v = 0, scale = 1;
    bool digits = false, point = false;
    for (; (*p >= '0' && *p <= '9') || (*p == '.' && !point); ++p) {
        if (*p == '.') { point = true; continue; }
        digits = true;
        if (point) v += (scale /= 10) * (*p - '0');
        else if (v < 1e9) v = v * 10 + (*p - '0');  // past this it only clamps to hi
    }
    while (*p == ' ') ++p;
    if (*p == '%') { ++p; while (*p == ' ') ++p; }
    if (!digits || *p) return keep;
    const double r = neg ? -(v + 0.5) : v + 0.5;
    return r < lo ? lo : r > hi ? hi : int(r);
}

// Both sticks held in: true once when the hold reaches kHoldMs; again only after a release. Present thread only.
struct ComboHold {
    uint64_t since = 0;
    bool timing = false, fired = false;
    bool update(bool both, uint64_t now) {
        if (!both) { timing = fired = false; return false; }
        if (!timing) { timing = true; since = now; }
        if (fired || now - since < kHoldMs) return false;
        fired = true;
        return true;
    }
};

// Feeds the menu the D-pad, A and (when feedB) B. The left stick pushed past its dead zone counts as the D-pad (ImGui
// itself moves between items with the D-pad only and scrolls with the stick). A press already over by this frame goes
// in as down then up, which ImGui applies on two frames. Up and down go in only as such a tap, once per press or per
// tilt of the stick, so holding them moves one setting, not one per key repeat. While both sticks are held in the
// stick reads as centred, so the combo does not move the focus. X, Y, shoulders, triggers and the right stick are
// never fed (X opens ImGui's window mover). Present thread only (it keeps the stick's last up/down state).
inline bool stickPast(int value, int sign) {  // XInput's dead zone plus a tenth of the travel beyond it
    const float n = float(value * sign - kStickDeadzone) / float(32767 - kStickDeadzone);
    return n > 0.10f;
}
struct StickUpDown { bool up = false, down = false; };
inline StickUpDown& lastStick() { static StickUpDown s; return s; }
inline void feedPad(ImGuiIO& io, const PadView& v, bool feedB) {
    const int lx = v.comboDown ? 0 : v.lx, ly = v.comboDown ? 0 : v.ly;
    auto button = [&](ImGuiKey k, uint16_t bit, bool stick) {
        const bool down = (v.buttons & bit) != 0 || stick;
        if ((v.edges & bit) && !down) { io.AddKeyEvent(k, true); io.AddKeyEvent(k, false); }
        else io.AddKeyEvent(k, down);
    };
    StickUpDown& was = lastStick();
    auto once = [&](ImGuiKey k, uint16_t bit, bool stick, bool& stickWas) {  // a tap on a new press or tilt, else up
        const bool press = (v.edges & bit) || (stick && !stickWas);
        stickWas = stick;
        io.AddKeyEvent(k, press);
        if (press) io.AddKeyEvent(k, false);
    };
    once(ImGuiKey_GamepadDpadUp, kUp, stickPast(ly, 1), was.up); once(ImGuiKey_GamepadDpadDown, kDown, stickPast(ly, -1), was.down);
    button(ImGuiKey_GamepadDpadLeft, kLeft, stickPast(lx, -1)); button(ImGuiKey_GamepadDpadRight, kRight, stickPast(lx, 1));
    button(ImGuiKey_GamepadFaceDown, kA, false);
    if (feedB) button(ImGuiKey_GamepadFaceRight, kB, false);
    else io.AddKeyEvent(ImGuiKey_GamepadFaceRight, false);
}
// Every controller key up: on the frame after the menu closes, so nothing stays pressed for the next open.
inline void releasePad(ImGuiIO& io) {
    const ImGuiKey keys[] = { ImGuiKey_GamepadDpadUp, ImGuiKey_GamepadDpadDown, ImGuiKey_GamepadDpadLeft,
                              ImGuiKey_GamepadDpadRight, ImGuiKey_GamepadFaceDown, ImGuiKey_GamepadFaceRight };
    for (ImGuiKey k : keys) io.AddKeyEvent(k, false);
    lastStick() = {};
}

// True while a text box (or a slider's own text entry) has the keys.
inline bool textInputActive() {
    const ImGuiContext& g = *GImGui;
    return g.IO.WantTextInput || (g.ActiveId && g.InputTextState.ID == g.ActiveId);
}
// -1 left, 1 right, 0 neither: pressed this frame (with key repeat unless `repeat` is false) on the D-pad or the left
// stick (fed as the D-pad), or on the keyboard's left / right arrows while keyboard navigation is on and no box is being
// typed in (the Steam Deck layout turns keyboard navigation off).
inline int padLeftRight(bool repeat = true) {
    if (ImGui::IsKeyPressed(ImGuiKey_GamepadDpadLeft, repeat)) return -1;
    if (ImGui::IsKeyPressed(ImGuiKey_GamepadDpadRight, repeat)) return 1;
    if ((ImGui::GetIO().ConfigFlags & ImGuiConfigFlags_NavEnableKeyboard) && !textInputActive()) {
        if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow, repeat)) return -1;
        if (ImGui::IsKeyPressed(ImGuiKey_RightArrow, repeat)) return 1;
    }
    return 0;
}
// Called right after a slider: left/right on it while it has the focus (and is not being adjusted) steps it.
inline bool padStep(int& v, int lo, int hi, int step) {
    if (!ImGui::IsItemFocused() || ImGui::IsItemActive()) return false;
    const int d = padLeftRight();
    const int n = std::clamp(v + d * step, lo, hi);
    if (!d || n == v) return false;
    v = n;
    return true;
}
// A slider the controller and the keyboard also step: SliderInt, then left/right by `step` while it has the focus, also
// after A or Space has made it active (ImGui's own step there, a hundredth of the range, is undone). A mouse drag or
// typed value is left alone. True when the value changed.
inline bool padSlider(const char* id, int& v, int lo, int hi, int step, const char* fmt, ImGuiSliderFlags flags = 0) {
    const int before = v;
    ImGui::SliderInt(id, &v, lo, hi, fmt, flags);
    const ImGuiInputSource src = GImGui->ActiveIdSource;
    if (ImGui::IsItemActive() && (src == ImGuiInputSource_Gamepad || (src == ImGuiInputSource_Keyboard && !textInputActive()))) {
        v = std::clamp(before + padLeftRight() * step, lo, hi);
    } else padStep(v, lo, hi, step);
    return v != before;
}
// Called right after a tick box: left turns it off, right on.
inline bool padTick(bool& v) {
    if (!ImGui::IsItemFocused() || ImGui::IsItemActive()) return false;
    const int d = padLeftRight();
    if (!d || (d > 0) == v) return false;
    v = d > 0;
    return true;
}

// The keys a setting's box and the desktop menu's keyboard navigation take, by Windows virtual-key code; menu.cpp polls
// them while the menu is open (the game sees no keys then). A digit key types its digit whatever the keyboard layout, with or without Shift.
struct TypingKey { int vk; ImGuiKey key; char ch; };
constexpr TypingKey kTypingKeys[] = {
    { 0x08, ImGuiKey_Backspace, 0 }, { 0x0D, ImGuiKey_Enter, 0 }, { 0x1B, ImGuiKey_Escape, 0 }, { 0x23, ImGuiKey_End, 0 },
    { 0x24, ImGuiKey_Home, 0 }, { 0x25, ImGuiKey_LeftArrow, 0 }, { 0x26, ImGuiKey_UpArrow, 0 }, { 0x27, ImGuiKey_RightArrow, 0 },
    { 0x28, ImGuiKey_DownArrow, 0 }, { 0x2E, ImGuiKey_Delete, 0 }, { 0x20, ImGuiKey_Space, 0 },  // Space: opens a list
    { 0x30, ImGuiKey_0, '0' }, { 0x31, ImGuiKey_1, '1' }, { 0x32, ImGuiKey_2, '2' }, { 0x33, ImGuiKey_3, '3' }, { 0x34, ImGuiKey_4, '4' },
    { 0x35, ImGuiKey_5, '5' }, { 0x36, ImGuiKey_6, '6' }, { 0x37, ImGuiKey_7, '7' }, { 0x38, ImGuiKey_8, '8' }, { 0x39, ImGuiKey_9, '9' },
    { 0x60, ImGuiKey_Keypad0, '0' }, { 0x61, ImGuiKey_Keypad1, '1' }, { 0x62, ImGuiKey_Keypad2, '2' }, { 0x63, ImGuiKey_Keypad3, '3' },
    { 0x64, ImGuiKey_Keypad4, '4' }, { 0x65, ImGuiKey_Keypad5, '5' }, { 0x66, ImGuiKey_Keypad6, '6' }, { 0x67, ImGuiKey_Keypad7, '7' },
    { 0x68, ImGuiKey_Keypad8, '8' }, { 0x69, ImGuiKey_Keypad9, '9' },
    { 0x6D, ImGuiKey_KeypadSubtract, '-' }, { 0x6E, ImGuiKey_KeypadDecimal, '.' }, { 0xBD, ImGuiKey_Minus, '-' }, { 0xBE, ImGuiKey_Period, '.' },
};
// Feeds those keys to the menu: each one that went down or up since the last call, and the character of one that went
// down. down(vk) says whether a key is held now. Present thread only.
struct TypingFeed {
    bool was[sizeof kTypingKeys / sizeof *kTypingKeys] = {};
    template <class Down> void feed(ImGuiIO& io, Down down) {
        for (size_t i = 0; i < sizeof kTypingKeys / sizeof *kTypingKeys; ++i) {
            const bool d = down(kTypingKeys[i].vk);
            if (d == was[i]) continue;
            was[i] = d;
            io.AddKeyEvent(kTypingKeys[i].key, d);
            if (d && kTypingKeys[i].ch) io.AddInputCharacter(kTypingKeys[i].ch);
        }
    }
};

// The text in a setting's box: the setting as `fmt` shows it ("Off", "35%", "240 fps") while idle, the bare number while
// it is typed in.
inline void boxText(char* buf, size_t n, int v, const char* fmt, bool typing) {
    if (typing) std::snprintf(buf, n, "%d", v);
    else std::snprintf(buf, n, fmt, v);
}
// A setting's slider (padSlider, its own text hidden) and, beside it, a box boxW wide that shows the value and that the
// value can be typed into: Enter sets the typed value (typedValue, clamped to lo..hi), Esc leaves it as it was, Up and
// Down step it by 1 at once. The box is left out of controller navigation, so left and right on the slider still step
// it. *typing is set while the box is being typed in. True when the value changed.
inline bool typedSlider(const char* id, int& v, int lo, int hi, int step, const char* fmt, float boxW, bool* typing) {
    ImGui::PushID(id);
    const float sp = ImGui::GetStyle().ItemSpacing.x;
    ImGui::SetNextItemWidth(std::max(1.0f, ImGui::GetContentRegionAvail().x - boxW - sp));
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0, 0, 0, 0));  // the box shows the value, and is where it is typed
    bool changed = padSlider("##slider", v, lo, hi, step, fmt, ImGuiSliderFlags_NoInput);
    ImGui::PopStyleColor();
    ImGui::SameLine(0, sp);
    ImGui::SetNextItemWidth(boxW);
    struct Step { int lo, hi, v; bool stepped; } st = { lo, hi, v, false };
    auto arrows = [](ImGuiInputTextCallbackData* d) {  // Up / Down: the value one up or down, set at once
        Step& s = *static_cast<Step*>(d->UserData);
        s.v = std::clamp(typedValue(d->Buf, s.lo, s.hi, s.v) + (d->EventKey == ImGuiKey_UpArrow ? 1 : -1), s.lo, s.hi);
        s.stepped = true;
        char t[16]; std::snprintf(t, sizeof t, "%d", s.v);
        d->DeleteChars(0, d->BufTextLen); d->InsertChars(0, t); d->SelectAll();
        return 0;
    };
    // The box always edits the digits alone, from the click that starts typing on; until then its own text is hidden
    // and the setting is drawn over it as the slider showed it ("240 fps", "Off").
    char buf[16];
    boxText(buf, sizeof buf, v, fmt, true);
    const bool wasTyping = GImGui->ActiveId == ImGui::GetID("##box");
    if (!wasTyping) ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0, 0, 0, 0));
    ImGui::PushItemFlag(ImGuiItemFlags_NoNav, true);
    const bool enter = ImGui::InputText("##box", buf, sizeof buf, ImGuiInputTextFlags_CharsDecimal | ImGuiInputTextFlags_EnterReturnsTrue |
                                        ImGuiInputTextFlags_AutoSelectAll | ImGuiInputTextFlags_CallbackHistory, arrows, &st);
    ImGui::PopItemFlag();
    if (!wasTyping) {
        ImGui::PopStyleColor();
        char label[16];
        boxText(label, sizeof label, v, fmt, ImGui::IsItemActive());  // the click's own frame: the digits it now edits
        const ImVec2 at(ImGui::GetItemRectMin().x + ImGui::GetStyle().FramePadding.x, ImGui::GetItemRectMin().y + ImGui::GetStyle().FramePadding.y);
        ImGui::GetWindowDrawList()->AddText(at, ImGui::GetColorU32(ImGuiCol_Text), label);
    }
    if (ImGui::IsItemActive()) *typing = true;
    const int now = enter ? typedValue(buf, lo, hi, v) : st.stepped ? st.v : v;
    changed |= now != v;
    v = now;
    ImGui::PopID();
    return changed;
}

// On the frame the combo opened the menu, right after Begin: the menu takes the focus and shows the outline at once,
// on its first setting.
inline void focusFirst() { ImGui::SetWindowFocus(); ImGui::SetNavCursorVisible(true); }

inline const char* hintA(bool popupOpen) { return popupOpen ? "Pick" : "Change"; }
inline const char* hintB(bool busy) { return busy ? "Back" : "Close menu"; }

// The controller hints, in exactly the height of one line of text (the footer line they stand in for): small badges
// with the button name, the D-pad as a drawn cross, and the A and B words in slots as wide as their longest word.
inline void drawHints(bool popupOpen, bool busy, float scale) {
    const float h = ImGui::GetTextLineHeight(), w = ImGui::GetContentRegionAvail().x;
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::Dummy(ImVec2(w, h));
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImU32 text = ImGui::GetColorU32(ImGuiCol_TextDisabled), plate = IM_COL32(0x2f, 0x33, 0x3a, 0xff),
                bright = ImGui::GetColorU32(ImGuiCol_Text), line = ImGui::GetColorU32(ImGuiCol_Separator);
    const float sp = ImGui::GetStyle().ItemSpacing.y, pad = 4 * scale, gap = 16 * scale, r = 4 * scale;
    dl->AddLine(ImVec2(p.x, p.y - sp * 0.5f), ImVec2(p.x + w, p.y - sp * 0.5f), line);  // in the gap above: moves nothing
    float x = p.x;
    auto badge = [&](const char* label) {  // returns nothing; advances x past the badge
        const float bw = label ? ImGui::CalcTextSize(label).x + 2 * pad : h;
        const float bwMin = std::max(bw, h);
        dl->AddRectFilled(ImVec2(x, p.y), ImVec2(x + bwMin, p.y + h), plate, r);
        if (label) dl->AddText(ImVec2(x + (bwMin - ImGui::CalcTextSize(label).x) * 0.5f, p.y), bright, label);
        else {  // the D-pad: a cross
            const float c = h * 0.5f, a = h * 0.32f, t = std::max(1.0f, h * 0.12f);
            dl->AddRectFilled(ImVec2(x + c - a, p.y + c - t), ImVec2(x + c + a, p.y + c + t), bright);
            dl->AddRectFilled(ImVec2(x + c - t, p.y + c - a), ImVec2(x + c + t, p.y + c + a), bright);
        }
        x += bwMin + pad;
    };
    auto word = [&](const char* s, const char* widest) {
        dl->AddText(ImVec2(x, p.y), text, s);
        x += ImGui::CalcTextSize(widest).x + gap;
    };
    badge(nullptr); word("Move", "Move");
    badge("A"); word(hintA(popupOpen), "Change");
    badge("B"); word(hintB(busy), "Close menu");
    badge("L3+R3"); word("Close menu", "Close menu");
}

}  // namespace pad
