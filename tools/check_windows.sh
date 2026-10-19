#!/bin/sh
# Builds the Windows host (opensims2_win) on macOS into build-wincheck/, with
# the Windows build's compat <simd/simd.h> and SDL3 (brew install sdl3):
# compiled and linked, not run (its shaders are DXIL). A change to the shared
# code that breaks the Windows build fails here, before the Windows CI; what
# only MSVC rejects (a missing #include its headers don't pull in) still
# needs the CI.
# usage: tools/check_windows.sh [jobs]   (default 2)
set -eu
cd "$(dirname "$0")/.."
jobs=${1:-2}
prefix=$(brew --prefix 2>/dev/null || echo /opt/homebrew)
[ -f "$prefix/include/SDL3/SDL.h" ] || { echo "check_windows: SDL3 not found (brew install sdl3)"; exit 2; }
cmake -S . -B build-wincheck -DOS2_WINDOWS_CHECK=ON -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_PREFIX_PATH="$prefix" >/dev/null
cmake --build build-wincheck --target opensims2_win test_simd -j"$jobs"
build-wincheck/test_simd
echo "windows check: ok"
