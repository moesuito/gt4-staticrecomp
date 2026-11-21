# CoreGS 2.0 on Linux (TASK-074, plan step L2)

Clean room: CoreGS 2.0 (`native/hw_gs`, TASK-027) may not be edited by an agent that has read PCSX2 sources. The
agent that wrote this port read no PCSX2 source, the `gt4recomp-gs` fork or any `private/tooling/pcsx2-*` or
`private/slice189-pcsx2-ref` material, and the changes below only replace Windows APIs, plus one shader workaround.

## Part 1: the libraries, shaders and tests (2026-11-21)

Status: **CoreGS 2.0 builds on Linux and passes its test suite on the RX 6600 (RADV)**. It is not yet connected to
the game; that is part 2.

### Results

| Check | Result | Label |
|---|---|---|
| Build (`-DGT4_LINUX_HW_GS=ON`, clang 23.1.4) | every CoreGS library, the 36 shaders (system glslangValidator 16.6.0), the writeback tests and the replay tool build | Confirmed |
| CTest on RADV (RX 6600, Mesa 26.2.5) | **258 entries: 257 pass, 1 skipped** (`hw_replace_q07_pcsx2_dumps` needs private texture dumps) | Confirmed |
| CTest on lavapipe (software, for comparison) | 253 pass; `hw_cache_equals_writeback` and `hw_scale_display_shadow` end in a segfault, `hw_core2_rop` and `hw_core2_prepare_shaders` fail | Confirmed; cause Unknown |
| Shaders | all built by `glslangValidator` 16.6.0 (`c4652e3f...`); the Windows build pins its own `glslangValidator.exe` by hash (IN-201), no Linux binary is pinned | Confirmed |

### A Mesa RADV bug, worked around in one shader

`hw_tone_hle_exact` (the tone-sequence strip dispatch, IN-383, which runs every race frame) failed on RADV only:
3,584 of 4,096 pixels wrong, exactly every strip but the first. The SPIR-V is valid (`spirv-val`) with the right
push-constant layout (`ArrayStride 4`), lavapipe gives the exact result, and RADV gives the same wrong result on every
run, with its ACO and its LLVM compiler alike. Reading `pc.quadrants[strip]` through constant indices (a `switch`)
makes RADV exact. So RADV (Mesa 26.2.5) mis-reads a push-constant array indexed by a value that varies per
invocation. `native/hw_gs/shaders/hw_tone_strips.comp` now reads it through the `switch`: the same value, on every
driver. The only other push-constant arrays (`core2_compose.comp` `c1`/`c2`) are read with constant indices. The
shaders are not pinned; Windows behaviour is unchanged by construction but has not been re-run.

### Source changes (all behind `#ifdef _WIN32` / `#ifndef _WIN32` unless noted)

- `hw_gs_vulkan_loader.*`: `libvulkan.so.1` with `dlopen`; the Xlib and Wayland surface entry points (each optional)
  instead of the Win32 one; the four X11/Wayland types are declared, not taken from Xlib.h. `<cstdint>` included
  (Windows got it through `<windows.h>`).
- `hw_gs_backend.*`: the present-wait poll sleeps with `std::this_thread::sleep_for` instead of a waitable timer;
  Linux instances enable `VK_KHR_surface` with the X11/Wayland extensions; new `Backend::attach_x11` and
  `attach_wayland` (the Win32 `attach_window` refuses on Linux); the window size for a surface that reports none
  (Wayland) comes from attach and `resize_window`; the performance HUD stays off on Linux with a message
  (`GT4_HW_HUD=1`).
- `core2/core2_gpu.cpp`, `core2/core2_renderer.cpp`: `GT4_HW_SHADER_CACHE`, `GT4_HW_TEXTURES` and
  `GT4_HW_TEXTURE_DUMP_DIR` are read with `getenv` (Linux paths are bytes); `<x86intrin.h>` and `<cmath>`.
- `writeback/hw_gs_vulkan_device_table.cpp`: the already loaded `libvulkan.so.1` (`RTLD_NOLOAD`).
- `hw_gs_stream_sink.cpp`: the profile lines' wall clock and CPU times through two helpers (QueryPerformanceCounter and
  GetThreadTimes/GetProcessTimes on Windows, `clock_gettime` on Linux). The Windows results are the same; the text
  moved into the helpers.
