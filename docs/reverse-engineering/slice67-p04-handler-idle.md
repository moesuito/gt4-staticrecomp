# Slice 67 — P04: handler arguments and explicit interrupted-idle

Date: 2026-10-04. Plan: PLAN.md section 6, P04 (slice 67 = P04).
Decision: docs/decisions/0031-p04-handler-arguments-and-idle.md.
Baseline: main ca43cad (P03 committed), 50/50 CTest + Python 73 (6 skips).

## References (all read, none assumed)

- ps2sdk `ee/kernel/include/kernel.h` (master): `AddIntcHandler(cause,
  handler(cause), next)` vs `AddIntcHandler2(cause,
  handler(cause, arg, addr), next, arg)`; same pair for DMAC;
  `MAX_THREADS` comment ("Thread 0 is always the idle thread");
  `TH_SELF 0`; `_iGetThreadId` ("used for a hack by SCE to work
  around the iWakeupThread design flaw").
- ps2sdk `ee/kernel/include/syscallnr.h` (master):
  `__NR_AddIntcHandler2` is `__NR_AddIntcHandler` (0x10);
  `__NR_AddDmacHandler2` is `__NR_AddDmacHandler` (0x12). Both forms
  share the number, so no boundary can tell them apart -- the kernel
  stores a3 either way, exactly like hardware.
- ps2sdk `ee/kernel/src/timer.c` (master): `TimerHandler_callback(s32
  cause, void *arg, void *addr)`, registered via `AddIntcHandler2`
  with arg NULL, forwards its own `addr` unchanged into the alarm
  callback.
- ps2sdk `ee/kernel/include/timer.h` (master):
  `timer_alarm_handler_t(id, scheduled_time, actual_time, arg,
  pc_value)` -- the slot that receives the forwarded `addr` is named
  `pc_value`. Chain: BIOS a2 -> handler `addr` -> alarm `pc_value`.
  This is the reference that closes PLAN.md section 4.4 item 4: a2 is
  the interrupted pc, established by the forwarding chain, not by the
  parameter name alone.
- ps2sdk `ee/kernel/src/kernel.S`: thin syscall wrappers (no dispatch
  logic; the call magnitudes above are the whole reference surface).
- In-repo: PLAN.md C10/C11 and section 4.4 items 4-5; OPUS_FEEDBACK.md
  sections 1.2-1.3 and item 266-269 (the a1/a2/idle claims, rechecked
  above instead of trusted); feedback-triage-2026-10-04.md item 11.

## What changed (files and lines, approximate)

- include/gt4recomp/ee_kernel.hpp: new `InterruptHandlerCall
  {handler, argument}` unit; `DeferredCall::handlers` rides pairs;
  `inject_interrupt`/`install_handler_frame` take the pair; new public
  `idle_thread_id = 0` with the thread-0 reference; `dispatch`,
  `deliver_idle_interrupt`, `start_interrupt` comments name the
  explicit-idle reading.
- src/ee/kernel.cpp: `dispatch` records the idle sentinel on the
  failing path (the single choke point, not an isolated zeroing);
  `get_thread_id` answers the RUN thread or 0; `add_intc/dmac_handler`
  comments record the shared-number storage rule; `start_interrupt`
  builds ordered pairs; `inject_interrupt` records idle unless a RUN
  thread is underneath; `install_handler_frame` sets a0/a1/a2 =
  (cause, argument, interrupted pc) with the per-slot reference;
  `deferred_return` chains, then idle -> dispatch-or-stay,
  deleted -> restore, stopped -> dispatch-or-stay, running ->
  restore; save/load write handler pairs.
- include/gt4recomp/ee_checkpoint.hpp: `interrupt_model` 2 -> 3 with
  the slice note. Pre-change checkpoints refuse through the 0028 gate.
- tests/unit/ee_kernel_test.cpp: four old idle-return rows now expect
  `NoRunnableThread` with the frame restored; timer-idle and SIF first
  frames gain a1/a2 assertions; new P04 rows (chained pair frames with
  restore, DMAC pair frame, RUN GetThreadId, idle sentinel, GetThreadId
  0 in-handler, wake-then-dispatch, quiet-idle stay, live-chain
  snapshot round-trip). Extended in place (no parallel harness).

## Behavior deltas vs the old code (all deterministic, both engines)

1. Handlers observe a1 = their registration's fourth word (was 0) and
   a2 = the interrupted pc (was 0). Registrations with argument 0 --
   every handler in the observed 90k boot prefix -- see no change.
2. `current_thread_id_` becomes 0 once nothing runs (was the last
   blocker's id); GetThreadId at idle answers 0 (was the stale id).
   The game's safe-wakeup wrapper now takes its direct path at idle
   instead of deferring through thread 2's ring.
3. Idle handler returns with nothing woken answer NoRunnableThread
   with the frame restored (were Jumped without a thread, or
   NoRunnableThread with handler-frame residue). The driver keeps
   pumping idle instead of re-executing an idle pc.
4. The snapshot wire for in-flight calls carries pairs; file-level
   `interrupt_model` 3 refuses older files before parsing.

## Test results and counts

- ee_kernel: all pre-existing rows pass (4 re-pinned to the corrected
  idle return, 2 extended with a1/a2); new P04 rows pass (chain,
  DMAC, RUN/idle GetThreadId, wake dispatch, quiet idle, chain
  snapshot). Pass (~9.4 s).
- ee_checkpoint, ee_device, ee_timer and the full unit set: unchanged,
  pass.
- Full runs: CTest 50/50 (gt4boot_build rebuilds the whole program;
  `gt4boot_services` 90,000 with disc + compare-interpreter green in
  ~16 s; `gt4boot_originating` 20k pin byte-identical, including
  thread-2 wait state; all resume/autosave fixtures green on the new
  wire order with fresh files); Python 73 collected, 67 run, 6 skip,
  exit 0, with the two known socket ResourceWarnings.
- Build warning-free (MSVC 19.44, Ninja, Debug). New code comments in
  pure ASCII (added-lines scan: 0 non-ASCII lines; remaining hits in
  these files are pre-existing).

## P04 acceptance (PLAN.md section 6, P04)

- Two handlers with different arguments receive their arguments:
  covered (INTC chain row asserts each frame's a1; DMAC row covers the
  second list).
- Handler in idle wakes a thread and the return chooses the ready one:
  covered (wakeup row: Jumped, worker RUN at its resume pc, waiter
  untouched, current = worker).
- Nobody woken means correctly idle: covered (NoRunnableThread, frame
  pc restored, both WAIT, current 0).
- No accidental WAIT-as-RUN restore: covered (both idle rows assert
  statuses; the stopped-thread branch dispatches instead of
  restoring).
- The interrupted frame is preserved: covered (RUN-chain restore
  asserts distinctive a1/a2 survivors; idle rows assert the pc).
- No isolated `current_thread_id_ = 0`: the sentinel is recorded in
  `dispatch`, read in `inject_interrupt`, `get_thread_id` and the
  `deferred_return` idle branch -- each row above exercises one side.
- Non-nesting/non-preemption preserved: `handler_active` and
  `preempt_if_outranked` untouched; the chain still refuses injection
  while live (existing rows).

## Out of scope, unchanged (verified by green suites)

JR/ERET (P05), DMA payload/tags (P06), RPC content (P07), quanta and
the service clock (P03 done), handler return values (v0 unmodeled --
no reference found that the game consumes it), the pending/mask/CP0
contracts (P01+P02 done).

## What P05 inherits concretely

The return-path outcomes are now explicit per context (Jumped over
RUN, dispatch-or-NoRunnableThread over idle/stopped), which is the
seam P05's explicit AOT exit reasons build on: the driver already
branches on Jumped vs NoRunnableThread at exactly these points. The
whole-program 90k boot still parks at the known
`syscall 0x00001604 service 0x100` boundary with the differential
green, so P05 starts from a stable, correctly-idling machine.
