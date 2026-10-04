# Checkpoint-system lesson — snapshots at service boundaries, and the proof that resume is replay

Prepared 2026-10-04. BUILD/VERIFY: as recorded in the sources —
C2: CTest 37/37, Python 73; C3b+c: CTest 40/40 (incl.
`gt4boot_checkpoint`, `gt4boot_resume_verify`), Python 73; chain
tooling: 41 total with `gt4boot_checkpoint_chain`. See the
[mapping plan](../plans/checkpoint-resume-mapping.md),
[decision 0022](../decisions/0022-checkpoint-resume.md),
[decision 0024](../decisions/0024-chained-checkpoints-and-idle-budget.md),
and the C-slice journal records (C1, C2, C3a, C3b+c) in
`docs/journal/2026-10-03.md`. EXPLAIN: this is the worked
explanation; tutoring review pending.

Every load-bearing statement below traces to one of those sources.
The two reframings (0026's flag, the specified-but-unimplemented
autosave) are noted explicitly at the end; the autosave absence
was confirmed by a read-only grep of `tools/gt4boot/main.cpp`
(`--checkpoint-at`/`--resume`/`--verify-resume` present,
`--checkpoint-every` absent).

## Objective and motivation

Every experiment repaid the whole prefix: the boot runs 41.9M
services to its step limit, and each probe near the frontier cost
a full 13–40 minute replay. This arc teaches the project's
snapshot discipline: capture the whole machine at a service
boundary, resume from the file instead of replaying, and prove the
resume equals a fresh run with a differential — not an assertion.
It ends with frontier work (slice 48's wait-graph probes at 243M
services) dropping from a full replay to restore-plus-leg, the
planned ~20x.

Two failures motivate the strictness: a chain that preserves
progress but cannot extend an idle stretch (kept as tooling,
rejected as the maturation path), and a stale binary that faked a
wall the new budget had already removed. Both look, from the
outside, like the machine not advancing.

## Step 1 — map before building (the plan)

The mapping comes from a read-only exploration subagent (nothing
implemented, no behavior changed) and states its design premise
first: snapshots are captured **at a service boundary** (after the
syscall/time advance, with no transfer in flight) — capture and
restore only, never a semantic change. The state inventory:

- `GuestState`: the existing save/restore pair already moves the
  whole context (`RegisterContext`: GPRs, FPRs, HI/LO, accumulator,
  FCR31, shift cache, CP0, full VU0, pc) — trivial. No host RNG
  exists in the model (time comes from the service clock), which
  makes a bit-for-bit checkpoint plausible.
- `GuestMemory`: RAM 32 MiB + 16 KiB scratchpad + the GS block
  (~32 MB per checkpoint); geometry trivial. **The problem**: MMIO
  windows hold `std::function` capturing `this` (not
  serializable) — so rebuild the topology (devices plus
  remappings, like boot construction) and serialize only the
  banks' **contents**. Lazy caches (ISO image, volume reader,
  sector map) reopen rather than serialize.
- `Kernel`: everything serializes except two pointers — threads
  with contexts, semaphores, INTC/DMAC handlers, deferred calls,
  the interrupt queue (mandatory), the syscall table, OSD, SIF
  registers, the RPC server table, the reboot-pending flag, the
  service clock (ticks, per-timer remainders, idle count), file
  handles, the PRTS cache **with cursor**, and the counters.
  Disc pointers and the service table / module are policy:
  relinked or rebuilt on load, with discs reopened hash-verified.
- Driver/interpreter: the pending-transfer flag and target (only
  ever saved as "none"), the statistics counters (which continue
  from N), the limits. Module and service table are stateless and
  rebuilt.

Five risks, each with its verification: a wrongly rebuilt MMIO
topology (sweep known bases + an FNV RAM digest against a clean
run); a transient-state snapshot (save only post-advance under
assert; proof is the resumed-vs-fresh differential); the service
clock and fractional remainders (test with a programmed compare);
the PRTS cursor and re-emitted handles (test mid font-load —
3 reads plus 7 copy-outs — demanding identical bytes); entry
identity (headers with pinned hashes, refusal on mismatch, the
interpreter comparison from the checkpoint). Effort 3–5 slices for
~20x per iteration; checkpoints live under `private/` or
`generated/` (ignored — RAM holds game bytes, never git).

## Step 2 — C1: CPU + RAM with round-trip (fatia A)

First slice: `GuestMemory::regions_snapshot()` (RAM only, no
MMIO), the versioned `GT4CKPT1` format in `ee_checkpoint.hpp/
.cpp` (`save_snapshot`/`load_snapshot`/`restore_memory`), and the
`ee_checkpoint` test — context round-trip over all fields, two
regions, exact blob size, and rejection of bad magic, bad version,
truncation, alias and geometry violations. Built as an isolated
target, without relinking the `gt4boot.exe` of the in-flight 10B
run. No boot behavior changes. Next: the `Kernel` (fatia B).

The lesson worth keeping: the first blob is deliberately the
narrow one (registers + RAM), and its test pins the rejection
classes before any hook exists that could write a bad file.

## Step 3 — C2: the kernel with its cursor (fatia B)