- `hw_gs_prepare.cpp`, `writeback/tests/hw_writeback_tests.cpp`, `writeback/hw_writeback_replay.cpp`: `_putenv_s`,
  `_getpid`, `_exit` and the 64-bit seeks from `native/host_compat.hpp` (which gains `_getpid` and `<unistd.h>`).
- New Linux-only files: `hud/hw_hud_system_linux.cpp` (the HUD system interface: no font, no readings) and
  `hw_cpu_sampler_linux.cpp` (the TASK-027 CPU sampler, a Windows diagnostic, off on Linux).
- `shaders/CMakeLists.txt`: the GPU exactness fixture links the Vulkan SDK import library on Windows (unchanged lines)
  and `Vulkan::Vulkan` on Linux.
- `cmake/LinuxCoreGS.cmake` (new): the targets and tests of `native/hw_gs/CMakeLists.txt` for Linux, which stays the
  Windows build file. Not built on Linux: `hw_gs_smoke` (a Win32 window tool) and `hw_cpu_sampler_fixture`;
  `hw_hud_fixture` is built but not registered (two checks need Windows to draw the font).

### How to build and test

```sh
cmake -S . -B <build dir> -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_COMPILER=clang++ -DGT4_LINUX_HW_GS=ON
cmake --build <build dir>
ctest --test-dir <build dir> -j 4
```

Needs `glslangValidator` (openSUSE: `glslang-devel`) and the Vulkan headers and loader; no private assets.

## Part 2: CoreGS in the game (2026-11-21)

Status: **the game runs on CoreGS 2.0 in a Linux window** (Wayland and X11), with the release profile's VU path,
the frame pacing and sound. The tester has not played it yet.

### Results (RX 6600, RADV, KDE Wayland, the AOT build)

| Check | Result | Label |
|---|---|---|
| `--gs-stream` runtime capsule | the 7 derived sources byte-identical to `slice191-gs-stream-runtime.json` (and slice200) | Confirmed |
| CoreGS core 1, scale 1, no sound, 600 frames, Wayland | 600 presents, 594 deinterlaced frames, all intervals 16-17 ms, exit 0 | Confirmed |
| **CoreGS 2.0 core 2 (`GT4_HW_CORE=2`), 3x, FXAA, sound, 1,200 frames, Wayland** | 1,200 presents, 1,199 deinterlaced; **all 1,199 intervals in 16-17 ms, 0 late frames, 0 underruns**; the same 5,898,359,180 guest cycles as every other renderer | Confirmed |
| CoreGS 2.0 core 2 on X11 (XWayland), 300 frames | 300 presents, exit 0 | Confirmed |
| CTest, the combined build (host, Vulkan, VU AOT, CoreGS) | **268/268** | Confirmed |
| Tester playing it (2026-11-21) | CoreGS 2.0 at 3x with FXAA: runs fine, the image looks good; the same executable without the `GT4_HW_*` / `GT4_RENDERER` switches (CoreGS 1.1) also looks good | Confirmed (tester report) |

### How to run it

