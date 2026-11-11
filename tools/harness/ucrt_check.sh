#!/usr/bin/env bash
# Re-runnable proof of core/sim/crt_math.cpp against the game's own ucrtbase.dll (laptop only).
# Usage: tools/harness/ucrt_check.sh [--random N] [fn ...|all]     (default: all)
#        tools/harness/ucrt_check.sh --golden fn ...                (print golden test vectors)
# Needs: x86_64-w64-mingw32-g++, wine, RL_GAME_DIR (source work/env.sh). The DLL is read in place.
set -euo pipefail
here="$(cd "$(dirname "$0")/../.." && pwd)"
dll="${RL_GAME_DIR:?source work/env.sh}/Binaries/Win64/ucrtbase.dll"
out="${UCRT_CHECK_OUT:-${TMPDIR:-/tmp}/ucrt_check}"
mkdir -p "$out"
x86_64-w64-mingw32-g++ -std=c++20 -O2 -mfma -ffp-contract=off -fno-fast-math -static \
  -I"$here/core" "$here/tools/harness/ucrt_check.cpp" "$here/core/sim/crt_math.cpp" \
  -o "$out/ucrt_check.exe"
# Wine substitutes its OWN builtin ucrtbase for any LoadLibrary of a file named ucrtbase.dll (even by
# full path), so load the real one through a differently named symlink (no copy is made).
ln -sf "$dll" "$out/ucrtref.dll"
[[ $# -eq 0 ]] && set -- all
cd "$out"
WINEDEBUG=-all wine ./ucrt_check.exe ucrtref.dll "$@"
