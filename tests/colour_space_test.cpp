// Host test for the colour space our swap chain presents in after a format change (src/colour_space.h). Built and run
// by tests/check_colour_space.sh. Expected values are DXGI_COLOR_SPACE_TYPE numbers from dxgicommon.h, written by hand:
// 0 = RGB_FULL_G22_NONE_P709 (sRGB), 1 = RGB_FULL_G10_NONE_P709 (scRGB), 12 = RGB_FULL_G2084_NONE_P2020 (HDR10).
// Formats: 10 = R16G16B16A16_FLOAT, 24 = R10G10B10A2_UNORM, 28 = R8G8B8A8_UNORM, 87 = B8G8R8A8_UNORM.
#include "colour_space.h"
#include <cstdio>

static int g_fails;
#define CHECK(cond, ...) do { if (!(cond)) { std::printf("FAIL %s:%d: ", __FILE__, __LINE__); std::printf(__VA_ARGS__); std::printf("\n"); ++g_fails; } } while (0)

int main() {
    // GitHub #5: the game recreates its swap chain from fmt 10 to fmt 24 and never sets a colour space: sRGB, not scRGB
    CHECK(colour::presentColourSpace(24, -1, false) == 0, "fmt 24 with no game colour space must be sRGB (0)");
    CHECK(colour::presentColourSpace(28, -1, false) == 0, "fmt 28 with no game colour space must be sRGB (0)");
    CHECK(colour::presentColourSpace(10, -1, false) == 1, "fmt 10 with no game colour space must be scRGB (1)");
    // the game's own colour space wins when our swap chain can present it at the new format
    CHECK(colour::presentColourSpace(24, 12, true) == 12, "HDR10 set by the game on fmt 24 must stay HDR10 (12)");
    CHECK(colour::presentColourSpace(10, 1, true) == 1, "scRGB set by the game on fmt 10 must stay scRGB (1)");
    // and falls back to the format's own default when it cannot
    CHECK(colour::presentColourSpace(24, 1, false) == 0, "scRGB refused at fmt 24 must fall back to sRGB (0)");
    CHECK(colour::presentColourSpace(10, 12, false) == 1, "HDR10 refused at fmt 10 must fall back to scRGB (1)");
    CHECK(colour::presentColourSpace(87, -1, false) == 0, "fmt 87 (B8G8R8A8) with no game colour space must be sRGB (0)");
    // the game choosing sRGB (0) is kept too, on SDR and FP16 formats
    CHECK(colour::presentColourSpace(24, 0, true) == 0, "sRGB set by the game on fmt 24 must stay sRGB (0)");
    CHECK(colour::presentColourSpace(10, 0, true) == 0, "sRGB set by the game on fmt 10 must stay sRGB (0)");
    CHECK(colour::presentColourSpace(10, 0, false) == 1, "sRGB refused at fmt 10 must fall back to scRGB (1)");
    if (!g_fails) std::printf("colour_space_test: all passed\n");
    return g_fails ? 1 : 0;
}
