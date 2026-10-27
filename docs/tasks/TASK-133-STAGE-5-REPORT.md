# TASK-133 Stage 5: Local rc7 Test Kit Assembly & Verification Report

Date: 2026-10-27  
Workspace: `C:\Antigravity\gt4-wt-133-rc7-ra-controls`  
Head Commit: `9ec11a4 docs(task-133): accept controls and plan local rc7 kit`  
Branch: `task/133-rc7-ra-controls`  
Author: João Alano <57041838+moesuito@users.noreply.github.com>

---

## 1. Executive Summary & Boundaries

Stage 5 of TASK-133 has assembled and validated a complete, local, installable and portable **1.3.0-rc7 test kit**. This test kit integrates:
1. Gran Turismo 4 v2.00 and Spec II 1.10 dual-target architecture (from rc6).
2. RetroAchievements support for Gran Turismo 4 v2.00 (integrated from 1.2.4), strictly isolated from Spec II.
3. 7 canonical controller response curves and standardized trigger travel (integrated from 1.2.5).

### Strict Execution Boundaries Enforced:
- **No Subagents:** Executed directly by the single repository writer.
- **No Game or Installer Execution:** The installer and game executables were not run.
- **No Private or Copyrighted Game Data:** Zero ISOs, ELFs, memory cards (`card.ps2`), saves, or owner credentials were included or staged.
- **No Remote Publishing:** Zero GitHub draft releases, tags, PRs, or public releases were created (`gh release create --draft` strictly avoided).
- **Tooling Isolation:** Release binaries and packages built on drive `O:` to preserve machine workspace boundaries.

---

## 2. Toolchain, Environment & Disk Provenance

- **Operating System:** Windows 11 Pro x64 (Build 26100)
- **Compiler:** Clang-cl 22.1.3 (`C:/Program Files/LLVM/bin/clang-cl.exe`) / MSVC 17.14
- **Build System:** Ninja 1.13.1, CMake 3.31
- **Rust Toolchain:** cargo 1.92.0 / rustc 1.92.0
- **Tauri CLI:** 2.11.0
- **NSIS:** Makensis v3.13 (`C:\Program Files (x86)\NSIS\makensis.exe`)
- **Archive Tool:** 7-Zip 25.01 x64 (`C:\Program Files\7-Zip\7z.exe`)
- **Python:** Python 3.12 (venv `private/tooling-venv/Scripts/python.exe`)
- **Disk Availability:**
  - `C:`: ~28.8 GB free
  - `O:`: ~89.8 GB free

---

## 3. Build Provenance & Input Manifests

### 3.1 Candidate Native Executables (Built in Stage 4)
- **Gran Turismo 4 (SCUS-97328 v2.00):**
  - Binary: `O:\gt4recomp-scratch\slice335-pcsx2-build\build\gt4vulkan.exe`
  - Renamed for distribution: `gt4.exe`
  - Size: 97,702,912 bytes
  - SHA-256: `17406ba4bfcd3ded71e68427c45b3528b43c1caccb3c4872fda3ded5634ae917`
  - Pinned Manifest: `docs/inputs/slice335-pcsx2.json`
- **Gran Turismo 4 Spec II (Mod 1.10):**
  - Binary: `O:\gt4recomp-scratch\slice336-spec2-build\build\gt4vulkan.exe`
  - Renamed for distribution: `gt4-spec2.exe`
  - Size: 104,719,872 bytes
  - SHA-256: `67951470d1faf767c8d619bac5483bbfd30f9b164e6aaeb04e7ad20858083f1a`
  - Pinned Manifest: `docs/inputs/slice336-spec2.json`

### 3.2 Game Distribution Staging
Assembled via `scripts/make_distribution.py` into:
`C:\Antigravity\gt4-staticrecomp\private\slice337-dist-130-rc7`
Contents (19 files total):
- `gt4.exe` (97,702,912 bytes)
- `gt4-spec2.exe` (104,719,872 bytes)
- `SDL3.dll` (2,848,256 bytes, SDL 3.4.18)
- Visual C++ Runtime: `msvcp140.dll` (557,728 bytes), `msvcp140_atomic_wait.dll` (50,304 bytes), `vcruntime140.dll` (124,544 bytes)
- `MANIFEST.json` (3,796 bytes)
- Shaders (`shaders/`): `copy_read.spv`, `copy_write.spv`, `decode.spv`, `display.spv`, `load_clut.spv`, `raster.spv`, `resolve.spv`, `snapshot.spv`, `upload.spv`
- Licenses (`licenses/`): `LICENSE-GT4Recomp.txt`, `LICENSE-ps2-web-recomp.txt`, `LICENSE-SDL3.txt`, `THIRD_PARTY.md`

