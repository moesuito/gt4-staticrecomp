# M21 — Trap slots, ERET, and the largest verified module

Date: 2026-10-02. Inputs: the pinned CORE.

## Trap in a likely branch's delay slot

The `beql …; break` idiom (a conditional division trap) is now represented
everywhere:

- The flow walker keeps the block's branch facts and flags the slot
  (`BasicBlock::delay_slot_traps`): the slot runs only when the branch is
  taken, and then the trap preempts the transfer.
- The interpreter stops at the trapping word with the Exception outcome when
  the slot fires (previously it reported an illegal delay slot), and skips the
  slot entirely when the likely branch is not taken.
- The translator emits `if (taken) { set_pc(slot); return; }` with no inline
  statement; the slot word lives in the unit's `trap_slots` set rather than
  the reachable run.

## ERET

`eret` decodes and executes like the reference: the target comes from `EPC` or
`ErrorEPC` by the error level, which the return clears, and it applies
immediately (no delay slot). In translated code it is a boundary: the module
derives the pc from CP0 and returns, exactly where the interpreter's step
lands — the same stop-at-the-boundary contract as syscalls.

## Translator emission gaps closed

The tree needed and now has: `lhu`/`lwu`/`sh`, `dsubu` (the trapping `dsub`
stays unsupported until the exception path exists), `mult`/`multu`/`div`/
`divu` plus the second-bank forms via mirroring helpers (`execute_multiply`,
`execute_div`, `execute_divu`, `write_hilo_low/high`), and `mfhi`/`mflo`/
`mfhi1`/`mflo1`.

## The largest verified module

```text
translated 0x00579780 matches the interpreter on 6 input states
translated module 0x0058ce48 (57 functions) matches the interpreter on 4 input states
```

- `0x00579780` (66 instructions, a division-based utility reading four bytes
  through its pointer and a lookup table in the data segment) — every
  register, HI/LO, the continuation, the stack window and the input buffer
  compared.
- `0x0058ce48` — **57 functions, 2,588 instructions**: the whole call tree
  that this session peeled one gap at a time (movn → lwl → the tail thunk and
  its syscall boundary → cache → the critical edge → COP0 → dsll32 → the trap
  slot → dsubu and the emission gaps → ERET). Four input states, including
  two that run the lazy initializer and stop at the first BIOS service — the
  translated module and the interpreter land on the same pc with identical
  registers, HI/LO and memory in the compared windows.

CTest 23/23; Python 71 collected (65 run, 6 skip).

## Whole-text decode status (corrected)

See the correction note in `m20-fully-decoding-text.md`: the true numbers over
all 1,334,917 words are 2,269 unsupported (0.17%), dominated by the VU0 macro
family (COP2, ~1,250 words), `lqc2`/`sqc2` (378), the trapping arithmetic
forms (123), `daddi` (23) and the MMI2/MMI3 parallel-multiply remainder (14).
The first 350,000 words — every region this project samples — decode with zero
unsupported. The next major block is therefore **VU0 macro mode**, the
curriculum's own M15-M17 material, which needs its own vector register file
and semantics.
