# M32, eleventh slice — live-handler lockstep, all idle underneath

Date: 2026-10-03. Inputs: the pinned CORE and ISO; a 100k leg from
`ckpt-780k.bin` (limit-hit, saved: `ckpt-880k.bin`) plus a 12k leg
whose full log's service mix was tallied. No probes, no model change.

## Lockstep (Confirmed)

Leg D6: 100,000 services, **100,000 module calls**, 9.2M steps — every
service now runs a module (handler) call, the post-coalescing steady
state fully established. Threads, nodes, descs, COMP all unchanged;
pending stays drained (1); the frontier file shrank (37.6 → 34.5 MB —
the museum no longer accumulates).

## The mix: nothing but time (Confirmed)

All 12,000 services of the tally leg are `0x100` (the patch-return /
idle service) — a single distinct value. No RPC, no pad, no signals,
no wakeups, no fileio. With live handlers the machine issues zero new
work: it matures the clock and nothing else.

## Forecast (Hypothesis, measured rate)

TIM2 COUNT advances ~1.02e9 per 100k leg now (0x459536c0 → 0x823c8ec0;
combined ≈ 2.20e9 vs. the firing neighborhood ≈ 4.26e9): ~2 legs to go,
likely firing mid-D8. Slice 12 runs D7+D8 watching for slice 7's
signature — which live handlers can no longer miss on phasing.

## Verification

- Deterministic prefixes reproduce (module-call counts identical
  across same-length legs); saves require clean Syscall stops and got
  them. Scratch log deleted after tallying.
- No product-code change (binary is slice 10's); gates re-run on the
  final tree before commit.
