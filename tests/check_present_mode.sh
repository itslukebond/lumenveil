#!/bin/bash
# Checks how a launch presents its frames (src/present_mode.h, present12.cpp, main.cpp): builds
# tests/present_mode_test.cpp with the host compiler and runs it (the [d3d12] present matrix, when the native path needs a
# D3D12 device, and the tearing rule), also against broken copies of the header (each must fail); then checks that
# p12Init keeps a native launch native when the game recreates its swap chain, decides the mode from the setting and the
# frame generation backend, and returns natively before any D3D12 device is made unless FSR or XeSS files are installed;
# that both Present hooks take the native path with the game's own interval; and that both creation hooks attach the
# game's swap chain natively and add the tearing flag only on flip-model swap chains. Each source check must fail on a
# broken copy.
# Usage: tests/check_present_mode.sh [source folder]   (default: src next to tests)
here=$(cd "$(dirname "$0")" && pwd)
src=$(cd "${1:-$here/../src}" && pwd) || exit 1
T=$(mktemp -d) || exit 1
trap 'rm -rf "$T"' EXIT

flags=(-std=c++17 -O2 -Wall -Wextra -Werror)
g++ "${flags[@]}" -I "$src" -o "$T/present_mode_test" "$here/present_mode_test.cpp" \
    || { echo "FAIL: present_mode_test did not build"; exit 1; }
"$T/present_mode_test" || { echo "FAIL: present_mode_test"; exit 1; }

# Broken headers: the host test must fail on each (built without -Werror: a broken rule may leave a parameter unused).
python3 -I - "$src/present_mode.h" "$T" <<'PY' || exit 1
import os, sys
s = open(sys.argv[1], encoding='utf-8').read()
broken = {
    'present2': ('if (setting == 2 || backendStarted) return kPresentD3D12;', 'if (backendStarted) return kPresentD3D12;'),
    'backend': ('if (setting == 2 || backendStarted) return kPresentD3D12;', 'if (setting == 2) return kPresentD3D12;'),
    'off': ('if (setting == 0) return kPresentOff;', 'if (setting == 0 && !backendStarted) return kPresentOff;'),
    'xess': ('return fsrFiles || xessFiles;', 'return fsrFiles;'),
    'wine': ('return supported && !wine;', 'return supported;'),
    'interval': ('&& interval == 0 && !refused;', '&& !refused;'),
    'refused': ('&& interval == 0 && !refused;', '&& interval == 0;'),
}
for name, (old, new) in broken.items():
    if s.count(old) != 1: print('FAIL: could not build the broken header:', name); sys.exit(1)
    os.makedirs(os.path.join(sys.argv[2], name))
    open(os.path.join(sys.argv[2], name, 'present_mode.h'), 'w', encoding='utf-8').write(s.replace(old, new))
PY
for b in present2 backend off xess wine interval refused; do
    g++ -std=c++17 -O2 -I "$T/$b" -o "$T/$b/t" "$here/present_mode_test.cpp" || { echo "FAIL: broken header $b did not build"; exit 1; }
    if "$T/$b/t" >/dev/null; then echo "FAIL: present_mode_test passes on the broken header: $b"; exit 1; fi
done

python3 -I - "$src/present12.cpp" "$src/main.cpp" "$src/present_mode.h" <<'PY' || exit 1
import re, sys
read = lambda p: open(p, encoding='utf-8', errors='replace').read().replace('\r\n', '\n')
p12, main, head = read(sys.argv[1]), read(sys.argv[2]), read(sys.argv[3])

def body(src, name):
    m = re.search(r'^\S[^;{}]*?\b' + name + r'\([^;{}]*\{\n(.*?)^\}', src, re.M | re.S)  # signature may span lines
    return re.sub(r'//[^\n]*', '', m.group(1)) if m else None