Second slice: `Kernel::save_kernel_state()` /
`load_kernel_state()` in the `GT4KERN1` format — threads with
contexts, semaphores, patches, OSD, deferred calls, SIF registers,
the interrupt queue, INTC/DMAC handlers, RPC servers, the IOP
image, the service clock, file handles, PRTS blocks **with
cursor**, and all counters. Pointers (discs, service table) are
policy and never enter the blob; parsing happens in temporaries (a
malformed blob never touches the live kernel) and exact-end is
required. The `ee_kernel_test.cpp` test drives root+worker,
sema+signal, patch, INTC/DMAC handlers, OSD and PRTS with an
advanced cursor, and proves: equal accessors, save-load-save
byte-identical, copy-out continuing from the cursor, and 3
rejections. CTest 37/37, Python 73.

Note what travels that a naïve snapshot would drop: the
interrupt queue (a snapshot without it resumes into a machine
that forgot what it was about to deliver), the fractional timer
remainders (without them the clock drifts by a fraction per
frame), and the PRTS cursor (without it the font's chunked
copy-outs re-serve chunk zero — the exact divergence M30 slice 46
diagnosed). Each of these is one of the plan's five risks, closed
by name.

## Step 4 — C3a: device banks without effects (fatia C, parte 1)

Third slice: `RegisterBank::registers_snapshot()` /
`restore_registers()` (wholesale swap, no effects), carried through
`DmaChannel` (restores straight into the bank — a restore never
fires a completion) and `TimerUnit`, plus the `GT4BANK1` bank
codec (`save_bank_section` / `load_bank_section`, caller order,
exact end). The `ee_device` test is green: round-trip with stale
values cleaned, restoration without DMA effects, timer, codec, and
3 rejections.

The load-bearing property is the DMA one: restoring a channel's
registers must not re-fire its completion behavior, or every
resume would re-deliver interrupts the fresh run already
consumed. "Restore is storage, never a start bit" is the same
rule the plan's topology risk demanded, now pinned by a test.

## Step 5 — C3b+c: the hooks and the differential proof (fatia C, parte 2)

Fourth slice wires the `gt4boot` hooks: the
`Driver::pending_transfer()` accessor, the `GT4CPT1` container
(service count plus the three section blobs — sections decode
standalone; the file only frames them), `--checkpoint-at N`
(paired with `--services N`: saves if and only if the run stops
at exactly N handled services on a syscall with no transfer in
flight — anything else refuses loudly instead of writing a
snapshot no resume could faithfully continue), `--resume`
(rebuilds identically — image, disc handles, kernel, services,
devices in map order, state — then overwrites registers, RAM,
kernel state and bank registers over it; counters recount from
zero, so `--services` on a resumed run means that many more),
`--verify-resume` (the resumed leg vs the same total fresh:
identical stops by kind/pc/service, identical states by
registers plus memory digest), and `--resume` extending
`--compare-interpreter` to the resumed total.

The first fireproof, as cited: `resume states identical (800
services)` — checkpoint at 400 plus 400 resumed equals 800
direct. Decision 0022 records the four rules (three magics with
strict framing; boundaries only; rebuild-then-apply; the proof is
a differential) and the consequences: `gt4boot_checkpoint` pins
`--checkpoint-at 400` (33.6 MB file, mostly the 32 MiB RAM),
`gt4boot_resume_verify` proves resume-for-400 equals direct-800,
CTest 40/40 — while resuming with a disc attached, large-N
verification, and any use beyond debugging stay explicitly out of
scope.

## Step 6 — chains that preserve, and the budget that extends (decision 0024)

Slice 5 proved the parked boot waits on TIM2 delay maturation
(~1.9e9 COUNT ticks past the checkpoint, ~202k consecutive idle
deliveries at the observed rate) against an idle budget of
200,000 — the run dies at or just before maturation. The budget
is the model's own stuck-machine guard (slice 11 picked 200,000
as "about an hour of virtual frames"); real hardware has no such
guard and keeps delivering frames. Three options, as decided:

1. **Chained checkpoints** (save each leg end, resume). Built and
   tested — `--resume` + `--checkpoint-at` combine, and
   `gt4boot_checkpoint_chain` proves saving works from a resumed
   leg. But the idle counter is checkpointed too, so a chain
   inherits the nearly-full counter and idles out ~1.7k services
   later — proven by leg 2 (1,723 services to
   `no-runnable-thread`). Chaining preserves progress but cannot
   extend a single idle stretch. Kept as tooling, rejected as the
   maturation path.
2. **Raise the budget** (adopted): 200,000 → 2,000,000, about 10x
   past the measured ~202k maturation. Idle delivery is cheap;
   the only cost of a higher guard is wall-clock on truly stuck
   runs, and no test depends on the old value.
3. **Faster timer rate.** Rejected without evidence: the
   per-frame step matches the hardware clock, and the poke showed
   the game proceeding normally once matured. The rate is not the
   wall; the guard is.

