// Host test for src/menu_pad.h: the controller combo that opens the Insert menu, the latch that keeps a button held at
// the close from reaching the game, and the menu driven only by controller input in a headless Dear ImGui context.
// Expected values are written out here (hold 500 ms, steps 1 and 5, the hint words), not taken from the code.
#include "menu_pad.h"
#include "imgui_internal.h"
#include <cstdio>
#include <cfloat>
#include <cstring>

static int g_fail;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++g_fail; } } while (0)

using namespace pad;

static void comboHold() {
    ComboHold h;
    CHECK(!h.update(true, 1000));          // both down: timing starts
    CHECK(!h.update(true, 1499));          // 499 ms: not yet
    CHECK(h.update(true, 1500));           // 500 ms: toggles once
    CHECK(!h.update(true, 1600));          // still held: no second toggle
    CHECK(!h.update(true, 9000));          // however long
    CHECK(!h.update(false, 9010));         // released: re-arms
    CHECK(!h.update(true, 9020));
    CHECK(h.update(true, 9520));           // a second hold toggles again
    ComboHold q;                           // a quick press of both does nothing
    CHECK(!q.update(true, 0)); CHECK(!q.update(true, 300)); CHECK(!q.update(false, 320)); CHECK(!q.update(true, 330));
    CHECK(!q.update(true, 829));           // the earlier 300 ms do not count towards the new hold
    CHECK(q.update(true, 830));
}

static void slots() {
    PadSlots p;
    // L3 on pad 0 and R3 on pad 1: not the combo
    p.record(0, true, kL3, 0, 0, 100); p.record(1, true, kR3, 0, 0, 100);
    PadView v = p.merge(110);
    CHECK(v.connected); CHECK(!v.comboDown); CHECK((v.buttons & (kL3 | kR3)) == (kL3 | kR3));
    p.record(1, true, kL3 | kR3, 0, 0, 120);
    CHECK(p.merge(125).comboDown);
    // an A tap between two frames still counts once as an edge
    p.record(0, true, kA, 0, 0, 130); p.record(0, true, 0, 0, 0, 131);
    v = p.merge(140);
    CHECK((v.edges & kA) != 0); CHECK((v.buttons & kA) == 0);
    CHECK((p.merge(150).edges & kA) == 0);  // taken once
    // a slot not updated for more than 250 ms is left out; a disconnect clears it
    PadSlots s;
    s.record(2, true, kB, 0, 0, 1000);
    CHECK(s.merge(1250).connected);
    CHECK(!s.merge(1251).connected);
    s.record(2, true, kB, 0, 0, 2000); s.record(2, false, 0, 0, 0, 2001);
    CHECK(!s.merge(2002).connected); CHECK((s.merge(2002).buttons & kB) == 0);
    // the stick comes from the pad pushed furthest
    PadSlots t;
    t.record(0, true, 0, 3000, 0, 10); t.record(1, true, 0, 0, -20000, 10);
    v = t.merge(20);
    CHECK(v.lx == 0); CHECK(v.ly == -20000);
}

static void latch() {
    PadSlots p;
    CHECK(p.hide(0, kB | kA) == (kB | kA));  // nothing latched: unchanged
    p.latchAll();                             // the menu closes while B is held
    CHECK(p.hide(0, kB) == 0);                // still held: hidden
    CHECK(p.hide(0, kB) == 0);
    CHECK(p.hide(0, 0) == 0);                 // released
    CHECK(p.hide(0, kB) == kB);               // pressed again: the game sees it
    p.latchAll();
    CHECK(p.hide(1, kL3 | kR3) == 0);         // the combo's sticks after a close by the combo
    CHECK(p.hide(1, kR3) == 0);               // L3 let go first, R3 still hidden
    CHECK(p.hide(1, kR3 | kA) == kA);         // a new press of another button shows at once
    CHECK(p.hide(1, 0) == 0);
    CHECK(p.hide(1, kL3) == kL3);
}

