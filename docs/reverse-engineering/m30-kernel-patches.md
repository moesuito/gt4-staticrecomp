# M30, fourth slice — the kernel-patch services

Date: 2026-10-02. Inputs: the pinned CORE. Follow-up to the third slice
(`m30-thread-scheduler.md`): the boot stopped at SetSyscall (0x74), the
SDK's kernel-patch wall. This slice models it with the design of
`docs/decisions/0006-kernel-patches.md`.

## What changed

- **`SetSyscall` (0x74)** in the kernel: records the patch, writes the guest
  handler into the synthetic syscall table at physical 0x1000 (the
  guest-visible view the SDK searches for), and replaces the service
  table's entry for that number with a dispatcher to the guest handler.
- **The patched dispatch and return**: the dispatcher saves the caller's
  `ra` and the resume address, points `ra` at a return stub (physical
  0x1600, which issues the model's private return service 0x100), and jumps
  to the handler. The return restores `ra` and resumes after the syscall,
  mirroring the real kernel dispatcher's EPC return.
- **The KSEG0 alias** (`GuestMemory::enable_kseg0_alias`): opt-in mapping of
  0x80000000 + physical to the same bytes, bounded by the region. The boot
  tool enables it because the SDK's table search reads KSEG0. The default
  stays strict.
- **`ServiceOutcome::Jumped`**: a handler that set `pc` itself (patched
  dispatch and the return stub).
- **Driver diagnostics**: a guest access fault now reports the pc of the
  instruction (or the translated entry) it happened at, e.g. `Guest fault
  at pc 0x005b7a40: Guest access at 0x10001810 ...`.

## The verified run

```
build/gt4boot.exe private/fingerprint-check/CORE.GT4 --services 9 --compare-interpreter
service 0x3c at 0x001001c8
service 0x3d at 0x001001e4
service 0x40 at 0x005adca4
service 0x40 at 0x005adca4
service 0x74 at 0x005b7554
service 0x74 at 0x005b7554
service 0x83 at 0x005b740c
service 0x100 at 0x00001604
service 0x83 at 0x005b740c
boundary: syscall 0x00001604 service 0x100
stats: module calls 7, interpreted steps 11403, services handled 9
interpreter: 954146 instructions, state identical (registers, HI/LO, FPU, VU0, CP0, pc, memory digest)
```

- The boot patches **FindAddress (0x83) → 0x005B73C8** and **Copy (0x5A) →
  0x005B7390** (the handler values live at 0x658368), then calls the patched
  FindAddress twice. Both calls land in the game's own scan helper, which
  reads KSEG0 through the alias and finds the two installed handler values
  in the synthetic table (the base it derives is 0x80001000, stored at
  0x658360). Each handler returns through the stub.
- The differential reference reaches the same stop after **954,146
  instructions** with every compared field identical — registers, FPU, VU0,
  CP0, pc and the whole-RAM digest. The stop is the second stub return,
  which the `--services 9` limit leaves unhandled: a clean checkpoint for
  the whole mechanism.
- `gt4boot_services` now pins this frontier.

## The next wall: the EE timer hardware

- With the patch services modeled, the boot continues past 0x005B7450 and
  reaches the next init function, **0x005B7A40**, which starts with
  `lw v1, 0(0x10001810)` — a read of **TIM3_MODE** (EE timer 3, base
  0x10001800). The model maps no hardware registers, so the access stops
  the run with the explicit fault `Guest fault at pc 0x005b7a40: Guest
  access at 0x10001810 (width 4) is outside the mapped region`.
- The function then copies code to 0x80076000/0x80002000 and later sets up
  the alarm/timer services; modeling it is the **EE timer/alarm subsystem**
  (register storage, counting, `SetAlarm` 0x18/0xFC, the alarm callback
  path), which needs its own decision. The fault pc and address are the
  evidence that starts it.
- Note that this wall arrives *before* the game's thread creation
  (0x005AEA78), so the cooperative scheduler still waits for its first
  end-to-end exercise; the timer/alarm init is on the path.

## Limits recorded

- The patched-call model restores `ra` and the resume address but not the
  caller-saved registers the real kernel would restore; unobservable to
  o32-compliant callers (decision 0006).
- The private return service 0x100 is outside the ABI by construction.
- The game's scan helper is interpreted (not a `jal` target, so not a
  module entry); 11,403 interpreted steps at this frontier.
- FlushCache is still registered but unreached (the fault comes first).

## Evidence

- CTest **30/30** (the `ee_kernel` unit test covers SetSyscall, the
  synthetic table, the dispatch and the stub return; `gt4boot_services` runs
  `--services 9` with the interpreter comparison).
- Python 73 collected (67 run, 6 skip) — unchanged.
- The syscall-site survey: 167 `syscall` words in the text — 3 in the crt0,
  140 in the SDK wrapper cluster around 0x5AD900, and 24 in two small
  clusters; wrapper-style returns dominate, the stub covers inline sites
  identically.
