// AMD Radeon Anti-Lag 2: whether it is available on this card, whether it runs this frame, and whether the menu shows its
// section. Pure, so tests/antilag_test.cpp checks it on the host; present12.cpp asks it, AMD's header does the calls.
#pragma once
#include <cstdint>

enum Al2State {
    kAl2NotStarted,  // the presenter has not started (a zeroed status)
    kAl2NotAmd,      // not an AMD card: never asked
    kAl2Wine,        // AMD under Wine/Proton: needs AMD's Windows driver, never asked
    kAl2NoDriver,    // AMD's D3D12 driver extension (amdxc64.dll) is not loaded
    kAl2Refused,     // the driver turned it down (card or driver too old)
    kAl2Ready,
};

// vendor: the adapter's PCI vendor; initHr: what Initialize returned (S_OK when it was not called)
inline Al2State al2Classify(unsigned vendor, bool wine, int32_t initHr) {
    if (vendor != 0x1002) return kAl2NotAmd;
    if (wine) return kAl2Wine;
    if (initHr == int32_t(0x80070006u)) return kAl2NoDriver;  // E_HANDLE: AMD's header found no amdxc64.dll
    return initHr == 0 ? kAl2Ready : kAl2Refused;
}

// On this frame: ready, switched on, and neither DLSS frame generation (Reflex's) nor Intel's swap chain (XeLL's) runs.
inline bool al2Run(Al2State s, int setting, bool streamline, bool intelSc) {
    return s == kAl2Ready && setting != 0 && !streamline && !intelSc;
}

// The menu's section: an AMD card, with FSR (1) or no frame generation (-1); DLSS (0) and XeSS (2) have their own.
inline bool al2MenuShown(int s, int backend) {
    return s > kAl2NotAmd && backend != 0 && backend != 2;
}
