# Slice 89: the semaphore id-space fix — the gate passes, the machine marches

Date: 2026-10-09. Baseline: `main` = `4c2aaa2` (slice-88 docs), clean tree.
Task: implement the fix direction slice 88 named — keep semaphore raw ids
inside the game's 8-bit handle space with slot reuse — then verify the boot.
Evidence for the collision itself: `slice88-live-no-card-session.md`.

## What changed

- `Kernel::find_free_semaphore_id` (new): the lowest free candidate from
  3, 7, …, 255 (bits 0 and 1 set per decision 0012, inside the 8-bit space
  the game's handle math assumes). `create_sema` uses it and returns the
  kernel's error (-1) when every candidate is in use; `delete_sema` already
  frees the id, so reuse follows. `next_semaphore_id_` is gone.
- Model compatibility: new `kernel` domain, `kernel_model = 2`
  (decision 0039). Checkpoint format **GT4CPT3** (GT4CPT2 files are refused
  as forensic with a message naming the id-space fix); kernel section
  **GT4KERN2**.
- Kernel unit tests: the 64 ids fill 3..255, the full table refuses, and a
  freed id is reused lowest-first; the checkpoint tests cover the new domain
  and the GT4CPT2 forensic refusal.

## Verification (fresh run, disc attached)

- **Handles are distinct** (services 1915/2000): A=0x177 (raw 119, gen 1),
  B=0x137 (raw 55), C=0x13F (raw 63); the old collision (A=B=0x13F) is gone.
- **The out-of-bounds generation write is gone**: 0x00874A4C stays 0 at
  services 300/1915/2000 (it flipped 0→1 before the fix).
- **The gate passes**: B+0x34 flips 0→1 between services 3,000 and 3,500 —
  the old park at ~3,000 is gone.
- **The machine marches** (module calls / interpreted steps):
  90k services 204,991 / 4,848,449; 500k 1,117,268 / 26,095,464;
  2M 4,454,821 / 103,827,113; 2.5M 5,567,345 / 129,737,981. VIF1 DMA:
  36,851 starts / 15.6 MB at 2M; 46,070 / 19.5 MB at 2.5M; 68,140 / 28.9 MB
  at the stop. No stationary cycle.
- **200M-step stop**: 3,697,027 services, 8,230,771 module calls,
  191,769,229 interpreted steps, boundary `step-limit 0x005b8218`; thread 4
  runs (status 0x1), thread 1 waits (1/0); 16 threads.
- **Gates**: CTest **53/53** (52.50 s); Python **73, OK (skipped=6)**; the 17
  gt4boot tests including both differential legs pass.

## Limits and honesty

- The stop at the step budget is a budget, not a new park; the next frontier
  beyond it is not yet characterized (a longer march is the next experiment).
- The reference comparison stays by meaning: the reference's raw ids are
  PCSX2-HLE ordinals (13/14/30/…), the model's are slot ordinals — both live
  inside the same 8-bit space and the game's flows work in both.
- The slice-88 statement "the model fix is a Hypothesis" is now verified at
  the gate and beyond; the older "guest-side knot" wording is superseded
  (decision 0039).
