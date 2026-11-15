#!/bin/bash
# Checks the Portable mode switch, the live layout change, the pointing glove and the Steam Deck layout's look: builds
# tests/portable_test.cpp (src/menu_look.h, src/menu_deck.h and src/menu_pad.h with Dear ImGui's core) with the host
# compiler and runs it, then checks the structure around it: the change is applied in compose's open block before the
# controller is fed and the navigation flags are set; the open branch drops a pending change; both layouts' layout
# buttons flip through the same handler (switchLayout); the glove's sway is set outside row() and the glove uses no
# image; README no longer says the layout changes the next time the menu opens. Each source check must fail on a broken
# copy. (The backend's names now come from the page model, checked by tests/menu_pages_test.cpp.)
# Usage: tests/check_portable.sh [source folder holding menu_look.h, menu_deck.h, menu_pad.h and menu.cpp] [Dear ImGui folder]
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
g++ -std=c++17 -O1 -Wall -Wextra -Werror -I "$src" -I "$imgui" -o "$T/portable_test" "$here/portable_test.cpp" "$T"/imgui*.o \
    || { echo "FAIL: portable_test did not build"; exit 1; }
"$T/portable_test" || { echo "FAIL: portable_test"; exit 1; }

python3 -I - "$src/menu.cpp" "$src/menu_look.h" "$src/../README.md" <<'PY' || exit 1
import re, sys
rd = lambda p: open(p, encoding='utf-8', errors='replace').read().replace('\r\n', '\n')
source, look, readme = rd(sys.argv[1]), rd(sys.argv[2]), rd(sys.argv[3])

def body(src, name):
    m = re.search(r'^\S[^\n;]*?\b' + name + r'\([^\n]*\{\n(.*?)^\}', src, re.M | re.S)
    return m.group(1) if m else None
def inline(src, name):
    m = re.search(r'^inline [^\n;]*?\b' + name + r'\([^\n]*\{\n(.*?)^\}', src, re.M | re.S)
    return m.group(1) if m else None
code = lambda b: re.sub(r'//[^\n]*', '', b)
HANDLER = 'g_layout = portable ? 1 : 2; iniSet(L"overlay", L"layout", g_layout); g_swap.flip(portable, ImGui::GetTime(), g_deckOpen);'
flat = lambda b: re.sub(r'\s+', ' ', code(b)).strip()

def problems(src, lk, rm):
    out = []
    comp, poll, menu, deckm = (body(src, n) for n in ('compose', 'pollToggle', 'drawMenu', 'drawDeckMenu'))
    row, glove = inline(lk, 'row'), inline(lk, 'glove')
    if None in (comp, poll, menu, deckm, row, glove): return ['compose, pollToggle, drawMenu, drawDeckMenu, look::row or look::glove not found']
    k = code(comp)
    due, feed, nav = k.find('if (g_swap.due(ImGui::GetTime()))'), k.find('pad::feedPad('), k.find('NavEnableGamepad')
    if due < 0 or due > feed or due > nav or due < k.find('if (g_open) {'): out.append('the change is not applied in compose before feedPad and the navigation flags')
    p = code(poll)
    if not re.search(r'g_open = true;\n\s*g_deckOpen = [^\n]*\n\s*g_deckTyping = \{\};\n\s*g_swap = \{\};', p): out.append('the open branch does not drop a pending change')
    sl = body(src, 'switchLayout')
    if sl is None or HANDLER not in flat(sl) or code(menu).count('switchLayout(want == 1);') != 1 or code(deckm).count('switchLayout(want == 1);') != 1:
        out.append('the two layouts do not flip through the same handler')
    if 'ctx().bob = ' in code(row) or 'GetTime' in code(row): out.append('the glove sway is set inside row()')
    for f, name in ((menu, 'drawMenu'), (deckm, 'drawDeckMenu')):
        if not re.search(r'look::ctx\(\)\.bob = look::bobX\(ImGui::GetTime\(\)\);', code(f)): out.append(name + ' does not set the glove sway')
    if 'AddImage' in glove or 'TextureID' in glove: out.append('the glove draws an image')
    if 'next time the menu opens' in rm: out.append('README still says the layout changes the next time the menu opens')
    return out

p = problems(source, look, readme)
if p:
    for x in p: print('FAIL:', x)
    sys.exit(1)
comp, menu, deckm = body(source, 'compose'), body(source, 'drawMenu'), body(source, 'drawDeckMenu')
late = comp.replace('        if (g_swap.due(ImGui::GetTime())) {', '        if (false) {', 1).replace('    ImGui::NewFrame();', '    ImGui::NewFrame();\n    if (g_swap.due(ImGui::GetTime())) {}', 1)
broken = {
    'swap after NewFrame': (source.replace(comp, late, 1), look, readme),
    'open keeps the swap': (source.replace('            g_swap = {}; g_focusPortable = false;', '            g_focusPortable = false;', 1), look, readme),
    'handler differs': (source.replace('g_swap.flip(portable, ImGui::GetTime(), g_deckOpen)', 'g_swap.flip(portable, 0, g_deckOpen)', 1), look, readme),
    'deck own handler': (source.replace(deckm, deckm.replace('if (want >= 0) switchLayout(want == 1);', 'if (want >= 0) g_layout = want == 1 ? 1 : 2;', 1), 1), look, readme),
    'bob in row': (source, look.replace('    ImDrawList* dl = w->DrawList;\n', '    ImDrawList* dl = w->DrawList;\n    ctx().bob = bobX(ImGui::GetTime());\n', 1), readme),
    'deck sway not set': (source.replace(deckm, deckm.replace('    look::ctx().bob = look::bobX(ImGui::GetTime());', '', 1), 1), look, readme),
    'glove image': (source, look.replace('    dl->AddLine(ImVec2(o.x + 3.4f', '    dl->AddImage(ImTextureID(), o, o);\n    dl->AddLine(ImVec2(o.x + 3.4f', 1), readme),
    'readme next time': (source, look, readme + '\nDesktop layout from the next time the menu opens.\n'),
}
for name, (s, l, r) in broken.items():
    if (s, l, r) == (source, look, readme): print('FAIL: could not build the broken copy:', name); sys.exit(1)
    if not problems(s, l, r): print('FAIL: check passed on the broken copy:', name); sys.exit(1)
print('Portable mode structure checks passed (and fail on %d broken copies)' % len(broken))
PY
echo "PASS: portable"
