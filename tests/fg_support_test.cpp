// Host test for src/fg_support.h: the reason the log gives when NVIDIA's Streamline says DLSS frame generation cannot run
// on this PC, and when a 500-frame window shows that frame generation added no frames. Expected values are written out
// from Streamline's sl_result.h, where Result counts up from eOk = 0: eErrorIO 1, eErrorDriverOutOfDate 2,
// eErrorOSOutOfDate 3, eErrorOSDisabledHWS 4, eErrorDeviceNotCreated 5, eErrorNoSupportedAdapterFound 6,
// eErrorAdapterNotSupported 7.
#include "fg_support.h"
#include <cstdio>
#include <cstring>

static int g_fail;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++g_fail; } } while (0)
static bool same(const char* a, const char* b) { return a && b && std::strcmp(a, b) == 0; }

static void reasons() {
    CHECK(fg_support::reason(0) == nullptr);  // supported: no reason
    CHECK(same(fg_support::reason(4), "Hardware-accelerated GPU scheduling is off in Windows"));
    CHECK(same(fg_support::reason(3), "Windows is too old"));
    CHECK(same(fg_support::reason(2), "the NVIDIA driver is too old"));
    CHECK(same(fg_support::reason(6), "no graphics card that supports it was found"));
    CHECK(same(fg_support::reason(7), "this graphics card does not support it"));
    CHECK(fg_support::reason(1) == nullptr);   // other results: the log gives the number alone
    CHECK(fg_support::reason(5) == nullptr);
    CHECK(fg_support::reason(-1) == nullptr);
    CHECK(fg_support::reason(99) == nullptr);
}

static void noneGenerated() {
    // frames with frame generation on, presented, rendered
    CHECK(fg_support::noneGenerated(500, 500, 500));     // on all window, every rendered frame shown once: none generated
    CHECK(!fg_support::noneGenerated(500, 1000, 500));   // 2x
    CHECK(!fg_support::noneGenerated(500, 501, 500));    // one generated frame is enough
    CHECK(!fg_support::noneGenerated(0, 500, 500));      // off: nothing to report
    CHECK(!fg_support::noneGenerated(499, 500, 500));    // turned on during the window: it may not have started yet
    CHECK(!fg_support::noneGenerated(0, 0, 0));          // nothing counted yet
}

int main() {
    reasons();
    noneGenerated();
    if (g_fail) { std::printf("%d failed\n", g_fail); return 1; }
    std::printf("fg_support_test passed\n");
    return 0;
}
