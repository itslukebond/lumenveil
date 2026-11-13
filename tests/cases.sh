#!/usr/bin/env bash
# The install script tests. Started by tests/run.sh inside the sandbox; never run this file directly.
# Usage (through run.sh): tests/run.sh [sh|ps|static|package|stress] ...   (no argument: everything)
set -u
if [ -z "${FFXV_T:-}" ] || [ "$(cat /proc/1/comm 2>/dev/null)" = systemd ]; then
    echo "cases.sh runs only inside the sandbox: use tests/run.sh" >&2
    exit 1
fi
R=$FFXV_REPO
T=$FFXV_T
PWSH=$FFXV_PWSH
ID_A=76561190000000000
ID_B=76561190000000001
LAUNCH='WINEDLLOVERRIDES="dxgi=n,b" %command%'
URL='https://github.com/NVIDIA/DLSS'
LICENCE="By downloading them you accept NVIDIA's licence for those files."
FSR_URL='https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/archive/refs/tags/v2.4.0.zip'
AMD_LICENCE="By downloading them you accept AMD's licence for those files."
TESTED="Tested on an NVIDIA GeForce RTX 5090 only, where AMD's files run FSR 3.1. Untested on AMD Radeon cards and with AMD's FSR 4."
FSRD=ffxv_dlss/fidelityfx
FSR_NAMES='amd_fidelityfx_loader_dx12.dll amd_fidelityfx_upscaler_dx12.dll amd_fidelityfx_framegeneration_dx12.dll'
XESS_URL='https://github.com/intel/xess/releases/download/v3.0.2/XeSS_SDK_3.0.2.zip'
XESS_ZIP=XeSS_SDK_3.0.2.zip
INTEL_LICENCE="By downloading them you accept Intel's licence for those files."
XESS_TESTED="Tested on an NVIDIA GeForce RTX 5090 only. Untested on Intel Arc."
XESSD=ffxv_dlss/xess
XESS_NAMES='libxess.dll libxess_fg.dll libxell.dll'
pass=0
fail=0
N=0
OUT=/dev/null

ok() { pass=$((pass + 1)); echo "ok   $1"; }
no() {
    fail=$((fail + 1)); echo "FAIL $1"
    [ -s "$OUT" ] && sed 's/^/     | /' "$OUT" | head -60
}
# t NAME COMMAND...: one assertion
t() { local name=$1; shift; if "$@"; then ok "$name"; else no "$name"; fi; }
has() { grep -qF -- "$1" "$OUT"; }
has_in() { grep -qF -- "$2" "$1"; }
hasi() { grep -qiF -- "$1" "$OUT"; }
lacks() { ! grep -qiF -- "$1" "$OUT"; }
eq() { [ "$1" = "$2" ]; }
same() { cmp -s -- "$1" "$2"; }
flat() { tr '\n' ' ' < "$1" | tr -s ' '; }   # a file as one line, so a wrapped sentence still matches

# File list with SHA-256 of every file, folders as "d", relative to $1. Skips the runs' own output files.
snap() {
    (cd "$1" && find . -mindepth 1 ! -name 'out.*.txt' -printf '%y %P\n' | LC_ALL=C sort |
        while IFS= read -r l; do
            p=${l#? }
            if [ "${l%% *}" = f ]; then printf 'f %s %s\n' "$p" "$(sha256sum < "$p" | cut -c1-64)"; else printf '%s\n' "$l"; fi
        done)
}

# ---- fixtures --------------------------------------------------------------------------------------
# The game's settings file, CRLF, modelled on the real one. $1 MaxFramerate (or "none"), $2 RenderingResolutionRatio.
ini_text() {
    printf '[BasicSettings]\r\nResolutionWidth=3840\r\nResolutionHeight=2160\r\nDisplayMode=1\r\n'
    printf 'RenderingResolutionRatio=%s\r\n' "$2"
    if [ "$1" != none ]; then printf 'MaxFramerate=%s\r\n' "$1"; fi
    printf 'VSync=0\r\n\r\n[AdvancedSettings]\r\nModelLOD=2\r\nShadowQuality=3\r\n'
}

# A release folder like the one tools/package.sh makes, with stand-in files. Read-only, as a test of its own.
mkrelease() {
    local d=$1
    mkdir -p "$d/scripts" "$d/game/ffxv_dlss/streamline" "$d/game/ffxv_dlss/fonts" || return 1
    cp "$R/install.sh" "$R/uninstall.sh" "$R/install.bat" "$R/uninstall.bat" "$R/update.sh" "$R/undo-update.sh" \
        "$R/update.bat" "$R/undo-update.bat" "$d/" &&
        cp "$R/scripts/setup.sh" "$R/scripts/setup.ps1" "$d/scripts/" || return 1
    printf 'MZ stand-in proxy, ffxv-dlss build 2\n' > "$d/game/dxgi.dll"
    for f in sl.interposer sl.common sl.dlss_g sl.reflex sl.pcl; do
        printf 'MZ stand-in %s\n' "$f" > "$d/game/ffxv_dlss/streamline/$f.dll"
    done
    for f in texgyreadventor-bold.otf texgyreadventor-regular.otf GUST-FONT-LICENSE.txt; do
        printf 'stand-in %s\n' "$f" > "$d/game/ffxv_dlss/fonts/$f"
    done
    chmod -R a-w "$d"
}

# Stand-in NVIDIA files: "MZ" + 2 MB, different bytes for rel and dev, and a second rel set (v2).
mkdlls() {
    mkdir -p "$T/sdkref"
    python3 -I - "$T/sdkref" <<'PY'
import sys, os
d = sys.argv[1]
def dll(tag, n=2 * 1024 * 1024):
    return b'MZ' + (tag.encode() * (n // len(tag) + 1))[:n]
for name, tag in [('rel_dlss', 'rel dlss '), ('rel_dlssg', 'rel dlssg '), ('dev_dlss', 'dev dlss '),
                  ('dev_dlssg', 'dev dlssg '), ('v2_dlss', 'second rel dlss '), ('v2_dlssg', 'second rel dlssg ')]:
    open(os.path.join(d, name + '.dll'), 'wb').write(dll(tag))
open(os.path.join(d, 'tiny.dll'), 'wb').write(b'MZ12345678')
open(os.path.join(d, 'nomz.dll'), 'wb').write(b'\0' * (2 * 1024 * 1024))
# AMD: a 40 KB loader and 2 MB providers, a second set (v2), a 10 KB loader and a 500 KB provider
for name, tag, n in [('fsr_loader', 'amd loader ', 40 * 1024), ('fsr_up', 'amd upscaler ', 2 * 1024 * 1024),
                     ('fsr_fg', 'amd framegen ', 2 * 1024 * 1024), ('v2fsr_loader', 'second amd loader ', 40 * 1024),
                     ('v2fsr_up', 'second amd upscaler ', 2 * 1024 * 1024), ('v2fsr_fg', 'second amd framegen ', 2 * 1024 * 1024),
                     ('fsr_tinyloader', 'tiny amd loader ', 10 * 1024), ('fsr_tinyprov', 'small amd upscaler ', 500 * 1024)]:
    open(os.path.join(d, name + '.dll'), 'wb').write(dll(tag, n))
# Intel
for name, tag, n in [('xess', 'intel xess ', 2 * 1024 * 1024), ('xess_fg', 'intel xess fg ', 2 * 1024 * 1024),
                     ('xell', 'intel xell ', 200 * 1024), ('xess_dx11', 'intel xess dx11 ', 160 * 1024),
                     ('v2xess', 'second intel xess ', 2 * 1024 * 1024), ('v2xess_fg', 'second intel xess fg ', 2 * 1024 * 1024),
                     ('v2xell', 'second intel xell ', 200 * 1024), ('xess_small', 'small intel xess ', 900 * 1024),
                     ('xell_tiny', 'tiny intel xell ', 100 * 1024)]:
    open(os.path.join(d, name + '.dll'), 'wb').write(dll(tag, n))
PY
}

# mksdk OUT TOP KIND: an SDK zip (OUT ends .zip) or folder. KIND: full, v2, dlssonly, tiny, nomz, decoy
mksdk() {
    python3 -I - "$T/sdkref" "$@" <<'PY'
import sys, os, zipfile
ref, out, top, kind = sys.argv[1:5]
def r(n): return open(os.path.join(ref, n + '.dll'), 'rb').read()
files = {}
if kind != 'decoy':
    rel = {'full': ('rel_dlss', 'rel_dlssg'), 'v2': ('v2_dlss', 'v2_dlssg'), 'dlssonly': ('rel_dlss', None),
           'tiny': ('tiny', 'rel_dlssg'), 'nomz': ('nomz', 'rel_dlssg')}[kind]
    files['lib/Windows_x86_64/rel/nvngx_dlss.dll'] = r(rel[0])
    if rel[1]: files['lib/Windows_x86_64/rel/nvngx_dlssg.dll'] = r(rel[1])
    files['lib/Windows_x86_64/dev/nvngx_dlss.dll'] = r('dev_dlss')
    files['lib/Windows_x86_64/dev/nvngx_dlssg.dll'] = r('dev_dlssg')
    files['include/nvsdk_ngx.h'] = b'// header\n'
else:
    files['samples/readme.txt'] = b'not the SDK\n'
    files['samples/bin/nvngx_dlss.dll'] = r('dev_dlss')
files['README.md'] = b'# DLSS\n'
if out.endswith('.zip'):
    with zipfile.ZipFile(out, 'w') as z:
        z.writestr(top + '/', b'')
        for k, v in files.items(): z.writestr(top + '/' + k, v)
else:
    for k, v in files.items():
        p = os.path.join(out, top, k) if top else os.path.join(out, k)
        os.makedirs(os.path.dirname(p), exist_ok=True)
        open(p, 'wb').write(v)
PY
}

# mkfsr OUT TOP KIND: AMD's SDK as a zip (OUT ends .zip) or folder, the DLLs in Kits/FidelityFX/signedbin.
# KIND: full, v2, nofg, noup, tinyloader, tinyprovider, nomz (the frame generation DLL), decoy (DLLs elsewhere only)
mkfsr() {
    python3 -I - "$T/sdkref" "$@" <<'PY'
import sys, os, zipfile
ref, out, top, kind = sys.argv[1:5]
def r(n): return open(os.path.join(ref, n + '.dll'), 'rb').read()
names = ['amd_fidelityfx_loader_dx12.dll', 'amd_fidelityfx_upscaler_dx12.dll', 'amd_fidelityfx_framegeneration_dx12.dll']
sets = {'full': ('fsr_loader', 'fsr_up', 'fsr_fg'), 'v2': ('v2fsr_loader', 'v2fsr_up', 'v2fsr_fg'),
        'nofg': ('fsr_loader', 'fsr_up', None), 'noup': ('fsr_loader', None, 'fsr_fg'),
        'tinyloader': ('fsr_tinyloader', 'fsr_up', 'fsr_fg'), 'tinyprovider': ('fsr_loader', 'fsr_tinyprov', 'fsr_fg'),
        'nomz': ('fsr_loader', 'fsr_up', 'nomz'), 'decoy': ('fsr_loader', 'fsr_up', 'fsr_fg')}
where = 'Samples/bin/' if kind == 'decoy' else 'Kits/FidelityFX/signedbin/'
files = {where + n: r(s) for n, s in zip(names, sets[kind]) if s}
files['Kits/FidelityFX/api/include/ffx_api.h'] = b'// header\n'
files['README.md'] = b'# FidelityFX SDK\n'
if out.endswith('.zip'):
    with zipfile.ZipFile(out, 'w') as z:
        z.writestr(top + '/', b'')
        for k, v in files.items(): z.writestr(top + '/' + k, v)
else:
    for k, v in files.items():
        p = os.path.join(out, top, k) if top else os.path.join(out, k)
        os.makedirs(os.path.dirname(p), exist_ok=True)
        open(p, 'wb').write(v)
PY
}

# mkxess OUT TOP KIND: Intel's XeSS SDK as a zip (OUT ends .zip) or folder, the DLLs in bin/ as in Intel's release.
# TOP '' puts the entries at the top of the zip. KIND: full, v2, nofg, noxell, smallxess, tinyxell, nomz (the frame
# generation DLL), decoy (DLLs outside bin/ only)
mkxess() {
    python3 -I - "$T/sdkref" "$@" <<'PY'
import sys, os, zipfile
ref, out, top, kind = sys.argv[1:5]
def r(n): return open(os.path.join(ref, n + '.dll'), 'rb').read()
names = ['libxess.dll', 'libxess_fg.dll', 'libxell.dll']
sets = {'full': ('xess', 'xess_fg', 'xell'), 'v2': ('v2xess', 'v2xess_fg', 'v2xell'), 'nofg': ('xess', None, 'xell'),
        'noxell': ('xess', 'xess_fg', None), 'smallxess': ('xess_small', 'xess_fg', 'xell'),
        'tinyxell': ('xess', 'xess_fg', 'xell_tiny'), 'nomz': ('xess', 'nomz', 'xell'), 'decoy': ('xess', 'xess_fg', 'xell')}
where = 'samples/' if kind == 'decoy' else 'bin/'
files = {where + n: r(s) for n, s in zip(names, sets[kind]) if s}
files[where + 'libxess_dx11.dll'] = r('xess_dx11')
files['inc/xess/xess.h'] = b'// header\n'
files['README.md'] = b'# XeSS SDK\n'
if out.endswith('.zip'):
    with zipfile.ZipFile(out, 'w') as z:
        if top: z.writestr(top + '/', b'')
        for k, v in files.items(): z.writestr((top + '/' if top else '') + k, v)
else:
    for k, v in files.items():
        p = os.path.join(out, top, k) if top else os.path.join(out, k)
        os.makedirs(os.path.dirname(p), exist_ok=True)
        open(p, 'wb').write(v)
PY
}

# mkcase NAME [LIBRARY FOLDER NAME]: a fake home with Steam, a second library holding the game, its Proton
# prefix with two Steam accounts' settings files, and an empty Downloads folder.
mkcase() {
    C=$T/cases/$1
    local libname=${2:-SteamLibrary}
    H=$C/home
    STEAM=$H/.local/share/Steam
    LIB=$C/$libname
    GAME="$LIB/steamapps/common/FINAL FANTASY XV"
    DOCS=$LIB/steamapps/compatdata/637650/pfx/drive_c/users/steamuser/Documents
    SAVE="$DOCS/My Games/FINAL FANTASY XV/Steam"
    CFG_A=$SAVE/$ID_A/savestorage/GraphicsConfig.ini
    CFG_B=$SAVE/$ID_B/savestorage/GraphicsConfig.ini
    DL=$H/Downloads
    rm -rf "$C"
    mkdir -p "$STEAM/steamapps" "$STEAM/config" "$STEAM/userdata/12345/config" "$GAME/data" \
        "$(dirname "$CFG_A")" "$(dirname "$CFG_B")" "$DL"
    cat > "$STEAM/steamapps/libraryfolders.vdf" <<EOF
"libraryfolders"
{
	"0"
	{
		"path"		"$STEAM"
		"label"		""
		"contentid"		"1111111111111111111"
		"totalsize"		"0"
		"apps"
		{
			"228980"		"123456"
		}
	}
	"1"
	{
		"path"		"$LIB"
		"label"		""
		"contentid"		"2222222222222222222"
		"totalsize"		"1000000000000"
		"apps"
		{
			"637650"		"100000000000"
		}
	}
}
EOF
    printf '"AppState"\n{\n\t"appid"\t\t"228980"\n\t"installdir"\t\t"Steamworks Shared"\n}\n' > "$STEAM/steamapps/appmanifest_228980.acf"
    printf '"AppState"\n{\n\t"appid"\t\t"637650"\n\t"universe"\t\t"1"\n\t"name"\t\t"FINAL FANTASY XV WINDOWS EDITION"\n\t"StateFlags"\t\t"4"\n\t"installdir"\t\t"FINAL FANTASY XV"\n}\n' > "$LIB/steamapps/appmanifest_637650.acf"
    printf '"InstallConfigStore"\n{\n}\n' > "$STEAM/config/config.vdf"
    printf '"UserLocalConfigStore"\n{\n}\n' > "$STEAM/userdata/12345/config/localconfig.vdf"
    printf 'MZ the game\n' > "$GAME/ffxv_s.exe"
    printf 'game data\n' > "$GAME/data/unrelated.dat"
    ini_text 120 100 > "$CFG_A"
    ini_text 240 100 > "$CFG_B"
}

# run IMPL ACTION [game=DIR] [sdk=PATH] [fsr=PATH] [xess=PATH] [from=PATH] [allowolder] [noprompt] [env:VAR=VALUE] [rel=DIR]
# [cwd=DIR]: runs one script as a player would, from $C or DIR. ACTION undo runs undo-update.sh.
run() {
    local impl=$1 act=$2 rel=$REL o script cwd=$C
    shift 2
    script=$act
    [ "$act" = undo ] && script=undo-update
    local args=() envs=()
    for o in "$@"; do
        case $o in
            game=*) if [ "$impl" = sh ]; then args+=(--game-dir "${o#game=}"); else args+=(-GameDir "${o#game=}"); fi ;;
            sdk=*) if [ "$impl" = sh ]; then args+=(--sdk "${o#sdk=}"); else args+=(-SdkPath "${o#sdk=}"); fi ;;
            fsr=*) if [ "$impl" = sh ]; then args+=(--fsr "${o#fsr=}"); else args+=(-FsrPath "${o#fsr=}"); fi ;;
            xess=*) if [ "$impl" = sh ]; then args+=(--xess "${o#xess=}"); else args+=(-XessPath "${o#xess=}"); fi ;;
            noprompt) if [ "$impl" = sh ]; then args+=(--no-prompt); else args+=(-NoPrompt); fi ;;
            from=*) if [ "$impl" = sh ]; then args+=(--from "${o#from=}"); else args+=(-From "${o#from=}"); fi ;;
            allowolder) if [ "$impl" = sh ]; then args+=(--allow-older); else args+=(-AllowOlder); fi ;;
            env:*) envs+=("${o#env:}") ;;
            rel=*) rel=${o#rel=} ;;
            cwd=*) cwd=${o#cwd=} ;;
        esac
    done
    N=$((N + 1))
    OUT=$C/out.$N.txt
    if [ "$impl" = sh ]; then
        (cd "$cwd" && env HOME="$H" "${envs[@]}" bash "$rel/$script.sh" "${args[@]}" < /dev/null > "$OUT" 2>&1)
    else
        (cd "$cwd" && env "${envs[@]}" "$PWSH" -NoProfile -File "$rel/scripts/setup.ps1" -Action "$act" \
            -SteamRoot "$STEAM" -DocumentsDir "$DOCS" -DownloadsDir "$DL" "${args[@]}" < /dev/null > "$OUT" 2>&1)
    fi
    RC=$?
}

# Every file of the release's game/ folder is in the game folder with the same bytes.
payload_ok() {
    local f n=0 rel=${2:-$REL}
    while IFS= read -r f; do
        n=$((n + 1))
        cmp -s -- "$rel/game/$f" "$1/$f" || return 1
    done < <(cd "$rel/game" && find . -type f -printf '%P\n')
    [ "$n" -ge 9 ]
}
none_installed() { [ ! -e "$GAME/ffxv_dlss" ] && ! grep -qaF 'ffxv-dlss' "$GAME/dxgi.dll" 2>/dev/null; }

