#!/bin/bash
# Checks that the FPS counter keeps its BASE reading (the rendered frame rate and its frame time) while frame
# generation is on, including when it adds no frames ("FG no frames"), and hides it when frame generation is off:
# a host test of pages::counterBase (src/menu_pages.h), then a source check that drawCounter draws BASE from it and
# not from the frame generation line's colour. Each source check must fail on a broken copy.
# Usage: tests/check_counter_base.sh [source folder] [Dear ImGui folder]
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
int main() {
    using namespace pages;
    char t[32];
    // frame generation on and adding no frames, 60 shown from 60 rendered: "FG no frames" with BASE 60 beside FPS 60
    for (int n : { 1, 2 }) {
        CHECK(!counterFg(t, sizeof t, true, true, n, 60, 60)); CHECK(std::strcmp(t, "FG no frames") == 0);
        CHECK(counterBase(true, 60));
    }
    CHECK(counterBase(true, 60) && counterFg(t, sizeof t, true, true, 0, 120, 60));  // generating 2.0x: BASE as today
    CHECK(!counterBase(false, 60));  // frame generation off: one number, as today
    CHECK(!counterBase(true, 0));    // nothing rendered counted yet
    if (fail) { std::printf("%d failed\n", fail); return 1; }
    std::printf("counter base test passed\n");
    return 0;
}
CPP
g++ -std=c++17 -O1 -Wall -Wextra -Werror -I "$src" -I "$imgui" -o "$T/t" "$T/t.cpp" \
    || { echo "FAIL: counter base test did not build"; exit 1; }
"$T/t" || { echo "FAIL: counter base test"; exit 1; }

python3 -I - "$src/menu.cpp" <<'PY' || exit 1
import re, sys
menu = open(sys.argv[1], encoding='utf-8', errors='replace').read().replace('\r\n', '\n')

def body(src, name):
    m = re.search(r'^\S[^\n;]*?\b' + name + r'\([^\n]*\{\n(.*?)^\}', src, re.M | re.S)
    return re.sub(r'//[^\n]*', '', m.group(1)) if m else None

def problems(menu):
    c = body(menu, 'drawCounter')
    if c is None: return ['drawCounter not found']
    out = []
    if 'const bool base = pages::counterBase(fg.generating, g_fpsRendered);' not in c:
        out.append('the counter does not decide BASE through counterBase')
    if not re.search(r'if \(base\) bigNumber\(g_bigThin, baseX, slotBase, g_fpsRendered, "BASE"\);', c):
        out.append('the BASE number is not drawn on that decision')
    if not re.search(r'if \(base\) \{ look::msText\(ms, sizeof ms, g_fpsRendered\);', c):
        out.append('the BASE frame time is not drawn on that decision')
    return out

p = problems(menu)
if p:
    for x in p: print('FAIL:', x)
    sys.exit(1)
broken = {
    'number on the colour': menu.replace('if (base) bigNumber(g_bigThin', 'if (gen) bigNumber(g_bigThin', 1),
    'frame time on the colour': menu.replace('if (base) { look::msText(ms, sizeof ms, g_fpsRendered);', 'if (gen) { look::msText(ms, sizeof ms, g_fpsRendered);', 1),
    'own decision': menu.replace('const bool base = pages::counterBase(fg.generating, g_fpsRendered);', 'const bool base = gen;', 1),
}
for name, b in broken.items():
    if b == menu: print('FAIL: could not build the broken copy:', name); sys.exit(1)
    if not problems(b): print('FAIL: check passed on the broken copy:', name); sys.exit(1)
print('counter base checks passed (and fail on %d broken copies)' % len(broken))
PY
echo "PASS: counter base"
