# 0024 — raise the idle budget past delay maturation (chained checkpoints)

Date: 2026-10-03. Evidence: `docs/reverse-engineering/m32-slice5-delay-node-fires.md`
(the poke threshold), slice 6's chain legs below.

## Context

Slice 5 proved the parked boot waits on TIM2 delay maturation (~1.9e9
COUNT ticks past the checkpoint, ~202k consecutive idle deliveries at
the observed rate). The idle budget is 200,000 — the run dies at or
just before maturation. The budget is this model's own stuck-machine
guard (slice 11 picked 200,000 as "about an hour of virtual frames");
real hardware has no such guard and simply keeps delivering frames.

## Options considered

1. **Chained checkpoints** (save each leg end, resume). Built and tested
   (`--resume` + `--checkpoint-at` now combine; `gt4boot_checkpoint_chain`
   proves the save works from a resumed leg). But the idle counter is
   checkpointed too, so a chain inherits the nearly-full counter and
   idles out after ~1.7k more services — proven by leg 2 (1723 services
   to `no-runnable-thread`). Chaining preserves progress but cannot
   extend a single idle stretch. Kept as tooling, rejected as the
   maturation path.
2. **Raise the budget** (adopted): 200,000 → 2,000,000, about 10x past
   the measured ~202k maturation. Idle delivery is cheap (one handler
   invocation); the only cost of a higher guard is wall-clock on truly
   stuck runs. No test depends on the old value (the 90k frontier run
   stops at its service limit, never at the budget).
3. **Faster timer rate.** Rejected without evidence: the per-frame step
   (9,600 for TIM2's clock) matches the hardware clock, and the poke
   showed the game proceeding normally once matured. The rate is not
   the wall; the guard is.

## Decision

Raise `idle_interrupt_budget` to 2,000,000 with the measurement cited
at the constant. Then run one long leg from the checkpoint and watch
the delays mature naturally: the first firing, which workers wake, and
whatever the boot does next. The delay base/time-scale puzzle (bases
near twice total elapsed ticks) stays open as Unknown — the run's
behavior decides whether it matters.

## Consequences

- If the boot advances on maturation, M31 (stable menu idle) re-enters
  scope with a pinned, higher frontier.
- If it does not fire by ~300k deliveries, the threshold math is wrong
  somewhere and slice 7 re-opens the due computation — the budget rise
  is still harmless.
- Long stuck runs take longer to report; scratch logs from them stay
  tail-only and get deleted (slice 47's lesson).

## Outcome (slice 6, 2026-10-03)

The tooling works: three chained 60k legs run to their limits and save
cleanly, COUNT advancing ~190M per leg toward the threshold (~7–8 legs
to go at that rate). The budget raise was verified live (idle counter
past 200k, legs no longer die at 11.7k) after a stale-binary false
alarm. The firing itself is still ahead — the chain files preserve
every step of advance, so slice 7 continues from `ckpt-180k.bin`
(`docs/reverse-engineering/m32-slice6-long-legs.md`).
