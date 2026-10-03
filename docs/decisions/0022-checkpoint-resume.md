# 0022 — Checkpoint and resume for the boot driver

Date: 2026-10-03. Status: accepted (checkpoint slices C1–C3).

## Context

Every experiment repaid the whole prefix: the boot runs 41.9M services
to its step limit, and each probe near the frontier cost a full 13–40
minute replay. The mapping in `docs/plans/checkpoint-resume-mapping.md`
showed a snapshot is feasible without changing model semantics, so the
project built one in four slices: CPU+RAM blobs (C1), kernel state (C2),
device banks plus the `gt4boot` hooks (C3).

## Decision

1. **Three section formats with their own magics** (`GT4CKPT1` for
   context+memory, `GT4KERN1` for kernel state, `GT4BANK1` for device
   banks), each strict (bad magic, truncation and trailing bytes throw),
   framed by a `GT4CPT1` file holding the service count plus the three
   blobs. Sections decode standalone; the file only frames them.
2. **Snapshot at service boundaries only.** `--checkpoint-at N`
   (paired with `--services N`) saves iff the run stops at exactly N
   handled services on a syscall with no transfer in flight; anything
   else refuses loudly instead of writing a snapshot a resume could not
   faithfully continue.
3. **Resume rebuilds identically, then applies.** `--resume` replays the
   same construction (image, disc handles, kernel, services, devices in
   `map_into` order, state) and overwrites registers, RAM, kernel state
   and bank registers over it. Counters recount from zero, so
   `--services` on a resumed run means that many more services. DMA
   restores bypass the start-bit behavior (a restore never fires a
   completion); pointers (disc sources, service table) are relinked
   policy, never serialized.
4. **The proof is a differential, not an assertion.** `--verify-resume`
   runs the resumed leg and the same total fresh, then requires
   identical stops (kind/pc/service) and identical states (registers
   plus memory digest). `--resume` also extends `--compare-interpreter`
   to the resumed total. Checkpoint files hold guest RAM and live in
   the ignored build directory, never in git.

## Consequences

- `gt4boot_checkpoint` pins `--checkpoint-at 400` (33.6 MB file,
  mostly the 32 MiB RAM); `gt4boot_resume_verify` proves resume-for-400
  equals direct-800 with identical states and digests. CTest 40/40.
- Iteration cost past a snapshot drops from a full replay to restore
  (seconds) plus the leg under study — the planned ~20x for frontier
  work such as slice 48's wait-graph probes at 243M services.
- Out of scope (later slices): resuming with a disc image attached,
  large-N verification, and any use of checkpoints beyond debugging.
