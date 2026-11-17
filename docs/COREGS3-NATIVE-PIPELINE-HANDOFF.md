# CoreGS 3.0 native Vulkan cutover: TASK-090 implementation handoff

Date: 2026-11-17. Review base: `task/090-coregs3` at `356a93c80bf0a3b16743633595f8c3a0cc204eaf`.

> Scope: static source review and implementation plan. No Vulkan build, GPU benchmark, headless replay or identity tests were run during this review. Do not treat proposed fixes below as implemented or validated.

## Decision

**Keep interception at the game's EE/PGL call, before VU1.** For actual native performance, the next milestones are (1) a correct conventional Vulkan color/depth path, (2) no runtime reliance on post-VU1 tap output for covered draws, and (3) measured suppression of the matching VU1/GS work. Showing native pixels alone is not enough.

## Confirmed state on the reviewed branch

| Area | Observation | Source |
| --- | --- | --- |
| Frontier | `coregs3_units.py` injects `before_shape_call()` ahead of `pgluCallShape` (`0x0049D308`, main caller `0x00455A5C`), plus direct shape sites. The captured PGL state/shape data are taken before VU1 processing. **Do not move this hook downstream.** | `scripts/coregs3_units.py`, `native/coregs3/coregs3_hooks.cpp` |
| Native geometry | `coregs3_vif.cpp`/`coregs3_shape.cpp` decode original VIF *asset data*, not VU1-program output. World/car Vulkan vertex shaders already perform model-to-clip transforms. | `native/coregs3/`, `native/hw_gs/shaders/coregs3_{world,car}.vert` |
| Post-VU1 dependencies | Live texture and state binding still invokes `live_world::build_tap_snapshot()`; emitted fog/state and some lighting are paired with worker results. `shade_mesh_items()` still performs some per-vertex work on CPU. | `native/hw_gs/coregs3/hw_coregs3_present.cpp`, `native/coregs3/coregs3_lighting.cpp` |
| Legacy work still executed | The MTVU runtime still emits PATH1 GIF packets. `GT4_COREGS3_REPLACE_3D=1` only hides CoreGS 2.0's 3D contribution at **presentation**; it does not suppress VU1/GS execution. | `native/mtvu_runtime.cpp`, `native/hw_gs/coregs3/hw_coregs3_present.cpp`, `docs/COREGS3-2D-SPLIT.md` §8.6 |
| Framebuffer | Current live passes use `R32_UINT` storage image writes and a native D32 depth attachment, with `colorAttachmentCount = 0`; car blend/DATE and FBMSK use shader reads/writes instead of conventional native color attachments. | `native/hw_gs/coregs3/hw_coregs3_present.cpp`, `native/hw_gs/shaders/coregs3_{car,world}.frag` |
| Public debug modes | `GT4_COREGS3=off|shadow|overlay|present`; `overlay` intentionally refuses initialization. Planned `replace`/`own3d` levels are not active switches. | `native/coregs3/coregs3_scene.hpp`, `coregs3_hooks.cpp` |
| Texture capacity | The captured A-Spec 3D-replace round documents 2,679 refused atlas placements in the fixed 2048x2048 atlas (not remeasured here). | `docs/COREGS3-2D-SPLIT.md` §8.6 |

## P0 — Fix the depth/alpha/color correctness contract

**Do not remove `layout(early_fragment_tests) in;` alone.** Both live car/world fragment shaders call `discard` for alpha tests; the car also performs DATE. Early tests can commit depth before a fragment is discarded. But simply removing the early-test qualifier does not fix this storage-image design: color `imageStore()` side effects can execute before a *late* depth rejection. The correct fix needs to change **color writes and depth behavior together**.

Similarly, `imageLoad` -> manual blend or FBMSK -> `imageStore` is not automatically ordered like native attachment blending when fragments overlap. Identify the required raster ordering and do not silently rely on undefined inter-fragment RMW results.

**Required invariants:**

1. A depth-rejected fragment never changes color or depth.
2. An alpha/DATE-rejected fragment does not leave a forbidden depth write.
3. Overlapping primitives obey deterministic blend, mask and draw-order semantics.
4. An unsupported state refuses explicitly and uses a faithful fallback instead of guessing.

**First isolated implementation experiment:** an opt-in native color-attachment renderer for a strictly classified opaque subset (normal RGBA/BGRA color attachment + D32 depth, standard raster/depth pipeline). Add alpha-tested cutouts and blend classes only after synthetic depth-overlap fixtures pass. Use fixed-function blending only for GS equations it actually represents. Keep GS-special DATE, partial FBMSK and custom blend cases on a proven ordered fallback until separately implemented; availability of fragment-interlock or attachment-order extensions on Android must be measured, not assumed. Leave the current `R32UI` live pass as an A/B baseline.

**P0 tests:** front cutout over a rear opaque triangle, reversed draw order, wheel/body overlap, car glass, reflection DATE, a framebuffer alpha-only write and the night B-Spec HUD. The reference image and depth outcome must both be checked. Document any feature requiring fallback.

