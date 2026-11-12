#!/usr/bin/env bash
# Checks a built dxgi.dll: it must carry the mod's marker text and none of the developer-only names.
# Usage: tests/check_dll.sh <dxgi.dll> [<path that must not appear> ...]
# Each text is searched as ASCII and as UTF-16; a path is also searched with backslashes.
set -u
dll=${1:?usage: check_dll.sh <dxgi.dll> [path ...]}
shift
exec python3 -I - "$dll" "$@" <<'PY'
import sys
dll, paths = sys.argv[1], sys.argv[2:]
data = open(dll, 'rb').read()
def count(text):
    return sum(data.count(text.encode(enc)) for enc in ('ascii', 'utf-16-le'))
bad = 0
for text in ['ffxv_dlss_shaders', '.dxbc', 'uidump', 'ffxv_dlss_ui', 'ffxv_dlss.trigger']:
    n = count(text)
    print(f'{text}: {n}')
    bad += n != 0
for p in paths:
    for form in {p, p.replace('/', '\\')}:
        n = sum(data.count(form.encode(enc)) for enc in ('utf-8', 'utf-16-le'))
        print(f'path {form}: {n}')
        bad += n != 0
n = count('ffxv-dlss')
print(f'ffxv-dlss: {n}')
bad += n == 0
print('FAIL' if bad else 'PASS')
sys.exit(1 if bad else 0)
PY
