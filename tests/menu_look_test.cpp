// Host test for src/menu_look.h: the desktop menu's look helpers (option stepping, toggle animation timings, the frame
// time graph's samples, scale and x placement, the counter's text and width) and the option row driven by keyboard,
// controller and mouse in a headless Dear ImGui context. Expected values are the intended behaviour
// (0.18 s with a peak of 1.05..1.15, a pulse of 1 to 1.9 over 0.42 s, 0.12 s off, a 0.2 s time constant, a 2 s window,
// 22 px arrow slots, 230 px minimum width), written out here, not recomputed the way the code does.
#include "menu_look.h"
#include "imgui_internal.h"
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstring>

static int g_fail;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++g_fail; } } while (0)
static bool near(double a, double b, double eps = 1e-4) { return std::fabs(a - b) <= eps; }

static void steps() {
    CHECK(look::stepWrap(0, -1, 5) == 4);  // past the first: the last
    CHECK(look::stepWrap(4, 1, 5) == 0);   // past the last: the first
    CHECK(look::stepWrap(2, 1, 5) == 3);
    CHECK(look::stepWrap(2, -1, 5) == 1);
    CHECK(look::stepWrap(0, 1, 1) == 0);   // one value: stays
    CHECK(look::stepWrap(0, -1, 1) == 0);
}

static void toggles() {
    // on: the inner square grows from 0.2 to 1 over 0.18 s, overshooting to between 1.05 and 1.15 on the way
    CHECK(near(look::toggleInner(true, 0).scale, 0.2));
    CHECK(near(look::toggleInner(true, 0.18f).scale, 1));
    CHECK(near(look::toggleInner(true, 0.5f).scale, 1)); CHECK(near(look::toggleInner(true, 0.5f).alpha, 1));
    float peak = 0;
    for (int ms = 0; ms <= 180; ++ms) peak = std::max(peak, look::toggleInner(true, ms / 1000.f).scale);
    CHECK(peak >= 1.05f); CHECK(peak <= 1.15f);
    CHECK(look::toggleInner(true, 0.09f).scale > 1.0f);  // the overshoot happens inside the 0.18 s
    // off: shrinks and fades out over 0.12 s
    CHECK(near(look::toggleInner(false, 0).scale, 1)); CHECK(near(look::toggleInner(false, 0).alpha, 1));
    const look::Anim mid = look::toggleInner(false, 0.06f);
    CHECK(mid.alpha > 0.01f && mid.alpha < 0.99f); CHECK(mid.scale < 1 && mid.scale > 0.2f);
    CHECK(near(look::toggleInner(false, 0.12f).alpha, 0)); CHECK(near(look::toggleInner(false, 0.12f).scale, 0.2));
    CHECK(near(look::toggleInner(false, 5).alpha, 0));
    // the pulse ring: scale 1 to 1.9 and alpha 0.9 to 0 over 0.42 s
    CHECK(near(look::pulse(0).scale, 1)); CHECK(near(look::pulse(0).alpha, 0.9));
    CHECK(near(look::pulse(0.42f).scale, 1.9)); CHECK(near(look::pulse(0.42f).alpha, 0));
    const look::Anim p = look::pulse(0.21f);
    CHECK(p.scale > 1 && p.scale < 1.9f); CHECK(p.alpha > 0 && p.alpha < 0.9f);
    CHECK(near(look::pulse(3).alpha, 0));
    // per-toggle state: the first sight of a toggle does not animate (the menu opening), a change does, from its time
    ImGuiStorage st;
    const float first = look::toggleAge(st, 42, true, 10.0f);
    CHECK(first >= 1.0f);                                       // past every animation
    CHECK(look::toggleAge(st, 42, true, 10.5f) >= 1.0f);        // unchanged later: still still
    CHECK(near(look::toggleAge(st, 42, false, 11.0f), 0));      // switched off at 11 s
    CHECK(near(look::toggleAge(st, 42, false, 11.1f), 0.1));
    CHECK(near(look::toggleAge(st, 42, true, 12.0f), 0));       // on again
    CHECK(look::toggleAge(st, 7, false, 12.0f) >= 1.0f);        // another toggle, first seen: no animation
}

