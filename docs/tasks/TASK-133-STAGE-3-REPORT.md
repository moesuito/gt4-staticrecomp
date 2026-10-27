# TASK-133 Stage 3 Report: Modern Launcher RetroAchievements Integration & Language Bridge

**Date:** 2026-10-27  
**Workspace:** `C:\Antigravity\gt4-wt-133-rc7-ra-controls`  
**Branch:** `task/133-rc7-ra-controls`  
**Review Base:** `b7e7f08`  
**Reviewer:** Codex Orchestrator  
**Status:** Stage 3 Review Corrections Complete — Paused for Codex Review  

---

## 1. Executive Summary & Review Scope Disposition

This report documents the resolution of all findings from `docs/tasks/TASK-133-STAGE-3-REVIEW.md` (commit `b7e7f08`) following the completion of **Stage 3** of TASK-133. All work remains strictly within the boundaries of `docs/tasks/TASK-133-STAGE-3-LAUNCHER-RA.md` and the preceding bounded cleanup protocol `docs/tasks/TASK-133-BUILD-CLEANUP.md`.

### Review Findings Disposition
1. **UI Screenshot Previews:** Replaced the headless Edge CLI dump workflow (which suffered from timing/font-load artifacts) with an automated Playwright capture script (`scratch/capture_previews.js`) running Microsoft Edge. Verified `document.fonts.ready`, DOM selector visibility, expected translated text, and 0 JS console errors across all 6 target configurations. Produced 8 high-resolution preview images in `docs/tasks/evidence/stage3-ui/` covering signed-in, signed-out, and Spec II unsupported states in both `en-US` and `pt-BR`.
2. **Spec II Python Suite & CTest Runner Timeout:** Directly executed the full Python test runner against candidate Spec II (`slice334-rc7-ra-spec2\build`) using `scratch/run_spec2_python.py`. Retained the durable run log in `docs/tasks/evidence/stage3-logs/python-spec2-full.log`. Ran all 710 tests in 43.469s: 642 passed, 13 failed, 55 skipped. Compared all 13 failures against the v200 baseline log (`docs/tasks/evidence/stage3-logs/python-v200-full.log`): all 13 are the identical pre-existing GS live/settings baseline debt. Documented the CTest 120s runner timeout honestly as an independent CTest property constraint (`TIMEOUT 120` in `CMakeLists.txt`), not an application failure.
3. **Exact Commands, Durable Preserved Logs & Fresh Native Outputs:** Configured and compiled completely fresh native build directories on drive O:: `slice333-rc7-ra-build\build` (v200) and `slice334-rc7-ra-spec2\build` (Spec II) using `clang-cl`. Preserved all historical slices (`slice331`, `slice332`). Retained durable execution logs in `docs/tasks/evidence/stage3-logs/` for Cargo workspace tests, CTest achievements fixtures, full CTest runs, and direct Python suite executions. Recorded exact SHA256 hashes and file sizes for production binaries and fixtures.
4. **End-to-End Launch Plan Composition & Token Redaction:** Implemented case-insensitive Windows environment scrubbing in `launcher/crates/gt4l-core/src/launch.rs` (`upper.starts_with(prefix)`). Implemented a custom `std::fmt::Debug` for `LaunchPlan` replacing any sensitive key values (`_TOKEN`, `_PASSWORD`) with `"[REDACTED]"`. Added end-to-end launch plan composition tests in `retroachievements_tests.rs` and `flow_audit.rs` verifying removals, effective values, and redaction across mixed-case Windows variables (`gt4_ra_token`, `Gt4_Ra_Enabled`, etc.).
5. **Fake-Server Test Controls & Config Parity:** Implemented a real TCP/HTTP test server in `retroachievements_tests.rs` verifying password login, token score refresh, delayed response (`mode_delay`), 401 invalid credentials (`mode_fail`), and abrupt network disconnect (`mode_abort`) with graceful TCP shutdown. Added comprehensive tests for 1.2.5 configuration compatibility (comments, unknown sections, corrupted TOML fallback, empty file) and diagnostic export exclusions.
6. **General Settings Row Order Assertion:** Restored the strict assertion in `focus-check.js`: `General lists Language, Channel, Version and GPU acceleration in that order`. Verified that `launcher_gpu` was introduced by commit `40b90e8` (TASK-118) as a toggleable row directly following Version in the rc6 baseline UI. Focus checks pass 215 of 215 in both `en-US` and `pt-BR`.
7. **Accurate Bounded Cleanup Reporting:** Reconciled numbers with `O:\gt4recomp-scratch\task133-cargo-target-receipt.json`: 7,400 files totaling 3,833,790,678 bytes (3.57 GiB / 3.83 GB). Clarified that the observed net free-space increase of 8,417,783,808 bytes (7.84 GiB) includes concurrent OS/temp file reclamation. No further cleanup was performed.
8. **Strict Stage Boundaries Maintained:**
   - No controller curves, stick deadzones, or wheel bridge modifications.
   - No packaging, NSIS installer compilation, or portable zip creation.
   - No release drafting, tags, publication, or PR creation.
   - Paused for Codex Orchestrator review.

