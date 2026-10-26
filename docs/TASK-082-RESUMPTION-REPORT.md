# TASK-082 Resumption Report: Native Variable Frame Rate & Controlled Render Skip

**Date:** 2026-10-26  
**Status:** Deferred by owner (blocking visual defect identified during laptop testing). Preserved for future resumption targeting CoreGS 2.0.  
**Branch:** `task/082-render-skip`  
**Implementation Commit:** `8da7bff6d4b19dde6f0d7eb09683cb52102ddd89`  
**Base:** `task/116-117-flare-perf` (merged at `0abe11f`)  
**Pull Request:** PR #113 (Draft, unmerged)  

---

## 1. Executive Summary and Owner Decision

On 2026-10-26, following owner hardware testing of the experimental portable build `rc7-test082`, the owner identified a blocking visual defect where old frames flashed repeatedly between newer frames during render-skipping under GPU load. Subsequent video review and recompiled EE disassembly analysis revealed that the guest engine's double-buffer flip unconditionally alternates display pointers to an unupdated framebuffer on skipped frames.

The owner decided to **defer further frame-skipping development** on the legacy GSdx external stream architecture, redirecting immediate engineering focus to **CoreGS 2.0** reaching GSdx-equivalent graphical accuracy, required features, and GPU performance/cost at the same settings.

TASK-082 is frozen in this documentation-only checkpoint. All experimental runtime code, timing logic, flare draining, launcher toggles, and test fixtures are preserved in branch `task/082-render-skip` for resumption targeting CoreGS 2.0.

---

## 2. Owner Test Conditions and Observed Defect

### 2.1 Hardware Environment
- **Device:** Laptop with Intel Core i5-7200U (2 cores / 4 threads, 15 W shared CPU/GPU power budget).
- **GPU:** Intel HD Graphics 620 (integrated).
- **Display Configuration:** 2x internal resolution upscale, windowed/borderless.
- **Game Modes:** Tested in both B-Spec and A-Spec. The defect was confirmed identical in both modes.

### 2.2 Baseline Observations (Without Skip vs. With Skip)
- **Without Render Skip (`GT4_NATIVE_FPS` unset):** The i5-7200U laptop ran at ~30–40 FPS under heavy rendering load (e.g. multi-car grids, complex tracks), resulting in severe game slowdown because physics and game clock slowed proportionally with rendering.
- **With Render Skip (`GT4_NATIVE_FPS=skip`):** The game ran at apparently normal simulation speed. However, RivaTuner Statistics Server (RTSS) reported ~60 FPS with extreme frametime variance, and severe visual corruption occurred.

### 2.3 Verified Visual Evidence
1. **Video 1 (`C:\Users\Alano\Desktop\20261026_043310.mp4`, Host VSync Enabled):**
   - Captured at ~59.94 FPS via mobile recording.
   - At ~7s: An older scene showing the red car with HUD Total Time `0:00.983` flashed between advancing frames (`0:06.433`, `0:06.466`, `0:06.500`, ...).
   - At ~10s: The exact same `0:00.983` view continued to recur between advancing frames (`0:09.983`, `0:10.016`, ...).
   - Evidence contact sheets:
     - `private/task082-owner-video-20261026/sequence.png`
     - `private/task082-owner-video-20261026/sequence7.png`
     - `private/task082-owner-video-20261026/sequence10.png`
2. **Video 2 (`C:\Users\Alano\Desktop\20261026_051341.mp4`, Host VSync Disabled):**
   - Host VSync was turned off (`VsyncEnable = false`).
   - The defect persisted, now manifesting as horizontal tearing/strips between disparate camera views rather than full-screen alternating flips.
   - At ~5s: A horizontal strip showing the side of the red #22 car appeared sliced inside a different track camera view.
   - At ~8s: The primary upper view showed the car from the front, while a lower horizontal slice showed a different side/wheel perspective.
   - Strip boundaries shifted across frames due to host swapchain presentation unsynchronized with display scanout.
   - **Conclusion:** Disabling host presentation VSync does not resolve the defect; the underlying display buffer being scanned out or swapped remains stale.

