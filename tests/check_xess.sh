#!/bin/bash
# Checks src/xess_rules.h (the back buffer formats XeSS frame generation takes, and the XeSS preset for the game's
# render scale): builds tests/xess_test.cpp with the host compiler and runs it. Then checks that present12.cpp asks the
# format rule each time the game's format can change (launch, a recreated game swap chain, a resize), not once per
# launch, and that each of those checks fails on a broken copy.
# Usage: tests/check_xess.sh [source folder holding xess_rules.h and present12.cpp]   (default: src next to tests)
here=$(cd "$(dirname "$0")" && pwd)
src=$(cd "${1:-$here/../src}" && pwd) || exit 1
T=$(mktemp -d) || exit 1
trap 'rm -rf "$T"' EXIT
g++ -std=c++17 -O2 -Wall -Wextra -Werror -I "$src" -o "$T/xess_test" "$here/xess_test.cpp" || { echo "FAIL: xess_test did not build"; exit 1; }
"$T/xess_test" || { echo "FAIL: xess_test"; exit 1; }

python3 -I - "$src/present12.cpp" <<'PY' || { echo "FAIL: XeSS-FG format rule in present12.cpp"; exit 1; }
import re, sys
source = open(sys.argv[1], encoding='utf-8', errors='replace').read().replace('\r\n', '\n')

def bodies(src):
    # Top-level function bodies: a line starting a definition, up to the next line that is a lone closing brace.
    return {name: body for name, body in re.findall(r'^\S[^\n;]*?\b(\w+)\([^\n]*\{\n(.*?)^\}', src, re.M | re.S)}

RULE = r'\bxessFgWanted\('

def problems(src):
    out, b = [], bodies(src)
    asks = {n for n, body in b.items() if re.search(RULE, body)}
    if 'p12Init' not in asks:
        out.append('p12Init does not ask xessFgWanted for the first format')
    # A recreated or resized game swap chain asks the rule for its new format, itself or through a function it calls.
    for name in ('reattach', 'hk_ResizeBuffers'):
        if name not in b:
            out.append(f'no {name}')
        elif name not in asks and not any(re.search(r'\b' + h + r'\(', b[name]) for h in asks - {'p12Init'}):
            out.append(f'{name} never asks the XeSS-FG format rule for the new format')
    # The rule only through xessFgWanted, and the refusal follows it instead of being set for the whole launch.
    if re.search(r'\bxessFgFormatOk\(', src):
        out.append('present12.cpp calls xessFgFormatOk directly instead of xessFgWanted')
    if re.search(r'\bg_xessRefused = (true|1)\b', src):
        out.append('g_xessRefused is set true outright: refused for the launch, not for the current format')
    # Every function that asks the rule records its answer in g_xessRefused (the menu's HDR message), itself or
    # through a function it calls, so the refusal follows the current format.
    writers = {n for n, body in b.items() if re.search(r'\bg_xessRefused = ', body)}
    for n in sorted(asks):
        if n not in writers and not any(re.search(r'\b' + w + r'\(', b[n]) for w in writers):
            out.append(f'{n} asks xessFgWanted but never updates g_xessRefused')
    # Our swap chain is resized, or Intel's created, only where the rule has just been asked.
    for call, what in ((r'g_sc->ResizeBuffers\(', 'g_sc->ResizeBuffers'), (r'\bxessFgSwapchain\(', 'xessFgSwapchain')):
        for n, body in b.items():
            if re.search(call, body) and n not in asks:
                out.append(f'{n} calls {what} without asking xessFgWanted')
    return out

fails = 0
got = problems(source)
for p in got:
    print('FAIL:', p); fails += 1

def mutate(what, old, new):
    global fails
    if old not in source or old == new:
        print(f'WRONG: fixture "{what}" found nothing to change'); fails += 1; return
    caught = bool(problems(source.replace(old, new, 1)))
    print(('ok' if caught else 'WRONG') + f': check fails on a copy where {what}')
    fails += 0 if caught else 1

if not got:
    b = bodies(source)
    helpers = [n for n in b if n != 'p12Init' and re.search(RULE, b[n])]
    for name in ('reattach', 'hk_ResizeBuffers'):
        body = b[name]
        m = next((m for h in helpers for m in [re.search(r'\b' + h + r'\([^;]*\)', body)] if m), None) or re.search(RULE + r'[^;]*', body)
        if not m:
            print(f'WRONG: no rule call in {name} to mutate'); fails += 1; continue
        mutate(f'{name} no longer asks the rule', body, body.replace(m.group(0), 'g_sc->ResizeBuffers(0, w, h, DXGI_FORMAT(0), g_scFlags)'))
    init = b['p12Init']
    at = init.find('    logf("fg: backend %s')
    mutate('a one-time refusal is back in p12Init', init,
           init[:at] + '    if (g_xessFg && !xessFgFormatOk(desc->Format)) { g_xessFg = false; g_xessRefused = true; }\n' + init[at:])
    rb = b['hk_ResizeBuffers']
    at = rb.find('    releaseTargets();\n')
    mutate('hk_ResizeBuffers resizes our swap chain itself', rb, rb[:at] + '    g_sc->ResizeBuffers(0, w, h, f, g_scFlags);\n' + rb[at:])
    for n in helpers + ['p12Init']:
        body = b[n]
        m = re.search(r'^[^\n]*\b(noteXessFormat\(|g_xessRefused = )[^\n]*\n', body, re.M)
        if not m:
            print(f'WRONG: no refusal update in {n} to mutate'); fails += 1; continue
        mutate(f'{n} no longer updates the refusal', body, body.replace(m.group(0), ''))
sys.exit(1 if fails else 0)
PY
echo "PASS: XeSS rules"
