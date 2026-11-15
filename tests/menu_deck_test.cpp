// Host test for src/menu_deck.h: Steam Deck detection, the Layout setting, the Deck layout's scale, its controller
// steps and its rows driven by the mouse in a headless Dear ImGui context. Expected values come from the spec
// (docs/changes 2026-11-13-steam-deck-ui-mode: the PCI IDs, the setting's values, 1280x800 at scale 1, steps of 5,
// the signed-off mockup's 760x768 panel), not from the code.
#include "menu_deck.h"
#include "imgui_internal.h"
#include <cstdio>
#include <cmath>
#include <string>

static int g_fail;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++g_fail; } } while (0)

using namespace deck;

static void detection() {
    CHECK(isSteamDeck(0x1002, 0x163F, nullptr));   // Steam Deck LCD
    CHECK(isSteamDeck(0x1002, 0x1435, nullptr));   // Steam Deck OLED
    CHECK(isSteamDeck(0x10DE, 0x2684, "1"));       // SteamDeck=1 on any adapter
    CHECK(isSteamDeck(0, 0, "1"));
    CHECK(!isSteamDeck(0x1002, 0x73BF, nullptr));  // a Radeon RX 6800
    CHECK(!isSteamDeck(0x10DE, 0x2684, nullptr));  // an RTX 4090
    CHECK(!isSteamDeck(0x10DE, 0x163F, nullptr));  // the device ID alone is not enough
    CHECK(!isSteamDeck(0x10DE, 0x2684, "0"));
    CHECK(!isSteamDeck(0x10DE, 0x2684, ""));
    CHECK(!isSteamDeck(0x1002, 0x73BF, "10"));
    CHECK(std::string(foundBy(0x1002, 0x163F, nullptr)) == "adapter");
    CHECK(std::string(foundBy(0x10DE, 0x2684, "1")) == "SteamDeck=1");
    CHECK(std::string(foundBy(0x10DE, 0x2684, "0")) == "not found");
}

static void layoutSetting() {
    // 0 Automatic follows the detection, 1 Steam Deck and 2 Desktop force it, anything else reads as Automatic
    for (bool d : { false, true }) {
        CHECK(deckLayoutOn(0, d) == d);
        CHECK(deckLayoutOn(1, d));
        CHECK(!deckLayoutOn(2, d));
        CHECK(deckLayoutOn(-1, d) == d);
        CHECK(deckLayoutOn(7, d) == d);
    }
    CHECK(layoutValue(0) == 0); CHECK(layoutValue(1) == 1); CHECK(layoutValue(2) == 2);
    CHECK(layoutValue(-1) == 0); CHECK(layoutValue(7) == 0);
}

static bool near(float a, float b) { return std::fabs(a - b) < 1e-4f; }

static void steps() {
    CHECK(near(deckScale(1280, 800), 1.0f));
    CHECK(near(deckScale(1280, 720), 0.9f));
    CHECK(near(deckScale(1920, 1080), 1.35f));
    // lists wrap both ways
    CHECK(wrapStep(0, 3, -1) == 2); CHECK(wrapStep(2, 3, 1) == 0); CHECK(wrapStep(1, 3, 1) == 2); CHECK(wrapStep(1, 4, -1) == 0);
    // sliders: Cap 30..480 and Sharpening 0..100, steps of 5, stopping at the ends
    CHECK(clampStep(30, 30, 480, 5, -1) == 30); CHECK(clampStep(30, 30, 480, 5, 1) == 35);
    CHECK(clampStep(480, 30, 480, 5, 1) == 480); CHECK(clampStep(478, 30, 480, 5, 1) == 480);
    CHECK(clampStep(0, 0, 100, 5, -1) == 0); CHECK(clampStep(100, 0, 100, 5, 1) == 100); CHECK(clampStep(50, 0, 100, 5, -1) == 45);
    // a value not in the list (DLSS left in the ini by a desktop) stays until stepped: right goes to the first entry,
    // left to the last
    const int ups[] = { -1, 1, 2 };
    CHECK(listStep(ups, 3, 0, 1) == -1); CHECK(listStep(ups, 3, 0, -1) == 2);
    CHECK(listStep(ups, 3, -1, 1) == 1); CHECK(listStep(ups, 3, 2, 1) == -1); CHECK(listStep(ups, 3, -1, -1) == 2);
    CHECK(listStep(ups, 3, 1, 0) == 1);
    // the focus: one row per press, stopping at both ends, remembered per page; pages wrap both ways
    Nav n;
    n.move(4, -1); CHECK(n.focus[0] == 0);
    for (int i = 0; i < 9; ++i) n.move(4, 1);
    CHECK(n.focus[0] == 3);
    n.turn(1); CHECK(n.page == 1); CHECK(n.focus[1] == 0);
    n.move(5, 1); n.move(5, 1); CHECK(n.focus[1] == 2);
    n.turn(1); CHECK(n.page == 2); n.turn(1); CHECK(n.page == 0); CHECK(n.focus[0] == 3);
    n.turn(-1); CHECK(n.page == 2); n.turn(-1); CHECK(n.page == 1); CHECK(n.focus[1] == 2);
}