---

## 3. Double-Buffer Mechanics and Root Cause Analysis

### 3.1 Verified Code Facts (From Recompiled Assembly)
1. **Main Loop Structure:** The GT4 main loop thread (`func_001016E8` in recompiled unit `unit_0000.cpp`) executes each frame:
   - `0x004A0690`: PGL lock.
   - `0x001077F8`: Packet double-buffer toggle (`D_006184C0.cur = 1 - cur`).
   - `0x00101400`: Calls `forEach(manager A, main_update_functor [0x00100EE0])` and `forEach(manager B, main_render_functor [0x00100F50])`.
   - `0x00107840`: DMA channel 1 and 2 wait (`0x004A23F0`).
   - `0x004A06F0`: PGL unlock.
   - `0x00101430`: Calls `forEach(manager B, main_idle_functor [0x00100FC0])` (DMAsafe).
   - `func_00107878`: The main engine flip function (return address `0x00101818`).
2. **Flip Assembly (`func_00107878` in `slice217-ee-aspect-producer/generated/unit_0001.cpp` L4635–L4735):**
   - **L4679/L4687:** Calls `0x004A22C0(-1)` (VBlank wait / `SleepThread` on scratchpad counter `0x70000050`).
   - **L4703–L4707:** Reads `D_0061850C.cur` (RAM `0x0061850C + 120 = 0x00618584`) and unconditionally toggles it:
     $$\text{cur} = 1 - \text{cur}$$
     and stores the inverted word back to `0x00618584`.
   - **L4711:** Calls `0x00105608(&page[cur])`.
   - **Inside `0x00105608` (`unit_0001.cpp` L2207–L2228):** Reads page descriptors and calls `0x004A0C10` (`unit_00E8.cpp` L1259–L1300).
   - **Inside `0x004A0C10`:** Performs a direct 64-bit store to privileged GS register `DISPFB2` (`0x12000090`) with the new page base pointer `FBP`, as well as writes to `DISPLAY2` (`0x120000A0`), `PMODE` (`0x12000000`), `SMODE2` (`0x12000020`), and `BGCOLOR` (`0x120000E0`).
   - **L4715–L4717:** Toggles the secondary page tracker at `0x006184D0 + 0xB8 + 120 = 0x00618600`.
3. **Render Submission Target (`unit_0001.cpp` L1473–L1489):**
   - The render subsystem reads `cur` from `0x00618584` and targets `page[1 - cur]`.
   - Therefore: `cur` represents the **currently displayed page**, and `1 - cur` represents the **page currently being drawn to**.
4. **The Gate Engagement Point:**
   - In `native/render_skip.cpp`, the gate requires 60 consecutive frames of rising `clock_f898` (+50 per frame) before skipping is permitted (`good_frames_ >= 60u`).
   - Prior to frame 60, all frames rendered normally and toggled pages every frame.
   - At 60 FPS, frame 59 corresponds exactly to:
     $$\frac{59 \times 1000}{60} = 983.33\text{ ms} \quad (\text{Total Time } 0:00.983)$$
   - Frame 59 rendered to **Page 1**.

### 3.2 Runtime Observations (Commit `8da7bff`)
- In commit `8da7bff`, `native/render_skip.hpp` bypassed `main_render_functor` (`0x00100F50`) and `main_idle_functor` (`0x00100FC0`) on skipped frames.
- However, `func_00107878` (FLIP) ran unconditionally.
- On Frame 60 (Rendered): Render drew to Page 0 (Total Time `0:01.000`). Flip toggled `cur = 0` and pointed `DISPFB2` to Page 0.
- On Frame 61 (Skipped): Render was bypassed. Nothing drew to Page 1. Flip ran: toggled `cur = 1` and pointed `DISPFB2` to Page 1. Page 1 still held its last drawn contents from Frame 59 (`0:00.983`)!
- On Frame 62 (Rendered): Render drew to `1 - cur = Page 0` (Total Time `0:01.033`). Flip toggled `cur = 0` and pointed `DISPFB2` to Page 0.
- On Frame 63 (Skipped): Render bypassed. Flip toggled `cur = 1` and pointed `DISPFB2` back to Page 1 (`0:00.983`).
- Every subsequent rendered frame updated Page 0. Every subsequent skipped frame displayed Page 1, which remained permanently frozen at `0:00.983`.
- `on_privileged_write` posted alternating `DISPFB2` addresses every VBlank, causing `gt4-gs` to present the stale buffer every other frame.

