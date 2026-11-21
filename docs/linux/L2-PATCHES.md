# Game patches on Linux (TASK-074, plan step L2)

Status: **the Linux build runs the release's game code and patches** (2026-11-21): forced 480p races, the game's own
16:9, the display aspect for other window shapes (ultrawide), the 360-degree chase camera, the field of view zoom, the
full trigger range, the license demo guard and the race HUD and overlay corrections. Every input is reproduced on Linux
and equals the 1.3.0-rc14 pins.

## How the patches work (from the Windows build, unchanged)

- **In the translated game code.** Some patches need code inside the game: the race HUD constants (TASK-080 P2, a
  two-word patch of the ELF before translation, `scripts/aspect_elf_patch.py`) and the call sites of the 360-degree
  camera, the field of view, the pedal scale and the 2D overlays (TASK-159/160/161/095, inserted into the translated C++
  by `scripts/camera360_units.py`). Their host functions are `native/camera_360_hook.cpp`, `fov_adjust_hook.cpp` and
  `display_aspect_hook.cpp`.
- **At every guest VBlank** (`native/mtvu_runtime.cpp` `vblank_sync`): the 16:9 hold (`GT4_FORCE_WIDESCREEN`), the
  display aspect request, the 480p race hold (`GT4_FORCE_480P`), the flare limits of Performance mode and the field of
  view zoom. The Linux window host also holds 480p, as `window.cpp` does on Windows.
- **In the runtime's dispatch** (the license guard capsule): the license demo guard, the race camera aspect hook and the
  HUD canvas hook.
- **Configured at start** (`native/linux/linux_main.cpp`, as `game_main.cpp` on Windows): the 360-degree camera, the
  field of view, the trigger range and the frame skip read their switches; an invalid value stops the start.

## Inputs reproduced on Linux