static void words() {
    CHECK(std::strcmp(hintA(false), "Change") == 0);
    CHECK(std::strcmp(hintA(true), "Pick") == 0);
    CHECK(std::strcmp(hintB(false), "Close menu") == 0);
    CHECK(std::strcmp(hintB(true), "Back") == 0);
}

// ---- the menu driven only by controller input, in a headless Dear ImGui context ------------------------------------
static int g_combo = 1, g_slider = 50;
static bool g_tick = false, g_hints = false, g_drawn = false, g_opened = false;
struct Seen { int focused = -1; bool popup = false, busy = false; float height = 0; ImVec2 sliderMin, sliderMax; };

static Seen frame(const PadView& v, bool feedB = true) {
    ImGuiIO& io = ImGui::GetIO();
    feedPad(io, v, feedB);
    ImGui::NewFrame();
    Seen s;
    if (!g_drawn) { ImGui::Render(); return s; }  // the menu closed: not drawn at all
    ImGui::SetNextWindowPos(ImVec2(10, 10));
    ImGui::Begin("menu", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings);
    if (g_opened) { focusFirst(); g_opened = false; }
    const char* items[] = { "One", "Two", "Three" };
    ImGui::Combo("##combo", &g_combo, items, 3);
    if (ImGui::IsItemFocused()) s.focused = 0;
    padSlider("##slider", g_slider, 0, 100, 5, "%d");
    if (ImGui::IsItemFocused()) s.focused = 1;
    s.sliderMin = ImGui::GetItemRectMin(); s.sliderMax = ImGui::GetItemRectMax();
    ImGui::Checkbox("##tick", &g_tick);
    if (ImGui::IsItemFocused()) s.focused = 2;
    padTick(g_tick);
    if (g_hints) drawHints(s.popup, s.busy, 1.0f);
    else ImGui::TextDisabled("Insert or Esc to close \xC2\xB7 changes apply and save immediately");
    s.popup = ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopup);
    s.busy = s.popup || ImGui::IsAnyItemActive();
    s.height = ImGui::GetWindowHeight();
    ImGui::End();
    ImGui::Render();
    return s;
}
static Seen idle(int n = 4) { Seen s; for (int i = 0; i < n; ++i) s = frame(PadView{}); return s; }
static PadView tapOf(uint16_t bit) { PadView v; v.connected = true; v.edges = bit; return v; }
static Seen tap(uint16_t bit) { frame(tapOf(bit)); return idle(); }

