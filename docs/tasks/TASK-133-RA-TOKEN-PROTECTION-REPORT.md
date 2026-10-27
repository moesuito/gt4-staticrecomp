# TASK-133: RetroAchievements Credential Protection Report

Date: 2026-10-27.  
Branch: `task/133-rc7-ra-controls`  
Worktree: `C:/Antigravity/gt4-wt-133-rc7-ra-controls`  
Starting Base: `adfd5132b775d354d4a5bb33152fb23fb542b85e`  
Review 1 Base: `c3c9959`  
Review 2 Base: `25c5070`  
Author: João Alano <57041838+moesuito@users.noreply.github.com>

---

## 1. Executive Summary & Design Overview

This work implements secure protection at rest for RetroAchievements authentication tokens on Windows for both installed and portable launcher layouts, addressing all requirements of `TASK-133-RA-TOKEN-PROTECTION.md` and resolving the findings in `TASK-133-RA-TOKEN-PROTECTION-REVIEW.md` and `TASK-133-RA-TOKEN-PROTECTION-REVIEW-2.md`.

### Core Design Principles
1. **Windows DPAPI Standard:** Uses Win32 Data Protection API (`CryptProtectData` and `CryptUnprotectData` via `crypt32.dll` and `kernel32.dll`) bound strictly to the current Windows user (`dwFlags = CRYPTPROTECT_UI_FORBIDDEN`, no machine-wide scope `CRYPTPROTECT_LOCAL_MACHINE`). No custom cryptography or bundled symmetric keys.
2. **Platform-Neutral Abstraction:** Introduced a clean `SecretProtector` trait and `SecretError` enum in `launcher/crates/gt4l-core/src/secret.rs`. Non-Windows platforms return explicit `UnsupportedPlatform` errors without silently persisting plaintext or falling back to fake implementations.
3. **Deterministic Testability & Seams:** Implemented `FakeProtector` for unit/concurrency tests and `AtomicFileOps` trait (`StdAtomicFileOps`, `MockAtomicFileOps`) for deterministic filesystem failure injection.
4. **Zero Plaintext at Rest & Serde Serialization Guard:** In `RaConfig`, `token` is annotated with `#[serde(default, skip_serializing)]`, ensuring that the TOML serializer can never write plaintext tokens to disk under any condition. `token_protected` stores the versioned envelope `dpapi:v1:<hex>`.
5. **Robust Atomic Writing & Unique Temporary Path Isolation:** Temporary files are named uniquely per write attempt (`.{filename}.tmp-{pid}-{counter}`) in the same directory (guaranteeing volume-local atomic rename). Independent launcher instances or processes never collide on predictable temporary file names, and unrelated pre-existing files are never touched or deleted.
6. **RAII Failure Cleanup & Process Interruption Distinction:**
   - **Normal Error Cleanup:** An internal `TempFileGuard` RAII structure ensures that if writing fails before write, mid-write, during flush, or during rename/replace, the temporary file created by this attempt is immediately removed from disk.
   - **Process Interruption (Forced Crash / Power Loss):** If the process is terminated externally before rename, the destination `retroachievements.toml` remains completely unmodified (preserving prior bytes). The orphaned temporary file contains only encrypted ciphertext (never plaintext). On retry, the launcher safely loads the valid configuration and completes the atomic save without data loss.
