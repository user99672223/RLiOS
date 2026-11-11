# RLiOS repo conventions

The standing brief lives one level up: `../CLAUDE.md` (workspace root). Read it first; this file
only holds repo-specific conventions. Never commit game files or anything from `work/`.

## Layout
- `core/{sim,game,net,render,ui,audio,io}`: C++20 core. `core/api/rlios.h` is the C API the Swift
  shell calls. `sim/` and `game/` have no platform deps and build on Linux.
- `core/render/`: Metal renderer (`renderer.mm`, `shaders/*.metal`); `stub/` is the non-Apple stub.
- `app/`: Swift shell + `project.yml` (XcodeGen; the Xcode project is generated, never committed).
- `tools/`: `check_no_fma.sh`, `ci/` scripts. `.github/workflows/`: `core.yml`, `ios.yml`.

## Build and test (Linux)
    cmake -S . -B build -G Ninja && cmake --build build -j3 && ctest --test-dir build --output-on-failure
    build/rlios_sim_cli --scenario core/tests/scenarios/basic.scn --out /tmp/x86.dump
Cross-check arm64 locally: build with an aarch64 toolchain (static), run under `qemu-aarch64`, and
`cmp` the per-tick dumps. FMA check on an arm64 build: `tools/check_no_fma.sh <build-dir>`
(`--selftest` is the negative control; set `OBJDUMP`/`CXX` for cross toolchains).

## iOS (macOS only; CI does this)
    xcodegen generate --spec app/project.yml
    xcodebuild build -project app/RLiOS.xcodeproj -scheme RLiOS -sdk iphonesimulator ...
Bundle id is the single variable `RLIOS_BUNDLE_ID` in `app/project.yml`.
Automation launch args: `--capture <dir>`, `--soak <seconds>`, (`--scenario`, `--flow`: not yet).

## Style
- clang-format 19 (`.clang-format`), CI enforces `--dry-run -Werror` on `core/`. Swift: 4 spaces.
- sim/ and game/: `-ffp-contract=off`, no fast-math, warnings as errors. Any non-obvious constant or
  ordering must cite its source (binary address, UScript, UE3 file:line, measurement).
- No allocations in the sim tick; no exceptions/RTTI in hot paths.
