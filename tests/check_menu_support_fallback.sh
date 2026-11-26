#!/bin/bash
# Checks the Frame generation row when Streamline reported DLSS frame generation unsupported and its start-up then
# stopped (frame generation not available, no backend running, DLSS still the one for this PC): the row gives the
# reported reason and what to do, in the signed-off words, not the generic card line; under Wine, or with nothing
# reported, the row is as before. Host test of src/menu_pages.h.
# Usage: tests/check_menu_support_fallback.sh [source folder] [Dear ImGui folder]
#        (defaults: src next to tests; third_party/imgui next to tests, or $IMGUI)
here=$(cd "$(dirname "$0")" && pwd)
src=$(cd "${1:-$here/../src}" && pwd) || exit 1
imgui=${2:-${IMGUI:-$here/../third_party/imgui}}
[ -f "$imgui/imgui.h" ] || { echo "FAIL: Dear ImGui not found at $imgui (pass its folder or set IMGUI)"; exit 1; }
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
static const char* const kHws =
    "Windows has Hardware-accelerated GPU scheduling off, and DLSS frame generation needs it. Turn it on in Settings, System, Display, Graphics, then restart the PC.";
static const char* const kGeneric = "Not available: DLSS frame generation needs an RTX 40 or 50 card.";
static bool has(const Help& h, const char* t, Tone tone) {
    for (int i = 0; i < h.n; ++i) if (std::strcmp(h.line[i].text, t) == 0 && h.line[i].tone == tone) return true;
    return false;
}
static bool mentions(const Help& h, const char* t) {
    for (int i = 0; i < h.n; ++i) if (std::strstr(h.line[i].text, t)) return true;
    return false;
}
// an RTX card on Windows whose DLSS frame generation start-up stopped after Streamline's support result `support`
static State stopped(int support) {
    State s;
    s.nvidia = true; s.want = 0; s.active = 0; s.upscaler = -1;
    s.backend = 0; s.launchBackend = -1; s.picked = 0; s.running = 0; s.fgBackend = -1; s.fgAvailable = false; s.fgEnable = 1;
    s.launchEnable = true; s.dlssg = true; s.fgSupport = support; s.wine = false;
    return s;
}
int main() {
    Help h = help(kBackend, stopped(2));
    CHECK(has(h, kDriver, kAmber)); CHECK(h.flag == kWarnFlag); CHECK(!mentions(h, kGeneric));
    h = help(kBackend, stopped(4));
    CHECK(has(h, kHws, kAmber)); CHECK(!mentions(h, kGeneric));
    for (int r : { 0, 99 }) {  // nothing reported, or no words for it: the row as before
        h = help(kBackend, stopped(r));
        CHECK(has(h, kGeneric, kDim)); CHECK(h.flag != kWarnFlag);
    }
    State w = stopped(2); w.wine = true;  // Proton: the log only
    h = help(kBackend, w);
    CHECK(has(h, kGeneric, kDim)); CHECK(!mentions(h, "driver"));
    State m = stopped(2); m.dlssg = false;  // nvngx_dlssg.dll missing: that warning stays first
    h = help(kBackend, m);
    CHECK(mentions(h, "nvngx_dlssg.dll not found")); CHECK(!mentions(h, "driver is too old"));
    if (fail) { std::printf("%d failed\n", fail); return 1; }
    std::printf("menu support fallback test passed\n");
    return 0;
}
CPP
g++ -std=c++17 -O1 -Wall -Wextra -Werror -I "$src" -I "$imgui" -o "$T/t" "$T/t.cpp" \
    || { echo "FAIL: menu support fallback test did not build"; exit 1; }
"$T/t" || { echo "FAIL: menu support fallback test"; exit 1; }
echo "PASS: menu support fallback"