7. **Safe, Atomic Migration with Legacy File Preservation:** Existing legacy plaintext `retroachievements.toml` files are migrated automatically on first load. If protection fails during migration, `unmigrated_plaintext` is preserved in memory such that subsequent unrelated option changes (e.g. toggling hardcore/enabled) abort save rather than overwriting disk with an unauthenticated config that erases the user's old token.
8. **Command Boundary Safety:** Launcher commands (`ra_status_at_with`, `ra_set_options_with`, `ra_set_options_with_custom`, `ra_login_at_with`, `ra_logout_with`) provide injectable seams for deterministic integration testing of migration/write failures, option updates, replacement logins, intentional logouts, and competing/stale network responses.
9. **Typed Error Categories & Localized UI Strings:** Distinguishes `TokenErrorKind::Inaccessible`, `TokenErrorKind::MigrationFailed`, and `TokenErrorKind::SaveFailed`. In `en-US.json` and `pt-BR.json`, distinct banners and error toasts are provided with 100% key and placeholder parity.
10. **Strict Spec II Isolation:** Gran Turismo 4 Spec II mod playthroughs remain completely isolated: RA launch environment is omitted, `GT4_RA_ENABLED=0` is explicitly set, and no credentials or envelopes are forwarded.

---

## 2. Review 2 Findings & Implemented Corrections

### Finding 1: Robust Temporary-File Handling & Filesystem Seams
- **Problem:** `write_disk_atomically` previously used a fixed static name `retroachievements.toml.tmp` and returned directly on `fs::write` error without cleaning up partial temporary files. Injected rename failure by making destination a directory did not exercise partial writes or process interruption, and shared static names could cause race collisions between independent instances.
- **Fix:**
  - Introduced `AtomicFileOps` trait in `gt4l_core::retroachievements` with `StdAtomicFileOps` and `MockAtomicFileOps` (supporting `fail_before_write`, `fail_during_write`, `fail_flush`, `fail_replace`).
  - Implemented dynamic unique temporary naming using process ID and an atomic counter: `.{filename}.tmp-{pid}-{counter}`.
  - Implemented `TempFileGuard` RAII cleanup: automatically unlinks the specific temporary file on any error before `persisted = true` is set upon successful rename. Unrelated pre-existing files (e.g. `retroachievements.toml.tmp` or other files) are untouched.
  - Clarified and tested process interruption: forced crash leaves only ciphertext in `.tmp`, never plaintext; on restart, `RaConfig::load` ignores orphaned `.tmp` files and subsequent retry succeeds cleanly.
- **Evidence:** Tested in `retroachievements_tests::interrupted_and_failed_atomic_writes_are_safe` covering unrelated file preservation, pre-write failure, partial write failure (15 bytes), flush failure, rename failure, process interruption simulation, retry recovery, and instance concurrency.

### Finding 2: Launcher Command Boundary Coverage
- **Problem:** Previous flow audit called `loaded.save_with` directly rather than exercising the launcher command boundary (`commands::ra_set_options`, `ra_login`, `ra_logout`) with injected migration/write failures and competing network replies.
- **Fix:**
  - Exposed `ra_status_at_with`, `ra_set_options_with`, `ra_set_options_with_custom`, `ra_login_at_with`, `ra_logout_with` in `gt4l-tauri::commands` accepting injectable protectors and `AtomicFileOps`.
  - In `flow_audit.rs` Step 8:
    - Step 8A: `commands::ra_status_at_with` with `fail_protector` surfaces `relogin_required` and `TokenErrorKind::MigrationFailed`, keeping legacy file untouched.
    - Step 8B: `commands::ra_set_options_with` aborts with `Err` explaining migration safety, keeping legacy file 100% untouched.
    - Step 8C: `commands::ra_set_options_with_custom` with partial write failure aborts with `Err`, leaving legacy file intact and cleaning up temporary files.
    - Step 8D: Stale network reply test using TCP server `SyncGate`: an in-flight login response delayed at the server is safely fenced off when `ra_revision` is bumped; stale credentials do not overwrite disk or resurrect tokens.
    - Step 8E: Replacement login via `commands::ra_login_at_with` cleanly replaces legacy credentials with protected ciphertext envelope.
    - Step 8F: Explicit logout via `commands::ra_logout_with` cleanly removes credentials and protected envelope.
- **Evidence:** Tested in `flow_audit::retroachievements_network_race_tests_with_real_tcp_server` (finished in 0.21s, passed).

