# CoreGS 2.0 (core 2) performance ledger

One table of every phase B optimisation step: what was tried, what it measured, and whether it stayed. The details
are in `IMPLEMENTATION-NOTES.md` (one row per IN number) and the journals of 2026-10-28 and 2026-10-29. Update this
file with every step, including the ones that are reverted.

## How to measure

- **Scene:** the headless night race (Hong Kong Reverse, B-Spec, VW Lupo, 5 AI cars, 3x). The presses are in the
  journal of 2026-10-28; local scripts `night-run.sh` and `title-run2.sh`.
  - Short race: VBlanks 6500-9100. Long race: VBlanks 9000-14000 (`FRAMES=14000 EVERY=1200 FROM=9000`).
  - Some pixels vary from run to run in every build:
    - frame 6600 (the first race camera);
    - 0-2 pixels at frames 7200 and 9000;
    - the box (1204-1359, 568-621) at frame 12000.
  - Compare frames against two runs of the same build before calling a difference real.
- **Title check:** the GT Mode visit then the title (the IN-411 bug scene), compared byte for byte with candidate 287
  at VBlanks 4200, 4300 and 4400.
- **Timing lines,** every 600 VBlanks:
  - `[core2] ms/vblank`:
    - CPU stages: gif, batch, upload, translate, targets, texture, draw, readback, glm, swizzle;
    - GPU: `gpu_wait` (fence waits), `gpu_busy` (GPU time from timestamps), `gpu_draw_passes`, and the pipeline
      statistics `prims`, `fragments` and `compute`.
  - `[core2] gpu_wait ms/vblank by readback reason`.
  - The `[core2] vblank=` counters: barriers, render passes, flushes, recycled images and more.
- **Owner tests:** F7/F8 in a 1080p window at 3x on the night race; the report is in the session log (UTF-16).
- **Rule:** one change per candidate, with an A/B switch, frames checked, and CTest in both interlock modes.

## Ledger

Numbers are per VBlank on the night race at 3x unless stated. "GS thread" is the `gif` field. "Emulator" is the
wall time of the whole emulator.

