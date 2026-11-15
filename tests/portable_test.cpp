// Host test for the Portable mode switch, the live layout change, the pointing glove and the Steam Deck layout's look
// (src/menu_look.h, src/menu_deck.h, src/menu_pad.h) in a headless Dear ImGui context. Expected values come from the
// spec (docs/changes 2026-11-15-portable-mode-deck-restyle) and its signed-off mockup: the change lands 0.38 s after a
// flip, the knob glides for 0.15 s, the desktop track is 40 x 18 with the knob 9 px from its ends and the word in a 28 px
// slot 10 px after it, the Deck track 72 x 32 with the knob 15 px from its ends, the glove's 40 x 24 grid at 0.6 (24 x
// 14.4) on the desktop and 0.9 (36 x 21.6) on the Deck, its colours, the 1.1 s sway of 0 to 2 px, labels 48 px into a
// Deck row, and the footer's three lines.
#include "menu_deck.h"
#include "menu_look.h"
#include "menu_pad.h"
#include "imgui_internal.h"
#include <cmath>
#include <cstdio>
#include <algorithm>
#include <vector>

static int g_fail;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++g_fail; } } while (0)
static bool near(double a, double b, double eps = 1e-3) { return std::fabs(a - b) <= eps; }
static ImFont* g_font;

// ---- the pending change (AC5) -------------------------------------------------------------------------------------
static void swap() {
    deck::Swap s;
    CHECK(!s.due(0)); CHECK(!s.due(100));
    CHECK(s.shown(false) == false); CHECK(s.shown(true) == true);          // nothing pending: the layout in effect
    s.flip(true, 10.0, false);                                              // desktop open, switched On at 10 s
    CHECK(s.shown(false) == true);                                          // shows the new state at once
    CHECK(!s.due(10.0)); CHECK(!s.due(10.379));                             // not before 0.38 s
    CHECK(s.due(10.38)); CHECK(s.due(11.0));                                // from 0.38 s on
    s.flip(false, 10.2, false);                                             // flipped back inside 0.38 s: cancelled
    CHECK(!s.due(10.38)); CHECK(!s.due(20)); CHECK(s.shown(false) == false);
    deck::Swap t;
    t.flip(true, 5.0, true);                                                // to the layout already in effect: nothing
    CHECK(!t.due(5.5)); CHECK(t.shown(true) == true);
    deck::Swap u;
    u.flip(false, 3.0, true);                                               // Deck open, switched Off
    CHECK(u.shown(true) == false); CHECK(!u.due(3.3)); CHECK(u.due(3.38)); CHECK(u.want == 0);
}

// ---- draw-list helpers ------------------------------------------------------------------------------------------------
static ImDrawList* windowList(const char* name) { ImGuiWindow* w = ImGui::FindWindowByName(name); return w ? w->DrawList : nullptr; }
// The centre x of the last diamond (a square turned 45 degrees, filled) among vertices [from, to) of dl, or NAN.
// A filled convex shape is stored as (inner, outer) vertex pairs; the inner ones are the shape's own points.
static float lastDiamondX(const ImDrawList* dl, int from, int to, float* half = nullptr) {
    float found = NAN;
    for (int i = from; i + 8 <= to; ++i) {
        const ImVec2 a = dl->VtxBuffer[i].pos, b = dl->VtxBuffer[i + 2].pos, c = dl->VtxBuffer[i + 4].pos, d = dl->VtxBuffer[i + 6].pos;
        const float h = (c.y - a.y) * 0.5f;
        if (h > 1 && near(a.x, c.x, 0.01) && near(b.y, d.y, 0.01) && near(b.x - d.x, c.y - a.y, 0.05) && near(b.y, (a.y + c.y) * 0.5f, 0.05)
            && near(b.x, a.x + h, 0.05)) {
            found = a.x;
            if (half) *half = h;
        }
    }
    return found;
}
static ImU32 rgb(int r, int g, int b) { return IM_COL32(r, g, b, 255); }
static bool hasColour(const ImDrawList* dl, int from, int to, ImU32 c) {
    for (int i = from; i < to; ++i) if (dl->VtxBuffer[i].col == c) return true;
    return false;
}

// ---- the desktop switch (AC1, AC2, AC3, AC4) --------------------------------------------------------------------
struct Desk {
    bool on = false, enabled = true, focusAfter = false, cursor = false;
    int flips = 0, abovePresses = 0;
    ImRect sw; ImGuiID swId = 0, aboveId = 0;
    int vtx0 = 0, vtx1 = 0;
};
static Desk g_desk;
static void deskWindow() {
    Desk& d = g_desk;
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(ImVec2(600, 300));
    ImGui::Begin("desk", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove);
    if (ImGui::Button("Position")) ++d.abovePresses;
    d.aboveId = ImGui::GetItemID();
    d.vtx0 = ImGui::GetWindowDrawList()->VtxBuffer.Size;
    if (look::portable("##portable", d.on, d.enabled)) ++d.flips;
    d.vtx1 = ImGui::GetWindowDrawList()->VtxBuffer.Size;
    d.sw = ImRect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
    d.swId = ImGui::GetItemID();
    if (d.focusAfter) { d.focusAfter = false; look::focusItem(d.cursor); }
    ImGui::End();
}
static void deskFrame() {
    ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad | ImGuiConfigFlags_NavEnableKeyboard;
    ImGui::NewFrame();
    deskWindow();
    ImGui::Render();
}
static void mouse(float x, float y, bool down) { ImGuiIO& io = ImGui::GetIO(); io.AddMousePosEvent(x, y); io.AddMouseButtonEvent(0, down); }
static void key(ImGuiKey k) { ImGuiIO& io = ImGui::GetIO(); io.AddKeyEvent(k, true); deskFrame(); io.AddKeyEvent(k, false); deskFrame(); }
static void focusSwitch() { g_desk.focusAfter = true; g_desk.cursor = true; deskFrame(); deskFrame(); }