### Finding 3: Reconciled UI Error Categories and Accurate Reports
- **Problem:** Need accurate localized error handling for save errors and migration errors (not just a fabricated status value), accurate report of the 589/5 workspace test count, and clear distinction between unaffected native binaries and launcher tests.
- **Fix:**
  - Added `ra.save_failed_desc`, `ra.err_save_migration`, `ra.err_save_failed` to `en-US.json` and `pt-BR.json` with 100% parity.
  - In `achievements.js`: mapped option save errors to localized error toasts (`ra.err_save_migration` / `ra.err_save_failed`) and refreshed status on error.
  - Reconciled workspace test suite: **34 test blocks, 589 passed, 0 failed, 5 ignored**.
  - Documented that native binaries (`slice335`, `slice336`) are unaffected and retain logs from Stage 4.
- **Evidence:** Playwright checks passed (215/215 en-US, 222/222 pt-BR, 209/209 spatial nav). Full workspace tests captured in `docs/tasks/evidence/ra-token-logs/cargo-test-workspace.log`.

---

## 3. Threat Model Boundaries & Security Disclosures

1. **What Windows DPAPI Protects:**
   - Protects the stored RetroAchievements authentication token at rest against offline extraction (e.g. stolen laptop, offline disk imaging, backup extraction by unauthorized parties).
   - Protects against other user accounts on the same Windows computer reading or using the stored credentials.
