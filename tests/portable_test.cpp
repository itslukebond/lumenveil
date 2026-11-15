// Host test for the layout button, the live layout change, the pointing glove and the Portable layout's look
// (src/menu_look.h, src/menu_deck.h, src/menu_pad.h) in a headless Dear ImGui context. Expected values are the intended
// behaviour and the design's sizes and colours: a click or tap on the unlit half switches, on the lit half nothing; left /
// right pick a half and A, Space or Enter the other layout; Up from the first row reaches the button and Down comes back;
// the change lands on the next frame with the button focused; the glove's 40 x 24 grid at 0.6 (24 x 14.4) on the desktop
// and 0.9 (36 x 21.6) on the Deck, its colours and the 1.1 s sway of 0 to 2 px; labels 48 px into a Deck row; rows in
// fixed slots at a 54 px pitch; the footer's key row with the version and two trademark lines.
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

// ---- the pending change --------------------------------------------------------------------------------
static void swap() {
    deck::Swap s;
    CHECK(!s.due(0)); CHECK(!s.due(100));
    s.flip(true, 10.0, false);                                              // desktop shown, Portable picked at 10 s
    CHECK(s.want == 1); CHECK(s.due(10.0));                                 // lands on the next frame
    s.flip(false, 10.0, false);                                             // asked back before it landed: cancelled
    CHECK(s.want == -1); CHECK(!s.due(20));
    deck::Swap t;
    t.flip(true, 5.0, true);                                                // the layout already shown: nothing
    CHECK(!t.due(5.5));
    deck::Swap u;
    u.flip(false, 3.0, true);                                               // Portable shown, Desktop picked
    CHECK(u.want == 0); CHECK(u.due(3.0));
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

static void mouse(float x, float y, bool down) { ImGuiIO& io = ImGui::GetIO(); io.AddMousePosEvent(x, y); io.AddMouseButtonEvent(0, down); }

// ---- the desktop layout button ---------------------------------------------------------------------
// The desktop menu's top: the button at the top bar's right, the tabs (never navigation stops) and two rows.
struct Desk {
    bool portable = false, enabled = true, focusAfter = false, cursor = false;
    int picks = 0, last = -1, tabTo = -1;
    ImRect btn; ImGuiID btnId = 0, row0 = 0, row1 = 0;
    int vtx0 = 0, vtx1 = 0;
};
static Desk g_desk;
static void deskWindow() {
    Desk& d = g_desk;
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(ImVec2(600, 400));
    ImGui::Begin("desk", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove);
    ImGui::SetCursorPos(ImVec2(340, 33));
    d.vtx0 = ImGui::GetWindowDrawList()->VtxBuffer.Size;
    const int want = look::layoutButton("##layout", d.portable, d.enabled, 230, 34);
    d.vtx1 = ImGui::GetWindowDrawList()->VtxBuffer.Size;
    d.btn = ImRect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
    d.btnId = ImGui::GetItemID();
    if (want >= 0) { ++d.picks; d.last = want; }
    if (d.focusAfter) { d.focusAfter = false; look::focusItem(d.cursor); }
    static const char* const names[] = { "Upscaling", "Frame generation", "Overlay" };
    ImGui::SetCursorPos(ImVec2(30, 82));
    d.tabTo = look::tabs(0, names, 3, 540, 32);
    ImGui::SetCursorPos(ImVec2(242, 128)); ImGui::Button("row0", ImVec2(300, 28)); d.row0 = ImGui::GetItemID();
    ImGui::SetCursorPos(ImVec2(242, 164)); ImGui::Button("row1", ImVec2(300, 28)); d.row1 = ImGui::GetItemID();
    ImGui::End();
}
static void deskFrame() {
    ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad | ImGuiConfigFlags_NavEnableKeyboard;
    ImGui::NewFrame();
    deskWindow();
    ImGui::Render();
}
static void key(ImGuiKey k) { ImGuiIO& io = ImGui::GetIO(); io.AddKeyEvent(k, true); deskFrame(); io.AddKeyEvent(k, false); deskFrame(); }
static void click(ImVec2 p) { mouse(p.x, p.y, false); deskFrame(); mouse(p.x, p.y, true); deskFrame(); mouse(p.x, p.y, false); deskFrame(); }
static void focusButton() { g_desk.focusAfter = true; g_desk.cursor = true; deskFrame(); deskFrame(); }
static void focusId(ImGuiID id) {
    ImGui::NewFrame(); ImGui::Begin("desk"); ImGui::SetFocusID(id, ImGui::GetCurrentWindow()); ImGui::SetNavCursorVisible(true); ImGui::End(); ImGui::Render();
    deskFrame();
}

static void desktopButton() {
    Desk& d = g_desk;
    d = Desk{};
    deskFrame(); deskFrame();
    const ImVec2 left(d.btn.Min.x + d.btn.GetWidth() * 0.25f, d.btn.GetCenter().y), right(d.btn.Min.x + d.btn.GetWidth() * 0.75f, left.y);
    // square (2 px corners), the lit half's 1 px #a9c8ff underline under Desktop while Desktop is shown
    ImDrawList* dl = windowList("desk");
    float ux0 = 1e9f, ux1 = -1e9f;
    for (int i = d.vtx0; i < d.vtx1; ++i) if (dl->VtxBuffer[i].col == rgb(0xa9, 0xc8, 0xff)) { ux0 = std::min(ux0, dl->VtxBuffer[i].pos.x); ux1 = std::max(ux1, dl->VtxBuffer[i].pos.x); }
    CHECK(near(ux0, d.btn.Min.x, 0.6)); CHECK(near(ux1, d.btn.GetCenter().x, 1.1));
    // a click on the lit half does nothing; on the unlit half it asks for that layout
    click(left); CHECK(d.picks == 0);
    click(right); CHECK(d.picks == 1); CHECK(d.last == 1);
    d.portable = true; deskFrame();
    float px0 = 1e9f;
    dl = windowList("desk");
    for (int i = d.vtx0; i < d.vtx1; ++i) if (dl->VtxBuffer[i].col == rgb(0xa9, 0xc8, 0xff)) px0 = std::min(px0, dl->VtxBuffer[i].pos.x);
    CHECK(px0 >= d.btn.GetCenter().x - 1.1f);                                  // Portable lit: its underline on the right half
    click(right); CHECK(d.picks == 1);
    click(left); CHECK(d.picks == 2); CHECK(d.last == 0);
    d.portable = false;
    mouse(-100, -100, false); deskFrame();
    // with the focus on it, A, Space and Enter ask for the other layout; right picks Portable, left Desktop (shown:
    // nothing)
    focusButton();
    CHECK(GImGui->NavId == d.btnId);
    d.picks = 0;
    key(ImGuiKey_GamepadFaceDown); CHECK(d.picks == 1); CHECK(d.last == 1);
    key(ImGuiKey_Space); CHECK(d.picks == 2);
    key(ImGuiKey_Enter); CHECK(d.picks == 3);
    key(ImGuiKey_LeftArrow); CHECK(d.picks == 3);
    key(ImGuiKey_RightArrow); CHECK(d.picks == 4); CHECK(d.last == 1);
    key(ImGuiKey_GamepadDpadLeft); CHECK(d.picks == 4);
    key(ImGuiKey_GamepadDpadRight); CHECK(d.picks == 5);
    CHECK(GImGui->NavId == d.btnId);                                           // left / right keep the focus on it
    d.portable = true; deskFrame();
    key(ImGuiKey_LeftArrow); CHECK(d.picks == 6); CHECK(d.last == 0);
    key(ImGuiKey_GamepadFaceDown); CHECK(d.picks == 7); CHECK(d.last == 0);
    d.portable = false; deskFrame();
    // Up from the first row reaches the button (the tabs are no stops), Down comes back to the first row
    focusId(d.row0); CHECK(GImGui->NavId == d.row0);
    key(ImGuiKey_UpArrow); CHECK(GImGui->NavId == d.btnId);
    key(ImGuiKey_DownArrow); CHECK(GImGui->NavId == d.row0);
    key(ImGuiKey_GamepadDpadUp); CHECK(GImGui->NavId == d.btnId);
    key(ImGuiKey_GamepadDpadDown); CHECK(GImGui->NavId == d.row0);
    key(ImGuiKey_DownArrow); CHECK(GImGui->NavId == d.row1);
    // not while a row has the focus
    const int before = d.picks;
    key(ImGuiKey_Space); key(ImGuiKey_RightArrow); key(ImGuiKey_Enter);
    CHECK(d.picks == before);
    // a click on a tab opens its page
    CHECK(d.tabTo == 0);
    {
        const ImVec2 t(30 + 540 / 6.f * 3, 82 + 16);
        mouse(t.x, t.y, false); deskFrame(); mouse(t.x, t.y, true); deskFrame();
        CHECK(d.tabTo == 1);
        mouse(t.x, t.y, false); deskFrame();
    }
    // disabled (the Portable fonts did not load): a click and A ask for nothing
    d.enabled = false;
    click(right);
    focusButton(); key(ImGuiKey_GamepadFaceDown); key(ImGuiKey_RightArrow);
    CHECK(d.picks == before);
    d.enabled = true;
    mouse(-100, -100, false); deskFrame();
    // the pure rule, both layouts
    CHECK(look::layoutPick(false, true, 10, 5, 0, false) == 1); CHECK(look::layoutPick(false, true, 4, 5, 0, false) == -1);
    CHECK(look::layoutPick(true, true, 4, 5, 0, false) == 0); CHECK(look::layoutPick(true, false, 0, 5, 1, false) == -1);
    CHECK(look::layoutPick(true, false, 0, 5, -1, false) == 0); CHECK(look::layoutPick(true, false, 0, 5, 0, true) == 0);
    CHECK(look::layoutPick(false, false, 0, 5, 0, true) == 1); CHECK(look::layoutPick(false, false, 0, 5, 0, false) == -1);
}

// ---- the Portable layout's frames ------------------------------------------------------------------------------------
static float g_w = 1280, g_h = 800;
struct DeckOut {
    deck::Look look;
    ImRect rowRects[8], button;
    int nRows = 0, pick = -1;
    float bodyEnd = 0, rowTop[8] = {};
    int vtxFoot0 = 0, vtxFoot1 = 0;
};
static deck::Typing g_typing;
static int g_sharp = 50;
// One page as drawDeckMenu lays it out, with the longest values; `focus` is the focused row (-1: the layout button),
// `a` this frame's A, `lr` its left / right.
static DeckOut deckPage(int page, int focus = 0, bool a = false, int lr = 0) {
    DeckOut o;
    o.look = deck::Look::at(g_w, g_h, g_font, g_font, g_font);
    deck::Look& L = o.look;
    deck::begin(L);
    o.pick = deck::layoutButton(L, true, focus == deck::kButton, lr, a);
    o.button = ImRect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
    deck::tabs(L, page, nullptr);
    L.y = deck::kBodyTop;
    int row = 0;
    auto in = [&](bool dim = false) { deck::RowIn r; r.focused = row == focus; r.dim = dim; if (r.focused && !dim) r.a = a; return r; };
    auto took = [&](deck::RowOut out) { o.rowRects[o.nRows] = ImRect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax()); ++o.nRows; ++row; return out; };
    auto top = [&]() { o.rowTop[o.nRows] = L.y; };
    deck::RowOut so;
    if (page == 0) {
        top(); took(deck::cycler(L, "##up", "Upscaler", "Auto", in(), "XeSS"));
        top(); took(deck::toggle(L, "##en", "Enabled", true, in()));
        top(); took(deck::cycler(L, "##scale", "Scaling mode", "Ultra Performance", in(), "33%"));
        top(); took(deck::cycler(L, "##model", "DLSS model", "Model N", in(), "ini"));
        top(); deck::slider(L, "##sharp", "Sharpening", g_sharp, 0, 100, 5, "%", in(), g_typing, &so); took(so);
        top(); deck::slider(L, "##gsharp", "Game sharpening", g_sharp, 0, 200, 5, "%", in(), g_typing, &so); took(so);
    } else if (page == 1) {
        top(); took(deck::cycler(L, "##fgb", "Backend", "Intel XeSS", in()));
        top(); took(deck::toggle(L, "##fg", "Enabled", true, in()));
        top(); took(deck::cycler(L, "##mode", "Mode", "Dynamic", in()));
        int t = 240;
        top(); deck::slider(L, "##target", "Target fps", t, 60, 480, 5, " fps", in(), g_typing, &so); took(so);
        deck::subHeading(L, "Latency");
        top(); took(deck::cycler(L, "##ll", "Xe Low Latency", "Boost", in()));
        top(); took(deck::cycler(L, "##cap", "Frame cap", "Auto", in(), "Adaptive Sync"));
        int cap = 90;
        top(); deck::slider(L, "##limit", "Cap", cap, 30, 480, 5, " fps", in(true), g_typing, &so); took(so);
    } else {
        top(); took(deck::toggle(L, "##counter", "FPS counter", true, in()));
        top(); took(deck::cycler(L, "##corner", "Position", "Bottom right", in()));
        top(); took(deck::toggle(L, "##hud", "Sharper HUD", true, in()));
    }
    o.bodyEnd = L.y;
    o.vtxFoot0 = ImGui::GetWindowDrawList()->VtxBuffer.Size;
    deck::footer(L, "Lumenveil 1.0.2", false);
    o.vtxFoot1 = ImGui::GetWindowDrawList()->VtxBuffer.Size;
    deck::end(L);
    return o;
}
static DeckOut deckFrame(int page, int focus = 0, bool a = false, int lr = 0) {
    ImGui::GetIO().DisplaySize = ImVec2(g_w, g_h);
    ImGui::NewFrame();
    DeckOut o = deckPage(page, focus, a, lr);
    ImGui::Render();
    return o;
}