static void desktopSwitch() {
    Desk& d = g_desk;
    d = Desk{};
    deskFrame(); deskFrame();
    const float s = look::ctx().s;
    // the item spans the track, the gap and the word's slot: 40 + 10 + 28, the same On and Off
    CHECK(near(d.sw.GetWidth(), (40 + 10 + 28) * s));
    const float wOff = d.sw.GetWidth();
    // a click on the track flips it, and a click on the word flips it back
    const ImVec2 track(d.sw.Min.x + 20 * s, (d.sw.Min.y + d.sw.Max.y) * 0.5f), word(d.sw.Min.x + (40 + 10 + 14) * s, track.y);
    mouse(track.x, track.y, false); deskFrame(); mouse(track.x, track.y, true); deskFrame(); mouse(track.x, track.y, false); deskFrame();
    CHECK(d.on); CHECK(d.flips == 1);
    deskFrame();                                                              // drawn On
    CHECK(near(d.sw.GetWidth(), wOff));                                       // On: no edge moved
    mouse(word.x, word.y, false); deskFrame(); mouse(word.x, word.y, true); deskFrame(); mouse(word.x, word.y, false); deskFrame();
    CHECK(!d.on); CHECK(d.flips == 2);
    mouse(-100, -100, false); deskFrame();
    // A, Space and Enter flip it while it has the focus
    focusSwitch();
    CHECK(GImGui->NavId == d.swId);
    key(ImGuiKey_GamepadFaceDown); CHECK(d.on); CHECK(d.flips == 3);
    key(ImGuiKey_Space); CHECK(!d.on); CHECK(d.flips == 4);
    key(ImGuiKey_Enter); CHECK(d.on); CHECK(d.flips == 5);
    // left goes Off, right On, never the reverse; a second press the same way does nothing
    key(ImGuiKey_GamepadDpadRight); CHECK(d.on); CHECK(d.flips == 5);
    key(ImGuiKey_GamepadDpadLeft); CHECK(!d.on); CHECK(d.flips == 6);
    key(ImGuiKey_GamepadDpadLeft); CHECK(!d.on); CHECK(d.flips == 6);
    key(ImGuiKey_RightArrow); CHECK(d.on); CHECK(d.flips == 7);
    key(ImGuiKey_LeftArrow); CHECK(!d.on); CHECK(d.flips == 8);
    // not while another row has the focus
    ImGui::NewFrame(); ImGui::Begin("desk"); ImGui::End(); ImGui::Render();
    GImGui->NavId = 0;
    deskFrame();
    {
        ImGui::NewFrame();
        deskWindow();
        ImGui::Render();
        ImGui::NewFrame();
        ImGui::Begin("desk");
        ImGui::SetFocusID(d.aboveId, ImGui::GetCurrentWindow());
        ImGui::End();
        ImGui::Render();
    }
    deskFrame();
    CHECK(GImGui->NavId == d.aboveId);
    const int before = d.flips;
    key(ImGuiKey_Space); key(ImGuiKey_GamepadDpadRight); key(ImGuiKey_Enter);
    CHECK(d.flips == before); CHECK(!d.on);
    // the switch is a navigation stop: down from the row above reaches it
    key(ImGuiKey_GamepadDpadDown); deskFrame();
    CHECK(GImGui->NavId == d.swId);
    // disabled (the Deck fonts failed): a click, A and right take nothing
    d.enabled = false;
    focusSwitch();
    mouse(track.x, track.y, false); deskFrame(); mouse(track.x, track.y, true); deskFrame(); mouse(track.x, track.y, false); deskFrame();
    key(ImGuiKey_GamepadFaceDown); key(ImGuiKey_GamepadDpadRight);
    CHECK(d.flips == before); CHECK(!d.on);
    d.enabled = true;
    mouse(-100, -100, false); deskFrame();
}