static void navigation() {
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.DisplaySize = ImVec2(800, 600);
    io.DeltaTime = 1.0f / 60;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;
    io.BackendFlags |= ImGuiBackendFlags_HasGamepad;
    unsigned char* px; int w, h;
    io.Fonts->GetTexDataAsRGBA32(&px, &w, &h);

    // opened by the combo: the outline is on the first setting at once
    idle(2);
    g_drawn = g_opened = true;
    Seen s = frame(PadView{});
    s = frame(PadView{});
    CHECK(s.focused == 0);
    CHECK(GImGui->NavCursorVisible);

    // D-pad down: one setting per tap, including a tap that is over within one frame
    CHECK(tap(kDown).focused == 1);
    CHECK(tap(kDown).focused == 2);
    CHECK(tap(kUp).focused == 1);
    CHECK(tap(kUp).focused == 0);
    // the left stick moves too
    PadView stick; stick.connected = true; stick.ly = -30000;
    frame(stick); frame(stick); s = idle();
    CHECK(s.focused == 1);
    stick.ly = -9000;                 // inside the dead zone plus a tenth: no move
    frame(stick); frame(stick); s = idle();
    CHECK(s.focused == 1);
    stick.ly = 0; stick.lx = 30000;   // right on the slider steps it, like the D-pad
    frame(stick); s = idle();
    CHECK(s.focused == 1); CHECK(g_slider == 55);
    g_slider = 50;
    s = tap(kUp);
    CHECK(s.focused == 0);
    stick.lx = 0; stick.ly = -30000; stick.comboDown = true;  // both sticks held in: the stick reads as centred
    frame(stick); frame(stick); s = idle();
    CHECK(s.focused == 0);

    // A opens the list, down moves in it, A picks; B closes it unchanged
    s = tap(kA);
    CHECK(s.popup); CHECK(s.busy);
    tap(kDown);
    s = tap(kA);
    CHECK(!s.popup); CHECK(g_combo == 2);
    s = tap(kA);
    CHECK(s.popup);
    tap(kUp);
    s = tap(kB);
    CHECK(!s.popup); CHECK(g_combo == 2); CHECK(!s.busy);

    // left/right on the focused slider: one step of 5, within its range
    tap(kDown);
    s = tap(kRight);
    CHECK(s.focused == 1); CHECK(g_slider == 55);
    tap(kLeft); tap(kLeft);
    CHECK(g_slider == 45);
    g_slider = 98;
    tap(kRight);
    CHECK(g_slider == 100);
    tap(kRight);
    CHECK(g_slider == 100);
    // left/right on the tick box: off and on; A toggles
    tap(kDown);
    tap(kRight);
    CHECK(g_tick);
    tap(kRight);
    CHECK(g_tick);
    tap(kLeft);
    CHECK(!g_tick);
    tap(kA);
    CHECK(g_tick);
    // B not fed (the menu closes on it instead): nothing in the menu reacts
    s = tap(kA);
    CHECK(!g_tick);
    frame(tapOf(kB), false); s = idle();
    CHECK(!s.popup);

    // holding down on the D-pad or the stick moves one setting per press, however long it is held
    tap(kUp); tap(kUp);
    CHECK(idle().focused == 0);
    PadView held; held.connected = true; held.edges = held.buttons = kDown;
    frame(held); held.edges = 0;
    for (int i = 0; i < 90; ++i) frame(held);         // 1.5 s, well past ImGui's key repeat delay
    CHECK(idle().focused == 1);
    CHECK(tap(kDown).focused == 2);                   // released and pressed again: one more
    held.buttons = kUp; held.edges = kUp; frame(held); held.edges = 0;
    for (int i = 0; i < 90; ++i) frame(held);
    CHECK(idle().focused == 1);
    tap(kUp);
    PadView tilt; tilt.connected = true; tilt.ly = -30000;
    for (int i = 0; i < 90; ++i) frame(tilt);
    CHECK(idle().focused == 1);
    for (int i = 0; i < 3; ++i) frame(tilt);          // back to centre, then down again: one more
    CHECK(idle().focused == 2);

    // A on the slider (as the footer offers), then left/right: still the slider's own step of 5
    tap(kUp);
    g_slider = 50;
    s = tap(kA);
    CHECK(s.focused == 1); CHECK(s.busy);
    tap(kRight);
    CHECK(g_slider == 55);
    tap(kLeft); tap(kLeft);
    CHECK(g_slider == 45);
    held.buttons = held.edges = kRight; frame(held); held.edges = 0;
    for (int i = 0; i < 90; ++i) frame(held);
    idle();
    CHECK(g_slider > 50); CHECK(g_slider % 5 == 0);
    s = tap(kB);
    CHECK(!s.busy); CHECK(s.focused == 1);
    // a mouse drag on the slider is not held to the controller's step
    g_slider = 50;
    s = idle();
    const float my = (s.sliderMin.y + s.sliderMax.y) * 0.5f;
    io.AddMousePosEvent(s.sliderMin.x + 4, my); idle(1);
    io.AddMouseButtonEvent(0, true); idle(1);
    io.AddMousePosEvent(s.sliderMax.x - 4, my); idle(1);
    io.AddMouseButtonEvent(0, false); idle(2);
    CHECK(g_slider == 100);
    io.AddMousePosEvent(-FLT_MAX, -FLT_MAX); idle(1);
    g_slider = 50;

    // closed for a single frame (the release frame), the combo reopens the menu on its first setting
    tap(kDown); tap(kDown); tap(kUp);
    CHECK(idle().focused == 1);
    g_drawn = false; frame(PadView{});
    g_drawn = g_opened = true; frame(PadView{});
    CHECK(frame(PadView{}).focused == 0);

    // the hints take the footer line's own height: the window does not change size
    const float without = idle().height;
    g_hints = true;
    const float with = idle().height;
    CHECK(without > 0); CHECK(with == without);
    g_hints = false;

    // a mouse click hides the outline; with no controller input it stays hidden
    io.AddMousePosEvent(20, 20); idle(1);
    io.AddMouseButtonEvent(0, true); idle(1); io.AddMouseButtonEvent(0, false); idle(2);
    CHECK(!GImGui->NavCursorVisible);
    ImGui::DestroyContext();
}

