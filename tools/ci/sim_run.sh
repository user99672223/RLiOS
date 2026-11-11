#!/usr/bin/env bash
# Run the app in the iOS Simulator: capture, phone screenshot, soak. Output in $OUT.
# Usage: tools/ci/sim_run.sh <path/to/RLiOS.app> <out-dir>
# ASSETPACK_DIR (optional): decrypted asset pack; installed into the app's Documents/assetpack for the
# runs and removed again at the end. Only our own renders/logs go to <out-dir>.
set -euo pipefail
APP="$1"
OUT="$2"
BUNDLE_ID="${BUNDLE_ID:-com.rlios.app}"
mkdir -p "$OUT"

# Pick an available iPhone simulator.
UDID="$(xcrun simctl list devices available -j | python3 -c '
import json,sys
d=json.load(sys.stdin)["devices"]
cands=[(rt,x) for rt,l in d.items() if "iOS" in rt for x in l if "iPhone" in x["name"]]
cands.sort(key=lambda t:(t[0],t[1]["name"]))
print(cands[-1][1]["udid"] if cands else "")')"
[[ -n "$UDID" ]] || { echo "no iPhone simulator available"; xcrun simctl list devices; exit 1; }
echo "simulator: $UDID"
xcrun simctl boot "$UDID" || true
xcrun simctl bootstatus "$UDID" -b
xcrun simctl install "$UDID" "$APP"

wait_for() { # file timeout
  for _ in $(seq 1 "$2"); do [[ -f "$1" ]] && return 0; sleep 1; done
  echo "timeout waiting for $1"; return 1
}
DATA="$(xcrun simctl get_app_container "$UDID" "$BUNDLE_ID" data)"
rm -rf "$DATA/Documents/capture" "$DATA/Documents/soak" "$DATA/Documents/assetpack"
EXPECT=""
if [[ -n "${ASSETPACK_DIR:-}" && -f "$ASSETPACK_DIR/manifest.txt" ]]; then
  cp -R "$ASSETPACK_DIR" "$DATA/Documents/assetpack"
  trap 'rm -rf "$DATA/Documents/assetpack"' EXIT
  EXPECT="--expect-pack"
  echo "asset pack installed into the app container ($(du -sh "$DATA/Documents/assetpack" | cut -f1))"
else
  echo "no asset pack: placeholder arena"
fi

echo "== capture =="
xcrun simctl launch "$UDID" "$BUNDLE_ID" --capture capture
wait_for "$DATA/Documents/capture/diagnostics.json" 240
mkdir -p "$OUT/capture"
cp "$DATA/Documents/capture/"* "$OUT/capture/"
cat "$OUT/capture/diagnostics.json"

echo "== phone screen =="
xcrun simctl terminate "$UDID" "$BUNDLE_ID" || true
xcrun simctl launch "$UDID" "$BUNDLE_ID"
sleep 8
xcrun simctl io "$UDID" screenshot "$OUT/phone_screen.png"
xcrun simctl terminate "$UDID" "$BUNDLE_ID" || true

echo "== soak (10 s) =="
xcrun simctl launch "$UDID" "$BUNDLE_ID" --soak 10
wait_for "$DATA/Documents/soak/soak.json" 60
mkdir -p "$OUT/soak"
cp "$DATA/Documents/soak/"* "$OUT/soak/"
cat "$OUT/soak/soak.json"

echo "== not-implemented flags =="
xcrun simctl launch "$UDID" "$BUNDLE_ID" --scenario x.scn
wait_for "$DATA/Documents/automation/not_implemented.json" 30
cp "$DATA/Documents/automation/not_implemented.json" "$OUT/"

xcrun simctl spawn "$UDID" log show --last 3m --predicate 'process == "RLiOS"' --style compact > "$OUT/app.log" 2>&1 || true
python3 "$(dirname "$0")/verify_capture.py" "$OUT/capture/capture.png" "$OUT/capture/diagnostics.json" $EXPECT
python3 - "$OUT/soak/soak.json" <<'PY'
import json,sys
d=json.load(open(sys.argv[1])); print("soak:",d)
assert d["ok"] is True and d["exit_code"]==0, "soak unhealthy"
PY
