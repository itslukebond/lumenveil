#!/bin/bash
# Checks the log lines that say why the mod could not do its job (src/fg_support.h, present12.cpp, main.cpp): builds
# tests/fg_support_test.cpp with the host compiler and runs it, also against broken copies of the header (each must
# fail); then checks that createTargets names the call that failed and keeps its summary line, that p12Init asks
# Streamline whether DLSS frame generation is supported only once it started and never turns frame generation off on
# the answer, that the 500-frame summary says once when frame generation is on but generated nothing, and that both
# swap chain creation hooks report a creation they did not take from a system module or no module (not the system's
# nested call inside ours, capped at 8) and say once when [d3d12] present=0 keeps the game's own swap chain. Each
# source check must fail on a broken copy.
# Usage: tests/check_diagnostics.sh [source folder]   (default: src next to tests)
here=$(cd "$(dirname "$0")" && pwd)
src=$(cd "${1:-$here/../src}" && pwd) || exit 1
T=$(mktemp -d) || exit 1
trap 'rm -rf "$T"' EXIT

flags=(-std=c++17 -O2 -Wall -Wextra -Werror)
g++ "${flags[@]}" -I "$src" -o "$T/fg_support_test" "$here/fg_support_test.cpp" \
    || { echo "FAIL: fg_support_test did not build"; exit 1; }
"$T/fg_support_test" || { echo "FAIL: fg_support_test"; exit 1; }

# Broken headers: the host test must fail on each.
python3 -I - "$src/fg_support.h" "$T" <<'PY' || exit 1
import os, sys
s = open(sys.argv[1], encoding='utf-8').read()
broken = {
    'scheduling': ('case kOSDisabledHWS: return "Hardware-accelerated GPU scheduling is off in Windows";', 'case kOSDisabledHWS: return nullptr;'),
    'numbers': ('kOSOutOfDate = 3, kOSDisabledHWS = 4', 'kOSOutOfDate = 4, kOSDisabledHWS = 3'),
    'part window': ('onFrames == rendered && presented == rendered', 'onFrames > 0 && presented == rendered'),
    'empty window': ('return rendered > 0 && ', 'return '),
}
for name, (a, b) in broken.items():
    if s.count(a) != 1: print('FAIL: could not build the broken header:', name); sys.exit(1)
    os.makedirs(os.path.join(sys.argv[2], name))
    open(os.path.join(sys.argv[2], name, 'fg_support.h'), 'w', encoding='utf-8').write(s.replace(a, b))