def problems(p12, main, head):
    out = []
    init, attach = body(p12, 'p12Init'), body(p12, 'p12NativeAttach')
    hp, hp1, np = body(main, 'hk_Present'), body(main, 'hk_Present1'), body(main, 'nativePresent')
    sc, hw, cb = body(main, 'hk_CreateSC'), body(main, 'hk_CreateSCHwnd'), body(main, 'createdBy')
    if None in (init, attach, hp, hp1, np, sc, hw, cb): return ['a function under check was not found']
    # a native launch stays native when the game recreates its swap chain: the guard comes before reattach
    lines = [l.strip() for l in init.split('\n') if l.strip()]
    if lines[:2] != ['if (g_native) return nullptr;', 'if (g_d3d12) return reattach(wnd, desc);']:
        out.append('p12Init does not return natively before reattach')
    # the mode: the setting and whether a frame generation backend started, after the backend is picked
    decide = init.find('const bool native = presentMode(ini(L"d3d12", L"present", 1), g_fg || g_fsrFg || g_xessFg) == kPresentNative;')
    pick = init.find('fgPickBackend(')
    if decide < 0: out.append('p12Init does not decide the mode from [d3d12] present and the frame generation backend')
    elif pick < 0 or pick > decide: out.append('the mode is decided before the frame generation backend is picked')
    # natively, no D3D12 device unless FSR or XeSS files are there: that return comes before any device is made
    early = init.find('if (native && !nativeNeedsD3D12(fsrPresent(), xessPresent())) { ad->Release(); return nativeInit(f, wnd); }')
    made = [i for i in (init.find('D3D12CreateDevice('), init.find('g_createDevice12(')) if i >= 0]
    if early < 0 or not made or early > min(made) or early < decide:
        out.append('the native return without FSR or XeSS files is not between the mode decision and the D3D12 device creation')
    # with the device, still native: after the fences, before our swap chain
    late = init.find('if (native) return nativeInit(f, wnd);')
    ours = init.find('DXGI_SWAP_CHAIN_DESC1 sd = *desc;')
    if late < 0 or ours < 0 or late > ours: out.append('a native launch with FSR or XeSS files goes on to make our swap chain')
    if not re.search(r'if \(FAILED\(hr\) && native\) \{[^}]*releaseDevice12\(\);\s*return nativeInit\(f, wnd\);', init):
        out.append('a failed D3D12 device natively does not fall back to native without it')
    # p12Enabled stays "anything but 0", so present=2 and present=1 both start the hooks
    if 'bool p12Enabled() { return ini(L"d3d12", L"present", 1) != 0; }' not in p12: out.append('p12Enabled is not "[d3d12] present other than 0"')
    # both Present hooks: our own swap chain first, then native, before the presenter's path
    for name, b, call in (('hk_Present', hp, 'if (p12IsNative(sc)) return nativePresent(sc, si, f, [&](UINT fl) { return o_Present(sc, si, fl); });'),
                          ('hk_Present1', hp1, 'if (p12IsNative(sc)) return nativePresent(sc, si, f, [&](UINT fl) { return o_Present1(sc, si, fl, p); });')):
        own, at, rest = b.find('if (p12Owns(sc))'), b.find(call), b.find('onPresent(sc);')
        if at < 0: out.append(name + ' has no native branch passing the game\'s interval'); continue
        if own < 0 or own > at or rest < at: out.append(name + ' does not take the native branch after p12Owns and before the presenter')
    # the native present: the counter, the menu before, the game's interval, the retry without tearing, then after
    want = ['onPresent(sc);', 'const UINT fl = p12NativeBefore(sc, si, f);', 'HRESULT hr = present(fl);',
            'if (hr == DXGI_ERROR_INVALID_CALL && fl != f) { p12NativeTearingRefused(); hr = present(f); }',
            'p12NativeAfter(sc, f);', 'return hr;']
    if [l.strip() for l in np.split('\n') if l.strip()] != want: out.append('nativePresent is not the fixed order with the retry without tearing')
    # the native swap chain is compared by pointer only, and attached from createdBy once the hook flagged it
    if 'bool p12IsNative(IDXGISwapChain* sc) { return g_native && sc && sc == g_nativeSc; }' not in p12: out.append('p12IsNative is not a pointer comparison')
    if not re.search(r'if \(s_nativeCreate\) p12NativeAttach\(sc, s_nativeTear\);\s*s_nativeCreate = false;', cb):
        out.append('createdBy does not attach the game\'s swap chain natively')
    if 'g_nativeSc = sc; g_nativeTearing = tearing;' not in attach: out.append('p12NativeAttach does not record the swap chain')
    if re.search(r'\bsc->AddRef\(\)', attach): out.append('p12NativeAttach holds a reference on the game\'s swap chain')
    for name, b, de in (('hk_CreateSC', sc, 'dd'), ('hk_CreateSCHwnd', hw, 'nd')):
        if 's_nativeCreate = game && desc && p12Native();' not in b: out.append(name + ' does not flag the game\'s swap chain natively')
        if 's_nativeTear = s_nativeCreate && p12NativeTearing() && flipModel(' + de + '.SwapEffect);' not in b:
            out.append(name + ' adds the tearing flag other than natively on flip model')
        if not re.search(r'if \(s_nativeTear && FAILED\(hr\)\) \{ s_nativeTear = false; p12NativeTearingRefused\(\); hr = o_CreateSC', b):
            out.append(name + ' does not retry without the tearing flag')
        if b.rstrip().count('s_nativeCreate = false;') < 1: out.append(name + ' leaves the native flag set')
    if 'return e == DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL || e == DXGI_SWAP_EFFECT_FLIP_DISCARD;' not in main: out.append('flipModel is not the two flip effects')
    # the header stays free of Windows types so the host test covers it
    if re.search(r'#include\s*[<"](windows|d3d|dxgi)', head, re.I): out.append('present_mode.h includes Windows headers')
    return out