| Step | Change | Measured effect | Kept? | Candidate |
|---|---|---|---|---|
| IN-406 | Present halving stops while the halved level still covers the window | Sharpness of GSdx at 3x in a 1080p window (owner test 13b) | yes | 287 |
| IN-407 | Small untextured sprites drawn by the CPU into memory | Part of B1, below | yes | 288 |
| IN-408 | CLUT loads and textures over a drawn CT32 target read on the GPU (GLM), no readback | Alone: slower, because GPU-palette textures were not cacheable | yes, with IN-409/410 | 288 |
| IN-409 | Indexed textures keep their indices; palette rows looked up at draw time | Made IN-408 a gain | yes | 288 |
| IN-410 | Submit every 96 batches | Readback waits fall, because the GPU is busy earlier | yes | 288 |
| B1 total | IN-407..410 | Readbacks 6.9-8.7 -> 0.8-1.6 ms; emulator 20.8-26.4 -> 17.0-20.6 ms | yes | 288 |
| IN-411 | GPU reads are not ageing uses; GPU-read targets stored before a drop or a row patch | Fixed the owner's title-backdrop bug; owner F8 57.0 VBlanks/s | yes | 289 |
| IN-412 | Repeated identical uploads dropped; block versions instead of page versions | 5-19 uploads dropped, decodes unchanged | **reverted** | - |
| IN-413 | Texture cache keyed by content (block tags) | Decodes 200 -> 15, but the emulator went 17-20 -> 26-30 ms (hashing about 2 MB per VBlank) | **reverted** | - |
| B2 lesson | | A decode costs about 10 us of CPU; the texture cache was not the bottleneck | | |
| measure | GLM page copies against the CPU upload swizzle | GLM copies 0.3-0.5 ms; swizzle 2.3-2.8 ms. The planned "uploads straight into the GLM on the GPU" would have removed little | | |
| IN-414 | Upload swizzle in page-row runs (in-page byte tables) | Swizzle 2.3-2.8 -> 0.9 ms; the new fixture found and fixed wrong T8H/T4HL/T4HH uploads | yes | 290 |
| IN-415 | No IN-411 store before a write that replaces whole pages under the target | Store waits 1.7-2.2 -> 0.2-0.3 ms; GS thread 16-22 -> 14-19 ms; owner F8 59.17 VBlanks/s, p99 19.6 ms | yes | 291 |
| IN-416 | Dirty rectangles exact page by page and block by block for writes in the same layout | Store waits about 0 on the long race; GS thread 9.0-15.7 -> 8.3-13.9 ms | yes | 292 |
| IN-417 | CLUT loads over several or depth targets, and textures over depth targets, via the GLM | CLUT readbacks about 1 -> 0; GS thread 8.3-13.9 -> 7.2-11.1 ms; owner F8 59.33, EE critical path 14.0 ms | yes | 293 |
| IN-418 | No allocation per batch or CLUT load; finished images reused by size | GS thread 7.2-11.1 -> 5.5-8.9 ms; owner F8 59.14 (now GPU-limited in the window) | yes | 294 |
| IN-419 | GPU timestamps; barriers only on real dependencies | Barriers halved; GPU time only 3-13% less | yes | 295 |
| IN-420 | Draw batches deferred into one render pass | Render passes 106-312 -> 4-8; GPU time 5-13% less | yes | 296 |
| IN-421 | D1 measured first: shader experiments, pipeline statistics | The whole fragment stage is 0.4-0.7 ms of GPU time; 53-65 M fragments; D1 not the next lever on the desktop | diagnostics only | - |
| IN-422 | Textures over no target decoded from a CPU page window (no GLM upload or barrier) | Barriers 254-961 -> 137-843; GPU time 3-10% less; emulator 0.1-0.3 ms less | yes | 297 |
| IN-423 | Per-VBlank spike log (`GT4_CORE2_SPIKE_MS`) | Found the race-start stutters: 40-160 new images per VBlank, each a device allocation | diagnostics | 298 |
| IN-424 | Image memory taken from 64 MiB blocks | Race-start VBlanks over 20 ms: 60 -> 2; averages unchanged; image memory +90 MB | yes | 298 |
| IN-425 | GPU cost map (`GT4_CORE2_GPU_PROFILE_AT`) | Headlight scene 5.9 ms of isolated GPU work against 7-19 ms per VBlank: gaps, not power | diagnostics | 299 |
| measure | Submit every 32 or 16 batches with two slots | Flare wait down, slot waits and spikes up (47 -> 66 -> 94) | not alone | - |
| IN-426 | Up to 8 submissions in flight, submit every 32 batches | Arcade race VBlanks over 17 ms 59 -> 15; GPU waits about a third; emulator 8.9-13.1 -> 7.6-10.5 ms | yes | 299 |
| IN-427 | 1x fix: texture from a target marked in the push word | 1x shows the 3D scene (was HUD only); 3x unchanged | yes | 300 |
| IN-428 | C1 mipmapping and LOD (all levels in one image, LOD in the pixel shader) | fidelity feature; distant asphalt less noisy; no visible cost on the night race | yes | 301 |
| IN-429 | readbacks of one transfer in one submission | camera cut readback 16.6 -> 14.4 ms | yes | 302 |
| IN-430 | dirty rectangles merged into the last pending one | camera cut dirty patches 863 -> 387, readback 13.3 ms, wall 46 -> 40 ms | yes | 302 |
| IN-431 | texture cache budget 192 MiB on integrated GPUs (768 discrete) | at 192 MiB: images 621 -> 507 MB, texture +0.16 ms | yes | 303 |
| IN-432 | present resampling (Lanczos-2 down, FSR1 EASU up, optional RCAS) | quality feature; three small compute passes at the window size | yes | 303 |
| IN-433 | forced bilinear option (forced, forced_no_sprite) | option, off by default | yes | 304 |
| IN-434 | anisotropic filtering of mipmapped draws (GT4_CORE2_ANISO) | option, off by default; GPU time unchanged within noise at 16x | yes | 304 |
| IN-435 | display reads in bands (12x dumps) | 12x: 1.6 GB of images, GPU 27 ms per VBlank | yes | 305 |
| IN-436 | scale sweep 1x-12x and the 12x cost found | GPU time per VBlank grows with the fragment count (6 M at 1x, 853 M at 12x, about 27 G fragments/s): 2.8 / 4.0 / 5.6 / 7.4 / 8.6 / 10.3 / 15.9 / 23.8 / 28.4 / 33.3 ms at 1/2/3/4/5/6/8/10/11/12x. Not bandwidth (skipping unneeded frame/depth reads: no change), not the interlock (removed: no change), not clocks (3.3 GHz, 98% busy, 85 W of 180 W). A fill-rate benchmark at 7680x5376: the uber shader 1.26 ms per full-screen pass, the same shader with the draw state folded to constants 0.41 ms, a minimal interlocked read-modify-write 0.58 ms, an RGBA8 attachment 0.21 ms (0.42 blended). The uber shader re-decides about 50 draw-state branches in every wave (`hw_process_pixel`: about 137 cycles per wave) | diagnostics only (read skipping reverted) | - |
| IN-437 | pipelines specialised per draw state (specialization constants, worker-thread compile, uber pipeline meanwhile) | Night race GPU busy per VBlank, 305 -> 306: 1x 2.8 -> 2.1-2.3, 3x 5.6 -> 4.2-4.5, 4x 7.4 -> 5.8, 5x 8.6 -> 7.2, 6x 10.3 -> 8.8, 8x 15.9 -> 11.2, 10x 23.8 -> 15.5, 11x 28.4 -> 18.3, 12x 33.3 -> 21.4 (max 42.4 -> 27.2); about 97 variants, 128 ms each on average (max 250 ms) on the worker; CPU wall within noise (a video playing during part of the sweep slowed every CPU stage alike) | yes | 306 |
| IN-438 | frame and depth words read only when the outcome depends on them; what limits 12x now | 12x GPU busy 21.3 -> 21.1 ms. Owner VRAM test 2500 -> 2800 MHz (+12%): 21.3 -> 21.1 (306), 21.1 -> 20.9 (438), so not memory bound; without interlock 20.6 (throw-away); all textures nearest 19.6 (throw-away, bound of a hardware-filter relaxation). Fill benchmark with incompressible colour: RGBA8 attachment 0.40 ms per 12x pass, the same as the specialised storage path (0.41) and the floor of a shader that may write an image (0.40): the earlier 0.21 ms was colour compression of a constant. A ROP path would not speed up this GPU | yes (exact; less memory traffic for bandwidth-limited GPUs such as the laptop's) | - |
| IN-439 | performance mode (decision 0073), first item: delayed downloads | GT4 downloads one tiny row per VBlank in a race (light flare visibility test, same buffer for 10-25 VBlanks). Answered with the previous same download's words, reduced on the GPU without a wait: 10x night race synchronous downloads 0.66 -> 0.24 per VBlank, download waits 3.5 -> 2.0 ms, wall 19.8 -> 19.2 ms (the GPU stays the limit at 10x: 16.2 ms busy). Frames at 10x differ from faithful mode by 2-54 pixels of 28.7 M (below the run-to-run variation) | yes, performance mode only (GT4_CORE2_PERFORMANCE=1 or GT4_CORE2_DELAYED_DOWNLOADS=1) | 307 |
| IN-440 | real cost of the GS memory upkeep at 12x (throw-away: no GLM writes) | 12x GPU busy 21.4 -> 20.8 ms with every GLM write (target materialisation, about 390 per race VBlank) removed: the isolated cost map (1.9 ms) overstates small commands, which overlap in a real run. Batching them would save a fraction of 0.6 ms | no (measurement only) | - |
| IN-441 | performance mode item 2: flares hidden (the flare visibility table answered as zeros, as rc7's TASK-117 does for the PCSX2 GS) | Clubman Stage Route 5 night (Arcade, B-Spec) at 10x: wall 18.1 -> 16.9 ms (max 19.9 -> 19.0), GPU 15.5 -> 15.4 ms, fragments 590 -> 587 M; 3676 tables hidden; no download left to delay. Hong Kong dusk (few lights) 3x/4x/6x/12x: within 0.1-0.6 ms. The light flares and the sun glare are gone; the rest of the frame is unchanged | yes, performance mode only (GT4_GS_HIDE_FLARES overrides) | 308 |
| IN-442 | constant fills as compute dispatches (core2_fill.comp) | Throw-away bounds at 12x: skipping the large untextured no-blend sprites 21.6 -> 18.1 ms (248 M of 853 M fragments: GT4's full-screen colour, depth and alpha clears), skipping large 1:1 copies 21.6 -> 20.4. Fill benchmark at 7680x5376: compute store 0.20 ms per pass, compute masked load+store 0.25, vkCmdClearColorImage 0.09, against 0.41-0.55 for the fragment stage. Implemented: GPU busy 3x 4.3 -> 4.1, 4x 5.4 -> 5.2, 6x 8.0 -> 7.9, 12x 21.6 -> 20.6 ms (fragments 853 -> 563 M); frames identical | yes (exact; GT4_CORE2_COMPUTE_FILLS=0 A/B) | 309 |
| owner | Test of 299 (Arcade night race, B-Spec, 3x) | 60 FPS, frametime nearly flat; GPU 2113 MHz, 66%; fidelity perfect. Phase B closed for the beta | - | 299 |

## What we learned

- **Measure before building.** The plan was wrong three times, and each time a measurement caught it:
  - the content-keyed texture cache (IN-413);
  - "the double copy is the cost" (it was the swizzle, IN-414);
  - D1 as the next GPU lever (IN-421).
- **Stores and readbacks:**
  - GPU waits came mostly from IN-411's safety stores, not from the flare download.
  - GT4 streams textures over the memory of its night render every frame. Whole-page writes (IN-415) and page-exact
    dirty rectangles (IN-416) removed the stores without changing a pixel.
- **CPU housekeeping:** allocation churn and device-memory churn cost the GS thread about 2 ms per VBlank (IN-418).
  Profile with the all-thread sampler (`build/prof/sampler2.cpp`, git-ignored) on a RelWithDebInfo build.
- **The GPU side:**
  - The owner sees the GPU at 45 W of 180 W: it waits more than it works.
  - The fixed GPU time is the chain of about 1000 small dependent commands per VBlank (texture decodes, GLM uploads,
    patches, CLUT loads); halving barriers and render passes gave only about 10%.
  - The fragment shader is cheap here. D1 remains required for the HD 620 beta gate, where shading is far slower.
- **GPU changes give far less than expected.** Removing half the barriers (IN-419), 97% of the render passes
  (IN-420) and more barriers (IN-422) each gave 3-13% of GPU time.
  - **Confirmed by the owner (297):** the RX 9060 XT runs at about 1650 of 3300 MHz, 47-50% use, 29 W; VRAM at about
    1350 of 2500 MHz; the same with GSdx.
  - The driver keeps the GPU at a low clock because it is idle, so GPU timestamps measure the clock more than work.
  - On this desktop the GPU is not the limit. The frame is the EE critical path (about 14 ms), of which the renderer
    owns about 2.5 ms (the EE waiting for the flare downloads).
- **Stutters are spikes, not averages.** The 600-VBlank averages hid them. Look per VBlank with the spike log. The
  race-start stutters were device memory allocations (IN-424), not readbacks.
- **Two submissions in flight were the structural limit:** the CPU waited for a GPU that ran at a low clock and was
  full of gaps (IN-426).
- **Split rings into aligned shares:** a non-power-of-two slot count broke buffer offsets. Only the frame checks
  caught it, and the bad run looked faster.
- **Timestamps:** with deferred draws, compute recorded before a pass overlaps the pass's timestamps, so
  `gpu_draw_passes` overstates drawing.
