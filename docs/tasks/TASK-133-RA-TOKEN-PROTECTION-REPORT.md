# TASK-133: RetroAchievements Credential Protection Report

Date: 2026-10-27.  
Branch: `task/133-rc7-ra-controls`  
Worktree: `C:/Antigravity/gt4-wt-133-rc7-ra-controls`  
Starting Base: `adfd5132b775d354d4a5bb33152fb23fb542b85e`  
Review Base: `c3c9959`  
Author: João Alano <57041838+moesuito@users.noreply.github.com>

---

## 1. Executive Summary & Design Overview

This work implements secure protection at rest for RetroAchievements authentication tokens on Windows for both installed and portable launcher layouts, addressing all requirements of `TASK-133-RA-TOKEN-PROTECTION.md` and the findings of `TASK-133-RA-TOKEN-PROTECTION-REVIEW.md`.

### Core Design Principles
1. **Windows DPAPI Standard:** Uses Win32 Data Protection API (`CryptProtectData` and `CryptUnprotectData` via `crypt32.dll` and `kernel32.dll`) bound strictly to the current Windows user (`dwFlags = CRYPTPROTECT_UI_FORBIDDEN`, no machine-wide scope `CRYPTPROTECT_LOCAL_MACHINE`). No custom cryptography or bundled symmetric keys.
2. **Platform-Neutral Abstraction:** Introduced a clean `SecretProtector` trait and `SecretError` enum in `launcher/crates/gt4l-core/src/secret.rs`. Non-Windows platforms return explicit `UnsupportedPlatform` errors without silently persisting plaintext or falling back to fake implementations.
3. **Deterministic Testability:** Implemented `FakeProtector` for unit and concurrency tests, with configurable key XOR and injected failure hooks (`fail_protect`, `fail_unprotect` simulating `NTE_BAD_KEYSET` / user mismatch, and corrupted envelope detection).
4. **Zero Plaintext at Rest & Serde Serialization Guard:** In `RaConfig`, `token` is annotated with `#[serde(default, skip_serializing)]`, ensuring that the TOML serializer can never write plaintext tokens to disk under any condition. `token_protected` stores the versioned envelope `dpapi:v1:<hex>`.
5. **Safe, Atomic Migration with Legacy File Preservation:** Existing legacy plaintext `retroachievements.toml` files are migrated automatically on first load. The token is encrypted and round-trip verified in memory before atomically replacing the original configuration via a sibling `.toml.tmp` file. If encryption or writing fails, the original legacy plaintext file is left 100% untouched. Crucially, if protection fails during migration, `unmigrated_plaintext` is preserved in memory such that subsequent unrelated option changes (e.g. toggling hardcore/enabled) will abort save rather than overwriting disk with an unauthenticated config that erases the user's old token.
6. **Typed Token Error Categories & Localized Guidance:** Distinguishes between `TokenErrorKind::Inaccessible` (an already-encrypted session that cannot be unlocked on this PC/user), `TokenErrorKind::MigrationFailed` (an old plaintext token that could not be protected/written), and `TokenErrorKind::SaveFailed`. The launcher surfaces distinct, localized banners in English (`en-US`) and Brazilian Portuguese (`pt-BR`).
7. **Byte-Safe Hex Decoding:** Operates strictly on byte chunks (`as_bytes().chunks_exact(2)` with `hex_val`), preventing slice panics on arbitrary malformed multi-byte UTF-8, odd length, bad characters, or empty/truncated inputs.
8. **Audit Debug & Serialization Paths:** Implemented custom `fmt::Debug` implementations for `RaConfig` and `RaAccountSession` that redact `token` to `[REDACTED]`. Synthetic regression tests verify that `format!("{cfg:?}")`, `format!("{session:?}")`, `toml::to_string(&cfg)`, and diagnostic export files never emit token secrets.
9. **Strict Spec II Isolation:** Gran Turismo 4 Spec II mod playthroughs remain completely isolated: RA launch environment is omitted, `GT4_RA_ENABLED=0` is explicitly set, and no credentials or envelopes are forwarded.

---

## 2. Review Findings & Implemented Fixes

