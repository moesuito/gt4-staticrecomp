# M30 lesson — the scheduler and the kernel patches: threads the game can block on

Prepared 2026-10-04. BUILD/VERIFY: passed for slices 3 and 4; see the
[M30 slice-3 evidence](../reverse-engineering/m30-thread-scheduler.md),
[slice-4 evidence](../reverse-engineering/m30-kernel-patches.md),
and [decision 0005](../decisions/0005-thread-scheduler.md) /
[decision 0006](../decisions/0006-kernel-patches.md). EXPLAIN: this
is the worked explanation; tutoring review pending.

## Objective and motivation

Slices 1–2 built a machine that runs one thread: the boot reaches
`CreateSema` (0x40) and stops, because a single-threaded model
cannot answer `WaitSema`. The game, meanwhile, is already acting
like a multithreaded program — its init creates semaphores, and
per the public ps2sdk `thread.c` it will create the
"KernelTopThread", start it, and lower the caller's priority while
that thread loops on `WaitSema`. This arc teaches the project's
answer to code that waits: keep every thread's state explicitly,
schedule deterministically, and when the game rewrites the kernel
itself, give the rewrite an object to land on. It ends one step
short of the payoff — the scheduler unit-verified but waiting for
its first end-to-end exercise, which belongs to the timer arc
that follows.

The motivating failure is structural, not local: ignoring threads
would silently lose wakeups, because the SDK's patched wrappers
(`iWakeupThread` and friends) route wakeups through thread
management the model would not be running.

## Step 1 — carry the whole context, share only memory

`RegisterContext` holds every per-thread register file — GPRs in
both halves, the FPU file, FCR31, the accumulator, HI/LO in both
banks, the shift cache, CP0, the VU0 macro file and flags, and the
pc. `GuestState::save_registers` and `restore_registers` are the
switch primitive; memory stays shared. The lesson for later work
is in that split: registers are per-thread snapshots, memory is
the one shared truth — which is exactly why a whole-RAM digest
can later prove two engines agree.

## Step 2 — a scheduler with four states and two switch rules

Thread states follow the kernel's vocabulary:
RUN/READY/WAIT/SUSPEND/DORMANT, priorities 0–127, highest first,
creation-order ties. A switch happens in exactly two situations:
a thread blocks, or a service makes a *strictly higher* priority
thread ready. No timer preemption. Each clause has its evidence:
the strictly-higher rule comes from observing the root lower its
own priority against a new thread; the no-preemption rule is a
documented first-model choice (a compute-bound thread the real
kernel would preempt is not preempted here), revisitable with
live-emulator evidence.

`ServiceOutcome` makes the switch visible to the driver: `Handled`
(continue at pc + 4), `Switched` (the kernel restored another
thread's context — the driver must not touch the pc),
`NoRunnableThread` (the current thread blocked and nothing can
run), or `Unhandled`. The driver becomes a scheduler loop, and
the new boundary is a first-class stop, not a crash.

## Step 3 — the semaphore contract, including the aliases

The service numbers come from the public `syscallnr.h`, and so do
the negative `i*` aliases, which follow the same C ABI: Sleep
`0x32`, Wakeup `0x33`/`-0x34`, Signal `0x42`/`-0x43`, Wait
`0x44`, Poll `0x45`/`-0x46`, Create `0x40`, Delete `0x41`/`-0x49`,
and the thread family `0x20`–`0x30` with theirs. The behaviors
that matter downstream: `SignalSema` hands the semaphore to the
first waiter, else increments to `max_count`; `WaitSema` consumes
or blocks and resumes with v0 = 0; a stored wakeup makes
`SleepThread` return immediately; `WakeupThread` keeps the
kernel's documented self-wakeup defect (returns -1); cancelling
returns and clears the count. The `ee_kernel` unit tests drive
every one of these with no game data — root setup, the semaphore
contract to max, entry/argument/gp/sp of a started thread,
preemption on self-lowering, blocking and release, sleep handing
the CPU back, full status layouts, suspend/resume, and the last
thread exiting into `NoRunnableThread`.

The verified run at this point is deliberately small — four
services, then the next wall:

```text
service 0x3c at 0x001001c8
service 0x3d at 0x001001e4
service 0x40 at 0x005adca4
service 0x40 at 0x005adca4
boundary: syscall 0x005b7554 service 0x74
interpreter: 942761 instructions, state identical
```

Both semaphores of the init pass, then pc `0x005B7554`, service
`0x74` (`SetSyscall`), inside the init-TLB-functions equivalent.
The scheduler is proven by units; the boot has not created a
thread yet (the game's creation at `0x005AEA78` waits past the
patch wall).

## Step 4 — where a kernel patch lands when the host is the kernel

`0x005B7450` patches syscall `0x83` (FindAddress) to the game's
scan helper `0x005B73C8` and `0x5A` (Copy) to `0x005B7390` — the
globals at `0x658368` hold exactly these pairs — then searches
low RAM for the values it just installed, derives the table base,
and stores it at `0x658360`. A patch that rewrites kernel code
has no object to rewrite when the host *is* the kernel, so the
model supplies the objects the game can observe:

- A **synthetic syscall table** at physical `0x1000` (256
  entries) in zero-filled low RAM: every entry starts as an
  opaque token (`0x80010000 + number * 4`); `SetSyscall` writes
  the guest handler into its slot and records it. The location is
  free because the game only ever derives it by searching.
- **Patched dispatch through a return stub**: the service-table
  entry for that number becomes a dispatcher saving the caller's
  `ra` and resume address, pointing `ra` at the stub (physical
  `0x1600`, issuing the private return service `0x100`), and
  jumping to the handler — mirroring the real dispatcher's EPC
  return without executing kernel code. Works for wrapped and
  inline syscalls identically — a real survey (167 `syscall`
  words: 3 in crt0, ~140 in the SDK cluster, 24 in two small
  clusters) confirmed wrapper-style returns dominate but inline
  sites take the same path.
- **An explicit KSEG0 alias**: `0x80000000 + physical` reads and
  writes the same bytes, enabled only by the boot tool, because
  the SDK search reads KSEG0. The default stays strict.
- `Copy` (0x5A) is deliberately *not* modeled: the boot patches it
  to its own implementation before any use; a pre-patch call
  would stay a boundary.

The run that proves it patches `0x83 → 0x005B73C8` and
`0x5A → 0x005B7390`, calls the patched FindAddress twice through
the game's own scan helper (which reads KSEG0 through the alias
and finds both values, deriving base `0x80001000`), and returns
each handler through the stub — interpreter agreeing at 954,146
instructions down to the whole-RAM digest. Emulating the search
predicates natively was rejected *because* running the game's
helper verifies dispatch, return, and aliased reads together;
mapping the BIOS ROM was rejected because the search targets RAM
tables and the project is a model, not the original kernel.

