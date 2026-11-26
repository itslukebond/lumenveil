#!/bin/bash
# Checks the Insert menu and FPS counter when frame generation is on but adds no frames: builds
# tests/menu_noframes_test.cpp (src/menu_pages.h, with Dear ImGui's core for the fit test) with the host compiler and runs
# it with the bundled TeX Gyre Adventor; then checks that menu.cpp passes the verdict to the page model and draws the
# counter's frame generation line from it. Each source check must fail on a broken copy.
# Usage: tests/check_menu_noframes.sh [source folder] [Dear ImGui folder]
#        (defaults: src next to tests; third_party/imgui next to tests, or $IMGUI)
here=$(cd "$(dirname "$0")" && pwd)
src=$(cd "${1:-$here/../src}" && pwd) || exit 1
imgui=${2:-${IMGUI:-$here/../third_party/imgui}}
font=$here/../third_party/tex-gyre-adventor/texgyreadventor-regular.otf
[ -f "$imgui/imgui.cpp" ] || { echo "FAIL: Dear ImGui not found at $imgui (pass its folder or set IMGUI)"; exit 1; }
[ -f "$font" ] || { echo "FAIL: $font not found"; exit 1; }
imgui=$(cd "$imgui" && pwd)
T=$(mktemp -d) || exit 1
trap 'rm -rf "$T"' EXIT

for f in imgui imgui_draw imgui_widgets imgui_tables; do
    g++ -std=c++17 -O1 -w -I "$imgui" -c "$imgui/$f.cpp" -o "$T/$f.o" & done
wait
g++ -std=c++17 -O1 -Wall -Wextra -Werror -I "$src" -I "$imgui" -o "$T/menu_noframes_test" "$here/menu_noframes_test.cpp" "$T"/imgui*.o \
    || { echo "FAIL: menu_noframes_test did not build"; exit 1; }
"$T/menu_noframes_test" "$font" || { echo "FAIL: menu_noframes_test"; exit 1; }

python3 -I - "$src/menu.cpp" <<'PY' || exit 1
import re, sys
menu = open(sys.argv[1], encoding='utf-8', errors='replace').read().replace('\r\n', '\n')

def body(src, name):
    m = re.search(r'^\S[^\n;]*?\b' + name + r'\([^\n]*\{\n(.*?)^\}', src, re.M | re.S)
    return re.sub(r'//[^\n]*', '', m.group(1)) if m else None

def problems(menu):
    out = []
    counter = body(menu, 'drawCounter')
    if counter is None: return ['drawCounter not found']
    if not re.search(r's\.noFrames = st\.noFrames;', menu): out.append('the page model is not given the no-frames verdict')
    if 'const bool gen = pages::counterFg(fgLine, sizeof fgLine, fg.available, fg.generating, fg.noFrames, g_fpsShown, g_fpsRendered);' not in counter:
        out.append('the counter\'s frame generation line does not come from counterFg')
    if re.search(r'snprintf\(fgLine', counter): out.append('the counter still writes its own frame generation line')
    return out

p = problems(menu)
if p:
    for x in p: print('FAIL:', x)
    sys.exit(1)
broken = {
    'verdict not passed': menu.replace('s.noFrames = st.noFrames;', '', 1),
    'verdict dropped in counter': menu.replace('fg.generating, fg.noFrames, g_fpsShown', 'fg.generating, 0, g_fpsShown', 1),
    'own counter line': menu.replace('const bool gen = pages::counterFg(', 'snprintf(fgLine, sizeof fgLine, "FG"); const bool gen = pages::counterFg(', 1),
}
for name, b in broken.items():
    if b == menu: print('FAIL: could not build the broken copy:', name); sys.exit(1)
    if not problems(b): print('FAIL: check passed on the broken copy:', name); sys.exit(1)
print('menu no-frames checks passed (and fail on %d broken copies)' % len(broken))
PY
echo "PASS: menu no frames"