// The Portable layout's button: a tap on the unlit half picks it, on the lit half nothing; with the focus on it, left /
// right pick a half and A the other layout; without the focus, nothing.
static void deckButton() {
    deckFrame(0); deckFrame(0);
    DeckOut o = deckFrame(0);
    const ImRect b = o.button;
    const float k = o.look.k;
    CHECK(near(b.GetWidth(), (deck::kButtonR - deck::kButtonL) * k, 0.5)); CHECK(b.GetHeight() >= 44 * k - 0.5f);  // a touch target
    CHECK(o.pick == -1);
    const ImVec2 left(b.Min.x + b.GetWidth() * 0.25f, b.GetCenter().y), right(b.Min.x + b.GetWidth() * 0.75f, left.y);
    mouse(right.x, right.y, false); deckFrame(0); mouse(right.x, right.y, true); o = deckFrame(0); mouse(right.x, right.y, false); deckFrame(0);
    CHECK(o.pick == -1);                                                    // Portable is lit: nothing
    mouse(left.x, left.y, false); deckFrame(0); mouse(left.x, left.y, true); o = deckFrame(0);
    CHECK(o.pick == 0);                                                     // Desktop asked for on the press
    mouse(left.x, left.y, false); deckFrame(0);
    mouse(-100, -100, false); deckFrame(0);
    CHECK(deckFrame(0, deck::kButton, false, 1).pick == -1);                // right: Portable, shown
    CHECK(deckFrame(0, deck::kButton, false, -1).pick == 0);                // left: Desktop
    CHECK(deckFrame(0, deck::kButton, true).pick == 0);                     // A: the other layout
    CHECK(deckFrame(0, 0, true, -1).pick == -1);                            // the focus on a row: nothing
    // the focus: up from the first row to the button, down back; a page change starts on the first row
    deck::Nav n;
    n.move(6, -1); CHECK(n.focus[0] == deck::kButton);
    n.move(6, -1); CHECK(n.focus[0] == deck::kButton);
    n.move(6, 1); CHECK(n.focus[0] == 0);
    n.move(6, 1); n.move(6, 1); CHECK(n.focus[0] == 2);
    n.turn(1); CHECK(n.page == 1); CHECK(n.focus[1] == 0);
    n.move(7, 1); n.turn(-1); CHECK(n.page == 0); CHECK(n.focus[0] == 0);
    n.focus[0] = deck::kButton; n.turn(-1); CHECK(n.page == 2); CHECK(n.focus[2] == 0);
    n.show(1); CHECK(n.page == 1); CHECK(n.focus[1] == 0);
    n.focus[1] = 3; n.show(1); CHECK(n.focus[1] == 3);                      // the page shown: unchanged
}

