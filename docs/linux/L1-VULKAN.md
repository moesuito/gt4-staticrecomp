# Linux Vulkan GS backend, headless (TASK-074, plan step L1, third part)

Status: **the native Vulkan GS backend runs on Linux, headless** (2026-11-20), on the RX 6600 (RADV) and on Mesa's
software driver lavapipe. This completes the build side of step L1 in [the Linux plan](PLAN.md), after the
[host-only build](HOST-BUILD.md) and the [game library](L1-GAME-LIBRARY.md). There is no window yet: presentation,
input and audio are step L2.

## Results

openSUSE, Linux 7.3, Mesa (RADV and llvmpipe, Vulkan 1.4.354), clang 23.1.4, Rust 1.99.0, naga 30.0.0.

| Check | Result | Label |
|---|---|---|
| SPIR-V from `tools/wgsl_spirv` (naga 30.0.0) on the pinned donor `gs.wgsl` | **all 9 shaders byte-identical to the Windows pins** in `slice114-native-base.json`; all pass `spirv-val --target-env vulkan1.2` | Confirmed |
| Port from `scripts/prepare_external_vulkan.py` | `gpu.hpp`, `gs.cpp`, `main.cpp` equal the slice114 pins (as CRLF); `gpu.cpp` matches no pin, since `native/gpu_*.inc` changed after slice114 and later ports are pinned with their own capsules | Confirmed / no pin to compare |
| CTest, full Linux tree | **39/39**: the 36 host fixtures plus `native_vulkan_pixels`, `native_vulkan_pixels_sync` (`GT4_GS_SYNC=1`) and `native_vulkan_pixels_lavapipe` | Confirmed |
| `vulkan_fixture` on RADV and lavapipe | full 4 MiB VRAM identical to the CPU rasterizer for overlapping sprites, channel mask, state restore and fragmented uploads in 10 pixel formats; display 640x448 to 2048x2048 match the CPU VRAM read; presentation skipped (headless) | Confirmed |
| Game through the Vulkan GS backend, 1,200 frames | runs on both drivers; the same 5,898,359,180 guest cycles as the CPU-raster run | Confirmed |
| Game library built with GCC 16.2.1 (`build_linux.sh ... g++`) | 327 objects; boots with the same 5,898,359,180 guest cycles and **12 of 12 frames byte-identical** to the clang library | Confirmed |
| Frames, lavapipe against the CPU rasterizer | **12 of 12 saved frames byte-identical** | Confirmed |
| Frames, RADV against the CPU rasterizer | 2 of 12 identical; 10 differ in alpha-blended pixels only (the translucent dialog box), by at most 3 per channel (histogram: 60,428 at -1, 66,454 at +1, 8,868 at -2, 7,006 at +2, 334 at ±3). Deterministic: a repeat run and a `GT4_GS_SYNC=1` run give the same frames | Confirmed |

