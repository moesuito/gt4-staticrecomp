# M30, third slice — the thread scheduler and the semaphore services

Date: 2026-10-02. Inputs: the pinned CORE. Follow-up to the second slice
(`m30-bios-services-and-bridge.md`): the boot stopped at CreateSema (0x40)
because a single-threaded model cannot answer WaitSema. This slice implements
the deterministic cooperative scheduler of
`docs/decisions/0005-thread-scheduler.md`.

## What changed

- **Thread contexts** (`ee_state.hpp/cpp`): `RegisterContext` holds every
  per-thread register file — GPRs in both halves, the FPU file, FCR31, the
  accumulator, HI/LO in both banks, the shift cache, CP0, the VU0 macro file
  and flags, and the pc. `GuestState::save_registers` and
  `restore_registers` are the switch primitive; memory stays shared.
- **The kernel model** (`ee_kernel.hpp/cpp`): thread and semaphore tables,
  thread states (RUN/READY/WAIT/SUSPEND/DORMANT), priorities 0-127, and the
  deterministic scheduler: highest priority first; equal priorities dispatch
  in creation order; a switch happens when a thread blocks, or when a
  service makes a **strictly higher** priority thread ready. No timer
  preemption (decision 0005).
- **`ServiceOutcome`**: a handler now reports what happened — `Handled`
  (continue at pc + 4), `Switched` (the kernel restored another thread's
  context; the driver must not touch the pc), `NoRunnableThread` (the current
  thread blocked and nothing can run), or `Unhandled`.
- **The service set** (numbers from the public `syscallnr.h`; negative
  aliases follow the same C ABI):
  | Service | Number | Model |
  | --- | --- | --- |
  | SetupThread | 0x3C | creates the root thread (id 1, priority 0), returns the stack top |
  | CreateThread | 0x20 | reads `ee_thread_t`, validates, returns a new id (dormant) |
  | DeleteThread | 0x21 | dormant threads only |
  | StartThread | 0x22 | builds the entry context (a0 = argument, gp, sp = region top, CP0 inherited), then may preempt |
  | ExitThread / ExitDeleteThread | 0x23 / 0x24 | dormant (and removed), dispatch |
  | TerminateThread | 0x25 / -0x26 | any live state to dormant, releases a semaphore wait |
  | ChangeThreadPriority | 0x29 / -0x2A | update, then preempt when outranked |
  | RotateThreadReadyQueue | 0x2B / -0x2C | accepted no-op (creation order is already deterministic) |
  | ReleaseWaitThread | 0x2D / -0x2E | releases a waiter without signaling |
  | GetThreadId | 0x2F / -0x2F | the running thread's id |
  | ReferThreadStatus | 0x30 / -0x31 | writes the public 0x30-byte layout |
  | SleepThread | 0x32 | blocks; a stored wakeup returns immediately |
  | WakeupThread | 0x33 / -0x34 | wakes a sleeper, counts otherwise; the documented self-wakeup defect returns -1 |
  | CancelWakeupThread | 0x35 / -0x36 | clears and returns the count |
  | SuspendThread / ResumeThread | 0x37 / -0x38, 0x39 / -0x3A | the SUSPEND bit gates readiness |
  | CreateSema | 0x40 | reads `ee_sema_t`, mirrors count and wait_threads back |
  | DeleteSema | 0x41 / -0x49 | idle semaphores only |
  | SignalSema | 0x42 / -0x43 | hands the semaphore to the first waiter, else increments to max_count |
  | WaitSema | 0x44 | consumes, or blocks and resumes with v0 = 0 |
  | PollSema | 0x45 / -0x46 | consumes or returns -1 |
  | ReferSemaStatus | 0x47 / -0x48 | writes the public semaphore layout |

## The verified run

```
build/gt4boot.exe private/fingerprint-check/CORE.GT4 --services 4 --compare-interpreter
service 0x3c at 0x001001c8
service 0x3d at 0x001001e4
service 0x40 at 0x005adca4
service 0x40 at 0x005adca4
boundary: syscall 0x005b7554 service 0x74
stats: module calls 4, interpreted steps 24, services handled 4
interpreter: 942761 instructions, state identical (registers, HI/LO, FPU, VU0, CP0, pc, memory digest)
```

