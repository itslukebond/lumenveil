#!/bin/bash
# Checks where the Insert menu looks for the game's GraphicsConfig.ini (GitHub #3): builds tests/game_config_test.cpp
# (src/game_config.h) with the host compiler and runs it, then checks that gameConfig() in menu.cpp builds its folder
# from USERPROFILE with gameSettingsRoot() and never asks Windows for the Documents known folder, which can point
# elsewhere (OneDrive, the Location tab) while the game keeps using USERPROFILE\Documents.
# Usage: tests/check_game_config.sh [source folder holding game_config.h and menu.cpp]   (default: src next to tests)
here=$(cd "$(dirname "$0")" && pwd)
src=$(cd "${1:-$here/../src}" && pwd) || exit 1
T=$(mktemp -d) || exit 1
trap 'rm -rf "$T"' EXIT

g++ -std=c++17 -O2 -Wall -Wextra -Werror -I "$src" -o "$T/game_config_test" "$here/game_config_test.cpp" || { echo "FAIL: game_config_test did not build"; exit 1; }
"$T/game_config_test" || { echo "FAIL: game_config_test"; exit 1; }

python3 -I - "$src/menu.cpp" <<'PY'
import sys
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

out = []
gc = body(text, 'std::wstring gameConfig()')
if gc is None: out.append('gameConfig() not found')
else:
    import re
    flat = re.sub(r'\s+', ' ', gc)
    if not re.search(r'envValue\(\[\]\(wchar_t\* b, DWORD n\) \{ return GetEnvironmentVariableW\(L"USERPROFILE", b, n\); \}\)', flat):
        out.append('gameConfig() does not read USERPROFILE through envValue()')
    if 'gameSettingsRoot(envValue(' not in flat: out.append('gameConfig() does not build its folder with gameSettingsRoot() from USERPROFILE')
    for bad in ('SHGetKnownFolderPath', 'FOLDERID_Documents'):
        if bad in gc: out.append('gameConfig() asks Windows for the Documents known folder (' + bad + ')')
for p in out: print('FAIL:', p)
sys.exit(1 if out else 0)
PY
status=$?
[ $status -eq 0 ] && echo "PASS: game config folder" || echo "FAIL: game config folder"
exit $status
