# CoreGS 2.0 (CORE2): roadmap after owner test 12 (2026-10-28)

Written by the TASK-027 implementer (clean room). It replaces the open-items part of `PARITY-PLAN.md` for core 2;
`PARITY-PLAN.md` stays the record of the old core.

## 1. Where we are

- **Owner test 12 (candidate 285, 1920 x 1080 window, 3x, 16:9): passed for picture faults.** "Zero bugs visuais
  agora nesse sentido": the ring notches, horizon stripes, jagged edges and random shadows are gone (IN-403).
- **Owner findings in the same test:**
  - stutter and frame-time spikes at the start of a race;
  - drops below 60 FPS in the middle of a race;
  - the GPU nearly idle (about 40 W at 3x);
  - the picture is clearly upscaled but not "crisp": no anti-aliasing;
  - 480i menus are not deinterlaced in core 2.

## 2. Measured state

Headless El Capitan race (`race-run.sh`, VBlanks 9600-10200, 3x) and the 20 s race capture. Stage timers come from
the `[core2] ms/vblank` line. Labels as in AGENTS.md.

### CPU (Confirmed)

The GS thread costs more than one frame, so the GPU waits on it. The whole emulator takes 21.6 ms per VBlank and
the GS work 20.2 ms, against 16.7 ms for 60 FPS.

| Stage | ms per VBlank | Notes |
|---|---|---|
| Front end (`batch`: GIF parse, vertex queue, `ShouldDraw`, batching) | 9.1 | ~2.8 MB of GIF per frame, about 19 ns per qword |
| Translate (vertices, pixel state, scissor) | 2.9 | ~425 draw batches per frame |
| Texture lookup and decode | 2.5 | ~240 GPU decodes and ~108 cache hits per frame |
| EE uploads | 2.5 | ~170 uploads per frame |
| Target lookup, creation, dirty rectangles | 1.9 | ~3 targets created and ~3 replaced per frame |
| Draw recording | 1.2 | |
| CLUT loads | 0.9 | ~2.8 per frame |
| Readbacks (synchronous) | 0.6 | |

- **Loading and menu scenes:**
  - In the interval with CLUT readbacks (VBlanks 7200-7800), CLUT loads take 7.6 ms and readbacks 7.55 ms.
  - Each readback submits and waits, so the CPU and the GPU stop overlapping.
- **The old core** took 15-16 ms for the whole emulator on the same race.

### GPU (Confirmed in code, cost Hypothesis)

- **One uber pipeline for every draw:** storage images with an ordered pixel interlock, and no hardware blending or
  depth test.
  - Cheap at 3x today, because the GPU waits on the CPU.
  - At 6x-12x, the interlock and per-pixel read-modify-write grow with the pixel count.
  - Mobile GPUs often lack the interlock.
- **Every image has its own `vkAllocateMemory`:** each target and each pooled texture, zero-cleared at creation.
  - A race start creates dozens of targets and textures in a few frames.
  - That burst is the prime suspect for the start-of-race spikes (Hypothesis: the timing per frame is not
    measured yet).

### Picture (Confirmed in code)

- **Deinterlace:** not wired in core 2 (`core2_renderer.cpp`, "no deinterlace yet"). The adaptive output-row rule
  (IN-386, IN-396) exists in the shared display code and the old core's GPU kernel.
- **FXAA:** off in core 2 (`out.fxaa = false`). The kernel exists in the old core (IN-376).
- **Mipmapping:** not implemented. `GT4_HW_MIPMAP` is parsed but every texture samples level 0.
  - The reference samples mip levels when the game sets MXL > 0 (Q-GSDX-COMPARISON 14).
  - Distant textures therefore alias and shimmer.
- **Sharpness check against gt4-gs at 3x (Hypothesis):**
  - Measured on the race capture: our frames hold 2.4x more spectral power above the native Nyquist than gt4-gs's
    (0.0365 against 0.0153, mean of six frames).
  - The frames are not exactly matched, and gt4-gs's dump is resampled to 480 rows.
  - The reading: the softness the owner sees is not missing resolution. It is aliasing (no mipmaps, no AA) and the
    present resampling.
- **Present:** box halvings then bilinear (IN-390). At 3x in a 1080p window, it reduces 1344 rows to 1080 and keeps
  1920 columns 1:1. There is no supersampling horizontally, so edges stay aliased.

### Quality options

- **The launcher's GSdx rows that players use** (`docs/launcher/gs-settings.toml`):
  - basic page: FXAA, CAS mode and sharpness;
  - advanced page: filter, trilinear, anisotropic, mipmap, dithering, TV shader, shade boost, integer scaling,
    linear present.
- **Core 2 has none of them yet.**

## 3. Test method

- **A fixed scene: the owner's saved race replay.**
  - The owner saves a race replay on a card; the agent works on a copy.
  - The same frames every run, with several B-Spec-style cameras.
- **Three uses of the replay:**
  - Headless game runs drive the menus to the replay with `--press`, then dump frames.
  - A `.gscap` capture of the replay gives the GS-only cost through `hw_writeback_replay`, with no EE noise.
  - gt4-gs replays the same capture for reference frames at the same scale (final images only).