// The knob: at the old end on the flip, between the ends halfway, at the new end from 0.15 s; a switch drawn for the
// first time shows its state without gliding. Desktop: ends 9 px in from a 40 px track; Deck: 15 px in from 72 px.
static void knob() {
    Desk& d = g_desk;
    d = Desk{};
    ImGuiIO& io = ImGui::GetIO();
    deskFrame(); deskFrame();
    const float s = look::ctx().s;
    auto knobX = [&]() { return lastDiamondX(windowList("desk"), d.vtx0, d.vtx1) - d.sw.Min.x; };
    CHECK(near(knobX(), 9 * s, 0.6));                                    // Off, still: the left end
    focusSwitch();
    io.AddKeyEvent(ImGuiKey_Space, true); deskFrame();                    // the flip lands on this frame
    CHECK(d.on);
    CHECK(near(knobX(), 9 * s, 0.6));                                    // at the flip: the old end
    io.AddKeyEvent(ImGuiKey_Space, false);
    const float dt = io.DeltaTime;
    io.DeltaTime = 0.075f; deskFrame();
    const float mid = knobX();
    CHECK(mid > 9 * s + 1 && mid < 31 * s - 1);                          // halfway: strictly between
    io.DeltaTime = 0.075f; deskFrame();
    CHECK(near(knobX(), 31 * s, 0.6));                                   // 0.15 s: the right end
    io.DeltaTime = 0.85f; deskFrame();
    CHECK(near(knobX(), 31 * s, 0.6));                                   // 1 s: still there
    // On: the track is lit (its border #cfe0ff and the blue fill), 40 x 18 from the item's left; Off it is not
    {
        const ImDrawList* dl = windowList("desk");
        float x0 = 1e9f, x1 = -1e9f, y0 = 1e9f, y1 = -1e9f;
        for (int i = d.vtx0; i < d.vtx1; ++i)
            if (dl->VtxBuffer[i].col == rgb(0xcf, 0xe0, 0xff)) {
                const ImVec2 p = dl->VtxBuffer[i].pos;
                x0 = std::min(x0, p.x); x1 = std::max(x1, p.x); y0 = std::min(y0, p.y); y1 = std::max(y1, p.y);
            }
        // the 40 x 18 border, a 1 px anti-aliased line: its outer edge 1 px beyond the rect each side
        CHECK(near(x0 - d.sw.Min.x, -1, 0.5) && near(x1 - x0, 40 * s + 2, 0.5) && near(y1 - y0, 18 * s + 2, 0.5));
        bool blue = false;
        for (int i = d.vtx0; i < d.vtx1; ++i) { const ImU32 c = dl->VtxBuffer[i].col; blue |= (c & 0xFFFFFF) == (IM_COL32(79, 141, 255, 0) & 0xFFFFFF) && (c >> 24) > 100; }
        CHECK(blue);
    }
    d.on = false; deskFrame(); io.DeltaTime = 1; deskFrame();
    CHECK(!hasColour(windowList("desk"), d.vtx0, d.vtx1, rgb(0xcf, 0xe0, 0xff)));
    io.DeltaTime = dt;
    // a switch not drawn on the frame before (the menu reopened, or the layout changed) does not glide
    d.on = true; deskFrame(); deskFrame();
    ImGui::NewFrame(); ImGui::Render();                                   // a frame without the menu
    d.on = false;                                                         // its state changed meanwhile
    deskFrame();
    CHECK(near(knobX(), 9 * s, 0.6));
    // the pure timing, both ways
    CHECK(near(look::knobAt(true, 0), 0)); CHECK(near(look::knobAt(false, 0), 1));
    CHECK(look::knobAt(true, 0.075f) > 0.01f && look::knobAt(true, 0.075f) < 0.99f);
    CHECK(near(look::knobAt(true, 0.15f), 1)); CHECK(near(look::knobAt(false, 0.15f), 0)); CHECK(near(look::knobAt(true, 1), 1));
    ImGuiStorage st;
    CHECK(look::switchAge(st, 9, true, 10, 1) >= 1);                      // first sight: still
    CHECK(near(look::switchAge(st, 9, false, 10.5f, 2), 0));              // flipped on the next frame: glides from now
    CHECK(near(look::switchAge(st, 9, false, 10.6f, 3), 0.1));
    CHECK(look::switchAge(st, 9, true, 11, 9) >= 1);                      // frames 4..8 not drawn: still again
}

// ---- the Deck layout's page frames ---------------------------------------------------------------------------------
static float g_w = 1280, g_h = 800;
struct DeckOut {
    deck::Look look;
    deck::RowOut portable;
    ImRect portableRect, rowRects[8];
    int nRows = 0;
    float bodyEnd = 0;
    int vtxFoot0 = 0, vtxFoot1 = 0;
};
static deck::Typing g_typing;
static int g_sharp = 50;
// One Deck page as drawMenu's Deck path lays it out, with the longest values; `focus` is the focused row, `a` this
// frame's A.
static DeckOut deckPage(int page, int focus = -1, bool a = false, bool portableOn = true) {
    DeckOut o;
    o.look = deck::Look::at(g_w, g_h, g_font, g_font, g_font);
    deck::Look& L = o.look;
    deck::begin(L);
    deck::tabs(L, page, nullptr);
    int row = 0;
    auto in = [&](bool dim = false) { deck::RowIn r; r.focused = row == focus; r.dim = dim; if (r.focused && !dim) r.a = a; return r; };
    auto took = [&](deck::RowOut out) { o.rowRects[o.nRows++] = ImRect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax()); ++row; return out; };
    if (page == 0) {
        deck::section(L, "Upscaling");
        took(deck::cycler(L, "##up", "Upscaler", "Auto  (FSR on this Deck)", in()));
        deck::status(L, "Intel XeSS 2.0.2, running on the Deck's AMD graphics.", deck::kDim);
        took(deck::toggle(L, "##en", "Enabled", true, in()));
        took(deck::cycler(L, "##scale", "Scaling mode", "Ultra Performance  \xC2\xB7  33%", in()));
        deck::status(L, "Intel XeSS needs a scaling mode of 50% or more, then a restart.", deck::kAmber);
        deck::RowOut so;
        deck::slider(L, "##sharp", "Sharpening", g_sharp, 0, 100, 5, "%", in(), g_typing, &so);
        took(so);
    } else if (page == 1) {
        deck::section(L, "Frame generation");
        took(deck::cycler(L, "##fgb", "Backend", "Intel XeSS", in()));
        deck::status(L, "Not available: Intel XeSS frame generation does not take the game's HDR output format.", deck::kDim);
        took(deck::toggle(L, "##fg", "Enabled", true, in()));
        took(deck::cycler(L, "##mode", "Mode", "2\xC3\x97", in()));
        deck::status(L, "Generating 2.0\xC3\x97 \xC2\xB7 120 shown from 60 rendered", deck::kGreen);
        took(deck::cycler(L, "##cap", "Frame cap", "Auto (Adaptive Sync)", in()));
        int cap = 90;
        deck::RowOut so;
        deck::slider(L, "##limit", "Cap", cap, 30, 480, 5, " fps", in(true), g_typing, &so);
        took(so);
    } else {
        deck::section(L, "Overlay");
        took(deck::toggle(L, "##counter", "FPS counter", true, in()));
        took(deck::cycler(L, "##corner", "Position", "Bottom right", in()));
        deck::section(L, "Layout");
        o.portable = took(deck::portableRow(L, "##portable", "Portable mode", portableOn, in()));
        o.portableRect = o.rowRects[o.nRows - 1];
        deck::status(L, "Bigger text, pages and touch controls for handhelds. Switches now.", deck::kDim);
    }
    o.bodyEnd = L.y;
    o.vtxFoot0 = ImGui::GetWindowDrawList()->VtxBuffer.Size;
    deck::footer(L, "Lumenveil 1.0.2");
    o.vtxFoot1 = ImGui::GetWindowDrawList()->VtxBuffer.Size;
    deck::end(L);
    return o;
}
static DeckOut deckFrame(int page, int focus = -1, bool a = false, bool on = true) {
    ImGui::GetIO().DisplaySize = ImVec2(g_w, g_h);
    ImGui::NewFrame();
    DeckOut o = deckPage(page, focus, a, on);
    ImGui::Render();
    return o;
}

