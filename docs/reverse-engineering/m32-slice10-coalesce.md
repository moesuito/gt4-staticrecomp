# M32, tenth slice — unstick deliveries: coalesce the queue

Date: 2026-10-03. Inputs: the pinned CORE and ISO; a measured 12k leg
from `ckpt-680k.bin` (temporary mix counters, since removed), the
backlog census from the checkpoint file, and a verification leg after
the fix. Ships a real model fix (decision 0025) with a unit test.

## The mix, measured (Confirmed — temp counters, since removed)

One 12k leg: 346,571 start attempts, 3,198 successes, 343,373 blocked
on an in-flight frame; 3,918 VBlanks + 3,198 timer-11s enqueued. The
backlog census: 463,198 = 255,036 VBlank + 208,162 timer-11, zero DMAC.
Every enqueue with no coalescing grows it; deliveries only dribble, so
any fresh timer interrupt waits ~140 legs behind stale frames — the
phasing miss of slice 9, quantified.

## The fix (decision 0025)

`queue_interrupt` now drops same-cause entries before pushing (hardware
status-bit semantics; entries carry no payload). DMAC completions keep
stacking (none accumulate; tests pin their counts). New permanent unit
test: same cause stays one, other causes still queue.

## Verification leg (Confirmed)

Same resume, same budget: pending **467,116 → 1**, module calls
6,719 → 11,999, steps 377k → 1.10M — the handlers run live every frame
now. Threads still wait and nodes/COMP stand (post-wrap delays still
maturing — expected); the next crossing cannot be missed on phasing.
Two self-inflicted delays are recorded so the next slices avoid them:
the first version erased one dup per enqueue (7k/leg — pointless), and
a bare `--target ee_kernel_tests` build left a stale `gt4boot.exe`
behind (re-verify the relink, always).

## Verification

- New unit test green; full gates (CTest 41/41, Python 73) on the final
  tree before commit. All probe instruments removed (grep-clean); the
  only product change is the coalescing plus its test.
