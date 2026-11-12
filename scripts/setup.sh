#!/usr/bin/env bash
# Installs, updates or removes Lumenveil, a mod for FINAL FANTASY XV played through Steam's Proton on Linux.
# Usage: bash scripts/setup.sh install|uninstall|update|undo [--game-dir <folder>] [--sdk <zip or folder>]
#        [--fsr <zip or folder>] [--xess <zip or folder>] [--from <file, zip or folder>] [--allow-older] [--no-prompt]
# Normally started through install.sh, uninstall.sh, update.sh or undo-update.sh in the release folder.
set -u

release=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)
payload=$release/game
mark='ffxv-dlss'                      # text inside this mod's dxgi.dll
record_rel='ffxv_dlss/installed_files.txt'
keep='.before_ffxv_dlss'              # suffix of the backups this script makes
part='.ffxv_dlss_part'                # suffix of a file while it is being copied
launch='WINEDLLOVERRIDES="dxgi=n,b" %command%'
sdk_url='https://github.com/NVIDIA/DLSS'
rel_dir='lib/Windows_x86_64/rel'
fsr_url='https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/archive/refs/tags/v2.4.0.zip'
fsr_rel='Kits/FidelityFX/signedbin'     # where AMD's signed DLLs are in their SDK
fsr_dir='ffxv_dlss/fidelityfx'
xess_url='https://github.com/intel/xess/releases/download/v3.0.2/XeSS_SDK_3.0.2.zip'
xess_dir='ffxv_dlss/xess'
prefix_docs='steamapps/compatdata/637650/pfx/drive_c/users/steamuser/Documents'
settings_rel='My Games/FINAL FANTASY XV/Steam'

usage() { echo "Usage: bash setup.sh install|uninstall|update|undo [--game-dir <folder>] [--sdk <zip or folder>] [--fsr <zip or folder>] [--xess <zip or folder>] [--from <file, zip or folder>] [--allow-older] [--no-prompt]"; }
action=${1:-}
case $action in install | uninstall | update | undo) shift ;; *) usage; exit 1 ;; esac
game_arg='' sdk_arg='' fsr_arg='' xess_arg='' from_arg='' allow_older=0 prompts=1
while [ $# -gt 0 ]; do
    case $1 in
        --game-dir | --sdk | --fsr | --xess | --from)
            [ $# -ge 2 ] || { usage; exit 1; }
            case $1 in --game-dir) game_arg=$2 ;; --sdk) sdk_arg=$2 ;; --fsr) fsr_arg=$2 ;; --xess) xess_arg=$2 ;; *) from_arg=$2 ;; esac
            shift 2 ;;
        --allow-older) allow_older=1; shift ;;
        --no-prompt) prompts=0; shift ;;
        *) usage; exit 1 ;;
    esac
done
[ -t 0 ] || prompts=0
case $action in undo) script_name=undo-update.sh ;; *) script_name=$action.sh ;; esac

say() { printf '%s\n' "$*"; }
note() { printf '        %s\n' "$*"; }
step_no() { # step_no TITLE: that step's number in $steps
    local k
    for k in "${!steps[@]}"; do [ "${steps[$k]}" = "$1" ] && { echo $((k + 1)); return; }; done
}
step() { printf '\n%s of %s  %s\n' "$(step_no "$1")" "${#steps[@]}" "$1"; }
ask() { # ask QUESTION: the player's answer, trimmed and without surrounding quotes, in $answer
    local s
    printf '        %s ' "$1"
    IFS= read -r s || s=''
    s=${s%$'\r'}
    s="${s#"${s%%[![:space:]]*}"}"
    s="${s%"${s##*[![:space:]]}"}"
    case $s in \"*\" | \'*\') s=${s:1:${#s}-2} ;; esac
    case $s in '~'/*) s=$HOME/${s#'~/'} ;; esac
    [ -e "$s" ] || s=${s//\\ / }   # a dragged-in path with escaped spaces
    answer=$s
}

game_running() { pgrep -x ffxv_s.exe > /dev/null 2>&1; }

# ---- finding things ------------------------------------------------------------------------------
unique_dirs() { # existing folders from standard input, by real path, first seen first
    local d
    while IFS= read -r d; do [ -n "$d" ] && [ -d "$d" ] && realpath -e -- "$d"; done | awk '!seen[$0]++'
}
steam_roots() {
    local d
    for d in "$HOME/.local/share/Steam" "$HOME/.steam/steam" "$HOME/.steam/root" \
        "$HOME/.var/app/com.valvesoftware.Steam/.local/share/Steam" "$HOME/snap/steam/common/.local/share/Steam"; do
        [ -d "$d/steamapps" ] && printf '%s\n' "$d"
    done | unique_dirs
}
vdf_value() { # vdf_value KEY FILE: every "KEY" "value" line's value, with doubled backslashes undone
    LC_ALL=C sed -n 's/^[[:space:]]*"'"$1"'"[[:space:]]*"\(.*\)"[[:space:]]*$/\1/p' "$2" | LC_ALL=C sed 's/\\\\/\\/g'
}
libraries() {
    local root
    {
        steam_roots | while IFS= read -r root; do
            printf '%s\n' "$root"
            [ -f "$root/steamapps/libraryfolders.vdf" ] && vdf_value path "$root/steamapps/libraryfolders.vdf"
        done
        # the library that holds the game folder, when it was given by hand
        if [ -n "${game:-}" ] && [ "$(basename -- "$(dirname -- "$game")")" = common ]; then
            dirname -- "$(dirname -- "$(dirname -- "$game")")"
        fi
    } | while IFS= read -r root; do [ -d "$root/steamapps" ] && printf '%s\n' "$root"; done | unique_dirs
}
find_game() {
    local lib dir acf
    while IFS= read -r lib; do
        acf=$lib/steamapps/appmanifest_637650.acf
        [ -f "$acf" ] || continue
        dir=$(vdf_value installdir "$acf" | head -n 1)
        if [ -n "$dir" ] && [ -f "$lib/steamapps/common/$dir/ffxv_s.exe" ]; then
            printf '%s\n' "$lib/steamapps/common/$dir"
            return 0
        fi
    done < <(libraries)
    return 1
}
choose_game() {
    local cand=$game_arg
    if [ -z "$cand" ]; then
        if game=$(find_game); then note "Found $game"; return 0; fi
        note "Could not find FINAL FANTASY XV in your Steam libraries."
    fi
    while :; do
        if [ -n "$cand" ]; then
            if [ -f "$cand/ffxv_s.exe" ]; then
                game=$(cd -- "$cand" && pwd -P)
                note "Using $game"
                return 0
            fi
            note "$cand does not hold ffxv_s.exe, so it is not the game folder."
        fi
        if [ "$prompts" = 0 ]; then
            note "Run this again and name the game folder (the folder that holds ffxv_s.exe), like this:"
            note "bash $script_name --game-dir \"/path/to/steamapps/common/FINAL FANTASY XV\""
            return 3
        fi
        ask "Game folder (the one that holds ffxv_s.exe), or Enter to stop:"
        cand=$answer
        [ -n "$cand" ] || return 3
    done
}
settings_files() { # settings_files [SUFFIX]: the game's settings files (or their backups) in every library
    local lib f
    while IFS= read -r lib; do
        for f in "$lib/$prefix_docs/$settings_rel"/*/savestorage/"GraphicsConfig.ini${1:-}"; do
            [ -f "$f" ] && printf '%s\n' "$f"
        done
    done < <(libraries)
}
account_of() { basename -- "$(dirname -- "$(dirname -- "$1")")"; }

# ---- settings file -----------------------------------------------------------------------------------
ini_get() { LC_ALL=C sed -n 's/^'"$2"'=\([0-9][0-9]*\)\r\{0,1\}$/\1/p' "$1" | head -n 1; }
ini_set() { LC_ALL=C sed -i -E 's/^('"$2"'=)[0-9]+(\r?)$/\1'"$3"'\2/' "$1" && [ "$(ini_get "$1" "$2")" = "$3" ]; }
at_least() { [ "${#1}" -gt 4 ] || [ "$1" -ge "$2" ]; }

# ---- install record -------------------------------------------------------------------------------------
record_add() { # record_add ENTRY...: merges entries into the record
    local rec=$game/$record_rel
    { [ -f "$rec" ] && tr -d '\r' < "$rec"; printf '%s\n' "$@"; } | awk 'NF && !seen[$0]++' > "$rec.new" &&
        mv -f -- "$rec.new" "$rec"
}
valid_entry() { # only this mod's own names: dxgi.dll or a plain relative path inside ffxv_dlss/
    case $1 in dxgi.dll) return 0 ;; ffxv_dlss/?*) ;; *) return 1 ;; esac
    case $1 in *\\* | *:*) return 1 ;; esac
    case "/$1/" in *//* | *./* | *' '/*) return 1 ;; esac   # empty, ".", ".." or a name Windows would trim
    return 0
}

# ---- zips ---------------------------------------------------------------------------------------------------
zip_tool=none
for z in unzip bsdtar python3; do
    if command -v "$z" > /dev/null 2>&1; then zip_tool=$z; break; fi