### 3.3 Launcher Release Builds
Built with `launcher/tools/build-release.ps1 -Label rc7` and path prefix remapping:
- **Installer Edition (`GT4Recomp.exe`):**
  - Path: `O:\gt4recomp-scratch\task133-cargo-target\release\GT4Recomp.exe`
  - Size: 17,953,792 bytes
  - SHA-256: `74f14811f63c16719cf6d9dad0c1eeed39e92aa3939724650de0594522ec204d`
  - Machine path scan: 0 hits (clean).
- **Portable Edition (`GT4Recomp.exe`):**
  - Path: `O:\gt4recomp-scratch\task133-cargo-target-portable\release\GT4Recomp.exe`
  - Size: 17,936,896 bytes
  - SHA-256: `b0d12f730e39cdedebaa52afcbf1a5bf572c98a1470fcba2297b1be3f16bce33`
  - Machine path scan: 0 hits (clean).

### 3.4 Third-Party Components
- **WebView2 Bootstrapper:**
  `C:\Antigravity\gt4-staticrecomp\private\release-100-webview2\MicrosoftEdgeWebview2Setup.exe`
  - Size: 1,876,736 bytes
  - SHA-256: `f414de01c4680f0bb0694d1ea7803c6968f3a81bf6bad01b89bfc65dce14c2a2`
- **SDL3 Runtime:**
  `C:\Antigravity\gt4-staticrecomp\private\tooling\sdl3-3.4.18\runtime\SDL3.dll`
  - Size: 2,848,256 bytes
  - SHA-256: `785ae6a8df6a7e0d3753a3eb1777b7899bb57c8bfbe19e0788eb59b6feeb10d4`

---

## 4. Test Kit Packages & SHA256SUMS

