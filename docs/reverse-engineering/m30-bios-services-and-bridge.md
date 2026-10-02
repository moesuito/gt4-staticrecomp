# M30, second slice — the BIOS service layer and the interpreter bridge

Date: 2026-10-02. Inputs: the pinned CORE. Follow-up to the first slice
(`m30-driver-first-slice.md`): the driver could run a module to a boundary,
but it could not resolve one. This slice adds the service layer, the bridge
that carries execution across boundaries the module cannot pass, and the
first two BIOS services — verified differentially on the whole game as one
module.

## What changed

- **The service layer** (`include/gt4recomp/ee_services.hpp`,
  `src/ee/services.cpp`): `ServiceTable` maps the number in v1 to a handler;
  an unregistered number stays a boundary. Three services are modeled:
  - **SetupThread (0x3C)**: returns the thread's stack pointer in v0, which
    ps2sdk's crt0 stores into sp.
  - **SetupHeap (0x3D)**: validates the heap request (start address mapped,
    nonzero size; -1 means "to the end of memory").
  - **FlushCache (0x64)**: no-op, like the reference's CACHE hint.
- **The bridge** (`src/ee/driver.cpp`): the driver still runs the module
  entry that owns the pc. When the module stops at a boundary it cannot pass,
  the driver resolves it:
  - a **syscall** with a registered handler runs the handler and continues at
    pc + 4;
  - a **jr-ra return**, an **unknown indirect transfer** and an **eret** hand
    control to the step-by-step interpreter, which continues until the next
    module entry — the interpreter is the reference every translated module
    was verified against, so mixing them is sound;
  - everything else (an unmodeled word, a trap, a break, an unregistered
    service, an unmapped pc) is the reported boundary.
- **The generated module exposes its entry table** as public
  `translated::has_entry` / `translated::call_entry` (the internal dispatch
  already had them as `detail::`); the driver's `Module` uses them directly,
  so a 15,000-entry module needs no linear table.
- **`gt4boot`** (`tools/gt4boot/main.cpp`): runs the whole game as one module
  under the driver, from the ELF entry, with `--services N` and
  `--compare-interpreter`. The module is generated into the ignored build
  tree from the pinned CORE; the `gt4boot_build` CTest fixture builds it on
  demand (140 s generation + compile + link here, 84.5 MB executable).

## The verified run

```
build/gt4boot.exe private/fingerprint-check/CORE.GT4 --services 2 --compare-interpreter
service 0x3c at 0x001001c8
service 0x3d at 0x001001e4
boundary: syscall 0x005adca4 service 0x40
stats: module calls 2, interpreted steps 9, services handled 2
interpreter: 942726 instructions, state identical (registers, HI/LO, FPU, VU0, CP0, pc, memory digest)
```

