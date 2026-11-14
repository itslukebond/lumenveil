#!/bin/bash
# Checks that every shader the mod compiles at run time uses shaderCompiler() (src/main.cpp), and that shaderCompiler()
# loads the game folder's d3dcompiler_47.dll before System32's. Proton's own d3dcompiler_47 (vkd3d-shader before 2.0)
# cannot build some of the mod's shaders; the game folder's copy is Microsoft's compiler on Windows and Proton alike.
# Usage: tests/check_shader_compiler.sh [source folder]   (default: src next to tests)
here=$(cd "$(dirname "$0")" && pwd)
src=$(cd "${1:-$here/../src}" && pwd) || exit 1

python3 -I - "$src" <<'EOF'
import os, re, sys
src = sys.argv[1]

def read(name):
    return open(os.path.join(src, name), encoding='utf-8', errors='replace').read().replace('\r\n', '\n')

def body(text, head):
    i = text.find(head)
    if i < 0: return None
    i = text.find('{', i)
    depth = 0
    for j in range(i, len(text)):
        depth += {'{': 1, '}': -1}.get(text[j], 0)
        if depth == 0: return text[i:j + 1]
    return None

problems = []
main = read('main.cpp')
sc = body(main, 'pD3DCompile shaderCompiler()')
if sc is None:
    problems.append('main.cpp: shaderCompiler() not found')
else:
    flat = re.sub(r'\s+', ' ', sc)
    game = flat.find('if (auto f = load(path(L"d3dcompiler_47.dll"))) return f;')
    system = flat.find('GetSystemDirectoryW(')
    fallback = re.search(r'return [^;]*\bload\(std::wstring\(sys, n\) \+ L"\\\\d3dcompiler_47\.dll"\)[^;]*;', flat)
    if game < 0: problems.append('shaderCompiler() does not return the game folder\'s D3DCompile when it loads')
    if system < 0 or not fallback: problems.append('shaderCompiler() does not return System32\'s D3DCompile as the fallback')
    if game >= 0 and system >= 0 and system < game: problems.append('shaderCompiler() tries System32 before the game folder')
    if 'GetProcAddress(m, "D3DCompile")' not in flat or not re.search(r'return f; \};', flat):
        problems.append('shaderCompiler()\'s loader does not return the D3DCompile it found')
    if not re.search(r'return fn; \}$', flat): problems.append('shaderCompiler() does not return the compiler it chose')

for name in sorted(os.listdir(src)):
    if not name.endswith(('.cpp', '.h')): continue
    text = read(name)
    if name == 'main.cpp' and sc is not None: text = text.replace(sc, '')
    for n, line in enumerate(text.split('\n'), 1):
        code = line.split('//', 1)[0]
        if re.search(r'\bD3DCompile\s*\(', code) or 'd3dcompiler_47' in code:
            problems.append(f'{name}:{n}: compiles or loads the compiler outside shaderCompiler(): {line.strip()}')

# Every run-time compile: dlss.cpp's compute shaders, main.cpp's HUD and font shaders and the game sharpening
# shader, menu.cpp's menu shaders.
for name, want in (('dlss.cpp', 1), ('main.cpp', 3), ('menu.cpp', 1)):
    text = read(name)
    if name == 'main.cpp' and sc is not None: text = text.replace(sc, '')
    got = len(re.findall(r'const pD3DCompile compile = shaderCompiler\(\);', text))
    if got != want: problems.append(f'{name}: {got} shader compile sites use shaderCompiler(), expected {want}')

for p in problems: print('FAIL:', p)
sys.exit(1 if problems else 0)
EOF
status=$?
[ $status -eq 0 ] && echo "PASS: shader compiler" || echo "FAIL: shader compiler"
exit $status
