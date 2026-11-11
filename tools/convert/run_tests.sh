#!/usr/bin/env bash
# Converter tests; exit 77 (ctest "skipped") when numpy/Pillow/astcenc are not available.
PY="${1:-python3}"
cd "$(dirname "$0")" || exit 1
"$PY" -c "import numpy, PIL" 2>/dev/null && command -v astcenc >/dev/null || { echo "skipped: needs numpy, Pillow, astcenc"; exit 77; }
exec "$PY" -W ignore -m unittest -v test_build_pack