# ---- T1 install finds the game in the second library --------------------------------------------------
case_t1() {
    local i=$1
    mkcase "t1-$i"
    run "$i" install
    t "T1 $i exit 0" eq "$RC" 0
    t "T1 $i payload byte-identical" payload_ok "$GAME"
    t "T1 $i output names the game folder" has "$GAME"
    t "T1 $i install record written" test -s "$GAME/ffxv_dlss/installed_files.txt"
}

# ---- T2 game not found ---------------------------------------------------------------------------------
case_t2() {
    local i=$1 before
    mkcase "t2-$i"
    rm -rf "$STEAM/steamapps"
    before=$(snap "$C")
    run "$i" install noprompt
    t "T2 $i no Steam folder: exit 3" eq "$RC" 3
    t "T2 $i no Steam folder: nothing written" eq "$(snap "$C")" "$before"
    if [ "$i" = sh ]; then t "T2 $i says how to pass the folder" has "--game-dir"; else t "T2 $i says how to pass the folder" has "-GameDir"; fi
    mkdir -p "$C/notgame"
    before=$(snap "$C")
    run "$i" install noprompt game="$C/notgame"
    t "T2 $i folder without ffxv_s.exe: exit 3" eq "$RC" 3
    t "T2 $i folder without ffxv_s.exe: nothing written" eq "$(snap "$C")" "$before"
    t "T2 $i folder without ffxv_s.exe: says so" has "ffxv_s.exe"
    run "$i" uninstall noprompt
    t "T2 $i uninstall, no game: exit 3" eq "$RC" 3
    t "T2 $i uninstall, no game: nothing written" eq "$(snap "$C")" "$before"
    # a prompt answered by the player (the folder dragged in, with quotes)
    if [ "$i" = sh ]; then
        N=$((N + 1)); OUT=$C/out.$N.txt
        (cd "$C" && printf "%s\n'%s'\n" "$C/notgame" "$GAME" | HOME="$H" script -qec "bash '$REL/install.sh'" /dev/null > "$OUT" 2>&1)
        RC=$?
    else
        N=$((N + 1)); OUT=$C/out.$N.txt
        (cd "$C" && "$PWSH" -NoProfile -File "$R/tests/unit.ps1" -Case prompt -Script "$REL/scripts/setup.ps1" \
            -SteamRoot "$STEAM" -DocumentsDir "$DOCS" -DownloadsDir "$DL" -Answer1 "$C/notgame" -Answer2 "\"$GAME\"" < /dev/null > "$OUT" 2>&1)
        RC=$?
    fi
    t "T2 $i prompt: answer used, exit 0" eq "$RC" 0
    t "T2 $i prompt: folder without the game refused first" has "ffxv_s.exe"
    t "T2 $i prompt: installed in the answered folder" payload_ok "$GAME"
}

# ---- T3 game running -----------------------------------------------------------------------------------
case_t3() {
    local i=$1 before name pid
    mkcase "t3-$i"
    if [ "$i" = sh ]; then name=ffxv_s.exe; else name=ffxv_s; fi
    mkdir -p "$T/proc-$i" && cp /usr/bin/sleep "$T/proc-$i/$name"
    run "$i" install
    t "T3 $i first install (game closed)" eq "$RC" 0
    "$T/proc-$i/$name" 300 &
    pid=$!
    t "T3 $i the game-like process is running" eq "$(cat /proc/$pid/comm)" "$name"
    before=$(snap "$C")
    run "$i" install
    t "T3 $i install while running: exit 2" eq "$RC" 2
    t "T3 $i install while running: says close the game" has "Close FINAL FANTASY XV first"
    t "T3 $i install while running: nothing changed" eq "$(snap "$C")" "$before"
    run "$i" uninstall
    t "T3 $i uninstall while running: exit 2" eq "$RC" 2
    t "T3 $i uninstall while running: nothing changed" eq "$(snap "$C")" "$before"
    UI=$i
    loose "$DL" v2 dlss
    printf 'replaced ffxv_dlss/nvngx_dlss.dll\n' > "$GAME/ffxv_dlss/last_update.txt"
    before=$(snap "$C")
    run "$i" update
    t "T3 $i update while running: exit 2" eq "$RC" 2
    t "T3 $i update while running: says close the game" has "Close FINAL FANTASY XV first"
    t "T3 $i update while running: nothing changed" eq "$(snap "$C")" "$before"
    run "$i" undo
    t "T3 $i undo while running: exit 2" eq "$RC" 2
    t "T3 $i undo while running: nothing changed" eq "$(snap "$C")" "$before"
    rm -f "$GAME/ffxv_dlss/last_update.txt"
    kill "$pid"; wait "$pid" 2> /dev/null
    run "$i" uninstall
    t "T3 $i uninstall after the game closed: exit 0" eq "$RC" 0
    t "T3 $i uninstall after the game closed: mod gone" none_installed
}

# ---- T4 another tool's dxgi.dll --------------------------------------------------------------------------
case_t4() {
    local i=$1 h
    mkcase "t4-$i"
    printf 'MZ another tool proxy\n' > "$C/foreign.dll"
    cp "$C/foreign.dll" "$GAME/dxgi.dll"
    run "$i" install
    t "T4 $i exit 0" eq "$RC" 0
    t "T4 $i foreign dxgi.dll kept under the backup name" same "$C/foreign.dll" "$GAME/dxgi.dll.before_ffxv_dlss"
    t "T4 $i mod dxgi.dll in place" same "$REL/game/dxgi.dll" "$GAME/dxgi.dll"
    t "T4 $i player told" has "dxgi.dll.before_ffxv_dlss"
    run "$i" install
    t "T4 $i second install: backup untouched" same "$C/foreign.dll" "$GAME/dxgi.dll.before_ffxv_dlss"
    t "T4 $i second install: exit 0" eq "$RC" 0
    # backup name taken and another foreign dxgi.dll: refuse
    mkcase "t4b-$i"
    printf 'MZ first tool\n' > "$C/one.dll"; printf 'MZ second tool\n' > "$C/two.dll"
    cp "$C/one.dll" "$GAME/dxgi.dll.before_ffxv_dlss"; cp "$C/two.dll" "$GAME/dxgi.dll"
    run "$i" install
    t "T4 $i two foreign files: exit 4" eq "$RC" 4
    t "T4 $i two foreign files: backup unchanged" same "$C/one.dll" "$GAME/dxgi.dll.before_ffxv_dlss"
    t "T4 $i two foreign files: dxgi.dll unchanged" same "$C/two.dll" "$GAME/dxgi.dll"
    t "T4 $i two foreign files: no mod files copied" test ! -e "$GAME/ffxv_dlss/streamline"
    # an older copy of this mod: replaced, no backup
    mkcase "t4c-$i"
    printf 'MZ old build of ffxv-dlss\n' > "$GAME/dxgi.dll"
    run "$i" install
    t "T4 $i older mod dxgi.dll: replaced" same "$REL/game/dxgi.dll" "$GAME/dxgi.dll"
    t "T4 $i older mod dxgi.dll: no backup" test ! -e "$GAME/dxgi.dll.before_ffxv_dlss"
}

sdk_ok() { # $1 dlss reference, $2 dlssg reference
    same "$T/sdkref/$1.dll" "$GAME/ffxv_dlss/nvngx_dlss.dll" && same "$T/sdkref/$2.dll" "$GAME/ffxv_dlss/streamline/nvngx_dlssg.dll"
}
fsr_ok() { # [$1 v2]: AMD's three files in place, with the bytes of that set
    local p=${1:-}
    same "$T/sdkref/${p}fsr_loader.dll" "$GAME/$FSRD/amd_fidelityfx_loader_dx12.dll" &&
        same "$T/sdkref/${p}fsr_up.dll" "$GAME/$FSRD/amd_fidelityfx_upscaler_dx12.dll" &&
        same "$T/sdkref/${p}fsr_fg.dll" "$GAME/$FSRD/amd_fidelityfx_framegeneration_dx12.dll"
}
no_fsr() { [ ! -e "$GAME/$FSRD" ]; }
xess_ok() { # [$1 v2]: Intel's three files in place, with the bytes of that set
    local p=${1:-}
    same "$T/sdkref/${p}xess.dll" "$GAME/$XESSD/libxess.dll" && same "$T/sdkref/${p}xess_fg.dll" "$GAME/$XESSD/libxess_fg.dll" &&
        same "$T/sdkref/${p}xell.dll" "$GAME/$XESSD/libxell.dll"
}
no_xess() { [ ! -e "$GAME/$XESSD" ]; }
xess_recorded() { grep -cE '^ffxv_dlss/xess/(libxess|libxess_fg|libxell)\.dll$' "$GAME/ffxv_dlss/installed_files.txt"; }
step_list() { grep -oE '^[0-9]+ of [0-9]+  .*' "$OUT"; }
steps_numbered() { # every step heading reads K of N, K counting up from 1 and N the number of headings
    local n
    n=$(step_list | wc -l)
    [ "$n" -ge 6 ] && step_list | awk -v n="$n" '{ if ($1 != NR || $3 != n) bad = 1 } END { exit bad }'
}
no_part() { [ -z "$(find "$GAME" -name '*ffxv_dlss_part*' -print -quit)" ]; }

# ---- T5 NVIDIA files from the player's download -----------------------------------------------------------
case_t5() {
    local i=$1 v extra
    for v in ${2:-default}; do
        extra=()
        [ "$v" != default ] && extra=("env:PATH=$T/bins/$v:$T/bins/base")
        mkcase "t5a-$i-$v"
        mksdk "$DL/DLSS-main.zip" DLSS-main full && touch -d '2026-01-01 10:00' "$DL/DLSS-main.zip"
        mksdk "$DL/DLSS-Sample.zip" DLSS-Sample decoy && touch -d '2026-01-05 10:00' "$DL/DLSS-Sample.zip"
        run "$i" install "${extra[@]}"
        t "T5 $i $v zip in Downloads beside a newer decoy: exit 0" eq "$RC" 0
        t "T5 $i $v zip in Downloads: rel files in place" sdk_ok rel_dlss rel_dlssg
        t "T5 $i $v zip in Downloads: recorded" grep -qx 'ffxv_dlss/streamline/nvngx_dlssg.dll' "$GAME/ffxv_dlss/installed_files.txt"
        t "T5 $i $v zip in Downloads: no temporary file left" eval '[ "$(find "$GAME" -type f | grep -vc -e "/ffxv_dlss/" -e "ffxv_s.exe$" -e "unrelated.dat$" -e "/dxgi.dll$")" = 0 ] && no_part'
    done
    [ -n "${2:-}" ] && return
    mkcase "t5f-$i"
    mksdk "$DL/DLSS-310.zip" DLSS-310 full && touch -d '2026-01-01 10:00' "$DL/DLSS-310.zip"
    mksdk "$DL/DLSS-main.zip" DLSS-main v2 && touch -d '2026-01-03 10:00' "$DL/DLSS-main.zip"
    run "$i" install
    t "T5 $i two good zips: the newest is used" sdk_ok v2_dlss v2_dlssg
    mkcase "t5b-$i"
    mksdk "$DL" DLSS-main full
    run "$i" install
    t "T5 $i extracted folder in Downloads" sdk_ok rel_dlss rel_dlssg
    mkcase "t5c-$i"
    mkdir -p "$DL/DLSS-main" && mksdk "$DL/DLSS-main" DLSS-main full
    run "$i" install
    t "T5 $i doubly nested extracted folder" sdk_ok rel_dlss rel_dlssg
    mkcase "t5d-$i"
    mkdir -p "$C/elsewhere" && mksdk "$C/elsewhere/nvidia sdk.zip" DLSS-main full
    run "$i" install sdk="$C/elsewhere/nvidia sdk.zip"
    t "T5 $i explicit zip path" sdk_ok rel_dlss rel_dlssg
    mkcase "t5e-$i"
    mkdir -p "$C/elsewhere" && mksdk "$C/elsewhere" "my sdk" full
    run "$i" install sdk="$C/elsewhere/my sdk"
    t "T5 $i explicit folder path" sdk_ok rel_dlss rel_dlssg
}

# ---- T6 no SDK, bad SDK, partial SDK, already in place -----------------------------------------------------
case_t6() {
    local i=$1 sep k before
    if [ "$i" = sh ]; then sep=/; else sep='\'; fi
    mkcase "t6-$i"
    run "$i" install
    t "T6 $i no SDK: exit 0" eq "$RC" 0
    t "T6 $i no SDK: mod files installed" payload_ok "$GAME"
    t "T6 $i no SDK: download address" has "$URL"
    t "T6 $i no SDK: names nvngx_dlss.dll and where it goes" has "ffxv_dlss${sep}nvngx_dlss.dll"
    t "T6 $i no SDK: names nvngx_dlssg.dll and where it goes" has "ffxv_dlss${sep}streamline${sep}nvngx_dlssg.dll"
    t "T6 $i no SDK: licence sentence" has "$LICENCE"
    t "T6 $i no SDK: Download ZIP named" has "Download ZIP"
    for k in tiny nomz; do
        mkcase "t6$k-$i"
        mksdk "$DL/DLSS-main.zip" DLSS-main "$k"
        run "$i" install
        t "T6 $i $k nvngx_dlss.dll: exit 0" eq "$RC" 0
        t "T6 $i $k nvngx_dlss.dll: refused and reported" hasi "not a valid DLL"
        t "T6 $i $k nvngx_dlss.dll: nothing copied from it" eval '[ ! -e "$GAME/ffxv_dlss/nvngx_dlss.dll" ] && [ ! -e "$GAME/ffxv_dlss/streamline/nvngx_dlssg.dll" ]'
    done
    mkcase "t6one-$i"
    mksdk "$DL/DLSS-main.zip" DLSS-main dlssonly
    run "$i" install
    t "T6 $i only nvngx_dlss.dll: exit 0" eq "$RC" 0
    t "T6 $i only nvngx_dlss.dll: copied" same "$T/sdkref/rel_dlss.dll" "$GAME/ffxv_dlss/nvngx_dlss.dll"
    t "T6 $i only nvngx_dlss.dll: Frame Generation note" has "Frame Generation needs"
    t "T6 $i only nvngx_dlss.dll: no dlssg" test ! -e "$GAME/ffxv_dlss/streamline/nvngx_dlssg.dll"
    mkcase "t6inplace-$i"
    mksdk "$DL/DLSS-main.zip" DLSS-main full
    run "$i" install
    rm "$DL/DLSS-main.zip"
    before=$(snap "$GAME")
    run "$i" install
    t "T6 $i already in place: reported" hasi "already in place"
    t "T6 $i already in place: bytes unchanged" eq "$(snap "$GAME")" "$before"
    t "T6 $i already in place: exit 0" eq "$RC" 0
}

# ---- T7 fps cap ------------------------------------------------------------------------------------------
case_t7() {
    local i=$1
    mkcase "t7-$i"
    run "$i" install
    t "T7 $i 120 becomes 240, every other byte kept" eq "$(ini_text 240 100 | sha256sum)" "$(sha256sum < "$CFG_A")"
    t "T7 $i same length" eq "$(stat -c %s "$CFG_A")" "$(ini_text 120 100 | wc -c)"
    t "T7 $i backup holds the original bytes" eq "$(ini_text 120 100 | sha256sum)" "$(sha256sum < "$CFG_A.before_ffxv_dlss")"
    t "T7 $i reported from 120 to 240" has "from 120 to 240"
    t "T7 $i account at 240 unchanged" eq "$(ini_text 240 100 | sha256sum)" "$(sha256sum < "$CFG_B")"
    t "T7 $i account at 240: no backup" test ! -e "$CFG_B.before_ffxv_dlss"
    t "T7 $i account at 240: reported already" hasi "already 240"
    run "$i" install
    t "T7 $i second install: backup still the original" eq "$(ini_text 120 100 | sha256sum)" "$(sha256sum < "$CFG_A.before_ffxv_dlss")"
    ini_text 60 100 > "$CFG_A"
    run "$i" install
    t "T7 $i limit lowered after install, installed again: raised" eq "$(ini_text 240 100 | sha256sum)" "$(sha256sum < "$CFG_A")"
    t "T7 $i limit lowered after install, installed again: backup still the original" eq "$(ini_text 120 100 | sha256sum)" "$(sha256sum < "$CFG_A.before_ffxv_dlss")"
    mkcase "t7b-$i"
    ini_text 60 100 > "$CFG_A"
    ini_text none 100 > "$CFG_B"
    run "$i" install
    t "T7 $i 60 becomes 240" eq "$(ini_text 240 100 | sha256sum)" "$(sha256sum < "$CFG_A")"
    t "T7 $i no MaxFramerate line: unchanged" eq "$(ini_text none 100 | sha256sum)" "$(sha256sum < "$CFG_B")"
    t "T7 $i no MaxFramerate line: reported skipped" hasi "skipped"
    t "T7 $i no MaxFramerate line: no backup" test ! -e "$CFG_B.before_ffxv_dlss"
    mkcase "t7c-$i"
    rm -rf "$DOCS/My Games"
    run "$i" install
    t "T7 $i no settings file: exit 0" eq "$RC" 0
    t "T7 $i no settings file: nothing created" test ! -e "$DOCS/My Games"
    t "T7 $i no settings file: skipped with the by-hand line" eval 'hasi skipped && has MaxFramerate=240'
    mkcase "t7d-$i"
    rm -rf "$LIB/steamapps/compatdata"
    run "$i" install
    t "T7 $i no Documents tree: exit 0" eq "$RC" 0
    t "T7 $i no Documents tree: nothing created" test ! -e "$LIB/steamapps/compatdata"
    t "T7 $i no Documents tree: skipped with the by-hand line" eval 'hasi skipped && has MaxFramerate=240'
    mkcase "t7e-$i"
    chmod a-w "$CFG_A"
    run "$i" install
    t "T7 $i read-only settings file: exit 0" eq "$RC" 0
    t "T7 $i read-only settings file: unchanged" eq "$(ini_text 120 100 | sha256sum)" "$(sha256sum < "$CFG_A")"
    t "T7 $i read-only settings file: reported failed" hasi "failed"
    t "T7 $i read-only settings file: by-hand line" has "MaxFramerate=240"
    t "T7 $i read-only settings file: no backup" test ! -e "$CFG_A.before_ffxv_dlss"
}

# ---- T8 Linux launch option ----------------------------------------------------------------------------------
case_t8() {
    local before
    mkcase t8
    mkdir -p "$T/fakeclip/ok" "$T/fakeclip/bad"
    printf '#!/bin/sh\ncat > "$FAKE_CLIP"\nexit 0\n' > "$T/fakeclip/ok/wl-copy"
    printf '#!/bin/sh\ncat > /dev/null\nexit 1\n' > "$T/fakeclip/bad/wl-copy"
    chmod +x "$T/fakeclip/ok/wl-copy" "$T/fakeclip/bad/wl-copy"
    before=$(snap "$STEAM")
    run sh install "env:PATH=$T/fakeclip/ok:$PATH" env:WAYLAND_DISPLAY=fake "env:FAKE_CLIP=$C/clip.txt"
    t "T8 launch option alone on a line" grep -qxF -- "$LAUNCH" "$OUT"
    t "T8 says where to paste it" has "Properties, General, Launch Options"
    t "T8 clipboard holds exactly the option" eq "$(od -An -c "$C/clip.txt")" "$(printf '%s' "$LAUNCH" | od -An -c)"
    t "T8 says copied" hasi "copied to the clipboard"
    run sh install "env:PATH=$T/fakeclip/bad:$PATH" env:WAYLAND_DISPLAY=fake
    t "T8 clipboard tool fails: not said copied" lacks "copied to the clipboard"
    t "T8 clipboard tool fails: option still printed" grep -qxF -- "$LAUNCH" "$OUT"
    run sh install "env:PATH=$T/fakeclip/ok:$PATH" "env:FAKE_CLIP=$C/clip2.txt"
    t "T8 no display: not said copied" lacks "copied to the clipboard"
    t "T8 no display: clipboard tool not run" test ! -e "$C/clip2.txt"
    run sh uninstall
    t "T8 uninstall reminds to remove the launch option" has 'WINEDLLOVERRIDES="dxgi=n,b"'
    t "T8 Steam's own files unchanged by install and uninstall" eq "$(snap "$STEAM")" "$before"
}