// ---- the layout change, in compose's order ------------------------------------------------------------
struct App {
    bool open = true, deckOpen = false, padUsed = true, focusDesk = false;
    int layout = 0, iniLayout = 0, iniWrites = 0, picksDesk = 0, picksDeck = 0, otherPresses = 0;
    deck::Swap swap;
    deck::Nav nav;
    ImGuiID btnId = 0, rowId = 0;
    ImRect deskBtn, deckBtn;
};
static App g_app;
static void pickLayout(bool portable) {  // the menu's handler: [overlay] layout, g_layout and the pending change
    App& a = g_app;
    a.layout = portable ? 1 : 2; a.iniLayout = a.layout; ++a.iniWrites;
    a.swap.flip(portable, ImGui::GetTime(), a.deckOpen);
}
static void appFrame(pad::PadView v) {
    App& a = g_app;
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(g_w, g_h);
    if (a.open) {
        if (a.swap.due(ImGui::GetTime())) {
            a.deckOpen = a.swap.want == 1; a.swap = {};
            deck::landFocus(a.deckOpen, a.nav, a.focusDesk);
        }
        pad::feedPad(io, v, false);
    }
    if (a.open && a.deckOpen) io.ConfigFlags &= ~(ImGuiConfigFlags_NavEnableGamepad | ImGuiConfigFlags_NavEnableKeyboard);
    else io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad | ImGuiConfigFlags_NavEnableKeyboard;
    ImGui::NewFrame();
    if (a.open && a.deckOpen) {
        deck::Look L = deck::Look::at(g_w, g_h, g_font, g_font, g_font);
        if (ImGui::IsKeyPressed(ImGuiKey_GamepadDpadUp)) a.nav.move(2, -1);
        if (ImGui::IsKeyPressed(ImGuiKey_GamepadDpadDown)) a.nav.move(2, 1);
        const bool aEdge = (v.edges & pad::kA) != 0;
        const int lr = ImGui::IsKeyPressed(ImGuiKey_GamepadDpadLeft, false) ? -1 : ImGui::IsKeyPressed(ImGuiKey_GamepadDpadRight, false) ? 1 : 0;
        deck::begin(L);
        const int want = deck::layoutButton(L, true, a.nav.focus[a.nav.page] == deck::kButton, lr, aEdge);
        a.deckBtn = ImRect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
        if (want >= 0) { ++a.picksDeck; pickLayout(want == 1); }
        a.nav.show(deck::tabs(L, a.nav.page, nullptr));
        L.y = deck::kBodyTop;
        int row = 0;
        auto in = [&]() { deck::RowIn r; r.focused = row == a.nav.focus[a.nav.page]; if (r.focused) r.a = aEdge; return r; };
        auto took = [&](deck::RowOut o) { if (o.press) a.nav.focus[a.nav.page] = row; ++row; return o.step; };
        if (took(deck::toggle(L, "##counter", "FPS counter", true, in()))) ++a.otherPresses;
        if (took(deck::cycler(L, "##corner", "Position", "Top left", in()))) ++a.otherPresses;
        deck::footer(L, "Lumenveil 1.0.2", false);
        deck::end(L);
    } else if (a.open) {
        ImGui::SetNextWindowPos(ImVec2(100, 100));
        ImGui::SetNextWindowSize(ImVec2(600, 300));
        ImGui::Begin("menu", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove);
        ImGui::SetCursorPos(ImVec2(340, 33));
        const int want = look::layoutButton("##layout", false, true, 230, 34);
        a.btnId = ImGui::GetItemID();
        a.deskBtn = ImRect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
        look::takeFocus(a.focusDesk, a.padUsed);
        if (want >= 0) { ++a.picksDesk; pickLayout(want == 1); }
        ImGui::SetCursorPos(ImVec2(242, 128));
        if (ImGui::Button("FPS counter", ImVec2(300, 28))) ++a.otherPresses;
        a.rowId = ImGui::GetItemID();
        ImGui::End();
    }
    ImGui::Render();
}
static pad::PadView padState(uint16_t buttons, uint16_t edges = 0) { pad::PadView v; v.connected = true; v.buttons = buttons; v.edges = edges; return v; }
static void frames(int n, pad::PadView v) { for (int i = 0; i < n; ++i) appFrame(v); }