static void scale() {
    // frame-rate independent: two steps of dt/2 land where one step of dt does
    const float two = look::easeScale(look::easeScale(10, 30, 0.008f), 30, 0.008f), one = look::easeScale(10, 30, 0.016f);
    CHECK(near(two, one));
    const float four = look::easeScale(look::easeScale(look::easeScale(look::easeScale(5, 50, 0.025f), 50, 0.025f), 50, 0.025f), 50, 0.025f);
    CHECK(near(four, look::easeScale(5, 50, 0.1f)));
    // 0.2 s time constant: after 0.2 s, 1 - 1/e of the way
    CHECK(near(look::easeScale(0, 1, 0.2f), 1 - std::exp(-1.0), 1e-5));
    // a 10x spike moves the scale less than 10 % of the way in one 16 ms frame
    const float moved = (look::easeScale(10, 100, 0.016f) - 10) / (100 - 10);
    CHECK(moved > 0 && moved < 0.10f);
    CHECK(near(look::easeScale(20, 20, 0.5f), 20));  // at the target: stays
}

static void ring() {
    look::FrameRing r;
    r.record(1.0, 1);
    CHECK(r.size() == 0);                                  // the first call only stores the time
    r.record(1.02, 2);                                     // 20 ms that produced two frames
    CHECK(r.size() == 2);
    CHECK(near(r.time(0), 1.01, 1e-9)); CHECK(near(r.time(1), 1.02, 1e-9));
    CHECK(near(r.ms(0), 10, 1e-3)); CHECK(near(r.ms(1), 10, 1e-3));
    r.record(1.025, 0);                                    // nothing presented: no sample, the time is not moved
    CHECK(r.size() == 2);
    r.record(1.03, 1);
    CHECK(r.size() == 3); CHECK(near(r.ms(2), 10, 1e-3)); CHECK(near(r.time(2), 1.03, 1e-9));
    r.record(3.2, 1);                                      // a long stall: the older samples are past 2.1 s
    CHECK(r.size() == 1); CHECK(near(r.time(0), 3.2, 1e-9)); CHECK(near(r.ms(0), 2170, 1e-2));
    // overflow keeps the newest 1024, oldest first
    look::FrameRing o;
    o.record(10.0, 1);
    for (int k = 1; k <= 2000; ++k) o.record(10.0 + k * 0.001, 1);
    CHECK(o.size() == 1024);
    CHECK(near(o.time(1023), 12.0, 1e-9));
    CHECK(near(o.time(0), 12.0 - 1023 * 0.001, 1e-9));
    for (int i = 1; i < o.size(); ++i) if (!(o.time(i) > o.time(i - 1))) { CHECK(o.time(i) > o.time(i - 1)); break; }
    // x is the sample's age over 2 s: now at the right edge, 1 s in the middle, 2 s at the left
    CHECK(near(look::ageX(5.0, 5.0, 100, 200), 300));
    CHECK(near(look::ageX(5.0, 4.0, 100, 200), 200));
    CHECK(near(look::ageX(5.0, 3.0, 100, 200), 100));
    CHECK(near(look::ageX(5.25, 5.0, 0, 400), 350));      // a redraw a quarter second later moves it by an eighth
}

