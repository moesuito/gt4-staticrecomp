# M32, nineteenth slice — gates open and stable, march continues

Date: 2026-10-03. Inputs: the pinned CORE and ISO; legs D14+D15 from
`ckpt-1580k.bin` (both limit-hit, both saved: `ckpt-1680k/1780k.bin`).
No probes, no model change.

## Legs D14+D15 (Confirmed)

COUNT `0x2ac9f540 → 0x67714d40 → 0xa418a540`, deltas `0x3CA70800` and
`0x3CA75800` — identical to five decimal places across legs (and to
every 100k leg since the rate tripled). Nodes, descs, COMP (`0x240`),
threads, queue (pending 1), deferred frame: all unchanged. Combined
≈ 2.76e9 vs. ≈ 4.29e9 needed: ~1.5 legs to go — D17 fires about
halfway, D16 stays quiet.

## Gate words: open, stable, baseline pinned (Confirmed)

`[0x65c710]` reads `(0, 0x30b188, 0, 0x30b1b0, 0, 0x30b1d8)` — identical
in both legs. So `[0x65c714]` is nonzero (a heap pointer, not a flag):
thread 4's gated engine work RUNS every VBlank already; there is
nothing closed left to open on this gate. The watchlist's first entry
closes as "open from the start" — future gate candidates need the same
treatment (dump first, assume nothing).

## Verification

- Deterministic prefixes reproduce (100,000 calls every leg); saves
  need clean Syscall stops and got them.
- No product-code change (docs only); full gates run on the final tree
  before commit.
