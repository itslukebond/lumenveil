// Host test for src/xess_rules.h: the back buffer formats XeSS frame generation takes, and the XeSS preset picked for
// the game's render scale. Built and run by tests/check_xess.sh.
// Format values are DXGI_FORMAT numbers from Microsoft's dxgiformat.h. The preset ranges are Intel's dynamic input
// ranges for a 3840x2160 output (XeSS-SR developer guide, "Dynamic Input Resolution": each preset accepts 1.0x up to
// its scale factor: Ultra Quality Plus 1.3x, Ultra Quality 1.5x, Quality 1.7x, Balanced 2.0x, Performance 2.3x,
// Ultra Performance 3.0x), as xessGetOptimalInputResolution reports them.
#include "xess_rules.h"
#include <cstdio>

static int g_fails;
#define CHECK(cond, ...) do { if (!(cond)) { std::printf("FAIL %s:%d: ", __FILE__, __LINE__); std::printf(__VA_ARGS__); std::printf("\n"); ++g_fails; } } while (0)

static void formats() {
    struct Row { unsigned fmt; const char* name; bool ok; };
    static const Row rows[] = {
        { 24, "R10G10B10A2_UNORM (HDR10)", true },
        { 28, "R8G8B8A8_UNORM", true },
        { 29, "R8G8B8A8_UNORM_SRGB", true },
        { 87, "B8G8R8A8_UNORM", true },
        { 91, "B8G8R8A8_UNORM_SRGB", true },
        { 10, "R16G16B16A16_FLOAT (scRGB)", false },
        { 2, "R32G32B32A32_FLOAT", false },
        { 0, "UNKNOWN", false },
    };
    for (const Row& r : rows) CHECK(xessFgFormatOk(r.fmt) == r.ok, "format %u %s: %d, expected %d", r.fmt, r.name, xessFgFormatOk(r.fmt), r.ok);
}

static void presets() {
    // Intel's preset ids (xess.h): 100 Ultra Performance .. 105 Ultra Quality Plus, 106 native anti-aliasing
    const int uqp = 105, uq = 104, q = 103, b = 102, p = 101, up = 100, aa = 106;
    const XessRange r[] = {  // highest quality first, minimum = 3840x2160 / factor, rounded up
        { uqp, 2954, 1662, 3840, 2160 }, { uq, 2560, 1440, 3840, 2160 }, { q, 2259, 1271, 3840, 2160 },
        { b, 1920, 1080, 3840, 2160 }, { p, 1670, 940, 3840, 2160 }, { up, 1280, 720, 3840, 2160 },
    };
    struct Case { const char* scale; unsigned inW, inH; int want; bool fits; };
    static const Case cases[] = {  // the game's render scales at 3840x2160 (its scaling modes: 100, 67, 58, 50, 33 %)
        { "100%", 3840, 2160, aa, true },
        { "67%", 2572, 1447, uq, true },   // below Ultra Quality Plus's 2954 minimum
        { "58%", 2227, 1252, b, true },    // below Quality's 2259
        { "50%", 1920, 1080, b, true },
        { "33%", 1267, 712, up, false },   // below every minimum: the lowest-quality preset, reported as not fitting
    };
    for (const Case& c : cases) {
        bool fits = !c.fits;
        const int got = xessPickPreset(c.inW, c.inH, 3840, 2160, r, int(sizeof r / sizeof r[0]), aa, &fits);
        CHECK(got == c.want && fits == c.fits, "%s (%ux%u): preset %d fits %d, expected %d fits %d", c.scale, c.inW, c.inH, got, fits, c.want, c.fits);
    }
}

// Whether Intel's swap chain should be the one presenting: only with XeSS-FG as the launch's backend, and only while the
// game presents in a format XeSS-FG takes. Asked again each time the game creates or resizes its swap chain.
static void wanted() {
    struct Step { unsigned fmt; bool want; };
    struct Run { const char* what; bool chosen; Step steps[3]; };
    static const Run runs[] = {
        // The recorded SDR launch: created as 16-bit float, recreated as 10-bit a second later.
        { "launch 10 then 24", true, { { 10, false }, { 24, true }, { 24, true } } },
        // HDR (scRGB) turned on and off again in the game.
        { "24, 10, 24", true, { { 24, true }, { 10, false }, { 24, true } } },
        // Another backend chosen: never Intel's swap chain, whatever the format.
        { "not chosen", false, { { 10, false }, { 24, false }, { 87, false } } },
    };
    for (const Run& r : runs)
        for (const Step& s : r.steps)
            CHECK(xessFgWanted(r.chosen, s.fmt) == s.want, "%s: format %u wanted %d, expected %d", r.what, s.fmt, xessFgWanted(r.chosen, s.fmt), s.want);
}

int main() {
    formats();
    presets();
    wanted();
    if (g_fails) { std::printf("xess_test: %d failure(s)\n", g_fails); return 1; }
    std::printf("xess_test: PASS\n");
    return 0;
}
