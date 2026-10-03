# M30, forty-ninth slice — the missing event per waiter class

Date: 2026-10-03/04. Inputs: the pinned CORE and ISO. Follow-up to the
forty-eighth slice (event-starved idle, not deadlock). This slice maps
every waited semaphore to its subsystem and ranks the missing events.
Verdict: two waiter classes, both awaiting unmodeled outside stimuli —
an SIF/RPC dispatcher starved of async arrivals, and engine workers
awaiting dispatch — with controller input a live alternative for the
menu-shaped wait. No model change ships.

## The semaphore census (temporary dump, since removed)

Resuming from the 243.7M checkpoint, the stop holds **46 live
semaphores** (deletion erases; the 11M+ ids are churn, not backlog).
The 7 waited ones, all count 0 with exactly one waiter each (no
count>0 anomaly, so no scheduler bug):

| waiter thread | sema id | max/init | attr | option |
|---|---|---|---|---|
| t2 (SIF/RPC worker) | 11 | 255/0 | 0 | 0x6d2360 |
| t6 (engine worker) | 4245855 | 1/0 | 0 | 0x6d2378 |
| t11 (engine worker) | 6407847 | 1/0 | 0 | 0x6d2378 |
| t4 (engine worker) | 10388483 | 1/0 | 0x64c7d4 | 0x6d2378 |
| t18 (engine worker) | 11235727 | 1/0 | 0 | 0x6d2378 |
| t8 (engine worker) | 11235775 | 1/0 | 0x655338 | 0x6d2378 |
| t3 (engine worker) | 11482435 | 1/0 | 0x1600 | 0x6d2378 |

Five of the six late semaphores share option `0x6d2378` (one creator:
the engine worker subsystem — every waiter runs entry `0x005786f0`);
sema 11 has its own (`0x6d2360`) and is a counting semaphore
(max 255). The options point into the engine's debug-string tables.
Ten further threads sleep in `SleepThread`, including thread 1 (main).

## Reading per class

- **Thread 2 + sema 11: the SIF/RPC dispatcher is starved of async
  arrivals.** The model IOP (decision 0014) answers purely
  synchronously, so IOP→EE callbacks never arrive, the dispatcher never
  posts jobs, and its worker never wakes. Every edge points outward to
  arrivals that do not exist in the model.
- **Engine workers + late binary semaphores: dispatch starvation one
  level down.** Each waits on its own job semaphore, posted only by a
  dispatcher that itself has nothing to dispatch — no requesters run,
  no completions arrive.
- **The sleepers (incl. main): menu-shaped wait.** Ten threads parked
  in sleep with fonts laid out for a 640-wide screen and timers
  running is exactly a title/menu wait for the player — but the pad has
  no emulation at all, so "no input" and "unmodeled device" are
  indistinguishable from here.

No cycle exists anywhere: nothing waits on anything that runs. The
SIF0 stuck start bit was rechecked and cleared again (zeroed channel
registers — vestigial, as in slice 48).

## Next experiments (slice 50)

Discriminate with the smallest external stimulus first: (a) a pad
probe — answer pad reads as connected-with-no-buttons (then scripted
buttons) and watch for selective wakeups; menu advance proves
input-wait. (b) A synthesized async SIF completion — if the dispatcher
wakes workers, the drought is IOP-side. Whichever wakes the machine
names the missing event and scopes its model.

## Verification

- CTest 40/40 and Python 73 (67 run, 6 skip), green on the final tree.
- All temporary instruments removed; scratch logs deleted
  post-evidence; the 243.7M checkpoint file stays in `build/` for the
  probes. This slice ships docs only.
