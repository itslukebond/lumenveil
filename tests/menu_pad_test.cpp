// Host test for src/menu_pad.h: the controller combo that opens the Insert menu, the latch that keeps a button held at
// the close from reaching the game, and the menu driven only by controller input in a headless Dear ImGui context.
// Expected values come from the spec (docs: hold 500 ms, steps 1 and 5, the hint words), not from the code.
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

    // holding down on the D-pad or the stick moves one setting per press, however long it is held (AC5)
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

    // A on the slider (as the footer offers), then left/right: still the slider's own step of 5 (AC6)
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

    // closed for a single frame (the release frame), the combo reopens the menu on its first setting (AC5)
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

int main() {
    comboHold();
    slots();
    latch();
    words();
    navigation();
    if (g_fail) { std::printf("%d checks failed\n", g_fail); return 1; }
    std::printf("menu_pad_test: all checks passed\n");
    return 0;
}
