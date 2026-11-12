#!/bin/bash
# Checks that the Ultra Performance scaling mode the menu writes for Intel XeSS renders inside XeSS's input range
# (src/xess_rules.h): builds tests/xess_scale_test.cpp with the host compiler and runs it.
# Usage: tests/check_xess_scale.sh [source folder holding xess_rules.h]   (default: src next to tests)
here=$(cd "$(dirname "$0")" && pwd)
src=$(cd "${1:-$here/../src}" && pwd) || exit 1
T=$(mktemp -d) || exit 1
trap 'rm -rf "$T"' EXIT
g++ -std=c++17 -O2 -Wall -Wextra -Werror -I "$src" -o "$T/xess_scale_test" "$here/xess_scale_test.cpp" || { echo "FAIL: xess_scale_test did not build"; exit 1; }
"$T/xess_scale_test" || { echo "FAIL: xess_scale_test"; exit 1; }
echo "PASS: XeSS Ultra Performance scale"
