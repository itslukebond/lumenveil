#!/bin/bash
# Checks which swap chain creations are taken as the game's when another DXGI wrapper (OptiScaler, Special K) passes
# the game's call on (src/swapchain_caller.h, main.cpp): builds tests/swapchain_caller_test.cpp with the host compiler
# and runs it, also against a copy of the header whose decision looks at the immediate caller only (it must fail); then
# checks that both factory hooks keep the p12Busy guard first and ask takeAsGame, that a call straight from the game or
# ReShade is decided as before with nothing walked or logged, that only a call from an unknown module walks the chain
# and checks the window, that the log lines are capped, and that the GetDesc and GetHwnd hooks go on only when our
# presenter took a swap chain passed on by a wrapper. Each source check must fail on a broken copy.
# Usage: tests/check_swapchain_caller.sh [source folder]   (default: src next to tests)
here=$(cd "$(dirname "$0")" && pwd)
src=$(cd "${1:-$here/../src}" && pwd) || exit 1
T=$(mktemp -d) || exit 1
trap 'rm -rf "$T"' EXIT

flags=(-std=c++17 -O2 -Wall -Wextra -Werror)
g++ "${flags[@]}" -I "$src" -o "$T/swapchain_caller_test" "$here/swapchain_caller_test.cpp" \
    || { echo "FAIL: swapchain_caller_test did not build"; exit 1; }
"$T/swapchain_caller_test" || { echo "FAIL: swapchain_caller_test"; exit 1; }

# The rule before this change, as a broken header: the immediate caller alone decides. The host test must fail on it.
mkdir "$T/old"
python3 -I - "$src/swapchain_caller.h" "$T/old/swapchain_caller.h" <<'PY' || exit 1
import sys
s = open(sys.argv[1], encoding='utf-8').read()
old = '    const int end = n < maxFrames + 1 ? n : maxFrames + 1;'
if s.count(old) != 1: print('FAIL: could not build the immediate-caller-only header'); sys.exit(1)
open(sys.argv[2], 'w', encoding='utf-8').write(s.replace(old, '    const int end = maxFrames ? 1 : 1;'))
PY
g++ "${flags[@]}" -I "$T/old" -o "$T/old/t" "$here/swapchain_caller_test.cpp" || { echo "FAIL: broken header did not build"; exit 1; }
if "$T/old/t" >/dev/null; then echo "FAIL: swapchain_caller_test passes with the immediate caller alone deciding"; exit 1; fi

python3 -I - "$src/main.cpp" "$src/swapchain_caller.h" <<'PY' || exit 1
import re, sys
read = lambda p: open(p, encoding='utf-8', errors='replace').read().replace('\r\n', '\n')
main, head = read(sys.argv[1]), read(sys.argv[2])

def body(src, name):
    m = re.search(r'^\S[^;{}]*?\b' + name + r'\([^;{}]*\{\n(.*?)^\}', src, re.M | re.S)  # signature may span lines
    return re.sub(r'//[^\n]*', '', m.group(1)) if m else None

