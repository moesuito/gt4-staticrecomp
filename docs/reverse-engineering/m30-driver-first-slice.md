# M30, first slice — the boundary driver

Date: 2026-10-02. Inputs: the pinned CORE. Follow-up to M29: the whole game
translates, but nothing executed a translated module as a program or named
the boundary it stops at. This slice builds that runner and verifies the
startup run through it against the interpreter.

## What changed

- **`ee::Driver`** (`include/gt4recomp/ee_driver.hpp`,
  `src/ee/driver.cpp`): it calls the module entry that owns the current pc and
  classifies where the module returned control. `ModuleCatalog` is the
  explicit list of callable entries; an address without an entry is a
  `NoEntry` boundary, never a guess.
- **`ee::classify_boundary`** names the stop from the guest state: the word at
  the pc plus the link register. The kinds map one-to-one to the stop shapes
  the translator emits (see the table below).
- **`gt4run`** (`tools/gt4run/main.cpp`): the driver as a program. It runs the
  startup module generated from the pinned CORE from the ELF entry, prints
  the boundary, and with `--compare-interpreter` repeats the run in the
  interpreter and requires the full final state to match. It is the future
  host of the BIOS service layer.
- **The startup differential test now goes through the driver**: instead of
  calling `translated::function_00100008` directly,
  `ee_translation_startup_tests` builds a one-entry catalog, runs
  `Driver::run_once()`, and checks the classified boundary (kind `Syscall`,
  pc 0x001001C8, service 0x3C) before the existing full-state comparison.
- **`ee_driver` unit tests** (`tests/unit/ee_driver_test.cpp`) cover the
  catalog, `run_once` execution and every classification branch with
  hand-assembled words, without needing game data.

## The verified startup run

```
build/gt4run.exe private/fingerprint-check/CORE.GT4 --compare-interpreter
boundary: syscall 0x001001c8 service 0x3c
interpreter: 942695 instructions, state identical (registers, HI/LO, FPU, shift cache, pc, memory digest)
```

- The driver executes the translated startup from the ELF entry (0x00100008)
  to the first BIOS syscall at **0x001001C8**, service **0x3C** (ExecPS2 in
  the PS2 ABI; the service number travels in v1).
- The interpreter reaches the same pc with `StepOutcome::Exception` /
  `Operation::Syscall` after **942,695 instructions**.
- The compared state is all 32 GPRs in both halves, all 32 FPU registers,
  FCR31, the FPU accumulator, HI/LO in both banks, the shift-amount cache,
  all 32 CP0 registers, the pc, and an FNV-1a digest of the whole flat guest
  window (text + data + the .bss the loops cleared). Everything matches.
- `ee_driver` unit evidence: a fake module shows `run_once` executes exactly
  the entry at the current pc (a register written by the module is observed);
  a missing entry returns `NoEntry` without executing anything; and the
  classification table below is checked word by word.

## The classification table

| Stop shape (translator) | Word at the pc | `BoundaryKind` |
| --- | --- | --- |
| halt at a syscall | `syscall` | `Syscall` (service = v1) |
| halt at a break | `break` | `Break` |
| eret boundary (pc derived from CP0) | `eret` | `ExceptionReturn` |
| unknown `jalr` / computed `jr` target | `jalr` / `jr` | `IndirectTransfer` |
| unmodeled word (VCALLMS, unassigned) | unsupported | `UnsupportedWord` |
| normal `jr ra` return | any; pc equals ra | `Returned` |
| trapping arithmetic overflow | an ordinary instruction | `InstructionStop` |
| pc outside the window or misaligned | unreadable | `Unmapped` |
| pc with no module entry | — | `NoEntry` |

## Limits recorded

- **The pc==ra rule is an inference**: the translator's normal return leaves
  pc at ra, so the driver reads that as `Returned`. A trapping stop whose
  address happens to equal ra would be reported as a return; no such case has
  been observed in the pinned code.
- **A stop at an ordinary instruction cannot name its cause from the pc
  alone.** With the current translator it is a trapping arithmetic overflow;
  the boundary carries the word so a later slice can disambiguate by other
  means (a stop-reason channel in the state, or generated metadata).
- **Resuming past a syscall has no entry yet.** The translator ends a
  function at the syscall and does not translate the continuation, so after a
  service handler runs there is no module entry at pc+4 to re-enter. The next
  slice must either emit resume entries for every halt address or make the
  syscall an inline runtime call; that decision is recorded in
  `docs/decisions/0004-driver-boundary-classification.md`.
- The `gt4run` binary and the startup module exist only where the local CORE
  is present (the same rule as the translation tests); nothing game-derived
  is committed.

## Evidence

- CTest **27/27**: the two new tests are `ee_driver` (unit, always built) and
  `gt4run_startup` (CLI, needs the local CORE; its pass regex requires the
  `boundary: syscall 0x001001c8 service 0x3c` line and exit code 0, which
  includes the `--compare-interpreter` state check).
- Python 73 collected (67 run, 6 skip) — unchanged by this slice; the
  savestate test now finds the copy that travels in `private/pcsx2/sstates/`
  (see the journal entry for 2026-10-02).
- The startup comparison is independent evidence: the translated module and
  the interpreter are different implementations of the same semantics; they
  agree on every compared field after 942,695 instructions.

## Next

1. **BIOS services**: start with service 0x3C (ExecPS2) and the other services
   the startup uses; this requires the resume decision above.
2. **Jump-table dispatch**: computed `jr` targets that are local blocks (not
   function entries) currently stop as `IndirectTransfer`.
3. The whole-program module as the driver's module (the `--all` build) once
   resuming works; the startup module was the smallest honest first target.
