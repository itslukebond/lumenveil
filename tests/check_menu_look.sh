#!/bin/bash
# Checks the desktop Insert menu's look: builds tests/menu_look_test.cpp (src/menu_look.h and src/menu_pad.h with Dear
# ImGui's core) with the host compiler and runs it, then checks menu.cpp's structure: every slider row of the desktop
# menu has a typed box (no bare padSlider left) and g_typing is set after the last box, every tick box has its animated
# look, every option is drawn as ‹ value ›, the counter's frame times are recorded by menuCount and its width ignores the
# live numbers, keyboard navigation is off in the Steam Deck layout, applyStyle (which the Deck layout uses) is as it
# was, and the selected row is never eased. Each source check must fail on a broken copy.
# Usage: tests/check_menu_look.sh [source folder holding menu_look.h, menu_pad.h and menu.cpp] [Dear ImGui folder]
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
g++ -std=c++17 -O1 -Wall -Wextra -Werror -I "$src" -I "$imgui" -o "$T/menu_look_test" "$here/menu_look_test.cpp" "$T"/imgui*.o \
    || { echo "FAIL: menu_look_test did not build"; exit 1; }
"$T/menu_look_test" || { echo "FAIL: menu_look_test"; exit 1; }

grep -qE '^project\(ffxv_dlss VERSION [0-9]+\.[0-9]+\.[0-9]+ ' "$src/../CMakeLists.txt" && grep -q 'LUMENVEIL_VERSION="\${PROJECT_VERSION}"' "$src/../CMakeLists.txt" \
    || { echo "FAIL: CMakeLists.txt does not give the dll its release number"; exit 1; }

python3 -I - "$src/menu.cpp" "$src/menu_look.h" "$src/menu_pad.h" <<'PY' || exit 1
import hashlib, re, sys
rd = lambda p: open(p, encoding='utf-8', errors='replace').read().replace('\r\n', '\n')
source, look, padh = rd(sys.argv[1]), rd(sys.argv[2]), rd(sys.argv[3])

def body(src, name):
    m = re.search(r'^\S[^\n;]*?\b' + name + r'\([^\n]*\{\n(.*?)^\}', src, re.M | re.S)
    return m.group(1) if m else None
def inline(src, name):  # a function in a header, indented by nothing, opening on its own line
    m = re.search(r'^inline [^\n;]*?\b' + name + r'\([^\n]*\{\n(.*?)^\}', src, re.M | re.S)
    return m.group(1) if m else None
code = lambda b: re.sub(r'//[^\n]*', '', b)
APPLY_STYLE = '5d58e3be4c10399ef4724fceb3307d0b8f5d98787d4b916f1e9e86d3dedf47b8'  # applyStyle's body before the restyle

def problems(src, lk, ph):
    out = []
    menu, comp, count, ctr, style = (body(src, n) for n in ('drawMenu', 'compose', 'menuCount', 'drawCounter', 'applyStyle'))
    row = inline(lk, 'row')
    if None in (menu, comp, count, ctr, style, row): return ['drawMenu, compose, menuCount, drawCounter, applyStyle or look::row not found']
    m = code(menu)
    # the panel: one fixed size that never scrolls, so no scrollbar for the panel to cover
    if not re.search(r'ImGuiWindowFlags_NoResize \| ImGuiWindowFlags_NoScrollbar \|', m) or not re.search(r'look::panel\(dl, wa, wb, look::kMenu\);', m):
        out.append('the menu panel can scroll')
    # the release number in the footer, from CMakeLists.txt's project VERSION
    if not re.search(r'const char\* ver = "Lumenveil " LUMENVEIL_VERSION;\n[^\n]*\n\s*dl->AddText\([^\n]*\bver\);', m): out.append('the menu does not show the release number')
    # the trademark lines, smaller than the notes
    if not re.search(r'look::fineText\("Uses NVIDIA DLSS[^;]*, 11\);', m): out.append('the trademark lines are not in the fine text')
    # sliders: every one with its box, and g_typing set once, after the last box
    if 'padSlider(' in m: out.append('a slider in the desktop menu has no typed box')
    if not re.search(r'const SliderSpec sp = sliderSpec\(r\.id, st\);\n[^\n]*\n\s*if \(pad::typedSlider\("##v", val, sp\.lo, sp\.hi, sp\.step, sp\.fmt, boxW, &typing\)\)', m):
        out.append('the slider rows are not typed sliders')
    last, t = m.rfind('pad::typedSlider('), m.find('g_typing = typing;')
    if t < 0 or t < last or m.count('g_typing = ') != 1: out.append('g_typing is not set after the last box')
    # tick boxes: each Checkbox has its look right after its pad tick, before the next row
    boxes = [x.start() for x in re.finditer(r'ImGui::Checkbox\(', m)]
    for i, b in enumerate(boxes):
        nxt = boxes[i + 1] if i + 1 < len(boxes) else len(m)
        seg = m[b:nxt]
        if not re.search(r'pad::padTick\([^\n]*\n[^\n]*look::toggleLook\(', seg): out.append('a tick box has no animated look after its pad tick')
    if not boxes: out.append('no tick boxes found')
    # options: no stock combo left; the scaling mode steps through the ‹ › frame
    if 'ImGui::Combo(' in m: out.append('a stock combo is left in the desktop menu')
    if not re.search(r'if \(r\.id == pages::kScaling\) \{[^\n]*\n[^\n]*\n\s*const int d = look::optionFrame\("##v", ', m): out.append('the scaling mode is not drawn as an option')
    # the counter: frame times recorded per rendered frame, the width from fixed slots only
    if not re.search(r'g_frames\.record\(', code(count)): out.append('menuCount does not record frame times')
    cw = re.search(r'look::counterWidth\(([^;]*)\);', code(ctr))
    if not cw or re.search(r'g_fps(Shown|Rendered)', cw.group(1)): out.append('the counter width depends on the live numbers')
    # keyboard navigation: on for the desktop menu, off in the Steam Deck layout, set before the frame starts
    k = code(comp)
    kb = re.search(r'if \(g_open && g_deckOpen\) io\.ConfigFlags &= ~ImGuiConfigFlags_NavEnableKeyboard;\n\s*else io\.ConfigFlags \|= ImGuiConfigFlags_NavEnableKeyboard;', k)
    if not kb or kb.start() > k.find('ImGui::NewFrame();'): out.append('keyboard navigation is not off in the Steam Deck layout')
    if not re.search(r'\{ 0x20, ImGuiKey_Space, 0 \}', ph): out.append('Space does not reach the menu')
    # the Deck layout's style: applyStyle unchanged
    if hashlib.sha256(style.encode()).hexdigest() != APPLY_STYLE: out.append('applyStyle changed')
    # the selected row is drawn where it is, never eased
    if re.search(r'easeScale|exp\(|GetTime\(|DeltaTime|ImLerp|toggleAge', code(row)): out.append('the selected row is eased')
    return out

