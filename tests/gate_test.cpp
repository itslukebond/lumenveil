// Host test for src/fg_gate.h, the start-up gate for frame generation. Built and run by tests/check_gate.sh.
// The frame sequences come from the timings in the mod's logs: frames without DLSS at launch, a short start-up scene
// through DLSS, the title and the first load without DLSS, then the world through DLSS again.
// FG itself is modelled with the mod's rule: on after 8 DLSS frames in a row while the gate lets it start,
// off after 8 frames in a row without DLSS.
#include "fg_gate.h"
#include "fg_select.h"
#include <cstdio>
#include <cstdlib>
#include <random>
#include <vector>

static int g_fails;
#define CHECK(cond, ...) do { if (!(cond)) { std::printf("FAIL %s:%d: ", __FILE__, __LINE__); std::printf(__VA_ARGS__); std::printf("\n"); ++g_fails; } } while (0)

struct Sim {
    FgGate gate;
    double t = 1000;          // ms since launch
    int validRun = 0, invalidRun = 0;
    bool fgOn = false, gateSaid = false, fgEverOn = false;
    // n frames, each ms long; tagged = DLSS ran on it. Returns true if FG was on at any point in the run.
    bool run(bool tagged, int n, double ms) {
        bool onInRun = false;
        for (int i = 0; i < n; ++i) {
            t += ms;
            gateSaid = gate.frame(tagged, (unsigned long long)t);
            validRun = tagged ? validRun + 1 : 0;
            invalidRun = tagged ? 0 : invalidRun + 1;
            if (!fgOn && validRun >= 8 && gateSaid) fgOn = true;
            else if (fgOn && invalidRun >= 8) fgOn = false;
            onInRun |= fgOn;
        }
        fgEverOn |= onInRun;
        return onInRun;
    }
};

// Start-up timings seen in the logs.
struct Startup { const char* name; int frames; double ms; double firstMs; };
static const Startup kStartups[] = {
    { "typical (250 frames at 8 ms)", 250, 8, 8 },
    { "long (400 frames at 12 ms)", 400, 12, 12 },
    { "shader cache cold (one 10 s frame, then 50 at 120 ms)", 50, 120, 10000 },
};

static void launchToWorld(const Startup& s) {
    Sim m;
    CHECK(!m.run(false, 42, 34), "%s: FG on before any DLSS frame", s.name);
    bool on = m.run(true, 1, s.firstMs);
    on |= m.run(true, s.frames - 1, s.ms);
    CHECK(!on, "%s: FG came on during the start-up scene", s.name);
    CHECK(!m.gate.open, "%s: gate open during the start-up scene", s.name);
    CHECK(!m.run(false, 30000 / 4.5, 4.5), "%s: FG on during the title and the first load", s.name);
    CHECK(m.gate.open && m.gate.how == FgGate::kTitleGap, "%s: gate not open by title gap after the title (how %d)", s.name, m.gate.how);
    // The first DLSS frame after the title starts the 20 s count: 2499 frames at 8 ms are 19.984 s of it.
    CHECK(!m.run(true, 2499, 8), "%s: FG on before 20 s of DLSS frames after the title", s.name);
    CHECK(m.run(true, 8, 8), "%s: FG not on once 20 s of DLSS frames had passed after the title", s.name);
}

static void titleGapOpensAfterTwoSeconds() {
    Sim m;
    m.run(false, 42, 34);
    m.run(true, 250, 8);
    const double end = m.t;                    // last DLSS frame
    m.run(false, 1990 / 5, 5);                 // 1.99 s without DLSS
    CHECK(!m.gate.open, "gate open %.0f ms after the start-up scene, before 2 s", m.t - end);
    m.run(false, 3, 5);                        // past 2 s
    CHECK(m.gate.open && m.gate.how == FgGate::kTitleGap, "gate not open 2 s after the start-up scene");
}

