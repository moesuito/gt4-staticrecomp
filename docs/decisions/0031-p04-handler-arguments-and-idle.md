# 0031 — Handler arguments and explicit interrupted-idle (P04)

Date: 2026-10-04. Status: accepted (implemented in slice 67).
Predecessors: 0028 (compatibility policy), 0029 (P01+P02
origin/pending/mask/dispatch), 0030 (P03 unified advance machine).
Plan: PLAN.md section 6, P04 (depends on P01-P03).

## Context

The P03 reprogram test ended every dispatch with a frame that carried
only the cause in a0: the registration's fourth word was stored but
never delivered (PLAN.md C10), and `current_thread_id_` kept the id of
whichever thread blocked last once nothing ran (PLAN.md C11). The
PLAN.md P04 acceptance needs per-handler arguments, an explicit
interrupted-idle context with a referenced GetThreadId value, and a
return dispatch that wakes correctly without restoring a waiter as
RUN -- while keeping the no-nesting/no-preemption policy. Merely
zeroing the current id is explicitly not an acceptable fix (PLAN.md
section 6, P04): the `deferred_return` branch must take part.

## Decision

1. **Handler and argument travel as one pair.** New
   `InterruptHandlerCall {handler, argument}` replaces the bare handler
   list on the in-flight call. `start_interrupt` copies both words
   from each matching registration in order; `install_handler_frame`
   and the chain step consume the pair. Two handlers for one cause
   keep their own arguments (P04 acceptance, pinned).
2. **a0/a1/a2 = (cause, argument, interrupted pc), by reference.**
   ps2sdk `ee/kernel/include/kernel.h` (master) prototypes the
   2-suffixed forms as `AddIntcHandler2(cause,
   handler(cause, arg, addr), next, arg)` and the same for DMAC, and
   `ee/kernel/include/syscallnr.h` gives both forms the SAME number
   (`__NR_AddIntcHandler2` is `__NR_AddIntcHandler` = 0x10, likewise
   0x12): the kernel stores a3 on every registration exactly like the
   hardware register, and the dispatcher always passes it on -- only
   `(cause, arg, addr)` handlers read it back. So a1 is the stored
   word for every registration (the plain form stores whatever the
   caller left in a3, as hardware does). a2 is the interrupted pc:
   ps2sdk's own TIM2 handler forwards its third argument into the
   alarm-callback slot the timer header names `pc_value`
   (`ee/kernel/src/timer.c` `TimerHandler_callback` into
   `ee/kernel/include/timer.h` `timer_alarm_handler_t`), which only
   fits a pc. This closes PLAN.md section 4.4 item 4 by reference
   instead of by the parameter name: no `addr` guess was needed.
3. **Interrupted-idle is explicit, and idle is thread 0.** New
   `Kernel::idle_thread_id = 0`: ps2sdk kernel.h documents that
   thread 0 is always the kernel's idle thread (MAX_THREADS comment),
   and the game's own ids start at 1. `dispatch` records the sentinel
   at its failing point -- the single choke point every block/exit
   path funnels through. `inject_interrupt` records it when the live
   context is not a RUN thread (same predicate GetThreadId uses, so
   frame and query agree by construction).
4. **GetThreadId over idle returns 0.** Naming the interrupted RUN
   thread inside a handler is unchanged; with no RUN thread the
   service names the idle thread. The game's safe-wakeup wrapper
   (0x005AEB68) compares `iGetThreadId()` against its live target, so
   idle must never look like a blocked thread -- the stale-id
   misroute behind the 181 ring posts. The wrapper's direct path
   (target never 0) now applies at idle.
5. **Return dispatch covers idle and the stopped thread.**
   `deferred_return` chains first (unchanged), then: idle sentinel
   -> dispatch, else restore the frame and report NoRunnableThread
   (staying idle, never re-executing an idle pc the driver would
   re-run as a blocking syscall); deleted thread -> restore and
   return; interrupted thread no longer RUN -> dispatch instead of
   restoring a waiter as RUN, else restore and stay idle. Handlers
   still never nest (`handler_active` unchanged) and service-level
   preemption still defers to the return (`preempt_if_outranked`
   unchanged).
6. **Versions.** `interrupt_model` 2 -> 3 (in-flight handler calls
   ride as pairs on the wire). Every pre-change checkpoint is
   forensic automatically through the 0028 gate.

## Consequences

- Four pre-existing `ee_kernel` expectations that pinned the old
  idle return (`Jumped` with no thread underneath) now pin the
  corrected contract (`NoRunnableThread` with the frame restored):
  the SIF-chain last return, the reprogram first and second returns,
  and the shared-accumulator VBlank return. Two more rows (timer-idle
  and SIF first frame) gained a1/a2 assertions with their prior
  content unchanged. Updated with evidence per the 0029 precedent,
  not silently.
- The 90,000-service disc boot with the compare-interpreter and the
  20,000-service originating pin are unchanged: the observed prefix
  registers handlers with argument 0 and the new idle value does not
  move the pinned frontier points.
- Out of scope, unchanged: JR/ERET (P05), DMA payload/tags (P06), RPC
  content (P07), quanta/clock (P03 done), handler return values (v0
  stays unmodeled -- no reference found that the game consumes it).

## Verification

Extended fixture `ee_kernel` in place (no parallel harness): chained
INTC pair frames with restore, DMAC pair frame, RUN GetThreadId,
idle sentinel + GetThreadId 0 inside the handler, wake-then-dispatch
with waiter untouched, quiet-idle stay with statuses pinned, live
chain snapshot round-trip with per-argument continuation. CTest 50/50
(includes `gt4boot_services` 90,000 with disc and the
compare-interpreter differential, plus all resume/autosave fixtures);
Python 73 collected, 67 run, 6 skip, exit 0 with the two known socket
ResourceWarnings. Evidence:
`docs/reverse-engineering/slice67-p04-handler-idle.md`.