static void change() {
    App& a = g_app;
    ImGuiIO& io = ImGui::GetIO();
    // Portable to Desktop by A on the button, A held through the change
    a = App{}; a.deckOpen = true; a.nav.focus[0] = deck::kButton;
    mouse(-100, -100, false);
    frames(3, padState(0));
    appFrame(padState(pad::kA, pad::kA));                         // the press: Desktop asked for
    CHECK(a.picksDeck == 1); CHECK(a.iniLayout == 2); CHECK(a.layout == 2); CHECK(a.deckOpen);
    appFrame(padState(pad::kA));                                  // the next frame: the desktop menu
    CHECK(!a.deckOpen); CHECK(a.open);                            // the menu stays open
    frames(3, padState(pad::kA));                                 // A still held
    CHECK(GImGui->NavId == a.btnId);                              // its button focused
    CHECK(GImGui->NavCursorVisible);                              // the controller has been used: the cursor shows
    CHECK(a.picksDesk == 0); CHECK(a.otherPresses == 0);          // the held A acted once only
    frames(2, padState(0));                                       // let go
    frames(1, padState(pad::kA, pad::kA)); frames(1, padState(0)); // a fresh A asks for Portable
    CHECK(a.picksDesk == 1); CHECK(a.iniLayout == 1);
    frames(2, padState(0));
    CHECK(a.deckOpen); CHECK(a.nav.focus[a.nav.page] == deck::kButton);  // its button focused, the page kept
    // Desktop to Portable with A held
    a = App{}; a.deckOpen = false; a.focusDesk = true;
    frames(3, padState(0));
    CHECK(GImGui->NavId == a.btnId);
    frames(1, padState(pad::kA, pad::kA));                        // A pressed (and held)
    CHECK(a.picksDesk == 1); CHECK(a.iniLayout == 1);
    frames(3, padState(pad::kA));
    CHECK(a.deckOpen); CHECK(a.nav.focus[a.nav.page] == deck::kButton);
    CHECK(a.picksDeck == 0); CHECK(a.otherPresses == 0);          // the held A did nothing in the new layout
    frames(2, padState(0));
    frames(1, padState(pad::kA, pad::kA)); frames(1, padState(0));
    CHECK(a.picksDeck == 1); CHECK(a.iniLayout == 2);             // the next fresh press does
    // Portable to Desktop by a tap, the touch held through the change and let go over the desktop button
    a = App{}; a.deckOpen = true;
    frames(3, padState(0));
    const ImVec2 dm(a.deckBtn.Min.x + a.deckBtn.GetWidth() * 0.25f, a.deckBtn.GetCenter().y);
    mouse(dm.x, dm.y, false); appFrame(padState(0));
    mouse(dm.x, dm.y, true); appFrame(padState(0));
    CHECK(a.picksDeck == 1);
    frames(3, padState(0));
    CHECK(!a.deckOpen);
    const ImVec2 sm(a.deskBtn.Min.x + a.deskBtn.GetWidth() * 0.75f, a.deskBtn.GetCenter().y);
    mouse(sm.x, sm.y, true); frames(3, padState(0));               // held, moved onto the Portable half
    mouse(sm.x, sm.y, false); frames(2, padState(0));              // let go there
    CHECK(a.picksDesk == 0); CHECK(a.otherPresses == 0);
    mouse(sm.x, sm.y, true); appFrame(padState(0)); mouse(sm.x, sm.y, false); appFrame(padState(0));
    CHECK(a.picksDesk == 1);                                       // a fresh click does
    // Desktop to Portable by the right arrow, the mouse held over where the Portable button will be
    a = App{}; a.deckOpen = false; a.focusDesk = true; a.padUsed = false;
    mouse(-100, -100, false); frames(3, padState(0));
    CHECK(GImGui->NavId == a.btnId);
    CHECK(!GImGui->NavCursorVisible);                             // no controller used: no nav cursor
    io.AddKeyEvent(ImGuiKey_RightArrow, true); appFrame(padState(0)); io.AddKeyEvent(ImGuiKey_RightArrow, false);
    CHECK(a.picksDesk == 1);
    mouse(1100, 70, true); frames(3, padState(0));                 // about where the Portable button's left half lands
    CHECK(a.deckOpen);
    const ImVec2 pm(a.deckBtn.Min.x + a.deckBtn.GetWidth() * 0.25f, a.deckBtn.GetCenter().y);
    mouse(pm.x, pm.y, true); frames(3, padState(0));
    mouse(pm.x, pm.y, false); frames(2, padState(0));
    CHECK(a.picksDeck == 0); CHECK(a.otherPresses == 0);
    mouse(pm.x, pm.y, true); appFrame(padState(0)); mouse(pm.x, pm.y, false); appFrame(padState(0));
    CHECK(a.picksDeck == 1);
    mouse(-100, -100, false);
    // after the change the D-pad moves down from the button to the first row
    a = App{}; a.deckOpen = false; a.focusDesk = true;
    frames(3, padState(0));
    CHECK(GImGui->NavId == a.btnId);
    frames(1, padState(pad::kDown, pad::kDown)); frames(2, padState(0));
    CHECK(GImGui->NavId == a.rowId);
    // closed before it landed: nothing changes while closed; the next opening picks from the ini as before
    a = App{}; a.deckOpen = true; a.nav.focus[0] = deck::kButton;
    frames(2, padState(0));
    a.nav.focus[0] = deck::kButton;
    appFrame(padState(pad::kA, pad::kA));
    CHECK(a.picksDeck == 1);
    a.open = false;                                                // closeMenu, the same frame
    frames(5, padState(0));
    CHECK(a.deckOpen);                                             // no change applied while closed
    a.open = true; a.swap = {};                                    // the open branch: g_swap = {} after the pick
    a.deckOpen = deck::deckLayoutOn(a.iniLayout, true);            // a Steam Deck, Desktop picked
    frames(3, padState(0));
    CHECK(!a.deckOpen);
}