static void counterText() {
    char b[32];
    look::msText(b, sizeof b, 240); CHECK(std::strcmp(b, "4.2 ms") == 0);
    look::msText(b, sizeof b, 60); CHECK(std::strcmp(b, "16.7 ms") == 0);
    look::msText(b, sizeof b, 1000); CHECK(std::strcmp(b, "1.0 ms") == 0);
    look::msText(b, sizeof b, 0); CHECK(std::strcmp(b, "") == 0);  // nothing counted yet
    // the width comes from the fixed slots and the info lines only (no live number goes in): 230 px at least, padding
    // 18 px a side
    CHECK(near(look::counterWidth(1, 100, 120), 230));
    CHECK(near(look::counterWidth(1, 250, 120), 286));
    CHECK(near(look::counterWidth(1, 120, 260), 296));
    CHECK(near(look::counterWidth(2, 250, 300), 460));
}

// ---- an option row driven by keyboard, controller and mouse in a headless Dear ImGui context -------------------------
static int g_a = 0, g_b = 1, g_slide = 50, g_changes = 0;
static bool g_typing = false, g_focusFirst = false;
static ImGuiID g_comboA, g_comboB;
static float g_rowX, g_rowY, g_rowW, g_rowH;
static ImVec2 g_boxMin, g_boxMax;
static const char* const kItems[] = { "One", "Two", "Three" };
constexpr float kW = 300;

static void frame(const pad::PadView& v = pad::PadView{}) {
    ImGuiIO& io = ImGui::GetIO();
    pad::feedPad(io, v, true);
    ImGui::NewFrame();
    ImGui::SetNextWindowPos(ImVec2(0, 0)); ImGui::SetNextWindowSize(ImVec2(600, 300));
    ImGui::Begin("menu", nullptr, ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoMove);
    if (g_focusFirst) { pad::focusFirst(); g_focusFirst = false; }
    const ImVec2 p = ImGui::GetCursorScreenPos();
    g_rowX = p.x; g_rowY = p.y; g_rowH = ImGui::GetFrameHeight(); g_rowW = kW;
    ImGui::SetNextItemWidth(kW);
    g_comboA = ImGui::GetID("##a");
    if (look::option("##a", &g_a, kItems, 3)) ++g_changes;
    ImGui::SetNextItemWidth(kW);
    g_comboB = ImGui::GetID("##b");
    if (look::option("##b", &g_b, kItems, 3)) ++g_changes;
    g_typing = false;
    pad::typedSlider("##s", g_slide, 0, 100, 5, "%d%%", 64, &g_typing);
    g_boxMin = ImGui::GetItemRectMin(); g_boxMax = ImGui::GetItemRectMax();
    ImGui::End();
    ImGui::Render();
}
static void idle(int n = 3) { for (int i = 0; i < n; ++i) frame(); }
static void key(ImGuiKey k, int held = 0) {
    ImGui::GetIO().AddKeyEvent(k, true); frame();
    for (int i = 0; i < held; ++i) frame();
    ImGui::GetIO().AddKeyEvent(k, false); idle();
}
static pad::PadView padTap(uint16_t bit) { pad::PadView v; v.connected = true; v.edges = bit; return v; }
static void click(float x, float y) {
    ImGuiIO& io = ImGui::GetIO();
    io.AddMousePosEvent(x, y); frame();
    io.AddMouseButtonEvent(0, true); frame();
    io.AddMouseButtonEvent(0, false); frame();
    io.AddMousePosEvent(-FLT_MAX, -FLT_MAX); idle();
}
static bool popupOpen() { return GImGui->OpenPopupStack.Size > 0; }  // a list is open
static bool anyPopup() { return popupOpen(); }