def problems(main, head):
    out = []
    names = ('hk_CreateSC', 'hk_CreateSCHwnd', 'takeAsGame', 'createdBy', 'hk_GetHwnd', 'callerChain')
    sc, hw, take, cb, gh, chain = (body(main, n) for n in names)
    if None in (sc, hw, take, cb, gh, chain): return ['a function under check was not found']
    # both hooks: Streamline's own swap chain inside p12Init first, then the decision, and the wrapper flag handed on
    for name, b, call in (('hk_CreateSC', sc, 'takeAsGame(_ReturnAddress(), d, desc ? desc->OutputWindow : nullptr, &viaWrapper)'),
                          ('hk_CreateSCHwnd', hw, 'takeAsGame(_ReturnAddress(), d, w, &viaWrapper)')):
        busy, at = b.find('if (p12Busy())'), b.find(call)
        if busy < 0 or at < 0 or busy > at: out.append(name + ' does not keep the p12Busy guard before takeAsGame')
        if 'calledByGame(' in b: out.append(name + ' decides without takeAsGame')
        if b.count('createdBy(*sc, hidden != nullptr, viaWrapper);') != 1: out.append(name + ' does not hand the wrapper flag to createdBy')
    # straight from the game or ReShade: decided first, as before, nothing walked or logged
    first = take.lstrip().split('\n', 1)[0]
    if first != 'if (calledByGame(ret)) return true;': out.append('takeAsGame does not take a call from the game or ReShade first, as before')
    gate = re.search(r'if \(moduleOf\(ret, &first\) != swapchain_caller::Mod::kOther\) return false;', take)
    walk = take.find('callerChain(')
    if not gate or walk < 0 or gate.start() > walk or take.find('logf(') < gate.start():
        out.append('a call from our own, Streamline or system modules is walked or logged')
    # the window is checked only for a call a wrapper passed on
    if not re.search(r'const char\* why = v\.kind != swapchain_caller::Verdict::kWrapper \? v\.reason : wrapperRefusal\(device, w\);', take):
        out.append('the window check is not made for (only) a call passed on by a wrapper')
    if 'swapchain_caller::decide(chain, n)' not in take: out.append('takeAsGame does not decide from the return chain')
    if not re.search(r'if \(logged\.fetch_add\(1\) < 8\) logf\("dxgi: swap chain via %ls not taken \(%s\), passed through"', take):
        out.append('refusals are not logged, or not capped at 8')
    if not re.search(r'if \(!named\.exchange\(true\)\) logf\("dxgi: the game\'s swap chain was passed on by %ls"', take):
        out.append('the wrapper is not named once in the log')
    if not re.search(r'\*viaWrapper = true;\s*return true;\s*$', take): out.append('a call passed on by a wrapper is not marked as such')
    # the chain starts at the hook's own return address
    if 'frames[at] != ret' not in chain: out.append('the return chain does not start at the hook\'s return address')
    # GetDesc / GetHwnd report the game's window only when our presenter took a swap chain passed on by a wrapper
    if not re.search(r'if \(p12\) p12Attach\(sc\);\s*if \(p12 && viaWrapper\) hookWrapperView\(sc\);', cb):
        out.append('hookWrapperView is not limited to our presenter on the wrapper path')
    if 'if (SUCCEEDED(hr) && w && p12IsGame(sc)) *w = p12GameWindow();' not in gh:
        out.append("hk_GetHwnd changes the window of swap chains other than the game's, or on failure")
    if '(*(void***)sc1)[20], (void*)hk_GetHwnd' not in main: out.append('GetHwnd (IDXGISwapChain1 slot 20) is not hooked')
    wr, hv = body(main, 'wrapperRefusal'), body(main, 'hookWrapperView')
    if wr is None or hv is None: return out + ['a function under check was not found']
    # the chain is every captured frame from ret upwards, each mapped to its module
    if not re.search(r'for \(int i = at; i < n && k < cap; \+\+i\) out\[k\+\+\] = moduleOf\(frames\[i\], &m\);\s*return k;\s*$', chain):
        out.append('the return chain is not the captured frames from the caller upwards')
    # the window check reads the device and the window, then asks refuse()
    for need, what in (('device->QueryInterface(IID_PPV_ARGS(&d11))', 'the device'), ('t.topLevel = GetAncestor(w, GA_ROOT) == w;', 'top-level'),
                       ('t.toolWindow = (GetWindowLongPtrW(w, GWL_EXSTYLE) & WS_EX_TOOLWINDOW) != 0;', 'tool window'),
                       ('GetClientRect(w, &r);', 'client area')):
        if need not in wr: out.append('the window check does not read ' + what)
    if not re.search(r'return swapchain_caller::refuse\(t\);\s*$', wr): out.append('the window check does not end in refuse()')
    # the refusal count lives for the process, so the cap holds
    if 'static std::atomic<int> logged;' not in take: out.append('the refusal count restarts on each call')
    # the wrapper path hooks GetDesc too when ReShade has not already
    if 'if (!o_GetDesc) MH_CreateHook(vt[12], (void*)hk_GetDesc, (void**)&o_GetDesc);' not in hv:
        out.append('GetDesc is not hooked on the wrapper path')
    # the header stays free of Windows types so the host test covers it
    if re.search(r'#include\s*[<"](windows|d3d|dxgi)', head, re.I): out.append('swapchain_caller.h includes Windows headers')
    return out

p = problems(main, head)
if p:
    for x in p: print('FAIL:', x)
    sys.exit(1)
