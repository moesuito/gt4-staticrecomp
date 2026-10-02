# 0005 — Threads and semaphores: a deterministic cooperative scheduler

Status: proposed 2026-10-02; implementation not started. The M30 second slice
stops at CreateSema (0x40) in the InitThread tree; this record fixes the
design before code is written.

Context: the EE kernel multiplexes threads and semaphores. The boot's
InitThread-equivalent (0x005B7310, reached from the init function 0x005B7560)
creates semaphores and, per the public ps2sdk `thread.c`, then creates the
"KernelTopThread", starts it and lowers the caller's priority; that thread
loops on WaitSema. A single-threaded model cannot answer WaitSema, and the
SDK's patched wrappers (iWakeupThread and friends) route wakeups through that
thread, so ignoring threads would silently lose wakeups.

Decision (recommended): implement a **deterministic cooperative scheduler**
inside the driver:

- One shared `GuestState` memory; each thread owns a snapshot of the
  register context (GPRs in both halves, HI/LO, the pc, the FPU file and
  FCR31, CP0, the VU0 macro file and flags — the kernel saves the VU0 state
  across thread switches).
- Thread states as the kernel's: READY, RUN, WAIT, DORMANT, with the
  documented priorities (0 highest, 127 lowest) and deterministic
  highest-priority-first dispatch; equal priorities run in creation order
  (the kernel rotates, but a fixed order is deterministic and is a recorded
  model choice).
- Switch points: a blocking kernel call (WaitSema with a zero count,
  SleepThread, and later SleepThreadUntil) and the moment a signal makes a
  higher-priority thread READY. No timer preemption in the first model.
- Semaphores as counters with the documented `ee_sema_t` fields; the kernel
  is the only writer of `count` and `wait_threads`, the model follows.
- Services: CreateSema/DeleteSema/SignalSema/WaitSema/PollSema/
  ReferSemaStatus, CreateThread/DeleteThread/StartThread/ExitThread/
  ExitDeleteThread/TerminateThread, GetThreadId/ChangeThreadPriority/
  ReferThreadStatus, SleepThread/WakeupThread.

Alternatives considered:

- **Full preemptive model with a timer source** (model the EE timer and
  switch on timer events). More faithful to hardware, but the clock is
  unobserved in the verified paths and would add a second unverifiable
  variable; rejected for the first model, revisit when a game thread is
  observed to require preemption.
- **Deferred continuations without thread states** (run a thread only when
  its semaphore is signaled). Rejected: it cannot express priorities,
  SuspendThread/ResumeThread or the kernel's status queries, all of which the
  SDK's patched wrappers use.
- **Ignore threads and run only the main thread.** Rejected: the KernelTopThread
  would never process wakeup/rotate/suspend requests, so the game's own
  thread management would silently diverge.
- **Reuse the PCSX2 thread implementation as a specification.** PCSX2 runs
  the real BIOS kernel, so its behavior is the reference, but its scheduler
  is interrupt-driven and coupled to the emulator's clock; it informs the
  model, it does not replace the decision.

Consequences:

- The driver's run loop becomes a scheduler loop; `RunOptions` and the
  boundary reporting stay, with a "no runnable thread" boundary added.
- The acceptance evidence is the existing differential harness: with the
  same service table and the same dispatch policy, `gt4boot
  --compare-interpreter` must agree past 0x005ADCA4 on the stop and the full
  state. The reference loop grows the same scheduler (written separately, as
  the current reference is).
- Fidelity limits must be recorded: without timer preemption, a compute-bound
  thread that the real kernel would preempt will not be preempted here. This
  is acceptable while the verified boot path blocks on semaphores; it is the
  first thing to revisit with live PCSX2 evidence.
- The kernel-patch services (Copy 0x5A, FindAddress 0x83, SetSyscall 0x74)
  come after this and need their own decision: in this model the host *is*
  the kernel, so a patch that rewrites kernel code has no object to rewrite;
  the model will likely record the syscall handler mappings and treat the
  code copies as guest-memory writes, which is what the game observes.