done
pattern() { printf '%s' "$1" | sed 's/[][*?\\]/\\&/g'; }
zip_list() {
    case $zip_tool in
        unzip) unzip -Z1 "$1" ;;
        bsdtar) bsdtar -tf "$1" ;;
        python3) python3 -I -c 'import sys, zipfile; print("\n".join(zipfile.ZipFile(sys.argv[1]).namelist()))' "$1" ;;
        *) return 1 ;;
    esac 2> /dev/null
}
zip_extract() { # zip_extract ZIP ENTRY OUT
    case $zip_tool in
        unzip) unzip -p "$1" "$(pattern "$2")" > "$3" ;;
        bsdtar) bsdtar -xOf "$1" "$(pattern "$2")" > "$3" ;;
        python3) python3 -I -c 'import sys, zipfile, shutil
with zipfile.ZipFile(sys.argv[1]) as z, z.open(sys.argv[2]) as i, open(sys.argv[3], "wb") as o: shutil.copyfileobj(i, o)' "$1" "$2" "$3" ;;
    esac 2> /dev/null
}

# ---- NVIDIA files ----------------------------------------------------------------------------------------------
downloads_dirs() {
    local d
    {
        if command -v xdg-user-dir > /dev/null 2>&1; then
            d=$(xdg-user-dir DOWNLOAD 2> /dev/null)
            [ -n "$d" ] && [ "$d" != "$HOME" ] && printf '%s\n' "$d"
        fi
        printf '%s\n' "$HOME/Downloads"
    } | unique_dirs
}
sdk_from() { # sdk_from PATH: sets src, src_kind, e_dlss, e_dlssg when PATH holds NVIDIA's rel files
    local p=$1 names d
    e_dlssg=''
    if [ -f "$p" ]; then
        if [ "$zip_tool" = none ]; then
            note "Found $p, but there is no unzip, bsdtar or python3 here to open it. Extract it, then run this again."
            return 1
        fi
        names=$(zip_list "$p") || return 1
        e_dlss=$(printf '%s\n' "$names" | grep -m 1 -E "(^|/)$rel_dir/nvngx_dlss\.dll$") || return 1
        printf '%s\n' "$names" | grep -qxF -- "${e_dlss%nvngx_dlss.dll}nvngx_dlssg.dll" && e_dlssg=${e_dlss%nvngx_dlss.dll}nvngx_dlssg.dll
        src=$p src_kind=zip
        return 0
    fi
    [ -d "$p" ] || return 1
    for d in "$p" "$p"/*/; do
        d=${d%/}
        if [ -f "$d/$rel_dir/nvngx_dlss.dll" ]; then
            e_dlss=$d/$rel_dir/nvngx_dlss.dll
            [ -f "$d/$rel_dir/nvngx_dlssg.dll" ] && e_dlssg=$d/$rel_dir/nvngx_dlssg.dll
            src=$d src_kind=folder
            return 0
        fi
    done
    return 1
}
find_sdk() {
    local d f
    if [ -n "$sdk_arg" ]; then
        sdk_from "$sdk_arg" && return 0
        note "No NVIDIA DLSS files found in $sdk_arg"
        return 1
    fi
    while IFS= read -r d; do
        while IFS= read -r f; do sdk_from "$f" && return 0; done \
            < <(find "$d" -maxdepth 1 -type f -name 'DLSS*.zip' -printf '%T@ %p\n' | sort -rn | cut -d ' ' -f 2-)
        while IFS= read -r f; do sdk_from "$f" && return 0; done \
            < <(find "$d" -mindepth 1 -maxdepth 1 -type d -name 'DLSS*' | LC_ALL=C sort)
    done < <(downloads_dirs)
    return 1
}
min_size() { # min_size NAME: the smallest size in bytes a real copy of that vendor DLL can have
    case $1 in
        amd_fidelityfx_loader_dx12.dll) echo 16384 ;;
        libxell.dll) echo 131072 ;;
        *) echo 1048576 ;;
    esac
}
valid_dll() { # valid_dll FILE NAME
    [ "$(head -c 2 -- "$1" | tr -d '\0')" = MZ ] && [ "$(stat -c %s -- "$1")" -gt "$(min_size "$2")" ]
}
part_of() { printf '%s\n' "$game/ffxv_dlss/.ffxv_dlss_part.$$.$1"; }
fetch_dll() { # fetch_dll KIND SOURCE ENTRY NAME: one vendor file from a download to a temporary name, checked
    local tmp
    tmp=$(part_of "$4")
    if [ "$1" = zip ]; then zip_extract "$2" "$3" "$tmp"; else cp -f --no-preserve=mode -- "$3" "$tmp" 2> /dev/null; fi
    if [ $? != 0 ]; then
        rm -f -- "$tmp"
        note "Could not read $4 from $2"
        return 1
    fi
    if ! valid_dll "$tmp" "$4"; then
        rm -f -- "$tmp"
        note "Refused $4 from $2: it is not a valid DLL."
        return 1
    fi
}
place_dll() { # place_dll NAME DEST: a fetched file renamed into place and recorded
    mkdir -p -- "$(dirname -- "$game/$2")" && mv -f -- "$(part_of "$1")" "$game/$2" && record_add "$2"
}
newer_in_place() { # newer_in_place NAME DEST: true when the file in place and the fetched one both have a version and
    # the one in place is higher; their versions are then in in_place_ver and download_ver
    [ -f "$game/$2" ] || return 1
    download_ver=$(version_of "$(part_of "$1")") in_place_ver=$(version_of "$game/$2")
    [ -n "$download_ver" ] && [ -n "$in_place_ver" ] && [ "$(ver_cmp "$in_place_ver" "$download_ver")" = 1 ]
}
put_dll() { # put_dll SOURCE DEST: one of NVIDIA's files from the found download, checked, then moved into place
    fetch_dll "$src_kind" "$src" "$1" "${2##*/}" || return
    if newer_in_place "${2##*/}" "$2"; then
        drop "$(part_of "${2##*/}")"
        note "Kept ${2##*/} $in_place_ver: it is newer than the one in this download ($download_ver)."
        put_kept=1
        return 0
    fi
    place_dll "${2##*/}" "$2"
}
keep_newer_set() { # keep_newer_set DIR NAME...: true (and the fetched files deleted) when any file of the set in place
    # is newer than the download's
    local d=$1 n
    shift
    for n in "$@"; do
        newer_in_place "$n" "$d/$n" || continue
        note "Kept the files in place: $n there ($in_place_ver) is newer than the one in this download ($download_ver)."
        for n in "$@"; do drop "$(part_of "$n")"; done
        return 0
    done
    return 1
}
explain_sdk() {
    note "NVIDIA's DLSS files are not included with this mod. To add them:"
    note "1. Open $sdk_url, choose Code, then Download ZIP."
    note "2. Leave the zip in your Downloads folder and run this again."
    note "Or copy them by hand from $rel_dir in that zip:"
    note "   nvngx_dlss.dll goes to ffxv_dlss/nvngx_dlss.dll in the game folder"
    note "   nvngx_dlssg.dll goes to ffxv_dlss/streamline/nvngx_dlssg.dll (Frame Generation)"
    note "By downloading them you accept NVIDIA's licence for those files."
}
step_nvidia() {
    local in_place=0
    [ -f "$game/ffxv_dlss/nvngx_dlss.dll" ] && in_place=1
    if ! find_sdk; then
        if [ $in_place = 1 ]; then
            note "NVIDIA's files are already in place."
            [ -f "$game/ffxv_dlss/streamline/nvngx_dlssg.dll" ] || note "nvngx_dlssg.dll is missing: Frame Generation needs it."
            st_nvidia='already done'
            return
        fi
        explain_sdk
        st_nvidia=skipped
        [ "$prompts" = 1 ] || return
        ask "Path to the DLSS zip or folder, or Enter to skip:"
        [ -n "$answer" ] || return
        sdk_arg=$answer
        find_sdk || return
    fi
    note "Using your download $src"
    st_nvidia=failed put_kept=0
    put_dll "$e_dlss" ffxv_dlss/nvngx_dlss.dll || return
    if [ -z "$e_dlssg" ]; then
        note "Copied nvngx_dlss.dll. This download has no nvngx_dlssg.dll: Frame Generation needs it."
        st_nvidia=done
        return
    fi
    if put_dll "$e_dlssg" ffxv_dlss/streamline/nvngx_dlssg.dll; then
        [ $put_kept = 1 ] || note "Copied nvngx_dlss.dll and nvngx_dlssg.dll."
    else
        note "Copied nvngx_dlss.dll only: Frame Generation needs nvngx_dlssg.dll."
    fi
    st_nvidia=done
}

