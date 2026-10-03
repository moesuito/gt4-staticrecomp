# M32, ninth slice — the wrap swept the band: no firing, and why

Date: 2026-10-03. Inputs: the pinned CORE and ISO; legs D4+D5 from
`ckpt-480k.bin` (both limit-hit, both saved: `ckpt-580k/680k.bin`). No
probes, no model change. The headline is negative and precise: TIM2
COUNT wrapped mid-D5, sweeping the entire theoretical firing band with
no dispatch — so the band is not the whole story. The slice reframes
the hunt from levels to delivery phasing, and from timeouts to events.

## Legs D4+D5 (Confirmed)

| leg | COUNT start → end | combined ≈ | signature? |
|---|---|---|---|
| D4 | 0xe3920d40 → 0xf6402e40 | 3.82e9 → 4.13e9 | none (flags 3, COMP 0xfffffe40) |
| D5 | 0xf6402e40 → **0x08ee29c0 (wrapped)** | 4.13e9 → max → 0.15e9 | none (flags 3, COMP unchanged, descs unchanged) |

D5 swept the whole band `[firing neighborhood, 0xFFFFFFFF]` — the range
the slice-5 poke proved sufficient — and nothing fired: no signal, no
unlink, no COMP rewrite, all six still waiting. A pure level threshold
cannot explain poke-fire vs. sweep-miss. What differs is phasing: the
poke's crossing landed inside a delivery-active window; D5's crossing
fell in a stuck desert (below).

## Delivery phasing: burst, then desert (Hypothesis, with numbers)

- The interrupt backlog keeps growing (245k → 280k → **463k** pending):
  VBlank is enqueued per idle call with no coalescing, while deliveries
  only dribble. A 463k-deep FIFO means any given timer interrupt waits
  thousands of deliveries that never come in-leg.
- The stuck deferred frame, parsed from `ckpt-680k.bin`: a VBlank
  (cause 2) with **all six handlers still pending** and a garbage
  context pc — born at the limit-hit, never started. Every leg ends
  the same way (deferred = 1), so every leg's early deliveries run,
  then the frame wedges and the rest of the leg is a delivery desert.
  The poke's crossing (~1k services in) sat inside the burst; D5's
  crossing (~53k in) sat in the desert.

## The deeper paradox (Unknown, recorded deliberately)

The six delay bases (≈ 4.29e9 combined) exceed this run's total elapsed
ticks: as timeouts they span ~40 minutes — not a real boot path on any
hardware that boots in 30 seconds. The waits read far more like
timeout-guarded *event* waits whose real completions (async replies the
model never delivers through the stalled queue) would arrive first on
hardware, with the delays as fallback guards that get cancelled. Under
this reading, marching 14 more legs to the timeout is the slow, wrong
way: the right quarry is the missing events and the stalled delivery.

## Next: slice 10 — unstick deliveries, hunt the events

1. Measure the burst/desert mix exactly (temporary counters — no more
   armchair).
2. Prime suspect as a concrete model bug: unbounded VBlank enqueue
   with no coalescing (hardware keeps status bits; a new frame does not
   queue behind 463k unhandled ones). If confirmed, coalesce.
3. For each of the six waits, name the real completion event beside
   the timeout guard (the wrapper's request side, slice 4's spine).

The chain files preserve all advance; the 14-leg timeout march stays a
fallback, explicitly demoted.

## Verification

- D4/D5 limit-hit with Syscall boundaries and saved cleanly; node,
  descriptor, thread, and COMP reads identical except COUNT/pending.
- No product-code change (binary is slice 6's); gates re-run on the
  final tree before commit.