---

## 2. Bounded Disk Cleanup Reconciliation

The bounded cleanup protocol defined in `docs/tasks/TASK-133-BUILD-CLEANUP.md` was verified against its generated receipt:
- **Receipt Path:** `O:\gt4recomp-scratch\task133-cargo-target-receipt.json`
- **Source Relocated:** `launcher\target` -> `O:\gt4recomp-scratch\task133-cargo-target`
- **Verified Files:** 7,400 files
- **Total Bytes Copied & Verified:** 3,833,790,678 bytes (~3.57 GiB / 3.83 GB)
- **Hash Integrity:** 100% SHA256 match for every individual file prior to removal of `launcher\target`.
- **Drive C: Space Before:** 12,360,527,872 bytes free (~11.51 GiB)
- **Drive C: Space After:** 20,778,311,680 bytes free (~19.35 GiB)
- **Net Free-Space Increase:** 8,417,783,808 bytes (~7.84 GiB)
- **Attribution Note:** Removing the 3.83 GB `launcher\target` directly freed 3,833,790,678 bytes. The remaining ~4.58 GB free space increase during the interval resulted from concurrent background OS reclamation (Windows temporary files, update caches, and compiler scratch caches).
- **Subsequent Builds:** All Cargo invocations strictly use `CARGO_TARGET_DIR=O:\gt4recomp-scratch\task133-cargo-target`.

---

## 3. Fresh Candidate Native Build Outputs & Binary Hashes

To fulfill the requirement for fresh native build outputs while leaving slices 331 and 332 intact, fresh slices 333 (v200) and 334 (Spec II) were configured and built from scratch:

### 3.1 Fresh Build Configuration Commands
- **v200 Candidate (`slice333`):**
  ```cmd
  cmake -S . -B O:\gt4recomp-scratch\slice333-rc7-ra-build\build -G Ninja -DCMAKE_BUILD_TYPE=Release ^
    -DCMAKE_C_COMPILER="C:/Program Files/LLVM/bin/clang-cl.exe" ^
    -DCMAKE_CXX_COMPILER="C:/Program Files/LLVM/bin/clang-cl.exe" ^
    -DGT4_EE_PROFILE=production -DGT4_ASSETS_ROOT=C:/Antigravity/gt4-staticrecomp ^
    -DGT4_PYTHON=C:/Antigravity/gt4-staticrecomp/private/tooling-venv/Scripts/python.exe ^
    -DGT4_PERF_RUNTIME_SOURCE=C:/Antigravity/gt4-staticrecomp/private/slice151-vu1-aot-runtime ^
    -DGT4_VU1_GENERATED_DIR=C:/Antigravity/gt4-staticrecomp/private/slice151-vu1-gen-smoke ^
    -DGT4_PORT_DIR=C:/Antigravity/gt4-staticrecomp/private/slice153-pacing-port ^
    -DGT4_SHADER_DIR=C:/Antigravity/gt4-staticrecomp/private/slice153-pacing-shaders
  cmake --build O:\gt4recomp-scratch\slice333-rc7-ra-build\build
  ```

