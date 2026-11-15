#!/bin/bash
# Checks the menu's frame on the game's own swap chain (src/native_frame.h, menu.cpp, main.cpp): builds
# tests/native_frame_test.cpp with the host compiler and runs it (100,000 randomised frames with counting fakes), also
# against broken copies of the header (each must fail); then checks that the native menu frame goes through nativeFrame
# with the real D3D11 operations, that no swap chain buffer is fetched or kept outside it, that the composite shader is
# compiled through shaderCompiler(), and that the pass-through flag main.cpp's hooks read is set and cleared only by the
# frame's save and restore. Each source check must fail on a broken copy.
# Usage: tests/check_native_frame.sh [source folder]   (default: src next to tests)
here=$(cd "$(dirname "$0")" && pwd)
src=$(cd "${1:-$here/../src}" && pwd) || exit 1
T=$(mktemp -d) || exit 1
trap 'rm -rf "$T"' EXIT

flags=(-std=c++17 -O2 -Wall -Wextra -Werror)
g++ "${flags[@]}" -I "$src" -o "$T/native_frame_test" "$here/native_frame_test.cpp" \
    || { echo "FAIL: native_frame_test did not build"; exit 1; }
"$T/native_frame_test" || { echo "FAIL: native_frame_test"; exit 1; }

# Broken headers: the stress test must fail on each (built without -Werror: a broken frame may leave a parameter unused).
python3 -I - "$src/native_frame.h" "$T" <<'PY' || exit 1
import os, re, sys
s = open(sys.argv[1], encoding='utf-8').read()
start = s.index('template <class Ops> NativeFrameResult nativeFrame(Ops& o, bool wanted, NativeTargetKey& built) {\n')
noop = s[:start] + ('template <class Ops> NativeFrameResult nativeFrame(Ops& o, bool wanted, NativeTargetKey& built) {\n'
                    '    (void)o; (void)wanted; (void)built;\n    return kNfNotWanted;\n}\n')
broken = {
    'release': s.replace('    o.releaseBuffer();\n', '', 1),
    'restore': s.replace('            if (!o.draw()) r = kNfDrawFailed;\n            o.restore();',
                         '            if (!o.draw()) r = kNfDrawFailed;\n            else o.restore();', 1),
    'rebuild': s.replace('if (o.rebuild(k)) built = k;', 'if (true) built = k;', 1),
    'acquire': s.replace('    if (!wanted) return kNfNotWanted;',
                         '    if (!wanted) { if (o.acquire()) o.releaseBuffer(); return kNfNotWanted; }', 1),
    'noop': noop,
}
for name, b in broken.items():
    if b == s: print('FAIL: could not build the broken header:', name); sys.exit(1)
    os.makedirs(os.path.join(sys.argv[2], name))
    open(os.path.join(sys.argv[2], name, 'native_frame.h'), 'w', encoding='utf-8').write(b)
PY
for b in release restore rebuild acquire noop; do
    g++ -std=c++17 -O2 -I "$T/$b" -o "$T/$b/t" "$here/native_frame_test.cpp" || { echo "FAIL: broken header $b did not build"; exit 1; }
    if "$T/$b/t" >/dev/null; then echo "FAIL: native_frame_test passes on the broken header: $b"; exit 1; fi
done

python3 -I - "$src/menu.cpp" "$src/main.cpp" "$src/present12.cpp" "$src/native_frame.h" <<'PY' || exit 1
import re, sys
read = lambda p: open(p, encoding='utf-8', errors='replace').read().replace('\r\n', '\n')
menu, main, p12, head = (read(p) for p in sys.argv[1:5])

def body(src, name):
    m = re.search(r'^\S[^;{}]*?\b' + name + r'\([^;{}]*\{\n(.*?)^\}', src, re.M | re.S)  # signature may span lines
    return re.sub(r'//[^\n]*', '', m.group(1)) if m else None

def strip(src): return re.sub(r'//[^\n]*', '', src)

