# M17–M21 lesson — advanced control: critical edges, COP0/traps, and the largest verified module

Prepared 2026-10-04. BUILD/VERIFY: as recorded in the sources —
M18: CTest 21/21, Python 71 collected; M19: CTest 21/21, Python 71
(65 run, 6 skip); M21: CTest 23/23, Python 71 (65 run, 6 skip). See
the [M18 evidence](../reverse-engineering/m18-critical-edges.md),
[M19 evidence](../reverse-engineering/m19-cop0-and-shifts.md), and
[M21 evidence](../reverse-engineering/m21-trap-slots-and-the-largest-module.md).
EXPLAIN: this is the worked explanation; tutoring review pending.

Every load-bearing statement below traces to one of those three
documents. Later reframings (M27, M28, M29) are noted explicitly at
the end and are pointers, not new claims.

## Objective and motivation

By the end of M17 the translator could walk functions entered above
their own transfers and stop cleanly at syscalls — but the
`0x0058ce48` call tree kept hitting control-flow shapes with no
representation at all. This arc teaches the project's gap-driven
motion: run the real call tree, read the exact word that stops it,
give that shape a representation in all three places (flow walker,
interpreter, translator), verify a module, and move to the next
stop. It ends with the largest verified translation so far: the
whole `0x0058ce48` tree — 57 functions, 2,588 instructions.

Three shapes motivate the three slices: a delay slot that is also a
branch target (M18), privileged and trapping words the decoder had
never named (M19), and a conditional trap sitting inside a likely
branch's delay slot (M21).

## Step 1 — the delay slot with two identities (M18)

The tree reached a cache-flush loop with a rotated decrement:

```text
005b0fc8: 11400014  beq t2, zero, 0x005b101c
005b0fcc: 254affff  addiu t2, t2, -0x1     <- delay slot of the beq
...
005b1014: 1d40ffed  bgtz t2, 0x005b0fcc    <- back-edge targeting that slot
```

The decrement at `0x005b0fcc` is both the `beq`'s delay slot and the
loop's back-edge target. Under the model it has two identities:
entered from the `beq` it runs as the delay slot (then the `beq`'s
target or fall-through applies); entered by the back-edge it runs as
an ordinary instruction and control continues at `0x005b0fcc + 4`.
The previous emission inlined every delay slot into its transfer, so
a second entry point had no representation and the function was
rejected. That rejection — not a wrong answer — was the honest
behavior: no silent fallback.

The representation, per the source:

- A delay slot that is a label target also gets a **standalone
  copy** at its own address. The edge path executes the copy and
  falls through to the next word, which is exactly what the model
  does.
- The transfer keeps its inline execution for the normal path and
  jumps past the copy on the path that would otherwise fall into
  it: branches (both forms) and calls emit `goto <slot + 4>;` after
  their decision or after the callee returns. Jumps and returns
  never fall through, so they need nothing.
- **C++ detail worth recording**: branch decisions are evaluated
  once, before the delay slot, into a variable. With forward gotos
  in the same scope MSVC rejects skipping an initialization
  (C2362), so all `taken_*` variables are declared at the top of
  the generated function and only assigned where the branch sits.

Verification is the cache-flush loop itself at `0x005b0f78`
(43 instructions, the critical edge in the middle): it translates
and matches the interpreter on 7 input states, including empty,
misaligned and multi-line ranges, comparing all registers and the
continuation:

```text
translated 0x005b0f78 matches the interpreter on 7 input states
(critical-edge loop included)
```

The survey after the slice shows the method working: the tree
advanced past movn → lwl → the tail thunk and its syscall boundary
→ cache → the critical edge, and stopped at something genuinely
new — COP0 (`mfc0` reading register 12, Status, at `0x005b72f8`):

```text
ERROR: Not supported ... 0x40026000 ; opcode=0x10 rs=0x00 at 0x005b72f8
```

Still queued at that point: a `break` in a likely-branch delay slot
(blocking `0x579780`) and the MMI parallel multiply family. The
M14 savestate work had already recorded the live menu state's
Status as `0x40000000`, which the source notes gives the future
CP0 model a realistic default.

## Step 2 — COP0, BREAK, and the shifts the tree actually reached (M19)

M19 adds 206 operations in total, and the source is explicit that
the shift family rode along because the tree demanded it
(`dsll32` — the `dsll32`/`srl`-style sign-extension idiom the
function at `0x5bae00` opens with), not because the curriculum said
so:

- **CP0 register file** in the guest state, starting from the live
  menu state the M14 observation captured: `Status = 0x40000000`
  (CU2 usable), everything else zero. The code under test was
  captured from a running game, not a cold reset, so the model
  starts where the game was.
- **`mfc0`**: sign-extended 32-bit read; `rt == 0` skips the read
  entirely (except register 9, Count, per the reference); register
  12 (Status) reads through the `0xf0c79c1f` mask; the performance
  counter (25) stops with context instead of inventing a value.
- **`mtc0`**: writes through; register 16 (Config) protects the
  read-only cache-size bits and reports the fixed ones
  (`(value & ~0xFC0) | 0x440`); register 24 (Debug) accepts the
  write as feedback only; the performance counter stops with
  context.
