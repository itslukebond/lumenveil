#!/bin/bash
# Checks the Insert menu's Steam Deck layout: builds tests/menu_deck_test.cpp (src/menu_deck.h with Dear ImGui's core)
# with the host compiler and runs it, then checks menu.cpp's structure: still three input hooks; the desktop scale line
# unchanged; the Deck is detected from the adapter and SteamDeck and logged on its own line; the Layout setting is read
# from [overlay] layout; the Deck fonts load only after the desktop's and only on a Deck or with Layout not Automatic;
# the layout is picked when the menu opens and changed only by the layout button (applied in compose's open block
# before the controller is fed); compose draws the desktop menu when the Deck layout is not open and gives ImGui's
# controller navigation back on that path; the layout button is in both layouts on every machine and saves to
# [overlay] layout through one handler, and the Portable mode row is gone; both layouts take their rows from the page
# model and save through the same helpers, setters and ini keys. Each source check must fail on a broken copy.
# Usage: tests/check_menu_deck.sh [source folder holding menu_deck.h and menu.cpp] [Dear ImGui folder]
#        (defaults: src next to tests; third_party/imgui next to tests, or $IMGUI)
here=$(cd "$(dirname "$0")" && pwd)
src=$(cd "${1:-$here/../src}" && pwd) || exit 1
imgui=${2:-${IMGUI:-$here/../third_party/imgui}}
[ -f "$imgui/imgui.cpp" ] || { echo "FAIL: Dear ImGui not found at $imgui (pass its folder or set IMGUI)"; exit 1; }
imgui=$(cd "$imgui" && pwd)
T=$(mktemp -d) || exit 1
trap 'rm -rf "$T"' EXIT

for f in imgui imgui_draw imgui_widgets imgui_tables; do
    g++ -std=c++17 -O1 -w -I "$imgui" -c "$imgui/$f.cpp" -o "$T/$f.o" & done
wait
g++ -std=c++17 -O2 -Wall -Wextra -Werror -I "$src" -I "$imgui" -o "$T/menu_deck_test" "$here/menu_deck_test.cpp" "$T"/imgui*.o \
    || { echo "FAIL: menu_deck_test did not build"; exit 1; }
"$T/menu_deck_test" || { echo "FAIL: menu_deck_test"; exit 1; }

python3 -I - "$src/menu.cpp" <<'PY' || exit 1
import re, sys
source = open(sys.argv[1], encoding='utf-8', errors='replace').read().replace('\r\n', '\n')

def body(src, name):
    m = re.search(r'^\S[^\n;]*?\b' + name + r'\([^\n]*\{\n(.*?)^\}', src, re.M | re.S)
    return m.group(1) if m else None

code = lambda b: re.sub(r'//[^\n]*', '', b)