### Finding 1: Migration Failure Safety Across Option Updates
- **Problem:** When migration failed on load, `token` was cleared in memory. If the user or launcher then updated an unrelated setting (such as toggling hardcore or enabled), `save_with` serialized the config with no token and no `token_protected`, overwriting the disk and permanently erasing the user's old token.
- **Fix:** Added `unmigrated_plaintext: Option<String>` (`#[serde(skip)]`) to `RaConfig`. When migration fails on load, the legacy plaintext is retained in `unmigrated_plaintext`. In `save_with`, if `unmigrated_plaintext` is present and protection still fails, `save_with` refuses to overwrite the configuration file, returning `Err("cannot save settings: existing legacy credentials could not be protected, preserving original configuration file")`. If explicit logout is performed (`clear_credentials()`), `unmigrated_plaintext` is cleared, allowing intentional credential removal.
- **Evidence:** Tested deterministically in unit test `migration_failure_prevents_option_updates_from_destroying_legacy_token` and launcher integration flow test `flow_audit.rs` (Step 8).

### Finding 2: Safe Hex Parsing Without Panics on Non-ASCII / UTF-8
- **Problem:** `secret::hex_decode` previously used `&s[i..i+2]`, which panicked when slicing across multi-byte UTF-8 boundaries (e.g. `dpapi:v1:aéa`).
- **Fix:** Rewrote `hex_decode` to operate strictly on byte slices via `s.trim().as_bytes()`, verifying even length and parsing byte pairs using `chunks_exact(2)` with a dedicated `hex_val(b: u8) -> Result<u8, SecretError>` helper. Non-ASCII characters, odd lengths, bad hex characters, and empty payloads are handled cleanly, returning `Err(SecretError::CorruptData(...))` without panics or memory corruption.
- **Evidence:** Tested in `malformed_non_ascii_and_truncated_ciphertext_never_panics` covering multi-byte accented characters (`aéa`, `\\u{00E9}`), emoji byte sequences (`\\u{1F980}`, `\\u{1F600}`), odd lengths, empty envelopes (`dpapi:v1:`, `fake:v1:`), and invalid prefixes.

### Finding 3: Typed Error Categories and Distinct UI Guidance
- **Problem:** All token errors produced the identical "cannot be unlocked on this Windows account or PC" banner, failing to distinguish between migration failure and an unprotectable encrypted token.
- **Fix:**
  - Added `pub enum TokenErrorKind { Inaccessible, MigrationFailed, SaveFailed }` to `gt4l_core::retroachievements`.
  - Added `token_error_kind: Option<TokenErrorKind>` to `RaStatusView`.
  - Added localized strings in `en-US.json` and `pt-BR.json` for `ra.migration_failed_desc`:
    - `en-US`: `"Your saved session credentials could not be protected on this PC. Sign in again to restore achievement tracking."`
    - `pt-BR`: `"Não foi possível proteger suas credenciais salvas neste PC. Entre novamente para restaurar o rastreamento de conquistas."`
  - In `achievements.js`, the UI dynamically chooses between `ra.relogin_required_desc` (for `Inaccessible`) and `ra.migration_failed_desc` (for `MigrationFailed`).
- **Evidence:** 100% key and placeholder parity verified between `en-US.json` and `pt-BR.json`. Playwright focus checks pass (215/215 en-US, 222/222 pt-BR).

### Finding 4: Debug and Serialization Audit for Plaintext Leakage
- **Problem:** `RaConfig` derived `Debug` and had `skip_serializing_if = "String::is_empty"`, meaning a non-empty token could theoretically be formatted by debug loggers or serialized if set.
- **Fix:**
  - Replaced `#[derive(Debug)]` with custom `fmt::Debug` implementations for `RaConfig` and `RaAccountSession`, explicitly printing `token: "[REDACTED]"`.
  - Changed serde attribute on `RaConfig::token` to `#[serde(default, skip_serializing)]`. This allows deserializing legacy plaintext tokens from disk on load, but makes it impossible for `serde` to serialize `token` back to disk, even if present in memory.
  - Verified diagnostic export exclusions (`diagnostic_export_forbids_retroachievements_file` and `diagnostic_export_exclusions_comprehensive`).
- **Evidence:** Tested in `debug_and_serialization_paths_never_leak_plaintext_token`.

### Finding 5: Concurrency, Race, Write Failure & Profile Isolation Tests
- **Problem:** Need deterministic validation of write interruptions, profile isolation, and multi-game switching.
- **Fix:**
  - `interrupted_and_failed_atomic_writes_are_safe`: verifies atomic `.toml.tmp` cleanup and directory collision failure handling.
  - `two_portable_profiles_and_installed_profile_independence`: verifies 2 portable profiles and 1 installed profile operate completely independently with separate keys and configs.
  - `switching_games_preserves_shared_retroachievements_file`: verifies switching between GT4 and Spec II preserves credentials and isolation.
- **Evidence:** All tests pass in `retroachievements_tests.rs`.