- **`ei`/`di`**: both gated exactly like the reference — they take
  effect in kernel mode (`KSU == 0`) or when already in an
  exception level (`_EDI`/`EXL`/`ERL`), toggling `Status.EIE`
  (bit 16). The bit layout comes from PCSX2's `CP0regs` union.
- **`break`**: decodes and traps like `syscall` — the model stops
  at the word (the breakpoint handler is not modeled). In
  translated code it becomes an automatic halt, the same boundary
  rule as syscalls.
- **The 64-bit shift family (12)**: `dsll`/`dsrl`/`dsra`, the
  `…32` forms (amount + 32) and the variable
  `sllv`/`srlv`/`srav`/`dsllv`/`dsrlv`/`dsrav`.

The references are PCSX2 master (`COP0.cpp` for the move semantics,
the EI/DI gate and the Status read mask, `R5900.h` for the Status
bit layout), fetched 2026-10-01 and used as documentation of
hardware behavior — no code copied. The translator emits the CP0
operations with helpers that mirror the interpreter bit for bit
(read mask, Config protection, EI gate), so translated functions
using `mfc0 Status` / `ei` stay verifiable.

Evidence, as cited: hand-computed interpreter fixtures (the masked
Status read, `mtc0` write-back, EI setting EIE in kernel mode and
being gated out with `KSU` set to supervisor, the Config write
masking, the break stopping with context, and the shift family's
round-trips `dsll`→`dsll32`→`dsrl32`, sign fills `dsra32`/`srav`
and variable amounts); decoder/disassembler fixtures for all 17 new
encodings, with the disassembler naming the CP0 registers
architecturally (`mfc0 v0, Status`); the CLI tests moving their
"unsupported word" example to `ldl` at `0x001041f4` because the COP0
and break words now decode; and a scan of the first 200,000 text
words finding 655 unsupported words, concentrated in the unaligned
64-bit family (`ldl`/`ldr`/`sdl`/`sdr`) — the next op family.

The survey after the slice is the honest loop-closing habit again:
through movn → lwl → tail thunk → cache → critical edge → COP0
and `dsll32`, stopping at:

```text
ERROR: A transfer leaves the function in function 0x005bae00:
beql a2, zero, 0x005baeac at 0x005baea4
```

The cause is visible in the code: the `beql`'s delay slot is a
`break` (the division trap idiom), so the walker marks the block
`branch-in-delay-slot` and stops following. The source already
sketches the fix that M21 ships: a likely branch's delay slot runs
only when taken, and the trap stops at the boundary — so the
emission should be `if (taken) { set_pc(delay); return; }` with no
inline statement. That, plus the `ldl`/`ldr`/`sdl`/`sdr` family,
are the two recorded next pieces.

## Step 3 — the trap slot, eret, and the two verified modules (M21)

The `beql …; break` idiom (a conditional division trap) is
represented end to end:

- The flow walker keeps the block's branch facts and flags the
  slot (`BasicBlock::delay_slot_traps`): the slot runs only when
  the branch is taken, and then the trap preempts the transfer.
- The interpreter stops at the trapping word with the Exception
  outcome when the slot fires (previously it reported an illegal
  delay slot), and skips the slot entirely when the likely branch
  is not taken.
- The translator emits `if (taken) { set_pc(slot); return; }` with
  no inline statement; the slot word lives in the unit's
  `trap_slots` set rather than the reachable run.

`eret` decodes and executes like the reference: the target comes
from `EPC` or `ErrorEPC` by the error level, which the return
clears, and it applies immediately (no delay slot). In translated
code it is a boundary: the module derives the pc from CP0 and
returns, exactly where the interpreter's step lands — the same
stop-at-the-boundary contract as syscalls.

The tree needed closing emission gaps and got them: `lhu`/`lwu`/
`sh`, `dsubu` (the trapping `dsub` stays unsupported until the
exception path exists), `mult`/`multu`/`div`/`divu` plus the
second-bank forms via mirroring helpers (`execute_multiply`,
`execute_div`, `execute_divu`, `write_hilo_low`/`high`), and
`mfhi`/`mflo`/`mfhi1`/`mflo1`.

Then the payoff — two new verified modules:

```text
translated 0x00579780 matches the interpreter on 6 input states
translated module 0x0058ce48 (57 functions) matches the interpreter on 4 input states
```

- `0x00579780` (66 instructions, a division-based utility reading
  four bytes through its pointer and a lookup table in the data
  segment) — every register, HI/LO, the continuation, the stack
  window and the input buffer compared.
- `0x0058ce48` — **57 functions, 2,588 instructions**: the whole
  call tree this arc peeled one gap at a time (movn → lwl → the
  tail thunk and its syscall boundary → cache → the critical edge
  → COP0 → dsll32 → the trap slot → dsubu and the emission gaps →
  ERET). Four input states, including two that run the lazy
  initializer and stop at the first BIOS service — the translated
  module and the interpreter land on the same pc with identical
  registers, HI/LO and memory in the compared windows.