static void optionRow() {
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.DisplaySize = ImVec2(800, 600);
    io.DeltaTime = 1.0f / 60;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad | ImGuiConfigFlags_NavEnableKeyboard;
    io.BackendFlags |= ImGuiBackendFlags_HasGamepad;
    unsigned char* px; int w, h;
    io.Fonts->GetTexDataAsRGBA32(&px, &w, &h);
    look::ctx().s = 1;

    idle(2);
    g_focusFirst = true; idle();
    CHECK(GImGui->NavId == g_comboA);                     // the first option has the focus

    // keyboard right / left: one step per press, wrapping, only the focused option
    key(ImGuiKey_RightArrow);
    CHECK(g_a == 1); CHECK(g_b == 1);
    key(ImGuiKey_LeftArrow); key(ImGuiKey_LeftArrow);
    CHECK(g_a == 2);                                      // 1, 0, then wrapped to the last
    key(ImGuiKey_RightArrow);
    CHECK(g_a == 0);                                      // past the last: the first
    CHECK(GImGui->NavId == g_comboA);                     // the arrows are not navigation stops
    // held well past the key repeat delay: still one step
    key(ImGuiKey_RightArrow, 90);
    CHECK(g_a == 1);
    // the D-pad: one step per press, held or not
    frame(padTap(pad::kRight)); idle();
    CHECK(g_a == 2);
    pad::PadView held; held.connected = true; held.buttons = held.edges = pad::kLeft;
    frame(held); held.edges = 0;
    for (int i = 0; i < 90; ++i) frame(held);
    frame(); idle();
    CHECK(g_a == 1);
    CHECK(GImGui->NavId == g_comboA);
    CHECK(g_b == 1);                                      // the other option never moved
    // Tab goes through every navigation stop in order: from the first option straight to the second, no arrow between
    key(ImGuiKey_Tab);
    CHECK(GImGui->NavId == g_comboB);
    key(ImGuiKey_UpArrow);
    CHECK(GImGui->NavId == g_comboA);
    // keyboard down moves to the next row; right then steps that one
    key(ImGuiKey_DownArrow);
    CHECK(GImGui->NavId == g_comboB);
    key(ImGuiKey_RightArrow);
    CHECK(g_b == 2); CHECK(g_a == 1);
    key(ImGuiKey_UpArrow);
    CHECK(GImGui->NavId == g_comboA);

    // Enter on the slider adjusts it like Space: no hidden text entry on the slider itself (its box takes typing)
    key(ImGuiKey_DownArrow); key(ImGuiKey_DownArrow);
    CHECK(GImGui->NavId != g_comboA && GImGui->NavId != g_comboB && GImGui->NavId != 0);
    ImGui::GetIO().AddKeyEvent(ImGuiKey_Enter, true); frame(); frame();
    CHECK(!pad::textInputActive());
    ImGui::GetIO().AddKeyEvent(ImGuiKey_Enter, false); idle();
    key(ImGuiKey_Escape);                                 // leaves the slider
    key(ImGuiKey_UpArrow); key(ImGuiKey_UpArrow);
    CHECK(GImGui->NavId == g_comboA);

    // Space, Enter and A open the list; nothing steps while it is open
    key(ImGuiKey_Space);
    CHECK(popupOpen());
    const int before = g_a;
    key(ImGuiKey_RightArrow);
    CHECK(g_a == before);
    key(ImGuiKey_Escape); idle();
    CHECK(!anyPopup());
    g_focusFirst = true; idle();
    key(ImGuiKey_Enter);
    CHECK(popupOpen());
    key(ImGuiKey_Escape); idle();
    CHECK(!anyPopup());
    g_focusFirst = true; idle();
    frame(padTap(pad::kA)); idle();
    CHECK(popupOpen());
    frame(padTap(pad::kB)); idle();
    CHECK(!anyPopup());

    // the mouse: a click on ‹ or › steps (22 px slots at each end of the row), a click on the value opens the list
    g_focusFirst = true; idle();
    const ImGuiID nav = GImGui->NavId;
    const int a0 = g_a;
    const float midY = g_rowY + g_rowH * 0.5f;
    click(g_rowX + 11, midY);
    CHECK(g_a == look::stepWrap(a0, -1, 3));
    click(g_rowX + g_rowW - 11, midY);
    click(g_rowX + g_rowW - 11, midY);
    CHECK(g_a == look::stepWrap(a0, 1, 3));
    CHECK(!anyPopup());                                   // the arrows step, they do not open the list
    CHECK(GImGui->NavId == nav || GImGui->NavId == 0 || GImGui->NavId == g_comboA);  // never an arrow
    click(g_rowX + g_rowW * 0.5f, midY);
    CHECK(popupOpen());
    key(ImGuiKey_Escape); idle();

    // keyboard arrows step only with keyboard navigation on (the Steam Deck layout clears it)
    g_focusFirst = true; idle();
    io.ConfigFlags &= ~ImGuiConfigFlags_NavEnableKeyboard;
    const int a1 = g_a;
    key(ImGuiKey_RightArrow);
    CHECK(g_a == a1);
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    key(ImGuiKey_RightArrow);
    CHECK(g_a == look::stepWrap(a1, 1, 3));

    // while a box is typed in, the arrows reach the box, not the options or the slider
    const int a2 = g_a, s2 = g_slide;
    click((g_boxMin.x + g_boxMax.x) * 0.5f, (g_boxMin.y + g_boxMax.y) * 0.5f);
    io.AddMousePosEvent((g_boxMin.x + g_boxMax.x) * 0.5f, (g_boxMin.y + g_boxMax.y) * 0.5f);
    io.AddMouseButtonEvent(0, true); frame(); io.AddMouseButtonEvent(0, false); frame();
    CHECK(g_typing);
    CHECK(pad::padLeftRight(false) == 0);
    io.AddKeyEvent(ImGuiKey_RightArrow, true); frame();
    CHECK(pad::padLeftRight(false) == 0);
    io.AddKeyEvent(ImGuiKey_RightArrow, false); frame();
    CHECK(g_a == a2); CHECK(g_slide == s2);
    key(ImGuiKey_Escape);                                 // Esc leaves the box
    CHECK(!g_typing); CHECK(g_slide == s2);
    io.AddMousePosEvent(-FLT_MAX, -FLT_MAX); idle();
    ImGui::DestroyContext();
}

