#!/bin/bash
# Checks the start-up gate for frame generation (src/fg_gate.h) and the backend choice (src/fg_select.h): builds
# tests/gate_test.cpp and tests/gate_first_load_test.cpp with the host compiler and runs them, then checks where present12.cpp uses the gate, that it starts
# exactly one backend (or none) from fgPickBackend, that the menu resolves Auto with the same function, and that each
# of those checks fails on a broken copy.
# Usage: tests/check_gate.sh [source folder holding fg_gate.h and present12.cpp]   (default: src next to tests)
here=$(cd "$(dirname "$0")" && pwd)
src=$(cd "${1:-$here/../src}" && pwd) || exit 1
T=$(mktemp -d) || exit 1
trap 'rm -rf "$T"' EXIT

g++ -std=c++17 -O2 -Wall -Wextra -Werror -I "$src" -o "$T/gate_test" "$here/gate_test.cpp" || { echo "FAIL: gate_test did not build"; exit 1; }
"$T/gate_test" || { echo "FAIL: gate_test"; exit 1; }
g++ -std=c++17 -O2 -Wall -Wextra -Werror -I "$src" -o "$T/gate_first_load_test" "$here/gate_first_load_test.cpp" || { echo "FAIL: gate_first_load_test did not build"; exit 1; }
"$T/gate_first_load_test" || { echo "FAIL: gate_first_load_test"; exit 1; }

python3 -I - "$src/present12.cpp" "$T" "$src/menu.cpp" <<'EOF'
import re, sys
path, tmp, menu_path = sys.argv[1], sys.argv[2], sys.argv[3]
menu_src = open(menu_path, encoding='utf-8', errors='replace').read().replace('\r\n', '\n')
crlf_src = open(path, encoding='utf-8', errors='replace').read()
source = crlf_src.replace('\r\n', '\n')

def bodies(src):
    # Top-level function bodies: a line starting a definition, up to the next line that is a lone closing brace.
    return {name: body for name, body in re.findall(r'^\S[^\n;]*?\b(\w+)\([^\n]*\{\n(.*?)^\}', src, re.M | re.S)}

def problems(src):
    out, b = [], bodies(src)
    fg = b.get('fgBeforePresent')
    if fg is None:
        return ['no fgBeforePresent']
    # The gate sees every presented frame: called at the top level of the function, not inside a condition.
    if fg.count('g_gate.frame(') != 1 or not re.search(r'^    const bool mayStart = g_gate\.frame\(tagged, GetTickCount64\(\)\);$', fg, re.M):
        out.append('g_gate.frame(tagged, ...) is not called on every frame in fgBeforePresent')
    # The switch-on condition asks the gate, before any drain wait.
    on = re.search(r'\bif \(((?:[^;])*?)\)\s*fgSetMode\(true\);', fg, re.S)
    if not on:
        out.append('no fgSetMode(true) condition in fgBeforePresent')
    else:
        cond = on.group(1)
        if 'mayStart' not in cond:
            out.append('the fgSetMode(true) condition does not use mayStart')
        elif 'fgDrained()' in cond and cond.index('mayStart') > cond.index('fgDrained()'):
            out.append('mayStart comes after fgDrained() in the fgSetMode(true) condition')
    # A resize or a recreated swapchain in the world must not close the gate again.
    for name in ('hk_ResizeBuffers', 'reattach'):
        if name not in b:
            out.append(f'no {name}')
        elif 'g_gate' in b[name]:
            out.append(f'{name} touches g_gate')
    out += selection_problems(src)
    return out

FLAGS = {'0': 'g_fg', '1': 'g_fsrFg', '2': 'g_xessFg'}

