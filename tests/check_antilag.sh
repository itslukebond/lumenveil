#!/bin/bash
# Checks AMD Radeon Anti-Lag 2: builds tests/antilag_test.cpp (src/antilag_rules.h) with the host compiler and runs it,
# checks that AMD's vendored header is the one from the SDK's commit 390aa4a8, then checks the presenter's call order
# and the menu's ini key in present12.cpp and menu.cpp. Each source check must fail on a broken copy.
# Usage: tests/check_antilag.sh [source folder holding antilag_rules.h, present12.cpp and menu.cpp]   (default: src)
here=$(cd "$(dirname "$0")" && pwd)
src=$(cd "${1:-$here/../src}" && pwd) || exit 1
T=$(mktemp -d) || exit 1
trap 'rm -rf "$T"' EXIT
g++ -std=c++17 -O2 -Wall -Wextra -Werror -I "$src" -o "$T/antilag_test" "$here/antilag_test.cpp" || { echo "FAIL: antilag_test did not build"; exit 1; }
"$T/antilag_test" || { echo "FAIL: antilag_test"; exit 1; }

hdr=$here/../third_party/amd-antilag2/ffx_antilag2_dx12.h
want=0554c804cfb02442974b90cf1887031bfd18d41fba2054a2cd23c8ee65e6265f  # AntiLag2-SDK 390aa4a8, ffx_antilag2_dx12.h
got=$(sha256sum "$hdr" 2>/dev/null | cut -d' ' -f1)
[ "$got" = "$want" ] || { echo "FAIL: $hdr is not AMD's header from 390aa4a8 (sha256 ${got:-missing})"; exit 1; }

python3 -I - "$src/present12.cpp" "$src/menu.cpp" <<'PY' || exit 1
import re, sys
present = open(sys.argv[1], encoding='utf-8', errors='replace').read().replace('\r\n', '\n')
menu = open(sys.argv[2], encoding='utf-8', errors='replace').read().replace('\r\n', '\n')

def bodies(src):
    return {n: b for n, b in re.findall(r'^\S[^\n;]*?\b(\w+)\([^\n]*\{\n(.*?)^\}', src, re.M | re.S)}
code = lambda b: re.sub(r'//[^\n]*', '', b)

def order(body, *marks):
    # every mark present, each after the one before it
    at = -1
    for m in marks:
        i = body.find(m, at + 1)
        if i < 0: return False
        at = i
    return True

def problems(p, m):
    out, b = [], {k: code(v) for k, v in bodies(p).items()}
    init, present_, fail_ = b.get('p12Init'), b.get('p12Present'), b.get('fail')
    if None in (init, present_, fail_): return ['p12Init, p12Present or fail not found']
    # AC1: its own key, off unless the ini says otherwise
    if 'ini(L"fg", L"antilag", 0)' not in init: out.append('p12Init does not read [fg] antilag with default 0')
    # AC4: the driver is asked only on an AMD card outside Wine, once, after the device exists
    if not re.search(r'g_vendor == 0x1002 && !g_wine \? AMD::AntiLag2DX12::Initialize\(&g_al2, g_d3d12\)', init):
        out.append('Anti-Lag 2 is initialised without the AMD-and-not-Wine check')
    if not order(init, 'D3D12CreateDevice(', 'Initialize(&g_al2'): out.append('Anti-Lag 2 is initialised before the D3D12 device exists')
    if [k for k, v in b.items() if re.search(r'(?<!De)Initialize\(&g_al2', v) and k != 'p12Init']: out.append('Anti-Lag 2 is initialised outside p12Init')
    # AC7: released after FSR's thread and Intel's swap chain are gone, before the queue and device
    if not order(fail_, 'fsrFgDestroy(', 'xessFgDestroy()', 'DeInitialize(&g_al2)', 'release(g_d3d12Queue)'):
        out.append('fail() releases Anti-Lag 2 out of order (after fsrFgDestroy and xessFgDestroy, before the queue and device)')
    # AC2/AC5: decided after the menu (a change applies this frame); FSR is told before Present; the driver's sleep after pacing
    if not order(present_, 'menuFrame(', 'al2Run('): out.append('p12Present decides Anti-Lag 2 before the menu has run')
    if not re.search(r'al2Run\(g_al2State, g_set\.antilag, g_fg, g_xessSc\)', present_):
        out.append('p12Present does not rule out DLSS and XeSS frame generation for Anti-Lag 2')
    if not order(present_, 'MarkEndOfFrameRendering(&g_al2)', 'g_sc->Present(si'): out.append('the end of the frame is not marked before Present')
    if not re.search(r'if \(g_fsrFg && al2\) AMD::AntiLag2DX12::MarkEndOfFrameRendering', present_):
        out.append('the end of the frame is marked without FSR frame generation and Anti-Lag 2 both on')
    if not order(present_, 'fsrPace()', 'Update(&g_al2'): out.append('Anti-Lag 2 is updated before the presenter paces the frame')
    # AC6: off means no call per frame after the one that turns it off
    if not re.search(r'if \(al2 \|\| g_al2\.m_enabled\) \{[^}]*Update\(&g_al2, al2, 0\)', present_):
        out.append('Anti-Lag 2 is updated every frame while off')
    # FSR's presenter reads the context through FidelityFX 2.4.0's private data GUID
    if '{0x5083ae5b, 0x8070, 0x4fca, {0x8e, 0xe5, 0x35, 0x82, 0xdd, 0x36, 0x7d, 0x13}}' not in p:
        out.append("FidelityFX's Anti-Lag 2 private data GUID is missing or changed")
    if 'SetPrivateData(kFfxAntiLag2Data' not in present_: out.append('p12Present does not hand the context to FSR')
    # the menu saves its own key
    if 'iniSet(L"fg", L"antilag", fs.antilag);' not in code(m): out.append('the menu does not save [fg] antilag')
    if not re.search(r'if \(al2MenuShown\(st\.antilag, st\.backend\)\)', code(m)): out.append('the menu does not show the section by al2MenuShown')
    return out