// ---- the glove ----------------------------------------------------------------------------------------
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
        // the design's colours
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

// ---- the Portable look --------------------------------------------------------------------------------
static void deckLook() {
    static const int kRowsOnPage[3] = { 6, 7, 3 };
    for (const ImVec2 sz : { ImVec2(1280, 800), ImVec2(1920, 1080) }) {
        g_w = sz.x; g_h = sz.y;
        for (int p = 0; p < 3; ++p) {
            deckFrame(p); DeckOut o = deckFrame(p, 0);
            const float k = o.look.k;
            // every page's rows sit in the fixed slots: 54 px apart from the body's top, after the heading slot when
            // there is one, and end above the help box's hairline
            CHECK(o.nRows == kRowsOnPage[p]);
            CHECK(o.bodyEnd <= deck::kBodyEnd); CHECK(o.bodyEnd <= deck::kHelpRule);
            for (int i = 0; i < o.nRows; ++i) {
                const float want = deck::kBodyTop + i * deck::kPitch + (p == 1 && i >= 4 ? deck::kSubH : 0);
                CHECK(near(o.rowTop[i], want, 0.01));
                CHECK(o.rowRects[i].GetHeight() >= 52 * k - 0.5f);    // a touch target
            }
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
            for (float bandY : { (deck::kTabTop + deck::kTabBottom) * 0.5f, deck::kFootTop + 22.f }) {
                const float cy = o.look.p(0, bandY).y;
                std::vector<ImVec2> b;
                for (const ImVec2& q : sorted) if (std::fabs(q.y - cy) < 20 * k) b.push_back(q);
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
            CHECK(badges == 5);  // L1, R1; A, L1 R1, B
            // the footer: the key row with the release number at its right end, then the two trademark lines under it,
            // inside the panel's bottom padding
            const ImU32 dim = ImGui::GetColorU32(look::kDim);
            float verLeft = 1e9f, verRight = -1e9f, tmTop = 1e9f, tmBot = -1e9f;
            const float split = o.look.p(0, deck::kTradeTop - 2).y;
            for (int i = o.vtxFoot0; i < o.vtxFoot1; ++i) {
                const ImDrawVert& v = dl->VtxBuffer[i];
                if (v.col != dim) continue;
                if (v.pos.y < split) { if (v.pos.x > o.look.p(500, 0).x) { verLeft = std::min(verLeft, v.pos.x); verRight = std::max(verRight, v.pos.x); } }
                else { tmTop = std::min(tmTop, v.pos.y); tmBot = std::max(tmBot, v.pos.y); }
            }
            const float verW = g_font->CalcTextSizeA(17 * k, FLT_MAX, 0, "Lumenveil 1.0.2").x;
            CHECK(near(verRight, o.look.p(deck::kPanelW - 30, 0).x, 4 * k));    // right-aligned at the panel's padding
            CHECK(verRight - verLeft > verW * 0.8f);                            // and the whole release text is there
            CHECK(tmTop < 1e8f && tmTop >= o.look.p(0, deck::kTradeTop).y - 1);
            CHECK(tmBot - tmTop > 20 * k);                                      // two lines
            CHECK(tmBot <= o.look.p(0, deck::kPanelH - deck::kBottomPad).y + 1);
        }
    }
    g_w = 1280; g_h = 800;
}

// A tap on a dimmed Portable row (a list, a tick box, a slider) focuses it, so the help box can say why
// it is dimmed, and changes nothing.
struct DimOut { deck::RowOut row[3]; int v = 50; };
static DimOut dimFrame() {
    DimOut o;
    ImGui::GetIO().DisplaySize = ImVec2(g_w, g_h);
    ImGui::NewFrame();
    deck::Look L = deck::Look::at(g_w, g_h, g_font, g_font, g_font);
    deck::begin(L);
    deck::RowIn in; in.dim = true;
    o.row[0] = deck::cycler(L, "##dc", "Anti-Lag 2", "Off", in);
    o.row[1] = deck::toggle(L, "##dt", "Enabled", true, in);
    static deck::Typing typing;
    deck::slider(L, "##ds", "Cap", o.v, 0, 100, 5, "%", in, typing, &o.row[2]);
    deck::end(L);
    ImGui::Render();
    return o;
}
static void dimRowTap() {
    mouse(-100, -100, false); dimFrame(); dimFrame();
    const deck::Look L = deck::Look::at(g_w, g_h, g_font, g_font, g_font);
    for (int i = 0; i < 3; ++i) {
        // the middle of the row's label, then of its control
        for (float x : { deck::kRowL + 100.f, deck::kCtlL + 150.f }) {
            const ImVec2 p = L.p(x, deck::kBodyTop + i * deck::kPitch + deck::kRowH * 0.5f);
            bool pressed = false, stepped = false, changed = false;
            for (bool down : { false, true, false }) {
                mouse(p.x, p.y, down);
                const DimOut o = dimFrame();
                pressed |= o.row[i].focus;
                for (const deck::RowOut& r : o.row) stepped |= r.step != 0;
                changed |= o.v != 50;
            }
            CHECK(pressed); CHECK(!stepped); CHECK(!changed);
        }
    }
    mouse(-100, -100, false);
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
    desktopButton();
    deckButton();
    change();
    gloveShape();
    gloveRows();
    deckLook();
    dimRowTap();
    ImGui::DestroyContext();
    if (g_fail) { std::printf("%d failure(s)\n", g_fail); return 1; }
    std::printf("portable_test passed\n");
    return 0;
}
