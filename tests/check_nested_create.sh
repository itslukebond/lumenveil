#!/bin/bash
# Checks that a swap chain creation the system DXGI makes inside one of ours (DXVK's CreateSwapChain calls
# CreateSwapChainForHwnd) goes straight to the original function, before it is classified or can reset the outer call's
# native-presentation decision: otherwise a native launch under Proton loses its menu and counter (src/main.cpp,
# hk_CreateSC and hk_CreateSCHwnd). Each check must fail on a broken copy.
# Usage: tests/check_nested_create.sh [source folder]   (default: src next to tests)
here=$(cd "$(dirname "$0")" && pwd)
src=$(cd "${1:-$here/../src}" && pwd) || exit 1

python3 -I - "$src/main.cpp" <<'PY' || exit 1
import re, sys
main = open(sys.argv[1], encoding='utf-8', errors='replace').read().replace('\r\n', '\n')

def body(src, name):
    m = re.search(r'^\S[^;{}]*?\b' + name + r'\([^;{}]*\{\n(.*?)^\}', src, re.M | re.S)
    return re.sub(r'//[^\n]*', '', m.group(1)) if m else None

HOOKS = (('hk_CreateSC', 'if (s_createDepth) return o_CreateSC(f, d, desc, sc);'),
         ('hk_CreateSCHwnd', 'if (s_createDepth) return o_CreateSCHwnd(f, d, w, desc, fs, o, sc);'))

def problems(main):
    out = []
    for name, guard in HOOKS:
        b = body(main, name)
        if b is None: out.append(name + ' not found'); continue
        g, take, native = b.find(guard), b.find('takeAsGame('), b.find('s_nativeCreate =')
        if g < 0: out.append(name + ' does not pass a nested creation straight through'); continue
        if take < 0 or native < 0: out.append(name + ': its decision lines were not found'); continue
        if not (g < take and g < native): out.append(name + ' classifies or decides before passing a nested creation through')
    return out

p = problems(main)
if p:
    for x in p: print('FAIL:', x)
    sys.exit(1)
sc, hw = HOOKS[0][1], HOOKS[1][1]
broken = {
    'CreateSwapChain unguarded': main.replace(sc, '', 1),
    'CreateSwapChainForHwnd unguarded': main.replace(hw, '', 1),
    'guard after the decision': main.replace(hw, '', 1).replace('    s_nativeCreate = game && desc && p12Native();\n    s_nativeTear = s_nativeCreate && p12NativeTearing() && flipModel(nd.SwapEffect);',
                                                                  '    s_nativeCreate = game && desc && p12Native();\n    ' + hw + '\n    s_nativeTear = s_nativeCreate && p12NativeTearing() && flipModel(nd.SwapEffect);', 1),
}
for name, b in broken.items():
    if b == main: print('FAIL: could not build the broken copy:', name); sys.exit(1)
    if not problems(b): print('FAIL: check passed on the broken copy:', name); sys.exit(1)
print('nested creation checks passed (and fail on %d broken copies)' % len(broken))
PY
echo "PASS: nested create"