2. **What Windows DPAPI Does Not Protect (Real Limitations):**
   - **Same-User Execution Context:** Does not protect against malware, scripts, or untrusted code running within the *same* Windows user session (as that process runs with the user's logon security token and can call `CryptUnprotectData`).
   - **Process Memory Inspection:** Does not protect against local memory scraping or debuggers inspecting the launcher or game process while running.
   - **Process Environment Handoff:** Does not alter how the token is handed off to `gt4vulkan.exe` via process environment variables (`GT4_RA_TOKEN`) during game launch.
   - **Pre-Migration Backups & Storage Remnants:** Does not perform physical cryptographic erasure of underlying storage blocks (SSD wear-leveling / TRIM); previous historical backups created by the OS or user prior to rc7 may retain plaintext tokens.
3. **Downgrade Behavior:**
   - If a user downgrades their installation from rc7 to 1.2.4 or 1.2.5, older versions do not recognize the `token_protected` envelope. The user will simply need to enter their credentials again. The launcher never downgrades ciphertext back to plaintext.

---

## 4. Automated Verification Results

All tests were executed serially from the workspace root `C:/Antigravity/gt4-wt-133-rc7-ra-controls`.

### 4.1 RetroAchievements Unit & Integration Test Suite (`gt4l-core`)
- **Command:**
  ```powershell
  cargo test --test retroachievements_tests -p gt4l-core --target-dir O:\gt4recomp-scratch\task133-cargo-target-v2 --manifest-path C:\Antigravity\gt4-wt-133-rc7-ra-controls\launcher\Cargo.toml
  ```
- **Result:** **21 passed, 0 failed, 0 ignored** (finished in 0.14s)
  - `diagnostic_export_forbids_retroachievements_file` ... ok
  - `diagnostic_export_exclusions_comprehensive` ... ok
  - `malformed_non_ascii_and_truncated_ciphertext_never_panics` ... ok
  - `launch_environment_includes_language_tag` ... ok
  - `debug_and_serialization_paths_never_leak_plaintext_token` ... ok
  - `fake_protector_encrypt_decrypt_and_injected_failures` ... ok
  - `real_windows_dpapi_roundtrip_tampered_blob_and_empty_handling` ... ok
  - `retroachievements_cache_explicitly_resolves_under_gt4_in_modern_layout` ... ok
  - `hostile_inherited_retroachievements_environment_is_scrubbed` ... ok
  - `failed_migration_leaves_original_plaintext_file_untouched` ... ok
  - `hostile_inherited_retroachievements_environment_with_case_variations_is_scrubbed_and_redacted` ... ok
  - `switching_games_preserves_shared_retroachievements_file` ... ok
  - `legacy_plaintext_migration_from_124_125_config_and_clean_disk_replacement` ... ok
  - `migration_failure_prevents_option_updates_from_destroying_legacy_token` ... ok
  - `unprotectable_token_preserves_ciphertext_and_non_secret_settings` ... ok
  - `retroachievements_config_125_compatibility_and_malformed_handling` ... ok
  - `real_windows_dpapi_end_to_end_migration_and_persistence` ... ok
  - `two_portable_profiles_and_installed_profile_independence` ... ok
  - `retroachievements_config_roundtrips_in_installed_and_portable_layouts` ... ok
  - `interrupted_and_failed_atomic_writes_are_safe` ... ok
  - `fake_server_http_login_refresh_and_failure_scenarios` ... ok

### 4.2 Full Rust Workspace Test Suite Reconciliation
- **Command:**
  ```powershell
  $env:GT4_SDL3_ROOT = "C:/Antigravity/gt4-staticrecomp/private/tooling/sdl3-3.4.18"
  cargo test --workspace --target-dir O:\gt4recomp-scratch\task133-cargo-target-v2 --manifest-path C:\Antigravity\gt4-wt-133-rc7-ra-controls\launcher\Cargo.toml
  ```
- **Durable Log:** `docs/tasks/evidence/ra-token-logs/cargo-test-workspace.log`
- **Reconciliation of Workspace Totals:**
  - Total test result blocks: **34** (29 executable test suites + 5 doc-tests)
  - Total tests passed: **589**
  - Total tests failed: **0**
  - Total tests ignored: **5** (2 in `real_disc.rs`, 2 in `gt4l_platform`, 1 in `gt4l-tauri`)
- **Complete Suite Breakdown:**
  1. `gt4controls` (lib): 18 passed, 0 failed, 0 ignored
  2. `gt4controls` (docs_match): 4 passed, 0 failed, 0 ignored
  3. `gt4l_core` (lib): 215 passed, 0 failed, 0 ignored
  4. `gt4l_core` (config_migration): 16 passed, 0 failed, 0 ignored
  5. `gt4l_core` (enabled_values): 3 passed, 0 failed, 0 ignored
  6. `gt4l_core` (gs_golden): 5 passed, 0 failed, 0 ignored
  7. `gt4l_core` (gs_release): 11 passed, 0 failed, 0 ignored
  8. `gt4l_core` (launch_contract): 21 passed, 0 failed, 0 ignored
  9. `gt4l_core` (multi_game): 5 passed, 0 failed, 0 ignored
  10. `gt4l_core` (release_lists): 5 passed, 0 failed, 0 ignored
  11. `gt4l_core` (relocate): 20 passed, 0 failed, 0 ignored
  12. `gt4l_core` (retroachievements_tests): 21 passed, 0 failed, 0 ignored
  13. `gt4l_core` (settings_config): 9 passed, 0 failed, 0 ignored
  14. `gt4l_core` (textures): 7 passed, 0 failed, 0 ignored
  15. `gt4l_disc` (lib): 6 passed, 0 failed, 0 ignored
  16. `gt4l_disc` (elf_rebuild): 3 passed, 0 failed, 0 ignored
  17. `gt4l_disc` (real_disc): 1 passed, 0 failed, 2 ignored
  18. `gt4l_disc` (setup_fake_iso): 12 passed, 0 failed, 0 ignored
  19. `gt4l_disc` (spec2_identify): 8 passed, 0 failed, 0 ignored
  20. `gt4l_platform` (lib): 48 passed, 0 failed, 2 ignored
  21. `gt4l_platform` (job_breakaway): 4 passed, 0 failed, 0 ignored
  22. `gt4l-tauri` / `GT4Recomp` (lib/bin, including `flow_audit` steps 1-8): 92 passed, 0 failed, 1 ignored
  23. `gt4saves` (lib): 6 passed, 0 failed, 0 ignored
  24. `gt4saves` (card_owner): 6 passed, 0 failed, 0 ignored
  25. `gt4saves` (hostile_cards): 10 passed, 0 failed, 0 ignored
  26. `gt4saves` (library): 12 passed, 0 failed, 0 ignored
  27. `gt4saves` (migration): 7 passed, 0 failed, 0 ignored
  28. `gt4saves` (per_game): 5 passed, 0 failed, 0 ignored
  29. `gt4saves` (save_management): 9 passed, 0 failed, 0 ignored
  30-34. Doc-tests (`gt4controls`, `gt4l_core`, `gt4l_disc`, `gt4l_platform`, `gt4saves`): 0 passed each

### 4.3 Playwright UI Focus and Spatial Navigation Checks
- **Command:**
  ```powershell
  $env:NODE_PATH = "C:\Users\Alano\AppData\Roaming\npm\node_modules"
  node C:\Antigravity\gt4-wt-133-rc7-ra-controls\docs\tasks\evidence\stage4-ui\run_ui_checks.js
  ```
- **Durable Log:** `docs/tasks/evidence/ra-token-logs/ui-checks.log`
- **Results:**
  - Focus Check (`focus-check.html?lang=en-US`): **215 of 215 passed**, 0 JS errors (`body[data-focus-check]="pass"`).
  - Focus Check (`focus-check.html?lang=pt-BR`): **222 of 222 passed**, 0 JS errors (`body[data-focus-check]="pass"`).
  - Spatial Navigation (`nav-test.html`): **209 of 209 passed**, 0 failures, 0 JS errors.

### 4.4 Native CTest Provenance & Baseline Status
- **Candidate Slices:** `slice335` (v2.00) and `slice336` (Spec II) built during Stage 4.
- **Status:** Native binaries were not modified in this launcher token protection task. Existing durable logs remain documented in `docs/tasks/evidence/stage4-logs/`:
  - `ctest-full-v200.log`: 49/50 passed; 1 baseline failure in `python_native_contracts` (710 tests, 13 known GS settings failures, 42 skipped).
  - `ctest-full-spec2.log`: 49/50 passed; 1 baseline failure in `python_native_contracts` (710 tests, 13 known GS settings failures, 55 skipped).
  - `curve-parity.log`: 8,960 / 8,960 bit-accurate matches (100.0000%).
- Unaffected native binaries are not falsely presented as newly executed.

---

## 5. Scope & Artifact Disposition

- **Worktree:** Strictly on `C:/Antigravity/gt4-wt-133-rc7-ra-controls`.
- **Branch:** `task/133-rc7-ra-controls`.
- **Files Staged & Committed:**
  - `docs/tasks/TASK-133-RA-TOKEN-PROTECTION-REPORT.md`
  - `docs/tasks/TASK-133-RA-TOKEN-PROTECTION-REVIEW-2.md`
  - `docs/tasks/evidence/ra-token-logs/cargo-test-workspace.log`
  - `docs/tasks/evidence/ra-token-logs/ui-checks.log`
  - `launcher/crates/gt4l-core/src/retroachievements.rs`
  - `launcher/crates/gt4l-core/tests/retroachievements_tests.rs`
  - `launcher/crates/gt4l-tauri/src/commands.rs`
  - `launcher/crates/gt4l-tauri/src/flow_audit.rs`
  - `launcher/crates/gt4l-tauri/ui/js/pages/achievements.js`
  - `launcher/crates/gt4l-tauri/ui/locales/en-US.json`
  - `launcher/crates/gt4l-tauri/ui/locales/pt-BR.json`
- **Zero Real Credentials or Live Server Calls:** All tests use synthetic data, `FakeProtector`, local TCP server loops, or offline DPAPI round-trips.
- **Zero Game/Installer Execution:** Neither the installer nor the game executable was launched.
- Ready for independent review by Codex.
