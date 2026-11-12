#!/bin/bash
# Checks the Proton environment settings (src/proton_env.h): builds tests/proton_env_test.cpp with the host compiler and
# runs it, then checks in main.cpp that protonEnv() does nothing unless the Wine check passes and that DllMain calls it
# before it loads the real dxgi.dll, and that each of those checks fails on a broken copy.
# Usage: tests/check_proton_env.sh [source folder holding proton_env.h and main.cpp]   (default: src next to tests)
here=$(cd "$(dirname "$0")" && pwd)
src=$(cd "${1:-$here/../src}" && pwd) || exit 1
T=$(mktemp -d) || exit 1
trap 'rm -rf "$T"' EXIT

g++ -std=c++17 -O2 -Wall -Wextra -Werror -I "$src" -o "$T/proton_env_test" "$here/proton_env_test.cpp" || { echo "FAIL: proton_env_test did not build"; exit 1; }
"$T/proton_env_test" || { echo "FAIL: proton_env_test"; exit 1; }

python3 -I - "$src/main.cpp" <<'EOF'
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
    out = []
    pe = body(src, 'static void protonEnv()')
    if pe is None: return ['protonEnv() not found']
    first = pe[1:].strip().split('\n', 1)[0]
    if not re.fullmatch(r'if \(!GetProcAddress\(GetModuleHandleW\(L"ntdll\.dll"\), "wine_get_version"\)\) return;', first):
        out.append('protonEnv() does not start with the Wine check: ' + first)
    dm = body(src, 'BOOL WINAPI DllMain(')
    if dm is None: return out + ['DllMain not found']
    call, load = dm.find('protonEnv();'), dm.find('g_realDxgi = LoadLibraryW(')
    if call < 0: out.append('DllMain does not call protonEnv()')
    elif load < 0: out.append('DllMain does not load the real dxgi.dll')
    elif call > load: out.append('DllMain calls protonEnv() after it loads the real dxgi.dll')
    return out

p = problems(text)
if p:
    for x in p: print('FAIL:', x)
    sys.exit(1)

# Each check must fail on a copy broken the way it guards against.
broken = {
    'no Wine check': text.replace('    if (!GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "wine_get_version")) return;\n', '', 1),
    'call after load': text.replace('        protonEnv();\n', '', 1).replace('        if (!g_realDxgi) return FALSE;\n', '        if (!g_realDxgi) return FALSE;\n        protonEnv();\n', 1),
    'no call': text.replace('        protonEnv();\n', '', 1),
}
for name, b in broken.items():
    if b == text: print('FAIL: could not build the broken copy:', name); sys.exit(1)
    if not problems(b): print('FAIL: check passed on the broken copy:', name); sys.exit(1)
print('main.cpp checks passed (and fail on 3 broken copies)')
EOF
