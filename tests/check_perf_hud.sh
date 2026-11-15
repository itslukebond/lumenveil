#!/bin/bash
# Checks the frame rate side of the HUD replacements: the Insert menu has a "Sharper HUD" checkbox in a HUD section
# above Overlay that saves [ui] sharp and switches the replacements through uiSetSharp; main.cpp starts from [ui] sharp
# (on when the key is missing) and has no trigger file for it; 664e is never swapped when bound (the game's second
# object of it draws fills and a glow upsample); the stretch draw keeps the game's own shader when it does not magnify,
# and otherwise gets the light shader on frames in which the game's TAA drew and the full one on others, with the
# game's put back after the draw; the sampler hook reads a bias worked out once per frame instead of
# asking the upscaler for its state on every call. Each check is also run on a copy broken the way it guards against
# and must fail there.
# Usage: tests/check_perf_hud.sh [source folder holding main.cpp and menu.cpp]   (default: src next to tests)
here=$(cd "$(dirname "$0")" && pwd)
src=$(cd "${1:-$here/../src}" && pwd) || exit 1

python3 -I - "$src/main.cpp" "$src/menu.cpp" <<'EOF' || exit 1
import re, sys
read = lambda p: open(p, encoding='utf-8', errors='replace').read().replace('\r\n', '\n')
sources = {'main': read(sys.argv[1]), 'menu': read(sys.argv[2])}

def body(src, name):
    m = re.search(r'^\S[^\n;]*?\b' + name + r'\([^\n]*\{\n(.*?)^\}', src, re.M | re.S)
    return m.group(1) if m else None

CAPTION = 'Crisper HUD text, icons and title screen. Costs a little frame rate at 4K.'

def problems(s):
    out = []
    main, menu = s['main'], s['menu']
    # menu: a HUD section above Overlay, the checkbox with its pad tick, the caption, the switch and the ini key
    hud, overlay = menu.find('SeparatorText("HUD")'), menu.find('SeparatorText("Overlay")')
    sec = menu[hud:overlay] if 0 <= hud < overlay else ''
    if not sec:
        out.append('the menu has no HUD section above Overlay')
    else:
        if 'label("Sharper HUD")' not in sec or not re.search(r'ImGui::Checkbox\("##sharp", &(\w+)\)', sec):
            out.append('the HUD section has no Sharper HUD checkbox')
        if 'pad::padTick(' not in sec:
            out.append('the Sharper HUD checkbox cannot be ticked with a controller')
        if CAPTION not in sec:
            out.append('the Sharper HUD caption is missing or reworded')
        if not re.search(r'uiSetSharp\(\w+\);', sec) or not re.search(r'iniSet\(L"ui", L"sharp", \w+\)', sec):
            out.append('ticking Sharper HUD does not switch the replacements and save [ui] sharp')
    # main: starts from [ui] sharp, on by default, at hook setup; no trigger file or blind toggle left
    hd = body(main, 'hookDevice') or ''
    if not re.search(r'g_uiFix = GetPrivateProfileIntW\(L"ui", L"sharp", 1, ', hd):
        out.append('hookDevice does not start the replacements from [ui] sharp with a default of on')
    if 'ffxv_dlss.uifix' in main or 'uiToggle' in main:
        out.append('the uifix trigger file or uiToggle is still there')
    sw = body(main, 'uiSetSharp') or ''
    if 'g_uiFix = on' not in sw:
        out.append('uiSetSharp does not set the switch')
    # stretch: TAA marked in its branch and cleared at present; the stretch draw picks its shader in both paths
    dr, pr, pk = body(main, 'hk_Draw') or '', body(main, 'onPresent') or '', body(main, 'stretchPick') or ''
    taa = re.search(r'if \(ctx == g_imm && g_immPS && g_immPS == g_taaPS\) \{\n(.*?)\n    \}', dr, re.S)
    if not taa or 'g_taaThisFrame = true;' not in taa.group(1):
        out.append('the TAA branch of hk_Draw does not mark the frame')
    if 'g_taaThisFrame = false;' not in pr:
        out.append('onPresent does not clear the TAA mark')
    ps = body(main, 'hk_PSSetShader') or ''
    if not re.search(r'for \(int i = 0; i < 2; \+\+i\) if \(ps == g_hudGame\[i\] && g_hudOurs\[i\]\) ps = g_hudOurs\[i\];', ps):
        out.append('hk_PSSetShader swaps 664e when it is bound')
    blit = re.search(r'if \(ctx == g_imm && g_immPS && g_immPS == g_blitPS\) \{\n(.*?)\n    \}', dr, re.S)
    if not blit or not re.search(r'const bool ours = stretchPick\(ctx\);\n\s*orig<PFN_Draw>\(ctx, 13\)\(ctx, c, s\);\n'
                                 r'\s*if \(ours\) setPS\(ctx, \(ID3D11PixelShader\*\)g_blitPS\);', blit.group(1)):
        out.append('the stretch draw does not pick its shader and put the game\'s back')
    if not re.search(r'const bool magnify = d\.Width && d\.Height && \(vp\.Width > d\.Width \* 1\.05f \|\| vp\.Height > d\.Height \* 1\.05f\);\n'
                     r'\s*if \(!magnify\) return false;', pk):
        out.append('stretchPick does not keep the game\'s shader where nothing is magnified')
    if 'setPS(ctx, g_taaThisFrame && g_stretchLight ? g_stretchLight : g_hudOurs[2]);' not in pk:
        out.append('stretchPick does not choose light or full by TAA')
    light = re.search(r'static const char\* kStretchLightHlsl = R"\((.*?)\)";', main, re.S)
    if not light or 'catmullRom(uv)' not in light.group(1) or 'clamp(c + 0.5 * (c - b)' not in light.group(1):
        out.append('the light stretch is not the sharpened Catmull-Rom with its clamp')
    elif re.search(r'\bjxx\b|\bcoh\b|for \(', light.group(1)):
        out.append('the light stretch still runs the edge-directed smoothing')
    # sampler hook: no upscaler state or string formatting per call; the bias is worked out in onPresent
    smp = body(main, 'hk_PSSetSamplers') or ''
    if not smp or re.search(r'dlssInfo\(|dlssEnabled\(|snprintf', smp.split('ID3D11SamplerState* v[16]')[0]):
        out.append('hk_PSSetSamplers asks the upscaler for its state before it knows a bias is needed')
    if not re.search(r'const float bias = g_frameBias\.load\(', smp):
        out.append('hk_PSSetSamplers does not read the per-frame bias')
    if not re.search(r'g_frameBias\.store\(', pr) or 'dlssInfo()' not in pr:
        out.append('onPresent does not work out the frame\'s bias')
    return out