static void staysOpenInTheWorld() {
    Sim m;
    m.run(false, 42, 34); m.run(true, 250, 8); m.run(false, 30000 / 4.5, 4.5); m.run(true, 30000 / 8, 8);
    CHECK(m.fgOn, "FG not on after 30 s in the world");
    m.run(false, 15000 / 4.5, 4.5);            // fast travel load
    CHECK(!m.fgOn, "FG still on during a 15 s load");
    CHECK(m.gate.open, "gate closed again by a load in the world");
    m.run(true, 7, 8);
    CHECK(!m.fgOn, "FG on before 8 frames after the load");
    m.run(true, 1, 8);
    CHECK(m.fgOn, "FG not back at the 8th frame after the load");
}

static void longStretchFallback() {
    Sim m;
    m.run(false, 42, 34);
    const double start = m.t + 10;
    m.run(true, 1, 10);                        // first DLSS frame at `start`
    m.run(true, (29900 - 10) / 10, 10);        // to 29.9 s into the stretch
    CHECK(!m.gate.open, "gate open %.0f ms into one stretch, before 30 s", m.t - start);
    CHECK(!m.fgEverOn, "FG on before the 30 s fallback");
    m.run(true, 11, 10);                       // 29.89 s + 0.11 s: exactly 30 s into the stretch
    CHECK(m.gate.open && m.gate.how == FgGate::kLongStretch, "gate not open after 30 s of one stretch");
    CHECK(m.fgOn, "FG not on once the 30 s fallback opened the gate");
}

static void shortBreaksDoNotCount() {
    {   // 5-frame breaks inside the start-up scene
        Sim m;
        m.run(false, 42, 34);
        for (int i = 0; i < 10; ++i) { m.run(true, 25, 8); m.run(false, 5, 8); }
        m.run(false, 7, 8);                    // the scene goes on for 2 s more without ending (no 8-frame gap)
        m.run(true, 250, 8);
        CHECK(!m.gate.open, "5-frame breaks inside the start-up scene opened the gate");
        CHECK(!m.fgEverOn, "FG on during a start-up scene with 5-frame breaks");
    }
    {   // a 1 s gap inside the start-up scene
        Sim m;
        m.run(false, 42, 34);
        m.run(true, 120, 8);
        m.run(false, 1000 / 8, 8);
        m.run(true, 120, 8);
        CHECK(!m.gate.open, "a 1 s gap inside the start-up scene opened the gate");
        CHECK(!m.fgEverOn, "FG on after a 1 s gap inside the start-up scene");
    }
    {   // a 4-frame stretch is not a start-up scene
        Sim m;
        m.run(false, 42, 34);
        m.run(true, 4, 8);
        m.run(false, 3000 / 5, 5);
        CHECK(!m.gate.open, "a 4-frame DLSS stretch then a 3 s gap opened the gate");
    }
}

// Stress: random sequences against the rule written as a check over the whole history.
struct Frame { bool tagged; unsigned long long t; };

// What the gate should say after frame j, worked out from the history up to j, written out from the rule.
// The rule's numbers, written here as literals so a change to the gate's constants cannot pass unnoticed.
static const long kRun = 8;
static const unsigned long long kGapMs = 2000, kLongMs = 30000;

// Title gap judged on frames 0..e at time `now`: the last stretch had ended (8 frames without DLSS after its last
// DLSS frame), some stretch of 8 or more DLSS frames had ended by then, and `now` is 2 s or more after that last
// DLSS frame. `now` may be the time of a later frame, which may itself be a DLSS frame (a load that showed nothing).
static bool titleGapOver(const std::vector<Frame>& h, long e, unsigned long long now) {
    long last = -1;
    for (long k = e; k >= 0; --k) if (h[k].tagged) { last = k; break; }
    if (last < 0 || e - last < kRun) return false;
    bool saw = false; long count = 0, gap = 0;
    for (long q = 0; q <= e; ++q) {
        if (h[q].tagged) { ++count; gap = 0; }
        else if (++gap == kRun) { saw |= count >= kRun; count = 0; }
    }
    return saw && now - h[last].t >= kGapMs;
}

