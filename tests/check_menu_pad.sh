#!/bin/bash
# Checks the Insert menu's controller support: builds tests/menu_pad_test.cpp (src/menu_pad.h with Dear ImGui's core)
# with the host compiler and runs it, then checks menu.cpp's structure: the controller hook blanks the pad while the
# menu is open and hides latched buttons while it is closed, takes no lock, every close goes through closeMenu, which
# latches before it clears g_open, and the menu never sees X or Y. Each source check must fail on a broken copy.
# Usage: tests/check_menu_pad.sh [source folder holding menu_pad.h and menu.cpp] [Dear ImGui folder]
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
g++ -std=c++17 -O1 -Wall -Wextra -Werror -I "$src" -I "$imgui" -o "$T/menu_pad_test" "$here/menu_pad_test.cpp" "$T"/imgui*.o \
    || { echo "FAIL: menu_pad_test did not build"; exit 1; }
"$T/menu_pad_test" || { echo "FAIL: menu_pad_test"; exit 1; }

python3 -I - "$src/menu.cpp" "$src/menu_pad.h" <<'PY' || exit 1
import re, sys
source = open(sys.argv[1], encoding='utf-8', errors='replace').read().replace('\r\n', '\n')
header = open(sys.argv[2], encoding='utf-8', errors='replace').read().replace('\r\n', '\n')

def body(src, name):
    m = re.search(r'^\S[^\n;]*?\b' + name + r'\([^\n]*\{\n(.*?)^\}', src, re.M | re.S)
    return m.group(1) if m else None

def problems(src, hdr):
    out = []
    hk, close, poll, init, comp, mf = (body(src, n) for n in ('hk_XInputGetState', 'closeMenu', 'pollToggle', 'menuInit', 'compose', 'menuFrame'))
    if None in (hk, close, poll, init, comp, mf): return ['hk_XInputGetState, closeMenu, pollToggle, menuInit, compose or menuFrame not found']
    code = lambda b: re.sub(r'//[^\n]*', '', b)
    if not re.search(r'^std::atomic<bool> g_open\b', src, re.M): out.append('g_open is not std::atomic<bool>')
    # the controller hook: records, blanks while open, hides latched buttons while closed, never waits
    h = code(hk)
    if 'g_pad.record(' not in h: out.append('the controller hook does not record the pad')
    if not re.search(r'if \(ok && g_open\) s->Gamepad = \{\};', h): out.append('the controller hook no longer blanks the pad while the menu is open')
    if not re.search(r'else if \(ok\) s->Gamepad\.wButtons = g_pad\.hide\(', h): out.append('the controller hook does not hide latched buttons while the menu is closed')
    if re.search(r'lock_guard|unique_lock|mutex|logf\(|new |malloc', h): out.append('the controller hook waits, logs or allocates')
    # every close goes through closeMenu, which latches before it clears g_open
    c = code(close)
    i, j = c.find('g_pad.latchAll();'), c.find('g_open = false;')
    if i < 0 or j < 0 or i > j: out.append('closeMenu does not latch the buttons before clearing g_open')
    rest = code(src.replace(close, ''))
    if re.search(r'g_open = (false|!g_open)|g_open\.store\(false', rest): out.append('the menu closes somewhere other than closeMenu')
    if 'closeMenu();' not in code(poll): out.append('pollToggle does not close through closeMenu')
    if 'combo.update(' not in code(poll): out.append('pollToggle does not time the stick combo')
    # B with the menu open and nothing busy closes it (AC7); the menu runs one ImGui frame after any close, even with the
    # counter off, so the pad keys are released and the window reopens on its first setting (AC5)
    pc = code(poll)
    if not re.search(r'bClose = g_open && !g_padBusy && \(g_padView\.edges & pad::kB\);', pc) or \
       not re.search(r'if \([^\n]*\|\| bClose\) \{', pc): out.append('B does not close the idle menu')
    if not re.search(r'\(g_counter \|\| g_open \|\| g_padRelease\) && compose\(', code(mf)):
        out.append('menuFrame skips the frame after a close when the counter is off')
    # the menu gets the controller from ImGui's own navigation, and only while a pad is connected
    if 'ImGuiConfigFlags_NavEnableGamepad' not in code(init): out.append('menuInit does not enable controller navigation')
    k = code(comp)
    if not re.search(r'if \(g_padView\.connected\) io\.BackendFlags \|= ImGuiBackendFlags_HasGamepad;', k) or \
       not re.search(r'else io\.BackendFlags &= ~ImGuiBackendFlags_HasGamepad;', k):
        out.append('compose does not follow the pad being connected (HasGamepad)')
    if re.search(r'ImGuiKey_GamepadFace(Left|Up)', code(src) + code(hdr)): out.append('X or Y reaches the menu')
    return out

p = problems(source, header)
if p:
    for x in p: print('FAIL:', x)
    sys.exit(1)
hk = body(source, 'hk_XInputGetState'); close = body(source, 'closeMenu')
broken = {
    'no blank while open': (source.replace('if (ok && g_open) s->Gamepad = {};', 'if (false) s->Gamepad = {};', 1), header),
    'no latch hiding': (source.replace('s->Gamepad.wButtons = g_pad.hide(', 's->Gamepad.wButtons = (', 1), header),
    'lock in the hook': (source.replace(hk, hk.replace('g_pad.record(', 'std::lock_guard<std::mutex> lk(g_inMx); g_pad.record(', 1), 1), header),
    'latch after clear': (source.replace(close, close.replace('g_pad.latchAll();', '', 1).replace('g_open = false;', 'g_open = false; g_pad.latchAll();', 1), 1), header),
    'close outside closeMenu': (source.replace('closeMenu();', 'g_open = false;', 1), header),
    'no gamepad nav': (source.replace('ImGuiConfigFlags_NavEnableGamepad', 'ImGuiConfigFlags_None', 1), header),
    'HasGamepad always on': (source.replace('else io.BackendFlags &= ~ImGuiBackendFlags_HasGamepad;', '', 1), header),
    'X fed': (source, header.replace('button(ImGuiKey_GamepadFaceDown, kA, false);', 'button(ImGuiKey_GamepadFaceDown, kA, false); io.AddKeyEvent(ImGuiKey_GamepadFaceLeft, false);', 1)),
    'g_open plain bool': (source.replace('std::atomic<bool> g_open', 'bool g_open', 1), header),
    'B never closes': (source.replace(' || bClose) {', ') {', 1), header),
    'B closes while busy': (source.replace('bClose = g_open && !g_padBusy && ', 'bClose = g_open && ', 1), header),
    'no frame after close': (source.replace('(g_counter || g_open || g_padRelease) && compose(', '(g_counter || g_open) && compose(', 1), header),
}
for name, (s, h) in broken.items():
    if (s, h) == (source, header): print('FAIL: could not build the broken copy:', name); sys.exit(1)
    if not problems(s, h): print('FAIL: check passed on the broken copy:', name); sys.exit(1)
print('menu.cpp controller checks passed (and fail on %d broken copies)' % len(broken))
PY