// The slider row's box shows the setting as the slider did ("Off", "35%") while idle; Enter and Esc as before.
static void sliderBox() {
    char b[16];
    pad::boxText(b, sizeof b, 0, "Off", false); CHECK(std::strcmp(b, "Off") == 0);
    pad::boxText(b, sizeof b, 35, "%d%%", false); CHECK(std::strcmp(b, "35%") == 0);
    pad::boxText(b, sizeof b, 240, "%d fps", false); CHECK(std::strcmp(b, "240 fps") == 0);
    pad::boxText(b, sizeof b, 240, "%d fps", true); CHECK(std::strcmp(b, "240") == 0);  // typed in: digits
    CHECK(pad::typedValue("35%", 0, 100, 7) == 35);
    CHECK(pad::typedValue("Off", 0, 100, 7) == 7);
}

// A box whose idle text has a unit ("240 fps"): from the click that starts typing, the box holds the digits only, so
// editing part of the number and pressing Enter sets it (Home, Delete, "1", Enter gives 140).
static int g_cap = 240;
static ImVec2 g_capMin, g_capMax;
static void capFrame() {
    ImGui::NewFrame();
    ImGui::SetNextWindowPos(ImVec2(0, 0)); ImGui::SetNextWindowSize(ImVec2(600, 300));
    ImGui::Begin("cap", nullptr, ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoMove);
    bool typing = false;
    pad::typedSlider("##cap", g_cap, 30, 480, 5, "%d fps", 90, &typing);
    g_capMin = ImGui::GetItemRectMin(); g_capMax = ImGui::GetItemRectMax();
    ImGui::End();
    ImGui::Render();
}
static void capKey(ImGuiKey k) { ImGuiIO& io = ImGui::GetIO(); io.AddKeyEvent(k, true); capFrame(); io.AddKeyEvent(k, false); capFrame(); }
static void boxActivation() {
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr; io.DisplaySize = ImVec2(800, 600); io.DeltaTime = 1.0f / 60;
    unsigned char* px; int w, h;
    io.Fonts->GetTexDataAsRGBA32(&px, &w, &h);
    capFrame(); capFrame();
    const ImVec2 c((g_capMin.x + g_capMax.x) * 0.5f, (g_capMin.y + g_capMax.y) * 0.5f);
    io.AddMousePosEvent(c.x, c.y); capFrame();
    io.AddMouseButtonEvent(0, true); capFrame();
    io.AddMouseButtonEvent(0, false); capFrame();
    capKey(ImGuiKey_Home); capKey(ImGuiKey_Delete);      // "240" becomes "40"
    io.AddInputCharacter('1'); capFrame(); capFrame();    // "140"
    capKey(ImGuiKey_Enter);
    CHECK(g_cap == 140);
    ImGui::DestroyContext();
}