// The Deck switch: a tap on the row, A and left / right flip it (step +1); its knob 15 px in from the 72 px track's ends.
static void deckSwitch() {
    deckFrame(2); deckFrame(2);
    DeckOut o = deckFrame(2, 2, false, true);
    const float k = o.look.k;
    const float trackL = o.portableRect.Min.x + (deck::kCtlL - deck::kRowL) * k;
    ImDrawList* dl = windowList("##deck");
    CHECK(near(lastDiamondX(dl, 0, o.vtxFoot0) - trackL, (72 - 15) * k, 0.8));  // On: the right end
    o = deckFrame(2, 2, true, true);
    CHECK(o.portable.step == 1);                                               // A
    const ImVec2 m = o.portableRect.GetCenter();
    mouse(m.x, m.y, false); deckFrame(2); mouse(m.x, m.y, true); o = deckFrame(2); mouse(m.x, m.y, false); deckFrame(2);
    CHECK(o.portable.press); CHECK(o.portable.step == 1);                      // a tap on the row
    mouse(-100, -100, false);
    for (int i = 0; i < 12; ++i) o = deckFrame(2, 2, false, false);  // past the 0.15 s glide
    CHECK(near(lastDiamondX(windowList("##deck"), 0, o.vtxFoot0) - trackL, 15 * k, 0.8));  // Off: the left end
}

// ---- the layout change, in compose's order (AC5, AC6) ----------------------------------------------------------------
struct App {
    bool open = true, deckOpen = false, padUsed = true, focusPortable = false;
    int layout = 0, iniLayout = 0, iniWrites = 0, flipsDesk = 0, flipsDeck = 0, otherPresses = 0;
    deck::Swap swap;
    deck::Nav nav;
    ImGuiID swId = 0, aboveId = 0;
    ImRect deskSw, deckSw;
};
static App g_app;
static void flip(bool on) {  // the menu's flip handler: [overlay] layout, g_layout and the pending change
    App& a = g_app;
    a.layout = on ? 1 : 2; a.iniLayout = a.layout; ++a.iniWrites;
    a.swap.flip(on, ImGui::GetTime(), a.deckOpen);
}
static void appFrame(pad::PadView v) {
    App& a = g_app;
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(g_w, g_h);
    if (a.open) {
        if (a.swap.due(ImGui::GetTime())) {
            a.deckOpen = a.swap.want == 1; a.swap = {};
            if (a.deckOpen) { a.nav.page = 2; a.nav.focus[2] = 2; } else a.focusPortable = true;
        }
        pad::feedPad(io, v, false);
    }
    if (a.open && a.deckOpen) io.ConfigFlags &= ~(ImGuiConfigFlags_NavEnableGamepad | ImGuiConfigFlags_NavEnableKeyboard);
    else io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad | ImGuiConfigFlags_NavEnableKeyboard;
    ImGui::NewFrame();
    if (a.open && a.deckOpen) {
        deck::Look L = deck::Look::at(g_w, g_h, g_font, g_font, g_font);
        deck::begin(L);
        const int to = deck::tabs(L, a.nav.page, nullptr);
        a.nav.page = to;
        if (ImGui::IsKeyPressed(ImGuiKey_GamepadDpadUp)) a.nav.move(deck::kRows[a.nav.page], -1);
        if (ImGui::IsKeyPressed(ImGuiKey_GamepadDpadDown)) a.nav.move(deck::kRows[a.nav.page], 1);
        const bool aEdge = (v.edges & pad::kA) != 0;
        int row = 0;
        auto in = [&]() { deck::RowIn r; r.focused = row == a.nav.focus[a.nav.page]; if (r.focused) r.a = aEdge; return r; };
        auto took = [&](deck::RowOut o) { if (o.press) a.nav.focus[a.nav.page] = row; ++row; return o.step; };
        if (a.nav.page == 2) {
            deck::section(L, "Overlay");
            if (took(deck::toggle(L, "##counter", "FPS counter", true, in()))) ++a.otherPresses;
            if (took(deck::cycler(L, "##corner", "Position", "Top left", in()))) ++a.otherPresses;
            deck::section(L, "Layout");
            bool on = a.swap.shown(a.deckOpen);
            if (took(deck::portableRow(L, "##portable", "Portable mode", on, in()))) { on = !on; ++a.flipsDeck; flip(on); }
            a.deckSw = ImRect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
        }
        deck::footer(L, "Lumenveil 1.0.2");
        deck::end(L);
    } else if (a.open) {
        ImGui::SetNextWindowPos(ImVec2(100, 100));
        ImGui::SetNextWindowSize(ImVec2(600, 300));
        ImGui::Begin("menu", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove);
        if (ImGui::Button("Position")) ++a.otherPresses;
        a.aboveId = ImGui::GetItemID();
        bool on = a.swap.shown(a.deckOpen);
        if (look::portable("##portable", on, true)) { ++a.flipsDesk; flip(on); }
        a.swId = ImGui::GetItemID();
        a.deskSw = ImRect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
        if (a.focusPortable) { a.focusPortable = false; look::focusItem(a.padUsed); }
        ImGui::End();
    }
    ImGui::Render();
}
static pad::PadView padState(uint16_t buttons, uint16_t edges = 0) { pad::PadView v; v.connected = true; v.buttons = buttons; v.edges = edges; return v; }
static void frames(int n, pad::PadView v) { for (int i = 0; i < n; ++i) appFrame(v); }

