#!/bin/bash
# Checks what the Insert menu shows when Streamline says DLSS frame generation cannot run on this PC: builds
# tests/menu_support_test.cpp (src/menu_pages.h, with Dear ImGui's core for the fit test) with the host compiler and runs
# it with the bundled TeX Gyre Adventor; then checks that menu.cpp gives the page model Streamline's result and whether
# the game runs under Wine, and that fgStatus reports Wine. Each source check must fail on a broken copy.
# Usage: tests/check_menu_support.sh [source folder] [Dear ImGui folder]
#        (defaults: src next to tests; third_party/imgui next to tests, or $IMGUI)
here=$(cd "$(dirname "$0")" && pwd)
src=$(cd "${1:-$here/../src}" && pwd) || exit 1
imgui=${2:-${IMGUI:-$here/../third_party/imgui}}
font=$here/../third_party/tex-gyre-adventor/texgyreadventor-regular.otf
[ -f "$imgui/imgui.cpp" ] || { echo "FAIL: Dear ImGui not found at $imgui (pass its folder or set IMGUI)"; exit 1; }
[ -f "$font" ] || { echo "FAIL: $font not found"; exit 1; }
imgui=$(cd "$imgui" && pwd)
T=$(mktemp -d) || exit 1
trap 'rm -rf "$T"' EXIT

for f in imgui imgui_draw imgui_widgets imgui_tables; do
    g++ -std=c++17 -O1 -w -I "$imgui" -c "$imgui/$f.cpp" -o "$T/$f.o" & done
wait
g++ -std=c++17 -O1 -Wall -Wextra -Werror -I "$src" -I "$imgui" -o "$T/menu_support_test" "$here/menu_support_test.cpp" "$T"/imgui*.o \
    || { echo "FAIL: menu_support_test did not build"; exit 1; }
"$T/menu_support_test" "$font" || { echo "FAIL: menu_support_test"; exit 1; }

python3 -I - "$src/menu.cpp" "$src/present12.cpp" <<'PY' || exit 1
import re, sys
menu = open(sys.argv[1], encoding='utf-8', errors='replace').read().replace('\r\n', '\n')
p12 = open(sys.argv[2], encoding='utf-8', errors='replace').read().replace('\r\n', '\n')

def body(src, name):
    m = re.search(r'^\S[^\n;]*?\b' + name + r'\([^\n]*\{\n(.*?)^\}', src, re.M | re.S)
    return re.sub(r'//[^\n]*', '', m.group(1)) if m else None

def problems(menu, p12):
    out = []
    fill, status = body(menu, 'fillState'), body(p12, 'fgStatus')
    if None in (fill, status): return ['a function under check was not found']
    if 's.fgSupport = st.slSupport;' not in fill: out.append('the page model is not given Streamline\'s result')
    if 's.wine = st.wine;' not in fill: out.append('the page model is not told about Wine')
    if 's.wine = g_wine;' not in status: out.append('fgStatus does not report Wine')
    return out

p = problems(menu, p12)
if p:
    for x in p: print('FAIL:', x)
    sys.exit(1)
broken = {
    'result not passed': (menu.replace('s.fgSupport = st.slSupport;', '', 1), p12),
    'wine not passed': (menu.replace('s.wine = st.wine;', '', 1), p12),
    'wine not reported': (menu, p12.replace('s.wine = g_wine;', '', 1)),
}
for name, (m, q) in broken.items():
    if (m, q) == (menu, p12): print('FAIL: could not build the broken copy:', name); sys.exit(1)
    if not problems(m, q): print('FAIL: check passed on the broken copy:', name); sys.exit(1)
print('menu support checks passed (and fail on %d broken copies)' % len(broken))
PY
echo "PASS: menu support"