### Finding 6: Resolution of Line-Ending Churn
- **Problem:** Commit `c3c9959` had full-file line ending churn across multiple files due to unintended CRLF writes to LF repository files.
- **Fix:** Rebuilt all touched files using exact byte stream writes matching original repository blob conventions:
  - LF files: `lib.rs`, `retroachievements.rs`, `secret.rs`, `retroachievements_tests.rs`, `achievements.js`, `en-US.json`, `pt-BR.json`, `1.3.0-rc7.md`, `launcher/1.3.0*.md`.
  - CRLF files: `commands.rs`, `flow_audit.rs`.
- **Evidence:** `git diff adfd513 --stat` shows 16 files changed, 1,235 insertions(+), 94 deletions(-) with zero line-ending churn.

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
  - `launch_environment_includes_language_tag` ... ok
  - `diagnostic_export_exclusions_comprehensive` ... ok
  - `fake_protector_encrypt_decrypt_and_injected_failures` ... ok
  - `debug_and_serialization_paths_never_leak_plaintext_token` ... ok
  - `malformed_non_ascii_and_truncated_ciphertext_never_panics` ... ok
  - `real_windows_dpapi_roundtrip_tampered_blob_and_empty_handling` ... ok
  - `retroachievements_cache_explicitly_resolves_under_gt4_in_modern_layout` ... ok
  - `switching_games_preserves_shared_retroachievements_file` ... ok
  - `failed_migration_leaves_original_plaintext_file_untouched` ... ok
  - `hostile_inherited_retroachievements_environment_is_scrubbed` ... ok
  - `hostile_inherited_retroachievements_environment_with_case_variations_is_scrubbed_and_redacted` ... ok
  - `legacy_plaintext_migration_from_124_125_config_and_clean_disk_replacement` ... ok
  - `migration_failure_prevents_option_updates_from_destroying_legacy_token` ... ok
  - `unprotectable_token_preserves_ciphertext_and_non_secret_settings` ... ok
  - `interrupted_and_failed_atomic_writes_are_safe` ... ok
  - `retroachievements_config_125_compatibility_and_malformed_handling` ... ok
  - `real_windows_dpapi_end_to_end_migration_and_persistence` ... ok
  - `two_portable_profiles_and_installed_profile_independence` ... ok
  - `retroachievements_config_roundtrips_in_installed_and_portable_layouts` ... ok
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
  - *Explanation of baseline difference:* The earlier Stage 4 baseline recorded 578 passed / 5 ignored. With the expansion of `retroachievements_tests` from 10 to 21 tests (+11 tests), the total count is exactly 578 + 11 = **589 passed**.
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
- No new native runs are falsely claimed without new logs or timestamps.

---

## 5. Scope & Artifact Disposition

- **Worktree:** Strictly on `C:/Antigravity/gt4-wt-133-rc7-ra-controls`.
- **Branch:** `task/133-rc7-ra-controls`.
- **Files Staged:**
  - `docs/release-notes/1.3.0-rc7.md`
  - `docs/release-notes/launcher/1.3.0.md`
  - `docs/release-notes/launcher/1.3.0-rc7.md`
  - `docs/release-notes/launcher/pt-BR/1.3.0.md`
  - `docs/release-notes/launcher/pt-BR/1.3.0-rc7.md`
  - `docs/tasks/TASK-133-RA-TOKEN-PROTECTION-REPORT.md`
  - `docs/tasks/TASK-133-RA-TOKEN-PROTECTION-REVIEW.md`
  - `docs/tasks/evidence/ra-token-logs/cargo-test-workspace.log`
  - `docs/tasks/evidence/ra-token-logs/ui-checks.log`
  - `launcher/crates/gt4l-core/src/lib.rs`
  - `launcher/crates/gt4l-core/src/retroachievements.rs`
  - `launcher/crates/gt4l-core/src/secret.rs`
  - `launcher/crates/gt4l-core/tests/retroachievements_tests.rs`
  - `launcher/crates/gt4l-tauri/src/commands.rs`
  - `launcher/crates/gt4l-tauri/src/flow_audit.rs`
  - `launcher/crates/gt4l-tauri/ui/js/pages/achievements.js`
  - `launcher/crates/gt4l-tauri/ui/locales/en-US.json`
  - `launcher/crates/gt4l-tauri/ui/locales/pt-BR.json`
- **Zero Real Credentials or Live Server Calls:** All tests use synthetic data, `FakeProtector`, local TCP server loops, or offline DPAPI round-trips.
- **Zero Game/Installer Execution:** Neither the installer nor the game executable was launched.
- Ready for independent review by Codex.
