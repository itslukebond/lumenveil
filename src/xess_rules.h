// Rules for Intel XeSS that need no GPU, so tests/xess_test.cpp covers them on the host.
#pragma once

// XeSS frame generation takes an 8-bit SDR or a 10-bit HDR10 back buffer; not 16-bit float (scRGB). dxgiFormat is a
// DXGI_FORMAT value (xess.cpp checks these numbers against the enum at compile time).
inline bool xessFgFormatOk(unsigned dxgiFormat) {
    switch (dxgiFormat) {
    case 24:  // R10G10B10A2_UNORM
    case 28:  // R8G8B8A8_UNORM
    case 29:  // R8G8B8A8_UNORM_SRGB
    case 87:  // B8G8R8A8_UNORM
    case 91:  // B8G8R8A8_UNORM_SRGB
        return true;
    default:
        return false;
    }
}

// Whether Intel's frame generation swap chain should present now: XeSS-FG is this launch's backend (chosen) and the
// format the game presents in now is one it takes. Asked at launch and whenever the game recreates or resizes its swap chain.
inline bool xessFgWanted(bool chosen, unsigned dxgiFormat) { return chosen && xessFgFormatOk(dxgiFormat); }

// One XeSS preset's accepted input size for the output size, from xessGetOptimalInputResolution.
struct XessRange { int preset; unsigned minW, minH, maxW, maxH; };

// The preset for the game's render size: native anti-aliasing when it renders at the output size, else the
// highest-quality preset (ranges are listed highest quality first) whose range holds the input size. If none does,
// the last (lowest-quality) one, with *fits false so the caller can say so.
inline int xessPickPreset(unsigned inW, unsigned inH, unsigned outW, unsigned outH, const XessRange* r, int n, int nativeAa,
                          bool* fits) {
    *fits = true;
    if (inW == outW && inH == outH) return nativeAa;
    for (int i = 0; i < n; ++i)
        if (inW >= r[i].minW && inH >= r[i].minH && inW <= r[i].maxW && inH <= r[i].maxH) return r[i].preset;
    *fits = false;
    return n > 0 ? r[n - 1].preset : nativeAa;
}

// The menu's Ultra Performance scaling mode, as the percent written to the game's RenderingResolutionRatio. XeSS's
// Ultra Performance takes at most 3.0x, and the game's 33% (rounded down, as the game renders) is below that at every
// output size; the game renders only at the scale values on its own list (34 renders at 100%), and none of them lies
// between 34% and 50%. So with XeSS, Ultra Performance is not offered (0) and Performance (50%) is the lowest mode.
constexpr int kXessUltraPerformancePct = 0;
inline int ultraPerformancePct(bool xess) { return xess ? kXessUltraPerformancePct : 33; }
// The lowest scaling mode the menu offers with XeSS.
inline int xessLowestPct() { return kXessUltraPerformancePct ? kXessUltraPerformancePct : 50; }