## P1 — Eliminate runtime dependence on the VU1 tap for supported draws

Capture and derive model positions, matrices, material, lighting, UV/texgen, fog and texture identity from the EE/PGL asset and scene records. Keep the VU1/GIF tap as an **oracle in validation builds**, not as the source of production fragments. Migrate host-side lighting to the GPU when practical while retaining CPU reference fixtures. Ensure the morph/skin, STQ, CLUT and mip families are covered with per-family vertex/pixel comparisons.

Add a tap-independent gate: when the tap is disabled, known supported draw families still render the same image; unknown families or insufficient state are counted and refused. Do not turn off the worker until this passes. Make texture uploads order-correct from the source assets and dynamic upload records rather than relying on a later frame-end GS memory picture.

## P2 — Actually skip covered VU1/GS drawing

Only after P0/P1 gates pass, choose a single supported program family and scene. Suppress the **covered rendering work**, not guest-observable state. The VIF/MTVU path must still preserve timing, DMA and job order, VIF state, subsequent VU register/memory dependencies, required SIGNAL/FINISH/LABEL/synchronization and any guest-visible GS readbacks. Keep uncategorized programs, effects, HUD, menus and readback-dependent operations on CoreGS 2.0.

Implement an explicit allowlist, reversible per-category cutover, and detailed counters: selected/covered draws, skipped VU1 programs, suppressed PATH1 GS drawing, fallback counts and saved worker/GS time. `GT4_COREGS3_REPLACE_3D` itself is **not** proof of this gate. Compare all non-skipped PATH1 output and guest digest, clock and state against the faithful run.

## P3 — Performance resources and mobile

- Eliminate fixed-atlas overflow with budgeted persistent mesh/texture caches keyed by source content and palette/mip version, plus dynamic uploads, eviction, LRU counters and bounded per-frame staging. Avoid decoding and re-uploading static track assets every frame.
- Measure EE hook work, VIF/MTVU, legacy CoreGS 2.0, native CPU decode/lighting, upload bandwidth, native Vulkan GPU and final frame time separately. Compare identical scenes/scales with old raster truly skipped, not merely hidden.
- On Adreno 740/Android, prioritize conventional tile-friendly color/depth attachments and test driver feature support. Do not infer mobile FPS from desktop results. Preserve exact game logic.

## Agent work order and acceptance

| Order | Deliverable | Gate |
| --- | --- | --- |
| 1 | Baseline report for A-Spec and B-Spec with old and native work counted separately | Reproducible per-subsystem times; current reference captured |
| 2 | Isolated opt-in opaque native-attachment backend, separate from existing `present` path | Synthetic overlapping-depth + color fixtures pass; existing modes unchanged |
| 3 | Alpha/DATE/blend/FBMSK classification, with explicit fallback | No incorrect depth writes or unordered storage-image RMW for migrated draws |
| 4 | Source-derived GPU vertex/material path with optional tap-oracle checks | Supported drawings match with runtime tap disabled |
| 5 | One-family real VU1/PATH1/GS skip prototype | Same guest state + non-skipped PATH1; actual worker/GS time reduction |
| 6 | Expand world/cars/effects, persistent caches and Android tests | Per-scene image and simulation gates pass, measurable end-to-end gain |

Do not combine the experimental native-attachment implementation and the VU1-skip cutover in one unmeasured change. Make each candidate reversible and attributable. Leave the faithful CoreGS 2.0 and `off`/`shadow` modes intact.

### Regression requirements

- Re-run the repo's CTest/Python suites and `scripts/coregs3_bench/coregs3-night-check.sh` on the owner's actual build system. The last recorded §38.8 run (2026-11-11) reports CoreGS3 CTest 21/21, Python 18/18, and an 8,997-VBlank identity gate with zero differences; these are **historical reports, not runs by this review**.
- Use frame-aligned images and per-material/draw masks for night B-Spec, day A-Spec, car glass, reflection, wet-road effects, sky, 2D/HUD, replay, photo readbacks and alternate aspect ratios. Test actual owner hardware before accepting a perf claim.
- Check identity with `GT4_COREGS3=off` and `shadow`; compare experimental `present`, `GT4_COREGS3_REPLACE_3D=0|1` and `GT4_COREGS3_2D_OVERLAY=0|1` as appropriate. Note that `overlay` currently refuses start.
- Stop on any guest state drift, unsound VU1 skip, unsynchronized framebuffer RMW, unmeasured missing material or mismatched scene/frame phase. Do not alter gameplay, timing, saves or required readback semantics.
- Follow `AGENTS.md` and decision 0070: clean-room, MIT-only code in the executable, one writer per branch/worktree, evidence before merge, owner approval before merging to `main`.

## One-sentence handoff

**The EE hooks are already before VU1. Keep them; build correct native attachments, remove post-VU1 runtime dependencies, then skip redundant VU1/GS rendering with identity and performance evidence.**
