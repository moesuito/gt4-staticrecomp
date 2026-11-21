# Linux host-only build (TASK-074, plan step L1, first part)

Status: **built and tested on Linux** (2026-11-20). This is the first part of step L1 in [the Linux plan](PLAN.md): CMake
accepts Linux, and the platform-neutral host code builds and passes its CTest fixtures with Clang or GCC. The game
executable is **not** built on Linux yet; the game library and a headless smoke runner are covered in
[L1-GAME-LIBRARY.md](L1-GAME-LIBRARY.md).

## What it builds

`cmake/LinuxHost.cmake`, included by the root `CMakeLists.txt` only when `WIN32` is false (the Windows recipe is
unchanged and never reads it). It needs no private assets: no donor runtime, no EE game library, no SDL3 package, no
Python environment.

- Libraries, with the same names and sources as `native/CMakeLists.txt`: `gt4_pacing`, `gt4_quickfix` (plus
  `gt4_quickfix_v200_fixture` for `GT4_TARGET=spec2`), `gt4_vulkan_extensions`, `gt4_usb`, `gt4_memcard`,
  `gt4_license_guard`, `gt4_pad_mapping`, `gt4_perf_probe`, `gt4_mtvu_measure`, `gt4_mtvu_journal`, and
  `gt4_gs_stream_files` (the `.gscap` file format and the READ_FIFO probe, without the Win32 shared-memory transport).
- 36 CTest fixtures, with the same test names as the Windows tree so results compare one to one.
- The same code-generation switches: `GT4_SSE41` (default on), `GT4_X86_64_V3`, `GT4_FP_CONTRACT_OFF`, `GT4_TARGET`.

## Commands

From the repository root, with the build directory outside the checkout or under an ignored path:

```sh
cmake -S . -B <build dir> -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_COMPILER=clang++   # or g++
cmake --build <build dir>
ctest --test-dir <build dir> --output-on-failure
```

## Results (2026-11-20, openSUSE, Linux 7.3, x86-64)

| Compiler | Target | CTest |
|---|---|---|
| clang 23.1.4 | v200 | 36/36 passed |
| GCC 16.2.1 | v200 | 36/36 passed |
| clang 23.1.4 | spec2 | 36/36 passed |

**Not checked yet:** the Windows build with these source changes. The changes are written so the Windows compiler sees
the same code as before (every new path sits behind `#ifndef _WIN32`, and `native/host_compat.hpp` is empty on
Windows), but CTest and the Python suite still have to be run on Windows before a merge.

## Source changes

- `native/host_compat.hpp` (new): POSIX stand-ins for `_putenv_s` (an empty value removes the variable, as on Windows),
  `tmpfile_s`, `fopen_s`, `GetEnvironmentVariableA` and `DWORD`. Defines nothing on Windows.
- `native/perf_clock.cpp`: TSC calibration with `<cpuid.h>`, `__rdtsc` and `CLOCK_MONOTONIC` off Windows.
- `native/perf_probe.cpp`: thread CPU time from `CLOCK_THREAD_CPUTIME_ID` off Windows.
- `native/standalone_fixture.cpp`: the linked-image checks (decision 0075) use the host's own path form. Windows keeps
  its drive path and backslashes exactly as before; Linux checks the same rules with POSIX paths, since a backslash is
  an ordinary file-name character there.
- Six fixtures and `gs_stream/gs_stream_file.cpp` include `host_compat.hpp`.

## Not built on Linux yet (later steps)

| Piece | Why | Plan step |
|---|---|---|
| `gt4vulkan` | needs a window, input and audio on Linux; the Vulkan GS backend, `vulkan_fixture` and a headless Vulkan runner now build ([L1 Vulkan](L1-VULKAN.md)) | L2 |
| `tools/vu_recomp` | needs the private donor headers | L1 |
| `native_host_io`, `host_io_fixture`, `wheel_fixture`, `window_heartbeat_fixture`, `key_forward_fixture` | Win32 window, WinMM audio, XInput | L2 (SDL3) |
| `gt4_achievements` and its fixtures | WinHTTP, GDI+ | L2 |
| `gt4_gs_stream` IPC, `gs_stream_client`, `gs_stream_record`, `gs_live_check` | Win32 file mapping and events | L3 |
| `shape_capture_export`/`control` fixtures | Win32 file APIs (`gt4_install_paths` now builds on Linux) | L4 |
| `native/hw_gs` (CoreGS 2.0) | Win32 Vulkan loader and timers; TASK-027 clean-room area | L2, by a TASK-027 agent |
| The Python suite (`tests/python`) | expects `private/tooling-venv/Scripts/python.exe` and Windows binaries | L4 |

## Open question for later steps

A portable folder made on Windows saves a relative image link with backslashes (`ISOs\gt4.iso`, decision 0075). On
Linux that is one file name, not a folder and a file. If portable folders are meant to move between Windows and
Linux, the launcher or the game needs a defined separator rule; this step does not change that behaviour.
