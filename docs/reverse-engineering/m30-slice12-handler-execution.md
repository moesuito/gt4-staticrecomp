# M30, twelfth slice — handler execution and the unblocked runtime

Date: 2026-10-02. Inputs: the pinned CORE. Follow-up to the eleventh slice
(`m30-slice11-timer-library-nodes.md`), whose wall was the TIM2 handler
never treating any timer node as due. This slice finds two model defects in
how injected handlers execute, fixes both (decision 0013), and unblocks the
game's delay machinery: the boot now runs **continuously** — 1,000,000
services, 33,650,798 interpreted steps, about 29 seconds — with the
differential passing at 3,000 services (interpreter reference at 7,554,609
instructions, full state identical).

## The measurements

- A watch at the due comparison (0x005B822C) in the driver never fired: the
  TIM2 handler's loop body never executed, although the handler was delivered
  ~4,700 times per 8,000-service run.
- The deferred-call state at a stop: `deferred calls: 1` with four causes
  queued — one handler call stuck on the stack forever.
- The trace's last services before that stop: a handler return (`0x100`),
  then the RPC thread's `iGetThreadId`/`iWakeupThread`/`SleepThread` loop,
  then a `NoRunnableThread` boundary where the idle hook had nothing to do
  because a stale handler call blocked delivery.

## The two defects

1. **Starvation by queued causes.** The driver's interrupt hook runs at the
   top of every bridge step. With the timer's cause (11) and VBlank (2)
   queued in one idle event, the TIM2 handler was injected, executed *one*
   instruction, and was preempted by the queued VBlank cause at the next
   step — repeatedly, so it never reached its body.
2. **Mid-handler thread switches.** Inside a handler, `iSignalSema` /
   `iWakeupThread` called the scheduler's preemption and switched to the
   woken thread, abandoning the handler's frame on the deferred stack. The
   frame was never resumed, and once a stale handler call was on top, the
   new no-nesting guard blocked all further injections.

## The fix

- `start_interrupt` refuses to inject while a handler call is active; the
  queued causes wait for the handler's return.
- `preempt_if_outranked` refuses to switch while a handler is active; the
  woken thread stays ready and the switch happens in `deferred_return`.
- `deliver_idle_interrupt` only counts a delivery when one happened.
- The new `--threads` diagnostic prints the handler tables and the
  deferred-call/pending-cause counts.

## The result

```
--services 1000000 --threads        (29 s)
stats: module calls 1197251, interpreted steps 33650798, services handled 1000000
thread 1: status 0x1 (running), wait 0/0, prio 64
thread 2: status 0x4, wait 2/11 (the RPC thread's queue)
thread 3: status 0x2 (ready), prio 0
timer 2: count 0x49507500, comp 0x00000000
```

- The TIM2 handler runs to completion every idle frame; the watch shows the
  active node's base time advancing and its due comparison passing.
- The delay callbacks fire: the service mix at the tail is the delay
  helper's create/wait/signal/delete cycles (0x40/0x41/0x44) plus the RPC
  thread's wakeup checks — the game's own wait machinery now works.
- The game runs without stalling: no no-runnable-thread boundary inside the
  million services.

## The next frontier

The game now spends its services inside library wait/retry loops (semaphore
ids climb into the thousands; the main thread is running). The next slice
should characterize what those loops wait for — most likely the RPC replies
the model IOP still answers with empty results (the loading path) — and
answer the first call whose reply the game acts on. The run's boundaries are
now the service limit, not a deadlock.

## Verification

- CTest **32/32** (`ee_kernel` covers the non-nesting guard and the deferred
  switch behavior); Python 73 (67 run, 6 skip).
- `gt4boot --compare-interpreter` at 3,000 services: interpreter reference
  at 7,554,609 instructions, full state identical (registers, HI/LO, FPU,
  VU0, CP0, pc, memory digest).