def problems(src):
    out = []
    init, fonts, poll, comp, menu, deckm = (body(src, n) for n in ('menuInit', 'loadFonts', 'pollToggle', 'compose', 'drawMenu', 'drawDeckMenu'))
    if None in (init, fonts, poll, comp, menu, deckm): return ['menuInit, loadFonts, pollToggle, compose, drawMenu or drawDeckMenu not found']
    c = code(src)
    # no new hook in the game's input path
    if c.count('MH_CreateHook(') != 3: out.append('menu.cpp does not create exactly three hooks')
    # the desktop scale is as before
    if '    g_scale = std::max(1.f, height / 1080.f);\n' not in init: out.append('the desktop scale line changed')
    # detection: the adapter's IDs and SteamDeck, logged on their own line, and the setting from [overlay] layout
    i = code(init)
    if not re.search(r'g_isDeck = deck::isSteamDeck\(ad1\.VendorId, ad1\.DeviceId, ', i): out.append('the Deck is not detected from the adapter IDs')
    if 'GetEnvironmentVariableA("SteamDeck"' not in i: out.append('SteamDeck is not read from the environment')
    if not re.search(r'g_layout = deck::layoutValue\(iniGet\(L"overlay", L"layout", 0\)\);', i): out.append('the Layout setting is not read from [overlay] layout')
    if not re.search(r'logf\("menu: adapter %04x:%04x, Steam Deck %s \(%s\), layout %s"', i): out.append('no adapter line in the log')
    if i.find('g_layout = ') > i.find('loadFonts();') or i.find('g_isDeck = ') > i.find('loadFonts();'):
        out.append('the detection runs after the fonts are loaded')
    # the Deck fonts: after the desktop's, on every machine (a PC can pick the Deck layout in the menu)
    f = code(fonts)
    blk = re.search(r'\n    \{ *\n[^\n]*OversampleH', f)
    gate = blk.start() if blk else -1
    if gate < 0 or 'g_layout' in f or 'g_isDeck' in f: out.append('the Deck fonts are not loaded on every machine')
    elif any(f.rfind(x) > gate for x in ('g_font = load(', 'g_big = load(', 'g_bigThin = load(', 'AddFontDefault(')):
        out.append('a desktop font loads after the Deck fonts')
    elif f.count('g_deck26') < 1 or f.find('g_deck26') < gate: out.append('the Deck fonts load outside their condition')
    # the layout is picked when the menu opens, never while it is open
    p = code(poll)
    if not re.search(r'g_open = true;\n\s*g_deckOpen = g_deck26 && deck::deckLayoutOn\(g_layout, g_isDeck\);', p): out.append('the layout is not picked when the menu opens')
    if c.count('g_deckOpen = ') != 2: out.append('the layout is changed somewhere other than the open and the layout button')
    k0 = code(comp)
    blk = k0.find('if (g_open) {')
    sw = k0.find('g_deckOpen = g_deck26 && g_swap.want == 1;')
    if blk < 0 or sw < blk or sw > k0.find('pad::feedPad('): out.append('the layout change is not applied in compose before the controller is fed')
    # compose: controller navigation off only in the Deck layout and back on otherwise; the desktop menu otherwise
    k = code(comp)
    if not re.search(r'if \(g_open && g_deckOpen\) io\.ConfigFlags &= ~ImGuiConfigFlags_NavEnableGamepad;\n\s*else io\.ConfigFlags \|= ImGuiConfigFlags_NavEnableGamepad;', k):
        out.append('compose does not give the controller navigation back outside the Deck layout')
    if k.find('NavEnableGamepad') > k.find('ImGui::NewFrame();'): out.append('the navigation flag is set after NewFrame')
    if not re.search(r'if \(g_open\) \{ if \(g_deckOpen\) drawDeckMenu\(\); else drawMenu\(\); \}', k): out.append('compose does not draw the desktop menu outside the Deck layout')
    # the layout button: in both layouts on every machine, the only thing that changes the layout while the menu is
    # open, saved to [overlay] layout through one handler; the Portable mode row is gone
    m = code(menu)
    d = code(deckm)
    sl = body(src, 'switchLayout')
    if sl is None or not re.search(r'g_layout = portable \? 1 : 2;\n\s*iniSet\(L"overlay", L"layout", g_layout\);\n\s*g_swap\.flip\(portable, ImGui::GetTime\(\), g_deckOpen\);', code(sl)):
        out.append('the layout button does not save to [overlay] layout')
    if not re.search(r'const int want = look::layoutButton\("##layout", false, g_deck26 != nullptr, [^\n]*\n(?:[^\n]*\n){1,2}\s*if \(want >= 0\) switchLayout\(want == 1\);', m) \
            or 'g_isDeck || g_layout' in m:
        out.append('the desktop layout button is not shown on every machine')
    if not re.search(r'const int want = deck::layoutButton\(L, true, [^\n]*\n\s*if \(want >= 0\) switchLayout\(want == 1\);', d):
        out.append('the Portable layout has no layout button')
    if c.count('switchLayout(') != 3 or c.count('iniSet(L"overlay", L"layout"') != 1 or c.count('g_swap.flip(') != 1:
        out.append('the layout changes somewhere other than the layout button')
    if '"Portable mode"' in c or 'portableRow(' in c or 'look::portable(' in c: out.append('the Portable mode row is still there')
    # both layouts take their rows from the page model and save through the same setters and ini keys
    for f, name in ((m, 'desktop'), (d, 'Portable')):
        if 'const pages::State st = fillState();' not in f or not re.search(r'pages::rows\((g_deskPage|nav\.page), st, rows, &sub\)', f):
            out.append('the %s menu does not take its rows from the page model' % name)
        for call in ('flipToggle(r.id)', 'setSlider(r.id, val)', 'stepList(r.id, st, '):
            if call not in f: out.append('the %s menu does not save through %s' % (name, call))
    shared = ''.join(code(body(src, n) or '') for n in ('pick', 'flipToggle', 'setSlider', 'saveFg', 'setScale', 'fillState'))
    for call in ('dlssSetUpscaler(', 'iniSet(L"dlss", L"upscaler"', 'dlssToggle()', 'dlssSetSharpness(', 'iniSet(L"dlss", L"sharpness"',
                 'L"RenderingResolutionRatio"', 'iniSet(L"fg", L"backend"', 'fgApply(fs)', 'iniSet(L"fg", L"enable"', 'iniSet(L"fg", L"limit"',
                 'iniSet(L"overlay", L"enable"', 'iniSet(L"overlay", L"corner"', 'scaleState()', 'fgPick()', 'iniSet(L"dlss", L"preset"',
                 'iniSet(L"fg", L"reflex"', 'iniSet(L"fg", L"antilag"', 'iniSet(L"ui", L"sharp"', 'iniSet(L"dlss", L"game_sharpening"'):
        if call not in shared: out.append('the menus do not save through ' + call)
    # the number box: B and Esc leave it before they close the menu
    if not re.search(r'g_padBusy = g_typing = ', d): out.append('the Deck menu does not hold B and Esc while typing')
    return out