def selection_problems(src, menu=None):
    # One backend per launch: p12Init takes it from fgPickBackend and a switch sets exactly one flag per case, so
    # DLSS-G, FSR FG and XeSS-FG are never on together; nothing else ever sets a flag true.
    out = []
    init = bodies(src).get('p12Init')
    if init is None:
        return ['no p12Init']
    if not re.search(r'= fgPickBackend\(setting, g_vendor, ', init):
        out.append('p12Init does not take the backend from fgPickBackend(setting, g_vendor, ...)')
    sw = re.search(r'switch \(backend\) \{\n(.*?)\n    \}', init, re.S)
    if not sw:
        return out + ['no switch (backend) in p12Init']
    cases = re.findall(r'^    case (\d+):(.*?)$', sw.group(1), re.M)
    if sorted(c for c, _ in cases) != ['0', '1', '2']:
        out.append('the backend switch does not have exactly cases 0, 1 and 2')
    for c, line in cases:
        set_ = re.findall(r'\b(g_fg|g_fsrFg|g_xessFg) = ', line)
        if set_ != [FLAGS.get(c)] or not re.search(r'\bbreak;', line):
            out.append(f'case {c} does not set only {FLAGS.get(c)} and break')
    rest = src.replace(sw.group(0), '')
    for flag, rhs in re.findall(r'\b(g_fg|g_fsrFg|g_xessFg) = ([^;]*);', rest):
        if rhs.strip() not in ('false', 'r == sl::Result::eOk') and not re.fullmatch(r'(g_fg|g_fsrFg|g_xessFg) = .*false', rhs.strip()):
            out.append(f'{flag} is set outside the backend switch: {flag} = {rhs}')
    m = menu if menu is not None else menu_src
    if not re.search(r'auto resolve = \[&\]\(int b\) \{ return fgPickBackend\(b, g_vendorId, ', m):
        out.append('menu.cpp does not resolve the backend with fgPickBackend')
    return out

got = problems(source)
for line in got:
    print('problem:', line)
fails = len(got)
print(('PASS' if not got else 'FAIL') + ': gate placement in ' + path)

# Each check must catch a broken copy.
def mutate(name, old, new):
    global fails
    if old not in source:
        print(f'WRONG: fixture "{name}": text to change not found'); fails += 1; return
    bad = problems(source.replace(old, new, 1))
    print(('ok' if bad else 'WRONG') + f': fixture "{name}" ' + ('fails: ' + '; '.join(bad) if bad else 'passes'))
    fails += 0 if bad else 1

call = re.search(r'^    const bool mayStart = g_gate\.frame\(tagged,[^\n]*\n', source, re.M)
if call:
    mutate('gate call deleted', call.group(0), '    const bool mayStart = true;\n')
    mutate('gate called only when a frame is tagged', call.group(0), '    bool mayStart = false; if (tagged) ' + call.group(0).strip().replace('const bool mayStart', 'mayStart') + '\n')
else:
    print('WRONG: no "const bool mayStart = g_gate.frame(tagged, ...)" line to mutate'); fails += 1
cond = re.search(r'g_set\.enable && mayStart\n(\s*)&& \(', source)
if cond and 'fgDrained()' in source:
    m = re.search(r'(g_set\.enable) && mayStart\n(\s*&& \(.*?)(\)\)\) fgSetMode\(true\);)', source, re.S)
    if m:
        mutate('mayStart after fgDrained()', m.group(0), m.group(1) + '\n' + m.group(2) + ' && mayStart' + m.group(3))
    else:
        print('WRONG: could not build the "mayStart after fgDrained()" fixture'); fails += 1
else:
    print('WRONG: no "g_set.enable && mayStart" line followed by the Wine term'); fails += 1
at = source.find('    releaseTargets();\n', source.find('\nHWND reattach('))
if at < 0:
    print('WRONG: no releaseTargets() line in reattach to mutate'); fails += 1
else:
    rest = source[at:]
    mutate('reattach resets the gate', rest, '    g_gate = {};\n' + rest)
mutate('XeSS-FG case also starts FSR FG', 'case 2: g_xessFg = ', 'case 2: g_fsrFg = true; g_xessFg = ')
mutate('FSR FG case falls through', "    case 1: g_fsrFg = g_set.enable && fsrLoad(); break;", "    case 1: g_fsrFg = g_set.enable && fsrLoad();")
mutate('Auto decided inline', 'fgPickBackend(setting, g_vendor, ', 'setting >= 0 ? setting : fgPickBackend(setting, g_vendor, ')
mutate('a flag set after the switch', '    logf("fg: backend %s', '    g_xessFg = xessFgPresent();\n    logf("fg: backend %s')
bad = selection_problems(source, menu_src.replace('return fgPickBackend(b, g_vendorId, ', 'return b >= 0 ? b : fgPickBackend(b, g_vendorId, '))
print(('ok' if bad else 'WRONG') + ': fixture "menu resolves Auto itself" ' + ('fails: ' + '; '.join(bad) if bad else 'passes'))
fails += 0 if bad else 1
sys.exit(1 if fails else 0)
EOF
