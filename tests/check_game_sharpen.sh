#!/bin/bash
# Checks the "Game sharpening" setting: builds tests/game_sharpen_test.cpp (src/game_sharpen.h: recognising the pass,
# the setting's range and ini text) with the host compiler and runs it, then checks the structure of main.cpp, menu.cpp
# and menu_pad.h: the pass is recognised by its CRC when the game creates it, our objects are made there and nowhere in
# dlss.cpp, at 100% the game's draw runs with nothing of ours bound, compute state is restored after our dispatch and
# the game's buffer is bound again after the draw, the menu rows save the setting and can be typed into, and Esc while
# typing leaves the box rather than the menu. Each source check must fail on a broken copy.
# Usage: tests/check_game_sharpen.sh [source folder]   (default: src next to tests)
here=$(cd "$(dirname "$0")" && pwd)
src=$(cd "${1:-$here/../src}" && pwd) || exit 1
T=$(mktemp -d) || exit 1
trap 'rm -rf "$T"' EXIT

g++ -std=c++17 -O1 -Wall -Wextra -Werror -I "$src" -o "$T/game_sharpen_test" "$here/game_sharpen_test.cpp" \
    || { echo "FAIL: game_sharpen_test did not build"; exit 1; }
"$T/game_sharpen_test" || { echo "FAIL: game_sharpen_test"; exit 1; }

python3 -I - "$src/main.cpp" "$src/menu.cpp" "$src/menu_pad.h" "$src/dlss.cpp" <<'PY' || exit 1
import re, sys
read = lambda p: open(p, encoding='utf-8', errors='replace').read().replace('\r\n', '\n')
main, menu, pad, dlss = (read(p) for p in sys.argv[1:5])

def body(src, name):
    m = re.search(r'^\S[^\n;]*?\b' + name + r'\([^\n]*\{\n(.*?)^\}', src, re.M | re.S)
    return m.group(1) if m else None
code = lambda b: re.sub(r'//[^\n]*', '', b)

