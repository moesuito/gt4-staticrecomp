# M32, twentieth slice — third wrap-miss; marching paused as sterile

Date: 2026-10-03. Inputs: the pinned CORE and ISO; legs D16+D17 from
`ckpt-1780k.bin` (both limit-hit, both saved: `ckpt-1880k/1980k.bin`).
No probes, no model change.

## D16+D17 (Confirmed)

COUNT `0xa418a540 → 0xe0bffd40 → 0x1d675540` (wrap mid-D17), ~1e9 per
leg as steady as ever; nodes, descs, COMP (`0x240`), threads, queue
(pending 1) all unchanged; saves clean. The third sweep of the firing
band with no dispatch (D5, D13, D17) — same shape every time.

## Lottery as the operative model (high confidence)

Firings (poke-2, D9) vs. misses (D5, D13, D17) now read as one pattern:
the firing sliver is sub-service while COUNT advances thousands+ per
service, so each band approach is a lottery ticket, occasionally won.
Nothing about the model or the game changed between a hit and a miss —
only alignment. Consequence: marching onward buys retirements at
lottery odds (~15 more legs for five sterile waiter-retirements, per
thread 3's precedent of wake → find nothing → clean up → sleep).

## Marching paused — and why (decision, recorded here)

The march premised that firing unblocks the boot. Thread 3's full
lifecycle disproved it (sterile). The remaining five firings would
convert sema-waiters into sleepers and nothing else — a deeper park,
not progress. The chain files stay (deterministic, resumable); the
marching stops here until an event-side reason restarts it.

## What unblocks progress instead (next)

The sync-without-async-announcement audit: our model answers SIF/DMA
requests synchronously but never feeds the async completion (pump
queue, dispatcher signal) the waiters need — the twin of the delay
story. Slice 21 re-censuses every parked thread with the new lens
(delay-timeout vs. event-wait-with-unmodeled-announcer vs. sleep),
naming the announcer each event-wait needs. The six delay semas (whose
only signaler is the dispatcher) are the first row of that table.

## Verification

- Deterministic prefixes reproduce (100,000 calls ×2 legs); saves need
  clean Syscall stops and got them.
- No product-code change (docs only); full gates run on the final tree
  before commit.
