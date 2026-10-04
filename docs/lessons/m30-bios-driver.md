# M30 lesson — the driver, the service table, and the bridge that resumes

Prepared 2026-10-04. BUILD/VERIFY: passed for slices 1 and 2; see the
[M30 slice-1 evidence](../reverse-engineering/m30-driver-first-slice.md),
[slice-2 evidence](../reverse-engineering/m30-bios-services-and-bridge.md),
and [decision 0004](../decisions/0004-driver-boundary-classification.md).
EXPLAIN: this is the worked explanation; tutoring review pending.

## Objective and motivation

M29 ends with a compiled whole game nobody has run: 15,068
functions, 924,991 instructions, one 146 MB module. Translation
verifies functions; it does not execute programs. This arc
teaches the project's execution model: a driver that runs a
translated module *as a program* until it stops, names the stop
from the guest state alone, answers what is answerable, and
bridges the rest with the one implementation every module was
verified against — the step-by-step interpreter. It ends with the
whole game running as one module from the ELF entry through
SetupThread and SetupHeap, state identical to the interpreter
after 942,726 instructions, stopping at the CreateSema wall.

The motivating gap is a missing verb: before this arc, the
project could say what code *means* but not what happens when it
*runs*. The driver is that verb, and classification is its
grammar.

## Step 1 — call the entry, name the stop

`ee::Driver` calls the module entry that owns the current pc and
classifies where control returned from the guest state alone —
the word at the pc, the link register, the memory window. The
table maps one-to-one onto the stop shapes the translator emits:

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

Two honest limits ship with the table: the pc==ra rule is an
inference (a trapping stop landing exactly on ra would
misreport; none exists in the pinned code), and a stop at an
ordinary instruction cannot name its cause from the pc alone
(the word rides along so a later slice can disambiguate). The
rejected alternatives say what the project values: no
stop-reason field in `GuestState` (a host diagnostic must not
become guest model state), no stop enum from generated functions
(signature churn for one ambiguity), and never running the
interpreter alongside just to compare (the driver must not
depend on the implementation it replaces).

The first verified run exercises exactly one row:

```text
build/gt4run.exe private/fingerprint-check/CORE.GT4 --compare-interpreter
boundary: syscall 0x001001c8 service 0x3c
interpreter: 942695 instructions, state identical (registers, HI/LO, FPU, shift cache, pc, memory digest)
```

The translated startup runs from the ELF entry (`0x00100008`)
to the first BIOS syscall at `0x001001C8` — service `0x3C`,
SetupThread, the number traveling in v1 — and the interpreter
reaches the same pc with a syscall Exception after 942,695
instructions. `ee_driver` unit tests cover the catalog, single
entries, and every classification branch with hand-assembled
words and no game data.

## Step 2 — answer three services, bridge everything else

The service layer maps the number in v1 to a handler, and an
unregistered number stays a boundary:

- **SetupThread (0x3C)**: returns the thread's stack pointer in
  v0, which the crt0 stores into sp. The pinned crt0 matches the
  public ps2sdk source line for line (gp, stack, size, args, the
  ExitThread stub at `0x00100228`); the menu RAM dump preserves
  the boot stack at the region top (`[0x1FF8000, 0x2000000)`),
  with gp `0x6DDDF0` in the control block. **High confidence** on
  the contract and region; the BIOS's exact offset below the top
  is unobserved. The model returns the region top aligned down
  to 16 bytes.
- **SetupHeap (0x3D)**: validates the heap request (mapped start,
  nonzero size; -1 means "to the end of memory") and records
  nothing — no verified path reads the kernel heap structure
  back. **High confidence** on the contract; the structure is
  **Unknown**.
- **FlushCache (0x64)**: no-op, following the established CACHE
  policy.

