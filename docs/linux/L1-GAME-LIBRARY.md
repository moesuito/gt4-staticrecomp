# Linux game library and headless smoke (TASK-074, plan step L1, second part)

Status: **the game code is regenerated, compiled and boots on Linux** (2026-11-20). This is the rest of step L1 in
[the Linux plan](PLAN.md), after the [host-only build](HOST-BUILD.md). It produces a **Linux candidate** game library:
unpinned, not production, with no gameplay qualification. The Windows production library, pins and launcher are
untouched.

## Results

All on this machine: openSUSE, Linux 7.3, x86-64, 12 threads, clang 23.1.4, Python 3.13 with pycdlib 1.20.0.

| Stage | Result | Label |
|---|---|---|
| CORE.GT4 from the local ISO | size 2,020,861, SHA-256 equal to `docs/inputs/usa-v2.00.json` | Confirmed |
| Donor at `1b48087` | all 38 source pins in `external-recomp-references.json` match, once checked out with `core.autocrlf=true` (below) | Confirmed |
| `ee_producer.py prepare` | rebuilt ELF equal to the pinned native ELF (`usa-v2.00-native.json`) | Confirmed |
| Translator `eerecomp` built with clang++ | no warnings with `-Wall -Wextra` | Confirmed |
| `ee_producer.py generate` | **328 files, 86,375,038 bytes, inventory `7a1a0909...`: byte-identical to the frozen Windows inventory** | Confirmed |
| `libps2game.a` | 327 x86-64 ELF objects, one per generated unit; object inventory `d5d86cc2...` | Confirmed |
| Reproducibility | a second build from scratch (`build_linux.sh`) gives the same object inventory; only the archive wrapper (member dates) differs | Confirmed |
| Headless boot | 1,200 frames in 10.4 s, then 12,000 frames with scripted input, no halt and no "no translated code" | Confirmed |

What the headless runs reached, from the saved frames: the game's "No GT4 game data found on memory card" dialog, the
create-data and format-card dialogs, the "Welcome to Gran Turismo 4" name entry, typing letters on its keyboard, and the
"Do you want to proceed with this name?" confirmation. The UI animates (button highlights) and the game reacts to each
scripted press.

**Not exercised yet (Unknown):** the opening movie (the IPU decoded nothing in these runs), any 3D scene or race, audio,
a memory card write (the card was never formatted in the scripted runs), VU1 AOT (the smoke runner uses the donor's VU1
interpreter), the Vulkan GS path, and frame-by-frame identity against the Windows build. The headless frame rate says
nothing about real performance: these are 2D menus drawn by the donor's CPU rasterizer.

## How to reproduce

1. **Python environment.** `python3 -m venv private/tooling-venv`, then
   `private/tooling-venv/bin/pip install -r scripts/requirements.txt`. On Linux the interpreter is
   `private/tooling-venv/bin/python` (Windows uses `Scripts/python.exe`).
2. **Donor, exactly as pinned.** The source pins were taken on a Windows checkout with CRLF line endings, so check
   the donor out with `core.autocrlf=true`; the files then match every pin and `git status` is clean, which is what
   `verify_donor` requires:
   ```sh
   git init private/references/ps2-web-recomp && cd private/references/ps2-web-recomp
   git config core.autocrlf true
   git remote add origin https://github.com/snuri00/ps2-web-recomp
   git fetch --depth 1 origin 1b4808793a12c45125262fd2d5f50a9ec9f7c020 && git checkout FETCH_HEAD
   ```
3. **CORE.GT4.** Extract `/CORE.GT4;1` from your ISO to `private/fingerprint-check/CORE.GT4`; `prepare` checks it
   against the pin. (`prepare --iso` checks the whole disc instead; see the ISO note below.)
4. **Producer.** `tools/ee_producer/build_linux.sh . private/<fresh-name> [clang++|g++]` runs prepare, the translator
   build, generate, the library build and `record-library`, which writes `linux-library.json`.
5. **Smoke runner.** Configure the root project with the donor and that producer:
   ```sh
   cmake -S . -B <build dir> -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_COMPILER=clang++ \
     -DGT4_DONOR=$PWD/private/references/ps2-web-recomp -DGT4_LINUX_PRODUCER=$PWD/private/<fresh-name>
   cmake --build <build dir> --target gt4_headless_smoke
   <build dir>/gt4_headless_smoke private/<fresh-name>/SCUS_973.28.elf --ntsc --disc <iso> --frames 1200 \
     --memcard <run dir>/card.ps2 --frames-dir <run dir>/frames --frame-every 100
   ```
   CMake refuses a `libps2game.a` whose SHA-256 differs from its `linux-library.json`. Frames are written as PPM.
   `--press frame:button` (select, start, up, down, left, right, cross, circle, square, triangle, l1-l3, r1-r3)
   scripts input.

## Changes in this part

- `tools/ee_producer/CMakeLists.txt`: a Linux branch (clang++ or g++ on Linux x86-64, `-Wall -Wextra`); the Windows
  checks and flags are unchanged.
- `scripts/ee_producer.py`: a separate Linux candidate recipe (`verify_linux_toolchain`: Clang or GCC, C++20,
  `-O3 -DNDEBUG`, no extra flags; the version is recorded, not pinned) chosen by `verify_host_toolchain` on Linux, the
  translator name without `.exe`, and `record_linux_library`, which writes `linux-library.json` and never
  `library.json`. The qualified Windows recipe (`verify_toolchain`, `QUALIFIED_OBJECT_INVENTORY`) is unchanged and
  never accepts a Linux receipt. The generated-source gate is the same on both systems.
- `scripts/ee_archive.py`: `elf_object_inventory`, the GNU ar and ELF counterpart of the COFF inventory.
- `tools/ee_producer/build_linux.sh`: the Linux counterpart of `build.cmd`.
- `cmake/LinuxHost.cmake`: the optional `gt4_headless_smoke` target (donor runtime with the same source list as
  `RUNTIME_SOURCES` in `native/CMakeLists.txt`, the donor's CPU rasterizer and headless `host/main.cpp`).
- Tests: `test_ee_archive.py` (ELF archive inventory: names, hashing, rejection of non-x86-64 or non-relocatable
  objects and bad archives) and `test_ee_producer.py` (the Linux recipe and its rejections; the Windows recipe refuses
  a Linux receipt; the host picks the recipe and translator name).

`vu_jit.cpp` and `vu_jit_web.cpp` are in the smoke runner's source list, as in the Windows runtime: outside Emscripten
the VU JIT mode is 0, so no code is generated at run time (`docs/ARCHITECTURE.md`, VU JIT row).

## ISO note

The local image used here is labelled "1.01". Its `CORE.GT4` and `SCUS_973.28` are byte-identical to the pins, but its
57-byte `SYSTEM.CNF` says `VER = 1.01` instead of `2.00`, so the whole-disc hash differs from `usa-v2.00.json`. The
producer only needs CORE (step 3). The launcher and `prepare --iso` check the whole disc and would refuse this image;
whether such images should be accepted is an owner decision for step L4, not part of this step.

## Still open in L1

- The Vulkan path is done headless: see [L1-VULKAN.md](L1-VULKAN.md).
- The VU1 and VU0 AOT capsules (`prepare_perf_runtime.py`) on Linux, so the smoke matches the production profile.