def problems(main, menu, pad, dlss):
    out = []
    names = ('gameSharpenMake', 'gameSharpenDraw', 'hk_CreatePS', 'hk_Draw', 'hookDevice')
    mk, dr, cps, hd, dev = (body(main, n) for n in names)
    poll, comp, ts = body(menu, 'pollToggle'), body(menu, 'compose'), body(pad, 'typedSlider')
    if None in (mk, dr, cps, hd, dev, poll, comp, ts): return ['a function under check was not found']
    mk, dr, cps, hd, dev, poll, comp, ts = map(code, (mk, dr, cps, hd, dev, poll, comp, ts))
    # recognised by its CRC when the game creates it; our objects made there, never in dlss.cpp
    if not re.search(r'crc32\(code, len\) != kGameSharpenCrc\) return;', mk): out.append('the pass is not recognised by its CRC')
    if 'gameSharpenMake(d, *o, c, l);' not in cps: out.append('hk_CreatePS does not make the objects for the pass')
    if 'gameSharpen' in code(dlss) or 'kGsHlsl' in dlss: out.append('dlss.cpp holds game sharpening code (it must work with the upscaler off)')
    # the draw: at 100% without a log sample, the game's draw with nothing of ours bound
    first = dr.find('orig<PFN_Draw>(ctx, 13)(ctx, c, s);')
    early = re.search(r'if \(g_gsBroken \|\| !g_gsCS \|\| n <= kGsStrength \|\| \(pct == 100 && !sample\) \|\| !gameSharpenBuffers\(n \* 16\)\) \{\n\s*orig<PFN_Draw>\(ctx, 13\)\(ctx, c, s\);', dr)
    if not early or first < 0 or dr.find('Set', 0, first) >= 0: out.append('at 100% the game draws with something of ours bound')
    if not re.search(r'if \(pct == 100\) \{[^}]*orig<PFN_Draw>\(ctx, 13\)\(ctx, c, s\);\s*game->Release\(\);\s*return;', dr):
        out.append('a log sample at 100% binds our constants for the draw')
    # compute state restored after our dispatch, before anything else
    d = dr.find('ctx->Dispatch(')
    rest = dr[d:]
    for need in ('ctx->CSSetShader(cs, nullptr, 0);', 'ctx1->CSSetConstantBuffers1(0, 2, cb, cbFirst, cbNum);',
                 'ctx->CSSetConstantBuffers(0, 2, cb);', 'ctx->CSSetUnorderedAccessViews(0, 2, uav, nullptr);'):
        if d < 0 or need not in rest: out.append('compute state not restored after the dispatch: ' + need)
    # our copy bound for the pass, then the game's buffer bound again as it was
    m = re.search(r'ctx->PSSetConstantBuffers\(0, 1, &g_gsCB\);\s*orig<PFN_Draw>\(ctx, 13\)\(ctx, c, s\);\s*'
                  r'if \(ranged\) ctx1->PSSetConstantBuffers1\(0, 1, &game, &first, &num\);\s*else ctx->PSSetConstantBuffers\(0, 1, &game\);', dr)
    if not m: out.append("the game's constant buffer is not bound again after the draw")
    # a pass drawn only through a call the setting does not change is named in the log
    at = body(main, 'gameSharpenAfterTaa')
    if at is None or not re.search(r'const int other = gameSharpenOtherCall\(g_gsDraws\);\s*if \(other >= 0\) \{\s*g_gs\.notDrawnLogged = true;\s*logf\("game sharpening: pass drawn with %s, which the setting does not change', code(at)):
        out.append('a pass drawn only through another draw call is not named in the log')
    if not re.search(r'if \(ctx == g_imm && g_immPS && g_immPS == g_gameSharpPS\) \{\s*gameSharpenDraw\(ctx, c, s\);\s*return;', hd):
        out.append('hk_Draw does not hand the pass to gameSharpenDraw')
    # the setting: read at start, set and saved by the menu row
    if 'g_gameSharpPct = gameSharpenIni(gs);' not in dev or 'L"game_sharpening"' not in dev: out.append('the setting is not read from ffxv_dlss.ini at start')
    mc = code(menu)
    row = re.search(r'label\("Game sharpening"\);.*?pad::typedSlider\("##gamesharp", gameSharp, 0, 200, 5, gameSharp \? "%d%%" : "Off", boxW, &typing\)\) \{\s*'
                    r'gameSharpenSet\(gameSharp\);\s*iniSet\(L"dlss", L"game_sharpening", gameSharp\);', mc, re.S)
    if not row: out.append('the Game sharpening row does not set and save the setting from a typed slider')
    if not re.search(r'label\("Sharpening"\);\s*const bool sharpChanged = pad::typedSlider\("##sharp", sharp, 0, 100, 1,', mc):
        out.append('the Sharpening row has no typed box')
    if mc.find('label("Game sharpening")') < mc.find('label("Sharpening")'): out.append('Game sharpening is not below Sharpening')
    # the box: digits only, out of controller navigation; Esc while typing leaves the box, not the menu; keys only while in front
    if 'ImGuiInputTextFlags_CharsDecimal' not in ts or 'PushItemFlag(ImGuiItemFlags_NoNav, true)' not in ts:
        out.append('the typed box takes more than numbers, or the controller can move onto it')
    if '(esc && !escDown && g_open && !g_typing)' not in poll: out.append('Esc while typing closes the menu')
    if 'keys.feed(io, [front](int vk) { return front && (GetAsyncKeyState(vk) & 0x8000) != 0; });' not in comp:
        out.append('keys reach the boxes when the game is not in front')
    return out

p = problems(main, menu, pad, dlss)
if p:
    for x in p: print('FAIL:', x)
    sys.exit(1)
