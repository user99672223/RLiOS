#!/usr/bin/env bash
# Fails if any fused multiply-add appears in the arm64 objects of sim/ and game/ (CLAUDE.md s5).
# Usage: tools/check_no_fma.sh <cmake-build-dir>      (build dir must be an arm64 build)
#        tools/check_no_fma.sh --selftest              (negative control: detector must fire)
# Env: OBJDUMP (default objdump), CXX (default c++) for --selftest.
set -euo pipefail
OBJDUMP="${OBJDUMP:-objdump}"
PATTERN='\b(fmadd|fmsub|fnmadd|fnmsub|fmla|fmls|fmad)\b'

if [[ "${1:-}" == "--selftest" ]]; then
  CXX="${CXX:-c++}"
  tmp="$(mktemp -d)"
  trap 'rm -rf "$tmp"' EXIT
  cat > "$tmp/probe.cpp" <<'CPP'
float probe(float a, float b, float c) { return a * b + c; }
CPP
  "$CXX" -O2 -ffp-contract=fast -c "$tmp/probe.cpp" -o "$tmp/on.o"
  "$CXX" -O2 -ffp-contract=off -c "$tmp/probe.cpp" -o "$tmp/off.o"
  "$OBJDUMP" -d "$tmp/on.o" | grep -Eq "$PATTERN" || { echo "selftest FAILED: fused op not detected with contraction on"; exit 1; }
  if "$OBJDUMP" -d "$tmp/off.o" | grep -Eq "$PATTERN"; then echo "selftest FAILED: fused op with contraction off"; exit 1; fi
  echo "selftest OK: detector fires with -ffp-contract=fast and is silent with =off"
  exit 0
fi

build="${1:?usage: $0 <build-dir> | --selftest}"
mapfile -t objs < <(find "$build/CMakeFiles/rlios_sim.dir" "$build/CMakeFiles/rlios_game.dir" -name '*.o' | sort)
[[ ${#objs[@]} -gt 0 ]] || { echo "no sim/game objects found under $build"; exit 1; }
arch="$("$OBJDUMP" -f "${objs[0]}" | grep -i 'file format' || true)"
echo "checking ${#objs[@]} objects ($arch)"
insns=0
fail=0
for o in "${objs[@]}"; do
  dis="$("$OBJDUMP" -d "$o")"
  n="$(grep -cE '^\s+[0-9a-f]+:\s' <<<"$dis" || true)"
  insns=$((insns + n))
  if grep -Eq "$PATTERN" <<<"$dis"; then
    echo "FMA found in $o:"; grep -E "$PATTERN" <<<"$dis" | head; fail=1
  fi
done
[[ $insns -gt 50 ]] || { echo "suspiciously few instructions ($insns): check is vacuous"; exit 1; }
[[ $fail -eq 0 ]] || exit 1
echo "no fused multiply-add in $insns instructions"
