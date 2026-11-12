// The first load after the title screen, where the loading screen is already drawn through the upscaler and runs
// straight into the world with no gap: frame generation must stay off for the whole loading screen and be on in the
// world. Timings: about 20 s of title, one long frame as the load starts, 15 s of loading screen, then the world.
// Frame generation is modelled as in gate_test.cpp: on after 8 upscaled frames in a row while the gate lets it start,
// off after 8 frames without the upscaler.
// Build and run: g++ -std=c++17 -O2 -Wall -Wextra -Werror -I src -o gate_first_load_test tests/gate_first_load_test.cpp
#include "fg_gate.h"
#include <cstdio>

static int g_fails;
#define CHECK(cond, ...) do { if (!(cond)) { std::printf("FAIL %s:%d: ", __FILE__, __LINE__); std::printf(__VA_ARGS__); std::printf("\n"); ++g_fails; } } while (0)

struct Sim {
    FgGate gate;
    double t = 1000;
    int validRun = 0, invalidRun = 0;
    bool fgOn = false;
    double onAt = -1;
    bool run(bool tagged, int n, double ms) {
        bool onInRun = false;
        for (int i = 0; i < n; ++i) {
            t += ms;
            const bool may = gate.frame(tagged, (unsigned long long)t);
            validRun = tagged ? validRun + 1 : 0;
            invalidRun = tagged ? 0 : invalidRun + 1;
            if (!fgOn && validRun >= 8 && may) { fgOn = true; onAt = t; }
            else if (fgOn && invalidRun >= 8) fgOn = false;
            onInRun |= fgOn;
        }
        return onInRun;
    }
};

int main() {
    Sim m;
    m.run(false, 42, 34);              // launch, no upscaler
    m.run(true, 250, 8);               // start-up scene through the upscaler
    m.run(false, 20000 / 4.5, 4.5);    // title screen and save list
    m.run(false, 1, 1140);             // the load starts with one long frame
    const double loadStart = m.t;
    const bool onWhileLoading = m.run(true, 15000 / 8, 8);   // loading screen drawn through the upscaler, 15 s
    CHECK(!onWhileLoading, "frame generation came on %.1f s into the first loading screen", (m.onAt - loadStart) / 1000);
    m.run(true, 30000 / 8, 8);         // the world, same unbroken stretch
    CHECK(m.fgOn, "frame generation not on after 30 s in the world");
    if (g_fails) { std::printf("gate_first_load_test: %d failure(s)\n", g_fails); return 1; }
    std::printf("gate_first_load_test: PASS\n");
    return 0;
}