| Input | How | Result | Label |
|---|---|---|---|
| Patched ELF | `aspect_elf_patch.patch_elf` on the verified ELF | `SCUS_973.28.aspect.elf` `f10a568f...`, as BUILD.md slice217 | Confirmed |
| Aspect units | the Linux translator with `--stats --entries docs/inputs/slice208-ee-entries.txt` (1,105 seeds, pinned) | 138,813 entries, 0 rejected seeds; inventory **equals the release source pin** `32feca3b...` | Confirmed |
| Release units | `camera360_units.py v200 <aspect units> <out>` | inventory **equals the release output pin** `83387682...` (slice408 `camera360_units`) | Confirmed |
| Runtime capsule | `prepare_perf_runtime.py <out> --gs-stream --frame-cost` | the 7 sources equal `slice262-frame-cost-runtime.json` (the release's source capsule) | Confirmed |
| License guard capsule | `prepare_license_guard.py <out> --display-aspect --frame-counters`, with `scripts/license_guard_overlay.inc` as of git `bc6d3d04` | the 3 sources equal the slice408 `license_guard_source_sha256` | Confirmed |

The license guard overlay changed after slice262 (`abd4347c`, the Spec II guard), while the v2.00 release still uses
the slice262 capsule. Its pinned hash (`45b06349...`) is the `bc6d3d04` version, so the Linux derivation uses that file
through a wrapper (below); the repository file is unchanged.

## Results (KDE Wayland, RX 6600, CoreGS 2.0 core 2 at 3x with FXAA, sound)

| Check | Result | Label |
|---|---|---|
| CTest, the release-patched build | 268/268 | Confirmed |
| Patches at start, launcher settings | `[camera360] on`, `[fov] on`, `[trigger] full range on`, `[aspect] auto`, `[display] races held at 480p` | Confirmed |
| In the game | the 480p hold set the race display index from 0 (normal) to 1 (480p); the 16:9 hold set the game's wide flag | Confirmed |
| 1,200 frames | all intervals 16-17 ms, 0 late, 0 underruns; guest cycles 5 more than without the patches (the patches' own writes) | Confirmed |
| 21:9 window (`GT4_OUTPUT_SIZE=1680x720`) | `[aspect] auto (output size 1680x720, 21.00:9)`; menus pillarboxed at 16:9 as on Windows | Confirmed |
| Tester playing it (2026-11-21) | ultrawide works; the image is steady and sharp | Confirmed (tester report) |
| The 360-degree camera, the zoom and the trigger range, seen by a person | not reported | Unknown |

## How to build and play

```sh
P=private/tooling-venv/bin/python
# The release units (after the L1 producer): patched ELF, aspect translation with the pinned seeds, the hook sites.
$P - <<'EOF'
import sys, subprocess
from pathlib import Path
sys.path.insert(0, 'scripts')
import aspect_elf_patch as ap, ee_entries
out = Path('private/<aspect>'); out.mkdir()
(out / 'SCUS_973.28.aspect.elf').write_bytes(ap.patch_elf(Path('private/<producer>/SCUS_973.28.elf').read_bytes()))
seed_file, seeds, pin = ee_entries.seed_set_pin(ap.SEED_SET)
subprocess.run(['private/<producer>/tool-build/eerecomp', str(out / 'SCUS_973.28.aspect.elf'), '--out',
                str(out / 'generated'), '--stats', '--entries', str(seed_file)], check=True)
EOF
$P scripts/camera360_units.py v200 private/<aspect>/generated private/<release-units>
# The runtime capsules
$P scripts/prepare_perf_runtime.py private/<frame-cost> --gs-stream --frame-cost
git show bc6d3d04:scripts/license_guard_overlay.inc > /tmp/license_guard_overlay.slice262.inc
$P -c "import sys; sys.path.insert(0,'scripts'); import prepare_license_guard as g; from pathlib import Path; \
g.GUARD_OVERLAY_PATH=Path('/tmp/license_guard_overlay.slice262.inc'); \
g.prepare(Path('private/<guard>'), display_aspect=True, frame_counters=True)"
# Configure as in L2-COREGS.md, with the frame-cost capsule and two more options
cmake -S . -B <build dir> ... -DGT4_LINUX_RUNTIME_CAPSULE=$PWD/private/<frame-cost> \
  -DGT4_LINUX_LICENSE_GUARD=$PWD/private/<guard> -DGT4_LINUX_GAME_UNITS=$PWD/private/<release-units>
```

CMake refuses units, a guard or a capsule that differ from their pins. Then play with the Windows launcher's defaults:

```sh
scripts/linux/play.sh <disc image> <memory card>       # GT4_FULLSCREEN=1, GT4_OUTPUT_SIZE=2560x1080, ... as needed
```

## Changes

- `native/linux/linux_main.cpp` (new): the Linux main, the non-standalone path of `game_main.cpp`.
- `native/linux/sdl_window.cpp`: the 480p hold at every VBlank, the display aspect's window size (start and resize)
  and race view presentation (to CoreGS through the GS stream seam, else the native Vulkan backend), the display
  switches validated at start, `GT4_FULLSCREEN` and `GT4_OUTPUT_SIZE`.
- `cmake/LinuxHost.cmake`: the slice262 frame-cost capsule as a third accepted profile; `GT4_LINUX_LICENSE_GUARD`
  (checked against slice408); `GT4_LINUX_GAME_UNITS` (checked against the slice408 inventory pin at configure time,
  built with the fast headers); `gt4_patch_hooks` (the three hook files) in every game executable's link group; the
  window executable runs the port's main as `gt4_donor_main`.
- `scripts/linux/play.sh` (new): the launcher's default settings for a direct start.

## Not yet on Linux

- The frame skip's host side (TASK-082, the launcher's 30 FPS setting): `native/window.cpp` `note_frame_skip_vblank`
  and its pacing inputs. `GT4_FRAME_SKIP` is read at start but nothing acts on it yet.
- The other release capsules: memory card flush (crash-safe saves, slice200), IPU fast decoder (slice365), SPU2
  (slice396), USB (racing wheels).
- The standalone start (`game.ini`), racing wheels, RetroAchievements, the performance HUD, input recording.
- Spec II (its own units, capsules and executable).