static void change() {
    ImGuiIO& io = ImGui::GetIO();
    const float dt = io.DeltaTime;
    io.DeltaTime = 1.0f / 60;  // 0.38 s is 23 frames
    App& a = g_app;
    // Deck to desktop by A, with A held through the change
    a = App{}; a.deckOpen = true; a.nav.page = 2; a.nav.focus[2] = 2;
    mouse(-100, -100, false);
    frames(3, padState(0));
    appFrame(padState(pad::kA, pad::kA));                         // the press: the switch flips Off
    CHECK(a.flipsDeck == 1); CHECK(a.iniLayout == 2); CHECK(a.layout == 2); CHECK(a.deckOpen);
    frames(20, padState(pad::kA));                                // 0.35 s: not yet
    CHECK(a.deckOpen);
    frames(5, padState(pad::kA));                                 // past 0.38 s: the desktop menu
    CHECK(!a.deckOpen);
    frames(3, padState(pad::kA));                                 // A still held
    CHECK(GImGui->NavId == a.swId);                               // its switch focused
    CHECK(GImGui->NavCursorVisible);                              // the controller has been used: the cursor shows
    CHECK(a.flipsDesk == 0); CHECK(a.otherPresses == 0);          // the held A acted once only
    frames(2, padState(0));                                       // let go
    CHECK(a.flipsDesk == 0);
    frames(1, padState(pad::kA, pad::kA)); frames(1, padState(0)); // a fresh A flips it
    CHECK(a.flipsDesk == 1); CHECK(a.iniLayout == 1);
    // ... which changes back to the Deck 0.38 s later, A held through that change too
    a.flipsDesk = 0;
    frames(30, padState(0));
    CHECK(a.deckOpen); CHECK(a.nav.page == 2); CHECK(a.nav.focus[2] == 2);  // Overlay page, Portable mode focused
    // desktop to Deck with A held
    a = App{}; a.deckOpen = false; a.focusPortable = true;
    frames(3, padState(0));
    CHECK(GImGui->NavId == a.swId);
    frames(1, padState(pad::kA, pad::kA));                        // A pressed (and held)
    CHECK(a.flipsDesk == 1); CHECK(a.iniLayout == 1);
    frames(30, padState(pad::kA));
    CHECK(a.deckOpen); CHECK(a.nav.page == 2); CHECK(a.nav.focus[2] == 2);
    CHECK(a.flipsDeck == 0); CHECK(a.otherPresses == 0);          // the held A did nothing in the new layout
    frames(2, padState(0));
    frames(1, padState(pad::kA, pad::kA)); frames(1, padState(0));
    CHECK(a.flipsDeck == 1); CHECK(a.iniLayout == 2);             // the next fresh press does
    // Deck to desktop by a tap, the mouse held through the change and let go over the desktop switch
    a = App{}; a.deckOpen = true; a.nav.page = 2; a.nav.focus[2] = 0;
    frames(3, padState(0));
    const ImVec2 dm = a.deckSw.GetCenter();
    mouse(dm.x, dm.y, false); appFrame(padState(0));
    mouse(dm.x, dm.y, true); appFrame(padState(0));
    CHECK(a.flipsDeck == 1); CHECK(a.nav.focus[2] == 2);
    frames(28, padState(0));
    CHECK(!a.deckOpen);
    const ImVec2 sm = a.deskSw.GetCenter();
    mouse(sm.x, sm.y, true); frames(3, padState(0));               // held, moved onto the switch
    mouse(sm.x, sm.y, false); frames(2, padState(0));              // let go there
    CHECK(a.flipsDesk == 0); CHECK(a.otherPresses == 0);
    CHECK(!GImGui->NavCursorVisible || a.padUsed);
    mouse(sm.x, sm.y, true); appFrame(padState(0)); mouse(sm.x, sm.y, false); appFrame(padState(0));
    CHECK(a.flipsDesk == 1);                                       // a fresh click does
    // desktop to Deck by the right arrow, the mouse held over where the Deck's switch will be
    a = App{}; a.deckOpen = false; a.focusPortable = true; a.padUsed = false;
    mouse(-100, -100, false); frames(3, padState(0));
    CHECK(GImGui->NavId == a.swId);
    CHECK(!GImGui->NavCursorVisible);                             // no controller used: no nav cursor
    io.AddKeyEvent(ImGuiKey_RightArrow, true); appFrame(padState(0)); io.AddKeyEvent(ImGuiKey_RightArrow, false); appFrame(padState(0));
    CHECK(a.flipsDesk == 1);
    mouse(640, 450, true); frames(30, padState(0));
    CHECK(a.deckOpen);
    const ImVec2 pm = a.deckSw.GetCenter();
    mouse(pm.x, pm.y, true); frames(3, padState(0));
    mouse(pm.x, pm.y, false); frames(2, padState(0));
    CHECK(a.flipsDeck == 0); CHECK(a.otherPresses == 0);
    mouse(pm.x, pm.y, true); appFrame(padState(0)); mouse(pm.x, pm.y, false); appFrame(padState(0));
    CHECK(a.flipsDeck == 1);
    mouse(-100, -100, false);
    // after the change the D-pad moves from the switch to the row above
    a = App{}; a.deckOpen = false; a.focusPortable = true;
    frames(3, padState(0));
    CHECK(GImGui->NavId == a.swId);
    frames(1, padState(pad::kUp, pad::kUp)); frames(2, padState(0));
    CHECK(GImGui->NavId == a.aboveId);
    // closed within 0.38 s: nothing changes while closed; the next opening picks from the ini as before
    a = App{}; a.deckOpen = true; a.nav.page = 2; a.nav.focus[2] = 2;
    frames(2, padState(0));
    frames(1, padState(pad::kA, pad::kA)); frames(1, padState(0));
    CHECK(a.flipsDeck == 1);
    frames(5, padState(0));
    a.open = false;                                                // closeMenu
    frames(40, padState(0));
    CHECK(a.deckOpen);                                             // no change applied while closed
    a.open = true; a.swap = {};                                    // the open branch: g_swap = {} after the pick
    a.deckOpen = deck::deckLayoutOn(a.iniLayout, true);            // a Steam Deck, Portable mode turned off
    frames(40, padState(0));
    CHECK(!a.deckOpen);
    io.DeltaTime = dt;
}