## What this arc does not claim (later evidence)

- The switch rules gained a guard afterward: decision 0013's
  handler-execution work forbids nested injections and preemption
  *inside* interrupt handlers, with the switch deferred to
  `deferred_return`. The two rules taught here (switch on block,
  switch on strictly-higher-ready) still govern thread context;
  the handler-active guard constrains them from above.
- `RegisterContext` later became the snapshot unit too
  (checkpoint save/load round-trips every field named in Step 1).
  That reuse was unplanned here and is recorded as evolution, not
  as foresight.
- The real kernel saves and restores caller-saved registers
  across a patched call; the model does not — unobservable to
  o32-compliant callers, recorded as a choice. The private
  service `0x100` stays outside the ABI by construction.
- Priorities are stated as the evidence docs state them
  (0–127, highest first, creation ties); the docs do not derive
  the numbering from hardware, and neither does this lesson.

## Connection to our implementation

| Piece | File | Job |
| --- | --- | --- |
| Context switch primitive | `src/ee/state.cpp` (`save_registers`, `restore_registers`) | per-thread snapshots, shared memory |
| Scheduler + semaphores | `src/ee/kernel.cpp` (thread/semaphore tables, `dispatch`, outcomes) | deterministic cooperative switching |
| Synthetic table + patched dispatch | `src/ee/kernel.cpp` (`set_syscall`, dispatcher, stub `0x1600`/`0x100`) | guest handlers without a BIOS |
| KSEG0 alias | `src/ee/state.cpp` (`enable_kseg0_alias`, boot opt-in) | the SDK search reads |
| Diagnostics | driver fault context (`Guest fault at pc …`) | faults name their instruction |
| Service + switch behavior | `tests/unit/ee_kernel_test.cpp` | every row above, no game data |

## Understanding checkpoint

1. `WaitSema` on a zero-count semaphore stops a single-threaded
   model cold. Why can the model not answer it without threads,
   and what exactly does the scheduler add that unblocks the
   boot?
2. A service makes a strictly higher-priority thread ready. Why
   *strictly* higher — what goes wrong with higher-or-equal, and
   which unit test pins the distinction?
3. `SetSyscall` writes the guest handler into a table the model
   invented at an address the model chose. Why is that legitimate
   instead of guessing, and which alternative did the game itself
   rule out by searching?
4. The patched dispatch saves `ra` and the resume address but not
   caller-saved registers. Why is that unobservable to a
   compliant caller, and what would break it?
5. Decision 0013 later forbids switching inside interrupt
   handlers. Does that contradict this lesson's switch rules?
   Explain which layer each rule governs.
6. The KSEG0 alias is opt-in, default strict. What breaks if it
   were always on, and which evidence forced adding it at all?