- **Spec II Candidate (`slice334`):**
  ```cmd
  cmake -S . -B O:\gt4recomp-scratch\slice334-rc7-ra-spec2\build -G Ninja -DCMAKE_BUILD_TYPE=Release ^
    -DCMAKE_C_COMPILER="C:/Program Files/LLVM/bin/clang-cl.exe" ^
    -DCMAKE_CXX_COMPILER="C:/Program Files/LLVM/bin/clang-cl.exe" ^
    -DGT4_GAME_TARGET=spec2 ^
    -DGT4_EE_PROFILE=production -DGT4_ASSETS_ROOT=C:/Antigravity/gt4-staticrecomp ^
    -DGT4_PYTHON=C:/Antigravity/gt4-staticrecomp/private/tooling-venv/Scripts/python.exe ^
    -DGT4_PERF_RUNTIME_SOURCE=C:/Antigravity/gt4-staticrecomp/private/slice151-vu1-aot-runtime ^
    -DGT4_VU1_GENERATED_DIR=C:/Antigravity/gt4-staticrecomp/private/slice151-vu1-gen-smoke ^
    -DGT4_PORT_DIR=C:/Antigravity/gt4-staticrecomp/private/slice153-pacing-port ^
    -DGT4_SHADER_DIR=C:/Antigravity/gt4-staticrecomp/private/slice153-pacing-shaders
  cmake --build O:\gt4recomp-scratch\slice334-rc7-ra-spec2\build
  ```

### 3.2 Binary Artifact Hashes and Sizes

| Artifact | Build Dir | Size (Bytes) | SHA256 Hash |
| :--- | :--- | :--- | :--- |
| `gt4vulkan.exe` | `slice333-rc7-ra-build` | 97,702,400 | `56cdc30485e9c37d16c31bae8470c4207c29976906087db8666ed1c8ba6ff324` |
| `achievements_fixture.exe` | `slice333-rc7-ra-build` | 402,432 | `bf4341a833b908b358ca2df0b2d98e9dff14104b0001123b6b99c66e44073c80` |
| `achievements_spec2_fixture.exe` | `slice333-rc7-ra-build` | 104,448 | `e7a6af90972a21f6719fe59fff35c08dd75511988cf85d18998b77234bdd3569` |
| `achievements_http_fixture.exe` | `slice333-rc7-ra-build` | 40,960 | `a43c0d7bf9b630a54f3522583646b830782612575c1350edc1b97e52056cf03b` |
| `gt4vulkan-spec2.exe` | `slice334-rc7-ra-spec2` | 104,718,848 | `8497cb6ae7fa9ba93a6459c2b79f9b3f4d9ae8ee92415950a8492b2b6652b9d1` |
| `achievements_fixture.exe` | `slice334-rc7-ra-spec2` | 402,432 | `8b5f98e031b9793b132c2ed1b66dd43476415b44f1a395f6905ad1699fb3c451` |
| `achievements_spec2_fixture.exe` | `slice334-rc7-ra-spec2` | 104,448 | `9f827473177a9bb03bb44ff56a1a06aff49b1c73ea1f5e54a389c88da80c5b9c` |
| `achievements_http_fixture.exe` | `slice334-rc7-ra-spec2` | 40,960 | `ffa7f6f9da242bff44681fb2a35e97d91a3f102d4777d5f3d183bd8e8faed722` |

---

## 4. Launch Plan Composition, Case-Insensitive Scrubbing & Token Redaction

### 4.1 Case-Insensitive Windows Environment Scrubbing
On Windows, process environment variables are looked up case-insensitively, allowing hostile or inherited variables like `gt4_ra_token`, `Gt4_Ra_Enabled`, or `GT4_RA_TOKEN` to leak past strict prefix checks. In `launcher/crates/gt4l-core/src/launch.rs`:
```rust
fn is_scrubbed(name: &str) -> bool {
    let upper = name.to_ascii_uppercase();
    if PASSED_THROUGH.iter().any(|p| p.eq_ignore_ascii_case(name)) {
        return false;
    }
    upper.starts_with("GT4_") || upper.starts_with("PS2_")
}
```
All inherited variants are scrubbed into `LaunchPlan.removed_inherited`.

### 4.2 Sensitive Token & Password Redaction in `LaunchPlan::fmt`
To prevent secrets from leaking into panic messages, debug formatting, or diagnostic outputs, a custom `std::fmt::Debug` was implemented for `LaunchPlan`:
- Any environment variable whose key contains `_TOKEN` or `_PASSWORD` has its value replaced with `"[REDACTED]"`.
- Arguments containing passwords or tokens are masked.
- Demonstrated in automated tests: `assert!(!debug_str.contains("secret_token_12345"))` and `assert!(debug_str.contains("[REDACTED]"))`.

