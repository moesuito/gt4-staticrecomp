# M30, fiftieth slice — the smallest stimulus: nothing deliverable wakes it

Date: 2026-10-03/04. Inputs: the pinned CORE and ISO. Follow-up to the
forty-ninth slice (two waiter classes, both awaiting unmodeled outside
stimuli). This slice probes every deliverable stimulus from the 243.7M
checkpoint: none wakes the machine, because every handler the model can
deliver is effect-free by the game's own code. The missing pieces are
milestone-scale (M32 async IOP, M35 pad), now precisely scoped.

## Probes (temporary instruments, since removed)

- **INTC cause 0 and cause 5, one shot each from the checkpoint.** Both
  legs end bit-identical to baseline (6,563 module calls, 368,172
  interpreted steps, idle at +11,723). Post-mortem: each injection lands
  behind ~245k queued VBlanks (append-only queue), and both handlers are
  device acks by code reading — cause 0 (0x004ab668) acks GS CSR
  `0x12001000` and counts, cause 5 (0x004ab548) services VIF1 status.
  Neither path can unblock a thread. Wrong tree, correctly negative.
- **SSUP op-4 answered nonzero once.** Never asked: the final leg issues
  no SIF syscalls at all (11,723 patched returns only), so the poller
  itself sleeps. Inapplicable at this phase, not refuted in general.
- **Handler inventory (static).** VBlank: accounting + empty chained
  slot. Timer 2: accounting. Cause 0 / cause 5: device acks above. SIF
  DMAC-ch5 pump (0x005b0e30): drains the inbound queue at 0x886818 into
  dispatcher 0x005ae090 — empty queue, so a no-op. No registered handler
  anywhere contains a wakeup/signal/release path.

## Verdict (high confidence)

Nothing the current model can deliver wakes the parked machine: every
deliverable interrupt runs effect-free guest code, and every waking path
needs originating traffic (async IOP inbound bytes into the SIF pump's
queue, pad reads the game would have to poll, delay-node firings from
decision 0011's open frontier) that only milestone work provides. This
is not a wall to fix with a service answer; it is the boundary where
the synchronous model ends. Next is M32 (async IOP, starting from the
mapped pump/queue/dispatcher) or M35 (pad, starting from the poll
points) — not another probe.

## Verification

- CTest 40/40 and Python 73 (67 run, 6 skip), green on the final tree.
- All temporary instruments (the inject flags, the queue helper, the
  SSUP one-shot) removed; scratch logs deleted; the 243.7M checkpoint
  stays in `build/` as the frontier asset. This slice ships docs only.