### 3.3 Remaining Hypotheses
- **Packet Double Buffering:** `func_001077F8` toggles `D_006184C0.cur` (RAM `0x006184CC`) and opens a DMA chain (`0x004A1A00` / `0x004A1BB0`). On skipped frames, the DMA chain is opened but receives no geometry packets, and `0x00107840` waits on idle DMA channels. Whether repeated multi-frame skips require explicit packet-buffer reset or whether idle waiting is completely leak-free over long runs remains to be verified under sustained multi-skip conditions.
- **Split-Screen and Replay Cameras:** Replay cameras maintain a separate `keys[cur]` buffer (`func_0033B0E8`), and split-screen mode uses `RacePS2Base::render_world_split` (`0x0033FD88`). Their interaction with render skipping has not been exercised.

---

## 4. Presentation, VSYNC, and Backpressure Analysis

### 4.1 GS Stream VSYNC Flow
- In `native/window.cpp`, guest VBlank invokes `on_vblank()`, which calls `release_frame()` and `gs_stream::on_vsync()`.
- `Client::vsync()` in `native/gs_stream/gs_stream_client.cpp` posts a `RecordType::vsync` record to the shared memory command ring at 60 Hz.
- The external process `gt4-gs.exe` processes the stream sequentially and invokes `vkQueuePresentKHR` on every received `RecordType::vsync`.
- Consequently, `gt4-gs` executed 60 presentation calls per second regardless of whether a new game frame had been drawn. RTSS, which hooks `vkQueuePresentKHR`, reported ~60 FPS.
- Frametimes fluctuated dramatically because rendered frames took significant GPU rasterization time while skipped frames took ~0 ms, producing an alternating short/long presentation cadence.

### 4.2 External Cap Failure and Backpressure Coupling
- When the owner applied an external 30 FPS cap via RivaTuner on `gt4-gs.exe`, the entire game slowed down.
- **Cause:** In `native/gs_stream/gs_stream_ipc.cpp` (`Session::post_vsync`):
  ```cpp
  const std::uint64_t limit = c.frames_in_flight_limit; // Default = 2
  if (posted - load(c.vsyncs_done) > limit) {
    wait_until([&] { return posted - load(c.vsyncs_done) <= limit; }, ...);
  }
  ```
- If `gt4-gs` presents at 30 Hz, `vsyncs_done` advances once every 33.3 ms.
- When `gt4-recomp` posts VSYNCs at 60 Hz (every 16.7 ms), by the third VSYNC the ring has 2 unconsumed frames in flight and the game thread blocks on `space_waiter`.
- This backpressure directly throttles the EE simulation thread, proving that **an external present cap cannot produce independent simulation without decoupling stream VSYNC consumption from host presentation**.

---

## 5. Proposed Correction and Orchestrator Review Caveats

### 5.1 Proposed Minimal Protocol and Guest Fix
1. **Producer Side (`gt4-recomp`):**
   - In `gs_stream_protocol.h`, define:
     ```cpp
     inline constexpr std::uint32_t kVsyncFlagSkipPresent = (1u << 1); // bit 1
     ```
   - On skipped frames, post `RecordType::vsync` with `kVsyncFlagSkipPresent` asserted. Guest VBlank, field alternation, ring counters, and simulation clock advance normally at 60 Hz without IPC stall.
