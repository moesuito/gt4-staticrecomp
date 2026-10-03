# M30, forty-eighth slice — the wait-for graph: event-starved, not deadlocked

Date: 2026-10-03. Inputs: the pinned CORE and ISO. Follow-up to the
forty-seventh slice (no wall through 243M; the machine idles with
245,036 interrupts pending). Verdict: the stop is **event starvation** —
every thread waits on signals only unmodeled events could send — not a
circular deadlock. No model change ships (the waking event is
unidentified).

## The census (temporary instruments, since removed)

Resuming from the 243.7M checkpoint and running the final ~11.7k
services with counters showed, over 3,123 idle ticks:

- **3,123 handler injections, 0 WAIT-to-Ready unblocks.** VBlank
  (134,917 queued) and timer-2 compare (110,119 queued) deliver and run
  their chains; nothing ever wakes.
- **Zero signal/wakeup/release calls in the whole leg.** Nobody even
  tries: the VBlank handler's chained callback slot is empty
  (`*(0x70002060) = 0`), and its accounting path calls nothing that
  could unblock.
- The only syscalls in the leg are 11,723 patched-syscall returns —
  pure handler traffic. No thread runs guest code even once.

The same resumed leg reproduces the original ending bit-for-bit
(`no-runnable-thread 0x00001604`, identical thread table down to the
semaphore ids, identical 245,036 pending count, 243,700,000 + 11,723 =
243,711,723 services) — the large-N checkpoint proof in passing.

## The graph

- 10 threads sleep in `SleepThread` (wakeable only by wakeup/release
  calls — none issued).
- 7 threads wait on semaphores 11, 11482435, 10388483, 4245855,
  11235775, 6407847, 11235727 (wakeable only by signals to those
  semaphores — none issued).
- The two live event sources (VBlank, timer 2) are effect-free here by
  the game's own code paths.

There is no cycle: every edge points outward to a signaler that does
not run. On hardware the same handlers would run with the same
(non-)effect, so the difference must be an event source the model never
generates. Leading candidates, in order: (a) **async IOP completions**
(the model IOP of decision 0014 answers purely synchronously, so a
dispatcher awaiting arrivals sleeps forever — matching the engine
workers parked in `SleepThread`); (b) controller input (the 640-wide
font layout suggests menu/UI preparation); (c) a GS-side sync. The
stuck SIF0 CHCR (0x184, STR set) was checked and cleared: its
addresses are all zero, so no real transfer is described — vestigial,
not the stall.

## What this means for M30's end

If the missing event is input (or any outside-world stimulus), this
idle *is* the boot at rest — the M30 completion state proposed in the
roadmap, awaiting M35. If it is an unmodeled device completion, a
slice fixes the model. Slice 49 identifies the event per waiter class:
which subsystem created each waited semaphore, and what hardware event
feeds it.

## Verification

- CTest 40/40 and Python 73 (67 run, 6 skip), green on the final tree.
- All temporary instruments (wakeup/delivery counters, the queue
  census, the signal/wakeup/release logs, the `--threads` dump call)
  are removed; this slice ships docs only.
