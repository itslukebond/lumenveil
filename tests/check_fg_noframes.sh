#!/bin/bash
# Checks what the mod says when DLSS frame generation is on but adds no frames (src/fg_support.h, present12.cpp): builds
# tests/fg_noframes_test.cpp with the host compiler and runs it, also against broken copies of the header (each must
# fail); then checks that the 500-frame summary keeps the menu's verdict from every window and names the scRGB reason
# in its once-per-launch line, and that switching DLSS frame generation off clears the verdict. Each source check must
# fail on a broken copy.
# Usage: tests/check_fg_noframes.sh [source folder]   (default: src next to tests)
here=$(cd "$(dirname "$0")" && pwd)
src=$(cd "${1:-$here/../src}" && pwd) || exit 1
T=$(mktemp -d) || exit 1
trap 'rm -rf "$T"' EXIT

g++ -std=c++17 -O2 -Wall -Wextra -Werror -I "$src" -o "$T/fg_noframes_test" "$here/fg_noframes_test.cpp" \
    || { echo "FAIL: fg_noframes_test did not build"; exit 1; }
"$T/fg_noframes_test" || { echo "FAIL: fg_noframes_test"; exit 1; }

# Broken headers: the host test must fail on each.
python3 -I - "$src/fg_support.h" "$T" <<'PY' || exit 1
import os, sys
s = open(sys.argv[1], encoding='utf-8').read()
broken = {
    'any backend': ('return backend == 0 && encoding == 2 ? kNoneScrgb : kNone;', 'return encoding == 2 ? kNoneScrgb : kNone;'),
    'any encoding': ('return backend == 0 && encoding == 2 ? kNoneScrgb : kNone;', 'return backend == 0 ? kNoneScrgb : kNone;'),
    'hdr10 named': ('backend == 0 && encoding == 2 ?', 'backend == 0 && encoding >= 1 ?'),
    'every window': ('if (!noneGenerated(onFrames, presented, rendered)) return kNotSeen;', ''),
    'never named': ('? kNoneScrgb : kNone;', '? kNone : kNone;'),
    'no reason': ('return n == kNoneScrgb ? "the game', 'return n == -1 ? "the game'),
}
for name, (a, b) in broken.items():
    if s.count(a) != 1: print('FAIL: could not build the broken header:', name); sys.exit(1)
    os.makedirs(os.path.join(sys.argv[2], name))
    open(os.path.join(sys.argv[2], name, 'fg_support.h'), 'w', encoding='utf-8').write(s.replace(a, b))
PY
for d in "$T"/*/; do
    g++ -std=c++17 -O2 -I "$d" -o "$d/t" "$here/fg_noframes_test.cpp" 2>/dev/null || { echo "FAIL: broken header did not build: $d"; exit 1; }
    if "$d/t" >/dev/null; then echo "FAIL: fg_noframes_test passes on the broken header $(basename "$d")"; exit 1; fi
done

python3 -I - "$src/present12.cpp" <<'PY' || exit 1
import re, sys
p12 = open(sys.argv[1], encoding='utf-8', errors='replace').read().replace('\r\n', '\n')

def body(src, name):
    m = re.search(r'^\S[^;{}]*?\b' + name + r'\([^;{}]*\{\n(.*?)^\}', src, re.M | re.S)
    return re.sub(r'//[^\n]*', '', m.group(1)) if m else None

def problems(p12):
    out = []
    after, mode = body(p12, 'fgAfterPresent'), body(p12, 'fgSetMode')
    if None in (after, mode): return ['a function under check was not found']
    sumAt, reset = after.find('if (++g_rendered == 500) {'), after.find('g_rendered = g_presented = g_onFrames = 0;')
    # the verdict: from this window's counts, DLSS (backend 0), the output's encoding, every window, before the reset
    v = after.find('g_status.noFrames = fg_support::noFrames(0, g_encode, g_onFrames, g_presented, g_rendered);')
    if v < 0 or not (sumAt < v < reset): out.append('the menu\'s verdict is not kept from every window before its counts reset')
    once = re.search(r'if \(!toldNone && fg_support::noneGenerated\(g_onFrames, g_presented, g_rendered\)\) \{\s*toldNone = true;(.*?)\n        \}', after, re.S)
    if not once: out.append('the no-frames line is not said once')
    else:
        b = once.group(1)
        if not re.search(r'if \(const char\* why = fg_support::noFramesReason\(g_status\.noFrames\)\)\s*logf\("fg: DLSS frame generation is on but generated no frames in the last %u: %s\. %s", g_rendered, why, fg_support::kScrgbAdvice\);', b):
            out.append('the no-frames line does not name the reason and the advice')
        if 'else logf("fg: DLSS frame generation is on but generated no frames in the last %u", g_rendered);' not in b:
            out.append('the no-frames line without a reason changed')
    if v > 0 and once and v > once.start(): out.append('the verdict is set after the once-per-launch line reads it')
    # switching DLSS frame generation off clears the verdict, after the proxy (FSR, XeSS) branch has returned
    clr = mode.find('if (!on) g_status.noFrames = fg_support::kNotSeen;')
    if clr < 0 or clr < mode.find('slDLSSGSetOptions('): out.append('switching DLSS frame generation off does not clear the verdict')
    return out

p = problems(p12)
if p:
    for x in p: print('FAIL:', x)
    sys.exit(1)
V = 'g_status.noFrames = fg_support::noFrames(0, g_encode, g_onFrames, g_presented, g_rendered);'
broken = {
    'no verdict': p12.replace(V, '', 1),
    'verdict from FSR': p12.replace(V, V.replace('noFrames(0,', 'noFrames(1,'), 1),
    'verdict once only': p12.replace('        ' + V + '\n', '', 1).replace('toldNone = true;\n', 'toldNone = true;\n            ' + V + '\n', 1),
    'verdict after reset': p12.replace('        ' + V + '\n', '', 1).replace('g_rendered = g_presented = g_onFrames = 0;', 'g_rendered = g_presented = g_onFrames = 0;\n        ' + V, 1),
    'reason dropped': p12.replace(': %s. %s", g_rendered, why, fg_support::kScrgbAdvice);', '", g_rendered);', 1),
    'advice dropped': p12.replace('%s. %s", g_rendered, why, fg_support::kScrgbAdvice);', '%s", g_rendered, why);', 1),
    'plain line changed': p12.replace('else logf("fg: DLSS frame generation is on but generated no frames in the last %u", g_rendered);', 'else {}', 1),
    'not cleared': p12.replace('    if (!on) g_status.noFrames = fg_support::kNotSeen;', '', 1),
    'cleared on': p12.replace('if (!on) g_status.noFrames = fg_support::kNotSeen;', 'if (on) g_status.noFrames = fg_support::kNotSeen;', 1),
}
for name, b in broken.items():
    if b == p12: print('FAIL: could not build the broken copy:', name); sys.exit(1)
    if not problems(b): print('FAIL: check passed on the broken copy:', name); sys.exit(1)
print('no-frames checks passed (and fail on %d broken copies)' % len(broken))
PY
echo "PASS: fg no frames"