- The boot now passes **both semaphores of the InitThread-equivalent**
  (0x005B7310 calls CreateSema twice, storing the ids at 0x658378 and
  0x65837C) and continues into the next init function.
- It stops at the **kernel-patch wall**: pc 0x005B7554, service **0x74
  (SetSyscall)**, inside the InitTLBFunctions-equivalent at 0x005B7450.
- The differential reference reaches the same stop after **942,761
  instructions** with every compared field identical (all registers, FPU,
  VU0, CP0, pc, whole-RAM digest).
- `gt4boot_services` now pins this frontier (`--services 4`).

## Evidence for the kernel model

- `ee_kernel` unit tests (no game data) drive every service and check the
  register contexts across switches: SetupThread's root; the semaphore
  contract (create/poll/signal to max/refer/delete, the mirrored structure
  fields); the started thread's entry/argument/gp/sp; the preemption when
  the root lowers its own priority; WaitSema blocking and SignalSema
  release; SleepThread handing the CPU back; ReferThreadStatus's full
  layout; suspend/resume; and NoRunnableThread when the last thread exits.
- The service numbers, argument registers and structure layouts come from
  the public ps2sdk (`kernel.h`, `syscallnr.h`, `thread.c`, `initsys.c`);
  the game's own code matches them (the disassembly of 0x005B7310 and
  0x005AEA78 uses the same `ee_sema_t`/`ee_thread_t` fields the SDK passes).
- **The scheduler is not yet exercised by the boot run**: the thread
  creation in the game's init (0x005AEA78) comes after the kernel-patch
  wall, so the boot stops before its first CreateThread. The unit tests are
  its evidence until the patch services let the boot reach it.

## Model choices recorded

- The root thread starts at priority 0 (the ABI passes none); the SDK's own
  `ChangeThreadPriority(GetThreadId(), 1)` then orders it against the
  KernelTopThread exactly as on hardware.
- Thread ids start at 1 (thread 0 is the idle thread in the public
  description) and are not reused; a failed creation does not consume one.
  Semaphore ids start at 1 too.
- A started thread inherits CP0 from the running thread and starts with an
  otherwise empty context; its stack pointer is the provided region's top
  aligned to 16 bytes.
- `RotateThreadReadyQueue` is a no-op: dispatch order is creation order, a
  documented deterministic substitute for the kernel's rotation.
- Self-suspend dispatches immediately (the raw kernel's variant leaves the
  current thread id unchanged; the SDK's patched wrapper avoids the
  difference).
- Error returns are -1 (0xFFFFFFFF in v0), matching the ABI's negative
  error codes.

## The next wall: the kernel-patch services

- 0x005B7450 is the SDK's TLB-patch initializer: it calls `SetSyscall
  (0x74)` twice with handler values from globals at 0x658368, then searches
  the kernel range [0x80000000, 0x80080000) through `FindAddress (0x83)`
  with data tables at 0x005B73C8/0x005B7390, and stores a derived address
  table at 0x658360 (read back later by 0x005B7418 to copy four bytes at a
  time).
- Modeling these needs its own decision: in this model the host **is** the
  kernel, so a patch that rewrites kernel code has no object to rewrite.
  The likely shape is to record the syscall handler mappings and jump to
  guest handlers on patched syscalls, but the search semantics
  (`FindAddress` over the BIOS range) and the expected return values must be
  settled first, with the disassembly as evidence.

## Limits recorded

- No timer preemption: a compute-bound thread the real kernel would preempt
  is not preempted here. The verified boot path blocks on semaphores, which
  the cooperative model answers; this is the first thing to revisit with
  live PCSX2 evidence (decision 0005).
- Equal-priority scheduling is creation order, not the kernel's rotation;
  recorded as a model choice.
- The whole-program module still cannot be interrupted inside a module call;
  the work budget counts interpreted instructions and module calls.

## Evidence

- CTest **30/30**: the new `ee_kernel` unit test plus the existing suite;
  `gt4boot_services` now runs with `--services 4` and requires the
  interpreter comparison to pass.
- Python 73 collected (67 run, 6 skip) — unchanged.
