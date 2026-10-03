# M32, fourteenth slice — the band swept twice, still silent

Date: 2026-10-03. Inputs: the pinned CORE and ISO; legs D11–D13 from
`ckpt-1280k.bin` (all limit-hit, all saved: `ckpt-1380k/1480k/1580k.bin`).
No probes, no model change. The result is a clean repeat with the
phasing excuse removed — and it forces the threshold math to be
redone, which in turn reopens slice 7.

## D11–D13: march and second wrap (Confirmed)

COUNT `0x74d3ed40 → 0xb17b4540 → 0xee229d40 → 0x2ac9f540` (wrap
mid-D13), ~1e9 per leg as steady as ever; nodes, descs, COMP
(`0x240`), threads all unchanged; saves clean every leg. The second
full sweep of the theoretical firing band, this time with live
per-frame delivery (coalescing holds, pending stays ~1) — and still
no dispatch, no unlink, no signal.

## The math, done carefully, says it should have fired (Confirmed
arithmetic, contradicted outcome)

For node `0x00889f80` (target `0x48000 + 0xFFFFFDC000`): firing needs
combined ≥ `target >> 8` = `0xFFFFFA50`. Since the overflow OR forces
the middle bits, that condition is exactly `COUNT_top9 = 0x1FF` — an
8M-tick window (~800 services) tested every frame. Missing it is
essentially impossible. Yet D13 swept it silently. And symmetrically:
slice 5's poke fired node `0x0088a000` from combined `0xFDDF63C0`
(top9 `0x1F8`) — *below* any threshold the same math produces. Both
directions break the story that the due test at `0x005B822C` alone
decides firing.

## What this means (Hypothesis, ranked)

1. The walk may never reach the test: gate (`MODE & 0x400` at handler
   time), head pointer moved mid-leg, or the `MODE & 0x800` overflow
   branch diverting. All three are directly readable — slice 15 dumps
   `[0x6592F0+0x18]` (head) and overflow every leg instead of assuming
   them.
2. Slice 7's attribution is downgraded: node `0x0088a000`'s
   consumption (flags → 0, unlink, desc cleared, sema signaled) is
   observed fact, but the *path* is uncertain again — a cancellation
   path could unlink and signal without the due test. The poke
   threshold narrative is withdrawn, not the observations.
3. The firing forecasts (slices 8–13) were computed from the same
   broken math; no forecast stands until the reachability question is
   settled. The march files stay valid advance (deterministic,
   saved), but marching is paused until a firing criterion that
   survives both directions exists.

## Verification

- Deterministic prefixes reproduce (100,000 calls ×3 legs); saves need
  clean Syscall stops and got them. No instruments to remove.
- Full gates run on the final tree before commit.
