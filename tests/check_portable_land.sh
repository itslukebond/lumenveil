#!/bin/bash
# Checks where a Portable mode change lands: builds tests/portable_land_test.cpp (deck::landFocus and look::takeFocus,
# the functions the menu calls) with the host compiler and runs it, then checks that the menu calls them: compose's
# landing block calls deck::landFocus after the new layout is set, and the desktop menu calls look::takeFocus right
# after its Portable mode switch. Each source check must fail on a broken copy.
# Usage: tests/check_portable_land.sh [source folder holding menu_look.h, menu_deck.h and menu.cpp] [Dear ImGui folder]
#        (defaults: src next to tests; third_party/imgui next to tests, or $IMGUI)
here=$(cd "$(dirname "$0")" && pwd)
src=$(cd "${1:-$here/../src}" && pwd) || exit 1
imgui=${2:-${IMGUI:-$here/../third_party/imgui}}
[ -f "$imgui/imgui.cpp" ] || { echo "FAIL: Dear ImGui not found at $imgui (pass its folder or set IMGUI)"; exit 1; }
imgui=$(cd "$imgui" && pwd)
T=$(mktemp -d) || exit 1
trap 'rm -rf "$T"' EXIT

for f in imgui imgui_draw imgui_widgets imgui_tables; do
    g++ -std=c++17 -O1 -w -I "$imgui" -c "$imgui/$f.cpp" -o "$T/$f.o" & done
wait
g++ -std=c++17 -O1 -Wall -Wextra -Werror -I "$src" -I "$imgui" -o "$T/portable_land_test" "$here/portable_land_test.cpp" "$T"/imgui*.o \
    || { echo "FAIL: portable_land_test did not build"; exit 1; }
"$T/portable_land_test" || { echo "FAIL: portable_land_test"; exit 1; }

python3 -I - "$src/menu.cpp" <<'PY' || exit 1
import re, sys
source = open(sys.argv[1], encoding='utf-8', errors='replace').read().replace('\r\n', '\n')

def body(src, name):
    m = re.search(r'^\S[^\n;]*?\b' + name + r'\([^\n]*\{\n(.*?)^\}', src, re.M | re.S)
    return m.group(1) if m else None
code = lambda b: re.sub(r'//[^\n]*', '', b)
LAND = 'deck::landFocus(g_deckOpen, g_deckNav, g_focusPortable);'
TAKE = 'look::takeFocus(g_focusPortable, g_padUsed);'

def problems(src):
    comp, menu = body(src, 'compose'), body(src, 'drawMenu')
    if None in (comp, menu): return ['compose or drawMenu not found']
    out = []
    k = code(comp)
    m = re.search(r'if \(g_swap\.due\(ImGui::GetTime\(\)\)\) \{\n(.*?)\n\s*\}', k, re.S)
    if not m or k.count(LAND) != 1 or LAND not in m.group(1) or m.group(1).find('g_deckOpen = ') > m.group(1).find(LAND):
        out.append('the landing block does not call deck::landFocus after setting the layout')
    d = code(menu)
    sw = d.find('look::portable("##portable"')
    if d.count(TAKE) != 1 or sw < 0 or d.find(TAKE) < sw or d.find(TAKE) > d.find('note(', sw):
        out.append('the desktop menu does not call look::takeFocus right after its Portable mode switch')
    return out

p = problems(source)
if p:
    for x in p: print('FAIL:', x)
    sys.exit(1)
comp = body(source, 'compose')
broken = {
    'no landing focus': source.replace(LAND, '', 1),
    'landing focus outside the block': source.replace(comp, comp.replace(LAND, '', 1).replace('        pad::feedPad(', '        ' + LAND + '\n        pad::feedPad(', 1), 1),
    'no desktop focus': source.replace(TAKE, '', 1),
    'desktop focus before the switch': source.replace(TAKE, '', 1).replace('        label("Portable mode");', '        label("Portable mode");\n        ' + TAKE, 1),
}
for name, s in broken.items():
    if s == source: print('FAIL: could not build the broken copy:', name); sys.exit(1)
    if not problems(s): print('FAIL: check passed on the broken copy:', name); sys.exit(1)
print('Portable mode landing structure checks passed (and fail on %d broken copies)' % len(broken))
PY
echo "PASS: portable land"
