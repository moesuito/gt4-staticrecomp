# 0034 - Interrupt-delivery granularity contract (option A: emitter poll points)

Date: 2026-10-04. Status: accepted (implemented in slice 72).
Predecessors: 0011 (timer ticks and DMA completions), 0029 (P01+P02),
0033 (P06 DMA payload and chains). Evidence:
`docs/reverse-engineering/slice70-1606-hunt.md` (the divergence),
`docs/reverse-engineering/slice71-raise-census.md` (the sizing),
`docs/reverse-engineering/slice72-optiona-prototype.md` (the
implementation and the three green acceptances). Closes incident 1606.

## Context

The driver delivers pending interrupts at translated-call/bridge-step
boundaries; the interpreter delivers before the next instruction. A
DMA channel programmed by guest code completes synchronously and
raises mid-module on both engines, so the same raise landed at
different guest pcs: driver at the module-exit continuation,
interpreter at the next instruction (slice 70: `0x004a1274` vs
`0x004abae4`, sp off by the frame, r29 + 3 stack words at the stop).

Census bounds (90k-service disc boot + dual 1605/1606 legs, slice 71):

- 21.8% of raises are in-module (2,668 of 12,227), from exactly one
  guest function (`004aba50`, VIF1/GIF DMA starts, ~2 per execution,
  0.6% of module calls). 100% of them diverged in delivery pc.
- 78.2% are loop-top (SIF-5, VBlank-2, TIM2-11) and delivered
  identically on both engines, field for field. Zero bridge-step
  raises. Queue depth <= 3.
- The divergent class's handler (`0x004ab6d8`) is pure: no guest
  syscalls, sp always restored. The syscall-issuing handlers are all
  loop-top-aligned. Divergent delivery is transient (green 1605 after
  10 divergent deliveries); only stops inside the fresh window (1606)
  observe it.

## Decision

**Option A: emitter poll points at guest-programmed DMA starts
(scoped).** Translated code checks the pending queue after a store
that starts a DMA transfer (STR to one of the completing channel
windows), so a synchronous completion delivers inside the module at
the same guest pc the interpreter uses.

1. **The emitter appends the check after every falling-through `sw`.**
   `GuestState::poll_dma_start(address, value, next_pc)` fires only
   on an exact completing-channel CHCR write (VIF0/VIF1/GIF) with STR
   set — with KSEG/mirror folding under the same condition the memory
   uses — and only when a delivery path is wired; anything else falls
   through with the pc untouched. On a match it sets the pc to the
   next guest instruction and asks the kernel (`start_interrupt`) to
   deliver; the early `Returned` unwinds through the existing caller
   propagation to the driver loop-top. A refused delivery (masked,
   gated, handler-less) continues translated execution exactly as
   before.
2. **Both evidences stand.** The P06 at-once completion is unchanged
   (the transfer still concludes inside the STR write); the reference
   stays instruction-exact (no deferral, no boundary-batch rule).
3. **Versioning.** Emitter semantics changed, so `translation_model`
   goes 1 -> 2; older checkpoints refuse loudly.
4. **Explicit non-coverage.** `swl`/`swr`, delay-slot stores and the
   runtime fallback carry no poll points: 90k services show every
   in-module raise comes from a plain falling-through `sw` by one
   function. Any future guest DMA programmer outside that shape
   reopens this scope; the census method is the check.

Options B (interpreter defers DMA delivery to region exits) and C
(deterministic async DMA at loop-top boundaries) were rejected in the
draft and stay rejected: B weakens the reference where it is most
needed and hides future translator bugs; C contradicts the measured
P06 synchronous-completion evidence and opens a new divergence class
for guest reads between STR and the boundary.

## Acceptance (all green in slice 72, MSVC 19.44 x64, disc boot)

- `--services 1606 --compare-interpreter` green (boundary
  `0x00001604`/`0x100`, 8200 module calls, state identical — the old
  r29 divergence gone).
- `--services 90000 --compare-interpreter` green (215013 module
  calls, exactly the census leg's count, 26812702 interpreter
  instructions, full state identical).
- Fresh raise census: 12,227 raises with identical dual-engine
  sequences, 12,224 deliveries with zero field mismatches, and all
  2,668 in-module raises delivering at `0x004abae4` on both engines.
  Zero in-module raise / divergent-delivery pairs.
- `WILL_FAIL` markers on both `gt4boot_services` legs removed by
  those green runs; the loud mismatch regexes stay.

## Slice 99 scope clarification (2026-10-09, no semantics change)

Pinned PCSX2 source differentiates interpreter BEQ/BNE-false event testing
before the next executed word from normal dynarec paired-slot paths. Other
branch families differ too; debugger Step Into does not force interpreter.
This decision's observed plain falling-through SW scope remains intact, not
an all-branch event/time contract. Source-derived trap EPC/BD predictions do
not establish BIOS-selected return or justify servicing native stopped slots.
See decision 0041 and `docs/reverse-engineering/slice99-branch-slot-event-audit.md`.