// ---- rows in a headless context --------------------------------------------------------------------------------------
static float g_w = 1280, g_h = 800;
static ImFont* g_font;
struct Frame {
    Look look;
    RowOut out[6];
    int value = 50, rowsDrawn = 0;
    bool sliderChanged = false;
    float bodyEnd = 0;
    ImRect rects[8]; int nRects = 0;
    int tab = -1;
};
static int g_slider = 50, g_page = 0;
static Typing g_typing;

// The longest page (Frame generation): a section, a list, a status line, a toggle, a list, a status line, a list and a
// slider, with the longest values the menu shows.
static Frame frame(bool dim = false, RowIn pad = {}, RowIn spad = {}) {  // pad: the Backend row's, spad: the slider's
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(g_w, g_h);
    ImGui::NewFrame();
    Frame f;
    f.look = Look::at(g_w, g_h, g_font, g_font, g_font);
    Look& L = f.look;
    begin(L);
    f.tab = tabs(L, g_page, nullptr);
    section(L, "Frame generation");
    auto row = [&](int i, RowOut o) { f.out[i] = o; f.rects[f.nRects++] = ImRect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax()); ++f.rowsDrawn; };
    RowIn in = pad; in.dim = dim;
    row(0, cycler(L, "##backend", "Backend", "Intel XeSS Frame Generation", in));
    status(L, "Not available: Intel XeSS frame generation does not take the game's HDR output format. Turn HDR off.", kDim);
    row(1, toggle(L, "##fg", "Enabled", true, RowIn{}));
    row(2, cycler(L, "##mode", "Mode", "2\xC3\x97", RowIn{}));
    status(L, "Generating 2.0\xC3\x97 \xC2\xB7 120 shown from 60 rendered", kGreen);
    row(3, cycler(L, "##cap", "Frame cap", "Auto (Adaptive Sync)", RowIn{}));
    RowIn sl = spad; sl.dim = dim;
    RowOut so;
    f.sliderChanged = slider(L, "##limit", "Cap", g_slider, 30, 480, 5, " fps", sl, g_typing, &so);
    row(4, so);
    f.bodyEnd = L.y;
    footer(L, "Saves at once");
    end(L);
    ImGui::Render();
    return f;
}
static void mouse(float x, float y, bool down) {
    ImGuiIO& io = ImGui::GetIO();
    io.AddMousePosEvent(x, y);
    io.AddMouseButtonEvent(0, down);
}
static ImVec2 mid(const ImRect& r) { return ImVec2((r.Min.x + r.Max.x) / 2, (r.Min.y + r.Max.y) / 2); }

static void layout() {
    for (const ImVec2 sz : { ImVec2(1280, 800), ImVec2(1280, 720), ImVec2(1920, 1080) }) {
        g_w = sz.x; g_h = sz.y;
        Frame f = frame(); f = frame();
        const float k = deckScale(sz.x, sz.y);
        const ImVec2 o = f.look.o;
        CHECK(near(f.look.k, k));
        // the panel, centred and on screen
        CHECK(o.x >= 0 && o.y >= 0);
        CHECK(near(o.x * 2 + 760 * k, sz.x)); CHECK(near(o.y * 2 + 768 * k, sz.y));
        CHECK(o.x + 760 * k <= sz.x && o.y + 768 * k <= sz.y);
        // every row inside the panel's body, above the footer: nothing scrolls
        CHECK(f.nRects == 5);
        for (int i = 0; i < f.nRects; ++i) {
            const ImRect& r = f.rects[i];
            CHECK(r.Min.x >= o.x && r.Max.x <= o.x + 760 * k + 0.01f);
            CHECK(r.Min.y >= o.y + 142 * k && r.Max.y <= o.y + 704 * k + 0.01f);
            CHECK(near(r.GetHeight(), 66 * k)); CHECK(near(r.GetWidth(), 720 * k));
        }
        CHECK(f.bodyEnd <= kFootTop);
        CHECK(f.bodyEnd > kBodyTop + 5 * 66);  // the five rows were laid out
        ImGuiWindow* w = ImGui::FindWindowByName("##deck");
        CHECK(w && w->ScrollMax.y == 0 && std::fabs(w->Size.y - 768 * k) < 1 && std::fabs(w->Size.x - 760 * k) < 1);
    }
    g_w = 1280; g_h = 800;
}