2. **Consumer Side (`gt4-gs`):**
   - In the GS consumer's VSYNC handler: when `kVsyncFlagSkipPresent` is set, execute pending GS state updates and advance `vsyncs_done`, but bypass `vkQueuePresentKHR`.
   - The host swapchain holds the previous completed frame buffer. RTSS reports genuine 30/20 FPS.

### 5.2 Orchestrator Review Caveats (Critical Limitations)
> [!WARNING]
> **Post-Flip RAM Restoration is Too Late:**  
> In `func_00107878`, `0x004A0C10` writes directly to the privileged hardware register `DISPFB2` (`0x12000090`) *during* the flip function execution.  
> Attempting to detect that a frame was skipped and restore `D_0061850C.cur` in `main_update_functor` at the start of the *next* frame is ineffective for hardware registers: by that time, `on_privileged_write` has already emitted the `RecordType::priv` packet with the stale `DISPFB2` address into the stream.  
> Any working fix must either:
> 1. Filter `on_privileged_write(0x12000090)` in `native/gs_stream/gs_stream_client.cpp` during skipped cycles; OR
> 2. Intervene in `func_00107878` before `0x004A0C10` executes, preventing the register write from occurring.

> [!IMPORTANT]
> **Preserve Guest VBlank vs. Host VSync Distinction:**  
> Guest VBlank (`0x004A22C0`) drives the 60 Hz fixed simulation step, input polling, and audio resampler. It must never be throttled or dropped. Host VSync (`vkQueuePresentKHR`) is the display presentation call, which can and should run at variable rates (30, 20, 60 Hz).

---

## 6. Reusable Work vs. GSdx-Specific Work

| Component | Architecture | Reusable for CoreGS 2.0? | Notes |
|---|---|---|---|
| `SkipManager` debt policy (`render_skip.cpp`) | Game-side EE | **Yes (100%)** | Pacing accumulator and debt math are renderer-agnostic |
| Monotonic clock gate (`clock_f898` +50) | Game-side EE | **Yes (100%)** | Validates stable race dynamics before skip engagement |
| Flare queue draining (`clear_flare_queues`) | Game-side EE | **Yes (100%)** | Prevents flare buffer overflow when skipping passes |
| Functor bypass (`0x00100F50`, `0x00100FC0`) | Game-side EE | **Yes (100%)** | Pure guest dispatch bypass |
| Launcher toggle (`skip_slow_frames`) | Launcher | **Yes (100%)** | Exposes user option, guards GT4 v2.00 |
| `kVsyncFlagSkipPresent` in `VsyncPayload` | GS Stream IPC | **No (GSdx only)** | Obsolete under in-process CoreGS 2.0 |
| `frames_in_flight_limit` backpressure | GS Stream IPC | **No (GSdx only)** | IPC-specific flow control artifact |
| `on_privileged_write` stream filtering | GS Stream IPC | **No (GSdx only)** | CoreGS 2.0 handles registers in-process |

---

## 7. Future CoreGS 2.0 Integration Requirements

When TASK-082 is resumed targeting CoreGS 2.0:
1. **Prerequisite Parity Gate:** CoreGS 2.0 must first achieve complete graphical equivalence with GSdx, implement all required render features, and match/exceed GPU performance/cost at identical settings.
2. **In-Process Simplicity:** Because CoreGS 2.0 runs in-process with the game engine, there is no IPC serialization, no shared memory ring, and no `frames_in_flight_limit` backpressure.
3. **Direct Presentation Bypass:** On a skipped cycle, CoreGS 2.0 simply refrains from calling `vkQueuePresentKHR` on its swapchain, holding the previous framebuffer image without stream protocol flags.
4. **Framebuffer Retention:** CoreGS 2.0's internal texture cache will bind and display the last completed frame texture directly, avoiding guest VRAM page pointer mismatch.
5. **No Premature Changes:** Do not modify CoreGS 2.0 implementation code during the interim.

---

## 8. Exact Build Recipes, Artifacts, and Pins