# ---- T9 install then uninstall puts things back -----------------------------------------------------------------
case_t9() {
    local i=$1 g d
    mkcase "t9-$i"
    printf 'MZ another tool proxy\n' > "$GAME/dxgi.dll"
    mksdk "$DL/DLSS-main.zip" DLSS-main full
    g=$(snap "$GAME"); d=$(snap "$DOCS")
    run "$i" install
    t "T9 $i install exit 0" eq "$RC" 0
    t "T9 $i install changed the game folder" test "$(snap "$GAME")" != "$g"
    run "$i" uninstall
    t "T9 $i uninstall exit 0" eq "$RC" 0
    t "T9 $i game folder as before (foreign dxgi.dll back)" eq "$(snap "$GAME")" "$g"
    t "T9 $i Documents as before (MaxFramerate back to 120)" eq "$(snap "$DOCS")" "$d"
    t "T9 $i MaxFramerate back to 120" eq "$(ini_text 120 100 | sha256sum)" "$(sha256sum < "$CFG_A")"
    # files the player added and files the mod wrote while running
    mkcase "t9b-$i"
    g=$(snap "$GAME")
    run "$i" install
    mkdir -p "$GAME/ffxv_dlss/ui"
    printf 'player art\n' > "$GAME/ffxv_dlss/ui/a.png"
    printf '[dlss]\r\npreset=11\r\n' > "$GAME/ffxv_dlss.ini"
    printf 'log\n' > "$GAME/ffxv_dlss.log"
    printf 'log\n' > "$GAME/ffxv_dlss/nvngx.log"
    printf 'log\n' > "$GAME/ffxv_dlss/streamline/sl.log"
    run "$i" uninstall
    t "T9 $i mod-written files removed" eval '[ ! -e "$GAME/ffxv_dlss.ini" ] && [ ! -e "$GAME/ffxv_dlss.log" ] && [ ! -e "$GAME/ffxv_dlss/nvngx.log" ] && [ ! -e "$GAME/ffxv_dlss/streamline" ]'
    t "T9 $i player art kept" eq "$(cat "$GAME/ffxv_dlss/ui/a.png")" "player art"
    t "T9 $i player art listed as left alone" has "a.png"
    t "T9 $i nothing else left" eq "$(snap "$GAME" | LC_ALL=C sort)" \
        "$(printf '%s\nd ffxv_dlss\nd ffxv_dlss/ui\nf ffxv_dlss/ui/a.png %s\n' "$g" "$(printf 'player art\n' | sha256sum | cut -c1-64)" | LC_ALL=C sort)"
    # a foreign dxgi.dll put there after the install
    mkcase "t9c-$i"
    printf 'MZ first tool\n' > "$C/one.dll"; printf 'MZ later tool\n' > "$C/two.dll"
    cp "$C/one.dll" "$GAME/dxgi.dll"
    run "$i" install
    cp "$C/two.dll" "$GAME/dxgi.dll"
    run "$i" uninstall
    t "T9 $i later foreign dxgi.dll not deleted" same "$C/two.dll" "$GAME/dxgi.dll"
    t "T9 $i backup left beside it" same "$C/one.dll" "$GAME/dxgi.dll.before_ffxv_dlss"
    t "T9 $i both left: said so" has "dxgi.dll.before_ffxv_dlss"
}

# ---- T10 later setting changes are kept ---------------------------------------------------------------------------
case_t10() {
    local i=$1
    mkcase "t10-$i"
    run "$i" install
    ini_text 144 50 > "$CFG_A"
    run "$i" uninstall
    t "T10 $i player's later cap and render scale kept" eq "$(ini_text 144 50 | sha256sum)" "$(sha256sum < "$CFG_A")"
    t "T10 $i backup removed" test ! -e "$CFG_A.before_ffxv_dlss"
    t "T10 $i render scale reminder" has "render scale is 50%"
    t "T10 $i says the limit was left" has "now 144"
    mkcase "t10b-$i"
    run "$i" install
    ini_text 240 50 > "$CFG_A"
    run "$i" uninstall
    t "T10 $i only the cap restored, render scale kept" eq "$(ini_text 120 50 | sha256sum)" "$(sha256sum < "$CFG_A")"
    t "T10 $i backup removed (b)" test ! -e "$CFG_A.before_ffxv_dlss"
}

# ---- T11 uninstall never deletes outside the mod's own files ----------------------------------------------------------
case_t11() {
    local i=$1 before e
    mkcase "t11-$i"
    printf 'MZ another tool proxy\n' > "$GAME/dxgi.dll"
    before=$(snap "$GAME")
    run "$i" uninstall
    t "T11 $i no record: exit 5" eq "$RC" 5
    t "T11 $i no record: game folder unchanged" eq "$(snap "$GAME")" "$before"
    t "T11 $i no record: said so" has "No install record found, nothing was removed"
    t "T11 $i no record: by-hand list" eval 'has ffxv_dlss.ini && has ffxv_dlss.log'
    mkcase "t11b-$i"
    run "$i" install
    printf 'victim\n' > "$LIB/steamapps/common/victim.txt"
    printf 'victim\n' > "$C/victim_abs.txt"
    printf 'victim\n' > "$GAME/victim2.txt"
    printf 'victim\n' > "$LIB/steamapps/victim3.txt"
    chmod u+w "$GAME/ffxv_dlss/installed_files.txt"
    printf '%s\n' '../victim.txt' "$C/victim_abs.txt" 'ffxv_s.exe' 'ffxv_dlss/../victim2.txt' 'ffxv_dlss\..\..\..\victim3.txt' 'C:/victim.txt' \
        >> "$GAME/ffxv_dlss/installed_files.txt"
    run "$i" uninstall
    t "T11 $i poisoned record: ../victim.txt kept" test -e "$LIB/steamapps/common/victim.txt"
    t "T11 $i poisoned record: absolute path kept" test -e "$C/victim_abs.txt"
    t "T11 $i poisoned record: ffxv_s.exe kept" test -e "$GAME/ffxv_s.exe"
    t "T11 $i poisoned record: ffxv_dlss/../victim2.txt kept" test -e "$GAME/victim2.txt"
    t "T11 $i poisoned record: backslash path kept" test -e "$LIB/steamapps/victim3.txt"
    for e in '../victim.txt' "$C/victim_abs.txt" 'ffxv_s.exe' 'ffxv_dlss/../victim2.txt' 'ffxv_dlss\..\..\..\victim3.txt' 'C:/victim.txt'; do
        t "T11 $i poisoned record: $e reported skipped" grep -qF -- "Skipped $e" "$OUT"
    done
    t "T11 $i poisoned record: valid entries removed" eval '[ ! -e "$GAME/dxgi.dll" ] && [ ! -e "$GAME/ffxv_dlss" ]'
}

# ---- T12 a failed copy part way through ------------------------------------------------------------------------------
case_t12() {
    local i=$1
    mkcase "t12-$i"
    mkdir -p "$GAME/ffxv_dlss"
    printf 'in the way\n' > "$GAME/ffxv_dlss/streamline"
    run "$i" install
    t "T12 $i exit 4" eq "$RC" 4
    t "T12 $i names the path" has "streamline"
    t "T12 $i record exists" test -s "$GAME/ffxv_dlss/installed_files.txt"
    run "$i" uninstall
    t "T12 $i uninstall removes what was copied" eval '[ ! -e "$GAME/dxgi.dll" ] && [ ! -e "$GAME/ffxv_dlss/fonts" ] && [ ! -e "$GAME/ffxv_dlss/installed_files.txt" ]'
    t "T12 $i the plain file is left" eq "$(cat "$GAME/ffxv_dlss/streamline")" "in the way"
}

# ---- T14 awkward library path through the whole chain -----------------------------------------------------------------
case_t14() {
    local i=$1 g d
    mkcase "t14-$i" "Stéam Library's (1)"
    printf 'MZ another tool proxy\n' > "$GAME/dxgi.dll"
    mksdk "$DL/DLSS-main.zip" DLSS-main full
    g=$(snap "$GAME"); d=$(snap "$DOCS")
    run "$i" install
    t "T14 $i exit 0" eq "$RC" 0
    t "T14 $i found the game" has "$GAME"
    t "T14 $i payload" payload_ok "$GAME"
    t "T14 $i NVIDIA files" sdk_ok rel_dlss rel_dlssg
    t "T14 $i fps cap" eq "$(ini_text 240 100 | sha256sum)" "$(sha256sum < "$CFG_A")"
    run "$i" uninstall
    t "T14 $i uninstall exit 0" eq "$RC" 0
    t "T14 $i game folder as before" eq "$(snap "$GAME")" "$g"
    t "T14 $i Documents as before" eq "$(snap "$DOCS")" "$d"
}

# ---- T20 AMD's files from the player's download -------------------------------------------------------------------
case_t20() {
    local i=$1
    mkcase "t20a-$i"
    mksdk "$DL/DLSS-main.zip" DLSS-main full
    mkfsr "$DL/FidelityFX-SDK-2.4.0.zip" FidelityFX-SDK-2.4.0 full && touch -d '2026-01-01 10:00' "$DL/FidelityFX-SDK-2.4.0.zip"
    mkfsr "$DL/FidelityFX-SDK-main.zip" FidelityFX-SDK-main decoy && touch -d '2026-01-05 10:00' "$DL/FidelityFX-SDK-main.zip"
    run "$i" install
    t "T20 $i zip in Downloads beside a newer zip without signedbin: exit 0" eq "$RC" 0
    t "T20 $i zip in Downloads: the signedbin files in place" fsr_ok
    t "T20 $i zip in Downloads: all three recorded" eq "$(grep -cE '^ffxv_dlss/fidelityfx/amd_fidelityfx_(loader|upscaler|framegeneration)_dx12\.dll$' "$GAME/ffxv_dlss/installed_files.txt")" 3
    t "T20 $i zip in Downloads: no temporary file left" no_part
    t "T20 $i zip in Downloads: NVIDIA files as before" sdk_ok rel_dlss rel_dlssg
    t "T20 $i zip in Downloads: summary line" has "AMD FSR files: done"
    mkcase "t20f-$i"
    mkfsr "$DL/FidelityFX-SDK-2.3.0.zip" FidelityFX-SDK-2.3.0 full && touch -d '2026-01-01 10:00' "$DL/FidelityFX-SDK-2.3.0.zip"
    mkfsr "$DL/FidelityFX-SDK-2.4.0.zip" FidelityFX-SDK-2.4.0 v2 && touch -d '2026-01-03 10:00' "$DL/FidelityFX-SDK-2.4.0.zip"
    run "$i" install
    t "T20 $i two good zips: the newest is used" fsr_ok v2
    mkcase "t20b-$i"
    mkfsr "$DL" FidelityFX-SDK-2.4.0 full
    run "$i" install
    t "T20 $i extracted folder in Downloads" fsr_ok
    mkcase "t20c-$i"
    mkdir -p "$DL/FidelityFX-SDK-2.4.0" && mkfsr "$DL/FidelityFX-SDK-2.4.0" FidelityFX-SDK-2.4.0 full
    run "$i" install
    t "T20 $i doubly nested extracted folder" fsr_ok
    mkcase "t20d-$i"
    mkdir -p "$C/elsewhere" && mkfsr "$C/elsewhere/amd sdk.zip" FidelityFX-SDK-2.4.0 full
    run "$i" install fsr="$C/elsewhere/amd sdk.zip"
    t "T20 $i explicit zip path" fsr_ok
    mkcase "t20e-$i"
    mkdir -p "$C/elsewhere" && mkfsr "$C/elsewhere" "my amd sdk" full
    run "$i" install fsr="$C/elsewhere/my amd sdk"
    t "T20 $i explicit folder path" fsr_ok
}

# ---- T21 no AMD files, bad AMD files, no frame generation DLL, already in place --------------------------------------
case_t21() {
    local i=$1 sep k n before
    if [ "$i" = sh ]; then sep=/; else sep='\'; fi
    mkcase "t21-$i"
    mksdk "$DL/DLSS-main.zip" DLSS-main full
    run "$i" install
    t "T21 $i no AMD files: exit 0" eq "$RC" 0
    t "T21 $i no AMD files: mod files installed" payload_ok "$GAME"
    t "T21 $i no AMD files: NVIDIA files as before" sdk_ok rel_dlss rel_dlssg
    t "T21 $i no AMD files: download address" has "$FSR_URL"
    for n in $FSR_NAMES; do t "T21 $i no AMD files: names $n" has "$n"; done
    t "T21 $i no AMD files: destination ffxv_dlss${sep}fidelityfx" has "ffxv_dlss${sep}fidelityfx"
    t "T21 $i no AMD files: licence sentence" has "$AMD_LICENCE"
    t "T21 $i no AMD files: says FSR is optional" hasi "optional"
    t "T21 $i no AMD files: no AMD folder made" no_fsr
    t "T21 $i no AMD files: not under Still to do" eval '! grep -i "still to do" "$OUT" | grep -qi "amd"'
    for k in tinyloader tinyprovider nomz; do
        mkcase "t21$k-$i"
        mkfsr "$DL/FidelityFX-SDK-2.4.0.zip" FidelityFX-SDK-2.4.0 "$k"
        run "$i" install
        t "T21 $i $k: exit 0" eq "$RC" 0
        t "T21 $i $k: refused and reported" hasi "not a valid DLL"
        t "T21 $i $k: nothing copied from that download" no_fsr
        t "T21 $i $k: no temporary file left" no_part
    done
    mkcase "t21noup-$i"
    mkfsr "$DL/FidelityFX-SDK-2.4.0.zip" FidelityFX-SDK-2.4.0 noup
    run "$i" install
    t "T21 $i no upscaler DLL: exit 0" eq "$RC" 0
    t "T21 $i no upscaler DLL: names it" has "amd_fidelityfx_upscaler_dx12.dll"
    t "T21 $i no upscaler DLL: nothing copied" no_fsr
    mkcase "t21nofg-$i"
    mkfsr "$DL/FidelityFX-SDK-2.4.0.zip" FidelityFX-SDK-2.4.0 nofg
    run "$i" install
    t "T21 $i no frame generation DLL: exit 0" eq "$RC" 0
    t "T21 $i no frame generation DLL: the other two copied" eval 'same "$T/sdkref/fsr_loader.dll" "$GAME/$FSRD/amd_fidelityfx_loader_dx12.dll" && same "$T/sdkref/fsr_up.dll" "$GAME/$FSRD/amd_fidelityfx_upscaler_dx12.dll"'
    t "T21 $i no frame generation DLL: none in place" test ! -e "$GAME/$FSRD/amd_fidelityfx_framegeneration_dx12.dll"
    t "T21 $i no frame generation DLL: says FSR frame generation needs it" has "FSR frame generation needs"
    mkcase "t21inplace-$i"
    mkfsr "$DL/FidelityFX-SDK-2.4.0.zip" FidelityFX-SDK-2.4.0 full
    run "$i" install
    rm "$DL/FidelityFX-SDK-2.4.0.zip"
    before=$(snap "$GAME")
    run "$i" install
    t "T21 $i AMD files already in place: reported" has "AMD's FSR files are already in place."
    t "T21 $i AMD files already in place: bytes unchanged" eq "$(snap "$GAME")" "$before"
    t "T21 $i AMD files already in place: exit 0" eq "$RC" 0
}

# ---- T22 uninstall takes AMD's files away ------------------------------------------------------------------------------
case_t22() {
    local i=$1 g
    mkcase "t22-$i"
    printf 'MZ another tool proxy\n' > "$GAME/dxgi.dll"
    mksdk "$DL/DLSS-main.zip" DLSS-main full
    mkfsr "$DL/FidelityFX-SDK-2.4.0.zip" FidelityFX-SDK-2.4.0 full
    g=$(snap "$GAME")
    run "$i" install
    t "T22 $i installed with AMD files" fsr_ok
    run "$i" uninstall
    t "T22 $i uninstall exit 0" eq "$RC" 0
    t "T22 $i AMD folder gone" no_fsr
    t "T22 $i game folder as before" eq "$(snap "$GAME")" "$g"
    mkcase "t22b-$i"
    mkfsr "$DL/FidelityFX-SDK-2.4.0.zip" FidelityFX-SDK-2.4.0 tinyprovider
    g=$(snap "$GAME")
    run "$i" install
    run "$i" uninstall
    t "T22 $i refused AMD download, then uninstall: game folder as before" eq "$(snap "$GAME")" "$g"
}

# ---- T24 Intel's files from the player's download ----------------------------------------------------------------------
case_t24() {
    local i=$1
    mkcase "t24a-$i"
    mksdk "$DL/DLSS-main.zip" DLSS-main full
    mkfsr "$DL/FidelityFX-SDK-2.4.0.zip" FidelityFX-SDK-2.4.0 full
    mkxess "$DL/$XESS_ZIP" XeSS_SDK_3.0.2 full && touch -d '2026-01-01 10:00' "$DL/$XESS_ZIP"
    mkxess "$DL/XeSS_SDK_samples.zip" XeSS_SDK_samples decoy && touch -d '2026-01-05 10:00' "$DL/XeSS_SDK_samples.zip"
    run "$i" install
    t "T24 $i zip in Downloads beside a newer zip without bin: exit 0" eq "$RC" 0
    t "T24 $i zip in Downloads: Intel's three files in place" xess_ok
    t "T24 $i zip in Downloads: libxess_dx11.dll not copied" test ! -e "$GAME/$XESSD/libxess_dx11.dll"
    t "T24 $i zip in Downloads: all three recorded" eq "$(xess_recorded)" 3
    t "T24 $i zip in Downloads: no temporary file left" no_part
    t "T24 $i zip in Downloads: NVIDIA files as before" sdk_ok rel_dlss rel_dlssg
    t "T24 $i zip in Downloads: AMD files as before" fsr_ok
    t "T24 $i zip in Downloads: summary line" has "Intel XeSS files: done"
    t "T24 $i step comes after the AMD step and before the frame rate limit" eq \
        "$(step_list | sed 's/^[0-9]* of [0-9]*  //' | grep -xE 'AMD FSR files|Intel XeSS files|Frame rate limit' | tr '\n' ,)" \
        "AMD FSR files,Intel XeSS files,Frame rate limit,"
    t "T24 $i steps numbered 1 to N, N the number of steps shown" steps_numbered
    mkcase "t24n-$i"
    mkxess "$DL/$XESS_ZIP" '' full
    run "$i" install
    t "T24 $i zip without a top folder" xess_ok
    mkcase "t24f-$i"
    mkxess "$DL/XeSS_SDK_3.0.1.zip" XeSS_SDK_3.0.1 full && touch -d '2026-01-01 10:00' "$DL/XeSS_SDK_3.0.1.zip"
    mkxess "$DL/$XESS_ZIP" XeSS_SDK_3.0.2 v2 && touch -d '2026-01-03 10:00' "$DL/$XESS_ZIP"
    run "$i" install
    t "T24 $i two good zips: the newest is used" xess_ok v2
    mkcase "t24b-$i"
    mkxess "$DL" XeSS_SDK_3.0.2 full
    run "$i" install
    t "T24 $i extracted folder in Downloads" xess_ok
    mkcase "t24c-$i"
    mkdir -p "$C/elsewhere" && mkxess "$C/elsewhere/intel sdk.zip" XeSS_SDK_3.0.2 full
    run "$i" install xess="$C/elsewhere/intel sdk.zip"
    t "T24 $i explicit zip path" xess_ok
    mkcase "t24d-$i"
    mkdir -p "$C/elsewhere" && mkxess "$C/elsewhere" "my intel sdk" full
    run "$i" install xess="$C/elsewhere/my intel sdk"
    t "T24 $i explicit folder path" xess_ok
}

