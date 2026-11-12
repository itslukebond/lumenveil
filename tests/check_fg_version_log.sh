#!/bin/bash
# Checks that the log names the frame generation file that runs (nvngx_dlssg.dll, its folder and version) whether or not
# the Insert menu is ever opened. dlssgVersion() writes that line the first time it finds the file loaded; the menu
# calls it only while open, so the frame generation path (fgAfterPresent in src/present12.cpp) must call it too.
# Each check must fail on a broken copy.
# Usage: tests/check_fg_version_log.sh [source folder holding present12.cpp]   (default: src next to tests)
here=$(cd "$(dirname "$0")" && pwd)
src=$(cd "${1:-$here/../src}" && pwd) || exit 1

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

def problems(src):
    b = body(src, 'unsigned fgAfterPresent()')
    if b is None: return ['fgAfterPresent() not found']
    code = re.sub(r'//[^\n]*', '', b)
    if not re.search(r'\bdlssgVersion\(\)', code): return ['fgAfterPresent() never calls dlssgVersion()']
    return []

p = problems(text)
if p:
    for x in p: print('FAIL:', x)
    sys.exit(1)
b = body(text, 'unsigned fgAfterPresent()')
broken = {
    'call removed': text.replace(b, re.sub(r'\bdlssgVersion\(\)', 'dlssgVersionX()', b), 1),
    'call commented out': text.replace(b, re.sub(r'([^\n]*\bdlssgVersion\(\))', r'// \1', b), 1),
}
for name, x in broken.items():
    if x == text: print('FAIL: could not build the broken copy:', name); sys.exit(1)
    if not problems(x): print('FAIL: check passed on the broken copy:', name); sys.exit(1)
print('present12.cpp frame generation version log checks passed (and fail on 2 broken copies)')
PY
