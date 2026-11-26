#!/bin/bash
# Checks the order of the Frame generation row's reasons when more than one applies: Streamline's unsupported result
# first, then the scRGB no-frames notice, then the version line; Wine keeps the unsupported line out, so the scRGB
# notice shows there. Host test of src/menu_pages.h.
# Usage: bash tests/check_menu_fg_precedence.sh [source folder]   (default: src next to tests)
here=$(cd "$(dirname "$0")" && pwd)
src=$(cd "${1:-$here/../src}" && pwd) || exit 1
T=$(mktemp -d) || exit 1
trap 'rm -rf "$T"' EXIT

cat > "$T/t.cpp" <<'CPP'
#include "menu_pages.h"
#include <cstdio>
#include <cstring>
static int fail;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++fail; } } while (0)
using namespace pages;
static const char* const kDriver = "The NVIDIA driver is too old for DLSS frame generation. Update the driver, then restart the game.";
static const char* const kScrgb =
    "Adding no frames: DLSS frame generation does not support the game's 16-bit scRGB HDR output. Turn HDR off in the game to use it.";
static const char* const kVersion = "NVIDIA DLSS frame generation 310.9.16.";
static bool has(const Help& h, const char* t, Tone tone) {
    for (int i = 0; i < h.n; ++i) if (std::strcmp(h.line[i].text, t) == 0 && h.line[i].tone == tone) return true;
    return false;
}
static bool mentions(const Help& h, const char* t) {
    for (int i = 0; i < h.n; ++i) if (std::strstr(h.line[i].text, t)) return true;
    return false;
}
// DLSS frame generation running on Windows, Streamline's result `support`, the window verdict `noFrames`
static State dlss(int support, int noFrames) {
    State s;
    s.nvidia = true; s.want = 0; s.active = 0; s.upscaler = -1;
    s.backend = 0; s.launchBackend = -1; s.picked = 0; s.running = 0; s.fgBackend = 0; s.fgAvailable = true; s.fgEnable = 1;
    s.launchEnable = true; s.dlssgVersion = "310.9.16"; s.generating = true; s.shown = 60; s.rendered = 60;
    s.fgSupport = support; s.noFrames = noFrames; s.wine = false;
    return s;
}
int main() {
    Help h = help(kBackend, dlss(2, 2));  // both: the unsupported reason, alone
    CHECK(has(h, kDriver, kAmber)); CHECK(!mentions(h, "scRGB")); CHECK(!mentions(h, kVersion));
    h = help(kBackend, dlss(0, 2));       // supported, scRGB: the notice
    CHECK(has(h, kScrgb, kAmber)); CHECK(!mentions(h, kVersion));
    h = help(kBackend, dlss(2, 0));       // unsupported, frames generated or not yet judged: the reason
    CHECK(has(h, kDriver, kAmber));
    State w = dlss(2, 2); w.wine = true;  // Proton: no unsupported line, so the scRGB notice
    h = help(kBackend, w);
    CHECK(has(h, kScrgb, kAmber)); CHECK(!mentions(h, "driver"));
    h = help(kBackend, dlss(0, 0));       // neither: the version
    CHECK(has(h, kVersion, kDim));
    if (fail) { std::printf("%d failed\n", fail); return 1; }
    std::printf("frame generation precedence test passed\n");
    return 0;
}
CPP
g++ -std=c++17 -O1 -Wall -Wextra -Werror -I "$src" -o "$T/t" "$T/t.cpp" || { echo "FAIL: precedence test did not build"; exit 1; }
"$T/t" || { echo "FAIL: precedence test"; exit 1; }
echo "PASS: menu fg precedence"