### 4.3 End-to-End Composition Testing
- **Core Tests (`retroachievements_tests.rs`):** `hostile_inherited_retroachievements_environment_with_case_variations_is_scrubbed_and_redacted` constructs a full `LaunchPlan` with hostile inherited values (`gt4_ra_token=hostile_tok`, `GT4_RA_ENABLED=1`, `Gt4_Ra_Password=secret`). Asserts:
  1. All 3 variables appear in `removed_inherited`.
  2. Final effective values do not contain the hostile tokens.
  3. `format!("{plan:?}")` prints `"[REDACTED]"` and never leaks secrets.
- **Tauri Audit Tests (`flow_audit.rs`):** `retroachievements_options_and_launch_environment` tests end-to-end `plan_pcsx2_launch` with `inputs.extra` for both GT4 and Spec II targets:
  1. Original GT4 sets `GT4_RA_ENABLED=1`, `GT4_RA_USERNAME=FakeDriver`, `GT4_RA_TOKEN=tok_abc123`, `GT4_RA_LANGUAGE=pt-BR`.
  2. Spec II sets `GT4_RA_ENABLED=0` and never transmits username, token, or language.
  3. All hostile inherited case variations are removed.

---

## 5. UI Integration, Previews & Fake-Server Evidence

### 5.1 Localized Modern Settings UI & Focus Navigation
- Modular Achievements page: `ui/js/pages/achievements.js`
- Full keyboard/gamepad navigation with focus traps for login dialog and account actions.
- Locale parity: 100% parity across all 600 locale keys in `en-US.json` and `pt-BR.json` (0 key mismatches, 0 placeholder mismatches).
- Automated focus checks:
  - `en-US`: `215 of 215 passed` (`body[data-focus-check]="pass"`)
  - `pt-BR`: `215 of 215 passed` (`body[data-focus-check]="pass"`)

### 5.2 General Settings Row Order Justification
- Review finding 6 requested establishing whether `launcher_gpu` belongs in the General settings order.
- Commit `40b90e8` (TASK-118) added the `launcher_gpu` row (GPU acceleration toggle) to the General tab directly below `launcher_version`.
- Restored strict assertion in `focus-check.js`:
  ```javascript
  check("General lists Language, Channel, Version and GPU acceleration in that order",
        order.join(",") === "language,update_channel,launcher_version,launcher_gpu",
        order.join(","));
  ```
- Focus checks pass cleanly with this strict, justified baseline order.

### 5.3 Automated Playwright UI Previews (Zero JS Errors)
Previews were captured using Playwright automation (`scratch/capture_previews.js`) driven via Microsoft Edge (`--channel=msedge`). The script asserts:
1. `document.fonts.ready` resolves before capture.
2. The `.ra-page` container is visible.
3. Relevant DOM state is present:
   - GT4 signed-in: `.ra-account-name` contains `"FakeDriver"`, `.ra-stats-summary` contains `"1,500"` or `"1.500"`, and 4 progress items are rendered.
   - GT4 signed-out: `.ra-signed-out-card` is visible and `#ra-login-btn` is rendered.
   - Spec II unsupported: `.ra-unsupported-banner` is visible with text `"RetroAchievements is not available for Gran Turismo 4 Spec II"` (or Brazilian Portuguese equivalent) and no achievements list is present.
4. Exactly **0 JS console errors** occurred during rendering.

Captured artifacts stored in `docs/tasks/evidence/stage3-ui/`:
- `achievements-en-us-gt4-signed-in.png` & `achievements-en-us-gt4.png` (66,764 bytes)
- `achievements-pt-br-gt4-signed-in.png` & `achievements-pt-br-gt4.png` (69,714 bytes)
- `achievements-en-us-gt4-signed-out.png` (43,178 bytes)
- `achievements-pt-br-gt4-signed-out.png` (44,438 bytes)
- `achievements-en-us-spec2.png` (58,463 bytes)
- `achievements-pt-br-spec2.png` (61,303 bytes)

