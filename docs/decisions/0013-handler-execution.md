# 0013 — Handler execution: no nested injections, no preemption

Status: implemented 2026-10-02 for the M30 twelfth slice
(`docs/reverse-engineering/m30-slice12-handler-execution.md`).

Implementation correction, 2026-10-09 (slice 95): the previous final-return
RUN branch restored the interrupted thread without checking a newly READY
higher-priority rival, contrary to the deferred-switch requirement below.
The corrected branch saves the exact interrupted context, marks it READY
and dispatches only for a strictly higher eligible priority after the full
handler chain finishes. It must not apply a syscall's PC +4 to an interrupt
resumption. Equal/lower/suspended controls and INTC/DMAC chains are covered
by unit regressions. Semantic `interrupt_model` is now 4; formats unchanged.
See `docs/reverse-engineering/slice95-interrupt-return-preemption.md` for
the red/green proof, boot effects and remaining clock limitations.

Context: the eleventh slice's wall was that the TIM2 handler never treated
any timer node as due. A watch at the due comparison (0x005B822C) showed the
instruction never executed, and the deferred-call state at a stop showed a
handler call stuck on the stack. Two model defects explained both:

- **Starvation**: the driver's interrupt hook ran before *every* bridge step,
  so with two queued causes (the timer's and VBlank's) the TIM2 handler got
  one instruction per delivery and was immediately preempted by the next
  queued cause; it never reached its body.
- **Mid-handler switching**: a handler's `iSignalSema`/`iWakeupThread` could
  preempt to a woken higher-priority thread through the scheduler, abandoning
  the handler's frame on the deferred stack (nothing ever resumed it), and
  the stuck frame then blocked every later injection.

Decision:

- **No nested injections.** `start_interrupt` delivers nothing while a
  handler call is active (the top of the deferred-call stack is an
  interrupt): the handler frame clears EIE, and on the EE an exception
  handler runs with EXL set, so queued causes wait for the handler's return.
- **No preemption inside a handler.** `preempt_if_outranked` returns false
  while a handler is active: a woken thread stays ready and the switch
  happens when the handler returns, where `deferred_return` re-dispatches.
  This matches the kernel's behavior of deferring thread switches out of
  exception context.

Alternatives considered:

- **Letting causes stack freely** (the previous behavior): demonstrably
  starves the first handler and abandons frames; it only looked alive because
  the injections kept the machine busy.
- **Delivering causes only between module calls**: the TIM2 handler is
  interpreted by the bridge, so "between units" is every instruction; the
  active-handler guard is the precise condition.

Consequences and limits:

- The game's TIM2 handler now runs to completion, its due nodes are processed
  and the library's callbacks signal the delay semaphores: the boot no
  longer deadlocks at the delay waits.
- The run is continuous: 1,000,000 services (33,650,798 interpreted steps)
  in about 29 seconds with the main thread running; the differential passes
  at 3,000 services with the interpreter reference at 7,554,609 instructions
  and the full state identical.
- **A handler that blocks** (a blocking service inside a handler) would
  still corrupt the interrupted thread's saved context; the model does not
  prevent it. No such case has been observed; it is a recorded limit.
- No nesting means a second pending cause (for example a higher-priority
  device interrupt) waits for the current handler, exactly as EXL forces on
  the hardware for kernel handlers.
