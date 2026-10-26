# TASK-133 Stage 2 Report: Fresh Baseline Gate and Native RetroAchievements Integration

**Date:** 2026-10-26  
**Workspace:** `C:\Antigravity\gt4-wt-133-rc7-ra-controls`  
**Branch:** `task/133-rc7-ra-controls`  
**Reviewer:** Codex Orchestrator  
**Status:** Stage 2 Complete — Paused for Codex Review  

---

## 1. Executive Summary & Review Scope Disposition

This report details the execution and results of **Stage 2** for TASK-133 in strict accordance with `docs/tasks/TASK-133-STAGE-2-NATIVE-RA.md`.

### Review Disposition & Historical Correction
1. **Provenance & Wording Correction:** In response to the Codex review of commit `7b7eb35`, the report text regarding the binary hash difference of the reconstructed baseline build has been updated. The Ninja build log confirms that rebuild actions took place, but the specific source or environmental delta causing the hash deviation is officially classified as **UNKNOWN** unless independently proven. The published `v1.3.0-rc6` binary remains the immutable distribution baseline reference.
2. **Fresh Baseline Gate Completed:** A fresh baseline build was produced on drive `O:` (`O:\gt4recomp-scratch\slice330-rc6-baseline\build`) using unchanged task133 worktree sources (at exact tag `v1.3.0-rc6`, commit `58322229c0bd63a0652b0550c5ad2154bd907039`) and immutable inputs before any integration edits were staged.
3. **Bounded Native RA Integration:** Native RetroAchievements code was ported from stable `1.2.4` (commit `ad46df6`), adapted semantically with target guards (`gt4::target::active().id`), wired into `CMakeLists.txt` and `native/window.cpp`, and verified across both production targets (`v200` and `spec2`).
4. **Strict Scope Enforcement:**
   - No launcher code modified (`launcher/` strictly untouched).
   - No controller curves modified during Stage 2.
   - No frame-skipping, wheel bridge, or CoreGS edits.
   - No release drafting, publishing, or PR merging.

---

## 2. Fresh Baseline Gate (Drive O:)

### 2.1 Configuration and Build Environment
- **Source Tree:** `C:/Antigravity/gt4-wt-133-rc7-ra-controls`
- **Output Directory:** `O:\gt4recomp-scratch\slice330-rc6-baseline\build`
- **Toolchain Versions Measured:**
  - `clang-cl`: 22.1.3 (LLVM git commit `e9846648fd6183ee6d8cbdb4502213fcf902a211`, `x86_64-pc-windows-msvc`)
  - `ninja`: 1.13.2
  - `cmake`: 4.3.1
  - `cargo`: 1.99.0 (`5f94df478 2026-08-27`)
  - `python`: 3.14.2 (`C:/Antigravity/gt4-staticrecomp/private/tooling-venv/Scripts/python.exe`)
  - `SDL3`: 3.4.18 (`C:/Antigravity/gt4-staticrecomp/private/tooling/sdl3-3.4.18`)

### 2.2 Baseline Configuration Command
```powershell
cmd.exe /c 'call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\Tools\VsDevCmd.bat" -arch=amd64 -host_arch=amd64 >nul && cmake -S C:/Antigravity/gt4-wt-133-rc7-ra-controls -B O:/gt4recomp-scratch/slice330-rc6-baseline/build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_COMPILER="C:/Program Files/LLVM/bin/clang-cl.exe" -DGT4_TARGET=v200 -DGT4_EE_PROFILE=aspect -DGT4_ASSETS_ROOT=C:/Antigravity/gt4-staticrecomp -DGT4_PYTHON=C:/Antigravity/gt4-staticrecomp/private/tooling-venv/Scripts/python.exe -DGT4_PERF_RUNTIME_SOURCE=C:/Antigravity/gt4-staticrecomp/private/slice262-frame-cost-runtime -DGT4_PORT_DIR=C:/Antigravity/gt4-staticrecomp/private/slice200-port -DGT4_SHADER_DIR=C:/Antigravity/gt4-staticrecomp/private/slice153-pacing-shaders -DGT4_LICENSE_GUARD_SOURCE=C:/Antigravity/gt4-staticrecomp/private/slice262-license-guard-runtime -DGT4_MEMCARD_RUNTIME_SOURCE=C:/Antigravity/gt4-staticrecomp/private/slice200-memcard-runtime -DGT4_VU0_GENERATED_DIR=C:/Antigravity/gt4-staticrecomp/private/slice160-vu0-gen-r3 -DGT4_VU0_IMAGE_FILE=C:/Antigravity/gt4-staticrecomp/private/slice160-vu0-image/vu0_image.bin -DGT4_VU1_GENERATED_DIR=C:/Antigravity/gt4-staticrecomp/private/slice157-vu1-all-gen -DGT4_VU_CORPUS_DIR=C:/Antigravity/gt4-staticrecomp/private/slice155-vu1-full-corpus -DGT4_RELEASE_DISTRIBUTION=ON -DGT4_SDL3_ROOT=C:/Antigravity/gt4-staticrecomp/private/tooling/sdl3-3.4.18'
```

