#!/bin/bash
# Checks the Insert menu's input code in menu.cpp beyond the button logic: every DirectInput state read is hidden
# from the game while the menu is open, not only the mouse's; the keyboard's own read methods are hooked as well as the
# mouse's, each distinct method once; a keyboard state read while the menu is closed goes through the key latch, and
# closing the menu latches every key; buffered keyboard events while open keep their releases; the menu decides whether the cursor follows
# DirectInput from a clock read after it copies the last read time; the input counters start from zero each time the
# menu opens; the `menu input:` line counts mouse state reads of each size. Each check is also run on a copy broken
# the way it guards against and must fail there.
# Usage: tests/check_menu_hooks.sh [source folder holding menu.cpp]   (default: src next to tests)
here=$(cd "$(dirname "$0")" && pwd)
src=$(cd "${1:-$here/../src}" && pwd) || exit 1

python3 -I - "$src/menu.cpp" <<'EOF' || exit 1
import re, sys
source = open(sys.argv[1], encoding='utf-8', errors='replace').read().replace('\r\n', '\n')

def body(src, name):
    m = re.search(r'^\S[^\n;]*?\b' + name + r'\([^\n]*\{\n(.*?)^\}', src, re.M | re.S)
    return m.group(1) if m else None

code = lambda b: re.sub(r'//[^\n]*', '', b)

def problems(src):
    out = []
    st, fm = body(src, 'getState'), body(src, 'feedMouse')
    if st is None or fm is None:
        return ['getState or feedMouse not found']
    # getState: the zeroing runs at the top level of the function (4-space indent), after the open check, so every
    # device's state read is hidden while the menu is open, whatever branch the mouse handling takes.
    guard = st.find('    if (!g_open || FAILED(hr) || !data) return hr;\n')
    zero = re.search(r'^    memset\(data, 0, cb\);', st, re.M)
    if guard < 0 or not zero or zero.start() < guard:
        out.append('getState does not zero every state read at its top level after the open check')
    # hookInput: a keyboard device as well as a mouse device for each interface, and one hook per distinct method
    hi, cm, gd = body(src, 'hookInput'), body(src, 'closeMenu'), body(src, 'getData')
    if hi is None or cm is None or gd is None: return out + ['hookInput, closeMenu or getData not found']
    h = code(hi)
    if not re.search(r'for \(const GUID\* g : \{ &GUID_SysMouse, &GUID_SysKeyboard \}\)', h) or 'CreateDevice(*g, &dev, nullptr)' not in h:
        out.append('hookInput does not hook a keyboard device as well as a mouse device')
    if 'for (int i = 0; i < n; ++i) if (at[i] == target) return;' not in h:
        out.append('hookInput can hook one method twice')
    # getState: a closed keyboard state read (256 bytes) goes through the key latch, before the open check returns
    latch = st.find('    if (!g_open && SUCCEEDED(hr) && data && cb == 256 && g_keyLatch.any()) g_keyLatch.hide((unsigned char*)data);')
    if latch < 0 or guard < 0 or latch > guard: out.append('a closed keyboard read does not go through the key latch')
    if 'g_keyLatch.latchAll();' not in code(cm): out.append('closing the menu does not latch the keys')
    # getData: a keyboard's buffered events keep their releases while open; other devices' events are consumed
    if not re.search(r'if \(od && isKeyboard\(d\)\) \*n = pad::keepReleases\(\(unsigned char\*\)od, \*n, cb\);\n\s*else if \(!\(fl & DIGDD_PEEK\)\) \*n = 0;', code(gd)):
        out.append('buffered keyboard events do not keep their releases while the menu is open')
    # feedMouse: the clock it compares with the last DirectInput read time is read after that time is copied.
    copy = fm.find('tick = g_diMouseTick')
    clock = re.search(r'\bnow = GetTickCount64\(\)', fm)
    if copy < 0 or not clock or clock.start() < copy:
        out.append('feedMouse reads the clock before it copies the last DirectInput read time')
    # feedMouse: while the menu is closed, its own counters and queued states go back to zero.
    closed = re.search(r'if \(!g_open\) \{([^\n]*)\}', fm)
    if not closed or not all(x in closed.group(1) for x in ('osDownFrames = ', 'pressesSent = ', 'sent[')):
        out.append('feedMouse keeps its counters or queued states while the menu is closed')
    # the log line counts the two mouse state sizes apart
    line = re.search(r'logf\("menu input:(.*?)\);', fm, re.S)
    if not line or 'of 16 bytes' not in line.group(1) or 'of 20' not in line.group(1):
        out.append('the menu input line does not count 16 and 20 byte state reads apart')
    return out

found = problems(source)
if found:
    for p in found:
        print('FAIL: ' + p)
    sys.exit(1)

def move_zero_into_mouse_branch(s):
    s2 = s.replace('    memset(data, 0, cb);', '', 1)
    return s2.replace('        std::lock_guard<std::mutex> lk(g_inMx);\n        if (cb == sizeof(DIMOUSESTATE)',
                      '        memset(data, 0, cb);\n        std::lock_guard<std::mutex> lk(g_inMx);\n        if (cb == sizeof(DIMOUSESTATE)', 1)

mutants = [
    ('getState zeroes only mouse reads', move_zero_into_mouse_branch),
    ('no keyboard device', lambda s: s.replace('{ &GUID_SysMouse, &GUID_SysKeyboard }', '{ &GUID_SysMouse }', 1)),
    ('a method hooked twice', lambda s: s.replace('for (int i = 0; i < n; ++i) if (at[i] == target) return;', '', 1)),
    ('no latch on closed reads', lambda s: s.replace(' g_keyLatch.hide((unsigned char*)data);', ' {}', 1)),
    ('close does not latch keys', lambda s: s.replace('g_keyLatch.latchAll();', '', 1)),
    ('keyboard events all consumed', lambda s: s.replace('*n = pad::keepReleases((unsigned char*)od, *n, cb);', '*n = 0;', 1)),
    ('feedMouse reads the clock before the copy', lambda s: re.sub(r'\bnow = GetTickCount64\(\)', 'now = g_diMouseTick', s, count=1)),
    ('feedMouse keeps counters while closed', lambda s: s.replace('osDownFrames = ', 'osDownFramez = ', 1)),
    ('one counter for both sizes', lambda s: s.replace('of 16 bytes', 'of 16 bitez', 1)),
]
for name, mutate in mutants:
    broken = mutate(source)
    if broken == source:
        print('FAIL: mutant "%s" did not change menu.cpp: the check has drifted from the code' % name)
        sys.exit(1)
    if not problems(broken):
        print('FAIL: the checks pass on a copy where %s' % name)
        sys.exit(1)
print('menu hooks: checks ok, and each fails on its broken copy (%d)' % len(mutants))
EOF
echo "PASS: menu hooks"