# ---- T25 no Intel files, bad Intel files, upscaling only, already in place ----------------------------------------------
case_t25() {
    local i=$1 sep k n before
    if [ "$i" = sh ]; then sep=/; else sep='\'; fi
    mkcase "t25-$i"
    mksdk "$DL/DLSS-main.zip" DLSS-main full
    run "$i" install
    t "T25 $i no Intel files: exit 0" eq "$RC" 0
    t "T25 $i no Intel files: mod files installed" payload_ok "$GAME"
    t "T25 $i no Intel files: NVIDIA files as before" sdk_ok rel_dlss rel_dlssg
    t "T25 $i no Intel files: AMD step as before" has "AMD FSR files: skipped"
    t "T25 $i no Intel files: download address" has "$XESS_URL"
    for n in $XESS_NAMES; do t "T25 $i no Intel files: names $n" has "$n"; done
    t "T25 $i no Intel files: destination ffxv_dlss${sep}xess" has "ffxv_dlss${sep}xess"
    t "T25 $i no Intel files: licence sentence" has "$INTEL_LICENCE"
    t "T25 $i no Intel files: says XeSS is optional" eval 'grep -i "xess" "$OUT" | grep -qi "optional"'
    t "T25 $i no Intel files: summary says skipped" has "Intel XeSS files: skipped"
    t "T25 $i no Intel files: no Intel folder made" no_xess
    t "T25 $i no Intel files: Still to do names the zip and where to get it" eval 'grep -i "still to do" "$OUT" | grep -F -- "$XESS_ZIP" | grep -qF -- "$XESS_URL"'
    for k in smallxess tinyxell nomz; do
        mkcase "t25$k-$i"
        mkxess "$DL/$XESS_ZIP" XeSS_SDK_3.0.2 "$k"
        run "$i" install
        t "T25 $i $k: exit 0" eq "$RC" 0
        t "T25 $i $k: refused and reported" hasi "not a valid DLL"
        t "T25 $i $k: nothing copied from that download" no_xess
        t "T25 $i $k: no temporary file left" no_part
    done
    for k in nofg noxell; do
        mkcase "t25$k-$i"
        mkxess "$DL/$XESS_ZIP" XeSS_SDK_3.0.2 "$k"
        run "$i" install
        t "T25 $i $k: exit 0" eq "$RC" 0
        t "T25 $i $k: libxess.dll copied" same "$T/sdkref/xess.dll" "$GAME/$XESSD/libxess.dll"
        t "T25 $i $k: no frame generation file in place" eval '[ ! -e "$GAME/$XESSD/libxess_fg.dll" ] && [ ! -e "$GAME/$XESSD/libxell.dll" ]'
        t "T25 $i $k: one file recorded" eq "$(xess_recorded)" 1
        t "T25 $i $k: says frame generation is unavailable" hasi "frame generation is unavailable"
        t "T25 $i $k: no temporary file left" no_part
    done
    mkcase "t25inplace-$i"
    mkxess "$DL/$XESS_ZIP" XeSS_SDK_3.0.2 full
    run "$i" install
    rm "$DL/$XESS_ZIP"
    before=$(snap "$GAME")
    run "$i" install
    t "T25 $i Intel files already in place: reported" has "Intel's XeSS files are already in place."
    t "T25 $i Intel files already in place: bytes unchanged" eq "$(snap "$GAME")" "$before"
    t "T25 $i Intel files already in place: exit 0" eq "$RC" 0
}

# ---- T26 uninstall takes Intel's files away -------------------------------------------------------------------------------
case_t26() {
    local i=$1 g
    mkcase "t26-$i"
    printf 'MZ another tool proxy\n' > "$GAME/dxgi.dll"
    mksdk "$DL/DLSS-main.zip" DLSS-main full
    mkxess "$DL/$XESS_ZIP" XeSS_SDK_3.0.2 full
    g=$(snap "$GAME")
    run "$i" install
    t "T26 $i installed with Intel files" xess_ok
    run "$i" uninstall
    t "T26 $i uninstall exit 0" eq "$RC" 0
    t "T26 $i Intel folder gone" no_xess
    t "T26 $i game folder as before" eq "$(snap "$GAME")" "$g"
    mkcase "t26b-$i"
    mkxess "$DL/$XESS_ZIP" XeSS_SDK_3.0.2 nofg
    g=$(snap "$GAME")
    run "$i" install
    t "T26 $i upscaling only installed" test -f "$GAME/$XESSD/libxess.dll"
    run "$i" uninstall
    t "T26 $i upscaling only, then uninstall: game folder as before" eq "$(snap "$GAME")" "$g"
}

# ---- update fixtures ---------------------------------------------------------------------------------------------------
# Where each vendor file lives in the game folder (written out here, not read from the scripts).
declare -A UDEST=([dlss]=ffxv_dlss/nvngx_dlss.dll [dlssg]=ffxv_dlss/streamline/nvngx_dlssg.dll
    [up]=ffxv_dlss/fidelityfx/amd_fidelityfx_upscaler_dx12.dll [fg]=ffxv_dlss/fidelityfx/amd_fidelityfx_framegeneration_dx12.dll
    [ld]=ffxv_dlss/fidelityfx/amd_fidelityfx_loader_dx12.dll [xs]=ffxv_dlss/xess/libxess.dll [xfg]=ffxv_dlss/xess/libxess_fg.dll
    [xl]=ffxv_dlss/xess/libxell.dll)
# Three releases of each file: v1 older, v2 the versions measured on the vendors' current files, v3 newer.
declare -A UVER=([dlss.v1]=310.4.0.0 [dlssg.v1]=310.4.0.0 [up.v1]=4.0.0.1 [fg.v1]=4.0.0.1 [ld.v1]=2.0.0.1
    [xs.v1]=2.0.1.0 [xfg.v1]=1.2.0.0 [xl.v1]=1.2.0.0
    [dlss.v2]=310.9.1.0 [dlssg.v2]=310.9.1.0 [up.v2]=4.1.2.4555 [fg.v2]=4.0.2.4555 [ld.v2]=2.3.0.4555
    [xs.v2]=2.0.2.68 [xfg.v2]=1.3.1.78 [xl.v2]=1.3.2.10
    [dlss.v3]=310.10.0.0 [dlssg.v3]=310.10.0.0 [up.v3]=4.2.0.1 [fg.v3]=4.1.0.1 [ld.v3]=2.4.0.1
    [xs.v3]=2.1.0.1 [xfg.v3]=1.4.0.1 [xl.v3]=1.4.0.1)
NV='dlss dlssg' AMD='up fg ld' INTEL='xs xfg xl' ALL='dlss dlssg up fg ld xs xfg xl'
uname() { printf '%s' "${UDEST[$1]##*/}"; }

