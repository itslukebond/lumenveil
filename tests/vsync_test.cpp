// Host test for src/vsync_rules.h: the frame cap our presenter holds when the game asks for VSync, and when the stall
// watchdog logs. Expected values are written out here (with VSync on the cap is the
// menu's custom cap if lower, else the auto cap, never uncapped; with VSync off it is as in 1.0.1: Auto = the auto
// cap, Off = 0, Custom = itself; a stall is 3 s with no new present, logged once, then once when presents resume).
// Menu cap values: -1 Auto, 0 Off, above 0 a custom fps.
#include "vsync_rules.h"
#include <cstdio>

static int g_fail;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++g_fail; } } while (0)

static void cap() {
    // VSync off: unchanged from 1.0.1
    CHECK(vsyncCap(-1, 139, false) == 139);
    CHECK(vsyncCap(0, 139, false) == 0);
    CHECK(vsyncCap(90, 139, false) == 90);
    CHECK(vsyncCap(200, 139, false) == 200);
    // VSync on: never uncapped, never above the auto cap
    CHECK(vsyncCap(-1, 139, true) == 139);
    CHECK(vsyncCap(0, 139, true) == 139);
    CHECK(vsyncCap(90, 139, true) == 90);
    CHECK(vsyncCap(139, 139, true) == 139);
    CHECK(vsyncCap(200, 139, true) == 139);
    CHECK(vsyncCap(-1, 59, true) == 59);   // 60 Hz
    CHECK(vsyncCap(30, 59, true) == 30);
}

static void route() {
    // The interval our swap chain presents at: Windows on our own or Streamline's
    // swap chain always 0; under Wine/Proton, or on FSR's or XeSS's frame generation swap chain, the game's own.
    CHECK(vsyncInterval(1, false, false) == 0);
    CHECK(vsyncInterval(2, false, false) == 0);
    CHECK(vsyncInterval(0, false, false) == 0);
    CHECK(vsyncInterval(1, true, false) == 1);
    CHECK(vsyncInterval(1, false, true) == 1);
    CHECK(vsyncInterval(0, true, true) == 0);
    // the cap stands in for VSync only where our Present dropped it
    CHECK(vsyncByCap(1, false, false));
    CHECK(!vsyncByCap(0, false, false));
    CHECK(!vsyncByCap(1, true, false));
    CHECK(!vsyncByCap(1, false, true));
    // our own pacing: never with Streamline (Reflex is in charge); FSR/XeSS frame generation or upscaling (1 FSR,
    // 2 XeSS) as in 1.0.1; and whenever the cap stands in for VSync. Upscaler 0 is DLSS, -1 none.
    CHECK(!ownPace(true, false, -1, true));
    CHECK(!ownPace(true, false, -1, false));
    CHECK(ownPace(false, true, 0, false));
    CHECK(ownPace(false, false, 1, false));
    CHECK(ownPace(false, false, 2, false));
    CHECK(!ownPace(false, false, 0, false));
    CHECK(!ownPace(false, false, -1, false));
    CHECK(ownPace(false, false, 0, true));
    CHECK(ownPace(false, false, -1, true));
}

static void watch() {
    StallWatch w;
    // nothing presented yet: never a stall, however long
    CHECK(w.step(0, 0) == kStallNone);
    CHECK(w.step(0, 60000) == kStallNone);
    // presenting every 16 ms: quiet
    unsigned n = 0; unsigned long long t = 60000;
    for (int i = 0; i < 100; ++i) CHECK(w.step(++n, t += 16) == kStallNone);
    // stops presenting: quiet until 3 s have passed, then one stall, then quiet while it lasts
    const unsigned long long stop = t;
    CHECK(w.step(n, stop + 500) == kStallNone);
    CHECK(w.step(n, stop + 2999) == kStallNone);
    CHECK(w.step(n, stop + 3000) == kStallBegan);
    CHECK(w.step(n, stop + 3500) == kStallNone);
    CHECK(w.step(n, stop + 60000) == kStallNone);
    // presents resume: one resume line carrying how long it lasted
    CHECK(w.step(n + 1, stop + 61000) == kStallEnded);
    CHECK(w.stalledMs == 61000);
    CHECK(w.step(n + 2, stop + 61016) == kStallNone);
    // a second stall is logged again
    CHECK(w.step(n + 2, stop + 64016) == kStallBegan);
    CHECK(w.step(n + 3, stop + 64100) == kStallEnded);
    CHECK(w.stalledMs == 3084);
    // a pause shorter than 3 s never logs, even when it ends
    CHECK(w.step(n + 3, stop + 66000) == kStallNone);
    CHECK(w.step(n + 4, stop + 66500) == kStallNone);
}

int main() {
    cap();
    route();
    watch();
    if (g_fail) { std::printf("%d failed\n", g_fail); return 1; }
    std::printf("vsync_test: all passed\n");
    return 0;
}