// ---- the glove (AC7, AC8) ----------------------------------------------------------------------------------------
static void gloveShape() {
    ImGui::NewFrame();
    ImGui::SetNextWindowPos(ImVec2(0, 0)); ImGui::SetNextWindowSize(ImVec2(300, 200));
    ImGui::Begin("glove", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoSavedSettings);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const int v0 = dl->VtxBuffer.Size;
    for (float unit : { 0.6f, 0.9f, 1.35f }) {  // desktop at 1, the Deck at 1 and 1.5
        const int a = dl->VtxBuffer.Size;
        const ImVec2 o(40, 60);
        look::glove(dl, o, unit);
        const int b = dl->VtxBuffer.Size;
        CHECK(b > a);
        float minX = 1e9f, minY = 1e9f, maxX = -1e9f, maxY = -1e9f;
        for (int i = a; i < b; ++i) {
            const ImVec2 p = dl->VtxBuffer[i].pos;
            minX = std::min(minX, p.x); maxX = std::max(maxX, p.x); minY = std::min(minY, p.y); maxY = std::max(maxY, p.y);
        }
        // inside its 40 x 24 grid (plus the outline's half pixel of anti-aliasing)
        CHECK(minX >= o.x - 0.6f && maxX <= o.x + 40 * unit + 0.6f);
        CHECK(minY >= o.y - 0.6f && maxY <= o.y + 24 * unit + 0.6f);
        CHECK(maxX - minX > 35 * unit && maxY - minY > 15 * unit);  // and it fills it: a hand, not a dot
        // the mockup's colours
        CHECK(hasColour(dl, a, b, rgb(0xff, 0xff, 0xff))); CHECK(hasColour(dl, a, b, rgb(0xcf, 0xd8, 0xe4)));
        CHECK(hasColour(dl, a, b, rgb(0xf2, 0xf5, 0xfa))); CHECK(hasColour(dl, a, b, rgb(0xe6, 0xeb, 0xf2)));
        CHECK(hasColour(dl, a, b, rgb(0x26, 0x30, 0x3d))); CHECK(hasColour(dl, a, b, rgb(0x9a, 0xa7, 0xb8)));
    }
    (void)v0;
    ImGui::End();
    ImGui::Render();
    // no texture but the font atlas in any draw command
    ImDrawData* dd = ImGui::GetDrawData();
    const ImTextureID atlas = ImGui::GetIO().Fonts->TexID;
    for (int n = 0; n < dd->CmdListsCount; ++n)
        for (const ImDrawCmd& c : dd->CmdLists[n]->CmdBuffer) CHECK(c.GetTexID() == atlas);
    // the sway: 0 to 2 px, both ends reached, eased at the turns, repeating every 1.1 s
    float lo = 9, hi = -9;
    for (int i = 0; i <= 2200; ++i) { const float b = look::bobX(i / 1000.0); lo = std::min(lo, b); hi = std::max(hi, b); }
    CHECK(near(lo, 0, 1e-4)); CHECK(near(hi, 2, 1e-4));
    for (double t : { 0.0, 0.13, 0.4, 0.77, 1.0 }) CHECK(near(look::bobX(t), look::bobX(t + 1.1), 1e-4));
    CHECK(near(look::bobX(0.55), 2, 1e-4));
    CHECK(std::fabs(look::bobX(0.01) - look::bobX(0)) < 0.002);                // no jump at the left turn
    CHECK(std::fabs(look::bobX(0.56) - look::bobX(0.55)) < 0.002);             // nor at the right one
}