def problems(menu, main, p12, head):
    out = []
    frame, ops, comp, objs, srv = (body(menu, 'menuNativeFrame'), re.search(r'^struct D3D11FrameOps \{\n(.*?)^\};', menu, re.M | re.S),
                                   body(menu, 'compileComposite'), body(menu, 'nativeObjects'), body(main, 'hk_PSSetShaderResources'))
    if None in (frame, ops, comp, objs, srv): return ['a function under check was not found']
    ops = strip(ops.group(1))
    # the native menu frame: the real operations, through nativeFrame, wanted only when the counter or menu shows
    if not re.search(r'D3D11FrameOps ops\{ sc \};\s*if \(nativeFrame\(ops, wanted, g_nativeBuilt\) == kNfDrawn\) \+\+g_nativeDrawn;', frame):
        out.append('menuNativeFrame does not draw through nativeFrame with the D3D11 operations')
    if 'const bool wanted = (g_counter || g_open || g_padRelease) && compose(nullptr, 0, false);' not in frame:
        out.append('the native frame is wanted other than when the counter or menu shows')
    if 'GetBuffer' in frame: out.append('menuNativeFrame fetches a swap chain buffer itself')
    # no swap chain buffer outside the frame: menu.cpp fetches one in D3D11FrameOps::acquire and in the D3D12 path only
    nm = strip(menu)
    gets = [m.start() for m in re.finditer(r'->GetBuffer\(', nm)]
    acq = nm.find('if (FAILED(sc->GetBuffer(0, IID_PPV_ARGS(&bb)))) { bb = nullptr; return false; }')
    d12 = nm.find('else if (FAILED(sc->GetBuffer(index, IID_PPV_ARGS(&bb)))) return false;')
    if acq < 0 or d12 < 0 or sorted(gets) != sorted([acq + len('if (FAILED(sc'), d12 + len('else if (FAILED(sc')]):
        out.append('menu.cpp fetches a swap chain buffer outside D3D11FrameOps::acquire and the D3D12 path')
    compose = body(menu, 'compose')
    if compose is None or compose.find('if (g_native) { g_nativeBox = box; return true; }') < 0 or \
       compose.find('if (g_native) { g_nativeBox = box; return true; }') > compose.find('GetBuffer('):
        out.append('compose reaches the D3D12 buffer fetch natively')
    # the buffer and view live in the operations for one frame and are released there
    for need, what in (('ID3D11Texture2D* bb = nullptr;', 'the buffer is not a member of the frame'),
                       ('ID3D11RenderTargetView* rtv = nullptr;', 'the view is not a member of the frame'),
                       ('void releaseView() { release(rtv); }', 'the view is not released'),
                       ('void releaseBuffer() { release(bb); }', 'the buffer is not released')):
        if need not in ops: out.append(what)
    if re.search(r'^(static )?ID3D11Texture2D\* g_(bb|back)', nm, re.M): out.append('a back buffer is kept in a global')
    for name in ('nativeInit', 'p12NativeAttach', 'p12NativeBefore', 'p12NativeAfter'):
        b = body(p12, name)
        if b is None: out.append(name + ' was not found'); continue
        if 'GetBuffer' in b: out.append(name + ' fetches a swap chain buffer')
    # the composite shader: compiled through shaderCompiler() in the one helper both renderers use
    if 'const pD3DCompile compile = shaderCompiler();' not in comp: out.append('compileComposite does not use shaderCompiler()')
    if 'compileComposite(&vs, &ps, &err)' not in objs: out.append('the native composite is not compiled through compileComposite')
    if re.search(r'\bD3DCompile\(', nm): out.append('menu.cpp calls D3DCompile directly')
    # the pass-through flag: set by save before the game's state is saved, cleared by restore after it is put back
    if 'void save() { g_nativeDrawing = true; g_saved.save(g_ctx11); }' not in ops: out.append('save does not set the pass-through flag')
    if 'void restore() { g_saved.restore(g_ctx11); g_nativeDrawing = false; }' not in ops: out.append('restore does not clear the pass-through flag')
    sets = sum(len(re.findall(r'\bg_nativeDrawing = ', strip(s))) for s in (menu, main, p12))
    if sets != 2: out.append('the pass-through flag is assigned %d times, expected 2 (save and restore)' % sets)
    first = [l.strip() for l in srv.split('\n') if l.strip() and not l.strip().startswith('using ')][:1]
    if first != ['if (g_nativeDrawing) return orig<F>(ctx, 8)(ctx, start, n, srv);']:
        out.append('hk_PSSetShaderResources does not pass the native frame\'s calls straight through first')
    # the header stays free of Windows types so the host test covers it
    if re.search(r'#include\s*[<"](windows|d3d|dxgi)', head, re.I): out.append('native_frame.h includes Windows headers')
    return out

p = problems(menu, main, p12, head)
if p:
    for x in p: print('FAIL:', x)
    sys.exit(1)
