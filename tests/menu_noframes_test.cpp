// Host test for src/menu_pages.h when frame generation is on but adds no frames (State::noFrames: 1 no frames, no cause
// named; 2 no frames with the game's 16-bit scRGB HDR output): the status line, the Enabled and Frame generation rows'
// help and the FPS counter's line stop showing frame generation as generating, in their exact words
// (written out here, not taken from the code); noFrames 0 keeps today's texts; and every new text fits both layouts,
// measured with the bundled TeX Gyre Adventor.
// Usage: menu_noframes_test <path to texgyreadventor-regular.otf>
#include "menu_pages.h"
#include "imgui.h"
#include <cstdio>
#include <cstring>
#include <string>

static int g_fail;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++g_fail; } } while (0)

using namespace pages;

static const char* const kScrgbLine =
    "Adding no frames: DLSS frame generation does not support the game's 16-bit scRGB HDR output. Turn HDR off in the game to use it.";
static const char* const kEnabledScrgb = "On, but adding no frames with the game's HDR output.";
static const char* const kEnabledNone = "On, but adding no frames at the moment.";

static bool has(const Help& h, const char* t, Tone tone) {
    for (int i = 0; i < h.n; ++i) if (std::strcmp(h.line[i].text, t) == 0 && h.line[i].tone == tone) return true;
    return false;
}
static bool mentions(const Help& h, const char* t) {
    for (int i = 0; i < h.n; ++i) if (std::strstr(h.line[i].text, t)) return true;
    return false;
}
// an RTX card with DLSS frame generation on, showing one frame per rendered one
static State dlssOn(int noFrames) {
    State s;
    s.nvidia = true; s.want = 0; s.active = 0; s.upscaler = -1; s.dlssVersion = "310.9.16";
    s.ratio = 67; s.live = 67; s.inW = 2560; s.inH = 1440; s.outW = 3840; s.outH = 2160; s.dispW = 3840; s.dispH = 2160;
    s.backend = 0; s.launchBackend = -1; s.picked = 0; s.running = 0; s.fgBackend = 0; s.fgAvailable = true; s.fgEnable = 1;
    s.launchEnable = true; s.frames = 1; s.reflex = 1; s.dlssgVersion = "310.9.16";
    s.generating = true; s.shown = 60; s.rendered = 60; s.noFrames = noFrames;
    return s;
}

static void statusLine() {
    Status st = status(dlssOn(2));
    CHECK(std::string(st.fg) == "adding no frames"); CHECK(st.fgTone == kAmber);
    st = status(dlssOn(1));
    CHECK(std::string(st.fg) == "adding no frames"); CHECK(st.fgTone == kDim);
    State g = dlssOn(0); g.shown = 120;  // generating: as today
    st = status(g);
    CHECK(std::string(st.fg) == "generating 2.0\xC3\x97"); CHECK(st.fgTone == kVendor); CHECK(st.fgVendor == 0);
    State off = dlssOn(0); off.generating = false; off.fgEnable = 0;  // off: as today
    CHECK(std::string(status(off).fg) == "off");
}

static void enabledRow() {
    Help h = help(kFgEnabled, dlssOn(2));
    CHECK(has(h, kEnabledScrgb, kDim)); CHECK(!mentions(h, "Generating"));
    CHECK(has(h, "Shows generated frames between the ones the game renders.", kInk));
    h = help(kFgEnabled, dlssOn(1));
    CHECK(has(h, kEnabledNone, kDim)); CHECK(!mentions(h, "Generating")); CHECK(!mentions(h, "HDR"));
    State g = dlssOn(0); g.shown = 120;
    h = help(kFgEnabled, g);
    CHECK(has(h, "Generating 2.0\xC3\x97: 120 fps shown from 60 rendered.", kDim));
    CHECK(!mentions(h, "adding no frames"));
}