# mkpe OUT NAME VERSION [key=value...]: a small 64-bit DLL with a real header, one section holding a version resource
# with the vendor's strings (as measured on their files), zero padding past the minimum size, and a signature block
# naming the vendor. Keys: machine, chars, magic, company, product, desc, orig, org ("none": no signature block),
# nover=1 (no version resource), cut=1 (ends before its signature block), text=1 (a text file), tag (extra bytes),
# certpad=N (N zero bytes added to the signature block after the vendor's name).
mkpe() {
    python3 -I - "$@" <<'PY'
import sys, struct
out, name, version = sys.argv[1:4]
o = dict(a.split('=', 1) for a in sys.argv[4:])
known = {
    'nvngx_dlss.dll': ('NVIDIA', 'NVIDIA Deep Learning SuperSampling', 'NVIDIA DLSS - DVS PRODUCTION', 'CL 38868064', 'NVIDIA Corporation'),
    'nvngx_dlssg.dll': ('NVIDIA', 'NVIDIA DLSS-G MFGLW', 'NVIDIA DLSS-G MFGLW - DVS PRODUCTION', 'CL 38868064', 'NVIDIA Corporation'),
    'nvngx_dlssd.dll': ('NVIDIA', 'NVIDIA DLSS Ray Reconstruction', 'NVIDIA DLSS-D - DVS PRODUCTION', 'CL 38868064', 'NVIDIA Corporation'),
    'amd_fidelityfx_loader_dx12.dll': ('Advanced Micro Devices, Inc.', 'AMD FidelityFX', 'AMD FidelityFX (DirectX) Loader Library', '', 'Advanced Micro Devices, Inc.'),
    'amd_fidelityfx_upscaler_dx12.dll': ('Advanced Micro Devices, Inc.', 'AMD FidelityFX Upscaler', 'AMD FidelityFX (DirectX) Upscaler Library', '', 'Advanced Micro Devices, Inc.'),
    'amd_fidelityfx_framegeneration_dx12.dll': ('Advanced Micro Devices, Inc.', 'AMD FidelityFX Frame Generation', 'AMD FidelityFX (DirectX) Frame Generation Library', '', 'Advanced Micro Devices, Inc.'),
    'libxess.dll': ('Intel Corporation', 'XeSS SDK', 'XeSS SDK', 'libxess.dll', 'Intel Corporation'),
    'libxess_dx11.dll': ('Intel Corporation', 'XeSS SDK', 'XeSS SDK', 'libxess_dx11.dll', 'Intel Corporation'),
    'libxess_fg.dll': ('Intel Corporation', 'XeSS Frame Generation SDK', 'XeSS Frame Generation SDK', 'libxess_fg.dll', 'Intel Corporation'),
    'libxell.dll': ('Intel Corporation', 'XeLL SDK', 'XeLL SDK', 'libxell.dll', 'Intel Corporation'),
    'dxgi.dll': ('crosire', 'ReShade', "crosire's ReShade post-processing injector for 64-bit", 'ReShade64.dll', 'none'),
}
company, product, desc, orig, org = known[name]
company, product = o.get('company', company), o.get('product', product)
desc, orig, org = o.get('desc', desc), o.get('orig', orig), o.get('org', org)
minimum = {'amd_fidelityfx_loader_dx12.dll': 16384, 'libxell.dll': 131072, 'dxgi.dll': 65536}.get(name, 1048576)
tag = ('%s %s %s ' % (name, version, o.get('tag', ''))).encode()
if o.get('text'):
    open(out, 'wb').write((b'This page could not be found. ' + tag) * (minimum // 30 + 100))
    sys.exit(0)
def pad4(b): return b + b'\0' * (-len(b) % 4)
def node(key, value, children, wtype, vlen):
    head = pad4(struct.pack('<HHH', 0, vlen, wtype) + (key + '\0').encode('utf-16-le'))
    body = pad4(head + value) + b''.join(pad4(c) for c in children)
    return struct.pack('<H', len(body)) + body[2:]
def string(k, v): return node(k, (v + '\0').encode('utf-16-le'), [], 1, len(v) + 1)
a, b, c, d = (int(x) for x in version.split('.'))
ms, ls = (a << 16) | b, (c << 16) | d
fixed = struct.pack('<13I', 0xFEEF04BD, 0x10000, ms, ls, ms, ls, 0x3F, 0, 0x40004, 2, 0, 0, 0)
strings = [string(k, v) for k, v in [('CompanyName', company), ('FileDescription', desc), ('FileVersion', version),
                                       ('OriginalFilename', orig), ('ProductName', product), ('ProductVersion', version)]]
ver = node('VS_VERSION_INFO', fixed, [node('StringFileInfo', b'', [node('040904b0', b'', strings, 1, 0)], 1, 0)], 0, len(fixed))
rva = 0x1000
rsrc = bytearray()
rsrc += struct.pack('<IIHHHH', 0, 0, 0, 0, 0, 1) + struct.pack('<II', 16, 0x80000000 | 0x18)
rsrc += struct.pack('<IIHHHH', 0, 0, 0, 0, 0, 1) + struct.pack('<II', 1, 0x80000000 | 0x30)
rsrc += struct.pack('<IIHHHH', 0, 0, 0, 0, 0, 1) + struct.pack('<II', 0x409, 0x48)
rsrc += struct.pack('<IIII', rva + 0x58, len(ver), 0, 0) + ver
rsrc += b'\0' * (-len(rsrc) % 512)
if o.get('nover'): rsrc = bytearray(512)
machine, chars, magic = (int(o.get(k, v), 0) for k, v in (('machine', '0x8664'), ('chars', '0x2022'), ('magic', '0x20b')))
filler = (tag * (minimum // len(tag) + 2))[:minimum + 4096 - 0x200 - len(rsrc)]
end = 0x200 + len(rsrc) + len(filler)
end += -end % 8
blob = b'\x30\x82\x01\x00' + tag + b'\x31\x0b\x30\x09\x06\x03\x55\x04\x06\x13\x02US'
if org != 'none': blob += b'\x31\x1a\x30\x18\x06\x03\x55\x04\x0a\x0c' + bytes([len(org)]) + org.encode()
blob += b'\0' * int(o.get('certpad', '0'))
blob += b'\0' * (-len(blob) % 8)
cert = struct.pack('<IHH', 8 + len(blob), 0x200, 2) + blob
f = bytearray(0x200)
f[0:2] = b'MZ'
struct.pack_into('<I', f, 0x3C, 0x80)
f[0x80:0x84] = b'PE\0\0'
struct.pack_into('<HHIIIHH', f, 0x84, machine, 1, 0, 0, 0, 240, chars)
struct.pack_into('<H', f, 0x98, magic)
struct.pack_into('<I', f, 0x98 + 108, 16)
struct.pack_into('<II', f, 0x98 + 128, *((0, 0) if o.get('nover') else (rva, len(rsrc))))
struct.pack_into('<II', f, 0x98 + 144, *((0, 0) if org == 'none' else (end, len(cert))))
f[0x188:0x190] = b'.rsrc\0\0\0'
struct.pack_into('<IIII', f, 0x190, len(rsrc), rva, len(rsrc), 0x200)
data = bytes(f) + bytes(rsrc) + filler
data += b'\0' * (end - len(data))
if org != 'none': data += cert
if o.get('cut'): data = data[:end - 16]
open(out, 'wb').write(data)
PY
}
# fx SHORT SET [key=value...]: the path of that fixture, made on first use. Extra keys make a variant (its own file).
fx() {
    local s=$1 v=$2 id
    shift 2
    id=$s-$v${*:+-$(printf '%s' "$*" | tr -c 'A-Za-z0-9.=' '_')}
    [ -f "$T/pe/$id.dll" ] || { mkdir -p "$T/pe" && mkpe "$T/pe/$id.dll" "$(uname "$s")" "${UVER[$s.$v]:-$v}" "$@"; } >&2
    printf '%s\n' "$T/pe/$id.dll"
}
# zipup ZIP ENTRY=FILE...: a zip holding those files under those names
zipup() {
    python3 -I - "$@" <<'PY'
import sys, zipfile
with zipfile.ZipFile(sys.argv[1], 'w') as z:
    for a in sys.argv[2:]:
        n, f = a.split('=', 1)
        z.write(f, n)
PY
}
# zipset ZIP PREFIX SET SHORT...: a zip with those files of that set under PREFIX/
zipset() {
    local z=$1 p=$2 v=$3 s a=()
    shift 3
    for s in "$@"; do a+=("$p/$(uname "$s")=$(fx "$s" "$v")"); done
    zipup "$z" "${a[@]}"
}
# loose DIR SET SHORT...: those files of that set as loose files in DIR
loose() {
    local d=$1 v=$2 s
    shift 2
    mkdir -p "$d"
    for s in "$@"; do cp "$(fx "$s" "$v")" "$d/$(uname "$s")"; done
}
at() { same "$(fx "$2" "$3")" "$GAME/${UDEST[$1]}"; }   # at SHORT SHORT SET: that file is in place (bytes)
all_at() { local v=$1 s; shift; for s in "$@"; do at "$s" "$s" "$v" || return 1; done; }
upd_base() { # upd_base NAME [SETS]: a game with the mod installed from the vendors' SDK layouts holding v1 files
    mkcase "$1"
    local w=${2:-nv amd intel}
    case " $w " in *' nv '*) zipset "$DL/DLSS-main.zip" DLSS-main/lib/Windows_x86_64/rel v1 $NV ;; esac
    case " $w " in *' amd '*) zipset "$DL/FidelityFX-SDK-2.4.0.zip" FidelityFX-SDK-2.4.0/Kits/FidelityFX/signedbin v1 $AMD ;; esac
    case " $w " in *' intel '*) zipset "$DL/$XESS_ZIP" XeSS_SDK_3.0.2/bin v1 $INTEL ;; esac
    run "$UI" install
    rm -f "$DL"/*.zip
}
upd_line() { has "$(uname "$1"): updated from ${UVER[$1.$2]} to ${UVER[$1.$3]}."; }   # upd_line SHORT FROM TO

# The PowerShell header reader and signature check, called directly on fixtures (tests/unit.ps1 -Case pe / signer).
unit_pe() {
    local U=$T/unit-pe s c
    rm -rf "$U" && mkdir -p "$U"
    for s in $ALL; do cp "$(fx "$s" v2)" "$U/$s.dll"; done
    cp "$(fx dlss 310.99.0.0 machine=0xaa64)" "$U/arm64.dll"
    cp "$(fx dlss 310.99.0.0 machine=0x14c magic=0x10b)" "$U/32bit.dll"
    cp "$(fx dlss 310.99.0.0 chars=0x0022)" "$U/exe.dll"
    cp "$(fx dlss 310.99.0.0 text=1)" "$U/text.dll"
    cp "$(fx dlss 310.99.0.0 nover=1)" "$U/nover.dll"
    cp "$(fx dlss 310.99.0.0 cut=1)" "$U/cut.dll"
    for c in pe signer; do
        OUT=$T/unit-$c.txt
        "$PWSH" -NoProfile -File "$R/tests/unit.ps1" -Case "$c" -Script "$R/scripts/setup.ps1" -Fixtures "$U" > "$OUT" 2>&1
        t "T29 ps unit $c: every check passes" eq "$?" 0
        t "T29 ps unit $c: checks ran" has "ok   "
    done
}
recorded_once() { [ "$(grep -cxF -- "${UDEST[$1]}" "$GAME/ffxv_dlss/installed_files.txt")" = 1 ]; }
no_upd_part() { [ -z "$(find "$GAME" -name '*ffxv_dlss_part*' -print -quit)" ]; }
# snap without the install record, which keeps naming what update may leave behind after an undo
snap_files() { snap "$1" | grep -v ' ffxv_dlss/installed_files.txt '; }
record_gained_only_update_names() { # record_gained_only_update_names OLD_RECORD_COPY: lines added since are kept copies or the list
    local l
    while IFS= read -r l; do
        case $l in ffxv_dlss/last_update.txt | *.before_update) ;; *) grep -qxF -- "$l" "$1" || return 1 ;; esac
    done < "$GAME/ffxv_dlss/installed_files.txt"
}

# ---- T27 update puts in newer files, loose or zipped -----------------------------------------------------------------
case_t27() {
    local i=$1 s
    UI=$i
    upd_base "t27a-$i"
    t "T27 $i base: v1 installed" all_at v1 $ALL
    loose "$DL" v2 $NV $AMD
    zipset "$DL/XeSS_SDK_3.1.0.zip" XeSS_SDK_3.1.0/deeper/bin v2 $INTEL
    run "$i" update
    t "T27 $i loose and nested zip: exit 0" eq "$RC" 0
    for s in $ALL; do
        t "T27 $i loose and nested zip: $(uname "$s") replaced" at "$s" "$s" v2
        t "T27 $i loose and nested zip: $(uname "$s") old and new version named" upd_line "$s" v1 v2
        t "T27 $i loose and nested zip: $(uname "$s") recorded once" recorded_once "$s"
    done
    t "T27 $i no part files" no_upd_part
    upd_base "t27b-$i"
    zipset "$DL/DLSS-310.9.1.zip" DLSS/lib/Windows_x86_64/rel v2 $NV
    zipset "$DL/FidelityFX-SDK-2.5.zip" FidelityFX-SDK/Kits/FidelityFX/signedbin v2 $AMD
    loose "$DL" v2 $INTEL
    run "$i" update
    t "T27 $i nested zip and loose: exit 0" eq "$RC" 0
    for s in $ALL; do
        t "T27 $i nested zip and loose: $(uname "$s") replaced" at "$s" "$s" v2
        t "T27 $i nested zip and loose: $(uname "$s") old and new version named" upd_line "$s" v1 v2
    done
}

# ---- T28 the highest version wins ---------------------------------------------------------------------------------------
case_t28() {
    local i=$1
    UI=$i
    upd_base "t28a-$i" nv
    zipup "$DL/a.zip" "a/nvngx_dlss.dll=$(fx dlss v2)"
    zipup "$DL/b.zip" "b/nvngx_dlss.dll=$(fx dlss v3)"
    cp "$(fx dlss 310.5.0.0)" "$DL/nvngx_dlss.dll"
    run "$i" update
    t "T28 $i 310.10.0.0 beats 310.9.1.0 and 310.5.0.0" at dlss dlss v3
    t "T28 $i says 310.10.0.0" upd_line dlss v1 v3
    upd_base "t28b-$i" nv
    cp "$(fx dlss v2)" "$DL/nvngx_dlss.dll"
    cp "$(fx dlss v3)" "$DL/nvngx_dlss (1).dll"
    cp "$(fx dlssg v2)" "$DL/nvngx_dlssg (2).dll"
    run "$i" update
    t "T28 $i nvngx_dlss (1).dll is seen and wins" at dlss dlss v3
    t "T28 $i nvngx_dlssg (2).dll alone is seen" at dlssg dlssg v2
    upd_base "t28c-$i" amd
    zipset "$DL/FidelityFX-SDK-a.zip" a/Kits/FidelityFX/signedbin v3 $AMD
    zipset "$DL/FidelityFX-SDK-b.zip" b/Kits/FidelityFX/signedbin v2 $AMD
    run "$i" update
    t "T28 $i the newer AMD set wins" all_at v3 $AMD
}

# ---- T29 the wrong kind of file is refused ---------------------------------------------------------------------------------
# t29_one I TAG SHORT REASON FIXTURE: the bad file alone changes nothing; beside a good, lower one, the good one goes in
t29_one() {
    local i=$1 tag=$2 s=$3 why=$4 bad=$5 before name
    name=$(uname "$s")
    upd_base "t29$tag-$i" nv
    cp "$bad" "$DL/$name"
    before=$(snap "$GAME")
    run "$i" update
    t "T29 $i $tag: exit 0" eq "$RC" 0
    t "T29 $i $tag: refused with the reason" has "Refused $name from $DL/$name: $why."
    t "T29 $i $tag: game folder unchanged" eq "$(snap "$GAME")" "$before"
    t "T29 $i $tag: no part files" no_upd_part
    zipup "$DL/good.zip" "x/$name=$(fx "$s" v2)"
    run "$i" update
    t "T29 $i $tag control: the good, lower file beside it goes in" at "$s" "$s" v2
    t "T29 $i $tag control: the bad one still refused" has "Refused $name from $DL/$name: $why."
}
# t29_set I TAG VENDOR_SET... : the same for a set whose BAD_SHORT is wrong
t29_set() {
    local i=$1 tag=$2 bs=$3 why=$4 bad=$5 label=$6 w=$7 set before s a=()
    shift 7
    set=$*
    upd_base "t29$tag-$i" "$w"
    for s in $set; do
        if [ "$s" = "$bs" ]; then a+=("bad/$(uname "$s")=$bad"); else a+=("bad/$(uname "$s")=$(fx "$s" v3)"); fi
    done
    zipup "$DL/bad.zip" "${a[@]}"
    before=$(snap "$GAME")
    run "$i" update
    t "T29 $i $tag: exit 0" eq "$RC" 0
    t "T29 $i $tag: refused with the reason" has "Refused $(uname "$bs") from $DL/bad.zip: $why."
    t "T29 $i $tag: the set is not used" has "$label: nothing was copied from $DL/bad.zip."
    t "T29 $i $tag: game folder unchanged" eq "$(snap "$GAME")" "$before"
    zipset "$DL/good.zip" good v2 $set
    run "$i" update
    t "T29 $i $tag control: the good, lower set beside it goes in" all_at v2 $set
}
case_t29() {
    local i=$1
    UI=$i
    t29_one "$i" 32bit dlss 'it is not a 64-bit Windows DLL' "$(fx dlss 310.99.0.0 machine=0x14c magic=0x10b)"
    t29_one "$i" arm64 dlss 'it is not a 64-bit Windows DLL' "$(fx dlss 310.99.0.0 machine=0xaa64)"
    t29_one "$i" exe dlss 'it is not a 64-bit Windows DLL' "$(fx dlss 310.99.0.0 chars=0x22)"
    t29_one "$i" text dlss 'it is not a 64-bit Windows DLL' "$(fx dlss 310.99.0.0 text=1)"
    t29_one "$i" cut dlss 'it is cut short' "$(fx dlss 310.99.0.0 cut=1)"
    t29_one "$i" nover dlss 'it has no version information' "$(fx dlss 310.99.0.0 nover=1)"
    t29_one "$i" company dlss "it is not NVIDIA's file" "$(fx dlss 310.99.0.0 company='Advanced Micro Devices, Inc.')"
    t29_one "$i" rr dlss 'it is a different NVIDIA file under this name' "$(fx dlss 310.99.0.0 product='NVIDIA DLSS Ray Reconstruction')"
    t29_one "$i" dlssasg dlssg 'it is a different NVIDIA file under this name' "$(fx dlssg 310.99.0.0 product='NVIDIA Deep Learning SuperSampling')"
    t29_one "$i" nosig dlss 'it has no digital signature' "$(fx dlss 310.99.0.0 org=none)"
    t29_one "$i" othersig dlss 'its digital signature does not name NVIDIA' "$(fx dlss 310.99.0.0 org='Example Signing Ltd')"
    t29_set "$i" dx11 xs 'it is a different Intel file under this name' "$(fx xs v3 orig=libxess_dx11.dll)" 'Intel XeSS' intel $INTEL
    t29_set "$i" upasld ld 'it is a different AMD file under this name' "$(fx ld v3 desc='AMD FidelityFX (DirectX) Upscaler Library')" 'AMD FSR' amd $AMD
}

# ---- T30 development, debug and ARM copies are never chosen ------------------------------------------------------------------
case_t30() {
    local i=$1 before
    UI=$i
    upd_base "t30a-$i" nv
    zipup "$DL/DLSS-310.10.zip" "DLSS/lib/Windows_x86_64/rel/nvngx_dlss.dll=$(fx dlss v2)" \
        "DLSS/lib/Windows_x86_64/dev/nvngx_dlss.dll=$(fx dlss v3)" "DLSS/lib/Windows_arm64ec/rel/nvngx_dlss.dll=$(fx dlss 310.11.0.0)" \
        "DLSS/lib/Windows_aarch64/rel/nvngx_dlss.dll=$(fx dlss 310.12.0.0)" "DLSS/lib/Debug/nvngx_dlss.dll=$(fx dlss 310.13.0.0)"
    zipup "$DL/streamline.zip" "sl/bin/x64/nvngx_dlssg.dll=$(fx dlssg v2)" "sl/bin/x64/development/nvngx_dlssg.dll=$(fx dlssg v3)"
    run "$i" update
    t "T30 $i DLSS SDK layout: the release copy goes in" at dlss dlss v2
    t "T30 $i Streamline layout: the release copy goes in" at dlssg dlssg v2
    upd_base "t30b-$i" nv
    zipup "$DL/devonly.zip" "DLSS/lib/Windows_x86_64/dev/nvngx_dlss.dll=$(fx dlss v3)" "sl/bin/x64/development/nvngx_dlssg.dll=$(fx dlssg v3)"
    before=$(snap "$GAME")
    run "$i" update
    t "T30 $i only development copies: nothing changes" eq "$(snap "$GAME")" "$before"
    t "T30 $i only development copies: says why" has "Refused nvngx_dlss.dll from $DL/devonly.zip: it comes from a development or ARM folder."
    upd_base "t30c-$i" nv
    mkdir -p "$C/sdk/lib/Windows_x86_64/rel" "$C/sdk/lib/Windows_x86_64/dev" "$C/sdk/lib/Windows_arm64ec/rel"
    cp "$(fx dlss v2)" "$C/sdk/lib/Windows_x86_64/rel/nvngx_dlss.dll"
    cp "$(fx dlss v3)" "$C/sdk/lib/Windows_x86_64/dev/nvngx_dlss.dll"
    cp "$(fx dlss 310.11.0.0)" "$C/sdk/lib/Windows_arm64ec/rel/nvngx_dlss.dll"
    run "$i" update from="$C/sdk"
    t "T30 $i folder given with --from: the release copy goes in" at dlss dlss v2
}

# ---- T31 AMD's and Intel's three files go in together or not at all ------------------------------------------------------------
case_t31() {
    local i=$1 w label set before
    UI=$i
    for w in amd intel; do
        if [ $w = amd ]; then label='AMD FSR' set=$AMD; else label='Intel XeSS' set=$INTEL; fi
        upd_base "t31a-$w-$i"
        zipset "$DL/two.zip" two v2 ${set% *}
        loose "$DL" v2 dlss
        run "$i" update
        t "T31 $i $w two of three: the set in place is unchanged" all_at v1 $set
        t "T31 $i $w two of three: says why" has "$label: $DL/two.zip has only 2 of the 3 files, so nothing was copied from it."
        t "T31 $i $w two of three: NVIDIA still updated in the same run" at dlss dlss v2
        upd_base "t31b-$w-$i"
        zipup "$DL/one.zip" "one/$(uname "${set%% *}")=$(fx "${set%% *}" v2)"
        run "$i" update
        t "T31 $i $w one of three: the set in place is unchanged" all_at v1 $set
        t "T31 $i $w one of three: says why" has "$label: $DL/one.zip has only 1 of the 3 files, so nothing was copied from it."
        upd_base "t31c-$w-$i"
        zipset "$DL/badone.zip" bad v2 ${set% *}
        python3 -I -c 'import sys,zipfile; zipfile.ZipFile(sys.argv[1],"a").write(sys.argv[2], sys.argv[3])' "$DL/badone.zip" \
            "$(fx "${set##* }" v2 org=none)" "bad/$(uname "${set##* }")"
        loose "$DL" v2 dlssg
        run "$i" update
        t "T31 $i $w three with one refused: the set in place is unchanged" all_at v1 $set
        t "T31 $i $w three with one refused: says why" has "Refused $(uname "${set##* }") from $DL/badone.zip: it has no digital signature."
        t "T31 $i $w three with one refused: NVIDIA still updated in the same run" at dlssg dlssg v2
        upd_base "t31d-$w-$i"
        zipset "$DL/good.zip" good v2 $set
        run "$i" update
        t "T31 $i $w three good: all three replaced" all_at v2 $set
    done
}

# ---- T32 never older by itself; up to date changes nothing -------------------------------------------------------------------------
case_t32() {
    local i=$1 before
    UI=$i
    upd_base "t32a-$i"
    loose "$DL" v2 $ALL
    run "$i" update
    rm -f "$DL"/*.dll
    loose "$DL" v1 dlss
    zipset "$DL/old-fsr.zip" old v1 $AMD
    before=$(snap "$GAME")
    run "$i" update
    t "T32 $i older file: exit 0" eq "$RC" 0
    t "T32 $i older file: game folder unchanged (kept copies and list too)" eq "$(snap "$GAME")" "$before"
    t "T32 $i older file: both versions named" has "Left nvngx_dlss.dll alone: the copy in place (310.9.1.0) is newer than the download (310.4.0.0)."
    t "T32 $i older AMD set: both versions named" has "Left AMD FSR alone: amd_fidelityfx_upscaler_dx12.dll in place (4.1.2.4555) is newer than the download's (4.0.0.1)."
    run "$i" update allowolder
    t "T32 $i allow older: exit 0" eq "$RC" 0
    t "T32 $i allow older: the older nvngx_dlss.dll goes in" at dlss dlss v1
    t "T32 $i allow older: the older AMD set goes in" all_at v1 $AMD
    t "T32 $i allow older: Intel untouched" all_at v2 $INTEL
    upd_base "t32b-$i"
    loose "$DL" v2 $ALL
    run "$i" update
    before=$(snap "$GAME")
    run "$i" update
    t "T32 $i equal: exit 0" eq "$RC" 0
    t "T32 $i equal: game folder unchanged, kept copies and list included" eq "$(snap "$GAME")" "$before"
    t "T32 $i equal: kept copies are still there" test -f "$GAME/${UDEST[dlss]}.before_update"
    t "T32 $i equal: up to date message for nvngx_dlss.dll" has "nvngx_dlss.dll: 310.9.1.0 is in place and nothing newer was found."
    t "T32 $i equal: up to date message for libxell.dll" has "libxell.dll: 1.3.2.10 is in place and nothing newer was found."
    t "T32 $i equal: no undo line" lacks "undo-update.sh on Linux"
}

# ---- T33 undo puts back the previous files, one step only ----------------------------------------------------------------------------
case_t33() {
    local i=$1 b0 b1 g
    UI=$i
    upd_base "t33a-$i"
    b0=$(snap_files "$GAME")
    cp "$GAME/ffxv_dlss/installed_files.txt" "$C/record-before"
    loose "$DL" v2 $ALL
    run "$i" update
    t "T33 $i update: kept copies made" eval '[ -f "$GAME/${UDEST[up]}.before_update" ] && [ -f "$GAME/ffxv_dlss/last_update.txt" ]'
    t "T33 $i update: says how to undo" has "The previous files are kept. To go back to them, run undo-update.sh on Linux or undo-update.bat on Windows."
    run "$i" undo
    t "T33 $i undo: exit 0" eq "$RC" 0
    t "T33 $i undo: game folder exactly as before the update (the record aside)" eq "$(snap_files "$GAME")" "$b0"
    t "T33 $i undo: the record gained only the kept copies' names and the list" record_gained_only_update_names "$C/record-before"
    t "T33 $i undo: reports what it did" has "Put back the previous nvngx_dlss.dll (310.4.0.0)."
    run "$i" undo
    t "T33 $i second undo: exit 0" eq "$RC" 0
    t "T33 $i second undo: nothing to undo" has "Nothing to undo."
    t "T33 $i second undo: nothing changed" eq "$(snap_files "$GAME")" "$b0"
    upd_base "t33b-$i"
    loose "$DL" v2 $ALL
    run "$i" update
    b1=$(snap "$GAME")
    rm -f "$DL"/*.dll
    loose "$DL" v3 dlss $AMD
    run "$i" update
    t "T33 $i second update: only the files it changed are kept" eval '[ -f "$GAME/${UDEST[dlss]}.before_update" ] && [ ! -e "$GAME/${UDEST[dlssg]}.before_update" ] && [ ! -e "$GAME/${UDEST[xs]}.before_update" ]'
    run "$i" undo
    t "T33 $i two updates then undo: back to the first update's files" eq "$(snap "$GAME" | grep -v -e before_update -e last_update.txt)" "$(printf '%s\n' "$b1" | grep -v -e before_update -e last_update.txt)"
    t "T33 $i two updates then undo: no kept copies or list left" eval '[ -z "$(find "$GAME" -name "*.before_update" -o -name last_update.txt)" ]'
    upd_base "t33c-$i" nv
    b0=$(snap_files "$GAME")
    loose "$DL" v2 $INTEL
    run "$i" update
    t "T33 $i a set the update added: in place" all_at v2 $INTEL
    t "T33 $i a set the update added: said so" has "libxess.dll: added, version 2.0.2.68."
    run "$i" undo
    t "T33 $i a set the update added: undo removes it" eq "$(snap_files "$GAME")" "$b0"
    t "T33 $i a set the update added: undo says so" has "Removed libxess.dll, which the update added."
    # a list naming files outside the eight is not followed
    upd_base "t33d-$i" nv
    printf 'outside\n' > "$LIB/steamapps/common/x"; printf 'kept outside\n' > "$LIB/steamapps/common/x.before_update"
    printf 'MZ ffxv-dlss mod\n' > "$GAME/dxgi.dll.before_update"
    printf 'abs\n' > "$C/abs.dll"; printf 'abs kept\n' > "$C/abs.dll.before_update"
    printf 'y\n' > "$LIB/steamapps/common/y"
    printf 'replaced ../x\nreplaced dxgi.dll\nreplaced %s\nadded ../y\nremoved ffxv_dlss/nvngx_dlss.dll\n' "$C/abs.dll" > "$GAME/ffxv_dlss/last_update.txt"
    g=$(snap "$C" | grep -v last_update.txt)
    run "$i" undo
    t "T33 $i bad list: exit 0" eq "$RC" 0
    t "T33 $i bad list: nothing outside the eight touched" eq "$(snap "$C" | grep -v last_update.txt)" "$g"
    t "T33 $i bad list: ../x skipped and reported" has 'Skipped "replaced ../x"'
    t "T33 $i bad list: dxgi.dll skipped and reported" has 'Skipped "replaced dxgi.dll"'
    t "T33 $i bad list: absolute path skipped and reported" has "Skipped \"replaced $C/abs.dll\""
    t "T33 $i bad list: ../y skipped and reported" has 'Skipped "added ../y"'
}

# ---- T34 uninstall after any mix of updates and undos leaves the game folder as before install ----------------------------------------
case_t34() {
    local i=$1 g d di seq k ok
    UI=$i
    for seq in 'update' 'update update2 undo' 'update install update2' 'update undo' 'update update2 undo undo'; do
        mkcase "t34-$i-${seq// /-}"
        printf 'MZ another tool proxy\n' > "$GAME/dxgi.dll"
        g=$(snap "$GAME"); d=$(snap "$DOCS")
        zipset "$DL/DLSS-main.zip" DLSS-main/lib/Windows_x86_64/rel v1 $NV
        zipset "$DL/FidelityFX-SDK-2.4.0.zip" FidelityFX-SDK-2.4.0/Kits/FidelityFX/signedbin v1 $AMD
        run "$i" install
        di=$(snap "$DOCS")
        ok=1
        for k in $seq; do
            case $k in
                update) loose "$DL" v2 $ALL; run "$i" update ;;
                update2) loose "$DL/new" v3 dlss $INTEL; run "$i" update "from=$DL/new" ;;
                *) run "$i" "$k" ;;
            esac
            [ "$RC" = 0 ] || ok=0
            [ "$(snap "$DOCS")" = "$di" ] || ok=0
        done
        t "T34 $i install, $seq: every run exit 0, Documents untouched by update and undo" eq "$ok" 1
        run "$i" uninstall
        t "T34 $i install, $seq, uninstall: exit 0" eq "$RC" 0
        t "T34 $i install, $seq, uninstall: game folder as before install" eq "$(snap "$GAME")" "$g"
        t "T34 $i install, $seq, uninstall: Documents as before install" eq "$(snap "$DOCS")" "$d"
    done
}

# ---- T35 no record, no game ------------------------------------------------------------------------------------------------------------
case_t35() {
    local i=$1 before a
    UI=$i
    for a in update undo; do
        mkcase "t35-$a-$i"
        loose "$DL" v2 $ALL
        before=$(snap "$C")
        run "$i" "$a"
        t "T35 $i $a without install: exit 5" eq "$RC" 5
        t "T35 $i $a without install: points at install" has "Run install first: no install record was found in this game folder."
        t "T35 $i $a without install: nothing changed" eq "$(snap "$C")" "$before"
        rm -rf "$STEAM/steamapps"
        before=$(snap "$C")
        run "$i" "$a" noprompt
        t "T35 $i $a without a game folder: exit 3" eq "$RC" 3
        t "T35 $i $a without a game folder: nothing changed" eq "$(snap "$C")" "$before"
    done
}

# ---- T36 the disk fills while update copies; a rename that fails puts the set back ------------------------------------------------------
case_t36() {
    local i=$1 inner=$T/t36-inner.sh used size
    UI=$i
    cat > "$inner" <<'EOF'
# inner IMPL REL SRC GAME AFTER STEAM DOCS DL PWSH HOME: update on a small tmpfs, copy the result out, then undo
impl=$1 rel=$2 src=$3 game=$4 after=$5 steam=$6 docs=$7 dl=$8 pwsh=$9 home=${10}
cp -a "$src/." "$game/" || exit 99
go() {
    if [ "$impl" = sh ]; then HOME=$home bash "$rel/$1.sh" --game-dir "$game" < /dev/null
    else "$pwsh" -NoProfile -File "$rel/scripts/setup.ps1" -Action "${1%-update}" -GameDir "$game" -SteamRoot "$steam" -DocumentsDir "$docs" -DownloadsDir "$dl" < /dev/null; fi
}
go update; echo "rc-update $?"
cp -a "$game/." "$after.update/"
go undo-update; echo "rc-undo $?"
cp -a "$game/." "$after/"
EOF
    upd_base "t36-$i"
    cp -a "$GAME" "$C/game-v1"   # what undo of the first update gives
    zipset "$DL/first.zip" first v2 $ALL
    run "$i" update
    rm -f "$DL"/*.zip
    loose "$DL" v3 $ALL
    used=$(du -sB4096 "$GAME" | cut -f1)
    size=$(( (used + $(du -cB4096 "$DL"/*.dll | tail -n 1 | cut -f1) + 64) * 4096 ))
    cp -a "$GAME" "$C/game-before" && mv "$GAME" "$C/game-src" && mkdir "$GAME" "$C/after" "$C/after.update"
    N=$((N + 1)); OUT=$C/out.$N.txt
    (cd "$C" && bwrap --ro-bind / / --dev /dev --proc /proc --bind "$T" "$T" --size "$size" --tmpfs "$GAME" \
        bash "$inner" "$i" "$REL" "$C/game-src" "$GAME" "$C/after" "$STEAM" "$DOCS" "$DL" "$PWSH" "$H" > "$OUT" 2>&1)
    t "T36 $i disk full: update exit 4" has "rc-update 4"
    t "T36 $i disk full: game folder as before the run" eq "$(snap "$C/after.update")" "$(snap "$C/game-before")"
    t "T36 $i disk full: no part files" eval '[ -z "$(find "$C/after.update" -name "*ffxv_dlss_part*" -print -quit)" ]'
    t "T36 $i disk full: the earlier update's undo still works" has "rc-undo 0"
    t "T36 $i disk full: undo gives back the files from before the earlier update" eq "$(snap_files "$C/after")" "$(snap_files "$C/game-v1")"
    # A file that cannot be renamed into place (a folder holds its name): AMD's set is put back, the rest still lands.
    upd_base "t36b-$i"
    rm -f "$GAME/${UDEST[fg]}"
    mkdir -p "$GAME/${UDEST[fg]}/blocker"
    loose "$DL" v2 $ALL
    run "$i" update
    t "T36 $i a rename fails: exit 4" eq "$RC" 4
    t "T36 $i a rename fails: AMD's files put back" eval 'at up up v1 && at ld ld v1'
    t "T36 $i a rename fails: the other vendors still updated" all_at v2 $NV $INTEL
    t "T36 $i a rename fails: no part files" no_upd_part
    run "$i" undo
    t "T36 $i a rename fails: undo after it" all_at v1 $NV $INTEL
    t "T36 $i a rename fails: AMD's files still the originals after undo" eval 'at up up v1 && at ld ld v1'
}

# ---- T40 failures part way: the undo stays whole, a retry works, nothing claims success --------------------------------------------
# t40_small I [dl [EXTRA]]: update with the game folder on a tmpfs that holds the game, the downloads' size if "dl" is
# given, and EXTRA (default 64) blocks of 4 KiB more; $C/game-before is the folder before, $C/after.update after, $OUT the output.
t40_small() {
    local i=$1 inner=$T/t40-inner.sh used size
    cat > "$inner" <<'EOF'
impl=$1 rel=$2 src=$3 game=$4 after=$5 steam=$6 docs=$7 dl=$8 pwsh=$9 home=${10}
cp -a "$src/." "$game/" || exit 99
if [ "$impl" = sh ]; then HOME=$home bash "$rel/update.sh" --game-dir "$game" < /dev/null
else "$pwsh" -NoProfile -File "$rel/scripts/setup.ps1" -Action update -GameDir "$game" -SteamRoot "$steam" -DocumentsDir "$docs" -DownloadsDir "$dl" < /dev/null; fi
echo "rc-update $?"
cp -a "$game/." "$after/"
EOF
    used=$(du -sB4096 "$GAME" | cut -f1)
    size=$((used + ${3:-64}))
    [ "${2:-}" = dl ] && size=$((size + $(du -cB4096 "$DL"/*.dll | tail -n 1 | cut -f1)))
    size=$((size * 4096))
    cp -a "$GAME" "$C/game-before" && mv "$GAME" "$C/game-src" && mkdir "$GAME" "$C/after.update"
    N=$((N + 1)); OUT=$C/out.$N.txt
    (cd "$C" && bwrap --ro-bind / / --dev /dev --proc /proc --bind "$T" "$T" --size "$size" --tmpfs "$GAME" \
        bash "$inner" "$i" "$REL" "$C/game-src" "$GAME" "$C/after.update" "$STEAM" "$DOCS" "$DL" "$PWSH" "$H" > "$OUT" 2>&1)
}
case_t40() {
    local i=$1 before
    UI=$i
    # A kept copy that cannot be replaced while the new ones are published (a folder holds AMD's first name): the
    # undo afterwards never leaves AMD's set mixed.
    upd_base "t40a-$i"
    loose "$DL" v2 $ALL
    run "$i" update
    rm -f "$DL"/*.dll
    loose "$DL" v3 $ALL
    rm -f "$GAME/${UDEST[up]}.before_update"
    mkdir -p "$GAME/${UDEST[up]}.before_update/blocker"
    run "$i" update
    t "T40 $i kept copy blocked: update exit 4" eq "$RC" 4
    run "$i" undo
    t "T40 $i kept copy blocked: undo leaves AMD's set whole" eval 'all_at v2 $AMD || all_at v1 $AMD'
    t "T40 $i kept copy blocked: NVIDIA's files whole" eval 'all_at v2 $NV || all_at v1 $NV'
    # A file that cannot be put back (a folder holds its name): undo says so, and a second undo once it is free works.
    upd_base "t40b-$i" nv
    loose "$DL" v2 $NV
    run "$i" update
    rm -f "$GAME/${UDEST[dlss]}"
    mkdir -p "$GAME/${UDEST[dlss]}/blocker"
    run "$i" undo
    t "T40 $i put back blocked: undo exit 4" eq "$RC" 4
    t "T40 $i put back blocked: the other file goes back" at dlssg dlssg v1
    rm -rf "$GAME/${UDEST[dlss]}"
    run "$i" undo
    t "T40 $i put back blocked: undo again exit 0" eq "$RC" 0
    t "T40 $i put back blocked: undo again puts the file back" at dlss dlss v1
    t "T40 $i put back blocked: undo again leaves the other as it was" at dlssg dlssg v1
    # A signature block far larger than any vendor's is refused before it is read.
    t29_one "$i" bigsig dlss 'its digital signature is too large to check' "$(fx dlss 310.99.0.0 certpad=2097152)"
    # --from pointed at a development or ARM folder itself, or a file inside one.
    upd_base "t40d-$i" nv
    mkdir -p "$C/sdk/lib/Windows_x86_64/rel" "$C/sdk/lib/Windows_x86_64/dev" "$C/sdk/lib/Windows_arm64ec/rel"
    cp "$(fx dlss v2)" "$C/sdk/lib/Windows_x86_64/rel/nvngx_dlss.dll"
    cp "$(fx dlss v3)" "$C/sdk/lib/Windows_x86_64/dev/nvngx_dlss.dll"
    cp "$(fx dlss 310.11.0.0)" "$C/sdk/lib/Windows_arm64ec/rel/nvngx_dlss.dll"
    before=$(snap "$GAME")
    for f in Windows_x86_64/dev Windows_arm64ec/rel Windows_x86_64/dev/nvngx_dlss.dll Windows_arm64ec/rel/nvngx_dlss.dll; do
        run "$i" update from="$C/sdk/lib/$f"
        t "T40 $i --from $f: nothing changes" eq "$(snap "$GAME")" "$before"
        t "T40 $i --from $f: says why" has "it comes from a development or ARM folder."
    done
    run "$i" update from="$C/sdk/lib/Windows_x86_64/rel"
    t "T40 $i --from the release folder itself: its copy goes in" at dlss dlss v2
    # A player's own folder named Dev above the download is not a development folder.
    mkdir -p "$C/Dev/DLSS"
    cp "$(fx dlss v3)" "$C/Dev/DLSS/nvngx_dlss.dll"
    run "$i" update from="$C/Dev/DLSS/nvngx_dlss.dll"
    t "T40 $i --from a file in a folder under Dev: it goes in" at dlss dlss v3
    upd_base "t40g-$i" nv
    mkdir -p "$C/Dev/DLSS"
    cp "$(fx dlss v3)" "$C/Dev/DLSS/nvngx_dlss.dll"
    run "$i" update from="$C/Dev/DLSS"
    t "T40 $i --from a folder under Dev: its copy goes in" at dlss dlss v3
    # The same development and ARM folders given as a relative path, from a terminal opened inside them.
    upd_base "t40h-$i" nv
    mkdir -p "$C/sdk/lib/Windows_x86_64/rel" "$C/sdk/lib/Windows_x86_64/dev" "$C/sdk/lib/Windows_arm64ec/rel"
    cp "$(fx dlss v2)" "$C/sdk/lib/Windows_x86_64/rel/nvngx_dlss.dll"
    cp "$(fx dlss v3)" "$C/sdk/lib/Windows_x86_64/dev/nvngx_dlss.dll"
    cp "$(fx dlss 310.11.0.0)" "$C/sdk/lib/Windows_arm64ec/rel/nvngx_dlss.dll"
    before=$(snap "$GAME")
    for f in Windows_x86_64/dev Windows_arm64ec/rel; do
        for a in . nvngx_dlss.dll; do
            run "$i" update from="$a" cwd="$C/sdk/lib/$f"
            t "T40 $i --from $a inside $f: nothing changes" eq "$(snap "$GAME")" "$before"
            t "T40 $i --from $a inside $f: says why" has "it comes from a development or ARM folder."
        done
    done
    run "$i" update from=. cwd="$C/sdk/lib/Windows_x86_64/rel"
    t "T40 $i --from . inside the release folder: its copy goes in" at dlss dlss v2
    # The disk fills while the downloads are read: exit 4, nothing changes, no part files.
    upd_base "t40e-$i"
    loose "$DL" v2 $ALL
    t40_small "$i"
    t "T40 $i disk full while reading downloads: exit 4" has "rc-update 4"
    t "T40 $i disk full while reading downloads: game folder as before" eq "$(snap "$C/after.update")" "$(snap "$C/game-before")"
    t "T40 $i disk full while reading downloads: no part files" eval '[ -z "$(find "$C/after.update" -name "*ffxv_dlss_part*" -print -quit)" ]'
    # The first update, with room for the downloads but not the kept copies: the folder and the record as before, and
    # no line says a file was updated or offers the undo.
    upd_base "t40f-$i"
    loose "$DL" v2 $ALL
    t40_small "$i" dl
    t "T40 $i first update, disk full: exit 4" has "rc-update 4"
    t "T40 $i first update, disk full: game folder and record as before" eq "$(snap "$C/after.update")" "$(snap "$C/game-before")"
    t "T40 $i first update, disk full: no updated line" lacks ": updated from"
    t "T40 $i first update, disk full: no undo advice" lacks "To go back to them"
    # The drive fills while the install record is copied aside (no room left after the downloads), and while the
    # longer record is written (room for the copy only): the record and the folder as before either way.
    upd_base "t40i-$i"
    loose "$DL" v2 $ALL
    t40_small "$i" dl 0
    t "T40 $i disk full at the record copy: exit 4" has "rc-update 4"
    t "T40 $i disk full at the record copy: game folder and record as before" eq "$(snap "$C/after.update")" "$(snap "$C/game-before")"
    [ "$i" = sh ] && t "T40 sh disk full at the record copy: that is where it stopped" has "installed_files.txt.ffxv_dlss_part"
    upd_base "t40j-$i"
    loose "$DL" v2 $ALL
    t40_small "$i" dl 1
    t "T40 $i disk full at the record write: exit 4" has "rc-update 4"
    t "T40 $i disk full at the record write: game folder and record as before" eq "$(snap "$C/after.update")" "$(snap "$C/game-before")"
    t "T40 $i disk full at the record write: the record copy itself went through" lacks "installed_files.txt.ffxv_dlss_part"
    [ "$i" = sh ] && t "T40 sh disk full at the record write: that is where it stopped" has "Could not copy ffxv_dlss/installed_files.txt"
}

# ---- T41 ReShade as dxgi.dll: kept and loaded by the mod, a newer one replaces the kept copy ------------------------------------
# reshade OUT VERSION [key=value...]: a 64-bit DLL whose version resource says ReShade (product=... to change it)
reshade() { mkpe "$1" dxgi.dll "$2" "${@:3}"; }
case_t41() {
    local i=$1 inner=$T/t41-inner.sh big=$T/release-big size p
    # ReShade, nothing kept yet: kept, the mod's dxgi.dll in place, the player told it keeps working; uninstall puts it back
    mkcase "t41a-$i"
    reshade "$C/rs1.dll" 6.3.3.0
    cp "$C/rs1.dll" "$GAME/dxgi.dll"
    run "$i" install
    t "T41 $i ReShade: exit 0" eq "$RC" 0
    t "T41 $i ReShade: kept under the backup name" same "$C/rs1.dll" "$GAME/dxgi.dll.before_ffxv_dlss"
    t "T41 $i ReShade: mod dxgi.dll in place" same "$REL/game/dxgi.dll" "$GAME/dxgi.dll"
    t "T41 $i ReShade: player told it was found" has "ReShade was found in the game folder."
    t "T41 $i ReShade: player told the mod loads it" has "Lumenveil loads it when the game starts"
    t "T41 $i ReShade: not called another tool" lacks "Another tool's dxgi.dll"
    run "$i" install
    t "T41 $i ReShade, second install: exit 0" eq "$RC" 0
    t "T41 $i ReShade, second install: kept copy untouched" same "$C/rs1.dll" "$GAME/dxgi.dll.before_ffxv_dlss"
    # the player runs ReShade's setup again (a newer ReShade as dxgi.dll), then the mod's install
    reshade "$C/rs2.dll" 6.4.1.0
    cp "$C/rs2.dll" "$GAME/dxgi.dll"
    run "$i" install
    t "T41 $i newer ReShade: exit 0" eq "$RC" 0
    t "T41 $i newer ReShade: replaces the kept copy" same "$C/rs2.dll" "$GAME/dxgi.dll.before_ffxv_dlss"
    t "T41 $i newer ReShade: mod dxgi.dll in place" same "$REL/game/dxgi.dll" "$GAME/dxgi.dll"
    t "T41 $i newer ReShade: player told" has "A newer ReShade was found in the game folder."
    t "T41 $i newer ReShade: no part files" eval '[ -z "$(find "$GAME" -name "*ffxv_dlss_part*" -print -quit)" ]'
    run "$i" uninstall
    t "T41 $i ReShade: uninstall exit 0" eq "$RC" 0
    t "T41 $i ReShade: uninstall puts the newer ReShade back byte for byte" same "$C/rs2.dll" "$GAME/dxgi.dll"
    t "T41 $i ReShade: uninstall leaves no kept copy" test ! -e "$GAME/dxgi.dll.before_ffxv_dlss"
    # ProductName with other case and trailing spaces is still ReShade; "ReShade Effects" is not
    mkcase "t41b-$i"
    reshade "$C/rs.dll" 6.3.3.0 'product=reshade  '
    cp "$C/rs.dll" "$GAME/dxgi.dll"
    run "$i" install
    t "T41 $i 'reshade  ': exit 0" eq "$RC" 0
    t "T41 $i 'reshade  ': taken as ReShade" has "ReShade was found in the game folder."
    mkcase "t41c-$i"
    reshade "$C/other.dll" 1.0.0.0 'product=ReShade Effects'
    cp "$C/other.dll" "$GAME/dxgi.dll"
    run "$i" install
    t "T41 $i 'ReShade Effects': exit 0" eq "$RC" 0
    t "T41 $i 'ReShade Effects': kept as another tool" same "$C/other.dll" "$GAME/dxgi.dll.before_ffxv_dlss"
    t "T41 $i 'ReShade Effects': not taken as ReShade" lacks "ReShade was found"
    t "T41 $i 'ReShade Effects': player told it is not loaded" has "Lumenveil does not load it"
    # another tool's dxgi.dll that is not a DLL at all: as today, plus the line that it is not loaded
    mkcase "t41d-$i"
    printf 'MZ another tool proxy\n' > "$C/foreign.dll"
    cp "$C/foreign.dll" "$GAME/dxgi.dll"
    run "$i" install
    t "T41 $i other tool: exit 0" eq "$RC" 0
    t "T41 $i other tool: kept" same "$C/foreign.dll" "$GAME/dxgi.dll.before_ffxv_dlss"
    t "T41 $i other tool: player told it is not loaded" has "Lumenveil does not load it"
    # mixed pairs refuse and change nothing
    for p in other-over-reshade reshade-over-other; do
        mkcase "t41e-$i-$p"
        reshade "$C/rs.dll" 6.3.3.0
        printf 'MZ another tool proxy\n' > "$C/foreign.dll"
        if [ "$p" = other-over-reshade ]; then
            cp "$C/rs.dll" "$GAME/dxgi.dll.before_ffxv_dlss"; cp "$C/foreign.dll" "$GAME/dxgi.dll"
        else
            cp "$C/foreign.dll" "$GAME/dxgi.dll.before_ffxv_dlss"; cp "$C/rs.dll" "$GAME/dxgi.dll"
        fi
        before=$(snap "$GAME")
        run "$i" install
        t "T41 $i $p: exit 4" eq "$RC" 4
        t "T41 $i $p: nothing changed" eq "$(snap "$GAME")" "$before"
    done
    # disk full after the newer ReShade replaced the kept copy: the kept copy is one ReShade or the other, whole,
    # and uninstall gives back a whole ReShade
    if [ ! -d "$big" ]; then
        cp -a "$REL" "$big" && chmod -R u+w "$big" &&
            python3 -I -c 'import sys; open(sys.argv[1], "wb").write(b"MZ" + b"\0" * 3000000 + b" ffxv-dlss build 3\n")' "$big/game/dxgi.dll" &&
            chmod -R a-w "$big" || { no "T41 release with a large dxgi.dll could not be made"; return; }
    fi
    cat > "$inner" <<'EOF'
# inner IMPL REL SRC GAME AFTER STEAM DOCS DL PWSH: install on a small tmpfs, copy the result out, uninstall, copy again
impl=$1 rel=$2 src=$3 game=$4 after=$5 steam=$6 docs=$7 dl=$8 pwsh=$9
cp -a "$src/." "$game/" || exit 99
go() {
    if [ "$impl" = sh ]; then bash "$rel/$1.sh" < /dev/null
    else "$pwsh" -NoProfile -File "$rel/scripts/setup.ps1" -Action "$1" -SteamRoot "$steam" -DocumentsDir "$docs" -DownloadsDir "$dl" < /dev/null; fi
}
go install; echo "rc-install $?"
cp -a "$game/." "$after.install/"
go uninstall; echo "rc-uninstall $?"
cp -a "$game/." "$after/"
EOF
    mkcase "t41f-$i"
    reshade "$C/rs1.dll" 6.3.3.0; reshade "$C/rs2.dll" 6.4.1.0
    cp "$C/rs1.dll" "$GAME/dxgi.dll"
    run "$i" install
    cp "$C/rs2.dll" "$GAME/dxgi.dll"
    mv "$GAME" "$C/game-src" && mkdir "$GAME" "$C/after" "$C/after.install"
    size=$(( $(du -s --block-size=1 "$C/game-src" | cut -f1) + 1048576 ))
    N=$((N + 1)); OUT=$C/out.$N.txt
    (cd "$C" && env HOME="$H" bwrap --ro-bind / / --dev /dev --proc /proc --bind "$T" "$T" --size "$size" --tmpfs "$GAME" \
        bash "$inner" "$i" "$big" "$C/game-src" "$GAME" "$C/after" "$STEAM" "$DOCS" "$DL" "$PWSH" > "$OUT" 2>&1)
    t "T41 $i disk full after the replace: install exit 4" has "rc-install 4"
    t "T41 $i disk full after the replace: the replace ran before the disk filled" has "A newer ReShade was found in the game folder."
    t "T41 $i disk full after the replace: kept copy is a whole ReShade" \
        eval 'cmp -s "$C/rs2.dll" "$C/after.install/dxgi.dll.before_ffxv_dlss" || cmp -s "$C/rs1.dll" "$C/after.install/dxgi.dll.before_ffxv_dlss"'
    t "T41 $i disk full after the replace: no part files" eval '[ -z "$(find "$C/after.install" -name "*ffxv_dlss_part*" -print -quit)" ]'
    t "T41 $i disk full after the replace: uninstall exit 0" has "rc-uninstall 0"
    t "T41 $i disk full after the replace: uninstall gives back a whole ReShade" \
        eval 'cmp -s "$C/rs2.dll" "$C/after/dxgi.dll" || cmp -s "$C/rs1.dll" "$C/after/dxgi.dll"'
}

# ---- T37 install does not undo an update ------------------------------------------------------------------------------------------------
case_t37() {
    local i=$1
    UI=$i
    upd_base "t37-$i"
    loose "$DL/new" v2 $ALL
    run "$i" update "from=$DL/new"
    zipset "$DL/DLSS-main.zip" DLSS-main/lib/Windows_x86_64/rel v1 $NV
    zipset "$DL/FidelityFX-SDK-2.4.0.zip" FidelityFX-SDK-2.4.0/Kits/FidelityFX/signedbin v1 $AMD
    zipset "$DL/$XESS_ZIP" XeSS_SDK_3.0.2/bin v1 $INTEL
    run "$i" install
    t "T37 $i install again: exit 0" eq "$RC" 0
    t "T37 $i install again: newer NVIDIA files kept" all_at v2 $NV
    t "T37 $i install again: newer AMD set kept" all_at v2 $AMD
    t "T37 $i install again: newer Intel set kept" all_at v2 $INTEL
    t "T37 $i install again: says so" has "Kept nvngx_dlss.dll 310.9.1.0: it is newer than the one in this download (310.4.0.0)."
    t "T37 $i install again: says so for AMD" has "Kept the files in place: amd_fidelityfx_loader_dx12.dll there (2.3.0.4555) is newer than the one in this download (2.0.0.1)."
    t "T37 $i install again: kept copies untouched" test -f "$GAME/${UDEST[dlss]}.before_update"
}

# ---- T38 the frame generation line -----------------------------------------------------------------------------------------------------
case_t38() {
    local i=$1
    UI=$i
    upd_base "t38a-$i" nv
    loose "$DL" v2 dlssg
    run "$i" update
    t "T38 $i frame generation file changed: undo line" has "If frame generation stops working after this update, run the undo."
    upd_base "t38b-$i" nv
    loose "$DL" v2 dlss
    run "$i" update
    t "T38 $i only nvngx_dlss.dll changed: no frame generation line" lacks "If frame generation stops working"
    t "T38 $i only nvngx_dlss.dll changed: updated" at dlss dlss v2
}

# ---- stress: repeated updates and undos ---------------------------------------------------------------------------------------------------
case_stress_update() {
    local i=$1 g d k a b n ok=0 rounds=0 back=0
    UI=$i
    mkcase "stressu-$i"
    g=$(snap "$GAME"); d=$(snap "$DOCS")
    zipset "$DL/DLSS-main.zip" DLSS-main/lib/Windows_x86_64/rel v1 $NV
    run "$i" install
    rm -f "$DL"/*.zip
    loose "$C/a" v2 $ALL
    loose "$C/b" v3 $ALL
    for k in $(seq 20); do
        rounds=$((rounds + 1)) n=0
        # the record is set aside: round 1 update B adds kept-copy names for the sets update A added
        run "$i" update "from=$C/a"; [ "$RC" = 0 ] && all_at v2 $ALL && n=$((n + 1))
        a=$(snap_files "$GAME" | grep -v -e before_update -e last_update.txt)
        run "$i" update "from=$C/b"; [ "$RC" = 0 ] && all_at v3 $ALL && n=$((n + 1))
        run "$i" undo; [ "$RC" = 0 ] && all_at v2 $ALL && n=$((n + 1))
        run "$i" undo; [ "$RC" = 0 ] && has "Nothing to undo." && [ "$(snap_files "$GAME" | grep -v -e before_update -e last_update.txt)" = "$a" ] && n=$((n + 1))
        [ $n = 4 ] && ok=$((ok + 1))
    done
    t "stress $i 20 rounds of update A, update B, undo, undo: $ok of $rounds rounds right" eval '[ $ok = 20 ] && [ $rounds = 20 ]'
    run "$i" update "from=$C/b"
    b=$(snap "$GAME")
    for k in $(seq 20); do
        run "$i" update "from=$C/a" allowolder; [ "$RC" = 0 ] && all_at v2 $ALL || continue
        run "$i" undo; [ "$RC" = 0 ] && [ "$(snap "$GAME" | grep -v -e before_update -e last_update.txt)" = "$(printf '%s\n' "$b" | grep -v -e before_update -e last_update.txt)" ] && back=$((back + 1))
    done
    t "stress $i 20 rounds of update and undo: back to the same files $back of 20 times" eq "$back" 20
    t "stress $i update: record has no duplicate lines" eq "$(sort "$GAME/ffxv_dlss/installed_files.txt" | uniq -d | wc -l)" 0
    run "$i" uninstall
    t "stress $i update: one uninstall gives back the game folder as before install" eval '[ "$(snap "$GAME")" = "$g" ] && [ "$(snap "$DOCS")" = "$d" ]'
}

# ---- stress: repeated cycles, read-only release, no temporary folder ----------------------------------------------------
case_stress() {
    local i=$1 g d k b0 bad=0 runs=0 installed=0
    mkcase "stress-$i"
    printf 'MZ another tool proxy\n' > "$GAME/dxgi.dll"
    mksdk "$DL/DLSS-main.zip" DLSS-main full
    mkfsr "$DL/FidelityFX-SDK-2.4.0.zip" FidelityFX-SDK-2.4.0 full
    mkxess "$DL/$XESS_ZIP" XeSS_SDK_3.0.2 full
    g=$(snap "$GAME"); d=$(snap "$DOCS")
    for k in $(seq 20); do
        run "$i" install; [ "$RC" = 0 ] || bad=$((bad + 1)); runs=$((runs + 1))
        payload_ok "$GAME" && sdk_ok rel_dlss rel_dlssg && fsr_ok && xess_ok && [ -f "$GAME/dxgi.dll.before_ffxv_dlss" ] && [ -f "$CFG_A.before_ffxv_dlss" ] &&
            [ "$(sha256sum < "$CFG_A")" = "$(ini_text 240 100 | sha256sum)" ] && installed=$((installed + 1))
        if [ "$k" = 1 ]; then b0=$(cat "$GAME/dxgi.dll.before_ffxv_dlss" "$CFG_A.before_ffxv_dlss" | sha256sum); fi
        [ "$(cat "$GAME/dxgi.dll.before_ffxv_dlss" "$CFG_A.before_ffxv_dlss" | sha256sum)" = "$b0" ] || bad=$((bad + 1))
        run "$i" uninstall; [ "$RC" = 0 ] || bad=$((bad + 1)); runs=$((runs + 1))
        [ "$(snap "$GAME")" = "$g" ] && [ "$(snap "$DOCS")" = "$d" ] || bad=$((bad + 1))
    done
    t "stress $i 20 install/uninstall cycles: $runs runs, $installed full installs, tree back each time" eval '[ $bad = 0 ] && [ $runs = 40 ] && [ $installed = 20 ]'
    bad=0; runs=0; installed=0
    for k in $(seq 20); do
        run "$i" install; [ "$RC" = 0 ] || bad=$((bad + 1)); runs=$((runs + 1))
        payload_ok "$GAME" && sdk_ok rel_dlss rel_dlssg && fsr_ok && xess_ok && [ -f "$GAME/dxgi.dll.before_ffxv_dlss" ] && [ -f "$CFG_A.before_ffxv_dlss" ] &&
            [ "$(sha256sum < "$CFG_A")" = "$(ini_text 240 100 | sha256sum)" ] && installed=$((installed + 1))
        if [ "$k" = 1 ]; then b0=$(cat "$GAME/dxgi.dll.before_ffxv_dlss" "$CFG_A.before_ffxv_dlss" | sha256sum); fi
        [ "$(cat "$GAME/dxgi.dll.before_ffxv_dlss" "$CFG_A.before_ffxv_dlss" | sha256sum)" = "$b0" ] || bad=$((bad + 1))
    done
    t "stress $i 20 installs in a row: $runs runs, $installed full installs, backups never changed" eval '[ $bad = 0 ] && [ $runs = 20 ] && [ $installed = 20 ]'
    t "stress $i record has no duplicate lines" eq "$(sort "$GAME/ffxv_dlss/installed_files.txt" | uniq -d | wc -l)" 0
    run "$i" uninstall
    t "stress $i one uninstall after 20 installs: tree as at the start" eval '[ "$(snap "$GAME")" = "$g" ] && [ "$(snap "$DOCS")" = "$d" ]'
    mkcase "notmp-$i"
    run "$i" install env:TMPDIR=/nonexistent/tmp env:TMP=/nonexistent/tmp env:TEMP=/nonexistent/tmp
    t "stress $i temporary folder missing: exit 0" eq "$RC" 0
    t "stress $i temporary folder missing: installed" payload_ok "$GAME"
    t "stress $i release folder is read-only" eval '! touch "$REL/x" 2>/dev/null'
}

# ---- T13 static checks --------------------------------------------------------------------------------------------------
case_t13() {
    local f n
    OUT=$T/t13.txt
    "$PWSH" -NoProfile -File "$R/tests/analyse.ps1" "$R/scripts/setup.ps1" > "$OUT" 2>&1
    t "T13 PSScriptAnalyzer 5.1 compatibility rules report nothing" eq "$?" 0
    printf '$a = $null ?? 1\nGet-Content -AsByteStream x\n' > "$T/bad51.ps1"
    OUT=$T/t13b.txt
    "$PWSH" -NoProfile -File "$R/tests/analyse.ps1" "$T/bad51.ps1" > "$OUT" 2>&1
    t "T13 control: the analyser flags 7.x-only code" eq "$?" 2
    OUT=$T/t13c.txt
    grep -nE 'utf8NoBOM|\$IsWindows|\$IsLinux|AsByteStream|&&|\|\||\?\?' "$R/scripts/setup.ps1" > "$OUT"
    t "T13 none of the constructs the analyser misses" eq "$?" 1
    n=$(grep -o 'Join-Path' "$R/scripts/setup.ps1" | wc -l)
    t "T13 Join-Path is used ($n)" test "$n" -gt 10
    t "T13 every Join-Path has two parts ($n)" eq "$(grep -oE "\(Join-Path (\\\$[A-Za-z_:]+|'[^']*') (\\\$[A-Za-z_:]+|'[^']*')\)" "$R/scripts/setup.ps1" | wc -l)" "$n"
    for f in "$R/scripts/setup.ps1" "$R/install.bat" "$R/uninstall.bat" "$R/update.bat" "$R/undo-update.bat"; do
        t "T13 pure ASCII: ${f#$R/}" eval '[ -s "$f" ] && [ "$(LC_ALL=C grep -c "[^ -~	]" <(tr -d "\r" < "$f"))" = 0 ]'
    done
    for f in "$R/scripts/setup.sh" "$R/install.sh" "$R/uninstall.sh" "$R/update.sh" "$R/undo-update.sh" "$R/tools/package.sh" \
        "$R/tests/run.sh" "$R/tests/cases.sh"; do
        t "T13 bash -n ${f#$R/}" bash -n "$f"
    done
}

# ---- T15 / T16 release package --------------------------------------------------------------------------------------------
expected_list() {
    printf '%s\n' INSTALL.txt LICENSE THIRD_PARTY_NOTICES.md install.bat install.sh uninstall.bat uninstall.sh \
        update.bat update.sh undo-update.bat undo-update.sh scripts/setup.ps1 scripts/setup.sh game/dxgi.dll \
        game/ffxv_dlss/streamline/sl.interposer.dll game/ffxv_dlss/streamline/sl.common.dll \
        game/ffxv_dlss/streamline/sl.dlss_g.dll game/ffxv_dlss/streamline/sl.reflex.dll game/ffxv_dlss/streamline/sl.pcl.dll \
        game/ffxv_dlss/fonts/texgyreadventor-bold.otf game/ffxv_dlss/fonts/texgyreadventor-regular.otf \
        game/ffxv_dlss/fonts/GUST-FONT-LICENSE.txt | LC_ALL=C sort
}
case_package() {
    local P=$T/pkg sl=$T/pkg-in/streamline f bad=0 pk
    mkdir -p "$sl/development" "$T/pkg-in/build"
    for f in sl.interposer sl.common sl.dlss_g sl.reflex sl.pcl sl.nis sl.dlss nvngx_dlss nvngx_dlssg nvngx_dlssd; do
        printf 'MZ stand-in %s\n' "$f" > "$sl/$f.dll"
    done
    printf 'MZ dev\n' > "$sl/development/sl.common.dll"
    printf 'MZ stand-in proxy, ffxv-dlss\n' > "$T/pkg-in/build/dxgi.dll"
    OUT=$T/pkg.txt
    bash "$R/tools/package.sh" --dll "$T/pkg-in/build/dxgi.dll" --streamline-bin "$sl" --version 9.9 --out "$P" > "$OUT" 2>&1
    t "T15 package: exit 0" eq "$?" 0
    pk=$P/lumenveil-9.9
    t "T15 package: exactly the listed files" eq "$(cd "$pk" && find . -type f -printf '%P\n' | LC_ALL=C sort)" "$(expected_list)"
    t "T15 package: zip holds exactly the listed files" eq "$(python3 -I -c 'import sys,zipfile; [print(n[len("lumenveil-9.9/"):]) for n in zipfile.ZipFile(sys.argv[1]).namelist() if not n.endswith("/")]' "$P/lumenveil-9.9.zip" | LC_ALL=C sort)" "$(expected_list)"
    t "T15 package: streamline files are the given ones" same "$sl/sl.common.dll" "$pk/game/ffxv_dlss/streamline/sl.common.dll"
    t "T15 package: fonts are the repo's" same "$R/third_party/tex-gyre-adventor/texgyreadventor-regular.otf" "$pk/game/ffxv_dlss/fonts/texgyreadventor-regular.otf"
    t "T15 package: INSTALL.txt is the guide" same "$R/INSTALL.md" "$pk/INSTALL.txt"
    # T16
    for f in install.bat uninstall.bat update.bat undo-update.bat; do
        t "T16 $f CRLF on every line" eval "[ \$(grep -c \$'\\r\$' '$pk/$f') -gt 0 ] && [ \$(grep -c \$'\\r\$' '$pk/$f') = \$(wc -l < '$pk/$f') ]"
    done
    t "T16 .sh files executable in the zip" eq "$(python3 -I -c '
import sys, zipfile
z = zipfile.ZipFile(sys.argv[1])
print(all((z.getinfo("lumenveil-9.9/" + n).external_attr >> 16) & 0o111 for n in ["install.sh", "uninstall.sh", "update.sh", "undo-update.sh", "scripts/setup.sh"]))' "$P/lumenveil-9.9.zip")" True
    mkdir -p "$T/unz" && python3 -I -c 'import sys,zipfile,os
z = zipfile.ZipFile(sys.argv[1]); z.extractall(sys.argv[2])
for i in z.infolist():
    m = (i.external_attr >> 16) & 0o777
    if m: os.chmod(os.path.join(sys.argv[2], i.filename), m)' "$P/lumenveil-9.9.zip" "$T/unz"
    for i in sh ps; do
        mkcase "t16-$i"
        run "$i" install "rel=$T/unz/lumenveil-9.9"
        t "T16 $i packaged scripts: exit 0" eq "$RC" 0
        t "T16 $i packaged scripts: payload installed" payload_ok "$GAME" "$T/unz/lumenveil-9.9"
    done
    mkcase pkgsh
    N=$((N + 1)); OUT=$C/out.$N.txt
    (cd "$C" && HOME="$H" "$T/unz/lumenveil-9.9/install.sh" < /dev/null > "$OUT" 2>&1)
    t "T16 install.sh runs directly (executable bit)" eq "$?" 0
    # T39 the packaged update and undo run directly too
    UI=sh
    PKG=$T/unz/lumenveil-9.9
    loose "$DL" v2 dlss
    N=$((N + 1)); OUT=$C/out.$N.txt
    (cd "$C" && HOME="$H" "$PKG/update.sh" < /dev/null > "$OUT" 2>&1)
    t "T39 packaged update.sh runs directly: exit 0" eq "$?" 0
    t "T39 packaged update.sh runs directly: the file goes in" at dlss dlss v2
    N=$((N + 1)); OUT=$C/out.$N.txt
    (cd "$C" && HOME="$H" "$PKG/undo-update.sh" < /dev/null > "$OUT" 2>&1)
    t "T39 packaged undo-update.sh runs directly: exit 0" eq "$?" 0
    t "T39 packaged undo-update.sh runs directly: the update is undone" test ! -e "$GAME/ffxv_dlss/nvngx_dlss.dll"
    run ps update "rel=$PKG"
    t "T39 packaged setup.ps1 update: exit 0" eq "$RC" 0
    t "T39 packaged setup.ps1 update: the file goes in" at dlss dlss v2
    run ps undo "rel=$PKG"
    t "T39 packaged setup.ps1 undo: the update is undone" test ! -e "$GAME/ffxv_dlss/nvngx_dlss.dll"
    # the package refuses bad input
    printf 'MZ no marker here\n' > "$T/pkg-in/build/dxgi.dll"
    OUT=$T/pkg2.txt
    bash "$R/tools/package.sh" --dll "$T/pkg-in/build/dxgi.dll" --streamline-bin "$sl" --version 9.8 --out "$P" > "$OUT" 2>&1
    t "T15 dxgi.dll without the marker: fails" test "$?" -ne 0
    t "T15 dxgi.dll without the marker: says why" hasi "ffxv-dlss"
    printf 'MZ ffxv-dlss built in %s/src\n' "$HOME" > "$T/pkg-in/build/dxgi.dll"
    OUT=$T/pkg3.txt
    bash "$R/tools/package.sh" --dll "$T/pkg-in/build/dxgi.dll" --streamline-bin "$sl" --version 9.7 --out "$P" > "$OUT" 2>&1
    t "T15 home path in a file: fails" test "$?" -ne 0
    t "T15 home path in a file: says why" hasi "home folder"
    printf 'MZ ffxv-dlss built by C:\\Users\\%s\\src\n' "$USER" > "$T/pkg-in/build/dxgi.dll"
    OUT=$T/pkg3b.txt
    bash "$R/tools/package.sh" --dll "$T/pkg-in/build/dxgi.dll" --streamline-bin "$sl" --version 9.71 --out "$P" > "$OUT" 2>&1
    t "T15 user name as a path part: fails" test "$?" -ne 0
    t "T15 user name as a path part: says why" hasi "user name"
    t "T15 the licence's author handle alone does not trip the user name check" eval '! grep -q LICENSE "$T/pkg.txt"'
    python3 -I -c 'import sys; open(sys.argv[1],"wb").write(b"MZ ffxv-dlss " + sys.argv[2].encode("utf-16-le"))' "$T/pkg-in/build/dxgi.dll" "$(dirname "$T/pkg-in/build/dxgi.dll")"
    OUT=$T/pkg4.txt
    bash "$R/tools/package.sh" --dll "$T/pkg-in/build/dxgi.dll" --streamline-bin "$sl" --version 9.6 --out "$P" > "$OUT" 2>&1
    t "T15 build folder path as UTF-16: fails" test "$?" -ne 0
    rm "$sl/sl.pcl.dll"
    printf 'MZ stand-in proxy, ffxv-dlss\n' > "$T/pkg-in/build/dxgi.dll"
    OUT=$T/pkg5.txt
    bash "$R/tools/package.sh" --dll "$T/pkg-in/build/dxgi.dll" --streamline-bin "$sl" --version 9.5 --out "$P" > "$OUT" 2>&1
    t "T15 a missing Streamline file: fails" test "$?" -ne 0
}

# ---- T18 the guide ------------------------------------------------------------------------------------------------------------
case_t18() {
    local f w
    OUT=$R/INSTALL.md
    for w in "$LAUNCH" "$URL" 'MaxFramerate=240' 'ffxv_dlss\nvngx_dlss.dll' 'ffxv_dlss\streamline\nvngx_dlssg.dll' \
        'ffxv_dlss\streamline' 'ffxv_dlss\fonts' 'dxgi.dll' 'install.bat' 'uninstall.bat' 'install.sh' 'uninstall.sh' \
        'lib/Windows_x86_64/rel' 'GraphicsConfig.ini' 'steamapps/compatdata/637650/pfx/drive_c/users/steamuser/Documents' \
        'Documents\My Games\FINAL FANTASY XV\Steam' 'ffxv_dlss.log' "$LICENCE" 'Download ZIP' 'Downloads' '--game-dir' \
        'dxgi.dll.before_ffxv_dlss' 'GraphicsConfig.ini.before_ffxv_dlss' 'present=0' 'Launch Options' \
        "$FSR_URL" 'FidelityFX-SDK-2.4.0.zip' 'Kits/FidelityFX/signedbin' 'ffxv_dlss\fidelityfx' "$AMD_LICENCE" \
        $FSR_NAMES '--fsr' "$XESS_URL" "$XESS_ZIP" 'ffxv_dlss\xess' "$INTEL_LICENCE" $XESS_NAMES '--xess' \
        '## Updating DLSS, FSR and XeSS' 'update.sh' 'update.bat' 'undo-update.sh' 'undo-update.bat' '.before_update' \
        '--from' '--allow-older' '-AllowOlder'; do
        t "T18 guide contains $w" has "$w"
    done
    t "T18 guide does not send AMD players to Code, Download ZIP" eval '! grep -i "download zip" "$OUT" | grep -qiE "amd|fidelityfx"'
    t "T18 guide has the tested-only note" eval 'flat "$R/INSTALL.md" | grep -qF -- "$TESTED"'
    t "T18 README has the tested-only note" eval 'flat "$R/README.md" | grep -qF -- "$TESTED"'
    t "T18 README says RX 9000 is untested" eval 'flat "$R/README.md" | grep -qF -- "AMD'"'"'s newest FSR on Radeon RX 9000 is untested"'
    t "T18 README names the AMD headers folder" has_in "$R/README.md" 'third_party/fidelityfx'
    t "T18 README has the AMD download" has_in "$R/README.md" "$FSR_URL"
    t "T18 guide has the XeSS tested-only note" eval 'flat "$R/INSTALL.md" | grep -qF -- "$XESS_TESTED"'
    t "T18 README has the XeSS tested-only note" eval 'flat "$R/README.md" | grep -qF -- "$XESS_TESTED"'
    t "T18 README has the Intel download" has_in "$R/README.md" "$XESS_URL"
    t "T18 menu has the XeSS tested-only note" has_in "$R/src/menu.cpp" "$XESS_TESTED"
    t "T18 notices: Intel XeSS SDK row" grep -q '^| Intel XeSS SDK 3.0.2 |' "$R/THIRD_PARTY_NOTICES.md"
    t "T18 notices: Intel's licence named and linked" eval 'flat "$R/THIRD_PARTY_NOTICES.md" | grep -qF "Intel Simplified Software License" && grep -qF "https://github.com/intel/xess/blob/v3.0.2/LICENSE.txt" "$R/THIRD_PARTY_NOTICES.md"'
    t "T18 notices: Intel's DLLs are not part of the project" eval 'flat "$R/THIRD_PARTY_NOTICES.md" | grep -qF "Intel'"'"'s runtime DLLs"'
    t "T18 notices: FidelityFX headers row" grep -q '^| AMD FidelityFX SDK 2.4.0 API headers |' "$R/THIRD_PARTY_NOTICES.md"
    t "T18 notices: the FidelityFX headers' licence exactly as ffx_api.h has it" python3 -I -c '
import sys
block = []
for l in open(sys.argv[1], encoding="utf-8").read().split("\n"):
    if not l.startswith("//"): break
    block.append(l[3:] if l.startswith("// ") else l[2:])
text = open(sys.argv[2], encoding="utf-8").read()
sys.exit(0 if "```\n" + "\n".join(block) + "\n```" in text else 1)' "$R/third_party/fidelityfx/api/include/ffx_api.h" "$R/THIRD_PARTY_NOTICES.md"
    t "T18 notices: AMD's DLLs are not part of the project" eval 'flat "$R/THIRD_PARTY_NOTICES.md" | grep -qF "AMD'"'"'s runtime DLLs"'
    # player-facing text: the guide, the README sections this change touched, every message in the scripts
    {
        cat "$R/INSTALL.md"
        sed -n '/^## Requirements/,/^## Settings/p;/^## Uninstall/,/^## Known issues/p' "$R/README.md"
        grep -oE "(say|step|note|ask|warn|fail)[a-z]* +['\"].*" "$R/scripts/setup.sh"
        grep -oE "(Say|Write-Step|Write-Note|Read-Answer|Write-Fail)[A-Za-z-]* +['\"].*" "$R/scripts/setup.ps1"
        grep -i 'echo' "$R/install.bat" "$R/uninstall.bat" "$R/update.bat" "$R/undo-update.bat"
        grep -oE '"([^"\\]|\\.)*"' "$R/src/menu.cpp"
    } > "$T/player-text.txt"
    OUT=$T/player-text.txt
    t "T18 player text collected" test "$(wc -l < "$OUT")" -gt 100
    t "T18 no em dash" lacks '—'
    t "T18 no en dash" lacks '–'
    t "T18 no hyphen used as a dash" eval '! grep -n " - " "$OUT" | grep -v "^[0-9]*:- " | grep -vE "^[0-9]*: +- " | grep -q .'
    t "T18 no US spelling: color" eval '! grep -qiE "\bcolor" "$OUT"'
    t "T18 no US spelling: license as a noun in prose" eval '! grep -n "license" "$OUT" | grep -viE "LICENSE|license\.txt|RTX SDKs License|LICENSE\.txt" | grep -q .'
    t "T18 licence spelt the UK way" has "licence"
}

# ---- T12b the disk fills while dxgi.dll is copied (review #1) --------------------------------------------------------------
# The game folder is a 1 MB tmpfs in a nested sandbox and the release's dxgi.dll is 3 MB with the marker near its end, so
# the copy really runs out of space part way. Install must fail cleanly and uninstall must give back the folder as it was.
case_t12b() {
    local i=$1 big=$T/release-big v inner=$T/t12b-inner.sh
    if [ ! -d "$big" ]; then
        cp -a "$REL" "$big" && chmod -R u+w "$big" &&
            python3 -I -c 'import sys; open(sys.argv[1], "wb").write(b"MZ" + b"\0" * 3000000 + b" ffxv-dlss build 3\n")' "$big/game/dxgi.dll" &&
            chmod -R a-w "$big" || { no "T12b release with a large dxgi.dll could not be made"; return; }
    fi
    cat > "$inner" <<'EOF'
# inner IMPL REL SRC GAME AFTER STEAM DOCS DL PWSH: fill the tmpfs game folder, install, uninstall, copy the result out
impl=$1 rel=$2 src=$3 game=$4 after=$5 steam=$6 docs=$7 dl=$8 pwsh=$9
cp -a "$src/." "$game/" || exit 99
go() {
    if [ "$impl" = sh ]; then bash "$rel/$1.sh" < /dev/null
    else "$pwsh" -NoProfile -File "$rel/scripts/setup.ps1" -Action "$1" -SteamRoot "$steam" -DocumentsDir "$docs" -DownloadsDir "$dl" < /dev/null; fi
}
go install; echo "rc-install $?"
cp -a "$game/." "$after.install/"
go uninstall; echo "rc-uninstall $?"
cp -a "$game/." "$after/"
EOF
    for v in plain foreign; do
        mkcase "t12b-$i-$v"
        [ "$v" = foreign ] && printf 'MZ another tool proxy\n' > "$GAME/dxgi.dll"
        cp -a "$GAME" "$C/game-before" && mv "$GAME" "$C/game-src" && mkdir "$GAME" "$C/after" "$C/after.install"
        N=$((N + 1)); OUT=$C/out.$N.txt
        (cd "$C" && env HOME="$H" bwrap --ro-bind / / --dev /dev --proc /proc --bind "$T" "$T" --size 1048576 --tmpfs "$GAME" \
            bash "$inner" "$i" "$big" "$C/game-src" "$GAME" "$C/after" "$STEAM" "$DOCS" "$DL" "$PWSH" > "$OUT" 2>&1)
        t "T12b $i $v disk full: install exit 4" has "rc-install 4"
        t "T12b $i $v disk full: no dxgi.dll without the marker left after the failed install" \
            eval '[ ! -e "$C/after.install/dxgi.dll" ] || grep -qaF "ffxv-dlss" "$C/after.install/dxgi.dll" || cmp -s "$C/after.install/dxgi.dll" "$C/game-before/dxgi.dll"'
        t "T12b $i $v disk full: uninstall exit 0" has "rc-uninstall 0"
        t "T12b $i $v disk full: uninstall gives back the game folder as it was" eq "$(snap "$C/game-before")" "$(snap "$C/after")"
    done
}

# ---- T19 a locked settings file (review #2, PowerShell) ------------------------------------------------------------------
# Another program holds GraphicsConfig.ini with no sharing: install and uninstall report it with the by-hand line and finish.
case_t19() {
    local i=$1 lk
    mkcase "t19-$i"
    flock -x "$CFG_A" sleep 60 & lk=$!
    sleep 0.3
    run "$i" install
    kill "$lk" 2> /dev/null; wait "$lk" 2> /dev/null
    t "T19 $i locked settings file at install: exit 0" eq "$RC" 0
    t "T19 $i locked settings file at install: mod files installed" payload_ok "$GAME"
    t "T19 $i locked settings file at install: reported failed with the by-hand line" eval 'hasi failed && has MaxFramerate=240'
    t "T19 $i locked settings file at install: unchanged" eq "$(ini_text 120 100 | sha256sum)" "$(sha256sum < "$CFG_A")"
    t "T19 $i locked settings file at install: other account still handled" hasi "already 240"
    t "T19 $i locked settings file at install: summary shown" hasi "summary"
    mkcase "t19u-$i"
    run "$i" install
    flock -x "$CFG_A" sleep 60 & lk=$!
    sleep 0.3
    run "$i" uninstall
    kill "$lk" 2> /dev/null; wait "$lk" 2> /dev/null
    t "T19 $i locked settings file at uninstall: exit 0" eq "$RC" 0
    t "T19 $i locked settings file at uninstall: mod files removed" none_installed
    t "T19 $i locked settings file at uninstall: settings file and backup kept" eval '[ -s "$CFG_A.before_ffxv_dlss" ] && [ "$(sha256sum < "$CFG_A")" = "$(ini_text 240 100 | sha256sum)" ]'
    t "T19 $i locked settings file at uninstall: reported with the by-hand line" eval 'hasi failed && has MaxFramerate=120'
}

# ---- T15b no personal details in the release (review #4) ------------------------------------------------------------------
case_t15b() {
    local sl=$T/pkg-in/streamline
    t "T15b LICENSE holds no user name" eval '[ ${#USER} -lt 3 ] || ! grep -qiF -- "$USER" "$R/LICENSE"'
    printf 'MZ stand-in proxy, ffxv-dlss\n' > "$T/pkg-in/build/dxgi.dll"
    printf 'MZ stand-in sl.pcl\n' > "$sl/sl.pcl.dll"
    printf 'MZ ffxv-dlss by x%sy\n' "$USER" > "$T/pkg-in/build/dxgi.dll"
    OUT=$T/pkg6.txt
    bash "$R/tools/package.sh" --dll "$T/pkg-in/build/dxgi.dll" --streamline-bin "$sl" --version 9.4 --out "$T/pkg" > "$OUT" 2>&1
    t "T15b user name inside a longer word: fails" test "$?" -ne 0
    t "T15b user name inside a longer word: says why" hasi "user name"
    python3 -I -c 'import sys; open(sys.argv[1], "wb").write(b"MZ ffxv-dlss " + ("A" + sys.argv[2] + "B").encode("utf-16-le"))' "$T/pkg-in/build/dxgi.dll" "$USER"
    OUT=$T/pkg7.txt
    bash "$R/tools/package.sh" --dll "$T/pkg-in/build/dxgi.dll" --streamline-bin "$sl" --version 9.3 --out "$T/pkg" > "$OUT" 2>&1
    t "T15b user name inside a longer word as UTF-16: fails" test "$?" -ne 0
}

# ---- T15c Intel's DLLs never ship --------------------------------------------------------------------------------------------
case_t15c() {
    local sl=$T/pkg-in/streamline shim=$T/pkg-shim n v=20
    mkdir -p "$shim"
    cat > "$shim/cp" <<'EOF'
#!/usr/bin/env bash
# cp, then a planted file beside the staged dxgi.dll's ffxv_dlss folder (the name in $PLANT)
/usr/bin/cp "$@" || exit
for a in "$@"; do
    case $a in */game/dxgi.dll) d=${a%/dxgi.dll}/ffxv_dlss/xess; mkdir -p "$d" && printf 'MZ planted\n' > "$d/$PLANT" ;; esac
done
EOF
    chmod +x "$shim/cp"
    printf 'MZ stand-in proxy, ffxv-dlss\n' > "$T/pkg-in/build/dxgi.dll"
    printf 'MZ stand-in sl.pcl\n' > "$sl/sl.pcl.dll"
    for n in libxess.dll libxess_fg.dll libxell.dll readme.txt; do
        v=$((v + 1))
        OUT=$T/pkg-plant-$n.txt
        PLANT=$n PATH="$shim:$PATH" bash "$R/tools/package.sh" --dll "$T/pkg-in/build/dxgi.dll" --streamline-bin "$sl" --version "9.$v" --out "$T/pkg" > "$OUT" 2>&1
        t "T15c planted $n: fails" test "$?" -ne 0
        if [ "$n" = readme.txt ]; then
            t "T15c control: a planted readme.txt fails on the file list, not as a vendor file" eval 'has "readme.txt" && ! has "must not ship"'
        else
            t "T15c planted $n: refused by name" has "game/ffxv_dlss/xess/$n must not ship"
        fi
    done
}

