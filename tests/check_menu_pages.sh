#!/bin/bash
# Checks the Insert menu's page model: builds tests/menu_pages_test.cpp (src/menu_pages.h, with Dear ImGui's core for the
# fit test) with the host compiler and runs it with the bundled TeX Gyre Adventor, then checks menu.cpp's structure: both
# layouts take their rows, help and status from menu_pages.h; the Portable mode row is gone and only the layout button
# changes the layout while the menu is open; Tab is not one of the typing keys ImGui sees; every setter writes the same
# ini keys as before; the desktop menu is a fixed size with no scrollbar; the desktop's L1 / R1 turn the page only when no
# box is typed in and no list is open; a desktop page change starts on the first row. Each source check must fail on a broken copy.
# Usage: tests/check_menu_pages.sh [source folder holding menu_pages.h and menu.cpp] [Dear ImGui folder]
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
g++ -std=c++17 -O1 -Wall -Wextra -Werror -I "$src" -I "$imgui" -o "$T/menu_pages_test" "$here/menu_pages_test.cpp" "$T"/imgui*.o \
    || { echo "FAIL: menu_pages_test did not build"; exit 1; }
"$T/menu_pages_test" "$font" || { echo "FAIL: menu_pages_test"; exit 1; }

python3 -I - "$src/menu.cpp" "$src/menu_pad.h" <<'PY' || exit 1
import re, sys
rd = lambda p: open(p, encoding='utf-8', errors='replace').read().replace('\r\n', '\n')
source, pad = rd(sys.argv[1]), rd(sys.argv[2])

def body(src, name):
    m = re.search(r'^\S[^\n;]*?\b' + name + r'\([^\n]*\{\n(.*?)^\}', src, re.M | re.S)
    return m.group(1) if m else None
code = lambda b: re.sub(r'//[^\n]*', '', b)
# the keys the menu saved before this change (1.0.1): the layout change keeps every one and adds none
KEYS = {('dlss', 'game_sharpening'), ('dlss', 'preset'), ('dlss', 'sharpness'), ('dlss', 'upscaler'), ('fg', 'antilag'),
        ('fg', 'backend'), ('fg', 'enable'), ('fg', 'frames'), ('fg', 'limit'), ('fg', 'reflex'), ('fg', 'target'),
        ('overlay', 'corner'), ('overlay', 'enable'), ('overlay', 'layout'), ('ui', 'sharp')}

def problems(src, pd):
    out = []
    menu, deckm, comp, init = (body(src, n) for n in ('drawMenu', 'drawDeckMenu', 'compose', 'menuInit'))
    if None in (menu, deckm, comp, init): return ['drawMenu, drawDeckMenu, compose or menuInit not found']
    c = code(src)
    # both layouts draw from the page model: rows, help and status
    for f, name, page in ((code(menu), 'desktop', 'g_deskPage'), (code(deckm), 'Portable', 'nav.page')):
        if 'const pages::State st = fillState();' not in f: out.append('the %s menu does not fill the page model' % name)
        if 'pages::rows(%s, st, rows, &sub)' % page not in f: out.append('the %s menu does not take its rows from the page model' % name)
        if not re.search(r'drawHelp\([^\n]*, helpId, pages::help\(helpId, st\)\);', f) or not re.search(r'drawStatus\([^\n]*, pages::status\(st\)\);', f):
            out.append('the %s menu does not draw the help and status from the page model' % name)
    # the Portable mode row is gone; only the layout button (and the setting read at start) sets the layout
    if '"Portable mode"' in c or 'look::portable(' in c: out.append('the Portable mode row is still there')
    sets = [m.start() for m in re.finditer(r'\bg_layout = ', c)]
    sl = body(src, 'switchLayout')
    if len(sets) != 2 or sl is None or 'g_layout = portable ? 1 : 2;' not in code(sl) or 'g_layout = deck::layoutValue(' not in code(init):
        out.append('the layout is set somewhere other than the layout button and the start')
    # Tab is never one of the keys ImGui sees, so ImGui's own Tab focus stays off
    keys = re.search(r'kTypingKeys\[\] = \{(.*?)\n\};', pd, re.S)
    if not keys or re.search(r'\{ *0x09\b|ImGuiKey_Tab', keys.group(1)): out.append('Tab is one of the typing keys')
    # the desktop page keys: L1 / R1 in compose, with the typing and list guards; no keyboard page key (the game sees the
    # key too and acts on it)
    k = code(comp)
    if re.search(r'VK_TAB', k): out.append('Tab turns the desktop page')
    if not re.search(r'if \(g_open && !g_deckOpen && !g_typing && !g_padBusy\) \{\n\s*const int turn = \(g_padView\.edges & pad::kR1\) \? 1 : \(g_padView\.edges & pad::kL1\) \? -1 : 0;', k):
        out.append('L1 and R1 do not turn the desktop page, or turn it while typing')
    # the ini keys are the same as before
    got = set(re.findall(r'iniSet\(L"([a-z0-9_]+)", L"([a-z0-9_]+)"', c))
    if got != KEYS: out.append('the menu saves different ini keys: %s' % sorted(got ^ KEYS))
    # the desktop menu: a fixed size from the slots, no scrollbar, no scrolling with the wheel
    m = code(menu)
    if 'ImGui::SetNextWindowSize({ 600 * s, Z::kHeight * s }, ImGuiCond_Always);' not in m: out.append('the desktop menu is not a fixed size')
    # a page change (a tab click, L1, R1) puts the focus and the help on the new page's first row
    if not re.search(r'if \(g_pageTurn\) \{ to = pages::turn\(g_deskPage, g_pageTurn\); g_pageTurn = 0; \}\n\s*const bool turned = to != g_deskPage;', m) \
            or not re.search(r'if \(\(turned \|\| [^\n]*\) && i == 0\) look::focusItem\(', m) or 'if (n && (turned || ' not in m:
        out.append('a desktop page change does not start on the first row')
    if 'ImGuiWindowFlags_NoScrollbar' not in m or 'ImGuiWindowFlags_NoScrollWithMouse' not in m: out.append('the desktop menu can scroll')
    return out

