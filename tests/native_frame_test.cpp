// Stress S1 for native_frame.h: nativeFrame driven through 100,000 randomised frames with counting fakes. The fake swap
// chain refuses a resize or recreation while any buffer or view reference is out, as DXGI does. Asserted after every
// frame: nothing held, restores equal saves, the game's state as before the frame, a rebuild exactly when the back
// buffer's size or format changed, nothing acquired on a frame not wanted. Each case must be reached a minimum number
// of times, so a frame that does nothing fails the run.
#include "native_frame.h"
#include <cstdio>
#include <cstdlib>
#include <random>

static int g_fail;
#define CHECK(c, ...) do { if (!(c)) { if (g_fail++ < 20) { std::printf("FAIL frame %d: ", frame); std::printf(__VA_ARGS__); std::printf("\n"); } } } while (0)
static int frame;

struct FakeSwapChain {
    int refs = 0;                 // buffer + view references the mod holds
    NativeTargetKey key = { 3840, 2160, 24 };
    bool resize(NativeTargetKey k) { if (refs) return false; key = k; return true; }
    bool recreate() { if (refs) return false; return true; }
};

// What the mod does to the game's D3D11 state: draw changes it, restore must put it back.
struct FakeOps {
    FakeSwapChain* sc;
    unsigned gameState = 0x1234, saved = 0;
    int saves = 0, restores = 0, rebuilds = 0, acquires = 0;
    bool failAcquire = false, failView = false, failRebuild = false, failDraw = false;
    bool holdingBuffer = false, holdingView = false, inSave = false;
    NativeTargetKey keyAtAcquire = {};

    bool acquire() {
        ++acquires;
        if (failAcquire) return false;
        holdingBuffer = true; ++sc->refs; keyAtAcquire = sc->key;
        return true;
    }
    NativeTargetKey key() const { return keyAtAcquire; }
    bool view() {
        if (failView) return false;
        holdingView = true; ++sc->refs;
        return true;
    }
    bool rebuild(NativeTargetKey) { ++rebuilds; return !failRebuild; }
    void save() { ++saves; saved = gameState; inSave = true; }
    bool draw() { gameState ^= 0xBEEF; return !failDraw; }  // the draw binds its own state, failing or not
    void restore() { ++restores; gameState = saved; inSave = false; }
    void releaseView() { if (holdingView) { holdingView = false; --sc->refs; } }
    void releaseBuffer() { if (holdingBuffer) { holdingBuffer = false; --sc->refs; } }
};

int main() {
    const unsigned seed = 20261115;
    std::printf("native_frame_test: seed %u\n", seed);
    std::mt19937 rng(seed);
    auto chance = [&](int pct) { return int(rng() % 100) < pct; };
    FakeSwapChain sc;
    FakeOps ops{ &sc };
    NativeTargetKey built = {};
    enum { kDrawn, kNotWanted, kAcquire, kView, kRebuild, kDraw, kResize, kFormat, kRecreate, kCases };
    const char* names[kCases] = { "drawn", "not wanted", "acquire failures", "view failures", "rebuild failures",
                                  "draw failures", "resizes", "format changes", "recreations" };
    const int minimum[kCases] = { 40000, 8000, 2000, 2000, 300, 2000, 800, 800, 800 };
    int count[kCases] = {};
    const NativeTargetKey sizes[] = { { 3840, 2160, 24 }, { 2560, 1440, 24 }, { 1920, 1080, 24 } };
    const unsigned formats[] = { 24, 10, 28, 87 };

    for (frame = 0; frame < 100000; ++frame) {
        // between frames the game may resize, change format or recreate: refused if the mod still holds anything
        if (chance(2)) {
            NativeTargetKey k = sizes[rng() % 3]; k.fmt = sc.key.fmt;
            const bool changed = !(k == sc.key);
            CHECK(sc.resize(k), "resize refused: %d references out", sc.refs);
            if (changed) ++count[kResize];
        }
        if (chance(2)) {
            NativeTargetKey k = sc.key; k.fmt = formats[rng() % 4];
            const bool changed = !(k == sc.key);
            CHECK(sc.resize(k), "format change refused: %d references out", sc.refs);
            if (changed) ++count[kFormat];
        }
        if (chance(2)) { CHECK(sc.recreate(), "recreation refused: %d references out", sc.refs); ++count[kRecreate]; }

        const bool wanted = !chance(10);
        ops.failAcquire = chance(3); ops.failView = chance(3); ops.failRebuild = chance(30); ops.failDraw = chance(3);
        const unsigned stateBefore = ops.gameState;
        const int acqBefore = ops.acquires, rebBefore = ops.rebuilds, savesBefore = ops.saves, restBefore = ops.restores;
        const NativeTargetKey builtBefore = built, keyNow = sc.key;

        const NativeFrameResult r = nativeFrame(ops, wanted, built);

        CHECK(sc.refs == 0, "%d references out after the frame (result %d)", sc.refs, int(r));
        CHECK(ops.saves - savesBefore == ops.restores - restBefore, "saves %d restores %d", ops.saves - savesBefore, ops.restores - restBefore);
        CHECK(ops.gameState == stateBefore, "game state %x, was %x (result %d)", ops.gameState, stateBefore, int(r));
        if (!wanted) {
            CHECK(ops.acquires == acqBefore, "acquired on a frame not wanted");
            CHECK(r == kNfNotWanted, "result %d on a frame not wanted", int(r));
            ++count[kNotWanted];
            continue;
        }
        const bool reached = r != kNfAcquireFailed && r != kNfViewFailed;
        const bool needRebuild = !(builtBefore == keyNow);
        CHECK(!reached || (ops.rebuilds - rebBefore) == (needRebuild ? 1 : 0),
              "rebuilds %d, size or format changed: %d", ops.rebuilds - rebBefore, int(needRebuild));
        if (r == kNfDrawn) CHECK(built == keyNow, "targets not recorded for the back buffer drawn into");
        if (r == kNfDrawn) CHECK(ops.saves - savesBefore == 1, "drawn without saving the game's state");
        switch (r) {
        case kNfDrawn: ++count[kDrawn]; break;
        case kNfAcquireFailed: ++count[kAcquire]; break;
        case kNfViewFailed: ++count[kView]; break;
        case kNfRebuildFailed: ++count[kRebuild]; break;
        case kNfDrawFailed: ++count[kDraw]; break;
        default: CHECK(false, "unexpected result %d", int(r));
        }
    }
    for (int i = 0; i < kCases; ++i) {
        std::printf("  %-17s %6d (minimum %d)\n", names[i], count[i], minimum[i]);
        if (count[i] < minimum[i]) { std::printf("FAIL: %s under its minimum\n", names[i]); ++g_fail; }
    }
    if (g_fail) { std::printf("native_frame_test: %d failures\n", g_fail); return 1; }
    std::printf("native_frame_test: all checks passed\n");
    return 0;
}
