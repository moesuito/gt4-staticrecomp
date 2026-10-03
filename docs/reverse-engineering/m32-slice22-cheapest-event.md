# M32, twenty-second slice — the cheapest event: look at the screen pipe

Date: 2026-10-03. Inputs: the pinned CORE and ISO; four disassembly
reads, three translated-code greps, two probe legs (SIF0 regs, job
ring, 12k service mix), one RAM scan. No model change. This slice was
chartered to pick the cheapest originating event — and the answer is
none of the three candidates, but a fourth: check whether the engine
emits anything at all.

## The three candidates, adjudicated (Confirmed)

- Main thread 1's wait: condvar via the `0x576xxx` helpers, sitting
  inside SDK flag-callback machinery (`0x00109340` flag dispatcher +
  `0x001097f0` chain). Feeding its queue needs the queue object plus
  its producer — both unknown. Cost unknown (medium-hard). NOT picked.
- The `0x00587b` frame dispatcher (wake-by-table, join on completion
  semas, signal onward, self-sleep): dormant, invoked only indirectly
  (one direct `jal` site in all translated code, to elsewhere).
  Tracing its invoker means object-graph forensics in RAM. Cost high.
  NOT picked.
- The stuck SIF0 channel: **falsified in 2 minutes**. CHCR has STR set
  but MADR/QWC/TADR read all zero — no transfer exists to complete;
  slice 48's "vestigial" stands doubly confirmed. DEAD.

## Two hard negatives that sharpen the picture (Confirmed)

- The job-ring producer is fully silent: consumer = producer = `0xb5`
  at `ckpt-243m` and still `0xb5`/`0xb5` now — 181 historical jobs,
  zero since, across the entire chain.
- A 12k service mix is 12,000/12,000 `0x100`: zero signals, zero wakes,
  zero SIF, zero pad. Nothing announces anything.

## The pick: M33 recon first (decision, recorded here)

Nobody looked at whether the engine *emits* anything: with VBlank
handlers live and gates open, the game may be submitting graphics
packets (DMAC → VIF → VU1 → GIF) that nobody observes. One recon
slice instruments DMA/GIF traffic (model-visible registers and data,
temporary) and answers binary: packets flow (the boot is alive at the
graphics layer — M31 becomes presentational) or the pipe is dry
(confirmed deep park, back to main's wait). It is the cheapest
falsifiable probe of engine output, it reuses existing machinery, and
the GIF/VIF channel controls already read idle (fast negative if so).
Thread 3's sleep-site migration (no observable announcer, no new
nodes) stays open as one line — interesting, not load-bearing.

## Verification

- Every negative above names its probe (regs dumped, mix tallied,
  RAM scanned); the failed static searches stay failed on record.
- Temporary probe and scratch log deleted; no product-code change
  (docs only); full gates run on the final tree before commit.