### 8.1 Build Recipe
- **Compiler:** `clang-cl` with Visual Studio 2022 x64 toolchain.
- **CMake Configuration:** C++20, Windows x86-64.
- **Build Directory:** `C:\Antigravity\gt4-wt-082-render-skip\build-p1`.
- **Command:**
  ```powershell
  cd C:\Antigravity\gt4-wt-082-render-skip
  cmake --build build-p1 --config Release
  ```

### 8.2 Existing Pins and Artifacts
- **Runtime Capsule Pin:** [`docs/inputs/slice311-task082-render-skip-runtime.json`](file:///C:/Antigravity/gt4-wt-082-render-skip/docs/inputs/slice311-task082-render-skip-runtime.json)
  - Receipt SHA256: `a0401048981082db8b2abc2224f59be4764e061ddafd3e718599a0af06f9660c`
- **PCSX2 Distribution Pin:** [`docs/inputs/slice313-pcsx2.json`](file:///C:/Antigravity/gt4-wt-082-render-skip/docs/inputs/slice313-pcsx2.json)
- **Experimental Test Kit:**
  - Directory: `C:\Users\Alano\Desktop\GT4Recomp-1.3.0-rc7-test082-portable`
  - Archive: `C:\Users\Alano\Desktop\GT4Recomp-1.3.0-rc7-test082-portable.zip` (SHA256 `6db33331af191560b54c850c2a5d31fb2cc0a3bfecb6552b48061d3bcd9efee9`)
  - Game Executable: SHA256 `71db977912ec55e3354589080437da6bb7c631f4715942a4f25a8868b3750a86`
  - Launcher Executable: SHA256 `27aec5237d67c4cef81b8503d378fd912ba18c4ef3691ab97d3335f806159854`

---

## 9. Test Baseline and Known Failures

### 9.1 CTest Verification
- Running `ctest --test-dir build-p1 --output-on-failure` yields:
  - **48 passed, 1 failed (Test 35: `python_native_contracts`).**
  - All 48 native C++ tests pass 100%, including `native_render_skip`, `native_fps_probe`, `native_gs_stream_client`, `native_mtvu_*`, and `vu_recomp_*`.

### 9.2 Verification of Python Test 35 Baseline Origin
- Test 35 runs `unittest discover` over `tests/python/`.
- It reports 13 failures in `tests/python/test_gs_settings.py` (e.g. `test_reset_gives_the_exact_default_ini`).
- **Baseline Audit:** These 13 failures are caused by `LoadTextureReplacementsAsync = true` being added to the default INI in the RC6 merge base (`task/116-117-flare-perf` at `0abe11f`), which differs from `LEGACY_DEFAULT`.
- **Verdict:** The failures are verified to predate and be completely independent of TASK-082. TASK-082's dedicated test suites (`test_render_skip.py` [5/5 pass] and `test_fps_probe.py` [7/7 pass]) pass 100%.

---

## 10. Outstanding Validation Matrix for Resumption

When resumed on CoreGS 2.0, the following quantitative validation criteria must be satisfied:

| Metric | Target | Measurement Method |
|---|---|---|
| **Simulation Speed** | Exactly 100% (3,000 clock units/sec) | $\Delta \text{clock\_f898} / \Delta t_{\text{wall}}$ |
| **Unique Rendered FPS** | Matches nominal budget (e.g. 30 FPS under 1:1 skip) | $\Delta \text{render\_frame\_counter}$ (+0xE32C) per wall second |
| **Host Present Calls** | Matches rendered FPS (no redundant presents) | `vkQueuePresentKHR` count via RTSS / PresentMon |
| **Present Pacing** | Consistent interval (e.g. $33.3\text{ ms} \pm 1\text{ ms}$) | Swapchain present interval histogram |
| **Audio Continuity** | Zero crackle, 0 underruns, 0 overflows | F8 diagnostic report audio counters |
| **Frame Chronology** | Monotonically non-decreasing timestamps | Video frame audit across camera cuts (B-Spec and A-Spec) |

---
