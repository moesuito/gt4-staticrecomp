# Decision 0034 (DRAFT): interrupt-delivery granularity contract

Status: **DRAFT** — not decided, not implemented. This draft turns the
slice-71 census (`docs/reverse-engineering/slice71-raise-census.md`)
into measured options for closing incident 1606. The `WILL_FAIL`
markers on both `gt4boot_services` legs stay until the chosen contract
lands and the differential goes green.

## Problem (measured)

The driver delivers pending interrupts at translated-call/bridge-step
boundaries; the interpreter delivers before the next instruction. A
DMA channel programmed by guest code completes synchronously and
raises mid-module on both engines, so the same raise lands at
different guest pcs: driver at the module-exit continuation,
interpreter at the next instruction (slice 70: `0x004a1274` vs
`0x004abae4`, sp off by the frame, r29 + 3 stack words at the stop).

Census bounds (90k-service disc boot + dual 1605/1606 legs):

- 21.8% of raises are in-module (2,668 of 12,227), from exactly one
  guest function (`004aba50`, VIF1/GIF DMA starts, ~2 per execution,
  0.6% of module calls). 100% of them diverge in delivery pc.
- 78.2% are loop-top (SIF-5, VBlank-2, TIM2-11) and deliver
  identically on both engines, field for field. Zero bridge-step
  raises. Queue depth <= 3.
- The divergent class's handler (`0x004ab6d8`) is pure: no guest
  syscalls, sp always restored. The syscall-issuing handlers are all
  loop-top-aligned. Divergent delivery is transient (green 1605 after
  10 divergent deliveries); only stops inside the fresh window (1606)
  observe it.

## Options

### A. Emitter poll points at guest-programmed DMA starts (scoped)

Translated code checks the pending queue after a store that starts a
DMA transfer (STR to one of the completing channel windows), so a
synchronous completion delivers inside the module at the same guest
pc the interpreter uses.

- Pros: the interpreter stays instruction-exact (the reference is not
  weakened); blast radius is the STR-write sites, not every boundary;
  matches the census shape (one emitter, pure handler); no device
  timing changes, so P06's at-once evidence (decisions 0011/0029/0033)
  stands.
- Cons: translator/emitter work with a coverage obligation — the
  census lists today's sites (`004aba50` only), but the mechanism must
  cover any future programmer, not the list; every downstream claim
  (P07, P09, P10) re-verified after.
- Measured cost anchor: ~1 in-module raise per 34 services; polls
  scoped to DMA-window STR writes fire ~1335 times per 215k module
  calls.

### B. Interpreter defers DMA-completion delivery to region exits

The reference loop stops delivering D-class causes instruction by
instruction and instead delivers them at translated-region exits,
matching the driver's granularity.

- Pros: no translator work; one deferral rule; loop-top classes
  already align, so the rule is DMA-scoped too.
- Cons: weakens the reference exactly where it is most needed — the
  differential would compare two coarse engines instead of an exact
  one against a coarse one, and future translator bugs inside DMA
  programmers would hide behind the deferral; needs the module-entry
  map inside the reference loop (a layering break); full
  re-verification all the same.

### C. Deterministic async DMA (complete at loop-top boundaries)

Transfers complete at the next service/idle boundary instead of inside
the STR write. Both engines raise at the same boundary, so delivery
pcs align by construction; matches the async-IOP direction of
decision 0023.

- Pros: single mechanism for present and future devices; no emitter
  or reference granularity hacks.
- Cons: contradicts the measured P06 evidence — completion is
  synchronous today (STR clears, QWC zeroes, payload moves inside the
  write; decisions 0011/0029/0033 and the payload capture depend on
  it); any guest read between STR and the boundary would see
  un-completed state, a new divergence class; largest
  re-verification surface of the three.

## Recommendation

**Option A**, scoped to DMA-window STR writes: it is the only option
that leaves both the device evidence (P06, at-once completion) and
the reference's exactness intact, and the census proves its target is
small (one function, one pure handler, zero bridge involvement).
Prototype behind the blind gate (markers stay): poll after STR, run
the 1606 differential, then extend the census leg to 90k and require
a green 90k differential before P07/P09 resume.

## Acceptance for closing incident 1606

- `--services 1606 --compare-interpreter` green (r29 identical).
- `--services 90000 --compare-interpreter` green (the census leg's
  volume, both engines, full state).
- Fresh raise census shows zero in-module raise/delivery-pc pairs.
- `WILL_FAIL` markers removed only by that green run (an XPASS still
  goes red until they are gone).
