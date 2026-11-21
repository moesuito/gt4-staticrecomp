# Linux build: plan (TASK-074, branch `linux/main`)

Status: **planned, not started** (owner, 2026-10-19). The goal is a native Linux x86-64 build of GT4Recomp:
the same game, the same PCSX2 GS renderer (built for Linux) and CoreGS 1.1, Vulkan everywhere.

## Why it is cheap

Confirmed by a code scan on 2026-10-19 (main 20cd762):
- **The game code is portable.** The translated EE units, the VU1 and VU0 AOT output and the donor runtime
  (ps2-web-recomp, which also targets the web) contain no Windows API. They are plain C++ for clang.
- **Graphics are Vulkan.** Mesa RADV is first class on AMD.
- **The PCSX2 GS already builds natively on Linux.** Only our gt4-gs frontend is Win32-specific.
- **The launcher is Tauri 2,** which is cross-platform.
- **The Windows-specific layer is thin.** It is the native adapter, listed below.

## Windows-specific pieces to replace

| Area | Files | Today | Linux replacement |
|---|---|---|---|
| Window, fullscreen, keys, cursor auto-hide | `native/window.cpp`, `window_mode.cpp`, `cursor_autohide.cpp` | Win32 | SDL3 window |
| Pads and keyboard | `native/host_io.cpp/.hpp` | XInput, Win32 keys | SDL3 gamepad (XInput, DualShock and DualSense layouts; analog L2/R2 pressure kept) |
| Audio | `native/host_io.cpp` | WinMM | SDL3 audio, which uses PipeWire/Pulse |
| Frame clock and pacing | `native/frame_clock.cpp/.hpp`, `pacing.cpp` | QueryPerformanceCounter, waitable timers | `clock_gettime(CLOCK_MONOTONIC)` and `clock_nanosleep` |
| Vulkan surface and present | `native/native_vulkan.cpp/.hpp` | `VK_KHR_win32_surface` | SDL3 `SDL_Vulkan_CreateSurface` (X11 and Wayland) |
| GS stream IPC | `native/gs_stream/gs_stream_ipc.cpp`, `gs_stream_client.cpp` | named file mapping, events, Job Object | `memfd`/`shm_open` with futex or eventfd; child process killed with the parent (`PR_SET_PDEATHSIG`) |
| Install paths, memory card, perf probe, shape capture export | `install_paths.cpp`, `memcard_store.cpp`, `perf_probe.cpp`, `shape_capture_export.cpp` | Known Folders, Win32 file APIs | XDG dirs (`~/.local/share/gt4recomp`), POSIX atomic rename and fsync |
| Build | `CMakeLists.txt` (refuses non-WIN32, requires clang-cl) | clang-cl, MSVC ABI | clang++ on Linux; keep the Windows path unchanged |
| Game library | pinned `ps2game.lib` | MSVC-ABI static library | rebuilt as `libps2game.a` with the same producer (tools/ee_producer) on Linux; new pins |
| Launch scripts | `scripts/play_performance.py`, `external_runtime_probe.py` | Windows paths, `.exe` | platform-neutral paths; Linux pins |
| gt4-gs frontend (fork `moesuito/gt4recomp-gs`) | `pcsx2-gt4gs/Main.cpp`, `StreamTransport.*` | Win32 child window, ACG, Job Object | POSIX transport; its own window (below) |

## The one real design change: who owns the window

On Windows, gt4-gs draws into a child window of the game window. Wayland does not allow embedding another
process's window. On Linux, **gt4-gs owns the game window** and presents to it; the game process has no
visible window when the PCSX2 GS is active.
- Input and focus: gt4-gs forwards window events (keys, focus, close, resize) back to the game over the GS
  stream's existing CONTROL channel.
- Pads can be read by the game process directly through SDL3; they do not need focus.
- With CoreGS 1.1 the game process owns its own window as usual.
- Fallback: X11 child embedding (XEmbed) is possible but not needed.

## Steps

TASK-074, in order. Each step is a short agent session with tests.
1. **L0, platform seam:** a small `platform/` layer (window, input, audio, clock, IPC, paths) with the existing
   Win32 code moved behind it unchanged. Windows behaviour stays byte-identical: the headless 42/42 frame dumps and
   CTest/Python must stay green.
2. **L1, build on Linux:** CMake accepts Linux and clang++. Rebuild the game library on Linux with the producer.
   Headless smoke with lavapipe (software Vulkan): this runs in WSL2 for correctness only.
3. **L2, SDL3 backend:** window, input, audio and clock on Linux, plus CoreGS 1.1 on screen. First playable build.
4. **L3, gt4-gs for Linux:** POSIX transport, gt4-gs owning the window, event forwarding. Build the fork's
   gt4-gs on Linux and add a Linux release asset.
5. **L4, packaging:** paths, memory card, a Linux distribution folder (tarball or AppImage), Linux pins and the
   launcher's Linux target.
6. **L5, owner test** on real Linux hardware. Also run the Windows build under **Proton/Wine** first; it may
   already work and is a zero-cost check.

## Testing environment

- **WSL2:** good for compiling and for headless correctness tests (lavapipe). Not for playing: its Vulkan runs
  through Dozen (Vulkan over D3D12), which is incomplete and slow, and AMD support there is limited.
- **Real tests:** a live USB or dual boot (Ubuntu, Fedora or Bazzite), or a Steam Deck.

## Rules

- Same rules as main (AGENTS.md): no JIT, MIT game executable, the gt4-gs fork stays GPL and separate.
- The Windows build must not change behaviour. SDL3 is zlib-licensed and fine to link.
- SDL3 also prepares the Android phase (TASK-022).
