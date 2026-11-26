// Host test for src/menu_pages.h when Streamline says DLSS frame generation cannot run on this PC (State::fgSupport, its
// slIsFeatureSupported result): the Frame generation row's help gives the reason and what to do, in its exact
// words (written out here, not taken from the code), on Windows only and only for DLSS; the status line stays as
// it is; a pending backend switch keeps its restart line; and every text fits both layouts, measured with the bundled
// TeX Gyre Adventor.
// Usage: menu_support_test <path to texgyreadventor-regular.otf>
#include "menu_pages.h"
#include "imgui.h"
#include <cstdio>
#include <cstring>
#include <string>

static int g_fail;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++g_fail; } } while (0)

using namespace pages;

static const char* const kHws =
    "Windows has Hardware-accelerated GPU scheduling off, and DLSS frame generation needs it. Turn it on in Settings, System, Display, Graphics, then restart the PC.";
static const char* const kOs = "Windows is too old for DLSS frame generation. Update Windows, then restart the game.";
static const char* const kDriver = "The NVIDIA driver is too old for DLSS frame generation. Update the driver, then restart the game.";
static const char* const kCard = "This graphics card can't run DLSS frame generation. Pick FSR above instead.";
static const char* const kRestart = "Restart the game to switch frame generation.";
static const char* const kVersion = "NVIDIA DLSS frame generation 310.9.16.";

static bool has(const Help& h, const char* t, Tone tone) {
    for (int i = 0; i < h.n; ++i) if (std::strcmp(h.line[i].text, t) == 0 && h.line[i].tone == tone) return true;
    return false;
}
static bool mentions(const Help& h, const char* t) {
    for (int i = 0; i < h.n; ++i) if (std::strstr(h.line[i].text, t)) return true;
    return false;
}
static int at(const Help& h, const char* t) {
    for (int i = 0; i < h.n; ++i) if (std::strcmp(h.line[i].text, t) == 0) return i;
    return -1;
}
// an RTX card on Windows with DLSS frame generation started, waiting for gameplay, Streamline's result `support`
static State dlss(int support) {
    State s;
    s.nvidia = true; s.want = 0; s.active = 0; s.upscaler = -1; s.dlssVersion = "310.9.16";
    s.ratio = 67; s.live = 67; s.inW = 2560; s.inH = 1440; s.outW = 3840; s.outH = 2160; s.dispW = 3840; s.dispH = 2160;
    s.backend = 0; s.launchBackend = -1; s.picked = 0; s.running = 0; s.fgBackend = 0; s.fgAvailable = true; s.fgEnable = 1;
    s.launchEnable = true; s.frames = 1; s.reflex = 1; s.dlssgVersion = "310.9.16";
    s.fgSupport = support; s.wine = false;
    return s;
}
// the same, with FSR frame generation picked in the menu and not yet restarted into
static State pendingFsr(int support) { State s = dlss(support); s.backend = 2; s.picked = 1; return s; }

static void reasons() {
    const struct { int r; const char* text; } cases[] = { { 4, kHws }, { 3, kOs }, { 2, kDriver }, { 6, kCard }, { 7, kCard } };
    for (const auto& c : cases) {
        const Help h = help(kBackend, dlss(c.r));
        CHECK(has(h, c.text, kAmber)); CHECK(h.flag == kWarnFlag);
        CHECK(!mentions(h, kVersion));  // the reason takes the version line's place
    }
    for (int r : { 0, 1, 5, 99 }) {  // supported, or a result with no signed-off words: the row as today
        const Help h = help(kBackend, dlss(r));
        CHECK(has(h, kVersion, kDim)); CHECK(h.flag != kWarnFlag); CHECK(!mentions(h, "DLSS frame generation needs"));
        CHECK(!mentions(h, "too old")); CHECK(!mentions(h, "can't run"));
    }
}

static void gates() {
    State w = dlss(4); w.wine = true;  // Proton: the log only
    Help h = help(kBackend, w);
    CHECK(!mentions(h, "scheduling")); CHECK(has(h, kVersion, kDim)); CHECK(h.flag != kWarnFlag);
    State f = dlss(4); f.fgBackend = 1; f.running = 1; f.picked = 1; f.fsrFgVersion = "3.1.4";  // FSR running: not DLSS's result
    h = help(kBackend, f);
    CHECK(!mentions(h, "scheduling")); CHECK(h.flag != kWarnFlag);
    // the status line is unchanged by the result
    CHECK(std::string(status(dlss(4)).fg) == std::string(status(dlss(0)).fg));
    CHECK(status(dlss(4)).fgTone == status(dlss(0)).fgTone);
}

static void pendingSwitch() {
    // FSR picked after the card line: the card line and the restart line both fit, in that order
    Help h = help(kBackend, pendingFsr(6));
    CHECK(has(h, kCard, kAmber)); CHECK(has(h, kRestart, kGreen)); CHECK(at(h, kCard) < at(h, kRestart));
    CHECK(!mentions(h, kVersion));
    h = help(kBackend, pendingFsr(3));
    CHECK(has(h, kOs, kAmber)); CHECK(has(h, kRestart, kGreen));
    // the three-line scheduling reason leaves no room: the restart line the player asked for stays
    h = help(kBackend, pendingFsr(4));
    CHECK(has(h, kRestart, kGreen)); CHECK(!mentions(h, "scheduling"));
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
        for (int r : { 2, 3, 4, 6, 7 })
            for (bool portable : { false, true })
                for (bool pending : { false, true }) {
                    State s = pending ? pendingFsr(r) : dlss(r); s.portable = portable;
                    const Help hp = help(kBackend, s);
                    int used = 0;
                    for (int i = 0; i < hp.n; ++i) used += lines(f, z.helpPx, z.helpW, hp.line[i].text);
                    if (used > kHelpLines) { std::printf("FAIL: result %d%s needs %d lines at %.0f px\n", r, pending ? " (switch pending)" : "", used, z.helpW); ++g_fail; }
                }
    ImGui::DestroyContext();
}

int main(int argc, char** argv) {
    reasons();
    gates();
    pendingSwitch();
    if (argc > 1) fit(argv[1]);
    else { std::printf("FAIL: no font path given for the fit test\n"); ++g_fail; }
    if (g_fail) { std::printf("%d failed\n", g_fail); return 1; }
    std::printf("menu_support_test passed\n");
    return 0;
}
