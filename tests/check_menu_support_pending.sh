#!/bin/bash
# Checks the Frame generation row when Streamline reported DLSS frame generation unsupported, its start-up then stopped,
# and the player has picked another backend that waits for a restart: the restart line shows, and the row never names a
# different cause (the generic card line) in place of the reported one. Host test of src/menu_pages.h.
# Usage: bash tests/check_menu_support_pending.sh [source folder]   (default: src next to tests)
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
static const char* const kGeneric = "Not available: DLSS frame generation needs an RTX 40 or 50 card.";
static const char* const kRestart = "Restart the game to switch frame generation.";
static const char* const kDriver = "The NVIDIA driver is too old for DLSS frame generation. Update the driver, then restart the game.";
static bool has(const Help& h, const char* t, Tone tone) {
    for (int i = 0; i < h.n; ++i) if (std::strcmp(h.line[i].text, t) == 0 && h.line[i].tone == tone) return true;
    return false;
}
static bool mentions(const Help& h, const char* t) {
    for (int i = 0; i < h.n; ++i) if (std::strstr(h.line[i].text, t)) return true;
    return false;
}
// DLSS frame generation's start-up stopped after Streamline's result `support`; FSR picked in the menu, not restarted
static State pendingFsr(int support) {
    State s;
    s.nvidia = true; s.want = 0; s.active = 0; s.upscaler = -1;
    s.backend = 2; s.launchBackend = -1; s.picked = 1; s.running = 0; s.fgBackend = -1; s.fgAvailable = false; s.fgEnable = 1;
    s.launchEnable = true; s.dlssg = true; s.fgSupport = support; s.wine = false;
    return s;
}
int main() {
    Help h = help(kBackend, pendingFsr(4));  // the 3-line scheduling reason gives way to the restart line
    CHECK(has(h, kRestart, kGreen)); CHECK(!mentions(h, kGeneric)); CHECK(!mentions(h, "scheduling"));
    h = help(kBackend, pendingFsr(2));       // a 2-line reason fits beside it
    CHECK(has(h, kDriver, kAmber)); CHECK(has(h, kRestart, kGreen)); CHECK(!mentions(h, kGeneric));
    h = help(kBackend, pendingFsr(0));       // nothing reported: the row as before
    CHECK(has(h, kGeneric, kDim)); CHECK(has(h, kRestart, kGreen));
    if (fail) { std::printf("%d failed\n", fail); return 1; }
    std::printf("menu support pending test passed\n");
    return 0;
}
CPP
g++ -std=c++17 -O1 -Wall -Wextra -Werror -I "$src" -o "$T/t" "$T/t.cpp" || { echo "FAIL: menu support pending test did not build"; exit 1; }
"$T/t" || { echo "FAIL: menu support pending test"; exit 1; }
echo "PASS: menu support pending"
