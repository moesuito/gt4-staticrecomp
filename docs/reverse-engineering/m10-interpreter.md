# M10 interpreter evidence — one instruction at a time

2026-10-01: BUILD/VERIFY passed. EXPLAIN lesson pending.

## Scope

M10 adds `Interpreter`, a one-instruction-at-a-time executor over the M9
`GuestState`: fetch the word at the PC from guest memory, decode with the M6
decoder, classify with the M7 flow model, execute, advance. Straight-line
programs and small branch programs run against hand-computed expected values;
larger generated synthetic program suites are M11-M12.

## Execution semantics (each confirmed by fixtures)

| Instruction group | Rule implemented |
| --- | --- |
| ADDU/SUBU/ADDIU/SLL/SRL/LUI/SLT/SLTU | 32-bit result, sign-extended into the 64-bit register (the CPU's rule) |
| DADDU | full 64-bit result |
| AND/OR/XOR/ANDI/ORI | full 64-bit operation on the 64-bit registers |
| LW/LH | loaded value sign-extended to 32 bits, then to 64 bits by the register write |
| LD/SD | full 64-bit round-trip |
| SW/SB | low 32/8 bits stored, little-endian |
| Effective address | GPR[rs] + sign-extended immediate, truncated to the 32-bit address model |
| BEQ/BNE/BLEQ/BGTZ/REGIMM | 64-bit comparisons; REGIMM family tests the sign bit |
| BEQL/BNEL/BLTZL/BGEZL/BLTZALL/BGEZALL | likely branches: when not taken, the delay slot is nullified (skipped) |
| J/JAL | absolute target from the PC's region; JAL links pc+8 |
| JR | target = low 32 bits of rs; JALR links into rd, reading the target before the write |
| Delay slots | the word after a taken transfer executes before it; a transfer inside a delay slot stops as `IllegalDelaySlot` before executing |
| SYSCALL | stops as `Exception` at the boundary; the handler is not modeled |
| Unsupported | stops in place with the operation recorded |

Stopped outcomes leave the PC at the offending word and are stable: stepping
again repeats the same result until the caller changes something.

## Worked example (also a unit fixture)

Program at `0x00100000`, words literal:

```text
0x2408FFFF  addiu t0, zero, -1      ; t0 = 0xffffffffffffffff
0x24090001  addiu t1, zero, 1       ; t1 = 1
0x0109502A  slt t2, t0, t1          ; signed: -1 < 1 -> 1
0x0109582B  sltu t3, t0, t1         ; unsigned: 0xffffffff < 1 -> 0
0x51400001  beql t2, zero, +1       ; not taken; the delay slot is nullified
0x240C7FFF  addiu t4, zero, 0x7fff  ; never executes
0x240D1234  addiu t5, zero, 0x1234  ; executes
```

Hand-computed final state: `t4` stays 0, `t5` is `0x1234`, the PC lands after
the skipped word. Every row is asserted in `tests/unit/ee_interpreter_test.cpp`.

## Fixture incident, recorded

The first run of the new test crashed with a modal abort dialog on the memory
scenario. The cause was a hand-encoding slip in the fixture itself: the word
`0x3C081000` is `lui t0, 0x1000`, not the intended `lui t0, 0x10`, so the
computed store address `0x10000100` left the mapped region. The M9 memory
model reported the exact address; the fixture was corrected to `0x3C080010`,
and the test now routes CRT error dialogs to stderr so a crash fails the run
instead of blocking it. The library behavior was correct throughout — the
context-carrying error is what made the slip findable in minutes.

## Verification

- Unit fixtures (hand-computed): sign-extending arithmetic and comparisons,
  likely-branch nullification, call/return with delay slots, JALR `rd == rs`
  ordering, `*AL` link-on-taken, illegal delay slot, unsupported and syscall
  stops (stable when re-stepped), mixed-width memory access, a countdown loop
  through taken branches, and the out-of-region fetch error.
- CTest: 10/10 tests pass; the Python suite is unchanged (37 collected).

## Limits

No exception handler, no HI/LO or MULT/DIV yet (they arrive with the ops that
use them), no interrupts or caches, one memory region, and no timing. Execution
is single-stepped; nothing runs the real image yet. M11-M12 generate and run
larger synthetic program suites over this same loop.
