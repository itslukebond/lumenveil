#!/usr/bin/env bash
# Builds the release folder and zip from a built dxgi.dll and the Streamline SDK's bin/x64 folder, then checks them.
# Usage: tools/package.sh --dll <built dxgi.dll> --streamline-bin <Streamline bin/x64 folder> --version <text> [--out <dir>]
# Writes <out>/lumenveil-<version>/ and <out>/lumenveil-<version>.zip (default <out>: dist/). Uploads nothing.
set -u
repo=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)
dll='' sl='' version='' out=$repo/dist
die() { echo "package: $*" >&2; exit 1; }
while [ $# -gt 0 ]; do
    [ $# -ge 2 ] || die "missing value for $1"
    case $1 in
        --dll) dll=$2 ;;
        --streamline-bin) sl=$2 ;;
        --version) version=$2 ;;
        --out) out=$2 ;;
        *) die "unknown option $1" ;;
    esac
    shift 2
done
[ -f "$dll" ] || die "no dxgi.dll given (--dll)"
[ -d "$sl" ] || die "no Streamline bin/x64 folder given (--streamline-bin)"
case $version in '' | *[!A-Za-z0-9._]*) die "the version may hold only letters, digits, dots and underscores" ;; esac
command -v python3 > /dev/null 2>&1 || die "python3 is needed to check the files and write the zip"

name=lumenveil-$version
dest=$out/$name
[ -e "$dest" ] || [ -e "$dest.zip" ] && die "$dest or its zip already exists; delete them first"
streamline=(sl.interposer.dll sl.common.dll sl.dlss_g.dll sl.reflex.dll sl.pcl.dll)
fonts=(texgyreadventor-bold.otf texgyreadventor-regular.otf GUST-FONT-LICENSE.txt)

mkdir -p "$dest/scripts" "$dest/game/ffxv_dlss/streamline" "$dest/game/ffxv_dlss/fonts" || die "cannot create $dest"
cp "$repo/install.sh" "$repo/uninstall.sh" "$repo/update.sh" "$repo/undo-update.sh" "$dest/" &&
    cp "$repo/scripts/setup.sh" "$repo/scripts/setup.ps1" "$dest/scripts/" &&
    cp "$repo/INSTALL.md" "$dest/INSTALL.txt" &&
    cp "$repo/LICENSE" "$repo/THIRD_PARTY_NOTICES.md" "$dest/" &&
    cp "$dll" "$dest/game/dxgi.dll" || die "copy failed"
for b in install.bat uninstall.bat update.bat undo-update.bat; do
    sed 's/\r$//; s/$/\r/' "$repo/$b" > "$dest/$b" || die "copy of $b failed"
done
for f in "${streamline[@]}"; do cp "$sl/$f" "$dest/game/ffxv_dlss/streamline/$f" || die "$f is not in $sl"; done
for f in "${fonts[@]}"; do cp "$repo/third_party/tex-gyre-adventor/$f" "$dest/game/ffxv_dlss/fonts/$f" || die "font $f missing"; done
chmod 644 $(find "$dest" -type f) &&
    chmod 755 "$dest/install.sh" "$dest/uninstall.sh" "$dest/update.sh" "$dest/undo-update.sh" "$dest/scripts/setup.sh"

# Check the output: exact file list, the mod's marker, nothing NVIDIA, AMD, Intel or personal.
python3 -I - "$dest" "$name" "${USER:-}" "${HOME:-}" "$repo" "$(cd "$(dirname "$dll")" && pwd -P)" <<'PY' || exit 1
import os, sys, zipfile
dest, name, user, home, repo, dlldir = sys.argv[1:7]
want = sorted('''INSTALL.txt LICENSE THIRD_PARTY_NOTICES.md install.bat install.sh uninstall.bat uninstall.sh
update.bat update.sh undo-update.bat undo-update.sh scripts/setup.ps1 scripts/setup.sh game/dxgi.dll
game/ffxv_dlss/streamline/sl.interposer.dll game/ffxv_dlss/streamline/sl.common.dll game/ffxv_dlss/streamline/sl.dlss_g.dll
game/ffxv_dlss/streamline/sl.reflex.dll game/ffxv_dlss/streamline/sl.pcl.dll
game/ffxv_dlss/fonts/texgyreadventor-bold.otf game/ffxv_dlss/fonts/texgyreadventor-regular.otf
game/ffxv_dlss/fonts/GUST-FONT-LICENSE.txt'''.split())
have = sorted(os.path.relpath(os.path.join(r, f), dest).replace(os.sep, '/') for r, _, fs in os.walk(dest) for f in fs)
problems = []
if have != want:
    problems.append('file list differs: extra %s, missing %s' % (sorted(set(have) - set(want)), sorted(set(want) - set(have))))
for f in have:
    base = f.rsplit('/', 1)[-1].lower()
    if base.startswith(('nvngx', 'amd_', 'ffx_', 'libxess', 'libxell')) or base.endswith(('.png', '.dds', '.pdb')):
        problems.append('%s must not ship' % f)
if b'ffxv-dlss' not in open(os.path.join(dest, 'game/dxgi.dll'), 'rb').read():
    problems.append('game/dxgi.dll lacks the text ffxv-dlss: not this mod\'s build')
checks = [('home folder', home), ('source folder', repo), ('build folder', dlldir)]
for f in have:
    data = open(os.path.join(dest, f), 'rb').read()
    # the user name anywhere, also inside a longer word such as a handle, in any letter case
    if len(user) >= 3:
        texts = [data.decode('latin-1'), data.decode('utf-16-le', 'ignore'), data[1:].decode('utf-16-le', 'ignore')]
        if any(user.lower() in t.lower() for t in texts):
            problems.append('%s contains the user name (%s)' % (f, user))
    for what, text in checks:
        if len(text) < 3:
            continue
        for form in {text, text.replace('/', '\\')}:
            if form.encode('utf-8') in data or form.encode('utf-16-le') in data:
                problems.append('%s contains the %s (%s)' % (f, what, form))
for f in ('install.bat', 'uninstall.bat', 'update.bat', 'undo-update.bat'):
    lines = open(os.path.join(dest, f), 'rb').read().split(b'\n')[:-1]
    if not lines or any(not l.endswith(b'\r') for l in lines):
        problems.append('%s is not CRLF' % f)
for f in ('install.bat', 'uninstall.bat', 'update.bat', 'undo-update.bat', 'scripts/setup.ps1'):
    if any(b > 0x7f for b in open(os.path.join(dest, f), 'rb').read()):
        problems.append('%s is not pure ASCII' % f)
if problems:
    print('package: the release is not right:', file=sys.stderr)
    for p in problems:
        print('  ' + p, file=sys.stderr)
    sys.exit(1)
with zipfile.ZipFile(dest + '.zip', 'w', zipfile.ZIP_DEFLATED) as z:
    for f in want:
        p = os.path.join(dest, f)
        info = zipfile.ZipInfo(name + '/' + f, date_time=(2026, 1, 1, 0, 0, 0))
        info.external_attr = (0o100000 | (os.stat(p).st_mode & 0o777)) << 16
        info.compress_type = zipfile.ZIP_DEFLATED
        z.writestr(info, open(p, 'rb').read())
print('wrote %s.zip (%d files)' % (dest, len(want)))
PY