static FgGate::Opened expected(const std::vector<Frame>& h, size_t j) {
    const unsigned long long now = h[j].t;
    // The gap before frame j counts on frame j, whatever frame j is; or, with frame j added, the gap is complete.
    if (titleGapOver(h, (long)j - 1, now) || titleGapOver(h, (long)j, now)) return FgGate::kTitleGap;
    long last = -1;                                        // last DLSS frame at or before j
    for (long k = (long)j; k >= 0; --k) if (h[k].tagged) { last = k; break; }
    if (last < 0 || (long)j - last >= kRun) return FgGate::kNo;
    long start = last, gap = 0;                            // inside a stretch: find where it began
    for (long k = last; k >= 0; --k) {
        if (h[k].tagged) { start = k; gap = 0; }
        else if (++gap >= kRun) break;                     // 8 frames without DLSS end the stretch before it
    }
    return now - h[start].t >= kLongMs ? FgGate::kLongStretch : FgGate::kNo;
}

// The first-load rule: frame generation may start once the gate opened by the 30 s stretch, or,
// after it opened by title gap, once the DLSS frames after that frame add up to 20 s. Each DLSS frame adds the time
// since the DLSS frame before it, when fewer than 8 frames without DLSS lie between the two (the same stretch).
static const unsigned long long kFirstLoadMs = 20000;
struct Ready {
    long openedAt = -1; FgGate::Opened how = FgGate::kNo;
    unsigned long long sum = 0; bool ready = false;
    bool after(const std::vector<Frame>& h, long j, FgGate::Opened open) {
        if (openedAt < 0 && open != FgGate::kNo) { openedAt = j; how = open; if (how == FgGate::kLongStretch) ready = true; }
        if (ready || openedAt < 0 || j <= openedAt || !h[j].tagged) return ready;
        long p = j - 1;
        while (p >= 0 && !h[p].tagged) --p;
        if (p >= 0 && (j - 1 - p) < kRun) sum += h[j].t - h[p].t;
        if (sum >= kFirstLoadMs) ready = true;
        return ready;
    }
};

static void stress() {
    std::mt19937 rng(20261111);
    int opened = 0, readied = 0;
    for (int seq = 0; seq < 10000; ++seq) {
        std::vector<Frame> h;
        unsigned long long t = 1000;
        FgGate g;
        FgGate::Opened first = FgGate::kNo;
        Ready r;
        const int segments = 2 + rng() % 6;
        bool bad = false;
        for (int s = 0; s < segments && !bad; ++s) {
            const bool tagged = s % 2;          // starts without DLSS, then alternates
            const int n = 1 + rng() % (rng() % 4 ? 40 : 600);
            for (int i = 0; i < n && !bad; ++i) {
                t += rng() % 20 == 0 ? 500 + rng() % 4000 : 2 + rng() % 199;
                h.push_back({ tagged, t });
                const bool said = g.frame(tagged, t);
                const FgGate::Opened want = first != FgGate::kNo ? first : expected(h, h.size() - 1);
                if (first == FgGate::kNo) first = want;
                const bool may = r.after(h, (long)h.size() - 1, want);
                if (g.open != (want != FgGate::kNo) || (g.open && g.how != want) || said != may) {
                    std::printf("FAIL stress seq %d frame %zu: gate %d (how %d) may start %d, expected %d may start %d\n", seq, h.size() - 1, g.open,
                                g.how, said, want, may);
                    ++g_fails; bad = true;
                }
            }
        }
        opened += first != FgGate::kNo;
        readied += r.ready;
    }
    std::printf("stress: 10000 sequences, %d opened the gate, %d let frame generation start\n", opened, readied);
    CHECK(opened > 1000 && opened < 9000, "stress sequences do not cover both outcomes (%d opened)", opened);
    CHECK(readied > 500 && readied < opened, "stress sequences do not cover both first-load outcomes (%d of %d)", readied, opened);
}