Output destination: `O:\gt4recomp-scratch\task133-rc7-kit\`

| Package Asset | Size (bytes) | SHA-256 Hash |
| :--- | :--- | :--- |
| `GT4Recomp-Setup-1.3.0-rc7.exe` | 51,818,079 | `437c9eacc84c0a0d628d10b91e7e1d6665ab097adc0c8a29ed592cc6ba8a35c5` |
| `GT4Recomp-1.3.0-rc7-portable.zip` | 83,613,348 | `530d5fb6698488213d85b5af74a1ef10fb7545cf3bbb96c8fca9517034328d7b` |

### `SHA256SUMS` Contents:
```text
437c9eacc84c0a0d628d10b91e7e1d6665ab097adc0c8a29ed592cc6ba8a35c5  GT4Recomp-Setup-1.3.0-rc7.exe
530d5fb6698488213d85b5af74a1ef10fb7545cf3bbb96c8fca9517034328d7b  GT4Recomp-1.3.0-rc7-portable.zip
```

---

## 5. Archive Contents & Integrity Validation

Both packages were unpacked to an isolated test directory using 7-Zip (`7z x`) and systematically verified:

### 5.1 Binary Hash Exactness
- `game/gt4.exe` (both archives) matches `slice335-pcsx2.json`:
  `17406ba4bfcd3ded71e68427c45b3528b43c1caccb3c4872fda3ded5634ae917` (Exact bit match).
- `game/gt4-spec2.exe` (both archives) matches `slice336-spec2.json`:
  `67951470d1faf767c8d619bac5483bbfd30f9b164e6aaeb04e7ad20858083f1a` (Exact bit match).
- `GT4Recomp.exe` in setup matches candidate installer build:
  `74f14811f63c16719cf6d9dad0c1eeed39e92aa3939724650de0594522ec204d` (Exact bit match).
- `GT4Recomp.exe` in portable zip matches candidate portable build:
  `b0d12f730e39cdedebaa52afcbf1a5bf572c98a1470fcba2297b1be3f16bce33` (Exact bit match).

### 5.2 Cleanliness & Data Privacy
- **Forbidden file check:** 0 `.iso`, 0 `.bin`, 0 `.elf`, 0 `.ps2`, 0 `.mc2`, 0 `.gt4`, 0 `.log` files found.
- **Forbidden name check:** 0 `core.gt4`, 0 `scus_973.28`, 0 `card.ps2`, 0 `credentials.json` found.
- **Directory Structure:**
  - Portable zip root includes: `GT4Recomp.exe`, `SDL3.dll`, `NOTICE.md`, `LICENSE.txt`, `PORTABLE.txt`, `user/` (empty), `data/` (empty), and `game/` with both targets.
  - Setup installer includes `.update-staging/` hierarchy with identical game layout plus `$PLUGINSDIR\MicrosoftEdgeWebview2Setup.exe`.

---

## 6. Automated Verification Summary

All automated controls were executed serially prior to kit packaging:

1. **Locales Parity:**
   - English (`en-US.json`): 604 keys.
   - Portuguese (`pt-BR.json`): 604 keys.
   - Missing keys: 0. Missing placeholders: 0.
2. **Playwright UI Checks:**
   - `focus-check.html?lang=en-US`: 215/215 passed, 0 failures.
   - `focus-check.html?lang=pt-BR`: 222/222 passed, 0 failures.
   - `nav-test.html`: 209/209 passed, 0 failures.
   - JavaScript runtime console errors: 0.
3. **Controller Curve Parity Sweep:**
   - 8,960/8,960 points passed (100.0000% parity between native C++ `controls.cpp` and JS `controls-model.js` across 7 curves, 5 deadzones, and 256 inputs).
4. **Rust Unit & Integration Workspace:**
   - 34 test blocks, 578 tests passed, 0 failed, 5 ignored (`live_network` tests).
5. **Targeted CTest:**
   - Gran Turismo 4 v200 (`slice335`): 5/5 passed (`native_audio_input`, `native_controls`, `ra_native_memory`, `ra_native_session`, `ra_native_leaderboard`).
   - Gran Turismo 4 Spec II (`slice336`): 5/5 passed.

### Accounting of Known Baseline Failures:
The 13 known baseline failures in the full Python CTest suite (from rc6) remain identified as existing GS settings/live window test expectations. They are tracked as known baseline behavior and were not modified or masked.

---

## 7. Owner Manual Test Checklist

The following items cannot be validated through automated mocks and require owner in-game verification on physical hardware:

- [ ] **Dual-Game Selection:**
  - Launch `GT4Recomp.exe`. Select Gran Turismo 4 (v2.00) and verify boot into menus/track.
  - Return to launcher, select Spec II (1.10) and verify separate boot and independent save card.
- [ ] **RetroAchievements Integration (Gran Turismo 4 v2.00):**
  - Navigate to **Settings > General > RetroAchievements**.
  - Enter valid credentials. Confirm banner shows logged-in status.
  - Launch Gran Turismo 4 v2.00: verify rich presence and achievement triggers fire.
  - Launch Spec II: verify RetroAchievements is cleanly deactivated (no attempts to hash or track Spec II).
- [ ] **Controller Response Curves & Triggers:**
  - Navigate to **Settings > Controls**.
  - Test all 7 curves (Linear, Quadratic, Cubic, S-Curve, Smooth Power, Sublinear, Logarithmic) using the visual curve preview.
  - In-game: test steering and throttle/brake feel on a physical gamepad (DualSense/DualShock 4 or Xbox controller).
  - Verify smooth, standardized trigger response (0..100% travel without sudden step-jumps).
- [ ] **Saves & Settings:**
  - Create a new save, export save, and rename save. Confirm tag labels match active game.
- [ ] **Audio & Pacing:**
  - Verify smooth audio pacing without crackles, buffer underruns, or presentation stuttering.

---

## 8. Release Documentation & Git State

### Updated Documentation:
- `docs/inputs/slice335-pcsx2.json`: candidate v200 native build manifest.
- `docs/inputs/slice336-spec2.json`: candidate Spec II native build manifest.
- `docs/release-notes/1.3.0-rc7.md`: full technical release notes for rc7.
- `docs/release-notes/launcher/1.3.0.md` & `pt-BR/1.3.0.md`: updated cumulative launcher notes.
- `docs/release-notes/launcher/1.3.0-rc7.md` & `pt-BR/1.3.0-rc7.md`: version-specific launcher notes.
- `docs/tasks/TASK-133-STAGE-5-REPORT.md`: this report.

### Git Status & Review Hand-off:
All generated release assets and logs remain strictly in `O:\gt4recomp-scratch\task133-rc7-kit\`. Only authorized documentation and input manifests are staged for commit. Work stops here for Codex review.
