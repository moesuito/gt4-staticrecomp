# M32, fifth slice — a delay node fires: the due comparison is the gate

Date: 2026-10-03. Inputs: the pinned CORE and ISO, resumed from the
243.7M checkpoint. Temporary `--poke-count` plus signal/wakeup logging
(all since removed) tested the TIM2 due comparison empirically. The
comparison is now the proven gate; a delay node fired and a worker's
completion arrived.

## The exact gate (Confirmed — code reading + live values)

The TIM2 handler `0x005B8158` tests each node at `0x005B822C` with
`sltu current, target` and **leaves the whole walk on the first
not-due node** (not just that node):

- `current = ((overflow@0x006592F0 << 16) | TIM2_COUNT) << ((MODE&3)*4)`
- `target = scheduled + base - accumulated` (64-bit)
- At the stop: overflow = 95, MODE = `0x782` (shift 8),
  COUNT = `0x89e863c0`, head node `0x0088a000` target =
  `0x24000 + 0xFFFFFC0000` = 1,099,507,087,360.
- The game arms TIM2 COMP from the head node: COMP `0xfffffe40` is
  exactly `target >> 8`. The library and the timer agree on the
  mechanism; only time is missing.

## The threshold experiment (Confirmed)

Two poked legs, same resume, same budget:

- `+0x73800000` (COUNT `0xFF98E500`): **bit-identical to baseline**
  (6,563 module calls, 368,172 steps, idle at 11,723). Below threshold.
- `+0x73E70000` (COUNT `0xFFFFE500`): **the machine moved** (+156 module
  calls, 376,897 steps, ran to the 12,000 limit). Exactly one signal in
  the log — `iSignalSema(11482435)` (the `0x42`-family stub at
  `0x005adcd4`) from handler context — thread 3 readied (status `0x1`,
  wait cleared), COMP reprogrammed to `0x240`, COUNT rewritten by game
  code to `0xffb75440` (re-arming).

One signal, the right semaphore, the waiter woken, the timer re-armed:
a delay node fired. Below threshold nothing happens; above it the
machinery runs. The due comparison gates the whole parked boot.

## What this means (high confidence)

- The six waits of slice 4 end when TIM2 COUNT matures ~1.9e9 ticks
  past the checkpoint (threshold between the two pokes). Natural
  accumulation runs ~0.59e9 per 12k leg, so ~3–4 chained legs
  (checkpoint each leg end, resume) reach it with **no model change**.
- The fired node's flags still read 3: the reschedule path rearmed it
  (one-shot would clear bit 1). Which of the six fired and the exact
  re-arm semantics stay open for slice 6.
- Thread 3 sits ready-but-unscheduled (the idle spin never yields —
  the structural property of slice 3, not a new gap; hardware would
  dispatch on the interrupt return).

## Open for slice 6

- Attribute the firing precisely (which node, which descriptor callback,
  where COMP `0x240` comes from) with one traced leg.
- Resolve the accumulation puzzle honestly: legs advance COUNT ~0.59e9
  while the idle budget implies up to ~1.9e9 — why legs end when they
  do — then pick the legitimate maturation path (chained checkpoints
  vs. budget) in decision 0024. No rate guessing before that.

## Verification

- Threshold response is binary and exact (identical vs. moved) on
  otherwise identical legs.
- All probe instruments removed (grep-clean); the checkpoint file is
  untouched (probes only resumed from it).
- Full gates run on the final tree before commit.
