# M18 — Critical edges: a branch that targets a delay slot

Date: 2026-10-01. Inputs: the pinned CORE.

## The pattern

The 0x58ce48 call tree reached a cache-flush loop with a rotated decrement:

```text
005b0fc8: 11400014  beq t2, zero, 0x005b101c
005b0fcc: 254affff  addiu t2, t2, -0x1     <- delay slot of the beq
...
005b1014: 1d40ffed  bgtz t2, 0x005b0fcc    <- back-edge targeting that slot
```

The decrement is both the beq's delay slot and the loop's back-edge target.
Under the model, the instruction at 0x5b0fcc has two identities: entered from
the beq it runs as the delay slot (then the beq's target or its fall-through
applies); entered by the back-edge it runs as an ordinary instruction and
control continues at 0x5b0fcc + 4. The previous emission inlined every delay
slot into its transfer, so a second entry point had no representation and the
function was rejected.

## The representation

- A delay slot that is a label target now also gets a **standalone copy** at
  its own address (the edge path: the copy executes and falls through to the
  next word, which is exactly what the model does).
- The transfer keeps its inline execution for the normal path and jumps past
  the copy on the path that would otherwise fall into it: branches (both
  forms) and calls emit `goto <slot + 4>;` after their decision or after the
  callee returns. Jumps and returns never fall through, so they need nothing.
- **C++ detail worth recording**: branch decisions are evaluated once, before
  the delay slot, into a variable. With forward gotos in the same scope MSVC
  rejects skipping an initialization (C2362), so all `taken_*` variables are
  now declared at the top of the generated function and only assigned where
  the branch sits.

## Verification

The cache-flush loop at 0x005b0f78 (43 instructions, the critical edge in the
middle) translates and matches the interpreter on 7 input states, including
empty, misaligned and multi-line ranges, comparing all registers and the
continuation:

```text
translated 0x005b0f78 matches the interpreter on 7 input states
(critical-edge loop included)
```

CTest 21/21; Python 71 collected.

## Survey after the slice

The 0x58ce48 call tree advanced past movn → lwl → the tail thunk and its
syscall boundary → cache → the critical edge. It now stops at:

```text
ERROR: Not supported ... 0x40026000 ; opcode=0x10 rs=0x00 at 0x005b72f8
```

That is **COP0**: `mfc0` reading register 12 (Status) — the next slice, with
BREAK (a `break` in a likely-branch delay slot still blocks 0x579780) and the
MMI parallel multiply family already queued. The M14 savestate work recorded
the live menu state's Status as 0x40000000, which gives the future CP0 model a
realistic default.
