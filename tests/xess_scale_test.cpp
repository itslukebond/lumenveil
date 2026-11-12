// Host test for the Ultra Performance scaling mode with Intel XeSS (src/xess_rules.h). Built and run by
// tests/check_xess_scale.sh.
// The game renders at its RenderingResolutionRatio percent of the output, rounded down (33% of 3840x2160 renders
// 1267x712), but only for the scale values on its own list: 34 renders at 100% (GPU session 2026-11-11, bench xess34).
// The values below are the ones seen rendering at their percent. Intel's XeSS-SR developer guide, "Dynamic Input Resolution": Ultra Performance accepts 1.0x up to 3.0x,
// so its smallest input is the output divided by 3, rounded up. The expected minimums below are that division, done
// by hand from the output sizes, not by the code under test.
#include "xess_rules.h"
#include <cstdio>

static int g_fails;
#define CHECK(cond, ...) do { if (!(cond)) { std::printf("FAIL %s:%d: ", __FILE__, __LINE__); std::printf(__VA_ARGS__); std::printf("\n"); ++g_fails; } } while (0)

int main() {
    struct Out { unsigned w, h, minW, minH; };
    static const Out outs[] = {  // output size, then output / 3 rounded up
        { 1280, 720, 427, 240 }, { 1920, 1080, 640, 360 }, { 2560, 1440, 854, 480 },
        { 3440, 1440, 1147, 480 }, { 3840, 2160, 1280, 720 },
    };
    static const int gameRenders[] = { 33, 50, 58, 67, 100 };  // scale values seen rendering at their percent
    auto rendered = [&](int pct) { for (int v : gameRenders) if (v == pct) return true; return false; };
    const int xessPct = ultraPerformancePct(true), otherPct = ultraPerformancePct(false);
    CHECK(otherPct == 33, "Ultra Performance for DLSS and FSR: %d%%, expected 33%% (the game's own mode)", otherPct);
    // XeSS: Ultra Performance is either not offered (0), with Performance (50%) the lowest mode, or a value the game
    // renders at that is also inside XeSS's range (checked per output size below).
    CHECK(xessPct == 0 || rendered(xessPct), "XeSS Ultra Performance %d%%: not a scale value the game renders at (34 renders at 100%%)", xessPct);
    CHECK(xessPct == 0 || (xessPct > 0 && xessPct < 50), "XeSS Ultra Performance %d%%: not below Performance's 50%%", xessPct);
    CHECK(xessLowestPct() == (xessPct ? xessPct : 50), "XeSS lowest mode %d%%, expected %d%%", xessLowestPct(), xessPct ? xessPct : 50);
    for (const Out& o : outs) {
        const int lowest = xessPct ? xessPct : 50;
        const unsigned w = o.w * lowest / 100, h = o.h * lowest / 100;
        CHECK(w >= o.minW && h >= o.minH, "XeSS at %ux%u: %d%% renders %ux%u, below Ultra Performance's %ux%u", o.w, o.h, lowest, w, h, o.minW, o.minH);
        const XessRange r[] = { { 100, o.minW, o.minH, o.w, o.h } };
        bool fits = false;
        xessPickPreset(w, h, o.w, o.h, r, 1, 106, &fits);
        CHECK(fits, "XeSS at %ux%u: %ux%u reported as outside Ultra Performance's range", o.w, o.h, w, h);
        // control: the game's 33% is below the range at this size, so the case above is the one that matters
        const unsigned w33 = o.w * 33 / 100, h33 = o.h * 33 / 100;
        CHECK(w33 < o.minW || h33 < o.minH, "control: 33%% at %ux%u renders %ux%u, inside the range", o.w, o.h, w33, h33);
    }
    if (g_fails) return 1;
    std::printf("xess_scale_test: PASS\n");
    return 0;
}