p = problems(source)
if p:
    for x in p: print('FAIL:', x)
    sys.exit(1)
init = body(source, 'menuInit'); fonts = body(source, 'loadFonts'); menu = body(source, 'drawMenu')
broken = {
    'a fourth hook': source.replace('MH_EnableHook(MH_ALL_HOOKS);', 'MH_CreateHook(nullptr, nullptr, nullptr);\n    MH_EnableHook(MH_ALL_HOOKS);', 1),
    'scale changed': source.replace('g_scale = std::max(1.f, height / 1080.f);', 'g_scale = std::max(1.f, height / 800.f);', 1),
    'no env': source.replace('GetEnvironmentVariableA("SteamDeck"', 'GetEnvironmentVariableA("SteamDeckX"', 1),
    'no adapter ids': source.replace('deck::isSteamDeck(ad1.VendorId, ad1.DeviceId, ', 'deck::isSteamDeck(0, 0, ', 1),
    'layout not read': source.replace('iniGet(L"overlay", L"layout", 0)', 'iniGet(L"overlay", L"layoutx", 0)', 1),
    'no log line': source.replace('logf("menu: adapter %04x:%04x', 'printf("menu: adapter %04x:%04x', 1),
    'fonts on a Deck only': source.replace(fonts, fonts.replace('    {  // the Steam Deck layout', '    if (g_isDeck || g_layout != 0) {  // the Steam Deck layout', 1), 1),
    'desktop font after': source.replace(fonts, fonts.replace('        if (!g_deck19) g_deck19 = g_deck26;\n    }\n', '        if (!g_deck19) g_deck19 = g_deck26;\n    }\n    if (!g_font) g_font = load(sys, 1);\n', 1), 1),
    'layout picked each frame': source.replace('    if (g_counter) drawCounter();', '    g_deckOpen = g_isDeck;\n    if (g_counter) drawCounter();', 1),
    'nav not given back': source.replace('else io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;', '', 1),
    'nav off always': source.replace('if (g_open && g_deckOpen) io.ConfigFlags &= ~ImGuiConfigFlags_NavEnableGamepad;', 'io.ConfigFlags &= ~ImGuiConfigFlags_NavEnableGamepad;', 1),
    'deck menu always': source.replace('if (g_open) { if (g_deckOpen) drawDeckMenu(); else drawMenu(); }', 'if (g_open) drawDeckMenu();', 1),
    'desktop button on a Deck only': source.replace('look::layoutButton("##layout", false, g_deck26 != nullptr', 'look::layoutButton("##layout", false, g_isDeck && g_deck26 != nullptr', 1),
    'swap applied after feedPad': source.replace(body(source, 'compose'), body(source, 'compose').replace('        pad::feedPad(io, g_padView, g_padBusy);\n', '', 1).replace('        if (g_swap.due(', '        pad::feedPad(io, g_padView, g_padBusy);\n        if (g_swap.due(', 1), 1),
    'layout not saved': source.replace(body(source, 'switchLayout'), body(source, 'switchLayout').replace('iniSet(L"overlay", L"layout", g_layout)', 'iniSet(L"overlay", L"layouts", g_layout)', 1), 1),
    'no button in Portable': source.replace(body(source, 'drawDeckMenu'), body(source, 'drawDeckMenu').replace('    if (want >= 0) switchLayout(want == 1);\n', '', 1), 1),
    'deck own rows': source.replace(body(source, 'drawDeckMenu'), body(source, 'drawDeckMenu').replace('pages::rows(', 'deckRows('), 1),
    'deck own toggle': source.replace(body(source, 'drawDeckMenu'), body(source, 'drawDeckMenu').replace('flipToggle(r.id)', 'dlssToggle()'), 1),
    'helper drops a key': source.replace(body(source, 'flipToggle'), body(source, 'flipToggle').replace('iniSet(L"ui", L"sharp"', 'iniSet(L"ui", L"sharper"', 1), 1),
    'Portable mode row back': source.replace(menu, menu + '    ImGui::TextUnformatted("Portable mode");\n', 1),
    'B closes while typing': source.replace('g_padBusy = g_typing = ', 'g_typing = ', 1),
}
for name, s in broken.items():
    if s == source: print('FAIL: could not build the broken copy:', name); sys.exit(1)
    if not problems(s): print('FAIL: check passed on the broken copy:', name); sys.exit(1)
print('menu.cpp Deck layout checks passed (and fail on %d broken copies)' % len(broken))
PY
echo "PASS: menu_deck"