static void pointer() {
    frame(); frame();
    Frame f = frame();
    const ImRect backend = f.rects[0];
    const float k = f.look.k, y = mid(backend).y;
    auto tap = [&](float x, float yy, bool dim = false) {
        mouse(x, yy, false); frame(dim); mouse(x, yy, true); Frame g = frame(dim); mouse(x, yy, false); frame(dim); return g;
    };
    // the right arrow steps +1, the left arrow -1, the row itself does what A does (+1)
    Frame g = tap(backend.Max.x - 20 * k, y);
    CHECK(g.out[0].press); CHECK(g.out[0].step == 1);
    g = tap(backend.Min.x + 310 * k, y);
    CHECK(g.out[0].press); CHECK(g.out[0].step == -1);
    g = tap(backend.Min.x + 100 * k, y);
    CHECK(g.out[0].press); CHECK(g.out[0].step == 1);
    // the toggle row flips
    g = tap(mid(f.rects[1]).x, mid(f.rects[1]).y);
    CHECK(g.out[1].press); CHECK(g.out[1].step == 1);
    // a dimmed row ignores taps on its arrows and on itself, and keeps its place
    g = tap(backend.Max.x - 20 * k, y, true);
    CHECK(!g.out[0].press); CHECK(g.out[0].step == 0);
    CHECK(g.rects[0].Min.x == backend.Min.x && g.rects[0].Min.y == backend.Min.y && g.rects[0].Max.y == backend.Max.y);
    g = tap(backend.Min.x + 100 * k, y, true);
    CHECK(!g.out[0].press);
    // the slider: a press on the track sets the value there, a drag follows, clamped at the ends
    const ImRect cap = f.rects[4];
    const float cy = mid(cap).y, left = cap.Min.x + 290 * k, right = cap.Min.x + 590 * k;  // the track: 300 wide
    g_slider = 50;
    mouse(left, cy, false); frame();
    mouse(left + 150 * k, cy, true); g = frame();
    CHECK(g.out[4].press); CHECK(g.sliderChanged); CHECK(g_slider == 255);  // the middle of 30..480
    mouse(left + 300 * k, cy, true); frame();
    CHECK(g_slider == 480);
    mouse(left - 80 * k, cy, true); frame();
    CHECK(g_slider == 30);
    mouse(right + 400 * k, cy - 200 * k, true); frame();  // far off the row while held: still follows, clamped
    CHECK(g_slider == 480);
    mouse(right, cy, false); frame();
    // a dimmed slider ignores a press and a drag
    g_slider = 100;
    mouse(left + 150 * k, cy, false); frame(true);
    mouse(left + 150 * k, cy, true); g = frame(true);
    CHECK(!g.out[4].press); CHECK(g_slider == 100);
    mouse(left, cy, true); frame(true);
    CHECK(g_slider == 100);
    mouse(left, cy, false); frame(true); frame();
    // tabs: a press on a tab picks its page, on L1 / R1 the page before / after (wrapping)
    const ImVec2 o = f.look.o;
    auto tabTap = [&](float x) { mouse(o.x + x * k, o.y + 112 * k, false); frame(); mouse(o.x + x * k, o.y + 112 * k, true);
                                 Frame t = frame(); mouse(o.x + x * k, o.y + 112 * k, false); frame(); return t.tab; };
    g_page = 0;
    CHECK(tabTap(80 + 300) == 1);
    CHECK(tabTap(80 + 500) == 2);
    CHECK(tabTap(80 + 100) == 0);
    CHECK(tabTap(50) == 2);    // L1 from page 0
    CHECK(tabTap(710) == 1);   // R1 from page 0
    g_page = 2;
    CHECK(tabTap(710) == 0);
    g_page = 0;
}