// The desktop rows: the glove is drawn at the selected row on the frame the selection moves, and at no other row;
// no diamond at the row's left; 6 px left of the row and inside the window's clip rect.
struct Rows { ImRect r[2]; int v0[2], v1[2]; ImVec4 clip; };
static Rows rowsFrame() {
    Rows o;
    ImGui::NewFrame();
    ImGui::SetNextWindowPos(ImVec2(0, 0)); ImGui::SetNextWindowSize(ImVec2(600, 200));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(30, 26));
    ImGui::Begin("rows", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoResize);
    ImGui::PopStyleVar();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    for (int i = 0; i < 2; ++i) {
        o.v0[i] = dl->VtxBuffer.Size;
        const ImVec2 p = ImGui::GetCursorScreenPos();
        look::row();
        o.v1[i] = dl->VtxBuffer.Size;
        ImGui::PushID(i); ImGui::Button("value", ImVec2(200, 0)); ImGui::PopID();
        const float sp = ImGui::GetStyle().ItemSpacing.y;
        o.r[i] = ImRect(p.x, p.y - sp * 0.5f, p.x + 540, p.y + ImGui::GetFrameHeight() + sp * 0.5f);
    }
    o.clip = dl->_ClipRectStack.back();
    ImGui::End();
    ImGui::Render();
    return o;
}
static int gloveVerts(const ImDrawList* dl, int a, int b) {
    int n = 0;
    for (int i = a; i < b; ++i) if (dl->VtxBuffer[i].col == rgb(0xcf, 0xd8, 0xe4) || dl->VtxBuffer[i].col == rgb(0x26, 0x30, 0x3d)) ++n;
    return n;
}
static void gloveRows() {
    look::ctx().s = 1;
    look::ctx().bob = 0;
    mouse(-100, -100, false);
    rowsFrame(); rowsFrame();
    Rows o = rowsFrame();
    const ImVec2 m0 = o.r[0].GetCenter(), m1 = o.r[1].GetCenter();
    mouse(m0.x, m0.y, false); rowsFrame(); o = rowsFrame();
    ImDrawList* dl = windowList("rows");
    CHECK(gloveVerts(dl, o.v0[0], o.v1[0]) > 0); CHECK(gloveVerts(dl, o.v0[1], o.v1[1]) == 0);
    // 24 x 14.4, its left 6 px left of the row, centred on it, inside the clip rect; no diamond there
    float minX = 1e9f, maxX = -1e9f, minY = 1e9f, maxY = -1e9f;
    for (int i = o.v0[0]; i < o.v1[0]; ++i) {
        const ImDrawVert& v = dl->VtxBuffer[i];
        if (v.col != rgb(0x26, 0x30, 0x3d) && v.col != rgb(0xcf, 0xd8, 0xe4) && v.col != rgb(0xff, 0xff, 0xff)) continue;
        minX = std::min(minX, v.pos.x); maxX = std::max(maxX, v.pos.x); minY = std::min(minY, v.pos.y); maxY = std::max(maxY, v.pos.y);
    }
    CHECK(minX >= o.r[0].Min.x - 6 - 0.6f && minX <= o.r[0].Min.x - 6 + 2);
    CHECK(maxX <= o.r[0].Min.x - 6 + 24 + 0.6f);
    CHECK(near((minY + maxY) * 0.5f, o.r[0].GetCenter().y, 1.5));
    CHECK(maxY - minY <= 14.4f + 1.2f);
    CHECK(minX >= o.clip.x && maxX <= o.clip.z && minY >= o.clip.y && maxY <= o.clip.w);
    CHECK(std::isnan(lastDiamondX(dl, o.v0[0], o.v1[0])));
    // the selection moves: on that very frame the glove is at the new row and nowhere else
    mouse(m1.x, m1.y, false); o = rowsFrame();
    dl = windowList("rows");
    CHECK(gloveVerts(dl, o.v0[1], o.v1[1]) > 0); CHECK(gloveVerts(dl, o.v0[0], o.v1[0]) == 0);
    // the sway moves it right by ctx().bob
    look::ctx().bob = 2;
    float x2 = 1e9f;
    o = rowsFrame(); dl = windowList("rows");
    for (int i = o.v0[1]; i < o.v1[1]; ++i) if (dl->VtxBuffer[i].col == rgb(0x26, 0x30, 0x3d)) x2 = std::min(x2, dl->VtxBuffer[i].pos.x);
    look::ctx().bob = 0;
    float x0 = 1e9f;
    o = rowsFrame(); dl = windowList("rows");
    for (int i = o.v0[1]; i < o.v1[1]; ++i) if (dl->VtxBuffer[i].col == rgb(0x26, 0x30, 0x3d)) x0 = std::min(x0, dl->VtxBuffer[i].pos.x);
    CHECK(near(x2 - x0, 2, 0.01));
    mouse(-100, -100, false);
}

