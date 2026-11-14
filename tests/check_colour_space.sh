#!/bin/bash
# Checks the colour space our swap chain presents in after the game changes format (GitHub #5, a washed-out picture on
# Windows): builds tests/colour_space_test.cpp with the host compiler and runs it, then checks in present12.cpp that
# fitSwapchain sets the colour space after an in-place resize that changes the format (Windows) and after a
# replacement, never after a same-format resize, that setColourSpace sets the space it chose (under Wine only the game's
# own, as in 1.0.1), and that reattach forgets the old swap chain's colour space.
# Usage: tests/check_colour_space.sh [source folder holding colour_space.h and present12.cpp]   (default: src next to tests)
here=$(cd "$(dirname "$0")" && pwd)
src=$(cd "${1:-$here/../src}" && pwd) || exit 1
T=$(mktemp -d) || exit 1
trap 'rm -rf "$T"' EXIT

g++ -std=c++17 -O2 -Wall -Wextra -Werror -I "$src" -o "$T/colour_space_test" "$here/colour_space_test.cpp" || { echo "FAIL: colour_space_test did not build"; exit 1; }
"$T/colour_space_test" || { echo "FAIL: colour_space_test"; exit 1; }

python3 -I - "$src/present12.cpp" <<'PY'
import re, sys
text = open(sys.argv[1], encoding='utf-8', errors='replace').read().replace('\r\n', '\n')

def body(src, head):
    i = src.find(head)
    if i < 0: return None
    i = src.find('{', i)
    depth = 0
    for j in range(i, len(src)):
        depth += {'{': 1, '}': -1}.get(src[j], 0)
        if depth == 0: return src[i:j + 1]
    return None

out = []
fit = body(text, 'HRESULT fitSwapchain(')
if fit is None: out.append('fitSwapchain not found')
else:
    m = re.search(r'if \(want == g_xessSc\) \{(.*?)\n    \}', fit, re.S)
    if not m: out.append('fitSwapchain: the in-place resize is not a block')
    elif not re.search(r'if \(SUCCEEDED\(hr\) && format != oldFormat && !g_wine\) setColourSpace\(format\);', m.group(1)):
        out.append('fitSwapchain: the in-place resize does not set the colour space when the format changes (Windows)')
    tail = fit[fit.find('swapchain replaced by'):]
    if 'setColourSpace(format);' not in tail: out.append('fitSwapchain: a replaced swap chain does not set the colour space')
    if 'g_sc->SetColorSpace1(' in fit: out.append('fitSwapchain sets a colour space outside setColourSpace()')
sc = body(text, 'static void setColourSpace(')
if sc is None: out.append('setColourSpace not found')
else:
    if 'colour::presentColourSpace(' not in sc or 'CheckColorSpaceSupport(' not in sc:
        out.append('setColourSpace does not use presentColourSpace with CheckColorSpaceSupport')
    win = sc[sc.find('UINT support'):] if 'UINT support' in sc else ''
    if not re.search(r'g_sc->SetColorSpace1\(DXGI_COLOR_SPACE_TYPE\(cs\)\)', win):
        out.append('setColourSpace does not set the colour space it chose')
    if not re.search(r'if \(g_wine\) \{\s*if \(g_colorSpace >= 0\) g_sc->SetColorSpace1\(DXGI_COLOR_SPACE_TYPE\(g_colorSpace\)\);\s*return;\s*\}', sc):
        out.append('setColourSpace under Wine does not keep the 1.0.1 behaviour (only the game\'s own colour space)')
ra = body(text, 'HWND reattach(')
if ra is None or 'g_colorSpace = -1;' not in ra: out.append('reattach does not forget the old colour space')
for p in out: print('FAIL:', p)
sys.exit(1 if out else 0)
PY
status=$?
[ $status -eq 0 ] && echo "PASS: colour space" || echo "FAIL: colour space"
exit $status