p = problems(source, pad)
if p:
    for x in p: print('FAIL:', x)
    sys.exit(1)
menu, deckm, comp = body(source, 'drawMenu'), body(source, 'drawDeckMenu'), body(source, 'compose')
broken = {
    'deck own rows': (source.replace(deckm, deckm.replace('pages::rows(nav.page, st, rows, &sub)', 'deckRows(nav.page, st, rows, &sub)'), 1), pad),
    'desktop own help': (source.replace(menu, menu.replace('drawHelp(', 'ownHelp('), 1), pad),
    'deck status not from the model': (source.replace(deckm, deckm.replace('pages::status(st)', 'ownStatus(st)'), 1), pad),
    'Portable mode row back': (source.replace(menu, menu + '    look::portable("Portable mode");\n', 1), pad),
    'layout set on open': (source.replace('            g_open = true;\n', '            g_open = true;\n            g_layout = 2;\n', 1), pad),
    'Tab fed to ImGui': (source, pad.replace('{ 0x08, ImGuiKey_Backspace, 0 },', '{ 0x08, ImGuiKey_Backspace, 0 }, { 0x09, ImGuiKey_Tab, 0 },', 1)),
    'Tab turns the page': (source.replace(comp, comp.replace('        if (turn) g_pageTurn = turn;', '        if (turn) g_pageTurn = turn;\n        if (GetAsyncKeyState(VK_TAB) & 0x8000) g_pageTurn = 1;', 1), 1), pad),
    'L1 while typing': (source.replace('if (g_open && !g_deckOpen && !g_typing && !g_padBusy) {', 'if (g_open && !g_deckOpen) {', 1), pad),
    'new ini key': (source.replace('iniSet(L"ui", L"sharp"', 'iniSet(L"ui", L"sharper"', 1), pad),
    'growing window': (source.replace('ImGui::SetNextWindowSize({ 600 * s, Z::kHeight * s }, ImGuiCond_Always);', 'ImGui::SetNextWindowSize({ 600 * s, 0 }, ImGuiCond_Always);', 1), pad),
    'turn keeps the focus': (source.replace(menu, menu.replace('if ((turned || opening', 'if ((opening', 1), 1), pad),
    'scrollbar': (source.replace(menu, menu.replace('ImGuiWindowFlags_NoScrollbar', 'ImGuiWindowFlags_None', 1), 1), pad),
}
for name, (s, pd) in broken.items():
    if (s, pd) == (source, pad): print('FAIL: could not build the broken copy:', name); sys.exit(1)
    if not problems(s, pd): print('FAIL: check passed on the broken copy:', name); sys.exit(1)
print('menu.cpp page model checks passed (and fail on %d broken copies)' % len(broken))
PY
echo "PASS: menu pages"