# ---- run -----------------------------------------------------------------------------------------------------------------------
REL=$T/release
mkrelease "$REL" || no "release folder could not be made (scripts missing?)"
mkdlls
mkdir -p "$T/bins/base" "$T/bins/unzip" "$T/bins/bsdtar" "$T/bins/python3"
for f in /usr/bin/*; do
    case ${f##*/} in unzip | bsdtar | python*) continue ;; esac
    ln -s "$f" "$T/bins/base/${f##*/}"
done
ln -s /usr/bin/unzip "$T/bins/unzip/unzip"
ln -s /usr/bin/bsdtar "$T/bins/bsdtar/bsdtar"
ln -s /usr/bin/python3 "$T/bins/python3/python3"

groups=${*:-sh ps static package stress}
for g in $groups; do
    case $g in
        sh | ps)
            for c in t1 t2 t3 t4 t5 t6 t7 t9 t10 t11 t12 t14 t20 t21 t22 t24 t25 t26 t27 t28 t29 t30 t31 t32 t33 t34 t35 t36 t37 t38 t40 t41; do
                "case_$c" "$g"
            done
            case_t12b "$g"
            [ "$g" = ps ] && case_t19 ps
            [ "$g" = sh ] && { case_t8; case_t5 sh "unzip bsdtar python3"; }
            [ "$g" = ps ] && {
                OUT=$T/unit.txt
                "$PWSH" -NoProfile -File "$R/tests/unit.ps1" -Case parser -Script "$R/scripts/setup.ps1" > "$OUT" 2>&1
                t "T1 ps library parser with Windows text" eq "$?" 0
                unit_pe
            }
            ;;
        static) case_t13; case_t18 ;;
        t[0-9]*) "case_$g" sh; "case_$g" ps ;;   # one case, both scripts (tests/run.sh t40)
        package) case_package ;;
        stress) case_stress sh; case_stress ps; case_stress_update sh; case_stress_update ps ;;
    esac
    [ "$g" = package ] && { case_t15b; case_t15c; }
done
echo
echo "passed $pass, failed $fail"
[ "$fail" = 0 ] && [ "$pass" -gt 0 ]
