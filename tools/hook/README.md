# Offline reference hook

Records ground truth from the real game on the laptop. Offline only: the game runs without EAC inside an empty
network namespace, and nothing is written into the game directory.

| File | Purpose |
|---|---|
| `rlhook.c` | DLL injected into the game. Finds GNames/GObjects by structure, detects the reflection layout, dumps the SDK, detours the per-tick physics function, and serves a command channel on the game thread. |
| `rlinject.c` | Starts the game suspended and loads the DLL. |
| `build.sh` | Builds both with mingw-w64 into `work/hook/bin/`. |
| `run.sh` | Launches the game (Proton, no network, dummy Epic arguments) with the hook. |
| `record.py` | Runs a scenario file: enters Training → Free Play, applies the initial state at a tick boundary, records exact per-tick state into `work/reference/<name>/`. |
| `scenarios/` | Scenario files (our own text; reference logs never go into git). |

Setup: `source work/env.sh` (machine paths), `tools/hook/build.sh`, then `tools/hook/record.py tools/hook/scenarios/ball_drop_500uu.txt`.

Command channel (`<out>/cmd.txt` → `<out>/cmd.out`): `find`, `hex`, `call`, `watch`, `watchp`, `recstart`, `unwatch`, `poke` — see comments in `rlhook.c`.
