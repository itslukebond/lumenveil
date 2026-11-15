#!/bin/bash
# Checks the game's D3D11 state saver the native menu frame uses (src/menu.cpp, D3D11State): every state its save()
# reads with a Get call, its restore() writes back with the matching Set call, nothing is set back that was not read,
# and every interface the Get calls hand out is released after the Set calls. Must fail on broken copies.
# Usage: tests/check_native_state.sh [source folder]   (default: src next to tests)
here=$(cd "$(dirname "$0")" && pwd)
src=$(cd "${1:-$here/../src}" && pwd) || exit 1
python3 -I - "$src/menu.cpp" <<'PY' || exit 1
import re, sys
from collections import Counter
menu = open(sys.argv[1], encoding='utf-8', newline='').read().replace('\r\n', '\n')

def body(s, head):
    i = s.find(head)
    if i < 0: return None
    i = s.index('{', i); d = 0
    for j in range(i, len(s)):
        d += s[j] == '{'; d -= s[j] == '}'
        if d == 0: return s[i + 1:j]
    return None

def problems(s):
    out = []
    st = body(s, 'struct D3D11State {')
    if st is None: return ['no D3D11State']
    save, restore = body(st, 'void save(ID3D11DeviceContext* c)'), body(st, 'void restore(ID3D11DeviceContext* c)')
    if save is None or restore is None: return ['no save or restore']
    gets = Counter(re.findall(r'c->(\w\w)Get(\w+)\(', save))
    sets = Counter(re.findall(r'c->(\w\w)Set(\w+)\(', restore))
    if not gets: out.append('save reads nothing')
    for k in gets - sets: out.append('read but not set back: %sGet%s' % k)
    for k in sets - gets: out.append('set back but never read: %sSet%s' % k)
    decl = st[:st.find('void save(')]
    ifaces = set()
    for line in decl.split(';'):
        m = re.match(r'\s*(ID3D11\w+)\*\s*(.*)$', line.strip().replace('\n', ' '))
        if not m: continue
        for part in m.group(2).split(','):
            n = re.match(r'\s*\**\s*(\w+)', part)
            if n: ifaces.add((n.group(1), '[' in part))
    if not ifaces: out.append('no interface members found')
    rel = restore[restore.rfind('->'):] if '->' in restore else restore  # releases come after the last Set call
    for name, arr in sorted(ifaces):
        if arr:
            if not re.search(r'release\(%s\[' % name, rel) and not re.search(r'for \([^)]*:\s*%s\)\s*release\(' % name, rel):
                out.append('array not released: ' + name)
        elif not re.search(r'\brelease\(%s\)' % name, rel):
            out.append('not released: ' + name)
    return out

p = problems(menu)
if p: print('FAIL: ' + '; '.join(p)); sys.exit(1)
broken = {
    'rasterizer not set back': menu.replace('c->RSSetState(rs); ', '', 1),
    'VS constant buffer not set back': menu.replace(' c->VSSetConstantBuffers(0, 1, &vsCb);', '', 1),
    'render targets not set back': menu.replace('        c->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, rtv, dsv);\n', '', 1),
    'compute shader not read': menu.replace(' c->CSGetShader(&cs, ci[5], &nci[5]);', '', 1),
    'depth state not released': menu.replace(' release(dss);', '', 1),
    'render targets not released': menu.replace('        for (auto*& r : rtv) release(r);\n', '', 1),
    'class instances not released': menu.replace('        for (UINT k = 0; k < 6; ++k) for (UINT i = 0; i < nci[k]; ++i) release(ci[k][i]);\n', '', 1),
    'released before set back': menu.replace(' c->PSSetSamplers(0, 1, &psSmp);', '', 1).replace(' release(psSmp);', ' release(psSmp); c->PSSetSamplers(0, 1, &psSmp);', 1),
}
for name, b in broken.items():
    if b == menu: print('FAIL: could not build the broken copy:', name); sys.exit(1)
    if not problems(b): print('FAIL: check passed on the broken copy:', name); sys.exit(1)
print('native state checks passed (and fail on %d broken copies)' % len(broken))
PY
echo "PASS: native state"
