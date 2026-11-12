#!/bin/bash
# Checks the Insert menu's mouse buttons: builds tests/menu_buttons_test.cpp (src/menu_buttons.h) with the host
# compiler and runs it, then checks that both DirectInput hooks in menu.cpp record presses, that the menu takes its
# button states from buttonEvents, that the game still sees no mouse input while the menu is open and unchanged
# input while it is closed, and that each of those source checks fails on a broken copy.
# Usage: tests/check_menu_buttons.sh [source folder holding menu_buttons.h and menu.cpp]   (default: src next to tests)
here=$(cd "$(dirname "$0")" && pwd)
src=$(cd "${1:-$here/../src}" && pwd) || exit 1
T=$(mktemp -d) || exit 1
trap 'rm -rf "$T"' EXIT

g++ -std=c++17 -O2 -Wall -Wextra -Werror -I "$src" -o "$T/menu_buttons_test" "$here/menu_buttons_test.cpp" || { echo "FAIL: menu_buttons_test did not build"; exit 1; }
"$T/menu_buttons_test" || { echo "FAIL: menu_buttons_test"; exit 1; }

python3 -I - "$src/menu.cpp" <<'EOF' || exit 1
import re, sys
source = open(sys.argv[1], encoding='utf-8', errors='replace').read().replace('\r\n', '\n')

def body(src, name):
    m = re.search(r'^\S[^\n;]*?\b' + name + r'\([^\n]*\{\n(.*?)^\}', src, re.M | re.S)
    return m.group(1) if m else None

def problems(src):
    out = []
    st, dt, fm = body(src, 'getState'), body(src, 'getData'), body(src, 'feedMouse')
    if st is None or dt is None or fm is None:
        return ['getState, getData or feedMouse not found']
    for name, b in (('getState', st), ('getData', dt)):
        # closed menu (or a failed read): the game's read goes back untouched, before anything is recorded
        first = b.find('if (!g_open || FAILED(hr)')
        if first < 0 or first > b.find('g_btns.read('):
            out.append(name + ' does not return the read untouched while the menu is closed')
        if not re.search(r'\bg_btns\.read\(', b):
            out.append(name + ' does not record button presses (g_btns.read)')
    if 'memset(data, 0, cb);' not in st:
        out.append('getState no longer hides the mouse state from the game')
    if not re.search(r'if \(!\(fl & DIGDD_PEEK\)\) \*n = 0;', dt):
        out.append('getData no longer consumes the events the menu used')
    if not re.search(r'\bg_btns\.take\(\)', fm):
        out.append('feedMouse does not take the recorded buttons (g_btns.take)')
    if not re.search(r'\bbuttonEvents\(', fm):
        out.append('feedMouse does not queue the states from buttonEvents')
    if not re.search(r'logf\("menu input:', fm):
        out.append('feedMouse does not log the menu input line')
    return out

found = problems(source)
if found:
    for p in found:
        print('FAIL: ' + p)
    sys.exit(1)

# Each check must fail on a copy broken the way it guards against.
mutants = [
    ('getState records no presses', lambda s: s.replace('g_btns.read(i, (m->rgbButtons[i] & 0x80) != 0)', 'void(0)', 1)),
    ('getData records no presses', lambda s: s.replace('g_btns.read(b, (e.dwData & 0x80) != 0)', 'void(0)', 1)),
    ('getState shows the game the mouse', lambda s: s.replace('memset(data, 0, cb);', '', 1)),
    ('getData leaves the events to the game', lambda s: s.replace('if (!(fl & DIGDD_PEEK)) *n = 0;', '', 1)),
    ('getState records while closed', lambda s: s.replace('if (!g_open || FAILED(hr) || !data) return hr;', 'if (FAILED(hr) || !data) return hr;', 1)),
    ('feedMouse ignores buttonEvents', lambda s: s.replace('buttonEvents(', 'buttonEventz(', 1)),
    ('feedMouse never takes the buttons', lambda s: s.replace('g_btns.take()', 'g_btns', 1)),
    ('no menu input line', lambda s: s.replace('logf("menu input:', 'logf("menu inp:', 1)),
]
for name, mutate in mutants:
    broken = mutate(source)
    if broken == source:
        print('FAIL: mutant "%s" did not change menu.cpp: the check has drifted from the code' % name)
        sys.exit(1)
    if not problems(broken):
        print('FAIL: the source checks pass on a copy where %s' % name)
        sys.exit(1)
print('source checks ok, and each fails on its broken copy (%d)' % len(mutants))
EOF
echo "PASS: menu buttons"
