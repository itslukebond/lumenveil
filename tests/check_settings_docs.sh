#!/bin/bash
# The installer's settings files come from USERPROFILE\Documents (where the game keeps them), not from the Documents
# folder Windows reports when that folder has moved (OneDrive). Builds a profile tree holding the game's file and a
# moved Documents tree holding an older copy, then runs tests/unit.ps1 -Case docs on scripts/setup.ps1.
# Usage: tests/check_settings_docs.sh [repo folder]   (default: the folder above tests)
here=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "${1:-$here/..}" && pwd) || exit 1
pwsh=${FFXV_PWSH:-$(command -v pwsh)}
[ -n "$pwsh" ] || { echo "FAIL: pwsh not found"; exit 1; }
T=$(mktemp -d) || exit 1
trap 'rm -rf "$T"' EXIT
rel='My Games/FINAL FANTASY XV/Steam'
mk() { mkdir -p "$1/$2/savestorage"; printf '[BasicSettings]\r\nMaxFramerate=%s\r\n' "$3" > "$1/$2/savestorage/GraphicsConfig.ini"
       cp "$1/$2/savestorage/GraphicsConfig.ini" "$1/$2/savestorage/GraphicsConfig.ini.before_ffxv_dlss"; }
mk "$T/profile/Documents/$rel" 111 120             # the game's file
mk "$T/OneDrive/Documents/$rel" 222 60             # the copy left in the moved Documents folder
touch -d '+1 hour' "$T/OneDrive/Documents/$rel/222/savestorage/"*   # newer, so "newest wins" cannot pick by accident
mkdir -p "$T/OneDrive/.config"
HOME="$T/OneDrive" XDG_CONFIG_HOME="$T/OneDrive/.config" USERPROFILE="$T/profile" \
    "$pwsh" -NoProfile -File "$here/unit.ps1" -Case docs -Script "$repo/scripts/setup.ps1" || { echo "FAIL: settings files"; exit 1; }
echo "PASS: settings files from USERPROFILE\\Documents"
