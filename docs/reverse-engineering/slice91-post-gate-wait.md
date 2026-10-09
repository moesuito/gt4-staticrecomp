# Slice 91: the post-gate wait — the main thread waits for a stream-job message

Date: 2026-10-09. Baseline: `main` = `c8443ed` (slice 90). No model code
changed here (exploration only). Task: identify the post-gate wait slice 90
named (services ~3.5k–10k).

## Method

Trace to 12,000 services (per-service `service N at pc`), a checkpoint at
10,000 services parsed offline (thread contexts + the stack), static
`jal`-caller scans over the reconstructed ELF (the CORE is compressed — raw
byte scans are noise), and targeted disassembly.

## The freeze, pinned

- **Thread events** (trace): 15 `CreateThread` calls, the last at service
  ~3,461 (threads 1–16 complete); six `SleepThread` calls — three early
  (~1,259/1,280/1,283, one woken by the single `WakeupThread` at ~1,303)
  and three late (**~3,644/3,679/3,715**); **no thread event after ~3,715**.
- **Checkpoint at 10k** (parsed offline): thread 1 sleeps (`ThreadWaitSleep`
  id 0) with pc 0x005ADBC8 (the SleepThread stub +4) and **ra = 0x0057689C**;
  thread 13 sleeps at the same site; thread 9 sleeps at 0x005B20D8; threads
  5–16 sit ready at their entry 0x005786F0.

## What thread 1 waits for (Confirmed)

- ra 0x0057689C is inside the game's **message receive** (0x005767E0): it
  polls the object (0x00576640), and when empty calls `SleepThread`; a
  message wakes it. The wait wrapper 0x004AF3A0 calls it when the object's
  +0x80 word is not 3.
- Thread 1's stack walk: the receive was called from 0x004AF3C4; the wait
  wrapper from the job framework's dispatch (0x004AF3E8, which calls the
  handler table entries at object+0xA8); the framework from 0x004AE4FC.
- The job object lives on **thread 1's stack** (0x01FFFE00): queue fields at
  +0x08/+0x18/+0x1C/+0x20 (empty: 0xFFFFFFFF markers), a handler table at
  **+0xA8 = 0x00688F28** (entries {arg, function}; entry 4 = the wait
  wrapper 0x004AF3A0), and callbacks into the file/stream region
  (0x004AC640, 0x004AD048, 0x004AD110, 0x004AFA88, 0x004AE254, 0x004AE39C,
  0x005726F4, 0x00108F18).
- The job was started through 0x004AE440 (framework family 0x004AE2xx–
  0x004AE4xx); the framework function 0x004AE1F8 is called from the
  **file/stream descriptor family 0x0044Dxxx** (slice 42's region) and from
  early boot code (0x0015xxxx) — the same framework serves both.
- The message module's wake-all path (0x00576704…, walking the object's
  waiter list and calling `WakeupThread` per waiter) exists but never ran for
  these objects: only one `WakeupThread` in the whole trace.

## Interpretation (labeled)

- **High confidence**: the game's main flow is blocked waiting for an
  internal **completion message from a file/stream job**; the message
  producer never fires. The reference at the same phase proceeds (its next
  step loads the movie with large disc reads); the model's RPC traffic is
  complete before ~10k and no read of that size ever happens.
- **Hypothesis (next hunt)**: the producer is the stream descriptor chain's
  completion path (0x0044Dxxx / the IOP reply or PRTS path); identifying the
  message-post function and its expected trigger is the next experiment.

## Side checks

- **Thread-id space is fine**: only 16 threads were ever created over 18.5M
  services (ids 1–16, no churn), so the semaphore-class id bug has no thread
  analogue here (item closed for this horizon).
- The model's service mix in the loop: `GetThreadId` 0x2F dominates (5,582
  of 12,000), plus semaphore ops, `ChangeThreadPriority` 0x29, SIF DMA
  0x77/−0x78 and idle 0x100.

## Next

1. Find the message-**post** function in the 0x00576xxx module (append +
   wake) and its callers; determine which producer should fire for the main
   thread's job.
2. Compare the job object's state with the reference's t=14 s state by
   meaning (does the reference's equivalent job carry a posted message or a
   completed flag?).
3. Inspect the stream descriptor chain (0x0044Dxxx) at ~10k in the model —
   where it stalls relative to the reference.
