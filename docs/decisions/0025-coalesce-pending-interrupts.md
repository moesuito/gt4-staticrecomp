# 0025 — coalesce pending interrupts (drain the 463k museum)

Date: 2026-10-03. Evidence: the slice-10 mix census below and the
verification leg after the fix.

## Context

Slice 9 left two facts: a 463,198-deep interrupt backlog (255,036
VBlank + 208,162 timer-11, counted entry by entry in `ckpt-680k.bin`)
and deliveries arriving in an early burst per leg, then a desert — so a
delay crossing can fall where no handler runs. The mix census for one
12k leg read: 346,571 start attempts, 3,198 successes, 343,373 blocked
on an in-flight frame; 3,918 VBlanks + 3,198 timer-11s enqueued against
3,198 deliveries.

## Options considered

1. **Leave the queue FIFO-unbounded.** Rejected: entries carry no
   payload (kind + number only; handlers read live state), so repeats
   add nothing. Hardware keeps one status bit per INTC cause (per DMAC
   channel for completions): a second pending instance of the same
   source is already represented. The backlog is pure model artifact —
   no hardware can hold 463k pending interrupts.
2. **Coalesce INTC causes on enqueue (adopted).** Before pushing, drop
   existing entries with the same cause. This both stops recurrence
   and sweeps the museum within the first enqueues of the next leg
   (each new VBlank/timer erases its stale dups). Delivery order across
   *different* causes is untouched; the differential reference uses the
   same kernel, so it stays green by construction.
3. **Coalesce DMAC completions too.** Rejected for now: no DMAC entries
   were found in the museum (they drain), and existing unit tests pin
   their stacking counts. Revisit with evidence if they accumulate.

## Decision

Coalesce INTC causes in `queue_interrupt`; leave `queue_dmac_completion`
stacking. Permanent unit test covers both halves (same cause stays one,
other causes still queue).

## Consequences and verification

- Verification leg from `ckpt-680k.bin`: pending **467,116 → 1**,
  module calls 6,719 → 11,999, steps nearly triple — handlers run live
  every frame instead of replaying stale ones. Threads still wait (the
  post-wrap delays still need maturation), nodes/COMP unchanged.
- First version erased only one dup per enqueue (`break` after erase)
  and drained 7k/leg — too slow to matter; corrected to erase all
  matches before pushing. A bare `--target` build also briefly hid the
  fix behind a stale `gt4boot.exe` (slice 6's lesson, repeated and
  re-learned: confirm the relink).
- Future delay crossings cannot be missed on phasing anymore: the walk
  tests live every frame. The remaining march to the band (~9–10 legs
  at the new rate) is honest waiting, and any game-side re-arm or
  cancel will now show immediately.
