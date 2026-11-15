// Host test for src/present_mode.h: which presentation a launch uses, when the native path needs a D3D12 device, and
// when the game's own swap chain gets the tearing flag. Expected values are the spec's (docs/changes
// presenter-copy-handover, criteria 1 and 12), written out cell by cell: [d3d12] present 0 is off (the game presents,
// no menu); 2 is always our D3D12 presenter; any other value is our presenter when a frame generation backend started at
// launch and native when none did. Natively a D3D12 device exists only for FSR or XeSS upscaling. Tearing natively:
// supported, not under Wine/Proton, interval 0, and not refused before.
#include "present_mode.h"
#include <cstdio>

static int g_fail;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++g_fail; } } while (0)

static void mode() {
    CHECK(presentMode(0, false) == kPresentOff);
    CHECK(presentMode(0, true) == kPresentOff);
    CHECK(presentMode(1, false) == kPresentNative);
    CHECK(presentMode(1, true) == kPresentD3D12);
    CHECK(presentMode(2, false) == kPresentD3D12);
    CHECK(presentMode(2, true) == kPresentD3D12);
    CHECK(presentMode(3, false) == kPresentNative);   // out of range: automatic
    CHECK(presentMode(3, true) == kPresentD3D12);
    CHECK(presentMode(-1, false) == kPresentNative);
    CHECK(presentMode(-1, true) == kPresentD3D12);
}

static void device() {
    CHECK(!nativeNeedsD3D12(false, false));            // DLSS only, or no upscaler files: no D3D12 device
    CHECK(nativeNeedsD3D12(true, false));              // FSR upscaling runs on it
    CHECK(nativeNeedsD3D12(false, true));              // XeSS upscaling too
    CHECK(nativeNeedsD3D12(true, true));
}

static void tearing() {
    // supported, wine, interval, refused
    CHECK(nativeTearing(true, false, 0, false));
    CHECK(!nativeTearing(false, false, 0, false));     // the system does not support it
    CHECK(!nativeTearing(true, true, 0, false));       // never under Wine/Proton
    CHECK(!nativeTearing(true, false, 1, false));      // the game asked for VSync
    CHECK(!nativeTearing(true, false, 2, false));
    CHECK(!nativeTearing(true, false, 0, true));       // a Present with it was refused this launch
    CHECK(!nativeTearing(false, true, 1, true));
    // creation and ResizeBuffers: the flag goes on whenever supported and not under Wine, whatever the interval
    CHECK(nativeTearingFlag(true, false));
    CHECK(!nativeTearingFlag(false, false));
    CHECK(!nativeTearingFlag(true, true));
}

int main() {
    mode(); device(); tearing();
    if (g_fail) { std::printf("present_mode_test: %d failed\n", g_fail); return 1; }
    std::printf("present_mode_test: all checks passed\n");
    return 0;
}
