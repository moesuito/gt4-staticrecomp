# Linux game window (TASK-074, plan step L2, first part)

Status: **GT4 runs in a Linux window** (2026-11-20): picture through the native Vulkan GS backend, sound, keyboard
and controller through SDL3, on Wayland and on X11 (XWayland). This is the first playable Linux build, a preview: it
has the base game only, not yet the Windows build's game patches, frame clock or other extras listed below.
Gameplay has not been tested by a person yet.

## Results (KDE Plasma Wayland session, RX 6600 with RADV, PipeWire, SDL 3.4.20)

| Check | Result | Label |
|---|---|---|
| Wayland window (`SDL_VIDEO_DRIVER=wayland`) | Vulkan surface attached, 1280x720 swapchain, 595 presentations in 600 frames, about 10.7 s (60 Hz FIFO) | Confirmed |
| X11 window through XWayland (`SDL_VIDEO_DRIVER=x11`) | the same: surface attached, 595 presentations, about 10.7 s | Confirmed |
| Audio on, paced by the sound device | 600 frames in 10.3 s, clean exit | Confirmed (sound not heard by the agent) |
| Exit | exit code 0 on both window systems (after the shutdown-order fix below) | Confirmed |
| CTest, Linux tree | 41/41: adds `native_keyboard_scancodes` and `native_linux_pacing`; `native_vulkan_extensions` now also checks the Linux selection | Confirmed |
| Python | `test_linux_surfaces.py` 3/3 (synthetic hooks, refused anchors, real derivation changes only the attach) | Confirmed |
| Frame pacing, paced (default) + FIFO, 1,200 frames with sound | **all 1,199 VBlank intervals in the 16-17 ms bucket**, mean 16.666 ms; 0 late frames, 0 audio underruns, 0 ring overflow; audio queue median 2,858 frames (target 2,880) | Confirmed |
| Frame pacing, paced + MAILBOX | the same: 1,199 of 1,199 in 16-17 ms, 0 late, 0 underruns, queue median 2,909 | Confirmed |
| Earlier behaviour (`GT4_PACING=audio`) + FIFO, for comparison | uneven: 269 intervals under 12 ms, 837 at 20-25 ms, 67 at 18-20 ms; queue median 3,328 | Confirmed |
| Linux frame clock (`native_linux_pacing`) | 60 grid waits: never early, lateness median 0.1 us, maximum 51 us | Confirmed |
| Gameplay by a tester (2026-11-20/21) | plays smoothly with the AOT build, sound plays well, no flicker; a DualShock 4 works through SDL3 | Confirmed (tester report) |
| DualShock 4: rumble, analog sticks, analog triggers; fullscreen toggle | all work | Confirmed (tester report, 2026-11-21) |
| Xbox and other controllers, `GT4_PAD` selection | not reported | Unknown |

## How to run it