// ---- the Deck look (AC7, AC9, AC10, AC12) --------------------------------------------------------------------------------
static void deckLook() {
    for (const ImVec2 sz : { ImVec2(1280, 800), ImVec2(1920, 1080) }) {
        g_w = sz.x; g_h = sz.y;
        for (int p = 0; p < 3; ++p) {
            deckFrame(p); DeckOut o = deckFrame(p, 0);
            const float k = o.look.k;
            // every page, with its most status lines, ends above the footer
            CHECK(o.bodyEnd <= deck::kFootTop);
            CHECK(o.nRows == deck::kRows[p]);
            ImDrawList* dl = windowList("##deck");
            // the focused row: the glove 2 px into the row (36 x 21.6), centred on it; the label 48 px in
            const ImRect r = o.rowRects[0];
            float gx0 = 1e9f, gx1 = -1e9f, gy0 = 1e9f, gy1 = -1e9f, lx = 1e9f;
            for (int i = 0; i < o.vtxFoot0; ++i) {
                const ImDrawVert& v = dl->VtxBuffer[i];
                if (v.pos.y < r.Min.y || v.pos.y > r.Max.y) continue;
                if (v.col == rgb(0x26, 0x30, 0x3d)) { gx0 = std::min(gx0, v.pos.x); gx1 = std::max(gx1, v.pos.x); gy0 = std::min(gy0, v.pos.y); gy1 = std::max(gy1, v.pos.y); }
                if (v.col == ImGui::GetColorU32(look::kInk) && v.pos.x < r.Min.x + 250 * k) lx = std::min(lx, v.pos.x);
            }
            CHECK(gx0 >= r.Min.x + 2 * k - 0.6f && gx0 <= r.Min.x + 2 * k + 3 * k);
            CHECK(gx1 <= r.Min.x + (2 + 36) * k + 0.6f);
            CHECK(near((gy0 + gy1) * 0.5f, r.GetCenter().y, 1.5 * k));
            CHECK(gy1 - gy0 <= 21.6f * k + 1.2f);
            CHECK(lx >= r.Min.x + 48 * k - 0.5f && lx <= r.Min.x + 48 * k + 4 * k);
            // no row cursor diamond at the row's left
            CHECK(std::isnan(lastDiamondX(dl, 0, o.vtxFoot0)) || lastDiamondX(dl, 0, o.vtxFoot0) > r.Min.x + 250 * k);
            // the badges (L1 / R1 and the footer's): square with 2 px corners, never rounded to half their height. Each
            // badge's outline has points within 2.5 px of all four corners of its bounds; a capsule's would not.
            const ImU32 bc = ImGui::GetColorU32(look::rgba(205, 215, 228, .5f));
            std::vector<ImVec2> pts;
            for (int i = 0; i < dl->VtxBuffer.Size; ++i) if (dl->VtxBuffer[i].col == bc) pts.push_back(dl->VtxBuffer[i].pos);
            int badges = 0;
            std::vector<ImVec2> sorted = pts;
            std::sort(sorted.begin(), sorted.end(), [](const ImVec2& a, const ImVec2& b) { return a.x < b.x; });
            // band by band (the tab bar and the footer)
            for (float bandY : { (deck::kTabTop + deck::kTabBottom) * 0.5f, deck::kFootTop + 24.f }) {
                const float cy = o.look.p(0, bandY).y;
                std::vector<ImVec2> b;
                for (const ImVec2& q : sorted) if (std::fabs(q.y - cy) < 20 * k) b.push_back(q);
                // the outline's points gather at the badges' left and right edges: pair the edges up
                std::vector<std::pair<size_t, size_t>> edges;
                for (size_t i = 0; i < b.size();) {
                    size_t j = i + 1;
                    while (j < b.size() && b[j].x - b[j - 1].x < 4 * k) ++j;
                    edges.push_back({ i, j });
                    i = j;
                }
                CHECK(edges.size() % 2 == 0);
                for (size_t e = 0; e + 1 < edges.size(); e += 2) {
                    const size_t i = edges[e].first, j = edges[e + 1].second;
                    float x0 = 1e9f, x1 = -1e9f, y0 = 1e9f, y1 = -1e9f;
                    for (size_t n = i; n < j; ++n) { x0 = std::min(x0, b[n].x); x1 = std::max(x1, b[n].x); y0 = std::min(y0, b[n].y); y1 = std::max(y1, b[n].y); }
                    auto corner = [&](float cx, float cyy) {
                        for (size_t n = i; n < j; ++n) if (std::fabs(b[n].x - cx) < 2.5f * k && std::fabs(b[n].y - cyy) < 2.5f * k) return true;
                        return false;
                    };
                    CHECK(corner(x0, y0) && corner(x1, y0) && corner(x0, y1) && corner(x1, y1));
                    CHECK(y1 - y0 > 20 * k);  // a badge, 28 high
                    ++badges;
                }
            }
            CHECK(badges == 6);  // L1, R1; A, B, L1 R1, D-pad
            // the footer: the save line, then 8 px lower the trademark line, with the release number at the save line's
            // right end
            const ImU32 dim = ImGui::GetColorU32(look::kDim);
            float saveTop = 1e9f, saveBot = -1e9f, tmTop = 1e9f, verLeft = 1e9f, verRight = -1e9f;
            const float split = o.look.p(0, deck::kFootTop + 44 + 23.8f).y;
            for (int i = o.vtxFoot0; i < o.vtxFoot1; ++i) {
                const ImDrawVert& v = dl->VtxBuffer[i];
                if (v.col != dim) continue;
                if (v.pos.y < split) {
                    saveTop = std::min(saveTop, v.pos.y); saveBot = std::max(saveBot, v.pos.y);
                    if (v.pos.x > o.look.p(500, 0).x) { verLeft = std::min(verLeft, v.pos.x); verRight = std::max(verRight, v.pos.x); }
                } else tmTop = std::min(tmTop, v.pos.y);
            }
            CHECK(saveTop > o.look.p(0, deck::kFootTop + 38).y);              // below the key row
            CHECK(tmTop < 1e8f && tmTop > saveBot + 6 * k);                     // the trademark line under it
            CHECK(tmTop < o.look.p(0, deck::kPanelH - 20).y);
            const float verW = g_font->CalcTextSizeA(15 * k, FLT_MAX, 0, "Lumenveil 1.0.2").x;
            CHECK(near(verRight, o.look.p(deck::kPanelW - 30, 0).x, 4 * k));    // right-aligned at the panel's padding
            CHECK(verRight - verLeft > verW * 0.8f);                            // and the whole release text is there
        }
    }
    g_w = 1280; g_h = 800;
}

int main() {
    swap();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.DeltaTime = 1.0f / 60;
    io.DisplaySize = ImVec2(1280, 800);
    io.BackendFlags |= ImGuiBackendFlags_HasGamepad;
    io.ConfigNavCursorVisibleAuto = true;
    ImFontConfig fc; fc.SizePixels = 26;
    g_font = io.Fonts->AddFontDefault(&fc);
    unsigned char* px; int w, h;
    io.Fonts->GetTexDataAsRGBA32(&px, &w, &h);
    io.Fonts->SetTexID((ImTextureID)1);
    look::ctx().s = 1;
    desktopSwitch();
    knob();
    deckSwitch();
    change();
    gloveShape();
    gloveRows();
    deckLook();
    ImGui::DestroyContext();
    if (g_fail) { std::printf("%d failure(s)\n", g_fail); return 1; }
    std::printf("portable_test passed\n");
    return 0;
}
