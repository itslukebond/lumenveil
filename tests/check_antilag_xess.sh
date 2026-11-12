#!/bin/bash
# Checks that AMD Radeon Anti-Lag 2 is switched off before Intel's frame generation swap chain takes over in
# fitSwapchain (the game's format change), so it is never on while XeLL runs. The check must fail on a broken copy.
# Usage: tests/check_antilag_xess.sh [source folder holding present12.cpp]   (default: src)
here=$(cd "$(dirname "$0")" && pwd)
src=$(cd "${1:-$here/../src}" && pwd) || exit 1
python3 -I - "$src/present12.cpp" <<'PY' || exit 1
import re, sys
present = open(sys.argv[1], encoding='utf-8', errors='replace').read().replace('\r\n', '\n')

def body(src, name):
    for n, b in re.findall(r'^\S[^\n;]*?\b(\w+)\([^\n]*\{\n(.*?)^\}', src, re.M | re.S):
        if n == name: return re.sub(r'//[^\n]*', '', b)
    return None

def problems(p):
    b = body(p, 'fitSwapchain')
    if b is None: return ['fitSwapchain not found']
    off = re.search(r'if \(want && g_al2\.m_enabled\)[^;]*AMD::AntiLag2DX12::Update\(&g_al2, false, 0\)', b)
    if not off: return ['fitSwapchain does not switch an enabled Anti-Lag 2 off when Intel\'s swap chain is wanted']
    out = []
    for later in ('xessFgSwapchain(', 'xessFgFrameStart('):
        i = b.find(later)
        if i < 0: out.append('fitSwapchain no longer calls ' + later)
        elif i < off.start(): out.append('Anti-Lag 2 is switched off after ' + later)
    return out

got = problems(present)
if got:
    for x in got: print('FAIL:', x)
    sys.exit(1)
fb = body(present, 'fitSwapchain')
m = re.search(r'[^\n]*if \(want && g_al2\.m_enabled\)[^\n]*\n', fb)
line = m.group(0)
broken = {
    'no switch off': present.replace(line.strip(), '', 1),
    'only when not wanted': present.replace('if (want && g_al2.m_enabled)', 'if (!want && g_al2.m_enabled)', 1),
    'switched off after XeLL starts': present.replace(line.strip(), '', 1).replace(
        '    if (g_colorSpace >= 0) g_sc->SetColorSpace1', '    ' + line.strip() + '\n    if (g_colorSpace >= 0) g_sc->SetColorSpace1', 1),
}
for name, p in broken.items():
    if p == present: print('FAIL: could not build the broken copy:', name); sys.exit(1)
    if not problems(p): print('FAIL: check passed on the broken copy:', name); sys.exit(1)
print("Anti-Lag 2 is off before Intel's swap chain (and the check fails on %d broken copies)" % len(broken))
PY
