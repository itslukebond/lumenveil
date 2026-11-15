#!/bin/bash
# Checks the VSync fix: builds tests/vsync_test.cpp (src/vsync_rules.h) with the host compiler and runs it, then checks
# present12.cpp: our swap chain always presents at interval 0, the tearing flag only when the game asked for interval
# 0, the game's VSync request recorded and logged, the cap rules used by both limiters, our own pacing run whenever
# the game asks for VSync and Reflex is not in charge, and the stall watchdog started and logging. Each source check
# must fail on a broken copy.
# Usage: tests/check_vsync.sh [source folder holding vsync_rules.h, present12.cpp and menu.cpp]   (default: src)
here=$(cd "$(dirname "$0")" && pwd)
src=$(cd "${1:-$here/../src}" && pwd) || exit 1
T=$(mktemp -d) || exit 1
trap 'rm -rf "$T"' EXIT
g++ -std=c++17 -O2 -Wall -Wextra -Werror -I "$src" -o "$T/vsync_test" "$here/vsync_test.cpp" || { echo "FAIL: vsync_test did not build"; exit 1; }
"$T/vsync_test" || { echo "FAIL: vsync_test"; exit 1; }

python3 -I - "$src/present12.cpp" "$src/menu.cpp" <<'PY' || exit 1
import re, sys
src = open(sys.argv[1], encoding='utf-8', errors='replace').read().replace('\r\n', '\n')
menu = re.sub(r'//[^\n]*', '', open(sys.argv[2], encoding='utf-8', errors='replace').read().replace('\r\n', '\n'))
code = re.sub(r'//[^\n]*', '', src)
bodies = {n: b for n, b in re.findall(r'^\S[^\n;]*?\b(\w+)\([^\n]*\{\n(.*?)^\}', code, re.M | re.S)}
out = []
present, init = bodies.get('p12Present'), bodies.get('p12Init')
reflex, pace, resize = bodies.get('setReflex'), bodies.get('fsrPace'), bodies.get('hk_ResizeBuffers')
status = bodies.get('fgStatus')
if None in (present, init, reflex, pace, resize, status):
    print('FAIL: p12Present, p12Init, setReflex, fsrPace, hk_ResizeBuffers or fgStatus not found'); sys.exit(1)
lines = [l.strip() for l in present.split('\n')]
def line(text): return text in lines
def order(body, *marks):
    at = -1
    for m in marks:
        i = body.find(m, at + 1)
        if i < 0: return False
        at = i
    return True

# the interval and the Present (findings 1, 2)
if not line('const UINT interval = vsyncInterval(si, g_wine, proxyFg());'): out.append('p12Present: interval not taken from vsyncInterval(si, g_wine, proxyFg())')
real = [c for c in re.findall(r'g_sc->Present\(([^;]*)\);', present) if 'DXGI_PRESENT_TEST' not in c]
if len(real) != 1: out.append('p12Present: expected one real Present on our swap chain, found %d' % len(real))
elif not real[0].startswith('interval, si == 0 && g_tearing ? flags | DXGI_PRESENT_ALLOW_TEARING : flags'):
    out.append('p12Present: our Present is not (interval, tearing flag only when the game asked for interval 0)')
if not order(present, 'const UINT interval = vsyncInterval(', 'g_sc->Present(interval,'): out.append('p12Present: interval decided after Present')
# the cap stands in for VSync only where it was dropped; one limiter (finding 5)
if not line('g_wantSync = vsyncByCap(si, g_wine, proxyFg());'): out.append("p12Present: VSync-by-cap not recorded from vsyncByCap(si, g_wine, proxyFg())")
if 'game asked for VSync' not in src: out.append('present12.cpp: no log line saying how VSync is honoured')
for name, b in (('setReflex', reflex), ('fsrPace', pace)):
    m = re.search(r'^\s*const int cap = vsyncCap\(g_set\.limit, g_autoCap, g_wantSync\);\n(.*)', b, re.M | re.S)
    if not m or not re.search(r'\bcap\b', m.group(1)): out.append('%s: cap not taken from, or not using, vsyncCap(g_set.limit, g_autoCap, g_wantSync)' % name)
if not line('if (ownPace(g_fg, proxyFg(), up, g_wantSync)) fsrPace();'): out.append('p12Present: our pacing is not routed by ownPace(g_fg, proxyFg(), up, g_wantSync)')
if not re.search(r'g_wantSync = vsyncByCap\(.*?setReflex\(\).*?g_sc->Present\(interval,', present, re.S): out.append('p12Present: Reflex cap not refreshed when VSync changes')
# the menu's cap row is usable whenever our pacing stands in for VSync (finding 4)
if not re.search(r's\.vsyncPaced\s*=\s*g_wantSync;', status): out.append('fgStatus: vsyncPaced not reported')
if not re.search(r's\.capUsable = st\.available \|\| st\.backend >= 0 \|\| di\.active == 1 \|\| di\.active == 2 \|\| st\.vsyncPaced;', menu): out.append('menu.cpp: Frame cap row not enabled for VSync pacing')
# the watchdog (finding 3, rare R1)
m = re.search(r'if \(!g_watchdog\) \{\s*g_watchdog = true;\s*if \(HANDLE th = CreateThread\(nullptr, 0, stallWatchdog', init)
if not m: out.append('p12Init: stall watchdog not started exactly once')
wd = bodies.get('stallWatchdog')
if wd is None: out.append('stallWatchdog not found')
else:
    if 'StallWatch' not in wd or '.step(' not in wd: out.append('stallWatchdog: does not use StallWatch')
    if 'd3d12: stall' not in wd: out.append('stallWatchdog: no stall line')
    if len(re.findall(r'logf\(.*?\);\s*flushLog\(\);', wd, re.S)) != 2 or wd.count('logf(') != 2: out.append('stallWatchdog: the stall and resume lines are not each flushed')
    if 'SMTO_ABORTIFHUNG' not in wd: out.append('stallWatchdog: does not check whether the game window responds')
if not order(present, 'g_stage = kStagePresent;', 'g_sc->Present(interval,', '++g_presents;', 'g_stage = kStageAfter;', 'fgAfterPresent()', 'refreshEncoding()', 'g_stage = kStagePace;', 'fsrPace()', 'g_stage = kStageAntiLag;', 'AntiLag2DX12::Update', 'g_stage = kStageFg;', 'xessFgFrameStart', 'g_stage = kStageGame;'):
    out.append('p12Present: stages not set before each step after Present')
if not order(resize, 'g_stage = kStageResize;', 'fgSetMode(false)', 'releaseTargets()', 'o_ResizeBuffers(sc, n, w, h, f, fl)', 'g_stage = kStageFit;', 'fitSwapchain('):
    out.append('hk_ResizeBuffers: stage not set before the teardown and both resizes')
if out:
    for o in out: print('FAIL:', o)
    sys.exit(1)
PY
echo "check_vsync: all passed"