// A load that stalls presentation across the 2 s mark, so the next frame shown is already the world.
static void stalledLoadStillOpensByTitleGap() {
    FgGate g;
    unsigned long long t = 0;
    for (int i = 0; i < 100; ++i) g.frame(true, t += 10);          // start-up scene, last DLSS frame at 1000
    for (int i = 0; i < 15; ++i) g.frame(false, t += 100);         // title frames to 2500: stretch over, gap 1.5 s
    CHECK(!g.open, "gate open 1.5 s after the start-up scene");
    g.frame(true, 4500);                                           // nothing shown for 2 s, then the world
    CHECK(g.open && g.how == FgGate::kTitleGap, "first world frame after a stalled load: gate %d how %d, expected title gap", g.open, g.how);
    Sim m;                                                         // and FG then follows the 8-frame rule
    m.run(false, 42, 34); m.run(true, 250, 8); m.run(false, 15, 100); m.t += 2000;
    CHECK(!m.run(true, 2499, 8), "FG on before 20 s of world frames after a stalled load");
    CHECK(m.run(true, 8, 8), "FG not on after 20 s of world frames after a stalled load");
}

// The 8-frame boundaries, with literal counts, not the gate's own constant.
static void eightFrameBoundaries() {
    {   // a stretch ends on the 8th frame without DLSS, not the 7th, even when the 7 span well over 2 s
        FgGate g;
        unsigned long long t = 0;
        for (int i = 0; i < 20; ++i) g.frame(true, t += 10);
        for (int i = 0; i < 7; ++i) g.frame(false, t += 500);       // 3.5 s, 7 frames
        CHECK(!g.open, "gate open after 7 frames without DLSS");
        g.frame(false, t += 500);                                   // 8th
        CHECK(g.open && g.how == FgGate::kTitleGap, "gate not open on the 8th frame without DLSS after a 4 s gap");
    }
    {   // 7 DLSS frames are not a start-up scene; 8 are
        for (int n = 7; n <= 8; ++n) {
            FgGate g;
            unsigned long long t = 0;
            for (int i = 0; i < n; ++i) g.frame(true, t += 10);
            for (int i = 0; i < 40; ++i) g.frame(false, t += 100);  // 4 s without DLSS
            CHECK(g.open == (n == 8), "%d DLSS frames then 4 s without: gate %d, expected %d", n, g.open, n == 8);
        }
    }
}

// After the title, frame generation also waits for 20 s of DLSS frames: the first load's loading screen can already be
// drawn through DLSS and run straight into the world (the first load is modelled in tests/gate_first_load_test.cpp).
static void Sim_toTitle(Sim& m) { m.run(false, 42, 34); m.run(true, 250, 8); m.run(false, 20000 / 4.5, 4.5); }

static void firstLoadWaitIsPaidOnce() {
    Sim m;
    Sim_toTitle(m);
    m.run(true, 30000 / 8, 8);                    // the first load and the world
    CHECK(m.fgOn, "FG not on after 30 s of DLSS frames after the title");
    m.run(false, 15000 / 4.5, 4.5);               // a later load without DLSS
    CHECK(!m.fgOn, "FG still on during a 15 s load without DLSS");
    CHECK(!m.run(true, 7, 8), "FG on before 8 DLSS frames after the later load");
    CHECK(m.run(true, 1, 8), "FG not back at the 8th DLSS frame after the later load: the 20 s wait came back");
}

