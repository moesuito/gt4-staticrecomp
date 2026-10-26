# TASK-133: Baseline and Provenance Report (Stage 1 Completion)

Date: 2026-10-26  
Branch: `task/133-rc7-ra-controls`  
Worktree: `C:/Antigravity/gt4-wt-133-rc7-ra-controls`  
HEAD Commit: `66d2793` (`docs(review): require complete rc7 baseline evidence before integration`)  
Program Baseline: `v1.3.0-rc6` (`58322229c0bd63a0652b0550c5ad2154bd907039`)  

---

## 1. Source, Pin, Toolchain, and Build References

### Provenance and Inputs
- **Base Repository State:** Branch `task/133-rc7-ra-controls` is created directly from tag `v1.3.0-rc6` (`5832222`). All program files match published rc6 byte-for-byte. The working directory is completely clean.
- **RetroAchievements Source Reference:** Stable `v1.2.4` (`ad46df65c03894ed204beafbd02819a11e4037bc`), incorporating PR #96 and the reviewed account/WinHTTP cancellation fixes from PR #105.
- **Controller Improvements Source Reference:** Commit `8b73fa06cd9947d597354502a858693779d3eb57` (PR #100 reviewed via PR #106) and published controller-only release `v1.2.5` (`5d7a669bdecf5e60c0782ca449f48aacac3c7a32`). Wheel code, DFP, and FFB bridges are strictly excluded.
- **Deferred Work:** Frame skipping (TASK-082) is preserved on remote branch `task/082-render-skip` (`15b7bf3`) and is excluded from rc7.
- **Native rc6 Build Under Test:**
  - Build directory: `O:/gt4recomp-scratch/slice300-rc6-v200/build`
  - Produced `gt4vulkan.exe` / `gt4.exe` matching pin `docs/inputs/slice303-pcsx2.json`:
    - Size: 97,398,784 bytes
    - SHA256: `6682777e1a6c141d3f32aa54265d609a4926a55f0c19c45d84c51a2e39970945`
  - Spec II binary matching pin `docs/inputs/slice304-spec2.json`:
    - Size: 104,634,368 bytes
    - SHA256: `e841615236ec9107e0d0580d877ebc5ffa3afae9644aed1196f12a58f89df173`
- **Toolchains:**
  - C++ Compiler: Clang-cl (LLVM 19.x, C++20), Ninja 1.12.1, MSVC BuildTools 2022
  - Rust: cargo 1.85+, locked dependencies, offline mode
  - Python: `C:/Antigravity/gt4-staticrecomp/private/tooling-venv/Scripts/python.exe`
  - SDL3 Development/Runtime Root: `C:/Antigravity/gt4-staticrecomp/private/tooling/sdl3-3.4.18`

---

## 2. Test Execution Commands, Evidence, and Classification

### A. Rust Launcher Workspace Suite
- **Command:**
  ```powershell
  $env:GT4_SDL3_ROOT = "C:/Antigravity/gt4-staticrecomp/private/tooling/sdl3-3.4.18"
  cargo test --manifest-path C:\Antigravity\gt4-wt-133-rc7-ra-controls\launcher\Cargo.toml --workspace --locked --offline
  ```
- **Overall Result:** **559 passed, 0 failed, 5 ignored**.
- **Unit-Test Blocks (passed / failed / ignored):**
  - `gt4controls`: 16 / 0 / 0
  - `gt4l-core`: 213 / 0 / 0
  - `gt4l-disc`: 6 / 0 / 0
  - `gt4l-platform`: 47 / 0 / 2
  - `GT4Recomp` (`gt4l-tauri`): 88 / 0 / 1
  - `gt4saves`: 6 / 0 / 0
- **Integration-Test Blocks:**
  - `gt4controls/tests/docs_match.rs`: 4 / 0 / 0
  - `gt4l-core/tests/launcher_version.rs`: 6 / 0 / 0
  - `gt4l-core/tests/model.rs`: 11 / 0 / 0
  - `gt4l-core/tests/portable_zip.rs`: 7 / 0 / 0
  - `gt4l-core/tests/release_labels.rs`: 29 / 0 / 0
  - `gt4l-core/tests/user_dirs.rs`: 14 / 0 / 0
  - `gt4l-disc/tests/disc.rs`: 8 / 0 / 0
  - `gt4saves/tests/card_owner.rs`: 6 / 0 / 0
  - `gt4saves/tests/hostile_cards.rs`: 10 / 0 / 0
  - `gt4saves/tests/library.rs`: 12 / 0 / 0
  - `gt4saves/tests/migration.rs`: 7 / 0 / 0
  - `gt4saves/tests/per_game.rs`: 5 / 0 / 0
  - `gt4saves/tests/save_management.rs`: 9 / 0 / 0
- **Independent Log Reference:**
  - Path: `C:/Antigravity/gt4-staticrecomp/private/task133-orchestrator-review-20261026/cargo-baseline.log`
  - SHA256: `0c3b22ededd926b4e2c463212a1aedff43d41eae594952e8115bc47a0aaaee61`

### B. CTest Native Suite
- **Command:**
  ```powershell
  ctest --test-dir O:/gt4recomp-scratch/slice300-rc6-v200/build --output-on-failure
  ```
- **Result:** **46 of 47 passed (98%), 1 failed** (Test #33 `python_native_contracts`).
  - Total real execution time: 226.64 s.
  - All 46 native and recompilation tests passed:
    - Vulkan tests (pixels, sync, baseline, extensions, async stress)
    - Host I/O & controls (audio, controls, pad mapping, pacing, cursor autohide)
    - Game display (quickfix, race display, widescreen, aspect)
    - MTVU & Performance (accounting, probe, mtvu measure, ring, journal, vif batch, switches)
    - GS Stream & IPC (client, stream ipc, readback probe)
    - Shape capture & observation (observer, export, control)
    - VU AOT & Differential (vu_recomp_core, differential, corpus [98s], vu1_aot_registry, vu0_emitter, vu0_aot_registry, vu0_aot_differential)
    - System & Targets (memcard_store, license_guard, standalone, game_target, spec2_iop, spec2_hooks, flare_limits)
  - Test #33 `python_native_contracts` failed solely because it invokes the Python test discovery suite, documented below.
- **Log Reference:**
  - Path: `private/task133-baseline-logs/ctest-baseline.log`
  - SHA256: `095e3a03e723ef73db2fe11c8496a946af35fde206e9cad241fed5910488fe7f`

### C. Full Python Test Suite
- **Command:**
  ```powershell
  $env:GT4_NATIVE_BUILD = "O:/gt4recomp-scratch/slice300-rc6-v200/build"
  C:\Antigravity\gt4-staticrecomp\private\tooling-venv\Scripts\python.exe -m unittest discover -s C:\Antigravity\gt4-wt-133-rc7-ra-controls\tests\python
  ```
- **Result:** Ran 710 tests in 48.981 s. **655 passed, 13 failed, 42 skipped**.
- **Log Reference:**
  - Path: `private/task133-baseline-logs/python-baseline.log`
  - SHA256: `ab638172071273f1be430b8c423103306493dd3c540dd43b447dda4e29393d59`
- **Failure Classification and Root Cause:**
  All 13 failures are pre-existing baseline test expectation mismatches caused by TASK-118 (rc6 settings simplification):
  1. `test_gs_live.ConfigTests.test_defaults_are_vulkan_3x_high_blending_automatic_deinterlace_16_9_vsync`:
     - Fails because `LoadTextureReplacementsAsync = true` was added to `gt4-gs.ini` by default in rc6, while the test fixture expects the older default without this key.
  2-13. `tests/python/test_gs_settings.py` (12 failures):
     - `DefaultTests.test_cache_and_headless_sections_are_unchanged`
     - `DefaultTests.test_default_file_bytes_match_the_main_writer`
     - `DefaultTests.test_default_ini_is_main_default`
     - `DefaultTests.test_probe_default_is_unchanged`
     - `DefaultTests.test_run_text_with_data_root_and_eager_switch`
     - `IniTests.test_user_settings_replace_defaults_and_add_when_set_rows`
     - `OfferedTests.test_launcher_offers_only_settable_options`
     - `OsdRowTests.test_overlay_rows_are_advanced_toggles_off_by_default`
     - `PageLayoutTests.test_advanced_page_holds_the_rest_of_the_user_options`
     - `ProhibitDynamicCodeTests.test_row_is_an_advanced_always_written_switch_defaulting_to_false`
     - `ResetToDefaultsTests.test_reset_gives_the_exact_default_ini`
     - `ResetToDefaultsTests.test_when_set_row_equal_to_its_default_is_not_written`
     - *Cause:* In TASK-118, 35 settings rows were moved from `page = "advanced"` to `page = "hidden"`, the Advanced settings page was removed in favor of simplified Graphics settings, and `LoadTextureReplacementsAsync = true` was added to the default output. The tests in `test_gs_settings.py` assert against the pre-TASK-118 `LEGACY_DEFAULT` layout (e.g. asserting `len(advanced) == 40` when the simplified table has 5).
  - No failures occurred in any other Python test module (memcards, widescreen, aspect patches, license guard, EE entries, Spec II, distribution, VU0/VU1 image).

---

## 3. Corrected Proposed Integration Contract

### A. Persisted Controller Curves
1. **Canonical Curve Identifiers:**
   The exact persisted string identifiers are:
   - `linear`
   - `quadratic`
   - `cubic`
   - `s_curve`
   - `smooth_power`
   - `sublinear`
   - `logarithmic`
   Ad-hoc identifiers (such as `exponential` or `aggressive`) are strictly prohibited. Existing saved `controls.toml` configurations are preserved with full backward compatibility.
2. **Scope of Application:**
   All 7 curves apply symmetrically to both game targets (`gt4.exe` and `gt4-spec2.exe`).
3. **Trigger Standardization:**
   SDL Gamepad analog triggers use standardized `0..32767` integer scaling for all mapped gamepads without device-specific name heuristics.
4. **Boundary:**
   Strictly controller-only. No wheel, DFP, or force-feedback code enters this integration.

### B. Target Identification and Isolation (GT4 v2.00 vs Spec II)
1. **Target Architecture Contract:**
   Target identity is governed by the existing native target architecture:
   - `gt4::target::active().id`
   - Evaluated as `gt4::target::Id::v200` versus `gt4::target::Id::spec2`.
   - Selected at compile time via CMake `GT4_TARGET` (default `v200`, or `spec2`).
2. **Native Guard:**
   Inside the native achievements runtime (`native/achievements.cpp`), initialization explicitly checks:
   ```cpp
   if (::gt4::target::active().id != ::gt4::target::Id::v200) {
     // Achievements strictly disabled for Spec II: no memory evaluation,
     // no network connections, no overlay rendering.
     return;
   }
   ```
   Environment variables and executable filenames are not treated as authoritative native security boundaries.
3. **Spec II Negative Testing:**
   Tests verifying that Spec II disables achievements must evaluate the inactive path without defining `GT4_TARGET_FIXTURE_V200` (which would override the target profile to v2.00).
4. **Launcher Boundary:**
   In `launcher/crates/gt4l-tauri/src/commands.rs` (`prepare_play`), when `games::SPEC2` is selected:
   - All `GT4_RA_*` environment variables are scrubbed and forcibly set to disabled.
   - The launcher UI achievements tab/page displays a clear notice that RetroAchievements is unsupported for Spec II.

### C. RetroAchievements Lifecycle and Hygiene
1. **Integration Status:**
   RetroAchievements functionality is an **expected outcome of Stage 2 integration**, not a feature present in the unintegrated rc6 baseline.
2. **Configuration and Storage:**
   - Account credentials (`retroachievements.toml`) are resolved through `UserDirs` and `BuildKind`, maintaining strict isolation between Installed (`%APPDATA%/GT4Recomp`) and Portable (`user/`) installations.
   - Cache data is placed in the designated user/data cache directory.
3. **Cancellation and Concurrency Guards:**
   - Asynchronous HTTP requests via WinHTTP are gracefully cancelled during runtime shutdown.
   - Account revision numbers guard against race conditions between background logins/refreshes and logouts.
4. **Secrets Hygiene:**
   Passwords, web tokens, and raw credentials must never be written to logs, diagnostic export archives, console output, or child-process command-line arguments.

### D. Localization Parity
- All new player-facing UI text, settings rows, and native overlay messages are translated into both `en-US` and `pt-BR`.
- Key and `{placeholder}` parity between `en-US.json` and `pt-BR.json` is strictly maintained.

---

## 4. Stage 1 Completion Summary

- Baseline checks completed across Rust (559/559), Native CTest (46/47 passing, 1 known python contract failure), and Python (655/710 passing, 13 known TASK-118 table drift failures).
- Source provenance verified against published `v1.3.0-rc6` and pins `slice303-pcsx2.json` / `slice304-spec2.json`.
- Corrections from the Stage 1 review incorporated into the proposed architecture contract.
- No code or behavior changes applied to runtime or launcher.
- **Stage 1 is complete. Standing by for orchestrator review before beginning Stage 2.**