The M21 source also records the whole-text decode correction
(pointing at the M20 document): the true numbers over all
1,334,917 words are 2,269 unsupported (0.17%), dominated by the VU0
macro family (COP2, ~1,250 words), `lqc2`/`sqc2` (378), the
trapping arithmetic forms (123), `daddi` (23) and the MMI2/MMI3
parallel-multiply remainder (14). The first 350,000 words — every
region this project samples — decode with zero unsupported. The
next major block is therefore **VU0 macro mode**, the curriculum's
own M15–M17 material, which needs its own vector register file and
semantics.

## What this arc does not claim (later reframings)

- **M27 — indirect control flow became a boundary.** The `jalr`
  and computed-`jr` shapes this arc could not follow (the survey's
  standing rejection class) later became stop-at-the-transfer
  boundaries — the same stop-at-the-boundary contract this arc
  established for syscalls, traps, and `eret`, extended to
  transfers whose target is only known at runtime. See
  `m27-indirect-flow-boundaries.md`.
- **M28 — modules dispatch their own indirect targets.** A
  per-module entry table backs `jalr` and computed `jr`, so a
  known target continues inline and only an unknown target keeps
  the M27 boundary stop. The M18 lesson (give every entry shape a
  representation) recurs one level up: from duplicate copies for
  dual-identity words to a dispatch table for runtime-known
  targets. See `m28-module-dispatch.md`.
- **M29 — the whole-program build.** Module-size policy plus
  `--functions N` / `--all` turn the verified-tree machinery into
  one module for the whole game. The 57-function module that is
  this arc's climax becomes one entry among 15,068 — the payoff of
  the "peel one gap, verify, repeat" motion at full-text scale.
  See `m29-whole-program-build.md`.

Nothing here contradicts those later slices: this arc made
dual-identity words, privileged moves, conditional traps, and
CP0-derived returns representable and verified; whether a tree is
*followable* (M27), *dispatchable* (M28), or *buildable at full
scale* (M29) is a separate question each later slice answers on
top of this one's contracts.

## Connection to our implementation

The three sources pin behaviors and unit names, not file paths, so
the table maps each piece to its representation and its source
instead of inventing locations:

| Piece | Representation (as cited) | Source |
| --- | --- | --- |
| Dual-identity delay slot | standalone copy at its own address; edge path falls through; transfer jumps past with `goto <slot + 4>` | M18 |
| Branch-decision variables | evaluated once before the slot; `taken_*` declared at function top (MSVC C2362) | M18 |
| Cache-flush loop `0x005b0f78` | 43 instructions; 7-state match, all registers + continuation | M18 |
| CP0 register file | `Status = 0x40000000` default from the M14 live capture; rest zero | M19 |
| `mfc0` / `mtc0` | sign-extended reads with the `0xf0c79c1f` Status mask; Config `… \| 0x440` protection; counter stops with context | M19 |
| `ei` / `di` | kernel-or-exception-level gate, toggling `Status.EIE` (bit 16) | M19 |
| `break` | traps like `syscall`; automatic halt in translated code | M19 |
| 64-bit/variable shifts (12) | `dsll`/`dsrl`/`dsra`, `…32` (amount + 32), `sllv`/`srlv`/`srav`/`dsllv`/`dsrlv`/`dsrav` | M19 |
| Trap slot (`beql …; break`) | `BasicBlock::delay_slot_traps`; interpreter Exception stop when taken, skip when not; translator `if (taken) { set_pc(slot); return; }`; `trap_slots` set | M21 |
| `eret` | EPC/ErrorEPC-derived target, level clear, no delay slot; derived-pc boundary in translation | M21 |
| Emission gaps | `lhu`/`lwu`/`sh`, `dsubu`, multiply/divide + second bank via mirroring helpers, `mfhi`/`mflo`/`mfhi1`/`mflo1` | M21 |
| Verified modules | `0x00579780` (66 insns, 6 states); `0x0058ce48` (57 functions, 2,588 insns, 4 states incl. lazy-init stops at first BIOS service) | M21 |

## Understanding checkpoint

1. At `0x005b0fcc`, one word is both a delay slot and a loop
   target. Why did the inlining-only emission have to *reject* the
   function rather than pick one meaning, and what does the
   standalone copy preserve for each entry path?
2. Branches and calls emit `goto <slot + 4>;` after their decision
   or the callee return, but jumps and returns emit nothing extra.
   Why is the extra jump unnecessary for the second group?
3. MSVC error C2362 forced the `taken_*` declarations to the top
   of the generated function. What host-language rule does a code
   generator have to respect here, and why does assignment-at-site
   still preserve single evaluation?
4. `mfc0` with `rt == 0` skips the read — except for register 9.
   What modeling choice does that exception reveal about Count
   versus the other CP0 registers?
5. The `beql …; break` emission is `if (taken) { set_pc(slot);
   return; }` with no inline statement. Explain why *both* halves
   matter: the condition around the stop, and the absence of an
   inline copy.
6. M21 ends with a 57-function verified module; M29 builds the
   whole game as one module. Explain why the second verdict can
   supersede the first in scale without contradicting it — and
   which contract from this arc the whole-program build still
   relies on.