static void firstLoadPausesAddUp() {            // 8 s stretches, 3 s apart
    Sim m;
    Sim_toTitle(m);
    CHECK(!m.run(true, 1000, 8), "FG on in the first 8 s stretch after the title");
    m.run(false, 3000 / 4.5, 4.5);
    CHECK(!m.run(true, 1000, 8), "FG on in the second 8 s stretch after the title");
    m.run(false, 3000 / 4.5, 4.5);
    CHECK(m.run(true, 1000, 8), "FG not on in the third 8 s stretch: a pause restarted the 20 s wait");
}

static void firstLoadBoundary() {               // off at 19.99 s of DLSS time, on at 20.00 s
    FgGate g;
    unsigned long long t = 0;
    for (int i = 0; i < 100; ++i) g.frame(true, t += 10);        // start-up scene
    for (int i = 0; i < 40; ++i) g.frame(false, t += 100);       // 4 s of title: open by title gap
    CHECK(g.open && g.how == FgGate::kTitleGap && !g.ready, "gate not open by title gap, or ready at once, after the title");
    g.frame(true, t += 10);                                      // first DLSS frame after the title: no time yet
    for (int i = 0; i < 1999; ++i) g.frame(true, t += 10);       // 19.99 s
    CHECK(!g.frame(true, t += 9), "FG may start at 19.999 s of DLSS frames after the title");
    CHECK(g.frame(true, t += 1), "FG may not start at 20.000 s of DLSS frames after the title");
}

// Frame generation backend chosen at launch (src/fg_select.h): -1 none, 0 DLSS-G, 1 FSR FG, 2 XeSS-FG. The expected
// values are the rules written out case by case: a backend picked by hand is used as is; Auto is DLSS-G on an
// NVIDIA GPU with nvngx_dlssg.dll, XeSS-FG on an Intel GPU with the three XeSS files, else FSR FG. Every combination.
static void backendSelection() {
    const unsigned nv = 0x10DE, amd = 0x1002, intel = 0x8086;
    struct Row { unsigned vendor; bool dlssg, xess; int autoPick; };
    static const Row rows[] = {
        { nv, false, false, 1 }, { nv, false, true, 1 }, { nv, true, false, 0 }, { nv, true, true, 0 },
        { amd, false, false, 1 }, { amd, false, true, 1 }, { amd, true, false, 1 }, { amd, true, true, 1 },
        { intel, false, false, 1 }, { intel, false, true, 2 }, { intel, true, false, 1 }, { intel, true, true, 2 },
    };
    int n = 0;
    for (const Row& r : rows) {
        for (int setting : { -1, 3, -7 }) {  // Auto, and values out of range, which count as Auto
            const int got = fgPickBackend(setting, r.vendor, r.dlssg, r.xess);
            CHECK(got == r.autoPick, "setting %d vendor %04x dlssg %d xess %d: backend %d, expected %d", setting, r.vendor, r.dlssg, r.xess, got, r.autoPick);
            ++n;
        }
        for (int setting : { 0, 1, 2 }) {  // picked by hand: used as is, whatever the GPU and files
            const int got = fgPickBackend(setting, r.vendor, r.dlssg, r.xess);
            CHECK(got == setting, "setting %d vendor %04x dlssg %d xess %d: backend %d, expected %d", setting, r.vendor, r.dlssg, r.xess, got, setting);
            ++n;
        }
    }
    CHECK(n == 72, "backend selection: %d cases run, expected 72", n);
}

int main() {
    backendSelection();
    stalledLoadStillOpensByTitleGap();
    eightFrameBoundaries();
    for (const Startup& s : kStartups) launchToWorld(s);
    titleGapOpensAfterTwoSeconds();
    staysOpenInTheWorld();
    longStretchFallback();
    shortBreaksDoNotCount();
    firstLoadWaitIsPaidOnce();
    firstLoadPausesAddUp();
    firstLoadBoundary();
    stress();
    if (g_fails) { std::printf("gate_test: %d failure(s)\n", g_fails); return 1; }
    std::printf("gate_test: PASS\n");
    return 0;
}
