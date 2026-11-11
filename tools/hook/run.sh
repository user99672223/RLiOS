#!/usr/bin/env bash
# Launch the real game OFFLINE with rlhook.dll injected.
#  - no EAC (RocketLeague.exe started directly, never Launcher.exe)
#  - no network at all: runs inside an empty network namespace (only loopback), so a hooked
#    client can never reach Epic/Psyonix services (CLAUDE.md §1)
#  - dummy Epic arguments (invalid exchange code): the game falls back to offline mode
#  - game install untouched: the DLL is injected by rlinject.exe, nothing is copied into the game dir
# Build first: tools/hook/build.sh
# Usage: run.sh <out-dir> [max-seconds]   env: RLHOOK_DUMP_DELAY, RLHOOK_EXIT_AFTER_DUMP
set -uo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
OUT="$(realpath -m "${1:?out dir}")"; MAXS="${2:-420}"
# Machine-specific paths come from the environment (see work/ENV.md): RL_USER, RL_USER_HOME, RL_GAME_DIR, RL_PROTON, RL_PREFIX
U="${RL_USER:?set RL_USER (see work/ENV.md)}"; UHOME="${RL_USER_HOME:-/home/$U}"
GAME="${RL_GAME_DIR:?set RL_GAME_DIR}"
PROTON="${RL_PROTON:?set RL_PROTON (path to the proton script)}"
PREFIX="${RL_PREFIX:?set RL_PREFIX (Proton compat data dir)}"
BIN="${RLHOOK_BIN:-$HERE/../../../work/hook/bin}"
NS=rloff
mkdir -p "$OUT"; rm -f "$OUT/DONE"
w() { echo "Z:${1//\//\\}"; }
sudo -n ip netns list | grep -q "^$NS\b" || sudo -n ip netns add $NS
sudo -n ip netns exec $NS ip link set lo up
ARGS=(-AUTH_LOGIN=unused -AUTH_PASSWORD=00000000000000000000000000000000 -AUTH_TYPE=exchangecode -epicapp=Sugar
      -epicenv=Prod -EpicPortal -epiclocale=en -epicsandboxid=9773aa1aa54f4f7b80e44bef04986cea ${RL_EXTRA_ARGS:-})
cd "$GAME/Binaries/Win64"
sudo -n ip netns exec $NS sudo -n -u $U env \
  PWD="$GAME/Binaries/Win64" UMU_ID=0 HOME=$UHOME USER=$U PATH=/usr/local/bin:/usr/bin:/bin \
  XDG_RUNTIME_DIR="${XDG_RUNTIME_DIR:-/run/user/$(id -u)}" DISPLAY="${DISPLAY:-:0}" WAYLAND_DISPLAY="${WAYLAND_DISPLAY:-}" \
  XAUTHORITY="${XAUTHORITY:-}" DBUS_SESSION_BUS_ADDRESS="${DBUS_SESSION_BUS_ADDRESS:-}" \
  STEAM_COMPAT_DATA_PATH="$PREFIX" STEAM_COMPAT_CLIENT_INSTALL_PATH="$UHOME/.steam" STEAM_COMPAT_INSTALL_PATH="$GAME" \
  SteamAppId=0 SteamGameId=0 WINEDEBUG=-all DXVK_LOG_LEVEL=none \
  RLHOOK_OUT="$(w "$OUT")" RLHOOK_DUMP_DELAY="${RLHOOK_DUMP_DELAY:-60}" RLHOOK_EXIT_AFTER_DUMP="${RLHOOK_EXIT_AFTER_DUMP:-0}" \
  "$PROTON" waitforexitandrun "$BIN/rlinject.exe" "$(w "$(realpath "$BIN")/rlhook.dll")" "$(w "$GAME/Binaries/Win64/RocketLeague.exe")" "${ARGS[@]}" \
  > "$OUT/proton.out" 2>&1 &
PID=$!
for ((t=0; t<MAXS; t+=5)); do
  sleep 5
  kill -0 $PID 2>/dev/null || { echo "game exited after ~${t}s"; break; }
  [ -e "$OUT/DONE" ] && [ "${RLHOOK_STOP_ON_DONE:-1}" = 1 ] && { echo "DONE after ~${t}s"; break; }
done
if kill -0 $PID 2>/dev/null; then
  sudo -n -u $U env HOME=$UHOME WINEPREFIX="$PREFIX/pfx" "$(dirname "$PROTON")/files/bin/wineserver" -k 2>/dev/null
  sleep 3; sudo -n kill $PID 2>/dev/null
fi
sed -i -E 's/(AUTH_PASSWORD=)[^ "]+/\1<redacted>/g' "$OUT/proton.out" 2>/dev/null
tail -5 "$OUT/proton.out"; echo ---; cat "$OUT/hook.log" 2>/dev/null