# ---- AMD files --------------------------------------------------------------------------------------------------
fsr_from() { # fsr_from PATH: sets fsrc, fsrc_kind and e_loader, e_up, e_fg (empty when missing) when PATH holds AMD's loader
    local p=$1 names d pre
    e_up='' e_fg=''
    if [ -f "$p" ]; then
        [ "$zip_tool" = none ] && return 1
        names=$(zip_list "$p") || return 1
        e_loader=$(printf '%s\n' "$names" | grep -m 1 -E "(^|/)$fsr_rel/amd_fidelityfx_loader_dx12\.dll$") || return 1
        pre=${e_loader%amd_fidelityfx_loader_dx12.dll}
        printf '%s\n' "$names" | grep -qxF -- "${pre}amd_fidelityfx_upscaler_dx12.dll" && e_up=${pre}amd_fidelityfx_upscaler_dx12.dll
        printf '%s\n' "$names" | grep -qxF -- "${pre}amd_fidelityfx_framegeneration_dx12.dll" && e_fg=${pre}amd_fidelityfx_framegeneration_dx12.dll
        fsrc=$p fsrc_kind=zip
        return 0
    fi
    [ -d "$p" ] || return 1
    for d in "$p" "$p"/*/; do
        d=${d%/}/$fsr_rel
        [ -f "$d/amd_fidelityfx_loader_dx12.dll" ] || continue
        e_loader=$d/amd_fidelityfx_loader_dx12.dll
        [ -f "$d/amd_fidelityfx_upscaler_dx12.dll" ] && e_up=$d/amd_fidelityfx_upscaler_dx12.dll
        [ -f "$d/amd_fidelityfx_framegeneration_dx12.dll" ] && e_fg=$d/amd_fidelityfx_framegeneration_dx12.dll
        fsrc=${p%/} fsrc_kind=folder
        return 0
    done
    return 1
}
find_fsr() {
    local d f
    if [ -n "$fsr_arg" ]; then
        fsr_from "$fsr_arg" && return 0
        note "No AMD FSR files found in $fsr_arg"
        return 1
    fi
    while IFS= read -r d; do
        while IFS= read -r f; do fsr_from "$f" && return 0; done \
            < <(find "$d" -maxdepth 1 -type f -name 'FidelityFX-SDK*.zip' -printf '%T@ %p\n' | sort -rn | cut -d ' ' -f 2-)
        while IFS= read -r f; do fsr_from "$f" && return 0; done \
            < <(find "$d" -mindepth 1 -maxdepth 1 -type d -name 'FidelityFX-SDK*' | LC_ALL=C sort)
    done < <(downloads_dirs)
    return 1
}
explain_fsr() {
    note "AMD FSR is optional, and DLSS works without AMD's files. They are not included with this mod. To add them:"
    note "1. Download AMD's FidelityFX SDK 2.4.0 from $fsr_url"
    note "   Your browser saves it as FidelityFX-SDK-2.4.0.zip (about 200 MB)."
    note "2. Leave the zip in your Downloads folder and run this again."
    note "Or copy these three files by hand from $fsr_rel in that zip to $fsr_dir in the game folder:"
    note "   amd_fidelityfx_loader_dx12.dll"
    note "   amd_fidelityfx_upscaler_dx12.dll"
    note "   amd_fidelityfx_framegeneration_dx12.dll (FSR frame generation)"
    note "By downloading them you accept AMD's licence for those files."
}
step_fsr() {
    local n names=()
    if ! find_fsr; then
        if [ -f "$game/$fsr_dir/amd_fidelityfx_loader_dx12.dll" ] && [ -f "$game/$fsr_dir/amd_fidelityfx_upscaler_dx12.dll" ]; then
            note "AMD's FSR files are already in place."
            [ -f "$game/$fsr_dir/amd_fidelityfx_framegeneration_dx12.dll" ] ||
                note "amd_fidelityfx_framegeneration_dx12.dll is missing, and FSR frame generation needs it."
            st_fsr='already done'
            return
        fi
        explain_fsr
        st_fsr=skipped
        return
    fi
    note "Using your download $fsrc"
    st_fsr=failed
    if [ -z "$e_up" ]; then
        note "This download has amd_fidelityfx_loader_dx12.dll but no amd_fidelityfx_upscaler_dx12.dll, so nothing was copied."
        return
    fi
    # All of them are checked before any is put in place, so a bad download changes nothing.
    names=(amd_fidelityfx_loader_dx12.dll amd_fidelityfx_upscaler_dx12.dll)
    fetch_dll "$fsrc_kind" "$fsrc" "$e_loader" "${names[0]}" &&
        fetch_dll "$fsrc_kind" "$fsrc" "$e_up" "${names[1]}" &&
        if [ -n "$e_fg" ]; then
            names+=(amd_fidelityfx_framegeneration_dx12.dll)
            fetch_dll "$fsrc_kind" "$fsrc" "$e_fg" "${names[2]}"
        fi
    if [ $? != 0 ]; then
        for n in "${names[@]}"; do rm -f -- "$(part_of "$n")"; done
        note "Nothing was copied from this download."
        return
    fi
    if keep_newer_set "$fsr_dir" "${names[@]}"; then st_fsr='kept newer'; return; fi
    for n in "${names[@]}"; do place_dll "$n" "$fsr_dir/$n" || return; done
    if [ -n "$e_fg" ]; then
        note "Copied AMD's three FSR files."
    else
        note "Copied amd_fidelityfx_loader_dx12.dll and amd_fidelityfx_upscaler_dx12.dll. This download has no"
        note "amd_fidelityfx_framegeneration_dx12.dll, and FSR frame generation needs it."
    fi
    st_fsr=done
}

# ---- Intel files ------------------------------------------------------------------------------------------------
xess_from() { # xess_from PATH: sets xsrc, xsrc_kind and e_xess, e_xfg, e_xell (empty when missing) when PATH holds bin/libxess.dll
    local p=$1 names d pre
    e_xfg='' e_xell=''
    if [ -f "$p" ]; then
        [ "$zip_tool" = none ] && return 1
        names=$(zip_list "$p") || return 1
        e_xess=$(printf '%s\n' "$names" | grep -m 1 -E '(^|/)bin/libxess\.dll$') || return 1
        pre=${e_xess%libxess.dll}
        printf '%s\n' "$names" | grep -qxF -- "${pre}libxess_fg.dll" && e_xfg=${pre}libxess_fg.dll
        printf '%s\n' "$names" | grep -qxF -- "${pre}libxell.dll" && e_xell=${pre}libxell.dll
        xsrc=$p xsrc_kind=zip
        return 0
    fi
    [ -d "$p" ] || return 1
    for d in "$p" "$p"/*/; do
        d=${d%/}/bin
        [ -f "$d/libxess.dll" ] || continue
        e_xess=$d/libxess.dll
        [ -f "$d/libxess_fg.dll" ] && e_xfg=$d/libxess_fg.dll
        [ -f "$d/libxell.dll" ] && e_xell=$d/libxell.dll
        xsrc=${p%/} xsrc_kind=folder
        return 0
    done
    return 1
}
find_xess() {
    local d f
    if [ -n "$xess_arg" ]; then
        xess_from "$xess_arg" && return 0
        note "No Intel XeSS files found in $xess_arg"
        return 1
    fi
    while IFS= read -r d; do
        while IFS= read -r f; do xess_from "$f" && return 0; done \
            < <(find "$d" -maxdepth 1 -type f -name 'XeSS_SDK*.zip' -printf '%T@ %p\n' | sort -rn | cut -d ' ' -f 2-)
        while IFS= read -r f; do xess_from "$f" && return 0; done \
            < <(find "$d" -mindepth 1 -maxdepth 1 -type d -name 'XeSS_SDK*' | LC_ALL=C sort)
    done < <(downloads_dirs)
    return 1
}
explain_xess() {
    note "Intel XeSS is optional, and DLSS works without Intel's files. They are not included with this mod. To add them:"
    note "1. Download Intel's XeSS SDK 3.0.2 from $xess_url"
    note "   Your browser saves it as XeSS_SDK_3.0.2.zip (about 77 MB)."
    note "2. Leave the zip in your Downloads folder and run this again."
    note "Or copy these three files by hand from the bin folder in that zip to $xess_dir in the game folder:"
    note "   libxess.dll"
    note "   libxess_fg.dll (XeSS frame generation)"
    note "   libxell.dll (XeSS frame generation)"
    note "By downloading them you accept Intel's licence for those files."
}
step_xess() {
    local n names=()
    if ! find_xess; then
        if [ -f "$game/$xess_dir/libxess.dll" ]; then
            note "Intel's XeSS files are already in place."
            [ -f "$game/$xess_dir/libxess_fg.dll" ] && [ -f "$game/$xess_dir/libxell.dll" ] ||
                note "libxess_fg.dll or libxell.dll is missing, so XeSS frame generation is unavailable."
            st_xess='already done'
            return
        fi
        explain_xess
        st_xess=skipped
        return
    fi
    note "Using your download $xsrc"
    st_xess=failed
    # Frame generation needs both of its files, so they are copied as a pair or not at all. All of them are checked
    # before any is put in place, so a bad download changes nothing.
    names=(libxess.dll)
    fetch_dll "$xsrc_kind" "$xsrc" "$e_xess" libxess.dll &&
        if [ -n "$e_xfg" ] && [ -n "$e_xell" ]; then
            names+=(libxess_fg.dll libxell.dll)
            fetch_dll "$xsrc_kind" "$xsrc" "$e_xfg" libxess_fg.dll && fetch_dll "$xsrc_kind" "$xsrc" "$e_xell" libxell.dll
        fi
    if [ $? != 0 ]; then
        for n in "${names[@]}"; do rm -f -- "$(part_of "$n")"; done
        note "Nothing was copied from this download."
        return
    fi
    if keep_newer_set "$xess_dir" "${names[@]}"; then st_xess='kept newer'; return; fi
    for n in "${names[@]}"; do place_dll "$n" "$xess_dir/$n" || return; done
    if [ ${#names[@]} = 3 ]; then
        note "Copied Intel's three XeSS files."
    else
        note "Copied libxess.dll. This download lacks libxess_fg.dll or libxell.dll, so XeSS frame generation is unavailable."
    fi
    st_xess=done
}

# ---- what a vendor DLL is -------------------------------------------------------------------------------------
# The header, version and signature block are read by offset, a few bytes at a time. Every offset is checked against
# the file's size and every count is capped, so a damaged file is refused rather than read past its end.
pe_num() { # pe_num FILE OFFSET BYTES: a little-endian number of 2 or 4 bytes, or fail when it lies outside the file
    [ "$2" -ge 0 ] && [ $(($2 + $3)) -le "$pe_size" ] || return 1
    od -An -v --endian=little -tu"$3" -j "$2" -N "$3" -- "$1" | tr -d ' \n'
}
pe_nums() { # pe_nums FILE OFFSET COUNT: COUNT 4-byte numbers from OFFSET, space separated
    [ "$2" -ge 0 ] && [ $(($2 + 4 * $3)) -le "$pe_size" ] || return 1
    od -An -v --endian=little -tu4 -j "$2" -N $((4 * $3)) -- "$1" | tr -s ' \n' ' '
}
pe_offset() { # pe_offset RVA: the file offset of an address in the image, from the section table in pe_secs
    local i va size
    for ((i = 0; i + 9 < ${#pe_secs[@]}; i += 10)); do
        va=${pe_secs[i + 3]} size=${pe_secs[i + 2]}
        [ "${pe_secs[i + 4]}" -gt "$size" ] && size=${pe_secs[i + 4]}
        if [ "$1" -ge "$va" ] && [ "$1" -lt $((va + size)) ]; then echo $((pe_secs[i + 5] + $1 - va)); return 0; fi
    done
    return 1
}
pe_dir_entry() { # pe_dir_entry FILE DIR WHICH: from the resource directory at file offset DIR, the offset field of the
    # entry with id WHICH, or of the first entry when WHICH is "first"
    local n e i
    n=$(pe_num "$1" $(($2 + 12)) 2) && i=$(pe_num "$1" $(($2 + 14)) 2) || return 1
    n=$((n + i))
    [ "$n" -ge 1 ] && [ "$n" -le 64 ] || return 1
    read -r -a e <<< "$(pe_nums "$1" $(($2 + 16)) $((2 * n)))" || return 1
    for ((i = 0; i + 1 < ${#e[@]}; i += 2)); do
        if [ "$3" = first ] || [ "${e[i]}" = "$3" ]; then echo "${e[i + 1]}"; return 0; fi
    done
    return 1
}
pe_info() { # pe_info FILE: sets pe_bad (notdll, short, nover, or empty when all was read), pe_ver, pe_company,
    # pe_product, pe_desc, pe_orig, pe_cert_off and pe_cert_len
    local f=$1 h opt nsec optsz chars ndd rva rsz base off sig ms ls text
    pe_bad=notdll pe_ver='' pe_company='' pe_product='' pe_desc='' pe_orig='' pe_cert_off=0 pe_cert_len=0 pe_secs=()
    pe_size=$(stat -c %s -- "$f" 2> /dev/null) || return 1
    [ "$(pe_num "$f" 0 2)" = 23117 ] || return 1                          # MZ
    h=$(pe_num "$f" 60 4) || return 1
    [ "$(pe_num "$f" "$h" 4)" = 17744 ] || return 1                       # PE\0\0
    [ "$(pe_num "$f" $((h + 4)) 2)" = 34404 ] || return 1                 # 64-bit x86 (0x8664)
    nsec=$(pe_num "$f" $((h + 6)) 2) && optsz=$(pe_num "$f" $((h + 20)) 2) && chars=$(pe_num "$f" $((h + 22)) 2) || return 1
    [ $((chars & 0x2000)) != 0 ] || return 1                              # a DLL, not a program
    opt=$((h + 24))
    [ "$(pe_num "$f" "$opt" 2)" = 523 ] || return 1                       # PE32+ (0x20b)
    [ "$nsec" -ge 1 ] && [ "$nsec" -le 96 ] && [ "$optsz" -ge 152 ] || return 1
    pe_bad=short
    ndd=$(pe_num "$f" $((opt + 108)) 4) || return 1
    [ "$ndd" -ge 5 ] || { pe_bad=nover; return 1; }
    read -r -a pe_secs <<< "$(pe_nums "$f" $((opt + optsz)) $((10 * nsec)))"
    [ "${#pe_secs[@]}" = $((10 * nsec)) ] || return 1
    pe_cert_off=$(pe_num "$f" $((opt + 144)) 4) && pe_cert_len=$(pe_num "$f" $((opt + 148)) 4) || return 1
    rva=$(pe_num "$f" $((opt + 128)) 4) && rsz=$(pe_num "$f" $((opt + 132)) 4) || return 1
    pe_bad=nover
    [ "$rva" -gt 0 ] && [ "$rsz" -gt 0 ] || return 1
    base=$(pe_offset "$rva") || return 1
    pe_bad=short
    off=$(pe_dir_entry "$f" "$base" 16) || { [ "$base" -lt "$pe_size" ] && pe_bad=nover; return 1; }
    [ $((off & 0x80000000)) != 0 ] || return 1
    off=$(pe_dir_entry "$f" $((base + (off & 0x7FFFFFFF))) first) || return 1
    [ $((off & 0x80000000)) != 0 ] || return 1
    off=$(pe_dir_entry "$f" $((base + (off & 0x7FFFFFFF))) first) || return 1
    [ $((off & 0x80000000)) = 0 ] || return 1
    rva=$(pe_num "$f" $((base + off)) 4) && rsz=$(pe_num "$f" $((base + off + 4)) 4) || return 1
    [ "$rsz" -ge 92 ] && [ "$rsz" -le 65536 ] || { pe_bad=nover; return 1; }
    off=$(pe_offset "$rva") || return 1
    [ $((off + rsz)) -le "$pe_size" ] || return 1
    sig=$(pe_num "$f" $((off + 40)) 4) || return 1
    [ "$sig" = 4277077181 ] || { pe_bad=nover; return 1; }                # 0xFEEF04BD
    ms=$(pe_num "$f" $((off + 48)) 4) && ls=$(pe_num "$f" $((off + 52)) 4) || return 1
    pe_ver=$((ms >> 16)).$((ms & 65535)).$((ls >> 16)).$((ls & 65535))
    # The version strings are UTF-16: each key is followed, after zero padding, by its value.
    text=$(od -An -v --endian=little -tu2 -j "$off" -N "$rsz" -- "$f" |
        LC_ALL=C awk '{ for (i = 1; i <= NF; i++) if ($i >= 32 && $i < 127) printf "%c", $i; else printf "\n" }')
    pe_company=$(pe_string CompanyName) pe_product=$(pe_string ProductName)
    pe_desc=$(pe_string FileDescription) pe_orig=$(pe_string OriginalFilename)
    pe_bad=''
}
pe_string() { printf '%s\n' "$text" | LC_ALL=C awk -v k="$1" 'f && length($0) { print; exit } $0 == k { f = 1 }'; }

vendor_of() { case $1 in nvngx_*) echo NVIDIA ;; amd_*) echo AMD ;; *) echo Intel ;; esac; }
check_kind() { # check_kind FILE NAME: true when FILE is the vendor's own NAME; else the reason is in $why
    local f=$1 name=$2 who company org ok=1
    who=$(vendor_of "$name")
    case $who in
        NVIDIA) company=NVIDIA org='NVIDIA Corporation' ;;
        AMD) company='Advanced Micro Devices' org=$company ;;
        *) company='Intel Corporation' org=$company ;;
    esac
    why=''
    pe_info "$f"
    case $pe_bad in
        notdll) why='it is not a 64-bit Windows DLL' ;;
        short) why='it is cut short' ;;
    esac
    if [ -z "$why" ] && [ -z "$pe_ver" ]; then why='it has no version information'; fi
    if [ -z "$why" ]; then
        case $pe_company in "$company"*) ;; *) why="it is not $who's file" ;; esac
    fi
    if [ -z "$why" ]; then
        case $name in
            nvngx_dlssg.dll) case $pe_product in *DLSS-G*) ;; *) ok=0 ;; esac ;;
            nvngx_dlss.dll) case $pe_product in *DLSS-G* | *'Ray Reconstruction'*) ok=0 ;; esac ;;
            amd_fidelityfx_loader_dx12.dll) case $pe_desc in *Loader*) ;; *) ok=0 ;; esac ;;
            amd_fidelityfx_upscaler_dx12.dll) case $pe_desc in *Upscaler*) ;; *) ok=0 ;; esac ;;
            amd_fidelityfx_framegeneration_dx12.dll) case $pe_desc in *'Frame Generation'*) ;; *) ok=0 ;; esac ;;
            *) [ "$pe_orig" = "$name" ] || ok=0 ;;
        esac
        [ $ok = 1 ] || why="it is a different $who file under this name"
    fi
    if [ -z "$why" ] && [ "$pe_size" -le "$(min_size "$name")" ]; then why='it is cut short'; fi
    if [ -z "$why" ]; then
        if [ "$pe_cert_len" -lt 8 ]; then
            why='it has no digital signature'
        elif [ "$pe_cert_len" -gt "$max_cert" ]; then
            why='its digital signature is too large to check'
        elif [ $((pe_cert_off + pe_cert_len)) -gt "$pe_size" ]; then
            why='it is cut short'
        elif [ "$(pe_num "$f" $((pe_cert_off + 6)) 2)" != 2 ]; then
            why='it has no digital signature'
        elif ! tail -c +$((pe_cert_off + 1)) -- "$f" | head -c "$pe_cert_len" | grep -qaF -- "$org"; then
            why="its digital signature does not name $who"
        fi
    fi
    [ -z "$why" ]
}
ver_cmp() { # ver_cmp A B: -1, 0 or 1 as version A is lower than, equal to or higher than B, number by number
    local IFS=. a b i
    read -r -a a <<< "$1"
    read -r -a b <<< "$2"
    for i in 0 1 2 3; do
        if [ "${a[i]:-0}" -lt "${b[i]:-0}" ]; then echo -1; return; fi
        if [ "${a[i]:-0}" -gt "${b[i]:-0}" ]; then echo 1; return; fi
    done
    echo 0
}
version_of() { # version_of FILE: its file version, or nothing when it has none that can be read
    [ -f "$1" ] || return 0
    pe_info "$1"
    printf '%s' "$pe_ver"
}