Build as in [L2 performance](L2-PERFORMANCE.md#how-to-build-it) with these changes: `-DGT4_LINUX_HW_GS=ON`, a capsule
from `prepare_perf_runtime.py <out> --gs-stream`, and a port from
`prepare_external_vulkan.py <out> --gs-stream-hooks --linux-surfaces`. Then, with the launcher's CoreGS 2.0 settings:

```sh
GT4_RENDERER=hw GT4_HW_CORE=2 GT4_HW_SCALE=3 GT4_HW_POSTFX=fxaa GT4_HW_SHADER_CACHE=<folder> \
  <build dir>/gt4_linux_window <producer>/SCUS_973.28.elf --window --ntsc --disc <iso> --memcard <card.ps2>
```

`GT4_HW_SCALE` takes 1-12 (the internal resolution multiplier); leave out `GT4_HW_POSTFX` for no FXAA;
`GT4_CORE2_FSR_SHARPNESS` sets sharpening as the launcher does. Without `GT4_RENDERER=hw` the same executable uses the
native Vulkan backend (CoreGS 1.1).

### Changes

- `native/gs_stream/gs_stream_ipc.cpp`: the shared rings stay common; the Win32 session and child process are behind
  `#ifdef _WIN32`, and on Linux every session or process call refuses with "the GS process transport ... is not
  available on Linux yet" (GT4_RENDERER=pcsx2 and GT4_GS_CAPTURE need it; step L3).
- `native/gs_stream/gs_stream_client.cpp`: compiles on Linux. A `QueryPerformanceCounter` of the same shape
  (`CLOCK_MONOTONIC`, real values) keeps the timing code shared; thread CPU time from `CLOCK_THREAD_CPUTIME_ID`; the
  GSdx failure dialog goes to the log; the GS executable hash and the child-window style are Windows-only.
  New `attach_linux_window` (Linux only) gives an X11/Wayland window to the in-process sink.
- `native/gs_stream/gs_stream_sink.hpp`, `native/hw_gs/hw_gs_stream_sink.cpp`: `InProcessSink::attach_linux_window`
  (a member only on Linux, so the Windows struct is unchanged), calling `Backend::attach_x11` / `attach_wayland`.
- `scripts/prepare_external_vulkan.py`: with both `--gs-stream-hooks` and `--linux-surfaces`, the derived `gs.cpp`
  window hook calls `attach_linux_window` (checked, reversible anchor; `test_linux_surfaces.py` 4/4).
- `cmake/LinuxHost.cmake`: the capsule may equal either pinned profile (`slice178-main-runtime` or
  `slice191-gs-stream-runtime`); the second links `gt4_gs_stream_client` (with `GT4_GS_STREAM_HW`), the transport,
  CoreGS's stream sink and `hw_gs_prepare.cpp`, and requires `GT4_LINUX_HW_GS=ON` and the matching port.
- `native/linux/sdl_window.cpp`: the log names the renderer in use.

## The performance HUD on Linux (2026-11-21)

`GT4_HW_HUD=1` (the launcher's Performance HUD setting) works on Linux. `hud/hw_hud_system_linux.cpp` gives the HUD what
`hud/hw_hud_system.cpp` asks Windows for:

| Reading | Windows | Linux |
|---|---|---|
| Font | Consolas through GDI | the system's monospaced font (fontconfig `monospace`, here Source Code Pro) through FreeType, cut into the same atlas by the same rule |
| CPU | process times over all threads | `CLOCK_PROCESS_CPUTIME_ID` over all hardware threads |
| Memory | working set | resident set (`/proc/self/statm`) |
| GPU and video memory | PDH `GPU Engine` and `GPU Process Memory` of the game's process | the kernel's DRM client statistics of the game's GPU files (`/proc/self/fdinfo`: `drm-engine-*` ns, `drm-memory-vram`), once per DRM client |
| Refresh rate | the window's monitor | SDL3's current display mode, published by the window host (`native/display_refresh.hpp`) |

Results: `hw_hud` (the fixture's font checks, labelled for Windows) passes; in the game the sampler took 40 readings at
0.24 ms of processor each and the panel drew 1,200 presentations at 21.3 us each, with every VBlank interval in
16-17 ms; the game's own DRM statistics were present (amdgpu: `drm-engine-gfx`, `drm-memory-vram` 415,548 KiB).  Tester
report (2026-11-21): the HUD works, its text and numbers show correctly. A driver without DRM client statistics shows `--` for
the GPU lines.

### Not yet on Linux

- The CPU sampler and the GS process (GSdx, `GT4_RENDERER=pcsx2`).
- Save states with `GT4_RENDERER=hw` (refused by the GS stream hook, as on Windows).
- The launcher's other per-game settings that the Windows `game_main.cpp` applies: `GT4_FORCE_480P`,
  `GT4_FORCE_WIDESCREEN`, `GT4_DISPLAY_ASPECT`, frame skip, the game patches (next step).
- lavapipe: 4 CoreGS tests fail there (2 segfaults); RADV passes all.