### 2.3 Baseline Binary Artifact
| Artifact | Size (Bytes) | SHA256 Hash |
| :--- | :--- | :--- |
| `O:\gt4recomp-scratch\slice330-rc6-baseline\build\gt4vulkan.exe` | 97,398,784 | `188fbbf27dac0120557f5c37ee7e1b9b374505fc7a2f10e62f12bec030f53a63` |

### 2.4 Baseline Test Results
- **CTest (47 tests):** 46 passed, 1 failed (Test #33 `python_native_contracts`), 0 skipped.
  - Log: `private/task133-baseline-logs/fresh-baseline-ctest.log` (SHA256: `ae87774565bae36727ceab98a61cf365896f36246ac48c4e823858fc5117aa4a`).
- **Python Suite (710 tests):** 655 passed, 13 failed, 42 skipped.
  - Failures: 12 in `test_gs_settings.py` (due to TASK-118 settings schema changes) and 1 in `test_gs_live.py`.
  - Log: `private/task133-baseline-logs/fresh-baseline-python.log` (SHA256: `d87585194079acf9555ae8ac4bfe803989745613ebb3f3c63881f89dc7789362`).
- **Baseline Gate Conclusion:** Baseline successfully reproduced; only pre-existing technical debt present. Integration released.

---

## 3. Native RetroAchievements Integration

### 3.1 Vendored Dependencies & Provenance
- Vendored `rcheevos` library at commit `bbc79b58a5a2a0b98dfbf2be6ca9e42c764661ce` under MIT License (`native/third_party/rcheevos/LICENSE`).
- Documentation updated in `native/THIRD_PARTY.md` detailing:
  - `rcheevos` upstream repository, commit hash, MIT license, and purpose.
  - Explicit documentation of required runtime Windows DLLs (`gdi32.dll`, `gdiplus.dll`, `winhttp.dll`).

### 3.2 Native Modules Integrated
- `native/achievements.hpp` & `native/achievements.cpp`: RA runtime manager, session management, event dispatching, and PS2 memory translation.
- `native/achievements_http.hpp` & `native/achievements_http.cpp`: Asynchronous, cancelable WinHTTP transport for RA API calls and badge image downloading.
- `native/achievements_overlay.hpp` & `native/achievements_overlay.cpp`: Win32 layered child window overlay using GDI+ for achievement unlock toasts, progression indicators, and leaderboard tracking.
- `native/achievements_fixture.cpp`: Comprehensive unit tests covering configuration parsing, disabled/invalid config handling, clean shutdown/reinit, inactive behavior safety, PS2 memory mapping, and end-to-end mock session triggers.
- `native/achievements_spec2_fixture.cpp`: Negative target fixture asserting that the Spec II target rejects RA initialization, performs no evaluations, and issues no HTTP requests.
- `native/achievements_http_fixture.cpp`: Integration fixture testing cancelable WinHTTP transport against a slow loopback server and verifying clean transport restart.

### 3.3 Semantic Adaptations & Target Isolation
1. **Target Guard:**
   - In `native/achievements.cpp`, `RuntimeManager::init` and `RuntimeManager::tick` explicitly check:
     ```cpp
     if (::gt4::target::active().id != ::gt4::target::Id::v200) {
         // RetroAchievements strictly disabled for Spec II and non-v2.00 targets.
         return;
     }
     ```
   - In `read_ps2_memory`, an identical check verifies `active().id == Id::v200`. For any other target (e.g., Spec II), the function zeroes the destination buffer and returns 0 bytes.
2. **User-Agent String:**
   - Updated from `GT4Recomp/1.2.4` to `GT4Recomp/1.3.0` in `native/achievements.cpp`.
3. **Preservation of Core Subsystems:**
   - Pacing (`native/pacing.cpp`, `frame_clock.cpp`), audio pump timing, MTVU/GS stream concurrency, and shutdown sequences remain unmodified and strictly respected.
4. **Lifecycle Hooks in `native/window.cpp`:**
   - `window_procedure`: `WM_MOVE` and `WM_SIZE` trigger `gt4::achievements::on_window_pos_changed(window)`.
   - `on_vblank`: `gt4::achievements::on_vblank(...)` called immediately before `ps2::gs::present()`.
   - `cleanup`: calls `gt4::achievements::shutdown()`.
   - `run_window`: calls `gt4::achievements::initialize(...)` after `on_vblank` hook registration.

---

## 4. Candidate Builds & Measurements

Separate, isolated build directories were created and compiled on drive `O:`.

### 4.1 Production Target 1: v2.00 Candidate (`slice331-rc7-ra-build`)
- **CMake Options:** Clang-cl for C and CXX, `GT4_TARGET=v200`, `GT4_EE_PROFILE=aspect`, production runtime capsules.
- **Build Directory:** `O:\gt4recomp-scratch\slice331-rc7-ra-build\build`
- **Output Artifacts:**

| Binary Artifact | Size (Bytes) | SHA256 Hash |
| :--- | :--- | :--- |
| `gt4vulkan.exe` | 97,699,840 | `5baa879e3b3975529529bff7a4b9ad5e3f2a534b604e90c191ebd0cea8704f99` |
| `achievements_fixture.exe` | 391,168 | `62e5b9d265172d9af89d44e6c43357f9d327be21633fc6bded33f9de56475b05` |
| `achievements_spec2_fixture.exe` | 104,448 | `7db0456434d4bd2f4bc61a5789ce2de1e5451448c7e1d6e8f34aad43c6fc62f6` |
| `achievements_http_fixture.exe` | 40,960 | `003f03c2f57b110802c82969e0e58e35868439c66990e792d90c877c286f931a` |

*Note: `gt4vulkan.exe` grew by 301,056 bytes compared to the baseline (`97,398,784` -> `97,699,840` bytes), exactly reflecting the static inclusion of `gt4_achievements` and `gt4_rcheevos`.*

### 4.2 Production Target 2: Spec II Candidate (`slice332-rc7-ra-spec2`)
- **CMake Options:** Clang-cl for C and CXX, `GT4_TARGET=spec2`, `GT4_EE_PROFILE=spec2`, `slice276-spec2-iop-runtime`, `slice277-license-guard-runtime`.
- **Build Directory:** `O:\gt4recomp-scratch\slice332-rc7-ra-spec2\build`
- **Output Artifact:**

| Binary Artifact | Size (Bytes) | SHA256 Hash |
| :--- | :--- | :--- |
| `gt4vulkan-spec2.exe` | 104,718,848 | `4eacbe9d72813eb8002dfad7d86a92ef7a7a2cae1bb47520a2e78a32451c481d` |

---

## 5. Candidate Verification & Test Evidence

### 5.1 CTest Results (50 Test Targets)
- **Total Tests:** 50
- **Passed:** 49
- **Failed:** 1 (Test #36 `python_native_contracts`, matching baseline)
- **Skipped / Disabled:** 0
- **New Tests Added & Verified:**
  - `Test #14: native_achievements` -> **Passed** (0.03s)
  - `Test #15: native_achievements_spec2` -> **Passed** (0.02s)
  - `Test #16: native_achievements_http` -> **Passed** (0.04s)
- **Log:** `private/task133-candidate-logs/candidate-ctest.log`  
  **SHA256:** `c371fd3a349eb71068674b66a803edc175eba1c2119c99bde3475c5a3b93feea`

### 5.2 Python Suite Results (710 Tests)
- **Total Tests:** 710
- **Passed:** 655
- **Failed:** 13 (Exact match to baseline debt: 12 in `test_gs_settings.py`, 1 in `test_gs_live.py`)
- **Skipped:** 42
- **New Failures:** 0
- **Execution Time:** 44.63s
- **Log:** `private/task133-candidate-logs/candidate-python.log`  
  **SHA256:** `06ac3cf38ca1553087c18d51c1ec8d5559c3dd4f5ee8e61eb34a1c22650914f8`

### 5.3 Specific Evidence Checklist
- [x] **v200 Configured Through Fake Inputs:** Verified in `test_rcheevos_session_and_unlock` (mock memory trigger and award request) and `test_clean_shutdown_and_reinit` (environment parsing and session lifecycle).
- [x] **Disabled/Invalid Config Handling:** Verified in `test_disabled_and_invalid_config` (`GT4_RA_ENABLED=0` remains inactive; missing credentials log warning and do not activate session).
- [x] **Spec II Target Negative Path:** Verified in `achievements_spec2_fixture.exe` without `GT4_TARGET_FIXTURE_V200` override. Forged `GT4_RA_*` values produce no active runtime, memory reads return 0 bytes and zero output buffer, and no HTTP requests or overlays occur.
- [x] **Delayed Loopback HTTP Cancellation & Restart:** Verified in `achievements_http_fixture.exe` with a 5-second stalled loopback server; cancel occurs in under 2 seconds, and the transport cleanly restarts on subsequent requests.
- [x] **Clean Shutdown & Safe Reinitialization:** Verified in `test_clean_shutdown_and_reinit` across multiple sequential sessions without deadlock or leak.
- [x] **Normal Game Loop Retained When RA Inactive:** Verified in `test_inactive_normal_behavior` (VBlank calls, position changes, session resets are no-ops when inactive).

---

## 6. Native Overlay Localization Analysis

Analysis of `native/achievements.cpp` and `native/achievements_overlay.cpp` confirms that toast messages and headers are currently hardcoded English strings:
1. **Toast Headers:**
   - `"RETROACHIEVEMENTS"`
   - `"RETROACHIEVEMENTS · HARDCORE"` / `"RETROACHIEVEMENTS · SOFTCORE"`
   - `"GRAN TURISMO 4 · HARDCORE"` / `"GRAN TURISMO 4 · SOFTCORE"`
   - `"HARDCORE ACHIEVEMENT UNLOCKED"` / `"ACHIEVEMENT UNLOCKED"`
   - `"GAME MASTERED · HARDCORE"` / `"GAME COMPLETED · SOFTCORE"`
   - `"LEADERBOARD STARTED"` / `"LEADERBOARD SUBMITTED"` / `"LEADERBOARD FAILED"`
2. **Toast Titles & Subtitles:**
   - `"Sign-in failed"` / `"Check your account credentials in the launcher."`
   - `"Signed in"`
   - `"Could not load achievement set"` / `"Failed to load Gran Turismo 4 achievements."`
   - `"Unlocked %u of %u (%u/%u pts)"`
   - `"All %u achievements unlocked (%u pts)!"`
   - `"Achievement Progress"`
3. **Current Interface Status:**
   - There is **no existing localization interface** between the launcher and the native overlay.
   - For Stage 3 (Launcher integration), a localization mechanism (such as environment variables e.g. `GT4_RA_LOCALE_*` or passing a localized string dictionary/JSON from the launcher) will need to be designed if in-game overlay strings are to support Brazilian Portuguese (`pt-BR`). If not passed, English remains the native default.

---

## 7. Remaining Work (Stage 3 & Beyond)

1. **Stage 3: Launcher Integration (Rust & Frontend):**
   - **RetroAchievements:**
     - Add `ra` section to launcher configuration model in `gt4l-core`.
     - Implement credential validation and secure token storage.
     - Add RA UI card in launcher Settings (Sign in, Token, Hardcore toggle, Encore toggle, Status display).
     - Add en-US and pt-BR localization keys for all new UI controls.
   - **Controller Response Curves:**
     - Integrate 7 curves from `v1.2.5` (`linear`, `quadratic`, `cubic`, `s_curve`, `smooth_power`, `sublinear`, `logarithmic`).
     - Wire dropdown selector in Controller settings.
     - Add en-US and pt-BR descriptions for each curve.
2. **Stage 4: Packaging, Pins, and Documentation:**
   - Pin new candidate executables.
   - Bump version to `1.3.0-rc7` in Cargo workspaces, Tauri conf, and NSIS scripts.
   - Write full release notes and launcher notes (en-US and pt-BR).
   - Build installer and portable zip (draft only, pending owner authorization).

---

## 8. Bounded Stop Point

All Stage 2 requirements have been completely fulfilled:
- Fresh baseline gate established and logged.
- Native RA and target isolation implemented and tested.
- Both production candidates (`gt4vulkan.exe` and `gt4vulkan-spec2.exe`) compiled and measured.
- CTest and Python test suites run and logged.
- No files outside the allowed scope were modified.

**Execution is stopped. Awaiting Codex Orchestrator review.**
