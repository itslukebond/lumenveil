#!/bin/bash
# Checks the folders the mod gives NVIDIA's runtime at start-up (initNgx in src/dlss.cpp). Frame generation's file,
# nvngx_dlssg.dll, lives with Streamline's in ffxv_dlss\streamline; when that folder is not in the list, the runtime
# quietly loads the older copy that comes with the driver. The list must hold ffxv_dlss\streamline first, then
# ffxv_dlss (where nvngx_dlss.dll lives), and the count passed must match. Each check must fail on a broken copy.
# Usage: tests/check_ngx_paths.sh [source folder holding dlss.cpp]   (default: src next to tests)
here=$(cd "$(dirname "$0")" && pwd)
src=$(cd "${1:-$here/../src}" && pwd) || exit 1

python3 -I - "$src/dlss.cpp" <<'EOF'
import re, sys
text = open(sys.argv[1], encoding='utf-8', errors='replace').read().replace('\r\n', '\n')

def body(src, head):
    i = src.find(head)
    if i < 0: return None
    i = src.find('{', i)
    depth = 0
    for j in range(i, len(src)):
        depth += {'{': 1, '}': -1}.get(src[j], 0)
        if depth == 0: return src[i:j + 1]
    return None

def problems(src):
    b = body(src, 'bool initNgx()')
    if b is None: return ['initNgx() not found']
    out = []
    # name -> the folder it holds, from the static wstring definitions in initNgx
    dirs = {}
    for name, expr in re.findall(r'static std::wstring (\w+) = ([^;]+);', b):
        expr = expr.strip()
        m = re.fullmatch(r'path\(L"([^"]+)"\)', expr)
        if m: dirs[name] = m.group(1); continue
        m = re.fullmatch(r'(\w+) \+ L"([^"]+)"', expr)
        if m and m.group(1) in dirs: dirs[name] = dirs[m.group(1)] + m.group(2).replace('\\\\', '\\')
    m = re.search(r'const wchar_t\* paths\[\] = \{([^}]*)\};', b)
    if not m: return ['initNgx() has no paths[] list']
    listed = [x.strip() for x in m.group(1).split(',') if x.strip()]
    got = []
    for x in listed:
        n = re.fullmatch(r'(\w+)\.c_str\(\)', x)
        got.append(dirs.get(n.group(1)) if n else None)
    want = ['ffxv_dlss\\streamline', 'ffxv_dlss']
    if got != want: out.append('paths[] holds %r, expected %r' % (got, want))
    n = re.search(r'fi\.PathListInfo\.Length = ([^;]+);', b)
    if not n: out.append('PathListInfo.Length is not set')
    elif n.group(1).strip() not in (str(len(listed)), '_countof(paths)'):
        out.append('PathListInfo.Length is %s for %d folders' % (n.group(1).strip(), len(listed)))
    if not re.search(r'fi\.PathListInfo\.Path = paths;', b): out.append('PathListInfo.Path is not paths')
    return out

p = problems(text)
if p:
    for x in p: print('FAIL:', x)
    sys.exit(1)

b = body(text, 'bool initNgx()')
lst = re.search(r'const wchar_t\* paths\[\] = \{([^}]*)\};', b).group(0)
items = [x.strip() for x in re.search(r'\{([^}]*)\}', lst).group(1).split(',')]
broken = {
    'streamline missing': text.replace(lst, 'const wchar_t* paths[] = { %s };' % items[-1], 1),
    'order swapped': text.replace(lst, 'const wchar_t* paths[] = { %s };' % ', '.join(reversed(items)), 1),
    'length one': re.sub(r'fi\.PathListInfo\.Length = [^;]+;', 'fi.PathListInfo.Length = 1;', text, count=1),
}
for name, x in broken.items():
    if x == text: print('FAIL: could not build the broken copy:', name); sys.exit(1)
    if not problems(x): print('FAIL: check passed on the broken copy:', name); sys.exit(1)
print('dlss.cpp NGX folder checks passed (and fail on 3 broken copies)')
EOF