// Empty controller slots: an answer of "not connected" is reused until the slot's re-ask time (slot 0 1 s, slot 1 2 s,
// slot 2 3 s, slot 3 4 s, from the read that got it); a connected slot is always asked; the counts are logged once,
// 30 s after the first read. Numbers are written out here.
static void emptySlots() {
    const uint64_t wait[4] = { 1000, 2000, 3000, 4000 };
    for (unsigned i = 0; i < 4; ++i) {
        const uint64_t w = wait[i];
        EmptySlots e;
        CHECK(!e.skip(i, 5000));                      // never answered: the first read asks
        e.answer(i, true, 10000);                     // empty
        CHECK(e.skip(i, 10000));
        CHECK(e.skip(i, 10000 + w - 1));              // still inside the window: answered by the mod
        CHECK(!e.skip(i, 10000 + w));                 // re-ask time: asked
        const uint64_t t = 10000 + w;
        e.answer(i, true, t);                         // still empty: a new window from that read
        CHECK(e.skip(i, t + 1)); CHECK(e.skip(i, t + w - 1)); CHECK(!e.skip(i, t + w));
        const uint64_t u = t + w;
        e.answer(i, false, u);                        // a pad answered: asked on every read from now on
        CHECK(!e.skip(i, u)); CHECK(!e.skip(i, u + 1)); CHECK(!e.skip(i, u + 100000));
        e.answer(i, true, u + 200000);                // it went away (a real read saw it): cached again
        CHECK(e.skip(i, u + 200001));
        e.answer(i, false, u + 200002);               // another thread's read finds a pad inside the window: asked again
        CHECK(!e.skip(i, u + 200003));
        for (unsigned j = 0; j < 4; ++j)              // slots are independent
            if (j != i) CHECK(!e.skip(j, 10001));
    }
    EmptySlots o;                                     // slot numbers above 3: always asked, never counted
    o.answer(4, true, 1000); o.answer(0xFFFFFFFFu, true, 1000);
    CHECK(!o.skip(4, 1001)); CHECK(!o.skip(0xFFFFFFFFu, 1001));
    for (unsigned j = 0; j < 4; ++j) CHECK(o.asked[j].load() == 0 && o.cached[j].load() == 0);

    EmptySlots c;                                     // counts: 2 asks and 5 answers from the mod on slot 2
    CHECK(!c.logDue(1000000));                        // no read yet: nothing to log
    CHECK(!c.skip(2, 1000)); c.answer(2, true, 1000);
    for (uint64_t k = 0; k < 5; ++k) CHECK(c.skip(2, 1100 + k));
    CHECK(!c.skip(2, 4000)); c.answer(2, true, 4000);
    CHECK(c.asked[2].load() == 2 && c.cached[2].load() == 5);
    for (unsigned j : { 0u, 1u, 3u }) CHECK(c.asked[j].load() == 0 && c.cached[j].load() == 0);
    CHECK(!c.logDue(1000 + 29999));                   // the first read was at 1000
    CHECK(c.logDue(1000 + 30000));                    // 30 s later: once
    CHECK(!c.logDue(1000 + 30001)); CHECK(!c.logDue(1000 + 900000));
}

