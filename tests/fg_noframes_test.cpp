// Host test for src/fg_support.h: after a 500-frame window, whether frame generation added no frames and whether the
// game's 16-bit scRGB HDR output is the reason. The reason is named only for DLSS (backend 0) with scRGB output
// (encoding 2) and a window with frame generation on for all of it and one frame shown per rendered one. Backends:
// -1 none, 0 DLSS, 1 FSR, 2 XeSS. Encodings: 0 SDR, 1 HDR10, 2 scRGB. Results: 0 nothing to report, 1 no frames
// added (no cause named), 2 no frames added with scRGB output.
#include "fg_support.h"
#include <cstdio>
#include <cstring>

static int g_fail;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++g_fail; } } while (0)

struct Window { const char* name; unsigned on, presented, rendered; };
// on for every frame, each rendered frame shown once: the only window that reports
static const Window kOneToOne{ "whole window 1:1", 500, 500, 500 };
static const Window kOthers[] = {
    { "2x", 500, 1000, 500 },
    { "one generated frame", 500, 501, 500 },
    { "on for part of the window", 499, 500, 500 },
    { "off", 0, 500, 500 },
    { "empty", 0, 0, 0 },
};

static void matrix() {
    const int backends[] = { -1, 0, 1, 2 }, encodings[] = { 0, 1, 2 };
    // the whole 1:1 window, by backend (rows) and encoding (columns), from the spec
    static const int want[4][3] = {
        { 1, 1, 1 },  // none
        { 1, 1, 2 },  // DLSS: scRGB named
        { 1, 1, 1 },  // FSR
        { 1, 1, 1 },  // XeSS
    };
    for (int b = 0; b < 4; ++b)
        for (int e = 0; e < 3; ++e) {
            const int got = fg_support::noFrames(backends[b], encodings[e], kOneToOne.on, kOneToOne.presented, kOneToOne.rendered);
            if (got != want[b][e]) { std::printf("FAIL: %s, backend %d, encoding %d: %d, want %d\n", kOneToOne.name, backends[b], encodings[e], got, want[b][e]); ++g_fail; }
            for (const Window& w : kOthers) {
                const int other = fg_support::noFrames(backends[b], encodings[e], w.on, w.presented, w.rendered);
                if (other != 0) { std::printf("FAIL: %s, backend %d, encoding %d: %d, want 0\n", w.name, backends[b], encodings[e], other); ++g_fail; }
            }
        }
}

// The reporter's log (GitHub #1): DLSS, output scRGB, 500 frames presented per 500 rendered with frame generation on.
static void reporter() { CHECK(fg_support::noFrames(0, 2, 500, 500, 500) == 2); }

static void reason() {
    const char* r = fg_support::noFramesReason(2);
    CHECK(r && std::strcmp(r, "the game is using 16-bit scRGB HDR output, which DLSS frame generation does not support") == 0);
    CHECK(fg_support::noFramesReason(0) == nullptr);
    CHECK(fg_support::noFramesReason(1) == nullptr);
}

int main() {
    matrix();
    reporter();
    reason();
    if (g_fail) { std::printf("%d failed\n", g_fail); return 1; }
    std::printf("fg_noframes_test passed\n");
    return 0;
}
