#!/bin/bash
# Checks that a resize of the game's own swap chain natively refreshes the output encoding (src/present12.cpp,
# hk_ResizeBuffers): after the game's call succeeds on the native swap chain, refreshEncoding runs on it before the
# hook returns, so the menu's next frame is encoded for the new format (an HDR toggle changes it). Must fail on broken
# copies.
# Usage: tests/check_native_resize.sh [source folder]   (default: src next to tests)
here=$(cd "$(dirname "$0")" && pwd)
src=$(cd "${1:-$here/../src}" && pwd) || exit 1
python3 -I - "$src/present12.cpp" <<'PY' || exit 1
import re, sys
p12 = open(sys.argv[1], encoding='utf-8', newline='').read().replace('\r\n', '\n')

def problems(s):
    i = s.find('HRESULT STDMETHODCALLTYPE hk_ResizeBuffers(')
    if i < 0: return ['no hk_ResizeBuffers']
    j = s.find('if (sc != g_game)', i)
    if j < 0: return ['no non-native part in hk_ResizeBuffers']
    nat = s[s.index('{', i) + 1:j]
    if 'if (g_native)' not in nat: return ['no native branch first in hk_ResizeBuffers']
    call = nat.find('o_ResizeBuffers(')
    m = re.search(r'if \(SUCCEEDED\((\w+)\) && sc == g_nativeSc\) refreshEncoding\(sc\);', nat)
    out = []
    if call < 0: out.append("the native branch does not call the game's ResizeBuffers")
    if not m: out.append('the native branch does not refresh the encoding after a successful resize of the native swap chain')
    elif call >= 0:
        if m.start() < call: out.append('the encoding is refreshed before the resize')
        if not re.search(r'HRESULT %s = o_ResizeBuffers\(' % m.group(1), nat): out.append('the refresh does not test the resize result')
        if not re.search(r'refreshEncoding\(sc\);[^\n]*\n\s*return %s;' % m.group(1), nat): out.append("the game's result is not returned after the refresh")
    return out

p = problems(p12)
if p: print('FAIL: ' + '; '.join(p)); sys.exit(1)
line = re.search(r'\n(\s*if \(SUCCEEDED\(\w+\) && sc == g_nativeSc\) refreshEncoding\(sc\);[^\n]*)', p12).group(1)
broken = {
    'no refresh': p12.replace(line, '', 1),
    'refresh on failure too': p12.replace(line, line.replace('SUCCEEDED(', '(void)(', 1), 1),
    'refresh on any swap chain': p12.replace(line, line.replace(' && sc == g_nativeSc', '', 1), 1),
}
for name, b in broken.items():
    if b == p12: print('FAIL: could not build the broken copy:', name); sys.exit(1)
    if not problems(b): print('FAIL: check passed on the broken copy:', name); sys.exit(1)
print('native resize checks passed (and fail on %d broken copies)' % len(broken))
PY
echo "PASS: native resize"
