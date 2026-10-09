# Decision 0039: semaphore ids stay inside the game's 8-bit handle space

Date: 2026-10-09. Slice 89. Status: **accepted (implemented, green)**.
Evidence: `docs/reverse-engineering/slice88-live-no-card-session.md` (the
collision found live), `docs/reverse-engineering/slice89-semaphore-id-space-fix.md`
(the fix and its verification).

## Context

The live no-card session (slice 88) showed the model's A/B handle collision:
the kernel handed out semaphore raw ids 3, 7, 11, … monotonically. The 80th
allocation returned 319 = 0x13F; the game's create wrapper (0x005782E8)
composes `handle = (generation << 8) | raw_id` and the resolve wrapper
(0x00578290) decodes `raw_id = handle & 0xFF`, so raw 319 produced the handle
**0x13F** — the same handle as raw id 63's semaphore. The gate's waits on A
and B then hit one semaphore, the "2 units for 3 takes" deadlock the old
slices called a guest-side knot; the generation write for raw 319 also landed
outside the game's 256-entry table (index 319 → 0x00874A4C).

## Decision

- The model's semaphore allocation returns the **lowest free id from
  3, 7, 11, …, 255** — the candidates keep bits 0 and 1 set (decision 0012's
  delay-library requirement) and stay inside the game's 8-bit handle space.
- **A deleted id is reusable**; the game's own generation table distinguishes
  stale handles (its resolve refuses a generation mismatch with -1), so reuse
  cannot alias a live semaphore.
- When every candidate is in use the creation returns the kernel's error
  return (-1). The game's wrappers retry on -1, exactly as they would with
  the real kernel's full table — a loud, visible state, not a silent fallback.

## Alternatives considered

- **Keeping monotonic ids** (3, 7, 11, …, 319, …): refuted by slice 88 — the
  handle composition collides as soon as the raw id passes 255, and the
  generation write leaves the table.
- **Wrapping modulo 256 without reuse**: would let a live id be handed out
  again while still in use.
- **Dropping the bits-0/1 requirement**: decision 0012's delay-library
  evidence stands (the library forces bit 1 and tests bit 0 of the raw id);
  the candidates keep both bits.

## Consequences

- A, B and C stamp with **distinct** handles (raw 119/55/63 at the stop
  checked: 0x177/0x137/0x13F) and the job submits (+0x34=1) between services
  3,000 and 3,500 — the old park at ~3,000 is gone. The machine then marches
  without a stationary cycle: 3,697,027 services at the 200M-step budget with
  the VIF1/GIF pipeline growing (VIF1 starts 36,851 at 2M services, 68,140 at
  the stop).
- The generation write for the init's create stays in bounds (0x00874A4C
  unchanged).
- **Compatibility**: a new `kernel` domain joins the model identity
  (`kernel_model = 2`) and the checkpoint format bumps to **GT4CPT3**
  (GT4CPT2 files are refused as forensic, naming the id-space fix); the
  kernel section bumps to **GT4KERN2** (the `next_semaphore_id` field is
  gone). Kernel unit tests pin the reuse and the full-table refusal.
- Draft decisions 0037/0038 (waiter-first traffic, the F chain) are
  superseded as the park's explanation: no synthesized traffic was needed.
