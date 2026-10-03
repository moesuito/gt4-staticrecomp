# M32, sixth slice — the honest maturation path: longer legs

Date: 2026-10-03. Inputs: the pinned CORE and ISO, resumed from the
243.7M checkpoint in chained 60k-service legs. One temporary diagnostic
(an idle-counter readout, since removed) settled a false alarm. Ships
the chained-checkpoint tooling, the raised idle budget, and three
chained legs of measured advance. Decision 0024 records the rationale.

## What shipped (product changes)

- `--resume` + `--checkpoint-at` now combine (decision 0024): the save
  still requires a clean exact-count Syscall stop, so every link is
  exact and counts stay leg-relative. `--verify-resume` stands alone.
- New `gt4boot_checkpoint_chain` CTest: resume the 400-service fixture
  for 400 more and save again (41 tests total now).
- Idle budget 200,000 → 2,000,000 (decision 0024): the guard is this
  model's own artifact, and the slice-5 poke put maturation just past
  the old value.

## Three chained 60k legs (Confirmed)

From `ckpt-243m.bin`: three consecutive limit-hit legs (all `syscall
0x1604`, all saved cleanly: `ckpt-60k/120k/180k.bin`), same shape each
(~33.6k module calls, ~1.88M steps), threads and nodes unchanged,
TIM2 COUNT advancing ~190M per leg
(`0x89e863c0 → 0x951d6ec0 → 0xa052c4c0 → 0xab87f540`), idle counter
chaining forward (212k → 229k → 245k — it is checkpointed, so chains
inherit it). Remaining to the slice-5 threshold: ~7–8 legs at this
rate (Hypothesis, wide bars — see below).

## False alarm, kept as a lesson

The first long leg after the budget raise stopped bit-identical at the
old 11,723-service wall — briefly suggesting the budget was not the
wall. It was a stale binary (the full-build tail had hidden that
`gt4boot.exe` never relinked). The very next build's legs run to the
limit with the idle counter sailing past 200k (212k and on). Lesson:
after a header change, confirm the tool binary relinked before reading
run behavior — `ctest` rebuilds it, a bare `--target` run may mislead
about which objects refreshed.

## Open puzzles for slice 7 (Unknown, with numbers)

- The per-leg COUNT advance (~190M) does not decompose cleanly: idle
  deliveries (+16k/leg × 9,600 = 154M) plus service advances (60k × 576
  = 35M) sum right, but the loop-top path should drain the 280k+
  backlog instead of servicing — yet services advance 60k/leg while
  pending still grows (+20k/leg) with one deferred call stuck throughout.
  The true call mix is unmapped; the firing estimate inherits that.
- The delay base/time-scale question stands (bases near twice total
  elapsed ticks). If the chain fires on schedule, behavior decides
  whether it matters.

Next: keep chaining (longer legs, fewer seams) until a node fires or
the mix is understood — the chain files make every leg resumable, so
no advance is ever lost.

## Verification

- New chain CTest passes; temporary diagnostic removed (grep-clean).
- Chain fidelity: deterministic replay reproduced a leg bit-for-bit
  (33,592 calls); the 128-step resume seam between links is noted and
  bounded (0.007%, states otherwise identical: same waits, same nodes).
- Full gates (CTest 41/41, Python 73) run on the final tree before
  commit.
