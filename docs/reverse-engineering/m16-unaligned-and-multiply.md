# M16 — Unaligned access, multiply/divide and PLZCW; a fifth function verified

Date: 2026-10-01. Inputs: the pinned CORE. References: PCSX2 master
(`R5900OpcodeImpl.cpp` for LWL/LWR/SWL/SWR and MULT/DIV, `MMI.cpp` for
MADD/MADDU and the compact second-bank forms, `common/BitUtils.h` for the
leading-sign count PLZCW uses), fetched 2026-10-01; used as documentation of
hardware behavior, no code copied.

## What was added

Twenty operations. The corrected operation count in the decoder is **188**:
earlier documents said 175 because the counting script included comment
fragments; the line-filtered count is authoritative. Nothing about the
interpreter or the tools changes because of that correction.

- **LWL/LWR/SWL/SWR** with the reference mask/shift tables. LWR with a
  nonzero shift replaces only the low 32 bits and keeps the register's upper
  half — the first place the state model needs that rule, which is why
  `write_gpr_low32` exists.
- **LWU/LHU** (zero-extending loads) and **SH**.
- **MULT/MULTU/DIV/DIVU** including the quirk cases: `0x80000000 / -1`
  saturates (`LO = 0x80000000`, `HI = 0`); division by zero writes `LO = ±1`
  by the dividend's sign and `HI = ` the dividend; both halves store
  sign-extended.
- **MADD/MADDU** (MMI functions 0x00/0x01) and the compact second-bank forms
  **MULT1/MULTU1/DIV1/DIVU1/MADD1/MADDU1** with the same rules.
- **PLZCW**: per-word leading-sign counts (negative values invert, zero counts
  32, the instruction stores one less than the count).

## Evidence

- Hand-computed interpreter fixtures: all four alignments for LWL/LWR/SWL/SWR;
  the lwr upper-half preservation (a check that a plain 32-bit write would
  fail); the load pair reassembling an unaligned word; multiply/divide signs;
  divide-by-zero signalling; the second HI/LO bank; PLZCW edge values (zero,
  sign bit, one).
- Ghidra comparison over the M15 listing plus regions around 0x5797c0 and
  0x5b2a80: `matched=594 non_nop=525 unsupported=4 r5900_only=34
  mismatched=0` — the new family agrees with Ghidra instruction by
  instruction.
- **Fifth verified real function**: `0x00572438` (a bit-count helper using
  `plzcw`, `movn`, `movz` and one initialized global). `gt4translate` now
  emits PLZCW; the native module matches the interpreter on 6 input states
  (`ee_translation_572438`).

## Observed gaps after the slice

- `0x00579780`: still rejected — a `break` sits in a likely-branch delay slot
  (the reference executes it as a trap; BREAK is not modeled).
- `0x0058ce48`'s call tree now stops at "a transfer leaves it below its
  entry" in `0x005b27f8` — a translator structural limit (a function entered
  above one of its own back-edges), recorded for a later slice.
- The MMI parallel multiply family (PMULT*/PMADD*/PMSUB*/PMULTH/PHMADH/
  PHMSBH/PDIV*/PDIVBW) remains unmodeled, as do BREAK and COP0.
