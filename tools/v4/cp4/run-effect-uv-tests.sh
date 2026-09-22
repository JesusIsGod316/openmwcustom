#!/usr/bin/env bash
set -euo pipefail
root=$(cd "$(dirname "$0")/../../.." && pwd)
out=${1:-"$root/build-effect-uv"}
mkdir -p "$out"
flags=(-std=c++20 -O2 -Wall -Wextra -Wpedantic -Werror -I"$root")
libs=(-losg -lOpenThreads)
if [[ -n ${OPENMW_UV_TEST_SYSROOT:-} ]]; then
    flags+=(-isystem "$OPENMW_UV_TEST_SYSROOT/include")
    libs=(-L"$OPENMW_UV_TEST_SYSROOT/lib/x86_64-linux-gnu" "${libs[@]}")
    export LD_LIBRARY_PATH="$OPENMW_UV_TEST_SYSROOT/lib/x86_64-linux-gnu${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
fi
if [[ ${OPENMW_UV_TEST_SANITIZERS:-0} == 1 ]]; then
    flags+=(-O0 -g1 -fsanitize=address,undefined -fno-omit-frame-pointer)
fi
"${CXX:-c++}" "${flags[@]}" "$root/tools/v4/cp4/effect-uv-tests.cpp" "${libs[@]}" -o "$out/effect-uv-tests"
"$out/effect-uv-tests"
