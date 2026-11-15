// Host test for src/history_rules.h: when the upscaler's history is reset. The expected values are written out
// here, not taken from the code: a run is a reset run exactly when it is the first run of its
// frame and the frame before had no run (or it is the very first frame); the gap reported at that run is the number of
// run-less frames immediately before it. The harness uses the gate the way dlss.cpp does: the reset flag starts set,
// a run takes and clears it (reading the gap first), and each game Present ends the frame.
// Every sequence of 1 to 8 frames with 0, 1 or 2 runs per frame is checked.
#include "history_rules.h"
#include <cstdio>

static int g_fail;
static long g_resetAfterGap, g_noResetSecond, g_noResetConsecutive, g_firstFrame;

static void runSequence(const int* runs, int frames) {
    HistoryGate g;
    bool reset = true;
    int lessBefore = 0;  // run-less frames immediately before the current one
    for (int f = 0; f < frames; ++f) {
        for (int r = 0; r < runs[f]; ++r) {
            const unsigned gap = g.gap;
            const bool got = reset;
            reset = false;
            g.upscaled();
            const bool want = r == 0 && (f == 0 || runs[f - 1] == 0);
            if (got != want) {
                std::printf("FAIL: frame %d run %d of a %d-frame sequence: reset %d, want %d\n", f, r, frames, got, want);
                ++g_fail;
            }
            if (want && f > 0 && gap != unsigned(lessBefore)) {
                std::printf("FAIL: frame %d: gap %u, want %d\n", f, gap, lessBefore);
                ++g_fail;
            }
            if (want && f == 0) ++g_firstFrame;
            else if (want) ++g_resetAfterGap;
            else if (r > 0) ++g_noResetSecond;
            else ++g_noResetConsecutive;
        }
        lessBefore = runs[f] ? 0 : lessBefore + 1;
        if (g.endFrame()) reset = true;
    }
}

int main() {
    int runs[8];
    for (int frames = 1; frames <= 8; ++frames) {
        int total = 1;
        for (int i = 0; i < frames; ++i) total *= 3;
        for (int code = 0; code < total; ++code) {
            int c = code;
            for (int i = 0; i < frames; ++i) { runs[i] = c % 3; c /= 3; }
            runSequence(runs, frames);
        }
    }
    // each class of run must actually have been checked
    if (!g_resetAfterGap || !g_noResetSecond || !g_noResetConsecutive || !g_firstFrame) {
        std::printf("FAIL: a class of run was never checked (%ld %ld %ld %ld)\n", g_resetAfterGap, g_noResetSecond,
                    g_noResetConsecutive, g_firstFrame);
        ++g_fail;
    }
    if (g_fail) { std::printf("%d failed\n", g_fail); return 1; }
    std::printf("history_reset_test: all passed (%ld resets after a gap, %ld second runs, %ld consecutive frames)\n",
                g_resetAfterGap, g_noResetSecond, g_noResetConsecutive);
    return 0;
}
