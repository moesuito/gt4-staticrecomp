# 0023 — M32 framing: the first originating traffic comes through SIF

Date: 2026-10-03. Evidence:
`docs/reverse-engineering/m32-slice1-server-inventory.md` (24 bound
servers, provably empty pump queue, thread-2 dispatch loop, no PADMAN).

## Context

Slices 47–50 proved the 243.7M-service stop is event starvation: every
deliverable interrupt runs effect-free guest code, and every waking
path needs traffic the synchronous model never originates. Two
milestones could supply it: M32 (async IOP) and M35 (pad).

## Options considered

1. **Pad first (M35).** Rejected for now: the server inventory shows no
   `0x80000100`/`0x80000101` (PADMAN) bound and the final leg issues no
   SIF calls at all, so the game has no pad consumer at this phase.
   There is nothing to answer yet; M35 waits on game progress.
2. **Another synchronous reply.** Rejected: same reason — with no live
   SIF caller, no answer can arrive anywhere.
3. **Async SIF traffic (M32).** Adopted: the pump queue is provably
   empty, the SIFCMD table is mapped, and thread 2 is shaped like a
   semaphore-gated dispatch loop. Originating traffic means bytes the
   IOP sends unasked — an inbound packet for the pump, or a call into
   an EE-side server loop.

## Decision

M32 proceeds in slices, additive to the synchronous model (nothing that
answers today changes, so the differentials stay green):

- Slice 2: identify thread 2's job source — its argument block and
  sema 11's signaler — by tracing `0x005ae9a0` live (the callees
  `0x005adbd0`/`0x005adce0` resolve to which syscalls; a
  signal/wakeup log from the checkpoint names the giver).
- Slice 3: deliver the first originating packet shaped by what slice 2
  finds, and watch which thread wakes.

## Consequences

- `gt4boot --threads` now lists bound SIF server sids permanently
  (`Kernel::sif_server_sids()`, unit-tested): the inventory M32 works
  from.
- No checkpoint format change (servers were already snapshotted).
- If thread 2 turns out not to be an EE-side server loop, slice 2 says
  so and the pump-queue packet becomes the lead — the framing holds
  either way.