// The menu's panel leaves the scrollbar's gutter out of its fill, so the scrollbar drawn before it stays visible: with
// contents taller than the window the fill stops at the scrollbar's left edge; with no scrollbar it
// reaches the window's right edge.
static bool g_tall;
static float g_fillEnd, g_barX, g_winR, g_winTop, g_winBot;
static int g_fillPast;
static void panelFrame() {
    ImGui::NewFrame();
    ImGui::SetNextWindowPos(ImVec2(0, 0)); ImGui::SetNextWindowSize(ImVec2(400, 200));
    ImGui::Begin("panel", nullptr, ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoTitleBar);
    ImGuiWindow* win = ImGui::GetCurrentWindow();
    g_fillEnd = look::panelEnd(win);
    g_barX = win->ScrollbarY ? ImGui::GetWindowScrollbarRect(win, ImGuiAxis_Y).Min.x : -1;
    g_winR = win->Pos.x + win->Size.x; g_winTop = win->Pos.y; g_winBot = win->Pos.y + win->Size.y;
    const int v0 = win->DrawList->VtxBuffer.Size;
    look::panel(win->DrawList, win->Pos, ImVec2(g_winR, g_winBot), look::kMenu, g_fillEnd);
    g_fillPast = 0;  // fill vertices (away from the streak on top and the line at the bottom) right of the fill's end
    for (int i = v0; i < win->DrawList->VtxBuffer.Size; ++i) {
        const ImDrawVert& d = win->DrawList->VtxBuffer[i];
        if (d.pos.y > g_winTop + 6 && d.pos.y < g_winBot - 2 && d.pos.x > g_fillEnd + 0.5f) ++g_fillPast;
    }
    ImGui::Dummy(ImVec2(10, g_tall ? 600 : 20));
    ImGui::End();
    ImGui::Render();
}
static void panelGutter() {
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr; io.DisplaySize = ImVec2(800, 600); io.DeltaTime = 1.0f / 60;
    unsigned char* px; int w, h;
    io.Fonts->GetTexDataAsRGBA32(&px, &w, &h);
    look::ctx().s = 1;
    g_tall = true; panelFrame(); panelFrame(); panelFrame();
    CHECK(g_barX > 0);                                    // the contents overflow: a scrollbar
    CHECK(near(g_fillEnd, g_barX));                       // the fill stops where the scrollbar starts
    CHECK(g_fillEnd < g_winR - 4);
    CHECK(g_fillPast == 0);
    g_tall = false; panelFrame(); panelFrame(); panelFrame();
    CHECK(g_barX < 0);                                    // no scrollbar
    CHECK(near(g_fillEnd, g_winR));                       // the fill reaches the window's edge
    ImGui::DestroyContext();
}

int main() {
    steps();
    toggles();
    scale();
    ring();
    counterText();
    sliderBox();
    boxActivation();
    panelGutter();
    optionRow();
    if (g_fail) { std::printf("%d checks failed\n", g_fail); return 1; }
    std::printf("menu_look_test: all checks passed\n");
    return 0;
}
