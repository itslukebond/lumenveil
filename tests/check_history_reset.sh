#!/bin/bash
# Checks the upscaler history reset: builds tests/history_reset_test.cpp (src/history_rules.h) with the host compiler
# and runs it, then checks dlss.cpp and main.cpp: a run is marked only after the upscaler succeeded, each game Present
# ends the frame and sets the reset after a frame without a run, FSR and XeSS still get the reset, and the motion
# pass writes zero motion (and the depth) while the game is paused. Each source check must fail on a broken copy.
# Usage: tests/check_history_reset.sh [source folder holding history_rules.h, dlss.cpp and main.cpp]   (default: src)
here=$(cd "$(dirname "$0")" && pwd)
src=$(cd "${1:-$here/../src}" && pwd) || exit 1
T=$(mktemp -d) || exit 1
trap 'rm -rf "$T"' EXIT
g++ -std=c++17 -O2 -Wall -Wextra -Werror -I "$src" -o "$T/history_reset_test" "$here/history_reset_test.cpp" || { echo "FAIL: history_reset_test did not build"; exit 1; }
"$T/history_reset_test" || { echo "FAIL: history_reset_test"; exit 1; }

python3 -I - "$src/dlss.cpp" "$src/main.cpp" <<'PY' || exit 1
import re, sys
dlss = open(sys.argv[1], encoding='utf-8', errors='replace').read().replace('\r\n', '\n')
main = re.sub(r'//[^\n]*', '', open(sys.argv[2], encoding='utf-8', errors='replace').read().replace('\r\n', '\n'))
m = re.search(r'kMvHlsl = R"\((.*?)\)";', dlss, re.S)
hlsl = m.group(1) if m else ''
cpp = re.sub(r'//[^\n]*', '', dlss.replace(hlsl, '') if hlsl else dlss)
bodies = lambda s: {n: b for n, b in re.findall(r'^\S[^\n;]*?\b(\w+)\([^\n]*\{\n(.*?)^\}', s, re.M | re.S)}
db, mb = bodies(cpp), bodies(main)
out = []
replace, endf = db.get('dlssReplaceTaa'), None
if replace is None: out.append('dlssReplaceTaa not found')
else:
    if not re.search(r'if \(ok\) \{ g_fg = \{[^\n]*\}; g_fgReady = true; g_hist\.upscaled\(\); \}', replace):
        out.append('dlssReplaceTaa: the run is not marked inside the success branch beside g_fgReady')
    if replace.count('g_hist.upscaled()') != 1: out.append('dlssReplaceTaa: the run is marked more than once or not at all')
    if 'const bool reset = g_reset.exchange(false);' not in replace or 'ev.InReset = reset ? 1 : 0;' not in replace:
        out.append('dlssReplaceTaa: the reset is not taken once into ev.InReset')
    if not re.search(r'\(which == 1 \? fsrUpscale : xessUpscale\)\([^;]*ev\.InReset != 0\)', replace, re.S):
        out.append('dlssReplaceTaa: FSR and XeSS no longer get the reset')
if not re.search(r'^void dlssEndFrame\(\) \{ if \(g_hist\.endFrame\(\)\) g_reset = true; \}', cpp, re.M):
    out.append('dlssEndFrame: does not set g_reset from g_hist.endFrame()')
op = mb.get('onPresent')
if op is None or not re.search(r'\+\+g_frame;\s*dlssEndFrame\(\);', op): out.append('onPresent: dlssEndFrame() not called right after ++g_frame')
if main.count('dlssEndFrame()') != 1: out.append('main.cpp: dlssEndFrame() called somewhere other than onPresent')
if re.search(r'gamePaused', cpp + main): out.append('C++ reads gamePaused (pausing must not reset the history)')
# the motion pass
if not hlsl: out.append('kMvHlsl not found')
else:
    if not re.search(r'float d = depthIn\[id\];\s*if \(g_gamePaused == 0\) \{ mvOut\[id\] = 0; depthOut\[id\] = d; return; \}', hlsl):
        out.append('kMvHlsl: no paused branch writing zero motion and the depth before returning')
    if not re.search(r'if \(vel\.y == 1\.0\) \{', hlsl): out.append('kMvHlsl: camera reprojection is not chosen by vel.y == 1.0 alone')
    if hlsl.count('g_gamePaused') != 2: out.append('kMvHlsl: g_gamePaused used other than in the paused branch')
if out:
    for o in out: print('FAIL:', o)
    sys.exit(1)
PY
echo "check_history_reset: all passed"