After the [L1 steps](L1-VULKAN.md#how-to-reproduce), derive the port with Linux surfaces and point the build at it:

```sh
private/tooling-venv/bin/python scripts/prepare_external_vulkan.py private/<port> --linux-surfaces
cmake -S . -B <build dir> ... -DGT4_LINUX_PORT_DIR=$PWD/private/<port>    # other options as in L1
cmake --build <build dir> --target gt4_linux_window
<build dir>/gt4_linux_window private/<producer>/SCUS_973.28.elf --window --ntsc --disc <iso> --memcard <card.ps2>
```

`--fullscreen` starts in fullscreen. Without `--frames` the game runs until the window is closed. The memory card file
is created on first use. Environment:

- `GT4_CONTROLS_FILE=<file>`: a controls table such as `docs/controls/keyboard-only.toml` (default: the built-in map,
  the same keys as the README table).
- `GT4_PAD=<guid>[/<serial>]`: the controller to read (default: the first one connected).
- `GT4_PACING=paced` (default) or `audio`: the frame clock releases every VBlank on a 60 Hz grid and the sound is
  resampled to follow it, or (the earlier rule, for comparison) the sound device paces the game.
- `GT4_AUDIO_LATENCY_MS=20..500`: the paced audio queue target (default 60). `GT4_PACING_MIN_INTERVAL_US`: the shortest
  gap between two releases (default 15700).
- `GT4_PRESENT_MODE=fifo` (default), `mailbox` or `immediate`.
- `GT4_AUDIO=none`: no sound; the frame clock paces.
- `SDL_VIDEO_DRIVER=x11` or `wayland`: choose the window system (default: SDL's choice, Wayland on a Wayland session).
- `VK_DRIVER_FILES=/usr/share/vulkan/icd.d/lvp_icd.x86_64.json`: software Vulkan (slow; for checks only).

Keys: the controls table (default W/Enter = Cross, S = Square, Space = Circle, Backspace/Esc = Triangle, arrows/A/D =
D-pad, Q/Shift = L1/R1, F/R = L2/R2, C/Tab = Select, P = Start), F11 or Alt+Enter = fullscreen, F8 = pacing report
(printed to the terminal; it also prints when the game closes).

## Design

- **Host** `native/linux/sdl_window.cpp` implements `native/window.hpp` for the port's `main.cpp`, as
  `native/window.cpp` does on Windows, with the same shape: one thread, and at every guest VBlank the window events,
  the pad, `ps2::gs::present()` and the pacing. Its SDL handling follows the donor's `host/window.cpp`, ported to SDL3
  (attribution in `native/THIRD_PARTY.md`).
- **Surface**: SDL3 reports the X11 display and window, or the Wayland display and surface, and the derived port's
  `attach_surface` passes them to `Context::attach_x11` / `attach_wayland` in `native/native_vulkan.cpp`, which create
  the Vulkan surface (`VK_KHR_xlib_surface`, `VK_KHR_wayland_surface`). The swapchain and presentation code is the
  Windows code, unchanged. Xlib and Wayland headers are not included: the four types the Vulkan platform headers name
  are declared instead, so Xlib's macros stay out.
- **Port**: `scripts/prepare_external_vulkan.py --linux-surfaces` splices the X11/Wayland attach into the derived
  `gpu.cpp` with checked, reversible anchors (the existing `--gs-stream-hooks` pattern). Without the option the
  derivation is byte-identical to before; `native/gpu_*.inc` is not edited.
- **Instance extensions**: `select_linux_instance_extensions` in `native/vulkan_extensions.cpp` (pure, fixture-tested):
  `VK_KHR_surface` with every available X11/Wayland surface extension and the same swapchain-maintenance family logic
  as Windows (now one shared helper). With no window-system extension the instance is headless.
- **Input**: the controls table (`controls.cpp`, unchanged) gives keyboard bindings as Win32 virtual-key codes;
  `native/keyboard_scancodes.cpp` translates them to USB HID scancodes, which are SDL's scancode values (the fixture
  checks every nameable key against SDL3's own constants). Gamepads use `read_sdl_gamepad`, moved unchanged from
  `host_io.cpp` to `native/sdl_gamepad.cpp` so both hosts share one table, then `gt4::pad::map_reading`.
- **Pacing** (the Windows host's design; `pacing.hpp` is shared and unchanged): `FrameClock` on Linux is
  `CLOCK_MONOTONIC` with an absolute `clock_nanosleep` and a 0.5 ms final spin (`native/frame_clock.cpp`, behind
  `#ifndef _WIN32`). At each VBlank `release_frame` waits for the `FramePacer` grid deadline (60 Hz, the donor's guest
  rate, as on Windows), at least 15.7 ms after the previous release, and restarts the grid a full period behind.
- **Audio** (`native/linux/sdl_audio.cpp`): an SDL3 stream at 48 kHz stereo. Paced: the guest's sound goes through
  `LinearResampler` into a `FrameRing`; each VBlank the `RateController` sets the ratio (within ±0.5 %) from the SDL
  queue plus the ring, and 256-frame blocks go to SDL while the queue stays within the target; underruns and overflow
  are counted. Audio mode: the earlier 80 ms rule. The F8 report is `pacing::Diagnostics`, as in the Windows F8.
- **Exit**: the window and SDL stay open until the process ends, because the port's global Vulkan context releases
  its swapchain and surface in its destructor at exit. Destroying the window first crashed (segfault on Wayland, abort
  on X11); now both exit with code 0.

## Not yet on Linux

- The Windows pacing inputs that come from other features: the frame skip (TASK-082) and the presentation latency
  shift (IN-521). A display whose refresh rate is not 60 Hz has not been measured (Unknown).
- Wayland and FIFO: a compositor may stop presenting to a hidden or minimized window, and FIFO then blocks the game
  until it is shown again. **Hypothesis**, not tested; `GT4_PRESENT_MODE=mailbox` paces the same (measured above) and
  does not block.
- `game_main.cpp` features: the game patches (widescreen and display aspect hooks, 360-degree camera, FoV, trigger
  range, flare limits), `game.ini` / standalone start, the memory card lock and backups.
- Racing wheels, RetroAchievements, the F7/F8 performance probe, input recording and replay, cursor auto-hide.
- VU1/VU0 AOT capsules, CoreGS 2.0 and the GS process (gt4-gs).
- The Windows build of these changes has not been compiled or tested.