broken = {
    'frame not through nativeFrame': (menu.replace('if (nativeFrame(ops, wanted, g_nativeBuilt) == kNfDrawn) ++g_nativeDrawn;',
                                                   'if (wanted && ops.acquire() && ops.view() && ops.draw()) ++g_nativeDrawn;', 1), main, p12, head),
    'always wanted': (menu.replace('const bool wanted = (g_counter || g_open || g_padRelease) && compose(nullptr, 0, false);',
                                   'const bool wanted = compose(nullptr, 0, false);', 1), main, p12, head),
    'buffer fetched in the frame function': (menu.replace('    D3D11FrameOps ops{ sc };\n',
                                                          '    ID3D11Texture2D* b0 = nullptr; sc->GetBuffer(0, IID_PPV_ARGS(&b0));\n    D3D11FrameOps ops{ sc };\n', 1), main, p12, head),
    'buffer kept in a global': (menu.replace('void menuNativeStats(', 'ID3D11Texture2D* g_back;\nvoid menuNativeStats(', 1), main, p12, head),
    'buffer fetched elsewhere': (menu.replace('void menuNativeStats(', 'void peek(IDXGISwapChain* sc) { ID3D11Texture2D* t; sc->GetBuffer(0, IID_PPV_ARGS(&t)); }\nvoid menuNativeStats(', 1), main, p12, head),
    'buffer not released': (menu.replace('void releaseBuffer() { release(bb); }', 'void releaseBuffer() {}', 1), main, p12, head),
    'view not released': (menu.replace('void releaseView() { release(rtv); }', 'void releaseView() {}', 1), main, p12, head),
    'native compose reaches the D3D12 path': (menu.replace('if (g_native) { g_nativeBox = box; return true; }', 'g_nativeBox = box;', 1), main, p12, head),
    'buffer fetched after the Present': (menu, main, p12.replace('    ++g_presents;\n', '    { ID3D11Texture2D* t; if (SUCCEEDED(sc->GetBuffer(0, IID_PPV_ARGS(&t)))) t->Release(); }\n    ++g_presents;\n', 1), head),
    'composite not through shaderCompiler': (menu.replace('const pD3DCompile compile = shaderCompiler();', 'const pD3DCompile compile = D3DCompile;', 1), main, p12, head),
    'native composite compiled apart': (menu.replace('    HRESULT hr = compileComposite(&vs, &ps, &err);\n    if (SUCCEEDED(hr)) hr = g_dev11->CreateVertexShader',
                                                     '    HRESULT hr = D3DCompile(nullptr, 0, nullptr, nullptr, nullptr, "vs", "vs_5_0", 0, 0, &vs, &err);\n    if (SUCCEEDED(hr)) hr = g_dev11->CreateVertexShader', 1), main, p12, head),
    'flag not set': (menu.replace('void save() { g_nativeDrawing = true; g_saved.save(g_ctx11); }', 'void save() { g_saved.save(g_ctx11); }', 1), main, p12, head),
    'flag set after the save': (menu.replace('void save() { g_nativeDrawing = true; g_saved.save(g_ctx11); }', 'void save() { g_saved.save(g_ctx11); g_nativeDrawing = true; }', 1), main, p12, head),
    'flag not cleared': (menu.replace('void restore() { g_saved.restore(g_ctx11); g_nativeDrawing = false; }', 'void restore() { g_saved.restore(g_ctx11); }', 1), main, p12, head),
    'flag cleared before the restore': (menu.replace('void restore() { g_saved.restore(g_ctx11); g_nativeDrawing = false; }', 'void restore() { g_nativeDrawing = false; g_saved.restore(g_ctx11); }', 1), main, p12, head),
    'flag set elsewhere': (menu, main, p12.replace('    ++g_presents;\n', '    g_nativeDrawing = false;\n    ++g_presents;\n', 1), head),
    'no pass-through for shader resources': (menu, main.replace('    if (g_nativeDrawing) return orig<F>(ctx, 8)(ctx, start, n, srv);', '', 1), p12, head),
    'Windows header': (menu, main, p12, head.replace('#pragma once', '#pragma once\n#include <windows.h>', 1)),
}
for name, files in broken.items():
    if files == (menu, main, p12, head): print('FAIL: could not build the broken copy:', name); sys.exit(1)
    if not problems(*files): print('FAIL: check passed on the broken copy:', name); sys.exit(1)
print('native frame checks passed (and fail on %d broken copies)' % len(broken))
PY
echo "PASS: native frame"
