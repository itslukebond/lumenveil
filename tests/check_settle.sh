#!/bin/bash
# Under Wine, frame generation switches back on only after a settled second of valid frames (fgBeforePresent times it
# from the frame where g_validRun becomes 1). Every function in src/present12.cpp that releases the render targets
# (a resize, a recreated game swapchain) must restart that window, or the next valid frame switches FG on at once.
# Usage: tests/check_settle.sh [present12.cpp]
f=${1:-"$(dirname "$0")/../src/present12.cpp"}
python3 -I - "$f" <<'EOF'
import re, sys
src = open(sys.argv[1], encoding='utf-8', errors='replace').read().replace('\r\n', '\n')
# Top-level function bodies: a line starting a definition, up to the next line that is a lone closing brace.
funcs = re.findall(r'^\S[^\n;]*?\b(\w+)\([^\n]*\{\n(.*?)^\}', src, re.M | re.S)
callers = [(name, body) for name, body in funcs if re.search(r'\breleaseTargets\(\);', body)]
if not callers:
    sys.exit('FAIL: found no function that releases the render targets')
bad = [name for name, body in callers if not re.search(r'\bg_validRun\s*=\s*0\s*;', body)]
for name, body in callers:
    print(f'{name}: releases targets, restarts settle window: {"no" if name in bad else "yes"}')
if bad:
    sys.exit('FAIL: settle window not restarted in: ' + ', '.join(bad))
print(f'PASS: {len(callers)} function(s) release the targets, each restarts the settle window')
EOF
