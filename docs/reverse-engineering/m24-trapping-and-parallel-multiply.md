# M24 — the trapping arithmetic and the parallel multiply/divide family

Date: 2026-10-02. Inputs: the pinned CORE. References: PCSX2 master
(`R5900OpcodeImpl.cpp` and `COP0.cpp` for the trapping forms and the COP0
branch, `MMI.cpp` for the parallel multiply/divide bodies,
`R5900OpcodeTables.cpp` for the MMI table layout), fetched 2026-10-02.

## What was added (349 operations in total)

- **The six trapping arithmetic forms**: ADD, SUB, DADD, DSUB, ADDI and
  DADDI. The overflow checks mirror the reference bit for bit: the 32-bit
  method compares bit 31 against bit 32 of the 64-bit sum of the
  sign-extended operands, the 64-bit method tests the sign-bit identity, and
  the subtract forms negate the right operand before the check — which also
  reproduces the reference's edge-case behavior for the most negative
  operand (a documented quirk, not a hardware-accurate check). The check runs
  before any register write, even when the destination is the zero register.
  When it fires, the step returns the stable Exception outcome at the
  offending word without touching the destination; the handler itself is not
  modeled, like SYSCALL and BREAK.
- **Eight parallel multiply/divide operations**: the halfword forms PMADDH,
  PMSUBH and PMULTH (eight signed halfword products filling the eight
  32-bit lanes of the HI/LO pair, with the reference's packing of the low
  accumulator words into the destination), and the word forms PMULTW,
  PMULTUW, PMADDUW, PDIVW and PDIVUW (two 32-bit lanes per pass, the second
  pass over the "1" accumulator pair; the divides publish quotient and
  remainder with the reference's special cases for $80000000/-1, zero
  divisors and unsigned overflow wraparound).
- The remaining unmodeled family members are recorded: PMADDW and PMSUBW
  (which carry the reference's "division voodoo" corrections), PHMADH and
  PHMSBH, and PDIVBW — none of them used by the pinned text.

## Evidence

- Decode and disassembly fixtures including the observed words: PMADDH at
  `0x70421409`, PMULTH at `0x704E0709`, PMULTUW at `0x7181C329`, and the
  hand-assembled trapping and parallel forms.
- Interpreter scenarios with hand-computed values: the trapping ops write
  their results, each overflow flavor stops with the Exception outcome and
  the destination intact, PMADDH fills the eight lanes and packs the
  destination, PMSUBH subtracts them back to zero, the word forms store the
  signed/unsigned products in both accumulators and both destination halves,
  and the divides cover quotient, remainder, the second lane pair and the
  zero-divisor path.
- **Application to the pinned text**: the unsupported words over the whole
  file drop from 672 to **497** — and in the real code region
  (0x00100000..0x616F1C) only **4 words remain**:
  - two BC0F (`0x4100fffa`, twice), whose condition is the reference's
    DMA-derived COP0 condition line; the model has no DMA controller, so
    these stop with context instead of guessing;
  - two words at unassigned function 0x28 (`0x00001028`, twice), inside the
    exception handler's register-dump code, unreachable in a model without
    the handler.
  The remaining 493 words live inside the text section's trailing data table
  (700 words at 0x616F28..0x617A14) and are not instructions.
- CTest 23/23; Python 71 collected (65 run, 6 skip).

## Limits recorded

- The trapping forms stop at the boundary; CP0 Cause/EPC/EXL are not
  fabricated because the handler never runs in this model.
- BC0F/BC0T/BC0FL/BC0TL stay unsupported pending an honest DMA model; the
  observed sites rely on that condition.
