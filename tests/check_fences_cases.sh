#!/bin/bash
# Checks tests/check_fences.sh itself: built from src/present12.cpp and src/up12.cpp, copies where no API signals a
# shared pair, where both APIs signal one (in the presenter, or through P12Shared in up12.cpp) and where up12.cpp makes
# a Signal call the check cannot attribute must each fail, and so must xess.cpp signalling a D3D12-owned fence from
# D3D11; the source as it stands must pass.
# Usage: tests/check_fences_cases.sh
here=$(cd "$(dirname "$0")" && pwd)
src="$here/../src/present12.cpp"
up="$here/../src/up12.cpp"
fsr="$here/../src/fsr.cpp"
xess="$here/../src/xess.cpp"
T=$(mktemp -d) || exit 1
trap 'rm -rf "$T"' EXIT
fails=0

expect() {  # expect <pass|fail> <label> <present12 file> [other files...]
    local want=$1 label=$2; shift 2
    if "$here/check_fences.sh" "$@" >"$T/out" 2>&1; then got=pass; else got=fail; fi
    if [ "$got" = "$want" ]; then echo "ok: $label ($got)"; else echo "WRONG: $label: expected $want, got $got"; sed 's/^/    /' "$T/out"; fails=$((fails + 1)); fi
}

expect pass "source as it stands" "$src" "$up" "$fsr" "$xess"

# Zero signallers: drop every D3D11 signal of the upload fence (the presenter's, and up12.cpp's through P12Shared), so
# the D3D12 wait on it never returns.
grep -v 'g_ctx11->Signal(g_up11,' "$src" >"$T/zero.cpp"
cmp -s "$src" "$T/zero.cpp" && { echo "WRONG: zero fixture: no D3D11 signal of g_up11 found to remove"; fails=$((fails + 1)); }
grep -v 's.ctx11->Signal(s.up11,' "$up" >"$T/zero_up12.cpp"
cmp -s "$up" "$T/zero_up12.cpp" && { echo "WRONG: zero fixture: no D3D11 signal of s.up11 found in up12.cpp"; fails=$((fails + 1)); }
expect fail "pair with no signaller" "$T/zero.cpp" "$T/zero_up12.cpp" "$fsr" "$xess"

# Two signallers: the D3D11 context also signals the presenter's fence.
sed 's/g_ctx11->Signal(g_up11, ++g_upVal);/&\r\n    g_ctx11->Signal(g_fence11, 1);/' "$src" >"$T/both.cpp"
cmp -s "$src" "$T/both.cpp" && { echo "WRONG: both fixture: insertion point not found"; fails=$((fails + 1)); }
expect fail "pair signalled from both APIs" "$T/both.cpp" "$up" "$fsr" "$xess"

# Two signallers through P12Shared: up12.cpp's D3D11 side signals the D3D12-owned fence.
sed 's/s\.ctx11->Signal(s\.up11, ++\*s\.upVal);/s.ctx11->Signal(s.fence11, ++*s.fenceVal);/' "$up" >"$T/both_up12.cpp"
cmp -s "$up" "$T/both_up12.cpp" && { echo "WRONG: up12 fixture: D3D11 signal of s.up11 not found"; fails=$((fails + 1)); }
expect fail "up12.cpp signals a D3D12-owned fence from D3D11" "$src" "$T/both_up12.cpp" "$fsr" "$xess"

# A Signal the check cannot attribute (not s.queue / s.ctx11 with a P12Shared fence).
sed 's/s\.queue->Signal(s\.fence, ++\*s\.fenceVal)/s.dev11->Signal(s.fence, ++*s.fenceVal)/' "$up" >"$T/odd_up12.cpp"
cmp -s "$up" "$T/odd_up12.cpp" && { echo "WRONG: odd fixture: D3D12 signal in up12Idle not found"; fails=$((fails + 1)); }
expect fail "unattributable Signal call in up12.cpp" "$src" "$T/odd_up12.cpp" "$fsr" "$xess"

# Two signallers through P12Shared in xess.cpp: its D3D11 side signals the D3D12-owned fence after the output copy.
sed 's/s\.ctx11->CopyResource(out, g_out\.r11);/&\n    s.ctx11->Signal(s.fence11, ++*s.fenceVal);/' "$xess" >"$T/both_xess.cpp"
cmp -s "$xess" "$T/both_xess.cpp" && { echo "WRONG: xess fixture: output copy in xessUpscale not found"; fails=$((fails + 1)); }
expect fail "xess.cpp signals a D3D12-owned fence from D3D11" "$src" "$up" "$fsr" "$T/both_xess.cpp"

[ "$fails" -eq 0 ] && echo "PASS: check_fences.sh cases" || { echo "FAIL: $fails case(s)"; exit 1; }
