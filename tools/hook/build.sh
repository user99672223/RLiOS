#!/usr/bin/env bash
# Build the offline reference hook (Windows x64 binaries, via mingw-w64) into work/hook/bin (never committed).
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
OUT="${RLHOOK_BIN:-$HERE/../../../work/hook/bin}"
mkdir -p "$OUT"
x86_64-w64-mingw32-gcc -O1 -Wall -shared -static-libgcc -o "$OUT/rlhook.dll" "$HERE/rlhook.c"
x86_64-w64-mingw32-gcc -O1 -Wall -municode -o "$OUT/rlinject.exe" "$HERE/rlinject.c"
echo "built: $OUT/rlhook.dll $OUT/rlinject.exe"