static void backendRow() {
    Help h = help(kBackend, dlssOn(2));
    CHECK(has(h, kScrgbLine, kAmber)); CHECK(h.flag == kWarnFlag);
    CHECK(!mentions(h, "NVIDIA DLSS frame generation 310.9.16."));  // the reason replaces the version line
    for (int n : { 0, 1 }) {  // no reason named: the row is as today
        h = help(kBackend, dlssOn(n));
        CHECK(has(h, "NVIDIA DLSS frame generation 310.9.16.", kDim)); CHECK(!mentions(h, "scRGB")); CHECK(h.flag != kWarnFlag);
    }
    // another backend picked in the menu: its restart line comes first and the reason gives way, so the box still fits
    State r = dlssOn(2); r.backend = 3; r.picked = 2;
    h = help(kBackend, r);
    CHECK(has(h, "Restart the game to switch frame generation.", kGreen)); CHECK(!mentions(h, "scRGB"));
}

static void counter() {
    char t[32];
    CHECK(!counterFg(t, sizeof t, true, true, 2, 60, 60)); CHECK(std::string(t) == "FG no frames");
    CHECK(!counterFg(t, sizeof t, true, true, 1, 60, 60)); CHECK(std::string(t) == "FG no frames");
    CHECK(counterFg(t, sizeof t, true, true, 0, 120, 60)); CHECK(std::string(t) == "FG 2.0\xC3\x97");  // vendor colour
    CHECK(!counterFg(t, sizeof t, true, false, 0, 60, 60)); CHECK(std::string(t) == "FG off");
    CHECK(!counterFg(t, sizeof t, false, false, 0, 60, 0)); CHECK(std::string(t) == "FG n/a");
    CHECK(!counterFg(t, sizeof t, true, true, 0, 0, 0)); CHECK(std::string(t) == "FG off");  // nothing counted yet
}

static int lines(ImFont* f, float px, float w, const char* t) { return int(f->CalcTextSizeA(px, FLT_MAX, w, t).y / px + 0.5f); }
static void fit(const char* fontPath) {
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    static const ImWchar ranges[] = { 0x20, 0xFF, 0x2190, 0x2193, 0 };
    ImFont* f = io.Fonts->AddFontFromFileTTF(fontPath, 19, nullptr, ranges);
    CHECK(f != nullptr);
    if (!f) { ImGui::DestroyContext(); return; }
    unsigned char* px; int w, h;
    io.Fonts->GetTexDataAsRGBA32(&px, &w, &h);
    for (const Fit& z : { kDeskFit, kDeckFit })
        for (int n : { 1, 2 })
            for (bool portable : { false, true }) {
                    State s = dlssOn(n); s.portable = portable;
                    for (Id id : { kBackend, kFgEnabled }) {
                        const Help hp = help(id, s);
                        int used = 0;
                        for (int i = 0; i < hp.n; ++i) used += lines(f, z.helpPx, z.helpW, hp.line[i].text);
                        if (used > kHelpLines) { std::printf("FAIL: row %d, noFrames %d needs %d lines at %.0f px\n", int(id), n, used, z.helpW); ++g_fail; }
                    }
                    char t[200];
                    const Status st = status(s);
                    std::snprintf(t, sizeof t, "DLSS 310.10.100   3840\xC3\x97" "2160 \xE2\x86\x92 3840\xC3\x97" "2160   Frame generation %s", st.fg);
                    if (f->CalcTextSizeA(z.statusPx, FLT_MAX, 0, t).x > z.statusW) { std::printf("FAIL: status too wide: %s\n", t); ++g_fail; }
                }
    ImGui::DestroyContext();
}

int main(int argc, char** argv) {
    statusLine();
    enabledRow();
    backendRow();
    counter();
    if (argc > 1) fit(argv[1]);
    else { std::printf("FAIL: no font path given for the fit test\n"); ++g_fail; }
    if (g_fail) { std::printf("%d failed\n", g_fail); return 1; }
    std::printf("menu_noframes_test passed\n");
    return 0;
}