// A value typed into a setting's box: what a person would type (spaces, a % sign, a fraction, a sign) is read and
// clamped to the setting's range; nothing usable keeps the value as it was. Ranges are the menu's (0 to 200, 0 to 100).
static void typed() {
    CHECK(typedValue("150", 0, 200, 100) == 150);
    CHECK(typedValue("0", 0, 200, 100) == 0);
    CHECK(typedValue("200", 0, 200, 100) == 200);
    CHECK(typedValue("  75  ", 0, 200, 100) == 75);
    CHECK(typedValue("150%", 0, 200, 100) == 150);
    CHECK(typedValue("150 %", 0, 200, 100) == 150);
    CHECK(typedValue("007", 0, 200, 100) == 7);
    CHECK(typedValue("999", 0, 200, 100) == 200);              // over the top: the top
    CHECK(typedValue("99999999999999999999", 0, 200, 100) == 200);
    CHECK(typedValue("101", 0, 100, 30) == 100);
    CHECK(typedValue("-5", 0, 200, 100) == 0);                 // below the bottom: the bottom
    CHECK(typedValue("+40", 0, 200, 100) == 40);
    CHECK(typedValue("150.4", 0, 200, 100) == 150);            // fractions round to the nearest whole
    CHECK(typedValue("150.5", 0, 200, 100) == 151);
    CHECK(typedValue(".5", 0, 200, 100) == 1);
    CHECK(typedValue("", 0, 200, 100) == 100);                 // nothing usable: unchanged
    CHECK(typedValue("   ", 0, 200, 60) == 60);
    CHECK(typedValue("abc", 0, 200, 100) == 100);
    CHECK(typedValue("1x5", 0, 200, 100) == 100);
    CHECK(typedValue("1 5", 0, 200, 100) == 100);
    CHECK(typedValue("-", 0, 200, 100) == 100);
    CHECK(typedValue("%", 0, 200, 100) == 100);
    CHECK(typedValue("1.2.3", 0, 200, 100) == 100);
    CHECK(typedValue("10*2", 0, 200, 100) == 100);
}

