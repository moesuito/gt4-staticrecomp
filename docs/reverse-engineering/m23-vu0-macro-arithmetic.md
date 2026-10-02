# M23 — VU0 macro mode, part 2: the full macro instruction set

Date: 2026-10-02. Inputs: the pinned CORE. References: PCSX2 master
(`VUops.cpp` for the implementations, `VUflags.cpp` for the MAC/status flag
model, `VU0.cpp` and `R5900OpcodeTables.cpp` for the dispatch and the sync
wrappers, `Config.h` for the default gamefix settings), fetched 2026-10-02;
used as documentation of hardware behavior.

## What was added (335 operations in total)

The complete VU0 macro instruction set behind 111 newly named operations:

- **Dispatch**: functions 0x00-0x3B index the reference's standard table;
  0x3C-0x3F index the packed table with `(word & 3) | ((word >> 4) & 0x7C)`,
  which folds the destination field into the operation index. The write mask
  for the floating forms is the low four bits of rs. Slots the model does not
  execute stay Unsupported: VCALLMS/VCALLMSR (they run VU0 micro code) and
  VLQI/VSQI/VLQD/VSQD with VILWR/VISWR (they touch the VU0 data memory).
- **The float model** (`vuDouble`): denormal inputs flush to a signed zero,
  infinities clamp to ±(2−2⁻²³)·2¹²⁷ — the reference's default VU0 overflow
  setting (vu0Overflow enabled, TriAce add-sub hack disabled).
- **The flags**: `VU_MAC_UPDATE` classifies each result lane (sign bit, zero,
  denormal flush, overflow clamp) into the MAC flag with the reference's bit
  layout (x at bit 3 down to w at bit 0); `VU_STAT_UPDATE` aggregates one
  status bit per lane group; the arithmetic syncs both into VI[16] (status,
  keeping its high bits) and VI[17] (MAC) exactly like `SYNCMSFLAGS`.
- **The arithmetic**: VADD/VSUB/VMUL with elementwise, broadcast (x/y/z/w,
  Q, I) and accumulator destinations; VMADD/VMSUB with the same variants
  (result = acc ± fs·ft); VMAX/VMINI comparing integer representations;
  VOPMULA/VOPMSUB outer products.
- **The rest of the packed table**: integer/float conversions with their
  scale factors and saturating edge, VABS, VCLIPw (the shift-six clip flag
  update), VMOVE/VMR32, the division unit (VDIV/VSQRT/VRSQRT publishing Q
  through VI[22] with the 0x10/0x20 status bits, VWAITQ as the no-op it is
  without a pipeline), VMTIR/VMFIR, the random generator (VRNEXT/VRGET/
  VRINIT/VRXOR and its LFSR), and the 16-bit integer forms VIADD/VISUB/
  VIADDI/VIAND/VIOR with the low-half writes and the sign-extended 5-bit
  immediate.

## Evidence

- Decode and disassembly fixtures for hand-assembled forms and observed
  words from the pinned text (`0x4BE1E1BC` vmulax, `0x4B010841` vaddy with a
  single-lane mask, `0x4BF8A33C` vmove, `0x4A6103BE` vrsqrt, `0x4BC532FE`
  vopmula, `0x4BE1097D` vftoi4), plus the Unsupported classification of
  VCALLMS.
- An interpreter scenario with hand-computed values covering the element and
  accumulator arithmetic, the flag registers (including the reference's
  detail that the status register keeps its earlier mirrored high bits), the
  conversions, the moves, the division unit's Q publication and the integer
  forms' wrapping.
- **Application to the pinned text**: the unsupported word count over all
  1,334,917 words drops from 1,426 to **672** — and the scan also exposed
  that the last 0xAF0 bytes of the text section (0x616F28..0x617A14, 700
  words) are a **data table**, not code: its words only decode as instructions
  by coincidence. Excluding it, the real code region (0x00100000..0x616F1C)
  has **81 unsupported words**, all in three known families: the trapping
  arithmetic (70: DADDI 23, DSUB 16, ADDI 15, SUB 8, DADD 6, ADD 2), the
  MMI2/MMI3 parallel multiply (7) and COP0's missing BC0F (2), plus two words
  at unassigned function 0x28.
- The observed macro families before this slice came from a purpose-built
  probe (a scan grouping COP2 words by the two dispatch tables): the largest
  were vmove (83), vmulax (81), vmadday (77), vmaddw (57) and vnop (88) —
  all covered now.
- CTest 23/23; Python 71 collected (65 run, 6 skip).

## Limits recorded

- The reference itself notes its macro-mode flag handling is approximate;
  this model mirrors that behavior bit for bit rather than the presumed
  hardware.
- VU0 micro execution (VCALLMS) and the VU0 data memory forms remain
  unmodeled; those words stop with context.
- The translator does not emit the macro operations yet; its targets will
  say when they matter.
