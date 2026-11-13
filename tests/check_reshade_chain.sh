#!/bin/bash
# Checks loading ReShade underneath Lumenveil (src/reshade_chain.h, main.cpp, present12.cpp): builds
# tests/reshade_chain_test.cpp with the host compiler and runs it, then checks that DllMain asks the decision before any
# new hook or load, that the factory and device hooks sit underneath ReShade's, that the environment variable is put
# back after the load, and that each of those checks fails on a broken copy.
# Usage: tests/check_reshade_chain.sh [source folder holding reshade_chain.h, main.cpp and present12.cpp]   (default: src)
here=$(cd "$(dirname "$0")" && pwd)
src=$(cd "${1:-$here/../src}" && pwd) || exit 1
T=$(mktemp -d) || exit 1
trap 'rm -rf "$T"' EXIT

g++ -std=c++17 -O2 -Wall -Wextra -Werror -I "$src" -o "$T/reshade_chain_test" "$here/reshade_chain_test.cpp" || { echo "FAIL: reshade_chain_test did not build"; exit 1; }
"$T/reshade_chain_test" || { echo "FAIL: reshade_chain_test"; exit 1; }

python3 -I - "$src/main.cpp" "$src/present12.cpp" <<'PY' || exit 1
import re, sys
main = open(sys.argv[1], encoding='utf-8', errors='replace').read().replace('\r\n', '\n')
present = open(sys.argv[2], encoding='utf-8', errors='replace').read().replace('\r\n', '\n')

def bodies(src):
    return {n: re.sub(r'//[^\n]*', '', b) for n, b in re.findall(r'^\S[^\n;]*?\b(\w+)\([^\n]*\{\n(.*?)^\}', src, re.M | re.S)}

def order(body, *marks):
    at = -1
    for m in marks:
        i = body.find(m, at + 1)
        if i < 0: return False
        at = i
    return True

ENV = 'L"RESHADE_DISABLE_LOADING_CHECK"'

def problems(m, p):
    out, b, pb = [], bodies(m), bodies(p)
    need = ('DllMain', 'reshadeLoad', 'calledByGame', 'onPresent', 'CreateDXGIFactory', 'CreateDXGIFactory1', 'CreateDXGIFactory2')
    if [n for n in need if n not in b] or 'p12Init' not in pb: return ['a function was not found: ' + ', '.join(n for n in need if n not in b)]
    if not order(b['DllMain'], 'init();', 'reshadeLoad();'): out.append('DllMain does not call reshadeLoad() after init()')
    r = b['reshadeLoad']
    # nothing new happens without ReShade: the decision first, and its skip returns before any hook or load
    if not re.search(r'if \(!d\.load\) \{[^}]*return;', r): out.append('reshadeLoad does not return when the decision says skip')
    skip = r.find('if (!d.load)')
    if skip < 0 or not order(r, 'reshade_chain::decide(', 'if (!d.load)'): out.append('reshadeLoad does not ask the decision first')
    for call in ('MH_CreateHook', 'MH_EnableHook', 'LoadLibraryW(', 'SetEnvironmentVariableW('):
        if r.find(call) < skip: out.append(call + ' comes before the decision (or is missing)')
    if not order(r, 'MH_EnableHook', 'LoadLibraryW('): out.append('ReShade is loaded before the pass-through hooks are on')
    # the variable is set for the load only, then put back
    if not order(r, 'SetEnvironmentVariableW(' + ENV + ', L"1")', 'LoadLibraryW(', 'SetEnvironmentVariableW(' + ENV + ', '):
        out.append('the environment variable is not set before the load and put back after it')
    # a failed load leaves no pass-through hook behind
    if not re.search(r'if \(!mod\) \{[^}]*MH_RemoveHook', r): out.append('a failed load leaves the pass-through hooks in place')
    if 'g_createDevice12 = ' not in r: out.append('reshadeLoad does not hand the unwrapped device call to present12.cpp')
    # with ReShade the exports' factories are ReShade's: only the pass-through hooks see real ones
    for n in ('CreateDXGIFactory', 'CreateDXGIFactory1', 'CreateDXGIFactory2'):
        if not re.search(r'if \(SUCCEEDED\(hr\) && !g_reshade\) hookFactory\(', b[n]): out.append(n + ' hooks the factory it returns while ReShade is loaded')
    if not [n for n, v in b.items() if n.startswith('bypassFactory') and 'hookFactory(' in v]: out.append('no pass-through factory hook hooks the real factory')
    if 'm == g_reshade' not in b['calledByGame']: out.append('calledByGame does not take calls from ReShade as the game\'s')
    if not re.search(r'if \(home && !homeDown[^;]*g_reshade', b['onPresent']): out.append('the Home key still toggles DLSS while ReShade is loaded')
    # our D3D12 device is created underneath ReShade's hook when ReShade is loaded
    if not re.search(r'if \(g_createDevice12\)\s*hr = g_createDevice12\(', pb['p12Init']): out.append('p12Init does not create its device past ReShade')
    return out

got = problems(main, present)
if got:
    for x in got: print('FAIL:', x)
    sys.exit(1)
broken = {
    'no reshadeLoad call': (main.replace('        reshadeLoad();\n', '', 1), present),
    'hook before the decision': (main.replace('    const auto d = reshade_chain::decide(', '    MH_CreateHook(nullptr, nullptr, nullptr);\n    const auto d = reshade_chain::decide(', 1), present),
    'load before the decision': (main.replace('    const auto d = reshade_chain::decide(', '    LoadLibraryW(L"x");\n    const auto d = reshade_chain::decide(', 1), present),
    'no return on skip': (main.replace('if (!d.load) {', 'if (!d.load) { (void)0; } {', 1), present),
    'variable left set': (main.replace('    SetEnvironmentVariableW(' + ENV + ', had', '    (void)(had', 1), present),
    'hooks left after a failed load': (main.replace('if (!mod) {\n        for (const Hook& h : hooks) { MH_DisableHook(h.target); MH_RemoveHook(h.target); }', 'if (!mod) {', 1), present),
    'export hooks ReShade\'s factory': (main.replace('if (SUCCEEDED(hr) && !g_reshade) hookFactory(', 'if (SUCCEEDED(hr)) hookFactory(', 1), present),
    'ReShade not the game': (main.replace('m == g_reshade', 'm == g_exe', 1), present),
    'Home toggles': (main.replace('if (home && !homeDown && g_reshade)', 'if (false)', 1), present),
    'device wrapped': (main, present.replace('if (g_createDevice12)', 'if (false)', 1)),
}
for name, (m, p) in broken.items():
    if (m, p) == (main, present): print('FAIL: could not build the broken copy:', name); sys.exit(1)
    if not problems(m, p): print('FAIL: check passed on the broken copy:', name); sys.exit(1)
print('ReShade loading checks passed (and fail on %d broken copies)' % len(broken))
PY