static void controller() {
    frame(); frame();
    RowIn focused; focused.focused = true;
    // left / right on the focused list: -1 / +1; A steps a list once; a dimmed or unfocused row takes nothing
    RowIn l = focused; l.lr = -1;
    CHECK(frame(false, l).out[0].step == -1);
    RowIn r = focused; r.lr = 1;
    CHECK(frame(false, r).out[0].step == 1);
    RowIn a = focused; a.a = true;
    CHECK(frame(false, a).out[0].step == 1);
    CHECK(frame(true, r).out[0].step == 0);
    CHECK(frame(true, a).out[0].step == 0);
    CHECK(frame(false, RowIn{ false, false, 1, true }).out[0].step == 0);  // not focused: nothing
    // the slider steps by 5 and stops at its ends
    g_slider = 470;
    frame(false, {}, r); CHECK(g_slider == 475);
    frame(false, {}, r); frame(false, {}, r); CHECK(g_slider == 480);
    frame(true, {}, l); CHECK(g_slider == 480);
    frame(false, {}, l); CHECK(g_slider == 475);
    frame(false, {}, RowIn{ false, false, 1, false }); CHECK(g_slider == 475);  // not focused: nothing
    // A on the slider: its number box takes typing; Enter sets the typed value, clamped
    frame(false, {}, a);
    bool active = false;
    for (int i = 0; i < 4 && !active; ++i) { frame(); active = g_typing.active; }
    CHECK(active);
    CHECK(g_typing.id != 0);
    ImGuiIO& io = ImGui::GetIO();
    io.AddKeyEvent(ImGuiKey_Backspace, true); frame(); io.AddKeyEvent(ImGuiKey_Backspace, false); frame();
    io.AddInputCharacter('9'); io.AddInputCharacter('0'); io.AddInputCharacter('0'); frame();
    io.AddKeyEvent(ImGuiKey_Enter, true); Frame e = frame(); io.AddKeyEvent(ImGuiKey_Enter, false); frame();
    CHECK(e.sliderChanged); CHECK(g_slider == 480); CHECK(!g_typing.active); CHECK(g_typing.id == 0);
    // B (leave) backs out of the box: the value stays
    g_slider = 100;
    frame(false, {}, a);
    for (int i = 0; i < 4 && !g_typing.active; ++i) frame();
    CHECK(g_typing.active);
    io.AddInputCharacter('3'); frame();
    leaveTyping(g_typing);
    CHECK(g_typing.id == 0);  // at once: the same frame's L1 / R1 and D-pad go to the menu again
    frame(); frame();
    CHECK(!g_typing.active); CHECK(g_typing.id == 0); CHECK(g_slider == 100);
    // a press on the number box takes typing too
    Frame f = frame();
    const ImRect cap = f.rects[4];
    const float bx = cap.Min.x + (604 + 52) * f.look.k, cy = mid(cap).y;
    mouse(bx, cy, false); frame(); mouse(bx, cy, true); frame(); mouse(bx, cy, false);
    for (int i = 0; i < 4 && !g_typing.active; ++i) frame();
    CHECK(g_typing.active);
    leaveTyping(g_typing); frame(); frame();
    CHECK(!g_typing.active);
}

// The slider dimmed while its number box is open (Frame cap set to Off or Auto by a tap): the box lets go at once, so
// the D-pad, A, L1 / R1 and Esc go to the menu again; the value stays.
static void dimWhileTyping() {
    g_slider = 120;
    RowIn a; a.focused = true; a.a = true;
    frame(false, {}, a);
    for (int i = 0; i < 4 && !g_typing.active; ++i) frame();
    CHECK(g_typing.active);
    frame(true);
    CHECK(g_typing.id == 0); CHECK(!g_typing.active);
    frame(true); frame();
    CHECK(g_typing.id == 0); CHECK(g_slider == 120);
}

int main() {
    detection();
    layoutSetting();
    steps();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.DeltaTime = 1.0f / 60;
    ImFontConfig fc; fc.SizePixels = 26;
    g_font = io.Fonts->AddFontDefault(&fc);
    unsigned char* px; int w, h;
    io.Fonts->GetTexDataAsRGBA32(&px, &w, &h);
    layout();
    pointer();
    controller();
    dimWhileTyping();
    ImGui::DestroyContext();
    if (g_fail) { std::printf("%d failure(s)\n", g_fail); return 1; }
    std::printf("menu_deck_test passed\n");
    return 0;
}
