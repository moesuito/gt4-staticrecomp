# M27 — indirect control flow becomes a boundary

Date: 2026-10-02. Inputs: the pinned CORE. Follow-up to M26, whose survey
named indirect control flow as the blocker for 93% of the rejections.

## What changed

Every transfer whose target is a runtime value now stops the module at the
transfer, exactly where the interpreter stops, instead of rejecting the whole
tree:

- **Indirect calls (`jalr`)** stop before the call with the pc at the transfer
  (the delay slot belongs to the call, so it is a boundary word too; the link
  register is not written, because the driver executing the call writes it).
- **Computed jumps (`jr` through another register)** stop the same way.
- **Instructions the model does not execute** (VCALLMS/VCALLMSR, the unassigned
  encodings) stop at the word, mirroring the interpreter's Unsupported stop.
- **Unmodeled instructions in a transfer's delay slot** stop at the slot,
  after the transfer's own state effects: a likely branch skips the slot when
  not taken (the existing trap-slot emission), a `jal` writes its link first,
  and `jr ra` stops without state changes. A function whose first instruction
  is a boundary translates as a stub that stops at its own entry.

The emitted module therefore never guesses a runtime target: it executes the
identical prefix and halts with the pc at the boundary, leaving the dispatch
to a future driver (the continuation code after an indirect call is still
translated, so a driver can resume there).

## Results (pinned CORE, 2026-10-02)

```
survey: entries=15067 translated=14938 functions_in_trees=316092
covered instructions: 858621 of 1334917 words in the file-backed text
reason: 119 x The call tree exceeds the function limit
reason: 5 x The call tree exceeds the instruction budget
reason: 5 x start-validation edge cases
```

- **99.1% of the direct-call targets translate** (14,938 of 15,067), up from
  62%; the covered instructions rose from 399,046 to **858,621 (64.3% of the
  file-backed text)**.
- Every remaining rejection is a **module-size policy**, not a semantic gap:
  the 256-function and instruction-budget limits, plus five start-validation
  edges.

## Evidence

- **A new verified module: `0x00101C28`**, a two-instruction trampoline ending
  in `jalr ra, a0`. The differential test runs three target-register values
  (zero, code, scratch) and compares all registers, the pc and the scratch
  memory after stopping at the same jalr address as the interpreter.
- The Python CLI suite now checks that a formerly rejected function
  (`0x5a3140`) translates with `state.set_pc(0x005a3194u);` at its jalr, that
  the trap-only seed becomes a boundary stub, and that the survey reports the
  outcome.
- A spot check of the `jr ra` + VCALLMS-in-delay-slot idiom (`0x004A53F8`)
  shows the module stopping at the slot (`set_pc(0x004a53fc)`), matching the
  interpreter's stop point.
- CTest 25/25; Python 72 collected (66 run, 6 skip).

## Limits recorded

- The boundary convention stops *before* the transfer; a driver must resolve
  the runtime target (or run a VU0 micro interpreter for VCALLMS) and resume
  from the stopped pc.
- Modules keep the 256-function limit; whole-program builds will want a
  larger policy (the 119 rejections are exactly that).
- The continuation after an indirect call is translated even though the
  module cannot reach it yet; it is the future resume point.
