# TASK-133 Stage 4 Report: Controller Improvements Integration & Parity Verification

**Date:** 2026-10-27  
**Workspace:** `C:\Antigravity\gt4-wt-133-rc7-ra-controls`  
**Branch:** `task/133-rc7-ra-controls`  
**Base Commit:** `83bcd1e` (Stage 3 Accepted by Codex)  
**Reviewer:** Codex Orchestrator  
**Status:** Stage 4 Complete — Paused for Codex Review  

---

## 1. Executive Summary & Review Scope Disposition

This report documents the completion of **Stage 4** of TASK-133 in strict compliance with `docs/tasks/TASK-133-STAGE-4-CONTROLLERS.md`, `docs/tasks/TASK-133-RC7-RA-CONTROLS-INTEGRATION.md`, and `AGENTS.md`.

### Disposition of Stage 4 Scope
1. **Controller-Only Semantics from 1.2.5:** Integrated the reviewed controller improvements from stable 1.2.5 (`v1.2.5` `5d7a669`, reviewed controller merge `8b73fa0` from PR #100 via #106).
2. **Seven Canonical Response Curves:** Preserved all seven canonical curve identifiers (`linear`, `quadratic`, `cubic`, `s_curve`, `smooth_power`, `sublinear`, `logarithmic`), including accepted aliases, defaults, and old-configuration backwards compatibility across native C++, Rust crates, and modern Tauri launcher models.
3. **Standardized SDL3 Trigger Travel:** Standardized gamepad trigger travel to the normalized SDL range `0..32767` reduced to byte `0..255` (`clamped >> 7`), eliminating device-name-specific heuristics (`DualSense`) in both native polling and launcher capture.
4. **Two-Language Parity:** Added localized player-facing choice strings for all four new curves in `en-US.json` and `pt-BR.json` with identical keys and placeholders.
5. **Clean Room Isolation:** Strictly adhered to clean-room boundaries:
   - Zero wheel/DFP/FFB bridge code (`wheel.cpp`, `wheel.rs`, `sdl_wheels.rs`, `wheel.toml` omitted).
   - Zero frame skipping from TASK-082 (unmerged PR #113 kept untouched).
   - Zero CoreGS work or GPL/PCSX2 source inspection.
   - Zero version bumps, installer compiling, portable zip generation, or release publications.
6. **Fresh Rebuilds on Drive O::** Built both targets from source in fresh slices (`slice335-rc7-controls-build` for v200, `slice336-rc7-controls-spec2` for Spec II) while keeping previous Stage 2/3 slices (`slice333`, `slice334`) completely intact.

---

## 2. File-by-File Applicability & Provenance

Comparing the 24 files touched in reviewed controller commit `8b73fa0` against the current rc6 base identified 17 applicable files, with the remaining files excluded or adapted:

| File | Status in Stage 4 | Semantic Mapping & Changes |
| --- | --- | --- |
| `native/controls.hpp` | Ported | Expanded `enum class Curve` with `s_curve`, `smooth_power`, `sublinear`, `logarithmic`. |
| `native/controls.cpp` | Ported | Implemented math shaping for all 4 curves; added alias parsing (`scurve`, `smooth`, `root`, `log`) and updated error message. |
| `native/controls_fixture.cpp` | Ported | Added unit tests for 7 curves (neutral=0, full=255, monotonicity, middle vector); rejection of `mode=dualsense`. |
| `native/host_io.hpp` | Ported | Declared `read_sdl_gamepad(SDL_Gamepad *gamepad)` for shared polling and virtual-device testing. |
| `native/host_io.cpp` | Ported | Extracted `read_sdl_gamepad` with standardized trigger normalization (`gt4::pad::trigger_byte`). |
| `native/host_io_fixture.cpp` | Ported | Added `check_standardized_sdl_triggers()` testing virtual SDL gamepad (PS5 and Xbox IDs) through rest, 1/4, 1/2, 3/4, and full travel. |
| `launcher/crates/gt4controls/src/lib.rs` | Ported | Added `Curve` variants, string conversion, alias parsing, error validation, round-trip serialization, and old-config compatibility tests. |
| `launcher/crates/gt4l-platform/src/pad.rs` | Ported | Replaced trigger division with bitshift `(clamped >> 7) as u8` to match native; added virtual trigger travel test. |
| `launcher/crates/gt4l-tauri/src/model.rs` | Ported | Updated `trigger_row` dropdown choices; added `curve_from_name` parser; updated `TriggerDto`; added unit test. |
| `launcher/crates/gt4l-tauri/ui/js/controls-model.js` | Ported | Updated `triggerOutput` to calculate `s_curve`, `smooth_power`, `sublinear`, `logarithmic` matching native formulas. |
| `launcher/crates/gt4l-tauri/ui/dev/mock-data.js` | Ported | Added the 4 new curves to analog dropdown mock definitions. |
| `launcher/crates/gt4l-tauri/ui/dev/nav-test.js` | Ported | Added quantized half-travel assertions for the 4 new curves in spatial navigation suite. |
| `launcher/crates/gt4l-tauri/ui/locales/en-US.json` | Added | Added `choice.curve.s_curve`, `choice.curve.smooth_power`, `choice.curve.sublinear`, `choice.curve.logarithmic`. |
| `launcher/crates/gt4l-tauri/ui/locales/pt-BR.json` | Added | Added localized Brazilian Portuguese equivalents with matching keys. |
| `docs/CONTROLS.md` | Ported | Documented the 7 canonical curves and explained standardized SDL3 Gamepad trigger behavior. |
| `launcher/crates/gt4l-app/src/main.rs` | Ported | Updated legacy Slint curve mapping and `curve_index` for exhaustive match integrity. |
| `launcher/crates/gt4l-app/ui/controller.slint` | Ported | Added the 4 curves to Slint ComboBox model and path preview command. |
| `docs/inputs/slice1200-pcsx2.json` | Excluded | Release pin for old stable 1.2.5 branch; inapplicable to rc7 candidate. |
| `scripts/external_runtime_probe.py` | Excluded | Pinned probe profiles belong to historical slices; no pin updates solely to hide mismatches. |
| `tests/python/test_gs_*.py` | Excluded | Historical probe pin tests testing `slice1200-pcsx2.json`; rc6 baseline preserved. |

### Disposition of Slint `gt4l-app`
- **Current Build Usage:** In `launcher/Cargo.toml`, workspace members include only `gt4l-core`, `gt4l-disc`, `gt4saves`, `gt4controls`, `gt4l-platform`, and `gt4l-tauri`. The legacy Slint crate `gt4l-app` is **not** a member of the active workspace and is not built during release or test workflows.
- **Disposition:** The changes in `gt4l-app/src/main.rs` and `gt4l-app/ui/controller.slint` were applied to ensure code integrity and avoid match exhaustion errors if the crate is ever compiled standalone. It remains legacy/inactive.

---

## 3. Mathematical Formula Parity & Verification Table

To ensure bit-accurate behavioral alignment between the game runtime, launcher tuning, and web preview UI, all seven response curves share identical mathematical definitions:

| Canonical Identifier | Formula \(f(n)\) for \(n \in [0, 1]\) | Native C++ (`native/controls.cpp`) | Rust (`gt4controls`) | Web UI (`controls-model.js`) | Quantized Byte at Half Travel (\(v=128\)) |
| --- | --- | --- | --- | --- | --- |
| `linear` | \(n\) | `value` | `Curve::Linear` | `n` | `128` (0.5020) |
| `quadratic` | \(n^2\) | `value * value` | `Curve::Quadratic` | `n * n` | `64` (0.2510) |
| `cubic` | \(n^3\) | `value * value * value` | `Curve::Cubic` | `n * n * n` | `32` (0.1255) |
| `s_curve` | \(n^2(3 - 2n)\) | `value * value * (3.0 - 2.0 * value)` | `Curve::SCurve` | `n * n * (3 - 2 * n)` | `128` (0.5020) |
| `smooth_power` | \(n^{1.5} = n\sqrt{n}\) | `value * std::sqrt(value)` | `Curve::SmoothPower` | `n * Math.sqrt(n)` | `90` (0.3529) |
| `sublinear` | \(n^{0.5} = \sqrt{n}\) | `std::sqrt(value)` | `Curve::Sublinear` | `Math.sqrt(n)` | `180` (0.7059) |
| `logarithmic` | \(\frac{\log_{10}(1 + 9n)}{\log_{10}(10)}\) | `std::log(1.0 + 9.0 * value) / std::log(10.0)` | `Curve::Logarithmic` | `Math.log(1 + 9 * n) / Math.log(10)` | `189` (0.7412) |

### Boundary & Monotonicity Properties Verified
- **Rest Point:** At \(v = 0\) (or \(v \le d\)), every curve evaluates to exactly `0` (pressure `0`).
- **Full Travel:** At \(v = 255\), every curve evaluates to exactly `255` (full pressure `255`).
- **Monotonicity:** In `native/controls_fixture.cpp`, every raw step \(0 \le raw \le 255\) is verified non-decreasing (`pressure >= prior`) across all 7 curves.

---

## 4. Fresh Native Build Provenance & Binary Hashes

Both game targets were configured and built from source in fresh slice directories on drive O: using Clang-cl (LLVM 22.1.3), Ninja, and MSVC 17.14 build tools.

### 4.1 Configure Recipes
#### v200 Candidate (`O:\gt4recomp-scratch\slice335-rc7-controls-build`)
```cmd
cmake -S . -B O:\gt4recomp-scratch\slice335-rc7-controls-build\build -G Ninja -DCMAKE_BUILD_TYPE=Release ^
  -DCMAKE_C_COMPILER="C:/Program Files/LLVM/bin/clang-cl.exe" ^
  -DCMAKE_CXX_COMPILER="C:/Program Files/LLVM/bin/clang-cl.exe" ^
  -DGT4_TARGET=v200 -DGT4_EE_PROFILE=aspect -DGT4_RELEASE_DISTRIBUTION=ON ^
  -DGT4_ASSETS_ROOT=C:/Antigravity/gt4-staticrecomp ^
  -DGT4_PYTHON=C:/Antigravity/gt4-staticrecomp/private/tooling-venv/Scripts/python.exe ^
  -DGT4_PERF_RUNTIME_SOURCE=C:/Antigravity/gt4-staticrecomp/private/slice262-frame-cost-runtime ^
  -DGT4_PORT_DIR=C:/Antigravity/gt4-staticrecomp/private/slice200-port ^
  -DGT4_VU1_GENERATED_DIR=C:/Antigravity/gt4-staticrecomp/private/slice157-vu1-all-gen ^
  -DGT4_VU0_GENERATED_DIR=C:/Antigravity/gt4-staticrecomp/private/slice160-vu0-gen-r3 ^
  -DGT4_VU0_IMAGE_FILE=C:/Antigravity/gt4-staticrecomp/private/slice160-vu0-image/vu0_image.bin ^
  -DGT4_VU_CORPUS_DIR=C:/Antigravity/gt4-staticrecomp/private/slice155-vu1-full-corpus ^
  -DGT4_LICENSE_GUARD_SOURCE=C:/Antigravity/gt4-staticrecomp/private/slice262-license-guard-runtime ^
  -DGT4_MEMCARD_RUNTIME_SOURCE=C:/Antigravity/gt4-staticrecomp/private/slice200-memcard-runtime ^
  -DGT4_SHADER_DIR=C:/Antigravity/gt4-staticrecomp/private/slice153-pacing-shaders
cmake --build O:\gt4recomp-scratch\slice335-rc7-controls-build\build
```

#### Spec II Candidate (`O:\gt4recomp-scratch\slice336-rc7-controls-spec2`)
```cmd
cmake -S . -B O:\gt4recomp-scratch\slice336-rc7-controls-spec2\build -G Ninja -DCMAKE_BUILD_TYPE=Release ^
  -DCMAKE_C_COMPILER="C:/Program Files/LLVM/bin/clang-cl.exe" ^
  -DCMAKE_CXX_COMPILER="C:/Program Files/LLVM/bin/clang-cl.exe" ^
  -DGT4_TARGET=spec2 -DGT4_EE_PROFILE=spec2 -DGT4_RELEASE_DISTRIBUTION=ON ^
  -DGT4_ASSETS_ROOT=C:/Antigravity/gt4-staticrecomp ^
  -DGT4_PYTHON=C:/Antigravity/gt4-staticrecomp/private/tooling-venv/Scripts/python.exe ^
  -DGT4_PERF_RUNTIME_SOURCE=C:/Antigravity/gt4-staticrecomp/private/slice276-spec2-iop-runtime ^
  -DGT4_PORT_DIR=C:/Antigravity/gt4-staticrecomp/private/slice200-port ^
  -DGT4_VU1_GENERATED_DIR=C:/Antigravity/gt4-staticrecomp/private/slice157-vu1-all-gen ^
  -DGT4_VU0_GENERATED_DIR=C:/Antigravity/gt4-staticrecomp/private/slice160-vu0-gen-r3 ^
  -DGT4_VU0_IMAGE_FILE=C:/Antigravity/gt4-staticrecomp/private/slice160-vu0-image/vu0_image.bin ^
  -DGT4_VU_CORPUS_DIR=C:/Antigravity/gt4-staticrecomp/private/slice155-vu1-full-corpus ^
  -DGT4_LICENSE_GUARD_SOURCE=C:/Antigravity/gt4-staticrecomp/private/slice277-license-guard-runtime ^
  -DGT4_SHADER_DIR=C:/Antigravity/gt4-staticrecomp/private/slice153-pacing-shaders
cmake --build O:\gt4recomp-scratch\slice336-rc7-controls-spec2\build
```

### 4.2 Exact Artifact Hashes & Sizes
All 12 primary binary artifacts were hashed using SHA256:

| Slice Target | Binary File | Size (Bytes) | SHA256 Hash |
| --- | --- | --- | --- |
| `slice335` (v200) | `gt4vulkan.exe` | 97,702,912 | `17406BA4BFCD3DED71E68427C45B3528B43C1CACCB3C4872FDA3DED5634AE917` |
| `slice335` (v200) | `controls_fixture.exe` | 165,376 | `2C44F2BB84ABFCC7879BF850CBF7D98BEB73D55D3532331EA9C52BC34DC84D56` |
| `slice335` (v200) | `host_io_fixture.exe` | 125,440 | `4F45914B34532011A03F0F3F02E3E93FC51D15156AD433D0018845DB962FFE99` |
| `slice335` (v200) | `achievements_fixture.exe` | 402,432 | `0C64875BDBD869F41C8ED224F66E5A37FEA2A3DA8A97F6CC9E31BC18D495FBFD` |
| `slice335` (v200) | `achievements_spec2_fixture.exe` | 104,448 | `2CD3C68927B26F7A22001E0BBD1BC257D51B8EC81782504F3400A62FC1EDDE16` |
| `slice335` (v200) | `achievements_http_fixture.exe` | 40,960 | `8D248B8B915E781FEEF88753404F8D8F89916C684C9FE39B132BF81766282ADF` |
| `slice336` (Spec II) | `gt4vulkan-spec2.exe` | 104,719,872 | `67951470D1FAF767C8D619BAC5483BBFD30F9B164E6AAEB04E7AD20858083F1A` |
| `slice336` (Spec II) | `controls_fixture.exe` | 165,376 | `A97EAE6B2A9E8E99E575CECA1C279501741B3F4690C059C9AEDD9F4517267720` |
| `slice336` (Spec II) | `host_io_fixture.exe` | 125,440 | `0CC9D314ABCA1C1B284E4A1D8EF3C2D0C406729781015A6B851B64D7A42B0820` |
| `slice336` (Spec II) | `achievements_fixture.exe` | 402,432 | `D48582C00A791EEE5152E1EBEB18B34BD44F98FB3F48449616B475F14FCEDF82` |
| `slice336` (Spec II) | `achievements_spec2_fixture.exe` | 104,448 | `FE9F83F705FB83E1B91A04201AF3BEE3235D1D12DFDE2499738021147AAC4003` |
| `slice336` (Spec II) | `achievements_http_fixture.exe` | 40,960 | `869CFF6E058912D21BE497BEF1B697D6600A91DF83301747B65E40560402FF74` |

*Note: Stage 2/3 slices `slice333-rc7-ra-build` and `slice334-rc7-ra-spec2` were untouched and remain intact.*

---

## 5. Comprehensive Test Execution Evidence

All test suites were executed serially.

### 5.1 Rust Workspace Test Suite
- **Command:** `cargo test --workspace` (with `CARGO_TARGET_DIR=O:\gt4recomp-scratch\task133-cargo-target` and `GT4_SDL3_ROOT=C:\Antigravity\gt4-staticrecomp\private\tooling\sdl3-3.4.18`)
- **Durable Log:** `docs/tasks/evidence/stage4-logs/cargo-test-workspace.log`
- **Result:** **34 test blocks, 578 passed, 0 failed, 5 ignored** (reconciled).
- **Delta from Stage 3:** +4 passed tests:
  1. `gt4controls::tests::new_curves_round_trip_and_unsupported_raw_modes_are_rejected`
  2. `gt4controls::tests::all_seven_curves_and_aliases_support_old_configs_and_monotonic_travel`
  3. `gt4l_platform::pad::tests::all_standardized_gamepads_keep_the_same_trigger_range_and_capture_threshold`
  4. `gt4l_tauri::model::tests::all_seven_curves_parse_and_serialize_in_tauri_model`
- **Preserved Behavior:** All channel-coordinated TCP race tests (`flow_audit::retroachievements_network_race_tests_with_real_tcp_server`) passed without race or deadlock.

### 5.2 UI Focus and Navigation Checks
- **Command:** `node docs/tasks/evidence/stage4-ui/run_ui_checks.js` (using Playwright Edge engine)
- **Durable Log:** `docs/tasks/evidence/stage4-logs/ui-checks.log`
- **Results:**
  - `dev/focus-check.html?lang=en-US`: **215 of 215 passed**, 0 JS errors.
  - `dev/focus-check.html?lang=pt-BR`: **222 of 222 passed**, 0 JS errors.
  - `dev/nav-test.html`: **154 of 154 passed** (+4 from Stage 3: verified quantized half-travel output for `s_curve`, `smooth_power`, `sublinear`, `logarithmic`), 0 failures, 0 JS errors.

### 5.3 Native CTest Suites
#### Targeted Controls & Audio Input Tests
- `slice335` (v200):
  - `Test #11: native_controls`: **Passed** (0.08s)
  - `Test #10: native_audio_input`: **Passed** (3.48s) — verified virtual PS5 and Xbox triggers through rest, 1/4, 1/2, 3/4, and full travel.
  - `Test #14: native_achievements`: **Passed** (0.07s)
  - `Test #15: native_achievements_spec2`: **Passed** (0.46s)
  - `Test #16: native_achievements_http`: **Passed** (0.07s)
- `slice336` (Spec II):
  - `Test #10: native_audio_input`: **Passed** (3.42s)
  - `Test #11: native_controls`: **Passed** (0.09s)
  - `Test #14: native_achievements`: **Passed** (0.07s)
  - `Test #15: native_achievements_spec2`: **Passed** (0.48s)
  - `Test #16: native_achievements_http`: **Passed** (0.08s)

#### Full CTest Suites (50 Tests Each)
- **v200 Candidate (`slice335`):**
  - **Durable Log:** `docs/tasks/evidence/stage4-logs/ctest-full-v200.log`
  - **Result:** **49/50 passed (98%)**, 1 failed.
  - `native_mtvu_ring` passed (30.12s), `native_mtvu_journal` passed (21.47s), `vu_recomp_corpus` passed.
  - Failing test: `Test #36: python_native_contracts` (Ran 710 tests, 13 failures, 42 skipped).
- **Spec II Candidate (`slice336`):**
  - **Durable Log:** `docs/tasks/evidence/stage4-logs/ctest-full-spec2.log`
  - **Result:** **49/50 passed (98%)**, 1 failed.
  - `native_mtvu_ring` passed (30.03s), `native_mtvu_journal` passed (21.53s), `vu_recomp_corpus` passed.
  - Failing test: `Test #36: python_native_contracts` (Ran 710 tests, 13 failures, 55 skipped).

### 5.4 Targeted Python Tests
- `tests/python/test_controls_probe.py`: **6 passed in 0.026s (OK)**.
- `tests/python/test_pacing_launcher.py`: **14 passed, 1 skipped in 0.029s (OK)**.

---

## 6. Accounting of Known Baseline Failures

The single failing CTest test (#36 `python_native_contracts`) in both `slice335` and `slice336` was inspected:
- It runs the Python discovery suite against the candidate executable.
- Exactly 13 tests failed out of 710, identically in both targets.
- These 13 failures represent the known, documented rc6 baseline tests (e.g. historical PCSX2 mode pins, unbuilt historical slice executables, and aspect-profile checks).
- No test assertions were weakened, and no pins were artificially updated to hide mismatches.

---

## 7. Limitations & Strict Boundaries

1. **Synthetic Device Testing Only:** All controller and trigger evaluations were conducted using mathematical models, test vectors, and synthetic SDL virtual joysticks (`SDL_AttachVirtualJoystick`). No physical DualSense, DualShock, or Xbox controllers were connected.
2. **No Subjective Gameplay Claims:** These tests prove mathematical parity, trigger threshold consistency, serialization round-tripping, and input mapping correctness. They do **not** establish subjective steering feel, car responsiveness, or in-game handling fidelity.
3. **Strict Stage Boundaries Maintained:**
   - No version numbers were bumped.
   - NSIS installer was not compiled.
   - Portable zip package was not assembled.
   - No pull requests, draft releases, or public releases were published.
   - Work is paused for Codex Orchestrator review.