dr = body(main, 'gameSharpenDraw')
broken = {
    'no CRC test': (main.replace('crc32(code, len) != kGameSharpenCrc) return;', 'false) return;', 1), menu, pad, dlss),
    'not made at load': (main.replace('gameSharpenMake(d, *o, c, l);', '', 1), menu, pad, dlss),
    'made in dlss.cpp': (main, menu, pad, dlss + '\nvoid init2() { gameSharpenMake(nullptr, nullptr, nullptr, 0); }\n'),
    'scaled at 100%': (main.replace('(pct == 100 && !sample) || ', '', 1), menu, pad, dlss),
    'sample at 100% binds ours': (main.replace(dr, dr.replace('    if (pct == 100) {', '    if (false) {', 1), 1), menu, pad, dlss),
    'shader not restored': (main.replace('    ctx->CSSetShader(cs, nullptr, 0);\n', '', 1), menu, pad, dlss),
    'UAVs not restored': (main.replace('ctx->CSSetUnorderedAccessViews(0, 2, uav, nullptr);', '', 1), menu, pad, dlss),
    'game buffer not rebound': (main.replace(dr, dr.replace('    if (ranged) ctx1->PSSetConstantBuffers1(0, 1, &game, &first, &num);  // the game', '    // the game', 1), 1), menu, pad, dlss),
    'other draw call not named': (main.replace('logf("game sharpening: pass drawn with %s', 'logf("game sharpening: pass used %s', 1), menu, pad, dlss),
    'other draw call never checked': (main.replace('const int other = gameSharpenOtherCall(g_gsDraws);', 'const int other = -1;', 1), menu, pad, dlss),
    'CS ranged buffers not restored': (main.replace('ctx1->CSSetConstantBuffers1(0, 2, cb, cbFirst, cbNum);', 'ctx1->CSSetConstantBuffers1(0, 1, cb, cbFirst, cbNum);', 1), menu, pad, dlss),
    'CS buffers not restored': (main.replace('    else ctx->CSSetConstantBuffers(0, 2, cb);\n', '', 1), menu, pad, dlss),
    'Game sharpening above Sharpening': (main, menu.replace('label("Sharpening");', 'label("Game sharpening"); label("Sharpening");', 1), pad, dlss),
    'pass not handed over': (main.replace('        gameSharpenDraw(ctx, c, s);\n', '', 1), menu, pad, dlss),
    'ini not read': (main.replace('g_gameSharpPct = gameSharpenIni(gs);', 'g_gameSharpPct = 100;', 1), menu, pad, dlss),
    'not set': (main, menu.replace('        gameSharpenSet(gameSharp);\n', '', 1), pad, dlss),
    'not saved': (main, menu.replace('        iniSet(L"dlss", L"game_sharpening", gameSharp);\n', '', 1), pad, dlss),
    'no box on Game sharpening': (main, menu.replace('pad::typedSlider("##gamesharp"', 'pad::padSlider("##gamesharp"', 1), pad, dlss),
    'no box on Sharpening': (main, menu.replace('pad::typedSlider("##sharp"', 'pad::padSlider("##sharp"', 1), pad, dlss),
    'box takes any text': (main, menu, pad.replace('ImGuiInputTextFlags_CharsDecimal | ', '', 1), dlss),
    'box in navigation': (main, menu, pad.replace('ImGui::PushItemFlag(ImGuiItemFlags_NoNav, true);', 'ImGui::PushItemFlag(ImGuiItemFlags_NoNav, false);', 1), dlss),
    'Esc while typing closes': (main, menu.replace(' && !g_typing)', ')', 1), pad, dlss),
    'keys when not in front': (main, menu.replace('return front && (GetAsyncKeyState', 'return (GetAsyncKeyState', 1), pad, dlss),
}
for name, files in broken.items():
    if files == (main, menu, pad, dlss): print('FAIL: could not build the broken copy:', name); sys.exit(1)
    if not problems(*files): print('FAIL: check passed on the broken copy:', name); sys.exit(1)
print('game sharpening checks passed (and fail on %d broken copies)' % len(broken))
PY
echo "PASS: game sharpening"
