#!/bin/bash
# Laptop-only: run every recorded ball scenario in the reference directory through the exactness
# harness (CLAUDE.md section 9). Reference recordings and collision data are game-derived and
# never committed.
#   tools/harness/run_all.sh [build-dir] [runner...]     e.g.  run_all.sh build-arm64 qemu-aarch64
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
BUILD=${1:-$ROOT/build}
shift
WORK=${RLIOS_WORK:-/home/familybadgujarwilly/Videos/rlios/work}
REF=${RLIOS_REFERENCE_DIR:-$WORK/reference}
COL=${RLIOS_COLLISION_FILE:-$WORK/collision/stadium_p.rlcol}
pass=0
fail=0
for d in "$REF"/ball_*/; do
    [ -f "$d/rec.tsv" ] || continue
    out=$("$@" "$BUILD/ball_harness" "${d%/}" --collision "$COL" 2>&1 | tail -1)
    echo "$out"
    case "$out" in PASS*) pass=$((pass + 1)) ;; SKIP*) ;; *) fail=$((fail + 1)) ;; esac
done
echo "ball scenarios: $pass passed, $fail failed"
[ "$fail" = 0 ]