Outcome (slice 6): the tooling works — three chained 60k legs run
to their limits and save cleanly (`ckpt-60k/120k/180k.bin`),
COUNT advancing ~190M per leg toward the threshold (~7–8 legs to
go). The budget raise verified live (idle counter past 200k, legs
no longer dying at 11.7k) after a stale-binary false alarm: the
first post-raise leg died at the old wall because `gt4boot.exe`
had never relinked — confirm the relink before reading runs. The
firing itself stays ahead; the chain files preserve every step,
so slice 7 continues from `ckpt-180k.bin`. The delay
base/time-scale puzzle stays open as Unknown; the run's behavior
decides whether it matters.

## What this arc does not claim (reframings)

- **0026's flag rides the kernel snapshot tolerantly.** When the
  first originating event (decision 0026) adds its one-shot flag
  to the kernel, the snapshot carries it — with a tolerant read
  so pre-decision checkpoints load as unsent (see the tripwire
  lesson and `m32-slice30-first-event-live.md`). Old files keep
  loading; the flag simply starts unset. Extension without
  invalidation is the payoff of versioned sections with strict
  framing: the format can grow because every reader states its
  magic.
- **Autosave is specified, NOT implemented.** The mapping's §5
  (owner decision, future) specifies `--checkpoint-every K DIR
  --keep N` plus `--quiet`: photos only at clean points, dirty
  points skipped quietly and never aborting, failures never
  photographed (resume from the last healthy one shortens replay
  instead of eliminating it). A read-only grep of
  `tools/gt4boot/main.cpp` confirms the current surface is
  exactly `--checkpoint-at` / `--resume` / `--verify-resume` —
  no `--checkpoint-every` exists. Manual use must prove itself at
  scale first; until then §5 stays a pointer, and any lesson or
  slice claiming autosave behavior is inventing it.

Nothing here contradicts later work: this arc made snapshots
exact (C1–C3a), resumption proven-equal (C3b+c, decision 0022),
and long idle stretches reachable (decision 0024). Whether any
particular resumed leg *advances the boot* is the separate
question each later slice answers with the files this arc's
tooling preserves.

## Connection to our implementation

The sources name units, formats, flags, and tests — so the table
maps each piece to its mechanism and its source:

| Piece | Mechanism (as cited / verified read-only) | Source |
| --- | --- | --- |
| Boundary rule | snapshot only post-advance with no transfer in flight; `Driver::pending_transfer()` | plan / C3b+c |
| CPU+RAM blob | `GuestMemory::regions_snapshot()` (RAM, no MMIO); `GT4CKPT1` (`save/load/restore_snapshot`); `ee_checkpoint` test + 5 rejection classes | C1 |
| Kernel blob | `save_kernel_state`/`load_kernel_state`; `GT4KERN1` (threads, semas, patches, OSD, deferred, SIF, queue, handlers, RPC, IOP image, clock, files, PRTS+cursor, counters); temporaries + exact-end | C2 |
| Bank blob | `registers_snapshot`/`restore_registers` (no effects); DMA bypass; `GT4BANK1` codec; `ee_device` test + 3 rejections | C3a |
| File + hooks | `GT4CPT1` (count + 3 blobs); `--checkpoint-at` (exact clean count or loud refusal); `--resume` (rebuild, apply, recount); `--verify-resume` (same stop + states + digest) | C3b+c / 0022 |
| First proof | ckpt-400 + 400 == direct-800; `gt4boot_checkpoint` (33.6 MB), `gt4boot_resume_verify`; CTest 40/40 | C3b+c / 0022 |
| Chains | `--resume` + `--checkpoint-at` combine; `gt4boot_checkpoint_chain`; counter inherited (leg 2: 1,723 to idle-out) | 0024 outcome |
| Idle budget | 200,000 → 2,000,000 (~10x past ~202k); rate rejected; stale-binary alarm | 0024 |
| Flag growth | 0026 one-shot flag snapshotted, tolerant read (pre-decision files load unsent) | tripwire lesson (pointer) |
| Autosave | specified in plan §5; `--checkpoint-every` confirmed absent from `main.cpp` | plan (pointer) |

## Understanding checkpoint

1. `--checkpoint-at N` refuses loudly unless the run stops at
   exactly N services on a clean syscall boundary. Why is a loud
   refusal better than writing the best available snapshot — and
   what resume failure does it prevent?
2. The kernel blob carries the interrupt queue, the fractional
   timer remainders, and the PRTS cursor. For each one: name the
   concrete divergence a resumed run would show if that field
   were dropped.
3. Restoring a DMA channel writes its registers straight into the
   bank and never fires a completion. Explain the double-delivery
   that re-firing would cause, and which test pins the no-effect
   property.
4. `resume states identical (800 services)` proves ckpt-400 + 400
   equals direct-800. What does that equality establish that a
   save-load-save byte-identity check cannot — and why must the
   proof compare stops *and* states *and* digests?
5. Chained checkpoints preserve progress but cannot extend a
   single idle stretch. Explain the mechanism (which checkpointed
   field defeats the chain), and why raising the budget fixes
   what chaining cannot.
6. The 0026 flag loads tolerantly from pre-decision files, while
   autosave stays unimplemented. State the rule that makes the
   first safe to rely on and the second forbidden to claim — and
   what evidence would move autosave from pointer to product.