p = problems(p12, main, head)
if p:
    for x in p: print('FAIL:', x)
    sys.exit(1)
early = '    if (native && !nativeNeedsD3D12(fsrPresent(), xessPresent())) { ad->Release(); return nativeInit(f, wnd); }\n'
made = '    ad->Release();\n    // Streamline must see the device first'
broken = {
    'native return after the D3D12 device': (p12.replace(early, '', 1).replace(made, '    ad->Release();\n    if (native && !nativeNeedsD3D12(fsrPresent(), xessPresent())) return nativeInit(f, wnd);\n    // Streamline must see the device first', 1), main, head),
    'native guard missing before reattach': (p12.replace('    if (g_native) return nullptr;', '', 1), main, head),
    'native guard after reattach': (p12.replace('    if (g_native) return nullptr;', '', 1).replace('    if (g_d3d12) return reattach(wnd, desc);', '    if (g_d3d12) return reattach(wnd, desc);\n    if (g_native) return nullptr;', 1), main, head),
    'present=2 read as automatic': (p12.replace('presentMode(ini(L"d3d12", L"present", 1), ', 'presentMode(1, ', 1), main, head),
    'backend not looked at': (p12.replace('ini(L"d3d12", L"present", 1), g_fg || g_fsrFg || g_xessFg)', 'ini(L"d3d12", L"present", 1), false)', 1), main, head),
    'no native path with FSR files': (p12.replace('    if (native) return nativeInit(f, wnd);\n', '', 1), main, head),
    'device failure not native': (p12.replace('        releaseDevice12();\n        return nativeInit(f, wnd);', '        releaseDevice12();\n        return nullptr;', 1), main, head),
    'hk_Present1 without the native branch': (p12, main.replace('    if (p12IsNative(sc)) return nativePresent(sc, si, f, [&](UINT fl) { return o_Present1(sc, si, fl, p); });\n', '', 1), head),
    'hk_Present without the native branch': (p12, main.replace('    if (p12IsNative(sc)) return nativePresent(sc, si, f, [&](UINT fl) { return o_Present(sc, si, fl); });\n', '', 1), head),
    'native present at interval 0': (p12, main.replace('return o_Present(sc, si, fl); });', 'return o_Present(sc, 0, fl); });', 1), head),
    'native present1 at interval 1': (p12, main.replace('return o_Present1(sc, si, fl, p); });', 'return o_Present1(sc, 1, fl, p); });', 1), head),
    'menu given interval 0': (p12, main.replace('p12NativeBefore(sc, si, f);', 'p12NativeBefore(sc, 0, f);', 1), head),
    'no retry without tearing': (p12, main.replace('    if (hr == DXGI_ERROR_INVALID_CALL && fl != f) { p12NativeTearingRefused(); hr = present(f); }\n', '', 1), head),
    'native before p12Owns': (p12, main.replace('    if (p12Owns(sc)) return o_Present(sc, si, f);\n    if (p12IsNative(sc)) return nativePresent(sc, si, f, [&](UINT fl) { return o_Present(sc, si, fl); });\n',
                                                '    if (p12IsNative(sc)) return nativePresent(sc, si, f, [&](UINT fl) { return o_Present(sc, si, fl); });\n    if (p12Owns(sc)) return o_Present(sc, si, f);\n', 1), head),
    'not attached natively': (p12, main.replace('    if (s_nativeCreate) p12NativeAttach(sc, s_nativeTear);\n', '', 1), head),
    'tearing on bitblt': (p12, main.replace('p12NativeTearing() && flipModel(dd.SwapEffect);', 'p12NativeTearing();', 1), head),
    'no creation retry': (p12, main.replace('if (s_nativeTear && FAILED(hr)) { s_nativeTear = false; p12NativeTearingRefused(); hr = o_CreateSCHwnd(', 'if (false) { hr = o_CreateSCHwnd(', 1), head),
    'reference held': (p12.replace('    g_nativeSc = sc; g_nativeTearing = tearing;', '    sc->AddRef(); g_nativeSc = sc; g_nativeTearing = tearing;', 1), main, head),
    'p12Enabled off for present=2': (p12.replace('return ini(L"d3d12", L"present", 1) != 0; }', 'return ini(L"d3d12", L"present", 1) == 1; }', 1), main, head),
    'Windows header': (p12, main, head.replace('#pragma once', '#pragma once\n#include <windows.h>', 1)),
}
for name, files in broken.items():
    if files == (p12, main, head): print('FAIL: could not build the broken copy:', name); sys.exit(1)
    if not problems(*files): print('FAIL: check passed on the broken copy:', name); sys.exit(1)
print('present mode checks passed (and fail on %d broken copies)' % len(broken))
PY
echo "PASS: present mode"