take = body(main, 'takeAsGame')
broken = {
    'p12Busy guard dropped': (main.replace('    if (p12Busy()) return o_CreateSC(f, d, desc, sc);', '', 1), head),
    'p12Busy after the decision': (main.replace('    if (p12Busy()) return o_CreateSCHwnd(f, d, w, desc, fs, o, sc);\n    bool viaWrapper = false;\n    bool game = takeAsGame(_ReturnAddress(), d, w, &viaWrapper);\n',
                                                '    bool viaWrapper = false;\n    bool game = takeAsGame(_ReturnAddress(), d, w, &viaWrapper);\n    if (p12Busy()) return o_CreateSCHwnd(f, d, w, desc, fs, o, sc);\n', 1), head),
    'old rule kept in a hook': (main.replace('bool game = takeAsGame(_ReturnAddress(), d, w, &viaWrapper);', 'bool game = calledByGame(_ReturnAddress());', 1), head),
    'flag not handed on': (main.replace('createdBy(*sc, hidden != nullptr, viaWrapper);', 'createdBy(*sc, hidden != nullptr, false);', 1), head),
    'calledByGame bypassed': (main.replace('    if (calledByGame(ret)) return true;\n', '', 1), head),
    'own modules walked': (main.replace('if (moduleOf(ret, &first) != swapchain_caller::Mod::kOther) return false;', 'moduleOf(ret, &first);', 1), head),
    'window check skipped': (main.replace(': wrapperRefusal(device, w);', ': nullptr;', 1), head),
    'window check for every caller': (main.replace('v.kind != swapchain_caller::Verdict::kWrapper ? v.reason : wrapperRefusal(device, w)', 'v.kind != swapchain_caller::Verdict::kWrapper ? v.reason : wrapperRefusal(device, w) ? wrapperRefusal(device, w) : wrapperRefusal(device, w)', 1).replace('if (calledByGame(ret)) return true;', 'if (calledByGame(ret)) return !wrapperRefusal(device, w);', 1), head),
    'log not capped': (main.replace('if (logged.fetch_add(1) < 8) logf(', 'logged.fetch_add(1); logf(', 1), head),
    'wrapper not named': (main.replace('if (!named.exchange(true)) logf("dxgi: the game\'s swap chain was passed on by %ls", name);', 'named = true;', 1), head),
    'not marked as wrapper': (main.replace('    *viaWrapper = true;\n', '', 1), head),
    'chain not from ret': (main.replace('frames[at] != ret', 'frames[at] == nullptr', 1), head),
    'view hooked always': (main.replace('if (p12 && viaWrapper) hookWrapperView(sc);', 'hookWrapperView(sc);', 1), head),
    'GetHwnd for any swap chain': (main.replace('if (SUCCEEDED(hr) && w && p12IsGame(sc)) *w', 'if (SUCCEEDED(hr) && w) *w', 1), head),
    'GetHwnd not hooked': (main.replace('(*(void***)sc1)[20], (void*)hk_GetHwnd', '(*(void***)sc1)[21], (void*)hk_GetHwnd', 1), head),
    'Windows header': (main, head.replace('#include <string_view>', '#include <string_view>\n#include <windows.h>', 1)),
    'chain always empty': (main.replace('out[k++] = moduleOf(frames[i], &m);\n    return k;', 'out[k++] = moduleOf(frames[i], &m);\n    return 0;', 1), head),
    'window check always passes': (main.replace('    return swapchain_caller::refuse(t);\n', '    (void)swapchain_caller::refuse(t);\n    return nullptr;\n', 1), head),
    'top-level not read': (main.replace('t.topLevel = GetAncestor(w, GA_ROOT) == w;', 't.topLevel = true;', 1), head),
    'tool window not read': (main.replace('t.toolWindow = (GetWindowLongPtrW(w, GWL_EXSTYLE) & WS_EX_TOOLWINDOW) != 0;', 't.toolWindow = false;', 1), head),
    'refusal count per call': (main.replace('static std::atomic<int> logged;', 'std::atomic<int> logged{0};', 1), head),
    'GetDesc not hooked on the wrapper path': (main.replace('        if (!o_GetDesc) MH_CreateHook(vt[12], (void*)hk_GetDesc, (void**)&o_GetDesc);\n', '', 1), head),
}
for name, files in broken.items():
    if files == (main, head): print('FAIL: could not build the broken copy:', name); sys.exit(1)
    if not problems(*files): print('FAIL: check passed on the broken copy:', name); sys.exit(1)
print('swap chain caller checks passed (and fail on %d broken copies)' % len(broken))
PY
echo "PASS: swap chain caller"
