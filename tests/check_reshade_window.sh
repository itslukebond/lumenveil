#!/bin/bash
# Checks two things about loading ReShade underneath Lumenveil (main.cpp): while ReShade is loaded, the game's swapchain
# reports the game's own window from GetDesc (ReShade takes its input window from there, and the swapchain itself sits
# on the hidden window), and the file loaded is the kept dxgi.dll.before_ffxv_dlss. Each check must fail on a broken copy.
# Usage: tests/check_reshade_window.sh [source folder holding main.cpp]   (default: src)
here=$(cd "$(dirname "$0")" && pwd)
src=$(cd "${1:-$here/../src}" && pwd) || exit 1
python3 -I - "$src/main.cpp" <<'PY' || exit 1
import re, sys
main = open(sys.argv[1], encoding='utf-8', errors='replace').read().replace('\r\n', '\n')

def bodies(src):
    return {n: re.sub(r'//[^\n]*', '', b) for n, b in re.findall(r'^\S[^\n;]*?\b(\w+)\([^\n]*\{\n(.*?)^\}', src, re.M | re.S)}

def problems(m):
    out, b = [], bodies(m)
    if 'hookSwapChain' not in b or 'reshadeLoad' not in b: return ['hookSwapChain or reshadeLoad not found']
    # the GetDesc hook goes on only while ReShade is loaded (no new hook without ReShade)
    if not re.search(r'if \(g_reshade\)[^;]*MH_CreateHook\(vt\[12\], \(void\*\)hk_GetDesc, \(void\*\*\)&o_GetDesc\)', b['hookSwapChain']):
        out.append('hookSwapChain does not hook GetDesc (slot 12) only while ReShade is loaded')
    g = b.get('hk_GetDesc')
    if g is None: out.append('hk_GetDesc not found')
    else:
        if not re.search(r'HRESULT hr = o_GetDesc\(sc, d\);', g): out.append('hk_GetDesc does not call the original first')
        if not re.search(r'if \(SUCCEEDED\(hr\) && d && p12IsGame\(sc\)[^)]*\)[^;]*d->OutputWindow = ', g):
            out.append("hk_GetDesc changes the window of swapchains other than the game's, or on failure")
        if 'p12GameWindow()' not in g: out.append("hk_GetDesc does not report the game's own window")
    # the file loaded is the kept one
    r = b['reshadeLoad']
    if 'const std::wstring kept = path(L"dxgi.dll.before_ffxv_dlss");' not in r: out.append('reshadeLoad does not use dxgi.dll.before_ffxv_dlss')
    if 'LoadLibraryW(kept.c_str())' not in r: out.append('reshadeLoad loads something other than the kept file')
    return out

got = problems(main)
if got:
    for x in got: print('FAIL:', x)
    sys.exit(1)
broken = {
    'no GetDesc hook': main.replace('MH_CreateHook(vt[12], (void*)hk_GetDesc, (void**)&o_GetDesc)', 'MH_CreateHook(vt[13], (void*)hk_GetDesc, (void**)&o_GetDesc)', 1),
    'hooked without ReShade': main.replace('if (g_reshade) MH_CreateHook(vt[12]', 'MH_CreateHook(vt[12]', 1),
    'every swapchain': main.replace('&& p12IsGame(sc)', '', 1),
    'hidden window kept': main.replace('p12GameWindow()', 'd->OutputWindow', 1),
    'another file loaded': main.replace('LoadLibraryW(kept.c_str())', 'LoadLibraryW(L"ReShade64.dll")', 1),
}
for name, m in broken.items():
    if m == main: print('FAIL: could not build the broken copy:', name); sys.exit(1)
    if not problems(m): print('FAIL: check passed on the broken copy:', name); sys.exit(1)
print("ReShade window and load path checks passed (and fail on %d broken copies)" % len(broken))
PY