# ---- update -------------------------------------------------------------------------------------------------------
# The eight vendor files update knows, in the order they are listed. AMD's and Intel's three are a set each.
targets=(nvngx_dlss.dll nvngx_dlssg.dll amd_fidelityfx_upscaler_dx12.dll amd_fidelityfx_framegeneration_dx12.dll
    amd_fidelityfx_loader_dx12.dll libxess.dll libxess_fg.dll libxell.dll)
amd_set=(amd_fidelityfx_upscaler_dx12.dll amd_fidelityfx_framegeneration_dx12.dll amd_fidelityfx_loader_dx12.dll)
intel_set=(libxess.dll libxess_fg.dll libxell.dll)
list_rel='ffxv_dlss/last_update.txt'
kept='.before_update'                 # suffix of the copy update keeps of each file it replaces
max_cert=1048576   # largest signature block read; the vendors' measure 10 to 12 KB
dest_of() {
    case $1 in
        nvngx_dlss.dll) echo ffxv_dlss/nvngx_dlss.dll ;;
        nvngx_dlssg.dll) echo ffxv_dlss/streamline/nvngx_dlssg.dll ;;
        amd_*) echo "$fsr_dir/$1" ;;
        *) echo "$xess_dir/$1" ;;
    esac
}
is_target_path() { local t; for t in "${targets[@]}"; do [ "$1" = "$(dest_of "$t")" ] && return 0; done; return 1; }
match_name() { # match_name FILE_NAME: true when it is a target's name, or that name saved again as "name (N).dll";
    # sets m_name and m_copy (the " (N)" part)
    local t
    for t in "${targets[@]}"; do
        if [ "$1" = "$t" ]; then m_name=$t m_copy=''; return 0; fi
        if [[ $1 =~ ^(.+)\ \(([0-9]{1,3})\)\.dll$ ]] && [ "${BASH_REMATCH[1]}.dll" = "$t" ]; then
            m_name=$t m_copy=" (${BASH_REMATCH[2]})"
            return 0
        fi
    done
    return 1
}
dev_path() { # dev_path PATH: true when a folder in PATH holds a development, debug or ARM build
    local d IFS=/
    set -f
    for d in $(dirname -- "$1"); do
        case ${d,,} in dev | development | debug | *arm64* | *aarch64*) set +f; return 0 ;; esac
    done
    set +f
    return 1
}
# Candidates: c_kind (zip or file), c_zip, c_path (entry in the zip, or the file), c_name, c_group (one folder of one
# download, with the copy suffix), c_from (what the player is told).
c_kind=() c_zip=() c_path=() c_name=() c_group=() c_from=()
declare -A dev_skipped=()
add_candidate() { # add_candidate KIND ZIP PATH RELATIVE_PATH FROM
    match_name "${3##*/}" || return 0
    if dev_path "$4"; then dev_skipped[$m_name]=$5; return 0; fi
    c_kind+=("$1") c_zip+=("$2") c_path+=("$3") c_name+=("$m_name") c_from+=("$5")
    c_group+=("$2|$(dirname -- "$4")|$m_copy")
}
scan_zip() { # scan_zip ZIP
    local names e
    if [ "$zip_tool" = none ]; then
        note "Skipped $1: there is no unzip, bsdtar or python3 here to open zips. Extract it and use --from."
        return
    fi
    if ! names=$(zip_list "$1"); then note "Skipped $1: it could not be opened as a zip."; return; fi
    while IFS= read -r e; do
        case $e in */ | '') continue ;; esac
        add_candidate zip "$1" "$e" "$e" "$1"
    done <<< "$names"
}
scan_file() { # scan_file FILE RELATIVE_PATH: a zip, or a loose DLL
    case ${1,,} in
        *.zip) scan_zip "$1" ;;
        *.dll) add_candidate file '' "$1" "$2" "$1" ;;
    esac
}
from_tail() { # from_tail DIR: DIR's own name and a slash, after its parent's name when that names an ARM build
    local p b a
    p=$(cd -- "$1" 2> /dev/null && pwd) || p=${1%/}   # a relative path such as "." names the folder it stands for
    b=${p##*/}
    p=${p%/*}
    a=${p##*/}
    case ${a,,} in *arm64* | *aarch64*) printf '%s/%s/' "$a" "$b" ;; *) printf '%s/' "$b" ;; esac
}
find_candidates() {
    local d f pre
    if [ -n "$from_arg" ]; then
        if [ -f "$from_arg" ]; then
            note "Looking in $from_arg"
            pre=$(from_tail "$(dirname -- "$from_arg")")
            scan_file "$from_arg" "$pre${from_arg##*/}"
        elif [ -d "$from_arg" ]; then
            note "Looking in $from_arg"
            d=${from_arg%/}
            pre=$(from_tail "$d")
            while IFS= read -r f; do scan_file "$d/$f" "$pre$f"; done \
                < <(find "$d" -mindepth 1 -maxdepth 6 -type f \( -iname '*.dll' -o -iname '*.zip' \) -printf '%P\n' | LC_ALL=C sort)
        else
            note "$from_arg was not found."
            return 1
        fi
        return 0
    fi
    while IFS= read -r d; do
        note "Looking in $d"
        while IFS= read -r f; do scan_file "$d/$f" "$f"; done \
            < <(find "$d" -mindepth 1 -maxdepth 1 -type f \( -iname '*.dll' -o -iname '*.zip' \) -printf '%P\n' | LC_ALL=C sort)
    done < <(downloads_dirs)
    return 0
}
drop() { [ -n "$1" ] && rm -f -- "$1"; }   # drop FILE: deletes one temporary file this script made
fetch_candidate() { # fetch_candidate INDEX OUT: the candidate's bytes to OUT
    if [ "${c_kind[$1]}" = zip ]; then
        zip_extract "${c_zip[$1]}" "${c_path[$1]}" "$2"
    else
        cp -f --no-preserve=mode -- "${c_path[$1]}" "$2" 2> /dev/null
    fi || { drop "$2"; note "Could not read ${c_name[$1]} from ${c_from[$1]}"; return 1; }
}
# The chosen download of each target waits in its part file; its version is in best_ver[name], its source in best_from.
declare -A best_ver=() best_from=()
stage_failed=0   # 1 once a download could not be copied into the game folder: the run stops before any change
choose_one() { # choose_one NAME: the highest version among the candidates for NAME that pass the kind check
    local i tmp found=0
    [ $stage_failed = 0 ] || return 0
    tmp=$(part_of "cand.$1")
    for i in "${!c_name[@]}"; do
        [ "${c_name[$i]}" = "$1" ] || continue
        found=1
        fetch_candidate "$i" "$tmp" || { stage_failed=1; return 0; }
        if ! check_kind "$tmp" "$1"; then
            note "Refused $1 from ${c_from[$i]}: $why."
            drop "$tmp"
            continue
        fi
        if [ -z "${best_ver[$1]:-}" ] || [ "$(ver_cmp "$pe_ver" "${best_ver[$1]}")" = 1 ]; then
            mv -f -- "$tmp" "$(part_of "$1")" || { drop "$tmp"; stage_failed=1; return 0; }
            best_ver[$1]=$pe_ver best_from[$1]=${c_from[$i]}
        else
            drop "$tmp"
        fi
    done
    if [ $found = 0 ] && [ -n "${dev_skipped[$1]:-}" ]; then
        note "Refused $1 from ${dev_skipped[$1]}: it comes from a development or ARM folder."
    fi
}
set_cmp() { # set_cmp "A1 A2 A3" "B1 B2 B3": ver_cmp over the first file that differs
    local a b i c
    read -r -a a <<< "$1"
    read -r -a b <<< "$2"
    for i in "${!a[@]}"; do
        c=$(ver_cmp "${a[i]}" "${b[i]:-0}")
        [ "$c" = 0 ] || { echo "$c"; return; }
    done
    echo 0
}
choose_set() { # choose_set LABEL NAME...: the highest set of three files from one folder of one download
    local label=$1 g i n k best='' good partial=() groups=() idx vers v
    shift
    [ $stage_failed = 0 ] || return 0
    local -A at=()
    for i in "${!c_name[@]}"; do
        for n in "$@"; do
            [ "${c_name[$i]}" = "$n" ] || continue
            g=${c_group[$i]}
            [ -n "${at[$g/$n]:-}" ] && continue
            at[$g/$n]=$i
            case " ${groups[*]-} " in *" $g "*) ;; *) groups+=("$g") ;; esac
        done
    done
    for g in "${groups[@]}"; do
        idx=() vers=''
        for n in "$@"; do [ -n "${at[$g/$n]:-}" ] && idx+=("${at[$g/$n]}"); done
        i=${idx[0]}
        if [ ${#idx[@]} -lt 3 ]; then
            partial+=("$label: ${c_from[$i]} has only ${#idx[@]} of the 3 files, so nothing was copied from it.")
            continue
        fi
        good=1
        for i in "${idx[@]}"; do
            n=${c_name[$i]}
            fetch_candidate "$i" "$(part_of "cand.$n")" || { stage_failed=1; return 0; }
            if ! check_kind "$(part_of "cand.$n")" "$n"; then
                note "Refused $n from ${c_from[$i]}: $why."
                note "$label: nothing was copied from ${c_from[$i]}."
                good=0
                break
            fi
            vers+="${vers:+ }$pe_ver"
        done
        if [ $good = 1 ] && { [ -z "$best" ] || [ "$(set_cmp "$vers" "$best")" = 1 ]; }; then
            best=$vers
            read -r -a v <<< "$vers"
            k=0
            for n in "$@"; do
                mv -f -- "$(part_of "cand.$n")" "$(part_of "$n")"
                best_ver[$n]=${v[k]} best_from[$n]=${c_from[$i]}
                k=$((k + 1))
            done
        fi
        for n in "$@"; do drop "$(part_of "cand.$n")"; done
    done
    if [ -z "$best" ]; then
        for k in "${partial[@]}"; do note "$k"; done
    fi
    for n in "$@"; do
        if [ -z "${best_ver[$n]:-}" ] && [ -n "${dev_skipped[$n]:-}" ]; then
            note "Refused $n from ${dev_skipped[$n]}: it comes from a development or ARM folder."
        fi
    done
}
# Change list: ch_name, ch_mode (replaced or added) and ch_old (the version it had), filled by plan_item.
ch_name=() ch_mode=() ch_old=()
plan_item() { # plan_item LABEL NAME...: compares the chosen download with the files in place and plans the change
    local label=$1 n c newer=0 older=0 o=() oldv
    shift
    [ -n "${best_ver[$1]:-}" ] || { up_to_date "$@"; return; }
    for n in "$@"; do
        oldv=$(version_of "$game/$(dest_of "$n")")
        o+=("$oldv")
        if [ -z "$oldv" ]; then c=1; else c=$(ver_cmp "${best_ver[$n]}" "$oldv"); fi
        [ "$c" = 1 ] && newer=1
        [ "$c" = -1 ] && older=1
    done
    if [ $older = 1 ] && [ $allow_older = 0 ]; then
        c=0
        for n in "$@"; do
            oldv=${o[c]}
            c=$((c + 1))
            [ -n "$oldv" ] && [ "$(ver_cmp "${best_ver[$n]}" "$oldv")" = -1 ] || continue
            if [ $# = 1 ]; then
                note "Left $n alone: the copy in place ($oldv) is newer than the download (${best_ver[$n]})."
                note "To put the older one in anyway, run update with --allow-older."
            else
                note "Left $label alone: $n in place ($oldv) is newer than the download's (${best_ver[$n]})."
                note "To put the older ones in anyway, run update with --allow-older."
            fi
            break
        done
        return
    fi
    if [ $newer = 0 ] && [ $older = 0 ]; then
        up_to_date "$@"
        return
    fi
    c=0
    for n in "$@"; do
        ch_name+=("$n") ch_old+=("${o[c]}")
        if [ -f "$game/$(dest_of "$n")" ]; then ch_mode+=(replaced); else ch_mode+=(added); fi
        c=$((c + 1))
    done
}
up_to_date() { # up_to_date NAME...: says what is in place
    local n v
    for n in "$@"; do
        v=$(version_of "$game/$(dest_of "$n")")
        if [ -n "$v" ]; then
            note "$n: $v is in place and nothing newer was found."
        elif [ -f "$game/$(dest_of "$n")" ]; then
            note "$n: in place, and nothing newer was found."
        else
            note "$n: not in place, and no download of it was found."
        fi
    done
}
update_parts() { # deletes every temporary name update may have made
    local n
    [ -n "${game:-}" ] || return 0
    for n in "${targets[@]}"; do
        drop "$(part_of "$n")"
        drop "$(part_of "cand.$n")"
        drop "$game/$(dest_of "$n")$kept$part"
    done
    drop "$game/$list_rel$part"
    drop "$game/$record_rel$part"
}
put_back() { # put_back FIRST LAST: undoes the renames of changes FIRST..LAST
    local k d
    for ((k = $1; k <= $2; k++)); do
        d=$game/$(dest_of "${ch_name[k]}")
        if [ "${ch_mode[k]}" = added ]; then
            drop "$d"
        elif [ -f "$d$kept" ]; then
            cp -f --no-preserve=mode -- "$d$kept" "$d$part" && mv -fT -- "$d$part" "$d"
        fi
    done
}
prep_failed() { # prep_failed: before any file was replaced; the record and folders as they were, nothing counts as done
    local d
    # the copy of the record goes back only when it was made in full; a cut copy is dropped and the record left alone
    if [ "$rec_saved" = 1 ]; then mv -fT -- "$game/$record_rel$part" "$game/$record_rel"; else drop "$game/$record_rel$part"; fi
    drop "$game/$record_rel.new"   # record_add's temporary name
    for d in "${made_dirs[@]}"; do rmdir -- "$game/$d" 2> /dev/null; done
    for k in "${!ch_name[@]}"; do ch_mode[k]=failed; done
}
publish_failed() { # publish_failed HAD_LIST: no file was replaced, and the earlier update's undo is gone
    for k in "${!ch_name[@]}"; do ch_mode[k]=failed; done
    [ "$1" = 1 ] && note "No file was changed, but the undo for the earlier update is no longer available."
}
made_dirs=() rec_saved=0
apply_changes() {
    local k n d err entries=() first last rc=0 item had_list=0
    entries=("$list_rel")
    for k in "${!ch_name[@]}"; do
        d=$(dest_of "${ch_name[k]}")
        entries+=("$d")
        [ "${ch_mode[k]}" = replaced ] && entries+=("$d$kept")
    done
    # (a) the record names every file this run may leave behind, before any of them exists; a copy of it, and the
    # folders this run makes, let a failure before (c) leave the game folder as it was
    rec_saved=0
    err=$(cp -f --no-preserve=mode -- "$game/$record_rel" "$game/$record_rel$part" 2>&1) ||
        { copy_failed "$record_rel" "$err"; prep_failed; return 4; }
    rec_saved=1
    made_dirs=()
    for k in "${!ch_name[@]}"; do
        d=$(dirname -- "$(dest_of "${ch_name[k]}")")
        while [ "$d" != . ] && [ ! -d "$game/$d" ]; do made_dirs+=("$d"); d=$(dirname -- "$d"); done
    done
    err=$({ for k in "${!ch_name[@]}"; do mkdir -p -- "$(dirname -- "$game/$(dest_of "${ch_name[k]}")")" || exit 1; done
        record_add "${entries[@]}"; } 2>&1) || { copy_failed "$record_rel" "$err"; prep_failed; return 4; }
    # (b) copies of the files about to be replaced, and the new list, under temporary names
    for k in "${!ch_name[@]}"; do
        [ "${ch_mode[k]}" = replaced ] || continue
        d=$game/$(dest_of "${ch_name[k]}")
        err=$(cp -f --no-preserve=mode -- "$d" "$d$kept$part" 2>&1) || { copy_failed "${ch_name[k]}" "$err"; prep_failed; return 4; }
    done
    err=$({ for k in "${!ch_name[@]}"; do printf '%s %s\n' "${ch_mode[k]}" "$(dest_of "${ch_name[k]}")"; done > "$game/$list_rel$part"; } 2>&1) ||
        { copy_failed "$list_rel" "$err"; prep_failed; return 4; }
    # (c) From here on only renames and deletions remain, which need no free space. The earlier update's list goes
    # first: if a kept copy cannot be replaced, there is no undo rather than one that would mix two updates.
    drop "$game/$record_rel$part"
    if [ -f "$game/$list_rel" ]; then had_list=1; drop "$game/$list_rel"; fi
    for k in "${!ch_name[@]}"; do
        [ "${ch_mode[k]}" = replaced ] || continue
        d=$game/$(dest_of "${ch_name[k]}")
        err=$(mv -fT -- "$d$kept$part" "$d$kept" 2>&1) || { copy_failed "${ch_name[k]}" "$err"; publish_failed $had_list; return 4; }
    done
    err=$(mv -fT -- "$game/$list_rel$part" "$game/$list_rel" 2>&1) || { copy_failed "$list_rel" "$err"; publish_failed $had_list; return 4; }
    for n in "${targets[@]}"; do   # one step only: kept copies from an earlier update go
        d=$(dest_of "$n")
        grep -qxF -- "replaced $d" "$game/$list_rel" || drop "$game/$d$kept"
    done
    # (d) the new files into place, one item at a time (each NVIDIA file, AMD's set, Intel's set); an item that cannot
    # be completed is put back, so a set is never left mixed
    first=0
    while [ "$first" -lt ${#ch_name[@]} ]; do
        item=$(vendor_of "${ch_name[first]}")
        last=$first
        if [ "$item" != NVIDIA ]; then
            while [ $((last + 1)) -lt ${#ch_name[@]} ] && [ "$(vendor_of "${ch_name[last + 1]}")" = "$item" ]; do last=$((last + 1)); done
        fi
        for ((k = first; k <= last; k++)); do
            n=${ch_name[k]}
            if ! err=$(mv -fT -- "$(part_of "$n")" "$game/$(dest_of "$n")" 2>&1); then
                copy_failed "$n" "$err"
                put_back "$first" $((k - 1))
                for ((k = first; k <= last; k++)); do ch_mode[k]=failed; done
                rc=4
                break
            fi
        done
        first=$((last + 1))
    done
    return $rc
}
show_versions() {
    local n v
    say ""
    say "Versions now in place"
    for n in "${targets[@]}"; do
        v=$(version_of "$game/$(dest_of "$n")")
        if [ -n "$v" ]; then
            note "$n $v"
        elif [ -f "$game/$(dest_of "$n")" ]; then
            note "$n (version not readable)"
        else
            note "$n not in place"
        fi
    done
}
need_record() {
    [ -f "$game/$record_rel" ] && return 0
    note "Run install first: no install record was found in this game folder."
    return 5
}
do_update() {
    steps=("Game folder" "Downloads" "Files")
    local k rc=0 changed=0 fg=0
    say "Lumenveil: update"
    if game_running; then say ""; note "Close FINAL FANTASY XV first, then run this again."; return 2; fi
    step "Game folder"
    choose_game || return 3
    need_record || return 5
    trap update_parts EXIT
    step "Downloads"
    find_candidates || return 1
    choose_one nvngx_dlss.dll
    choose_one nvngx_dlssg.dll
    choose_set "AMD FSR" "${amd_set[@]}"
    choose_set "Intel XeSS" "${intel_set[@]}"
    if [ $stage_failed = 1 ]; then
        note "Nothing was changed. If the game's drive is full, free some space; if that download is damaged, download it again. Then run update again."
        return 4
    fi
    step "Files"
    plan_item nvngx_dlss.dll nvngx_dlss.dll
    plan_item nvngx_dlssg.dll nvngx_dlssg.dll
    plan_item "AMD FSR" "${amd_set[@]}"
    plan_item "Intel XeSS" "${intel_set[@]}"
    if [ ${#ch_name[@]} -gt 0 ]; then
        apply_changes || rc=$?
        for k in "${!ch_name[@]}"; do
            case ${ch_mode[k]} in
                replaced) note "${ch_name[k]}: updated from ${ch_old[k]:-an unknown version} to ${best_ver[${ch_name[k]}]}." ;;
                added) note "${ch_name[k]}: added, version ${best_ver[${ch_name[k]}]}." ;;
                *) continue ;;
            esac
            changed=1
            [ "${ch_name[k]}" = nvngx_dlssg.dll ] && fg=1
        done
    fi
    update_parts
    show_versions
    if [ $changed = 1 ]; then
        say ""
        say "The previous files are kept. To go back to them, run undo-update.sh on Linux or undo-update.bat on Windows."
        [ $fg = 1 ] && say "If frame generation stops working after this update, run the undo."
    fi
    return $rc
}
do_undo() {
    steps=("Game folder" "Undo")
    local l mode p rc=0 list left=()
    say "Lumenveil: undo the last update"
    if game_running; then say ""; note "Close FINAL FANTASY XV first, then run this again."; return 2; fi
    step "Game folder"
    choose_game || return 3
    need_record || return 5
    step "Undo"
    list=$game/$list_rel
    if [ ! -f "$list" ]; then note "Nothing to undo."; return 0; fi
    while IFS= read -r l || [ -n "$l" ]; do
        l=${l%$'\r'}
        [ -n "$l" ] || continue
        mode=${l%% *} p=${l#* }
        if { [ "$mode" != replaced ] && [ "$mode" != added ]; } || ! is_target_path "$p"; then
            note "Skipped \"$l\": not one of the files update changes."
            continue
        fi
        if [ "$mode" = added ]; then
            drop "$game/$p" && note "Removed ${p##*/}, which the update added."
            rmdir -- "$(dirname -- "$game/$p")" 2> /dev/null   # the folder too, when the update made it and it is empty
        elif [ ! -f "$game/$p$kept" ]; then
            note "The kept copy of ${p##*/} is missing, so ${p##*/} was left as it is."
            rc=4
        elif mv -fT -- "$game/$p$kept" "$game/$p"; then
            note "Put back the previous ${p##*/} ($(version_of "$game/$p"))."
        else
            left+=("$l")
            rc=4
        fi
    done < "$list"
    if [ ${#left[@]} = 0 ]; then
        drop "$list"
    elif printf '%s\n' "${left[@]}" > "$list$part" && mv -fT -- "$list$part" "$list"; then
        note "Close anything that may be using these files, then run the undo again to finish."
    else
        drop "$list$part"
    fi
    [ $rc = 0 ] && note "Done: the files are as they were before the last update."
    return $rc
}

# ---- install --------------------------------------------------------------------------------------------------
copy_failed() { # copy_failed FILE MESSAGE
    note "Could not copy $1: $2"
    note "Check that you can write to the game folder, then run this again."
}
install_files() {
    local f err files=()
    if [ ! -d "$payload" ]; then
        note "The release's game folder is missing. Extract the whole zip, then run this again."
        return 4
    fi
    mapfile -t files < <(cd "$payload" && find . -type f -printf '%P\n' | LC_ALL=C sort)
    local foreign=0
    if [ -f "$game/dxgi.dll" ] && ! grep -qaF -- "$mark" "$game/dxgi.dll"; then
        if [ -e "$game/dxgi.dll$keep" ]; then
            note "The game folder holds another tool's dxgi.dll, and dxgi.dll$keep is there too."
            note "Move one of them out of the game folder, then run this again. Nothing was changed."
            return 4
        fi
        foreign=1
    fi
    err=$(mkdir -p -- "$game/ffxv_dlss" 2>&1) || { copy_failed ffxv_dlss "$err"; return 4; }
    err=$(record_add "${files[@]}" "$record_rel" 2>&1) || { copy_failed "$record_rel" "$err"; return 4; }
    if [ $foreign = 1 ]; then
        err=$(mv -- "$game/dxgi.dll" "$game/dxgi.dll$keep" 2>&1) || { copy_failed dxgi.dll "$err"; return 4; }
        note "Another tool's dxgi.dll was in the game folder. It is kept as dxgi.dll$keep"
        note "and comes back when you uninstall."
    fi
    # Each file goes to a temporary name beside its target and is renamed into place only once it is whole, so a
    # copy cut short (a full disk) never leaves a broken file under a real name.
    for f in "${files[@]}"; do
        err=$({ mkdir -p -- "$game/$(dirname -- "$f")" && cp -f --no-preserve=mode -- "$payload/$f" "$game/$f$part" &&
            mv -f -- "$game/$f$part" "$game/$f"; } 2>&1) ||
            { rm -f -- "$game/$f$part"; copy_failed "$f" "$err"; return 4; }
    done
    note "Copied ${#files[@]} files."
}
step_fps() {
    local f n acct files=()
    mapfile -t files < <(settings_files)
    if [ ${#files[@]} = 0 ]; then
        note "Skipped: the game's settings file was not found. Start the game once, change any graphics"
        note "option, then run this again, or set MaxFramerate=240 by hand in"
        note "<Steam library>/$prefix_docs/$settings_rel/<number>/savestorage/GraphicsConfig.ini"
        st_fps=skipped
        return
    fi
    st_fps='already done'
    for f in "${files[@]}"; do
        acct=$(account_of "$f")
        n=$(ini_get "$f" MaxFramerate)
        if [ -z "$n" ]; then
            note "Account $acct: skipped, the file has no MaxFramerate line. Set MaxFramerate=240 by hand in $f"
            st_fps=skipped
        elif at_least "$n" 240; then
            note "Account $acct: already $n fps, left as it is."
        elif [ ! -w "$f" ]; then
            note "Account $acct: failed, the settings file is read-only. Set MaxFramerate=240 by hand in $f"
            st_fps=failed
        elif { [ -e "$f$keep" ] || cp -p -- "$f" "$f$keep"; } && ini_set "$f" MaxFramerate 240; then
            note "Account $acct: raised the game's limit from $n to 240 fps. Your old settings file is kept as"
            note "GraphicsConfig.ini$keep"
            [ "$st_fps" = 'already done' ] && st_fps=done
        else
            note "Account $acct: failed to change the settings file. Set MaxFramerate=240 by hand in $f"
            st_fps=failed
        fi
    done
}
copy_clipboard() {
    if [ -n "${WAYLAND_DISPLAY:-}" ] && command -v wl-copy > /dev/null 2>&1; then
        printf '%s' "$launch" | wl-copy > /dev/null 2>&1
    elif [ -n "${DISPLAY:-}" ] && command -v xclip > /dev/null 2>&1; then
        printf '%s' "$launch" | xclip -selection clipboard > /dev/null 2>&1
    elif [ -n "${DISPLAY:-}" ] && command -v xsel > /dev/null 2>&1; then
        printf '%s' "$launch" | xsel --clipboard --input > /dev/null 2>&1
    else
        return 1
    fi
}
step_launch() {
    note "Steam needs this launch option for the game, so that it loads the mod:"
    say ""
    say "$launch"
    say ""
    if copy_clipboard; then note "It has been copied to the clipboard."; fi
    note "In Steam: right click the game, Properties, General, Launch Options, paste."
    note "If you already have launch options, put WINEDLLOVERRIDES=\"dxgi=n,b\" in front of them and keep one %command%."
}
do_install() {
    steps=("Game folder" "Mod files" "NVIDIA DLSS files" "AMD FSR files" "Intel XeSS files" "Frame rate limit" "Steam launch option")
    local nv launch_no xs
    nv=$(step_no "NVIDIA DLSS files") launch_no=$(step_no "Steam launch option") xs=$(step_no "Intel XeSS files")
    say "Lumenveil: install"
    if game_running; then say ""; note "Close FINAL FANTASY XV first, then run this again."; return 2; fi
    step "Game folder"
    choose_game || return 3
    step "Mod files"
    install_files || return 4
    step "NVIDIA DLSS files"
    step_nvidia
    step "AMD FSR files"
    step_fsr
    step "Intel XeSS files"
    step_xess
    step "Frame rate limit"
    step_fps
    step "Steam launch option"
    step_launch
    say ""
    say "Summary"
    note "Game folder: found"
    note "Mod files: done"
    note "NVIDIA DLSS files: $st_nvidia"
    note "AMD FSR files: $st_fsr (optional)"
    note "Intel XeSS files: $st_xess (optional)"
    note "Frame rate limit: $st_fps"
    note "Launch option: set it in Steam (step $launch_no)"
    say ""
    if [ "$st_nvidia" = skipped ] || [ "$st_nvidia" = failed ]; then
        say "Still to do: add NVIDIA's DLSS files (step $nv) and set the launch option in Steam (step $launch_no)."
    else
        say "Still to do: set the launch option in Steam (step $launch_no)."
    fi
    if [ "$st_xess" = skipped ]; then
        say "Still to do, if you want Intel XeSS (optional, step $xs): put XeSS_SDK_3.0.2.zip from $xess_url"
        say "in your Downloads folder and run this again."
    fi
    say "Then start the game and press Insert for the mod's menu."
    say "To remove the mod later, run uninstall.sh."
    return 0
}

# ---- uninstall -------------------------------------------------------------------------------------------------
restore_fps() {
    local b f old now acct r any=0
    while IFS= read -r b; do
        any=1
        f=${b%"$keep"}
        acct=$(account_of "$f")
        old=$(ini_get "$b" MaxFramerate)
        if [ ! -f "$f" ]; then
            note "Account $acct: the settings file is gone, so its backup was left: $b"
            continue
        fi
        now=$(ini_get "$f" MaxFramerate)
        if [ -n "$old" ] && [ "$now" = 240 ]; then
            if [ -w "$f" ] && ini_set "$f" MaxFramerate "$old"; then
                note "Account $acct: set the game's limit back to $old fps."
            else
                note "Account $acct: failed to change the settings file. Set MaxFramerate=$old by hand in $f"
                continue
            fi
        else
            note "Account $acct: your limit is now ${now:-not set}, left as it is."
        fi
        rm -f -- "$b"
    done < <(settings_files "$keep")
    [ $any = 1 ] || note "Nothing to put back."
    while IFS= read -r f; do
        r=$(ini_get "$f" RenderingResolutionRatio)
        if [ -n "$r" ] && [ "$r" != 100 ]; then
            note "The game's render scale is $r%. Set Resolution back to 100% in the game's graphics options"
            note "if you lowered it for DLSS."
        fi
    done < <(settings_files)
}
remove_files() {
    local rec=$game/$record_rel e p d n=0 left dirs=()
    if [ ! -f "$rec" ]; then
        note "No install record found, nothing was removed."
        note "To remove the mod by hand, delete these from the game folder: dxgi.dll (if it is this mod's),"
        note "ffxv_dlss.ini, ffxv_dlss.log and the ffxv_dlss folder."
        return 5
    fi
    while IFS= read -r e; do
        e=${e%$'\r'}
        [ -n "$e" ] && [ "$e" != "$record_rel" ] || continue
        if ! valid_entry "$e"; then note "Skipped $e: not one of the mod's own files."; continue; fi
        p=$game/$e
        if [ "$e" = dxgi.dll ]; then
            if [ -f "$p" ] && grep -qaF -- "$mark" "$p"; then rm -f -- "$p" && n=$((n + 1)); fi
            continue
        fi
        if [ -f "$p" ] || [ -L "$p" ]; then rm -f -- "$p" && n=$((n + 1)); fi
        dirs+=("$(dirname -- "$e")")
    done < "$rec"
    rm -f -- "$rec"
    for p in "$game/ffxv_dlss.ini" "$game/ffxv_dlss.log" "$game"/ffxv_dlss/*.log "$game"/ffxv_dlss/streamline/*.log; do
        if [ -f "$p" ]; then rm -f -- "$p" && n=$((n + 1)); fi
    done
    note "Removed $n files."
    while IFS= read -r d; do
        rmdir -- "$game/$d" 2> /dev/null
    done < <(printf '%s\n' "${dirs[@]}" ffxv_dlss/streamline ffxv_dlss/fonts "$fsr_dir" "$xess_dir" ffxv_dlss | awk -F/ '!seen[$0]++ { print NF "\t" $0 }' | sort -rn | cut -f 2-)
    if [ -d "$game/ffxv_dlss" ]; then
        left=$(cd "$game/ffxv_dlss" && find . -mindepth 1 ! -type d -printf 'ffxv_dlss/%P\n' | LC_ALL=C sort)
        if [ -n "$left" ]; then
            note "These files were not installed by this script and were left alone:"
            printf '%s\n' "$left" | while IFS= read -r p; do note "   $p"; done
        fi
    fi
    if [ -e "$game/dxgi.dll$keep" ]; then
        if [ ! -e "$game/dxgi.dll" ]; then
            mv -- "$game/dxgi.dll$keep" "$game/dxgi.dll" && note "Put the other tool's dxgi.dll back."
        else
            note "dxgi.dll in the game folder is not this mod's, so it and dxgi.dll$keep were both left as they are."
        fi
    fi
    return 0
}
do_uninstall() {
    steps=("Game folder" "Frame rate limit" "Mod files")
    say "Lumenveil: uninstall"
    if game_running; then say ""; note "Close FINAL FANTASY XV first, then run this again."; return 2; fi
    step "Game folder"
    choose_game || return 3
    step "Frame rate limit"
    restore_fps
    step "Mod files"
    remove_files || return $?
    say ""
    say "Done. You can now remove WINEDLLOVERRIDES=\"dxgi=n,b\" from the game's launch options in Steam."
    return 0
}

case $action in
    install) do_install ;;
    uninstall) do_uninstall ;;
    update) do_update ;;
    *) do_undo ;;
esac
exit $?
