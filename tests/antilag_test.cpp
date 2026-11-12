// Host test for src/antilag_rules.h: when AMD Radeon Anti-Lag 2 is available, when it runs, and when the menu shows its
// section. Expected values come from the spec's truth tables (docs: vendor 0x1002 is AMD, E_HANDLE means AMD's driver
// extension is not loaded, frame generation backends 0 DLSS-G, 1 FSR, 2 XeSS, -1 none), not from the code.
#include "antilag_rules.h"
#include <cstdio>
#include <initializer_list>

static int g_fail;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++g_fail; } } while (0)

static const int32_t kOk = 0, kHandle = int32_t(0x80070006u), kNoInterface = int32_t(0x80004002u),
                     kInvalidArg = int32_t(0x80070057u), kFail = int32_t(0x80004005u);

static void classify() {
    // not an AMD card: nothing else matters
    CHECK(al2Classify(0x10DE, false, kOk) == kAl2NotAmd);
    CHECK(al2Classify(0x8086, false, kOk) == kAl2NotAmd);
    CHECK(al2Classify(0x10DE, true, kHandle) == kAl2NotAmd);
    CHECK(al2Classify(0, false, kOk) == kAl2NotAmd);
    // AMD under Wine/Proton: never tried, whatever the result says
    CHECK(al2Classify(0x1002, true, kOk) == kAl2Wine);
    CHECK(al2Classify(0x1002, true, kHandle) == kAl2Wine);
    // AMD on Windows
    CHECK(al2Classify(0x1002, false, kOk) == kAl2Ready);
    CHECK(al2Classify(0x1002, false, kHandle) == kAl2NoDriver);
    CHECK(al2Classify(0x1002, false, kNoInterface) == kAl2Refused);
    CHECK(al2Classify(0x1002, false, kInvalidArg) == kAl2Refused);
    CHECK(al2Classify(0x1002, false, kFail) == kAl2Refused);
}

static void run() {
    // runs only when ready, switched on, and neither NVIDIA's (Streamline) nor Intel's frame generation swap chain is in
    CHECK(al2Run(kAl2Ready, 1, false, false));
    CHECK(!al2Run(kAl2Ready, 0, false, false));            // off by default
    CHECK(!al2Run(kAl2Ready, 1, true, false));             // DLSS frame generation: Reflex owns latency
    CHECK(!al2Run(kAl2Ready, 1, false, true));             // XeSS frame generation: Xe Low Latency owns it
    for (Al2State s : { kAl2NotStarted, kAl2NotAmd, kAl2Wine, kAl2NoDriver, kAl2Refused })
        CHECK(!al2Run(s, 1, false, false));
}

static void menuShown() {
    // AMD card (any state past "not AMD") with FSR or no frame generation: the section shows (greyed when not ready)
    for (Al2State s : { kAl2Wine, kAl2NoDriver, kAl2Refused, kAl2Ready }) {
        CHECK(al2MenuShown(s, 1));
        CHECK(al2MenuShown(s, -1));
        CHECK(!al2MenuShown(s, 0));                         // DLSS frame generation
        CHECK(!al2MenuShown(s, 2));                         // XeSS frame generation: its own Xe Low Latency section
    }
    for (int backend : { -1, 0, 1, 2 }) {
        CHECK(!al2MenuShown(kAl2NotAmd, backend));          // NVIDIA and Intel cards never see it
        CHECK(!al2MenuShown(kAl2NotStarted, backend));      // the presenter has not started
    }
}

int main() {
    CHECK(kAl2NotStarted == 0);  // a zeroed status reads as not started
    classify();
    run();
    menuShown();
    if (g_fail) { std::printf("%d checks failed\n", g_fail); return 1; }
    std::printf("antilag_test: all checks passed\n");
    return 0;
}
