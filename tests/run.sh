#!/usr/bin/env bash
# Runs the install script tests inside a bubblewrap sandbox: the whole filesystem is read-only except one
# fresh test folder, the tests see only their own processes, and there is no network or display.
# Usage: tests/run.sh            (test folders go under $FFXV_TEST_TMP, else $TMPDIR, else /tmp)
set -u
here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
repo=$(cd "$here/.." && pwd)

if ! command -v bwrap >/dev/null 2>&1; then
    echo "bwrap (bubblewrap) is needed to run these tests; they never run outside the sandbox." >&2
    exit 1
fi
pwsh=$(command -v pwsh || true)
[ -n "$pwsh" ] && pwsh=$(readlink -f "$pwsh")

base=${FFXV_TEST_TMP:-${TMPDIR:-/tmp}}
mkdir -p "$base"
T=$(mktemp -d "$base/ffxv-dlss-tests.XXXXXX") || exit 1
mkdir -p "$T/home" "$T/tmp"
echo "test folder: $T"

bwrap --ro-bind / / --dev /dev --proc /proc --tmpfs /tmp --tmpfs /run --bind "$T" "$T" \
    --unshare-pid --unshare-net --die-with-parent \
    --setenv HOME "$T/home" --setenv TMPDIR "$T/tmp" --setenv FFXV_T "$T" --setenv FFXV_REPO "$repo" \
    --setenv FFXV_PWSH "$pwsh" --setenv PSModulePath "${PSModulePath:-$HOME/.local/share/powershell/Modules}" \
    --unsetenv WAYLAND_DISPLAY --unsetenv DISPLAY --unsetenv XDG_RUNTIME_DIR --unsetenv DBUS_SESSION_BUS_ADDRESS \
    --chdir "$T" bash "$repo/tests/cases.sh" "$@"
status=$?
# A run leaves about 300 MB behind, so the folder goes after a pass; after a failure it stays for a look.
if [ "$status" -eq 0 ]; then
    chmod -R u+w "$T" && rm -rf "$T"
else
    echo "tests failed; test folder kept: $T"
fi
exit "$status"