- **SetupThread** runs at 0x001001C8 (the crt0's first syscall) and
  **SetupHeap** at 0x001001E4 (the second); the driver then runs translated
  code again: the startup function and the init function 0x005B7560 are the
  two module calls, and the bridge interprets the nine instructions between
  them.
- The run stops at the **third** syscall: pc 0x005ADCA4, service **0x40**
  (CreateSema), inside the init tree. With `--services 2` the driver stops
  before handling it; with no handler registered it stops there anyway.
- The interpreter reference walks the same path in 942,726 instructions and
  reaches the same stop with the **whole state identical**: all GPRs in both
  halves, all FPU registers and FCR31, the accumulator, HI/LO in both banks,
  the shift cache, all 32 CP0 registers, the whole VU0 file (VF lanes, VI,
  accumulator and flags), the pc, and an FNV-1a digest of the full 32 MiB of
  RAM.
- The first syscall now arrives after **942,726 interpreted instructions**
  instead of 942,695 because the reference run handles the two services and
  continues (the count is reported by the reference loop, not by the module).

## Service semantics: evidence

- **SetupThread**: the prototype is public ps2sdk
  (`void *SetupThread(void *gp, void *stack, s32 stack_size, void *args,
  void *root_func)`), and the pinned game's crt0 matches the public crt0
  source line for line: it passes gp, stack, size, args and the ExitThread
  stub at 0x00100228 (a `v1 = 0x23` thunk), then stores v0 into sp. The
  menu RAM dump preserves the boot stack at the top of the region the crt0
  provided ([0x1FF8000, 0x2000000)): the thread control block at
  0x1FFFF20..0x1FFFF53 holds gp = 0x6DDDF0 and the ExitThread root pointer,
  and boot frames at 0x1FFFD38..0x1FFFD58 hold return addresses into the
  crt0 (0x00100218). The model returns the region top aligned down to 16
  bytes; the exact offset below the top that the BIOS applies is not
  observable from this dump. **High confidence** on the contract and the
  region; the offset is unobserved.
- **SetupHeap**: public prototype (`void SetupHeap(void *heap_start, s32
  heap_size)`); the crt0 passes the end of .bss and -1. The model validates
  and records nothing: no verified path reads the kernel heap structure
  back. **High confidence** on the contract; the structure itself is
  **Unknown**.
- **FlushCache**: registered as a no-op following the established cache
  policy; the verified run stopped before reaching it (the crt0 calls it at
  0x001001F0 only after the init function returns).

## The next wall: the thread and semaphore scheduler

- The stop is at 0x005ADCA4, the CreateSema wrapper (0x005ADCA0 sets
  v1 = 0x40). Its caller is 0x005B7310 (called from the init function
  0x005B7560): it builds two semaphore structures on the stack (with option
  pointers into the data segment) and stores the returned IDs at 0x658378
  and 0x65837C.
- The public ps2sdk `InitThread` source shows the full shape: it creates a
  semaphore, creates the "KernelTopThread" (`CreateThread`, `StartThread`,
  `ChangeThreadPriority`, `GetThreadId`), and that thread waits on the
  semaphore (`WaitSema`) to process wakeup/rotate/suspend requests. The
  patched wrappers route through it with `iSignalSema`.
- **Implementing these services means implementing a cooperative EE thread
  scheduler** — dispatch, priorities, wait states and semaphores — because
  a single-threaded model cannot answer WaitSema. That is the next
  milestone-sized design decision; the run above stops exactly at its first
  call.
- The same init tree also reaches the kernel-patch services (Copy 0x5A,
  FindAddress 0x83, SetSyscall 0x74); their role in a model that *is* the
  kernel needs its own decision.

## Limits recorded

- A module call runs to its own boundary and cannot be interrupted; the work
  budget counts interpreted instructions and module calls, so a loop inside
  a module is not bounded by it. No such loop has been hit before a
  boundary.
- The bridge interprets the gaps between module entries (nine instructions
  in this run); it is the correctness-first path. The two performance
  alternatives recorded in decision 0004 (resume entries per halt address,
  or inline syscall calls in generated code) remain open.
- The `pc == ra` return inference and the ordinary-word stop classification
  are unchanged from the first slice; the interpreter path now uses the
  step outcome (`boundary_from_step`), so it is exact where the word-based
  classification would have to infer.

## Evidence

- CTest **29/29**: `ee_driver` (unit, no game data: entries, services, the
  bridge, every boundary kind and the work budget), `gt4run_startup` (the
  first-slice CLI), `gt4boot_build` + `gt4boot_services` (the whole-program
  run above, built on demand by the fixture).
- Python 73 collected (67 run, 6 skip) — the generated header gained the
  public entry-table wrappers; the CLI tests are unaffected.
- The differential comparison is independent evidence: the module and the
  interpreter are different implementations; with the same service table and
  limit they agree on the stop and every compared field.

## Next

1. **The thread and semaphore scheduler** (decision pending): the services
   InitThread uses, modeled from the public ABI with a deterministic
   cooperative scheduler; the acceptance evidence is this same differential
   harness, extended past 0x005ADCA4.
2. The kernel-patch services (FindAddress/Copy/SetSyscall) and what they
   mean when the model is the kernel.
3. Performance: resume entries or inline syscall calls to shrink the
   interpreted gaps.
