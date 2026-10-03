# 0026 — the first originating event: a SIF pump SET_SREG packet (mechanism, not unblock)

Date: 2026-10-03. Evidence: the waiter censuses
(`m32-slice21-announcement-audit`, `m30-slice48-wait-graph`, today's
1980k leg), the pump-path map (`m32-slice29-pump-path`), the march
record (slices 5–20, esp. 17's live walk trace and 20's sterile
verdict), and decision 0023 with its slice 2–3 outcome.

## Context

Every recon line is closed and all agree: the parked machine needs
originating traffic, and no parked thread can produce it. The
candidates were an RPC reply through the SIF pump waking thread 2,
a PCDV/PRTS completion waking a worker, and a VBlank-chained engine
completion. Mapping the delivery path changed the ranking — this
decision records the honest outcome, including the leading
candidate's explicitly bounded effect.

## Options considered and ranked

1. **SIF pump SET_SREG packet (ADOPTED as specified mechanism).**
   The only hardware-faithful, model-synthesizable traffic with a
   complete game-side consumer path: queue bytes → DMAC-ch5
   completion → pump `0x005b0e30` → drain → SifSetDChain re-kick →
   table dispatch → register write → gp restore. Deterministic,
   additive (the queue is empty today), fully specified below. Its
   honestly bounded effect: it advances register state and proves
   the async path live, but wakes nobody — the two reachable
   handlers are register accessors and no waiter polls them. It is
   the first event because it is the only synthesizable one, not
   because it unblocks anyone.
2. **Delay maturation (REJECTED as unblock; needs no synthesis).**
   The walk runs constantly (2,000 runs per 2k leg, slice 17) and
   would mature by time alone — but firings are proven sterile
   (thread 3's precedent: wake, find nothing, deeper park, slice
   20). Time unblocks nothing; marching is paused until an
   event-side reason restarts it.
3. **Ring-job post waking thread 2 (REJECTED: fabrication).**
   Posting `{0,N}` + signaling sema 11 would pick which thread wakes
   — a game-side decision the model cannot know. The 181
   historical `{0,3}` jobs most likely come from the delay system
   itself, which reduces this to option 2.
4. **VBlank-chained completion (REJECTED: saturated).** VBlank
   delivers every idle tick already; its chains are effect-free by
   the game's own gates.
5. **PCDV/PRTS completion waking a worker (REJECTED: no waiter).**
   No parked thread waits inside a disc-read path (sema-waiters are
   in delay wrappers, sleepers in condvars); there is no live
   request to complete, and final legs issue no SIF calls at all.
6. **Pad/GS-side (REJECTED: no consumer).** Standing since slice 1.

## Decision

Slice 30 implements ONE synthesized SIF pump packet, exactly:

- **Bytes** (guest RAM, physical `0x00886740` via `[0x00886818]`):
  count byte `0x18` at `[queue+0]`, then words `{0x00000000,
  0x00000001, 0x00000000, reg, value}` — word 2 (`& 0x7fffffff =
  1`) dispatches table entry 1 (`0x005b0850`); word 4 = register
  index, word 5 = value. Register 1, value 1: reproduces the live
  console state of slice 14 (regs 0 and 1 both set) rather than
  inventing one.
- **Cause**: DMAC channel-5 completion through the existing
  `queue_dmac_completion` machinery; the registered handler
  `0x005b0e30` runs in interrupt context (no nesting, no
  preemption — decision 0013 unchanged).
- **Trigger (determinism rule, cf. decision 0016):** a pure function
  of the guest service sequence — the first idle-tick delivery at
  which (DMAC-ch5 handler registered) and (queue count byte == 0)
  both hold, one-shot per boot. Both engines (translated module and
  reference interpreter) share the kernel and the service sequence,
  so both inject at the same boundary and the differential stays
  green by construction. No host timing may enter the condition.
- **Observable effects (all model-side or game-read):** pump drains
  (count byte back to 0), `SIFREG[1]` reads 1 at stop, SIF0 CHCR
  shows the re-kick (`STR`), handler chain returns cleanly.

## Verification bar (slice 30 must meet all four)

1. **Gates green**, especially every differential: identical full
   state vs the interpreter at the comparison point (proves the
   injection is sequence-deterministic, not host-timed).
2. **Mechanism asserted**: stop-time dumps show the register write
   and the drained queue; the pump ran (no crash, clean return).
3. **Negative assertion (load-bearing): the thread census before
   and after is identical** — no waiter wakes. This pins the
   "mechanism, not unblock" verdict into the test suite so no later
   slice can mistake path-liveness for progress.
4. **New coverage**: a kernel-level test for the queue→register
   write (synthetic packet, no game data) plus the boot-level
   register-set + census-unchanged assertions. Extend the existing
   CTest/Python fixtures; no parallel harnesses.

## Consequences and what it unlocks

- The async path (DMAC completion → handler → table → gp-restore)
  runs live for the first time — the machinery every future
  originating event (RPC replies once a waiter exists, pad/input
  servers, GS-side) will reuse. Slice 30's implementation is that
  machinery's first customer, not the boot's unblocker.
- The negative assertion converts today's honest "wakes nobody"
  into a tripwire: the day a synthesized (or matured) event DOES
  wake someone, the census changes and the test fails loudly —
  which is exactly the signal the main line is waiting for.
- Explicit non-goals: no ring posts, no fabricated wakes, no pad
  traffic (still no consumer), no delay-model changes (sterile).

## Trade-offs

- Cost: one small, deterministic, additive model behavior plus
  tests; zero game-code assumptions beyond verified bytes.
- Risk: a reader could mistake "async path live" for "boot
  unblocked" — mitigated by verification item 3, which asserts the
  opposite.
- Alternative rejected: doing nothing keeps the stall forever;
  synthesizing a wake fabricates game decisions (evidence
  discipline forbids it); waiting on legs replays proven sterility.
