# Slice 88: live no-card session — the semaphore id-space collision found (root cause of the park)

Date: 2026-10-09 (session started late 2026-10-08). Baseline: `main` =
`3ddc495` (docs refresh), clean tree, **no model code touched this slice**.
Task (owner-ordered): with the memory card removed (the model has no card
services), capture staged savestates of the real boot, compare the job-system
sequence against the model, and locate the first divergence. No model change
ships here; the fix is the next slice.

## Run identity

- Model compatibility: unchanged (time=3 interrupt=3 rpc=1 translation=2).
- Reference: bundled PCSX2 under `private/pcsx2/pcsx2-v2.9.94` is actually
  **v2.9.114** (exe metadata 2.9.114.0 and the emulog banner; the folder name
  is stale — docs corrected). The **active** config is
  `%USERPROFILE%\Documents\PCSX2\inis\PCSX2.ini` (not the bundled copy under
  `private/pcsx2/pcsx2-config/`, which still points at the original machine's
  `F:\Games\PS2\BIOS`). The active config already points at the repo BIOS
  (`private/pcsx2/bios`, v02.30 / SCPH-90001 v18), PINE 28011 enabled,
  fast boot on, software renderer (owner-set, `Renderer = 13`), patches on
  (the bundled pnach has a disabled widescreen section and an applied
  trigger-sensitivity section; the text-window hash check below bounds the
  interference).
- Memory card: slots 1 and 2 disabled in the active ini (owner removed the
  card with the save; both card images were scanned — no `97328` save entry).
  The model has no card services, so the no-card run is the correct analog.
- Savestate format: version `0x9a590000`, the same value the bundled
  emulator reports at startup — the v2.9.93-state skew concern is resolved by
  the version match (a live load test was not needed).

## Method (three live runs)

- **Run 1 (with card, 4 s cadence)**: the boot stops at the
  "No GT4 game data found on memory card … create game data?" dialog; the
  anchors were already in their final state at the dialog. One state kept as
  evidence (`with-card-save-dialog.p2s`).
- **Run 2 (no card, 4 s cadence)**: no save dialog; a "no memory card" notice,
  then the intro movie, then the **main menu with no input** (owner-confirmed;
  Gran Turismo Mode greyed out). Collected the menu/loop states.
- **Run 3 (no card, 1 s cadence, a savestate saved on every anchor change)**:
  captured the whole early sequence.

Anchors read every sample: A `0x0064C3C8`, B `0x0086CB80` (+0x34/+0x38/
+0x3C/+0x40/+0x80s), C `0x0086CC80`, gate `0x00616F24`, flag `0x0088D7C8`,
generation table `0x00874550` (42 words). Method: PINE reads + PCSX2
save-state requests, copies kept in the approved host temp dir; key states
archived under ignored `private/pcsx2/sstates/slice88-live-no-card/`.

## Reference timeline (Confirmed, dense run)

| Time | A | B+0x00 | B+0x34 | C+0x00 | Generation table |
|---|---|---|---|---|---|
| t=2 s | 0 | 0 | 0 | 0 | empty (BSS clear phase) |
| t=6 s | -1 | 0 | 0 | 0 | empty (table init writes -1s) |
| t=8 s | **0x11E** (raw 30, gen 1) | **0x10D** (raw 13) | 0 | **0x10F** (raw 15) | 6–16, 26, 29, 30 |
| t=10 s | 0x11E | 0x10D | **1** | 0x10F | +31–37; four live args 0x1BEF0/0x59440/0x90EA80/1 |
| t=14 s…359 s | stable | stable | 1 | stable | +39 |

Facts: the three handles are **distinct from their first appearance**; no
A=B state is observed; the submission (+0x34=1) follows the stamps; the state
is reached ~20 s after game start and persists through the movie/menu loop.
The old slot-9 "menu" anchors are the same values — the recreation the old
slices inferred happens **before the save/menu screens**, within the first
seconds of the boot.

## Model sequence (Confirmed by dumps/sweeps)

- service 100: A=-1, B=0, C=0, generation table empty.
- service ~125–145: a burst of 11 `CreateSema` calls composes generation
  entries 35, 39, …, 75; B=0x13F (raw 63, gen 1), C=0x147 (raw 71, gen 1).
- service 1910→1915: **A flips -1 → 0x13F — the same handle as B** — and the
  generation table gains no index (63 stays 1).
- Bisect with `--steps` (280 591…280 733): the store is
  **0x00548520 `sw v0, 0x0(s0)`** (init 0x00548500, sole A-stamper), with the
  create wrapper 0x005782E8 composing (pc 0x00578374 at step 280 715).
- Dump of `0x00874A4C` = generation table base + index **319**×4: 0 at
  services 1900/1913, **1** at 1920/2000 — the wrapper wrote generation 1 at
  index 319, **outside the game's 256-entry table**.

## Root cause (High confidence)

The game's create wrapper (0x005782E8) composes
`handle = (generation << 8) | raw_id` using the 256-entry generation table at
0x00874550 indexed by the raw id, and the resolve wrapper (0x00578290)
decodes `id = handle & 0xFF`. The raw id space is therefore **8-bit**.

The model's kernel allocates raw ids **3, 7, 11, …** (start 3, step 4,
monotonic, never reused). The 80th allocation is `3 + 4×79 = 319 = 0x13F`.
At service ~1913 the init's create was the 80th allocation, so:

- composed handle `(1<<8) | 0x13F` = **0x13F**, colliding with sema 63's
  handle (raw 63, gen 1 → 0x13F);
- the game's resolve turns both handles into raw id 63, so the gate's waits
  on A and B hit the **same** model semaphore — the slice-78 "2 units for 3
  takes" knot is a consequence of this collision, **not a guest-side knot**;
- the generation write for raw 319 lands at 0x00874A4C (past the 1024-byte
  table) and flips 0→1 — a confirmed out-of-bounds side effect.

This supersedes the slice-78 verdict "sema 63 is a guest-side knot; no model
gap" for the knot's cause: the gap is model-side (the id space).

## Fix direction (next slice, not implemented)

Make the model's semaphore id space match the contract the game assumes:
raw ids **within 0..255** with reuse of freed slots (lowest-free-slot, the
shape the reference's composed ids suggest: 6–16, 26, 29–39, all ≤255; the
kernel tables are 256 entries per slice 87). Acceptance: the model's early
sequence stamps A/B/C with **distinct** handles and submits the job
(+0x34=1) without the park; differential and CTest green; kernel tests
updated (they pin the old 3, 7, 11 shape). Also check the thread-id space
for the same class of bug (open).

## Corrections recorded

- Bundled emulator: v2.9.114, not v2.9.94 (folder name stale).
- The live config is `Documents\PCSX2\inis\PCSX2.ini`; the bundled
  `private/pcsx2/pcsx2-config/PCSX2.ini` is not the active file.
- The playbook's "fix the BIOS path" step is already done in the active
  config (it resolves relative to the PCSX2 data root).
- Savestate format version matches the bundled emulator (0x9a590000).

## Limits and honesty

- The reference menu/loop is owner-confirmed and captured in states; the
  model fix is a Hypothesis (it should unblock the gate) until tested.
- The with-card dialog state is kept for history; the old slot-9 states are
  not the no-card analog.
- Only metadata, addresses and counts appear here; payloads and captures stay
  under ignored directories.