p = problems(source, look, padh)
if p:
    for x in p: print('FAIL:', x)
    sys.exit(1)
menu = body(source, 'drawMenu'); ctr = body(source, 'drawCounter'); style = body(source, 'applyStyle'); row = inline(look, 'row')
cnt = body(source, 'menuCount')
broken = {
    'panel scrolls': (source.replace('ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoScrollbar |', 'ImGuiWindowFlags_NoResize |', 1), look, padh),
    'no release number': (source.replace('"Lumenveil " LUMENVEIL_VERSION;', '"Lumenveil";', 1), look, padh),
    'trademarks at note size': (source.replace('look::fineText("Uses NVIDIA DLSS', 'ImGui::TextWrapped("Uses NVIDIA DLSS', 1), look, padh),
    'bare slider': (source.replace('pad::typedSlider("##v", val, sp.lo, sp.hi, sp.step, sp.fmt, boxW, &typing)', 'pad::padSlider("##v", val, sp.lo, sp.hi, sp.step, sp.fmt)', 1), look, padh),
    'typing set early': (source.replace(menu, menu.replace('    g_typing = typing;\n', '', 1).replace('    bool typing = false;\n', '    bool typing = false;\n    g_typing = typing;\n', 1), 1), look, padh),
    'tick without look': (source.replace(menu, re.sub(r'\n[^\n]*look::toggleLook\([^\n]*', '', menu, count=1), 1), look, padh),
    'stock combo': (source.replace('look::option("##v", &c, names, count, v.main, v.rest)', 'ImGui::Combo("##v", &c, names, count)', 1), look, padh),
    'scale not an option': (source.replace('look::optionFrame("##v", ', 'look::optionFrameX("##v", ', 1), look, padh),
    'no record': (source.replace(cnt, cnt.replace('g_frames.record(', 'g_frames.size(', 1), 1), look, padh),
    'width from numbers': (source.replace(ctr, re.sub(r'look::counterWidth\(([^;]*)\);', r'look::counterWidth(\1 + g_fpsShown);', ctr, count=1), 1), look, padh),
    'keyboard nav in the Deck': (source.replace('io.ConfigFlags &= ~ImGuiConfigFlags_NavEnableKeyboard;', 'io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;', 1), look, padh),
    'no Space': (source, look, padh.replace('{ 0x20, ImGuiKey_Space, 0 }', '{ 0x20, ImGuiKey_None, 0 }', 1)),
    'applyStyle changed': (source.replace(style, style.replace('s.WindowRounding = 10;', 's.WindowRounding = 0;', 1), 1), look, padh),
    'row eased': (source, look.replace(row, row.replace('\n', '\n    (void)ImLerp(0.f, 1.f, 0.5f);\n', 1), 1), padh),
}
for name, (s, l, h) in broken.items():
    if (s, l, h) == (source, look, padh): print('FAIL: could not build the broken copy:', name); sys.exit(1)
    if not problems(s, l, h): print('FAIL: check passed on the broken copy:', name); sys.exit(1)
print('menu.cpp look checks passed (and fail on %d broken copies)' % len(broken))
PY
echo "PASS: menu_look"