PY
for d in "$T"/*/; do
    g++ -std=c++17 -O2 -I "$d" -o "$d/t" "$here/fg_support_test.cpp" 2>/dev/null || { echo "FAIL: broken header did not build: $d"; exit 1; }
    if "$d/t" >/dev/null; then echo "FAIL: fg_support_test passes on the broken header $(basename "$d")"; exit 1; fi
done

python3 -I - "$src/present12.cpp" "$src/main.cpp" <<'PY' || exit 1
import re, sys
read = lambda p: open(p, encoding='utf-8', errors='replace').read().replace('\r\n', '\n')
p12, main = read(sys.argv[1]), read(sys.argv[2])

def body(src, name):
    m = re.search(r'^\S[^;{}]*?\b' + name + r'\([^;{}]*\{\n(.*?)^\}', src, re.M | re.S)
    return re.sub(r'//[^\n]*', '', m.group(1)) if m else None

def problems(p12, main):
    out = []
    ct, sup, init, after = body(p12, 'createTargets'), body(p12, 'fgSupportCheck'), body(p12, 'p12Init'), body(p12, 'fgAfterPresent')
    sc, hw, note, off = body(main, 'hk_CreateSC'), body(main, 'hk_CreateSCHwnd'), body(main, 'noteNotTaken'), body(main, 'notePresentOff')
    if None in (ct, sup, init, after, sc, hw, note, off): return ['a function under check was not found']
    # 1. createTargets: each call names itself before it runs, and a failure logs the step after the summary line
    for call, step in (('CreateCommittedResource(', 'shared texture'), ('CreateSharedHandle(', 'share handle'),
                       ('OpenSharedResource1(', 'open in D3D11'), ('g_alloc->Reset()', 'allocator reset'),
                       ('GetBuffer(i,', 'back buffer'), ('CreateCommandList(', 'command list'), ('->Close()', 'command list close')):
        at, named = ct.find(call), ct.find('step = "' + step + '"')
        if at < 0 or named < 0 or named > at: out.append('createTargets does not name "%s" before %s' % (step, call))
    summary = ct.find('logf("d3d12: present targets %ux%u fmt %u: %08x", d.Width, d.Height, d.Format, hr);')
    failed = ct.find('if (FAILED(hr)) {')
    if summary < 0: out.append('the present targets summary line changed')
    if failed < summary or 'logf("d3d12: present targets failed at %s %d: %08x", step, buf, hr);' not in ct \
       or 'logf("d3d12: present targets failed at %s: %08x", step, hr);' not in ct:
        out.append('a failed step is not logged after the summary line')
    if 'buf = int(i); step = "back buffer";' not in ct: out.append('the back buffer index is not kept for the log')
    # 2. the support check: asked once Streamline started, logged, kept, and never a switch
    if 'case 0: g_fg = fgStart(); if (g_fg) fgSupportCheck(adesc.AdapterLuid); break;' not in init:
        out.append('p12Init does not ask for frame generation support right after Streamline starts')
    if 'DXGI_ADAPTER_DESC adesc = {};' not in init: out.append('the adapter description is read uninitialised when GetDesc fails')
    if 'slIsFeatureSupported(sl::kFeatureDLSS_G, info)' not in sup or 'info.deviceLUID = (uint8_t*)&luid; info.deviceLUIDSizeInBytes = sizeof luid;' not in sup:
        out.append('the support check does not ask about this adapter')
    if 'g_status.slSupport = int(r);' not in sup: out.append('the support result is not kept for the menu')
    if not re.search(r'fg_support::reason\(int\(r\)\)', sup) or 'logf("fg: DLSS frame generation supported on this PC");' not in sup:
        out.append('the support result is not logged with its reason')
    if 'slGetFeatureRequirements(sl::kFeatureDLSS_G, q)' not in sup or 'osVersionDetected' not in sup or 'driverVersionRequired' not in sup:
        out.append('the requirements are not logged')
    if re.search(r'g_fg\s*=|g_set\.enable\s*=|fgSetMode\(', sup): out.append('the support check changes frame generation')
    if not re.search(r'static_assert\([^;]*int\(sl::Result::eErrorOSDisabledHWS\) == fg_support::kOSDisabledHWS[^;]*"fg_support\.h values"\);', p12):
        out.append('fg_support.h numbers are not tied to sl::Result')
    # 2b. on but nothing generated: counted per frame, said once, window reset
    count, sumAt = after.find('if (g_fgOn) ++g_onFrames;'), after.find('if (++g_rendered == 500) {')
    if count < 0 or sumAt < 0 or count > sumAt: out.append('frames with frame generation on are not counted before the summary')
    if not re.search(r'if \(!toldNone && fg_support::noneGenerated\(g_onFrames, g_presented, g_rendered\)\) \{\s*toldNone = true;', after):
        out.append('the no-frames line is not said once from the window counts')
    if 'static bool toldNone;' not in after: out.append('the no-frames line is not once per launch')
    if 'g_rendered = g_presented = g_onFrames = 0;' not in after: out.append('the on-frames count is not reset with the window')
    # 3. skipped takeovers: both hooks, right after the decision, then the depth guard over the rest
    for name, b, call in (('hk_CreateSC', sc, 'bool game = takeAsGame(_ReturnAddress(), d, desc ? desc->OutputWindow : nullptr, &viaWrapper);'),
                          ('hk_CreateSCHwnd', hw, 'bool game = takeAsGame(_ReturnAddress(), d, w, &viaWrapper);')):
        if not re.search(re.escape(call) + r'\s*if \(!game\) noteNotTaken\(_ReturnAddress\(\)\);\s*else if \(!p12Enabled\(\)\) notePresentOff\(\);\s*CreateDepth depth;', b):
            out.append(name + ' does not note a creation it did not take, then guard the nested call')
    if not re.search(r'^\s*if \(s_createDepth\) return;', note): out.append('the system\'s nested call inside ours is noted')
    if 'static thread_local int s_createDepth;' not in main or 'struct CreateDepth { CreateDepth() { ++s_createDepth; } ~CreateDepth() { --s_createDepth; } };' not in main:
        out.append('the creation depth is not per thread, or not undone')
    if 'if (k != swapchain_caller::Mod::kSystem && k != swapchain_caller::Mod::kNone) return;' not in note:
        out.append('callers other than system modules or no module are noted (ours, Streamline, or twice for a wrapper)')
    if 'static std::atomic<int> logged;' not in note or 'if (logged.fetch_add(1) >= 8) return;' not in note: out.append('the not-taken lines are not capped at 8')
    if 'static std::atomic<bool> told;' not in off or 'if (!told.exchange(true)) logf(' not in off: out.append('present=0 is not said once')
    return out

p = problems(p12, main)
if p:
    for x in p: print('FAIL:', x)
    sys.exit(1)
broken = {
    'step named after the call': (p12.replace('if (SUCCEEDED(hr)) { step = "share handle"; hr = g_d3d12->CreateSharedHandle(', 'if (SUCCEEDED(hr)) { hr = g_d3d12->CreateSharedHandle(', 1), main),
    'open step not named': (p12.replace('step = "open in D3D11"; ', '', 1), main),
    'failed step not logged': (p12.replace('else logf("d3d12: present targets failed at %s: %08x", step, hr);', 'else {}', 1), main),
    'summary line changed': (p12.replace('logf("d3d12: present targets %ux%u fmt %u: %08x", d.Width', 'logf("d3d12: targets %ux%u fmt %u: %08x", d.Width', 1), main),
    'buffer index lost': (p12.replace('buf = int(i); step = "back buffer";', 'step = "back buffer";', 1), main),
    'support not asked': (p12.replace('if (g_fg) fgSupportCheck(adesc.AdapterLuid); ', '', 1), main),
    'support asked always': (p12.replace('if (g_fg) fgSupportCheck(adesc.AdapterLuid);', 'fgSupportCheck(adesc.AdapterLuid);', 1), main),
    'adapter desc uninitialised': (p12.replace('DXGI_ADAPTER_DESC adesc = {};', 'DXGI_ADAPTER_DESC adesc;', 1), main),
    'no adapter': (p12.replace('info.deviceLUID = (uint8_t*)&luid; info.deviceLUIDSizeInBytes = sizeof luid;', '', 1), main),
    'result not kept': (p12.replace('g_status.slSupport = int(r);', '', 1), main),
    'support switches fg off': (p12.replace('g_status.slSupport = int(r);', 'g_status.slSupport = int(r); if (r != sl::Result::eOk) g_fg = false;', 1), main),
    'requirements not logged': (p12.replace('slGetFeatureRequirements(sl::kFeatureDLSS_G, q)', 'sl::Result::eErrorIO', 1), main),
    'numbers not tied': (p12.replace('int(sl::Result::eErrorOSDisabledHWS) == fg_support::kOSDisabledHWS', 'true', 1), main),
    'on frames not counted': (p12.replace('    if (g_fgOn) ++g_onFrames;\n', '', 1), main),
    'no-frames every window': (p12.replace('if (!toldNone && fg_support::noneGenerated', 'if (fg_support::noneGenerated', 1), main),
    'on frames not reset': (p12.replace('g_rendered = g_presented = g_onFrames = 0;', 'g_rendered = g_presented = 0;', 1), main),
    'not noted in CreateSC': (p12, main.replace('    if (!game) noteNotTaken(_ReturnAddress());\n', '', 1)),
    'present off not said': (p12, main.replace('    else if (!p12Enabled()) notePresentOff();\n', '', 1)),
    'no depth guard': (p12, main.replace('    CreateDepth depth;\n', '', 1)),
    'nested call noted': (p12, main.replace('    if (s_createDepth) return;\n', '', 1)),
    'depth shared across threads': (p12, main.replace('static thread_local int s_createDepth;', 'static int s_createDepth;', 1)),
    'depth never undone': (p12, main.replace('~CreateDepth() { --s_createDepth; }', '~CreateDepth() {}', 1)),
    'every caller noted': (p12, main.replace('if (k != swapchain_caller::Mod::kSystem && k != swapchain_caller::Mod::kNone) return;', '', 1)),
    'not capped': (p12, main.replace('if (logged.fetch_add(1) >= 8) return;', 'logged.fetch_add(1);', 1)),
    'present off every time': (p12, main.replace('if (!told.exchange(true)) logf(', 'told = true; logf(', 1)),
}
for name, files in broken.items():
    if files == (p12, main): print('FAIL: could not build the broken copy:', name); sys.exit(1)
    if not problems(*files): print('FAIL: check passed on the broken copy:', name); sys.exit(1)
print('diagnostics checks passed (and fail on %d broken copies)' % len(broken))
PY
echo "PASS: diagnostics"
