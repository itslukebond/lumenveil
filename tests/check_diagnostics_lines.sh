#!/bin/bash
# Checks that the diagnostics log lines themselves are written, in their words, where tests/check_diagnostics.sh pins
# only the logic around them: the unsupported line with its reason and Streamline's number (present12.cpp
# fgSupportCheck), the once-per-launch no-frames line (fgAfterPresent), the two not-taken lines (main.cpp noteNotTaken)
# and the present=0 line (notePresentOff). Each check must fail on a broken copy.
# Usage: tests/check_diagnostics_lines.sh [source folder]   (default: src next to tests)
here=$(cd "$(dirname "$0")" && pwd)
src=$(cd "${1:-$here/../src}" && pwd) || exit 1

python3 -I - "$src/present12.cpp" "$src/main.cpp" <<'PY' || exit 1
import re, sys
read = lambda p: open(p, encoding='utf-8', errors='replace').read().replace('\r\n', '\n')
p12, main = read(sys.argv[1]), read(sys.argv[2])

def body(src, name):
    m = re.search(r'^\S[^;{}]*?\b' + name + r'\([^;{}]*\{\n(.*?)^\}', src, re.M | re.S)
    return re.sub(r'//[^\n]*', '', m.group(1)) if m else None

UNSUP = 'else if (const char* why = fg_support::reason(int(r))) logf("fg: DLSS frame generation not supported: %s (Streamline result %d)", why, int(r));'
UNSUP_N = 'else logf("fg: DLSS frame generation not supported (Streamline result %d)", int(r));'
NONE = 'logf("fg: DLSS frame generation is on but generated no frames in the last %u", g_rendered);'
SYS = 'if (k == swapchain_caller::Mod::kSystem) logf("dxgi: swap chain from %ls not taken (called by a system module), passed through", name);'
ANON = 'else logf("dxgi: swap chain from %p not taken (called from code in no module, often another program\'s overlay), passed through", ret);'
OFF = 'if (!told.exchange(true)) logf("d3d12: present=0 in ffxv_dlss.ini, the game keeps its own swap chain: no menu or frame generation");'

def problems(p12, main):
    out = []
    sup, after, note, off = body(p12, 'fgSupportCheck'), body(p12, 'fgAfterPresent'), body(main, 'noteNotTaken'), body(main, 'notePresentOff')
    if None in (sup, after, note, off): return ['a function under check was not found']
    if UNSUP not in sup: out.append('the unsupported line with its reason and number is not written')
    if UNSUP_N not in sup: out.append('the unsupported line without words is not written')
    once = re.search(r'if \(!toldNone && fg_support::noneGenerated\(g_onFrames, g_presented, g_rendered\)\) \{\s*toldNone = true;(.*?)\n        \}', after, re.S)
    if not once or NONE not in once.group(1): out.append('the no-frames line is not written when the window qualifies')
    if SYS not in note: out.append('the not-taken line for a system module is not written')
    if ANON not in note: out.append('the not-taken line for code in no module is not written')
    if note.find(SYS) >= 0 and note.find('if (logged.fetch_add(1) >= 8) return;') > note.find(SYS): out.append('the not-taken lines come before the cap')
    if OFF not in off: out.append('the present=0 line is not written in its words')
    return out

p = problems(p12, main)
if p:
    for x in p: print('FAIL:', x)
    sys.exit(1)
broken = {
    'unsupported silent': (p12.replace(UNSUP, 'else if (const char* why = fg_support::reason(int(r))) (void)why;', 1), main),
    'unsupported number silent': (p12.replace(UNSUP_N, 'else {}', 1), main),
    'no-frames silent': (p12.replace(NONE, '', 1), main),
    'system caller silent': (p12, main.replace(SYS, 'if (k == swapchain_caller::Mod::kSystem) (void)name;', 1)),
    'anonymous caller silent': (p12, main.replace(ANON, 'else (void)ret;', 1)),
    'present=0 any words': (p12, main.replace(OFF, 'if (!told.exchange(true)) logf("d3d12: off");', 1)),
}
for name, (a, b) in broken.items():
    if (a, b) == (p12, main): print('FAIL: could not build the broken copy:', name); sys.exit(1)
    if not problems(a, b): print('FAIL: check passed on the broken copy:', name); sys.exit(1)
print('diagnostics line checks passed (and fail on %d broken copies)' % len(broken))
PY
echo "PASS: diagnostics lines"