found = problems(sources)
if found:
    for p in found:
        print('FAIL: ' + p)
    sys.exit(1)

def edit(which, old, new):
    def f(s):
        t = dict(s); t[which] = s[which].replace(old, new, 1); return t
    return f

mutants = [
    ('the HUD section is renamed', edit('menu', 'SeparatorText("HUD")', 'SeparatorText("Display")')),
    ('the checkbox has no pad tick', edit('menu', 'sharpFlip |= pad::padTick(', 'sharpFlip |= pad::padTock(')),
    ('the caption is reworded', edit('menu', 'Costs a little frame rate at 4K.', 'Costs a little frame rate.')),
    ('the checkbox does not save', edit('menu', 'iniSet(L"ui", L"sharp", ', 'iniSet(L"ui", L"sharpx", ')),
    ('the default is off', edit('main', 'L"ui", L"sharp", 1, ', 'L"ui", L"sharp", 0, ')),
    ('the trigger file stays', edit('main', '// ---- swapchain / present', 'static const wchar_t* kTrig = L"ffxv_dlss.uifix";\n// ---- swapchain / present')),
    ('the TAA branch does not mark the frame', edit('main', 'g_taaThisFrame = true;', 'g_taaThisFrame = false;')),
    ('onPresent does not clear the mark', edit('main', '    g_taaThisFrame = false;\n', '\n')),
    ('the full stretch is used on TAA frames', edit('main', 'setPS(ctx, g_taaThisFrame && g_stretchLight ?', 'setPS(ctx, !g_taaThisFrame && g_stretchLight ?')),
    ('the replacement runs where nothing is magnified', edit('main', '    if (!magnify) return false;\n', '')),
    ('the stretch reads only the width', edit('main', ' || vp.Height > d.Height * 1.05f', '')),
    ('the game\'s shader is not put back', edit('main', '            if (ours) setPS(ctx, (ID3D11PixelShader*)g_blitPS);\n', '')),
    ('664e is swapped when bound', edit('main', 'for (int i = 0; i < 2; ++i) if (ps == g_hudGame[i]', 'for (int i = 0; i < 3; ++i) if (ps == g_hudGame[i]')),
    ('the light stretch keeps the edge smoothing', edit('main', '    return clamp(c + 0.5 * (c - b)', '    float jxx = 0;\n    return clamp(c + 0.5 * (c - b)')),
    ('the sampler hook asks the upscaler per call', edit('main', 'const float bias = g_frameBias.load(', 'const DlssInfo di = dlssInfo(); (void)di;\n    const float bias = g_frameBias.load(')),
    ('onPresent does not store the bias', edit('main', 'g_frameBias.store(', 'g_frameBias.exchange(')),
]
for name, mutate in mutants:
    broken = mutate(sources)
    if broken == sources:
        print('FAIL: mutant "%s" did not change the sources: the check has drifted from the code' % name)
        sys.exit(1)
    if not problems(broken):
        print('FAIL: the checks pass on a copy where %s' % name)
        sys.exit(1)
print('perf hud: checks ok, and each fails on its broken copy (%d)' % len(mutants))
EOF
echo "PASS: perf hud"