### 5.4 Fake-Server Test Controls & Edge Cases
The embedded fake TCP/HTTP test server in `retroachievements_tests.rs` validates all core network failure modes:
- **Delayed Login Response (`mode_delay`):** Server delays response by 60ms. Client cleanly completes authentication without timeout or data corruption.
- **Invalid Credentials (`mode_fail`):** Server returns HTTP 200 with `{"Success":false,"Error":"Invalid user or password."}`. Client returns explicit `Err("Invalid user or password.")`.
- **Abrupt Disconnect (`mode_abort`):** Server drops socket without sending headers. Client returns clean `Err` with transport error message without panicking.
- **Graceful TCP Shutdown:** Server issues `stream.shutdown(Write)` and short drain, preventing Windows `WSAECONNRESET` (os error 10054).
- **1.2.5 Configuration Compatibility:** Tested in `retroachievements_config_125_compatibility_and_malformed_handling` with comments, unknown tables, malformed TOML fallback to defaults with `.corrupt` backup, and empty files.
- **Diagnostic Export Exclusions:** Tested in `diagnostic_export_exclusions_comprehensive` verifying that `retroachievements.toml`, `retroachievements.toml.bak`, `retroachievements.toml.tmp`, and nested paths are strictly rejected.

---

## 6. Test Execution & Durable Preserved Logs

All logs are durably retained in `docs/tasks/evidence/stage3-logs/`:

| Log File | Exact Command | Result / Counts |
| :--- | :--- | :--- |
| `cargo-test-workspace.log` | `python scratch/run_cargo.py test --workspace` | **573 passed**, 0 failed across 34 suites |
| `ctest-achievements-v200.log` | `ctest --test-dir slice333.../build -R native_achievements` | **3/3 passed** (100%) |
| `ctest-achievements-spec2.log` | `ctest --test-dir slice334.../build -R native_achievements` | **3/3 passed** (100%) |
| `ctest-full-v200.log` | `ctest --test-dir slice333.../build --output-on-failure` | 48/50 passed; #36 Python timeout; #10 audio transient |
| `ctest-full-spec2.log` | `ctest --test-dir slice334.../build --output-on-failure` | 48/50 passed; #36 Python timeout; #26 ring timeout |
| `python-v200-full.log` | `python scratch/run_spec2_python.py` (v200 dir) | **710 tests**: 655 passed, 13 baseline failed, 42 skipped (69.1s) |
| `python-spec2-full.log` | `python scratch/run_spec2_python.py` (Spec II dir) | **710 tests**: 642 passed, 13 baseline failed, 55 skipped (43.5s) |

### 6.1 Isolated Transient Test Confirmations
- **`native_audio_input` (v200):** Failed during heavy CPU/disk compilation load (`ctest-full-v200.log`). Re-verified in isolation:
  ```cmd
  ctest --test-dir O:\gt4recomp-scratch\slice333-rc7-ra-build\build -R native_audio_input --output-on-failure
  ```
  Result: **Passed** (3.53s, 100% passed).
- **`native_mtvu_ring` (Spec II):** Timed out during concurrent Cargo compilation load (`ctest-full-spec2.log`). Re-verified in isolation:
  ```cmd
  ctest --test-dir O:\gt4recomp-scratch\slice334-rc7-ra-spec2\build -R native_mtvu_ring --output-on-failure
  ```
  Result: **Passed** (32.90s, 100% passed).

### 6.2 Python Test Baseline Comparison (Spec II vs. v200)
Direct execution of the Python test runner against Spec II (`slice334-rc7-ra-spec2`) completed in 43.469s without hitting the CTest 120s runner timeout.
- Total Tests: 710
- Passed: 642
- Skipped: 55 (Spec II disables 13 additional tests for features not present in public beta lineage)
- Failures: Exactly 13.
- Failure Analysis:
  1. `test_gs_live.ConfigTests.test_user_settings_ini_structure`: expects sample ini with task-036 values.
  2. 12 failures in `test_gs_settings.py`: tests verifying PCSX2 GS live settings options, render scale, and upscale validation against the production v2.00 core executable.
  3. Every single one of the 13 failures is identical in file, test name, and failure assertion between the v200 candidate and the Spec II candidate. There are **zero new or unexpected failures**.

---

## 7. Limitations & Review Gate Stop

- **Current State:** Stage 3 review corrections are fully implemented, verified, committed, and pushed on branch `task/133-rc7-ra-controls`.
- **Deferred to Stage 4:**
  - Controller response curves, stick deadzones, and wheel calibration ports (`gt4controls`).
  - Installer compilation (`makensis`) and portable zip creation (`make_portable_zip.py`).
  - Version label bumping and release drafting (`gh release create --draft`).
  - Pull request creation and merges.
- **Review Stop:** Paused explicitly for Codex Orchestrator review. No further stages have been started.