got = problems(present, menu)
if got:
    for x in got: print('FAIL:', x)
    sys.exit(1)
pb, fb = bodies(present)['p12Present'], bodies(present)['fail']
broken = {
    'default on': (present.replace('ini(L"fg", L"antilag", 0)', 'ini(L"fg", L"antilag", 1)', 1), menu),
    'no vendor check': (present.replace('g_vendor == 0x1002 && !g_wine ? ', '!g_wine ? ', 1), menu),
    'release after the device': (present.replace(fb, fb.replace('AMD::AntiLag2DX12::DeInitialize(&g_al2);', '', 1).replace(
        'release(g_d3d12Queue);', 'release(g_d3d12Queue); AMD::AntiLag2DX12::DeInitialize(&g_al2);', 1), 1), menu),
    'decided before the menu': (present.replace(pb, pb.replace('const bool al2 = al2Run(', 'const bool al2 = al2Late(', 1).replace(
        '    menuFrame(g_sc, bb);', '    const bool al2Early = al2Run(g_al2State, g_set.antilag, g_fg, g_xessSc);\n    menuFrame(g_sc, bb);', 1), 1), menu),
    'Initialize before the device': (present.replace('    hr = D3D12CreateDevice(', '    hr = D3D12CreateDeviceX(', 1), menu),
    'Initialize elsewhere': (present.replace('    s.antilag = g_al2State;', '    AMD::AntiLag2DX12::Initialize(&g_al2, g_d3d12); s.antilag = g_al2State;', 1), menu),
    'no end-of-frame mark': (present.replace('if (g_fsrFg && al2) AMD::AntiLag2DX12::MarkEndOfFrameRendering(&g_al2);', '', 1), menu),
    'mark without FSR': (present.replace('if (g_fsrFg && al2) AMD::AntiLag2DX12::MarkEndOfFrameRendering', 'if (al2) AMD::AntiLag2DX12::MarkEndOfFrameRendering', 1), menu),
    'FSR not told': (present.replace('g_sc->SetPrivateData(kFfxAntiLag2Data,', 'g_sc->GetDesc1(nullptr); (void)(kFfxAntiLag2Data,', 1), menu),
    'menu shows it always': (present, menu.replace('if (al2MenuShown(st.antilag, st.backend))', 'if (true)', 1)),
    'XeSS not ruled out': (present.replace('al2Run(g_al2State, g_set.antilag, g_fg, g_xessSc)', 'al2Run(g_al2State, g_set.antilag, g_fg, false)', 1), menu),
    'update every frame': (present.replace('if (al2 || g_al2.m_enabled) {', 'if (true) {', 1), menu),
    'update before pacing': (present.replace(pb, pb.replace('fsrPace();', 'fsrPaceX();', 1).replace(
        'g_tUs[4] += lapUs(t);', 'fsrPace(); g_tUs[4] += lapUs(t);', 1), 1), menu),
    'GUID changed': (present.replace('0x5083ae5b', '0x5083ae5c', 1), menu),
    'menu does not save': (present, menu.replace('iniSet(L"fg", L"antilag", fs.antilag);', '', 1)),
}
for name, (p, m) in broken.items():
    if (p, m) == (present, menu): print('FAIL: could not build the broken copy:', name); sys.exit(1)
    if not problems(p, m): print('FAIL: check passed on the broken copy:', name); sys.exit(1)
print('Anti-Lag 2 presenter and menu checks passed (and fail on %d broken copies)' % len(broken))
PY