// A setting's typed box, driven as a player would: a click on the box, keys through TypingFeed (Windows key codes),
// in a headless Dear ImGui context. Enter sets the typed value (clamped), Esc leaves it, Up and Down step it at once;
// left and right on the controller still step the slider and never move to the box.
static int g_box = 100;
static bool g_typing;
static ImVec2 g_boxMin, g_boxMax;
static bool boxFrame(TypingFeed& keys, const int* down, bool click, bool focusSlider = false, uint16_t pad = 0) {
    ImGuiIO& io = ImGui::GetIO();
    keys.feed(io, [&](int vk) { for (const int* d = down; d && *d; ++d) if (*d == vk) return true; return false; });
    const ImVec2 mid((g_boxMin.x + g_boxMax.x) * 0.5f, (g_boxMin.y + g_boxMax.y) * 0.5f);
    if (focusSlider || pad) io.AddMousePosEvent(-FLT_MAX, -FLT_MAX);  // the controller alone
    else io.AddMousePosEvent(mid.x, mid.y);
    io.AddMouseButtonEvent(0, click);
    feedPad(io, tapOf(pad), true);
    ImGui::NewFrame();
    ImGui::SetNextWindowPos(ImVec2(0, 0)); ImGui::SetNextWindowSize(ImVec2(600, 200));
    ImGui::Begin("box", nullptr, ImGuiWindowFlags_NoSavedSettings);
    if (focusSlider) focusFirst();
    g_typing = false;
    const bool changed = typedSlider("##game", g_box, 0, 200, 5, "%d%%", 64, &g_typing);
    g_boxMin = ImGui::GetItemRectMin(); g_boxMax = ImGui::GetItemRectMax();
    ImGui::End();
    ImGui::Render();
    return changed;
}
static void press(TypingFeed& keys, int vk) {
    const int down[] = { vk, 0 };
    boxFrame(keys, down, false);
    boxFrame(keys, nullptr, false);
}
static void typedBox() {
    ImGuiContext* ctx = ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.DisplaySize = ImVec2(800, 600);
    io.DeltaTime = 1.0f / 60;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;
    io.BackendFlags |= ImGuiBackendFlags_HasGamepad;
    unsigned char* px; int w, h;
    io.Fonts->GetTexDataAsRGBA32(&px, &w, &h);
    TypingFeed keys;
    auto clickBox = [&] { boxFrame(keys, nullptr, true); boxFrame(keys, nullptr, false); };

    boxFrame(keys, nullptr, false); boxFrame(keys, nullptr, false);   // laid out: the box's place is known
    CHECK(g_boxMax.x - g_boxMin.x == 64);                              // the box keeps its width
    CHECK(!g_typing);
    clickBox();
    CHECK(g_typing);                                                   // a click on the box starts typing
    press(keys, '1'); press(keys, '5'); press(keys, '0');
    CHECK(g_box == 100);                                               // nothing set until Enter
    press(keys, 0x0D);                                                 // Enter
    CHECK(g_box == 150);
    boxFrame(keys, nullptr, false);
    CHECK(!g_typing);

    clickBox(); press(keys, '9'); press(keys, '9'); press(keys, '9'); press(keys, 0x0D);
    CHECK(g_box == 200);                                               // over the top: the top
    clickBox(); press(keys, 0x60 + 4); press(keys, 0x6E); press(keys, 0x60 + 6); press(keys, 0x0D);  // keypad 4.6
    CHECK(g_box == 5);                                                 // rounded
    clickBox(); press(keys, '4'); press(keys, '2'); press(keys, 0x1B); // Esc
    CHECK(g_box == 5);                                                 // left as it was
    boxFrame(keys, nullptr, false);
    CHECK(!g_typing);
    clickBox(); press(keys, 0x26);                                     // Up: at once
    CHECK(g_box == 6);
    press(keys, 0x28); press(keys, 0x28);                              // Down twice
    CHECK(g_box == 4);
    press(keys, 0x0D);
    CHECK(g_box == 4);
    clickBox(); press(keys, 0x08); press(keys, 0x0D);                  // everything deleted, then Enter: unchanged
    CHECK(g_box == 4);

    // the controller, in a menu just opened (focus on its first setting): right on the slider steps it by 5 and the
    // focus stays on the slider
    ImGui::DestroyContext(ctx);
    ctx = ImGui::CreateContext();
    ImGuiIO& io2 = ImGui::GetIO();
    io2.IniFilename = nullptr; io2.DisplaySize = ImVec2(800, 600); io2.DeltaTime = 1.0f / 60;
    io2.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad; io2.BackendFlags |= ImGuiBackendFlags_HasGamepad;
    io2.Fonts->GetTexDataAsRGBA32(&px, &w, &h);
    keys = TypingFeed{};
    g_box = 100;
    boxFrame(keys, nullptr, false, true); boxFrame(keys, nullptr, false);
    CHECK(GImGui->NavId != 0);                                         // the slider has the focus
    const unsigned slider = GImGui->NavId;
    boxFrame(keys, nullptr, false, false, kRight); boxFrame(keys, nullptr, false);
    CHECK(g_box == 105);
    CHECK(GImGui->NavId == slider);
    boxFrame(keys, nullptr, false, false, kRight); boxFrame(keys, nullptr, false);
    CHECK(g_box == 110);
    CHECK(GImGui->NavId == slider);
    ImGui::DestroyContext(ctx);
}

int main() {
    typed();
    typedBox();
    emptySlots();
    comboHold();
    slots();
    latch();
    words();
    navigation();
    if (g_fail) { std::printf("%d checks failed\n", g_fail); return 1; }
    std::printf("menu_pad_test: all checks passed\n");
    return 0;
}