The bridge resolves what the module cannot pass: a syscall with
a handler runs inline and continues at pc + 4; a jr-ra return, an
unknown indirect transfer, or an eret hands control to the
interpreter, which continues until the next module entry. This
composes through calls and returns — a resumed callee returning
to its caller needs no entry at the return address — and required
no translator change. The generated module exposes its entry
table publicly (`translated::has_entry` / `call_entry`), so a
15,000-entry module needs no linear catalog. The two designs that
lost (resume entries per halt address; inline syscall calls)
remain future *performance* options, explicitly not correctness
requirements.

The verified whole-game run shows the composition working:

```text
service 0x3c at 0x001001c8
service 0x3d at 0x001001e4
boundary: syscall 0x005adca4 service 0x40
stats: module calls 2, interpreted steps 9, services handled 2
interpreter: 942726 instructions, state identical
```

Two module calls (the startup function, then init `0x005B7560`),
nine interpreted instructions bridged between them, stop at the
third syscall — `CreateSema` (0x40) at `0x005ADCA4`, whose
caller builds two semaphore structures with data-segment option
pointers. The reference walks the same path in 942,726
instructions — 31 more than the first slice's 942,695, because
the reference now handles the two services and continues, and
the count is reported by the reference loop, not the module.
`gt4boot` (whole game as one module, `--services N`,
`--compare-interpreter`, fixture-built on demand) is the runner
from here on.

## What later evidence reframed (not smoothed over)

- The first slice's `ModuleCatalog`/`NoEntry` design was
  superseded *within the arc itself*: a pc without an entry is
  bridged by the interpreter, not stopped. The boundary kinds,
  the startup run, and its evidence stand unchanged.
- The driver Esono grew far past "run entry, classify stop":
  service outcomes that switch threads (decision 0005),
  handler injection with no-nesting and deferred switches
  (decisions 0009/0013), idle delivery with budgets and clocks,
  and differentials that now run 90,000 services with the
  interpreter. None of that revises this lesson; each addition
  cites the bridge contract taught here (boundaries the module
  cannot pass belong to the interpreter) — including the
  temporary forced-interpretation hooks of later probe slices,
  which borrow the bridge without changing it.
- `FlushCache` was registered but unreached (the crt0 calls it at
  `0x001001F0` only after the init function returns) — an
  honest pending item inside a passing slice.
- The next wall named here (a cooperative scheduler for
  WaitSema, then the kernel-patch services) became the next two
  lessons' arcs, exactly as predicted.

## Connection to our implementation

| Piece | File | Job |
| --- | --- | --- |
| Entry call + classification | `src/ee/driver.cpp` (`Driver::run`, `classify_boundary`) | named stops from guest state |
| Service dispatch | `src/ee/services.cpp` (`ServiceTable`, Setup/Heap/Cache) | v1-numbered handlers, unregistered stays boundary |
| Bridge continuation | `src/ee/driver.cpp` (interpreter fallback) | syscall inline, returns/transfers/eret interpreted |
| Entry table | generated `has_entry` / `call_entry` | dispatches without a linear catalog |
| Whole-game runner | `tools/gt4boot/main.cpp` (`--services`, `--compare-interpreter`) | one module from the ELF entry |
| Boundaries + budget | `tests/unit/ee_driver_test.cpp` | hand-assembled words, no game data |

## Understanding checkpoint

1. A translated module is a plain function over `GuestState`.
   Why must classification read *only* guest state, and which
   rejected alternative would have broken that rule?
2. The interpreter is both the reference every module was
   verified against *and* the bridge. Why is that sound rather
   than circular, and which rejected alternative states the
   circularity worry explicitly?
3. `pc == ra` classifies a return. Construct the misreport the
   docs admit, and explain why "no such case observed" is a
   sufficient (if provisional) answer.
4. The 942,695 → 942,726 instruction drift between the two
   slices' reference runs: what changed, who reports the count,
   and why is the drift evidence of health rather than rot?
5. Resume entries per halt address would shrink the 9 bridged
   instructions. Why are they performance, not correctness —
   and what would have to change in the translator to emit them?
6. SetupHeap "validates and records nothing". What future
   observation would force modeling the heap structure — for
   example, which guest read-back, if ever seen, would turn the
   current Unknown into a wall?
