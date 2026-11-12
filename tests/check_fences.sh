#!/bin/bash
# Checks that every fence shared between the D3D11 and D3D12 devices in src/present12.cpp is signalled from one API
# only. A fence signalled from both can be set backwards when the two queues run out of order, and a wait for the
# higher value then never returns. Signals made elsewhere through P12Shared (src/up12.cpp, src/fsr.cpp, src/xess.cpp:
# s.queue for D3D12, s.ctx11 for D3D11) count against the presenter's fence they name, and any other Signal call there
# fails.
# Usage: tests/check_fences.sh [present12.cpp [other.cpp ...]]   (default: the files in src next to tests)
here=$(cd "$(dirname "$0")" && pwd)
f=${1:-"$here/../src/present12.cpp"}
[ $# -gt 0 ] && shift
[ $# -gt 0 ] || set -- "$here/../src/up12.cpp" "$here/../src/fsr.cpp" "$here/../src/xess.cpp"
python3 -I - "$f" "$here/../src/common.h" "$@" <<'EOF'
import re, sys
src = open(sys.argv[1], encoding='utf-8', errors='replace').read()
header = open(sys.argv[2], encoding='utf-8', errors='replace').read()
others = sys.argv[3:]
# Pairs: CreateSharedHandle(<d3d12 fence>, ...) followed by OpenSharedFence(h, IID_PPV_ARGS(&<d3d11 fence>))
pairs = re.findall(r'CreateSharedHandle\((\w+),[^;]*;\s*if \(SUCCEEDED\(hr\)\) hr = \w+->OpenSharedFence\(h, IID_PPV_ARGS\(&(\w+)\)\)', src)
if not pairs:
    sys.exit('FAIL: found no shared fence pairs to check')
s12 = set(re.findall(r'g_d3d12Queue->Signal\((\w+),', src))
s11 = set(re.findall(r'g_ctx11->Signal\((\w+),', src))

# P12Shared: field names from common.h, matched in order to the presenter's initializer in p12Shared()
if others:
    struct = re.search(r'struct P12Shared \{(.*?)\};', header, re.S)
    init = re.search(r'\*s = \{([^}]*)\};', src)
    if not struct or not init:
        sys.exit('FAIL: P12Shared or its initializer in p12Shared() not found, so the other files cannot be checked')
    fields = re.findall(r'(\w+);', re.sub(r'//[^\n]*', '', struct.group(1)).replace(',', ';'))
    values = [v.strip().lstrip('&') for v in init.group(1).split(',')]
    if len(fields) != len(values):
        sys.exit(f'FAIL: P12Shared has {len(fields)} fields but p12Shared() sets {len(values)}')
    shared = dict(zip(fields, values))
    for path in others:
        code = open(path, encoding='utf-8', errors='replace').read()
        calls = re.findall(r'(\w+(?:\.|->)\w+|\w+)->Signal\(([^,]*),', code)
        for obj, arg in calls:
            m12 = re.fullmatch(r'\w+\.queue', obj)
            m11 = re.fullmatch(r'\w+\.ctx11', obj)
            a = re.fullmatch(r'\s*\w+\.(\w+)\s*', arg)
            if not (m12 or m11) or not a or a.group(1) not in shared:
                sys.exit(f'FAIL: {path}: Signal call this check cannot attribute: {obj}->Signal({arg}, ...)')
            (s12 if m12 else s11).add(shared[a.group(1)])
        print(f'{path}: {len(calls)} signal(s) through P12Shared')

both = [(a, b) for a, b in pairs if a in s12 and b in s11]
none = [(a, b) for a, b in pairs if a not in s12 and b not in s11]
for a, b in pairs:
    print(f'pair {a} (D3D12 signals: {"yes" if a in s12 else "no"}) / {b} (D3D11 signals: {"yes" if b in s11 else "no"})')
if both:
    sys.exit('FAIL: signalled from both APIs: ' + ', '.join(f'{a}/{b}' for a, b in both))
if none:
    sys.exit('FAIL: signalled by neither API, so a wait on it never returns: ' + ', '.join(f'{a}/{b}' for a, b in none))
print(f'PASS: {len(pairs)} shared fence pair(s), each signalled from one API')
EOF