The RADV difference is in the driver's arithmetic, not in synchronisation: lavapipe runs the same SPIR-V and matches
the CPU exactly, and RADV gives the same result every time and in synchronous mode. `vulkan_fixture` does not draw
blended primitives, so it does not catch this. **Unknown:** whether AMD's Windows driver shows the same difference
(the project's AMD testing is on Windows), and which shader operation is responsible. The shaders are pinned donor
code, so a fix belongs in a later, reviewed step with a blended-primitive fixture first.

Headless wall time for 1,200 frames: 1.4 s on RADV, 4.0 s on lavapipe, 10.4 s with the CPU rasterizer. These runs have
no frame pacing and draw 2D menus, so they say nothing about in-game performance.

## How to reproduce

After the [game library steps](L1-GAME-LIBRARY.md#how-to-reproduce):

```sh
cargo build --release --locked --manifest-path tools/wgsl_spirv/Cargo.toml --target-dir private/<wgsl-target>
private/<wgsl-target>/release/gt4-wgsl-spirv private/references/ps2-web-recomp/runtime/src/gs/gpu/gs.wgsl private/<shaders>
private/tooling-venv/bin/python scripts/prepare_external_vulkan.py private/<port>
cmake -S . -B <build dir> -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_COMPILER=clang++ \
  -DGT4_DONOR=$PWD/private/references/ps2-web-recomp -DGT4_LINUX_PRODUCER=$PWD/private/<producer> \
  -DGT4_LINUX_PORT_DIR=$PWD/private/<port> -DGT4_LINUX_SHADER_DIR=$PWD/private/<shaders>
cmake --build <build dir>
ctest --test-dir <build dir> --output-on-failure
<build dir>/gt4_vulkan_headless_smoke private/<producer>/SCUS_973.28.elf --ntsc --disc <iso> --frames 1200 \
  --memcard <run dir>/card.ps2 --frames-dir <run dir>/frames --frame-every 100
```

CMake refuses shaders that differ from the slice114 pins. To force the software driver, set
`VK_DRIVER_FILES=/usr/share/vulkan/icd.d/lvp_icd.x86_64.json`.

## Source changes

All new paths are compiled only outside Windows (`#ifndef _WIN32` / `#else`); the Windows code is the same text as
before, except that the Vulkan entry point list is now two named groups (below).

- `native/native_vulkan.hpp`: on Linux, no `<windows.h>`; `HWND` (the Win32 STRICT opaque-pointer form) and `HMODULE`
  (the `dlopen` handle) are declared, so the shared adapter `native/gpu_tail.inc` compiles unchanged. The `.inc` files
  are not edited, so the Windows port they derive stays the same.
- `native/native_vulkan.cpp`: Linux loads `libvulkan.so.1` with `dlopen`, enables no instance extension (headless) and
  its `attach_window` refuses with a message naming L2. The entry point list is split into `VULKAN_CORE_FUNCTIONS` and
  `VULKAN_PRESENT_FUNCTIONS` (surface and swapchain); `VULKAN_FUNCTIONS` is both, as before. Windows still requires
  both groups; headless Linux requires the core group only. `vkCreateWin32SurfaceKHR` is in the Windows-only
  `VULKAN_PLATFORM_FUNCTIONS`.
- `native/install_paths.cpp`: the executable folder comes from `/proc/self/exe` on Linux.
- `native/fixture.cpp`: the Win32 presentation block runs on Windows only; Linux prints that presentation was skipped.
  Every raster, display and readback check runs on both.
- `tools/ee_producer/CMakeLists.txt` (Linux branch): `-fPIE`, so a GCC-built library links into the default PIE
  executables (this GCC does not default to it). clang already defaults to `-fPIE`: a clang rebuild with the flag
  gives the same object inventory (`d5d86cc2...`). CMake's `CMAKE_POSITION_INDEPENDENT_CODE` was tried first and
  rejected, because it gives `-fPIC` for static libraries and changes clang's objects.
- `cmake/LinuxHost.cmake`: optional `GT4_LINUX_PORT_DIR` and `GT4_LINUX_SHADER_DIR` build `gt4_ps2runtime_vulkan`,
  `vulkan_fixture` with three CTest entries (lavapipe only when its driver file exists) and, with a recorded game
  library, `gt4_vulkan_headless_smoke` (the port's `main.cpp`, with `host_compat.hpp` supplying `_putenv_s`).

**Not checked yet:** the Windows build with these changes (CTest and Python on Windows), needed before a merge.

## What L1 still leaves to later steps

- A window, presentation, input and audio on Linux: step L2 (SDL3; CoreGS 2.0 there needs `glslangValidator`).
- VU1 and VU0 AOT on Linux (`scripts/prepare_perf_runtime.py` capsules), so a Linux run matches the production profile.
- A blended-primitive check in `vulkan_fixture`, and a decision on the RADV rounding difference.
- The movie, 3D scenes, races, audio and card writes have not run on Linux yet.
