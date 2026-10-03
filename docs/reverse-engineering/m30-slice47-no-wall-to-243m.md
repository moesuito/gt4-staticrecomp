# M30, forty-seventh slice — no wall through 243M: the machine idles

Date: 2026-10-03. Inputs: the pinned CORE and ISO. Follow-up to the
forty-sixth slice (the copy-out cursor; verified past 41.9M services).
This slice raised the budget 5x (10B steps) to find the next wall: there
is none within reach — the run ends early with the machine fully idle.

## The run

- **243,711,723 services handled** (241,845,012 module calls,
  7,119,118,045 interpreted steps), exit 0, **no fault** — ~6x past the
  previous 41.9M record.
- Boundary: **no-runnable-thread at 0x00001604**, reached at 7.36B of the
  10B-step budget: the scheduler found nothing runnable and idle
  interrupts woke nothing either, so the driver stopped early.
- Stop-time threads show the familiar healthy pattern (sleepers in the
  SleepThread stub, semaphore waiters, one ready worker), timer 2 still
  counting — but **245,036 interrupts stand pending**, piled up while
  nobody ran.

## Reading

The machine is not computing anymore: every thread is parked and even
idle delivery cannot wake work. That is either the boot at rest (all
initialization done, waiting on something external — input that does
not exist, an event that never comes) or a distributed stall (threads
waiting on each other). Telling the two apart needs the wait-for graph:
which semaphore each waiter blocks on, who signals it, and whether the
chain grounds out in a sleeper that only an unmodeled event wakes. That
analysis is slice 48's experiment.

## Housekeeping (same day)

The 10B run's per-service log outgrew the SSD's comfort zone (~14 GB of
~25 GB free); all scratch logs were deleted after extracting their
evidence (~39 GB recovered). Long runs should gain a `--quiet` mode so
the service trace stops costing gigabytes of I/O per exploration.

## Verification

- CTest 37/37 (including `ee_checkpoint`) and Python 73 (67 run, 6
  skip), green on the final tree.
- No model change in this slice; the tree holds docs only on top of the
  green checkpoint-C1 commit.