- **Per-frame timing:** each frame's GS time and its stage split, plus max and p99, so a spike can be attributed to a
  stage and a frame. The game's F7/F8 report gives the same split in owner runs.
- **Picture metrics:** per frame on matched frames:
  - edge energy;
  - power above the native Nyquist;
  - a shimmer measure (frame-to-frame difference on static camera shots).

## 4. Plan

One writer at a time. Every step is one candidate, with CTest and the race480p memory gate, and an owner test at the
end of each phase.

### Phase A: quick picture fixes (done, IN-405, candidate 286)

- **A1. Deinterlace in core 2.**
  - The FFMD field offset is already applied. Run the adaptive output-row deinterlace (IN-386, IN-396) on the
    composed image, reusing the shared kernel and its history images.
  - Acceptance: the GT Mode ticker and the Info dialog are clean at 3x, as with the old core (IN-396 evidence).
- **A2. FXAA in core 2.** Port the IN-376 pass to the core 2 present, behind the same option.

### Phase B: CPU performance (the GPU waits on the CPU)

Goal: GS thread at or under 8 ms per VBlank in the race at 3x, and no frame over 16.7 ms at a race start.

- **B1. Measurement first:**
  - the per-frame timeline in the replay tool and the game (F8);
  - the owner's replay as the scene.
- **B2. Front-end fast path** (9.1 ms):
  - a tight PACKED/REGLIST loop for the common register sets (ST, RGBAQ, XYZ2, XYZF2);
  - no per-vertex allocation or virtual call;
  - the bounding box and `ShouldDraw` computed once per primitive from integers.
  - Profile before and after.
- **B3. Translate, targets, textures** (~7 ms):
  - cache the translated pixel state per register version;
  - reuse vertex arrays;
  - find why ~240 textures per frame are decoded again (page versions bumped by unrelated uploads? palette hash
    misses?) and key the cache to what really changed.
- **B4. Race-start spikes:**
  - sub-allocate images from large device-memory blocks;
  - clear with a GPU command instead of on creation;
  - pre-size the texture pool;
  - check that nothing compiles at run time (the draw pipeline is built at start-up; confirm with the timeline).
- **B5. Readbacks without stalls:**
  - CLUT loads from a target are resolved on the GPU (palette read from the target image) instead of by a CPU
    readback;
  - the remaining readbacks are fenced per region instead of waiting for the whole queue.

### Phase C: picture quality

- **C1. Mipmapping and LOD** (fidelity, not an enhancement): MIPTBP1/2, the K/L LOD rule, MMIN/MMAG, MXL. Decode
  levels from GS memory like level 0, with trilinear when the guest asks for it.
  - Expected: less shimmer on the track and trees, the map's thin white lines gone (IN-388 analysis).
- **C2. Present resampling:** a sharper downscale (Catmull-Rom or Lanczos-2) for internal sizes above the window,
  bilinear only for small factors.
- **C3. Sharpening:** contrast-adaptive sharpening as an option, like the launcher's CAS row. Write it from AMD's
  published MIT FidelityFX CAS description or source, never from a GPL copy.
- **C4. Supersampling as the real AA:** once phases B and D make 4x-6x cheap, recommend an internal scale above the
  window. At 1080p, 6x is a 2x2 supersample.
- **C5. Texture filtering options:** forced bilinear and anisotropic filtering (owner choices, off by default).

### Phase D: GPU fast path (6x-12x, power parity, mobile)

- **D1. Hardware raster path:**
  - draws whose state needs no feedback render to real attachments, with fixed-function blending and depth;
  - the feedback rules are DATE, PABE, AFAIL other than KEEP, COLCLAMP 0 and partial FBMSK (Q-GSDX-COMPARISON);
  - the colour image gets an RGBA8 view of the same memory (mutable format, r32ui and rgba8 are compatible);
  - depth gets a D32F attachment, converted where the game reads Z as a texture or readback.
  - The interlocked uber shader stays for the rest.
- **D2. Measure against gt4-gs on the same capture and scale.** Per-frame GPU time, and the owner's power reading
  live.

### Phase E: completeness before the beta

- Core 2 at 1x (today only the HUD shows); 480i races without the forced 480p.
- Spec step 3:
  - T8 from a target (3.5) and the 16-from-32 shuffle (3.4);
  - depth as a texture (2.7, 3.6);
  - GPU local-to-local moves (4.3);
  - the alpha-only tone variant (7.2).
- Fractional scales and 12x (owner requirement): the launcher's percentage scale maps to a fractional internal scale.
- GSdx-compatible texture replacement (hash and naming in prose from the comparison team).
- A memory budget for targets and textures.
- Launcher entry as a beta renderer (needs upscaling and AA), the GSdx fallback kept.
- Later: the frame-skip study (TASK-082 branch) for CoreGS.

## 5. Order and owner tests

| Order | Step | Owner test |
|---|---|---|
| 1 | A1 deinterlace, A2 FXAA | 13: menus (480i) and FXAA on/off |
| 2 | B1-B5 CPU performance | 14: race start and mid-race frame times, GPU power |
| 3 | C1-C3 picture | 15: shimmer, sharpness, CAS |
| 4 | D1-D2 GPU fast path | 16: 6x and above, power against GSdx |
| 5 | Phase E | beta candidate |

The owner's saved replay is needed from step 2 on.
