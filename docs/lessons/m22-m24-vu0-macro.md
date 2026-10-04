# M22–M24 lesson — VU0 macro mode, trapping arithmetic, and the four words that remain

Prepared 2026-10-04. BUILD/VERIFY: as recorded in the sources —
M22: CTest 23/23, Python 71 (65 run, 6 skip); M23: CTest 23/23,
Python 71 (65 run, 6 skip); M24: CTest 23/23, Python 71 (65 run,
6 skip). See the
[M22 evidence](../reverse-engineering/m22-vu0-macro-moves.md),
[M23 evidence](../reverse-engineering/m23-vu0-macro-arithmetic.md),
and
[M24 evidence](../reverse-engineering/m24-trapping-and-parallel-multiply.md).
EXPLAIN: this is the worked explanation; tutoring review pending.

Every load-bearing statement below traces to one of those three
documents. The later reframing (M25) is noted explicitly at the end
and is a pointer, not a new claim.

## Objective and motivation

M21 closed with a number that looked like a verdict: 2,269
unsupported words over the whole text, dominated by one family —
the VU0 macro operations (COP2, ~1,250 words). The game does vector
math through a coprocessor the model had never named, so every such
word stopped with context. This arc teaches the project's
reference-mirroring motion: take the hardware documentation clone
(PCSX2 master, read as behavior description, no code copied), build
the register file first, then the moves, then the arithmetic, and
watch the unsupported count fall in three measured steps —
2,269 → 1,426 → 672 → 497 — until the real code region holds only
**4 words**. Along the way the scan exposes that the text
section's last 700 words were never code at all.

Two shapes motivate the arc beyond the count: a vector unit whose
register zero is a constant, not a register (M22), and arithmetic
that must stop instead of writing when it overflows (M24).

## Step 1 — the register file that is not quite a register file (M22)

M22 adds 224 operations in total, and the first job is state, not
instructions:

- **VU0 macro-mode state**: 32 vector registers of four 32-bit
  lanes, the integer register file, and the clip flag. Register 0
  is hardwired: the vector constant reads as (0, 0, 0, 1.0) and
  ignores writes, the integer register reads zero and ignores
  writes — matching the reference and the hardware.
- **QMFC2/QMTC2**: the full 128-bit moves between both GPR halves
  and the vector register file.
- **CFC2/CTC2** with the reference's special cases: the reciprocal
  register (20) reads through its mantissa mask (`& 0x7FFFFF`,
  upper word untouched) and keeps its exponent constant on writes
  (`| 0x3F800000`); MAC_FLAG (17), TPC (26) and VPU_STAT (29) are
  read-only; FBRST (28) masks to `0x0C0C` and its VU0-reset bit
  clears the whole VU0 register file; CMSAR1 (31) stops with
  context (running a VU1 sub-routine is not modeled) and the VU1
  bits of FBRST stop the same way; CLIP_FLAG (18) reaches both the
  shadow register and the VI entry, like the reference's
  fall-through; other registers store.
- **LQC2/SQC2**: the 16-byte quad moves between memory and the
  vector file, requiring 16-byte alignment, with the reference's
  detail that a load into the constant register still performs the
  access and discards it.
- **VNOP** decodes as the reference no-operation — its encoding
  (`0x4a0002ff`, SPECIAL2 index 47) is the single most frequent
  macro word in the text (88 occurrences).

The slice also decodes the macro dispatch for the next slice's
benefit: the VU macro encoding (opcode 0x12, rs 0x10–0x1F)
dispatches functions 0x00–0x3B through the standard table and
functions 0x3C–0x3F through a **packed secondary index**:
`(word & 3) | ((word >> 4) & 0x7C)` — which mixes the function's
low bits with the fd field, exactly as the reference computes it.
The observed text's dominant macro families map to VMULAx/y/z/w
(93+82+51+15), VNOP (88), and scattered VMULA/VMADDA/VSUBA
accumulate forms. That table, with the vector flag semantics, is
explicitly the next slice.

Evidence, as cited: decoder and disassembler fixtures for all seven
encodings; interpreter fixtures (the 128-bit round trip through
both GPR halves, a quad loaded from and stored to memory with
hand-set bytes, CFC2 sign-extension of a control value, and the
constant register reading as (0, 0, 0, 1.0)); application to the
pinned text, where the unsupported count over all 1,334,917 words
drops from **2,269 to 1,426**; the first 350,000 words remain at
zero.

Honest limits recorded in the source: the macro arithmetic
(VADD/VMUL/VMADD/VDIV/… including the flags) is not modeled yet
and those words stop with context; VCALLMS (running VU0 micro
code) and the VU1 controls stop the same way; the translator does
not emit the COP2 operations yet. And one supersession worth
keeping visible: the VU1 control bits of FBRST (`ctc2` to VI28
with bits 0x100/0x200) no longer stop — superseded 2026-10-02 by
M30 slice 8, because GT4 resets VU1 as part of its display setup
and, since the model executes no VU1 microcode, the reset has no
target state: the bits are recorded and execution continues
(decision 0010).

## Step 2 — the full macro table, and the data table hiding in the text (M23)

M23 adds 335 operations in total behind 111 newly named operations —
the complete VU0 macro instruction set:

- **Dispatch**: functions 0x00–0x3B index the reference's standard
  table; 0x3C–0x3F index the packed table with
  `(word & 3) | ((word >> 4) & 0x7C)`, which folds the destination
  field into the operation index. The write mask for the floating
  forms is the low four bits of rs. Slots the model does not
  execute stay Unsupported: VCALLMS/VCALLMSR (they run VU0 micro
  code) and VLQI/VSQI/VLQD/VSQD with VILWR/VISWR (they touch the
  VU0 data memory).
- **The float model** (`vuDouble`): denormal inputs flush to a
  signed zero, infinities clamp to ±(2−2⁻²³)·2¹²⁷ — the reference's
  default VU0 overflow setting (vu0Overflow enabled, TriAce
  add-sub hack disabled).
- **The flags**: `VU_MAC_UPDATE` classifies each result lane (sign
  bit, zero, denormal flush, overflow clamp) into the MAC flag with
  the reference's bit layout (x at bit 3 down to w at bit 0);
  `VU_STAT_UPDATE` aggregates one status bit per lane group; the
  arithmetic syncs both into VI[16] (status, keeping its high bits)
  and VI[17] (MAC) exactly like `SYNCMSFLAGS`.
- **The arithmetic**: VADD/VSUB/VMUL with elementwise, broadcast
  (x/y/z/w, Q, I) and accumulator destinations; VMADD/VMSUB with
  the same variants (result = acc ± fs·ft); VMAX/VMINI comparing
  integer representations; VOPMULA/VOPMSUB outer products.
- **The rest of the packed table**: integer/float conversions with
  their scale factors and saturating edge, VABS, VCLIPw (the
  shift-six clip flag update), VMOVE/VMR32, the division unit
  (VDIV/VSQRT/VRSQRT publishing Q through VI[22] with the 0x10/0x20
  status bits, VWAITQ as the no-op it is without a pipeline),
  VMTIR/VMFIR, the random generator (VRNEXT/VRGET/VRINIT/VRXOR and
  its LFSR), and the 16-bit integer forms VIADD/VISUB/VIADDI/VIAND/
  VIOR with the low-half writes and the sign-extended 5-bit
  immediate.

Evidence, as cited: decode and disassembly fixtures for
hand-assembled forms and observed words from the pinned text
(`0x4BE1E1BC` vmulax, `0x4B010841` vaddy with a single-lane mask,
`0x4BF8A33C` vmove, `0x4A6103BE` vrsqrt, `0x4BC532FE` vopmula,
`0x4BE1097D` vftoi4), plus the Unsupported classification of
VCALLMS; an interpreter scenario with hand-computed values covering
the element and accumulator arithmetic, the flag registers
(including the reference's detail that the status register keeps
its earlier mirrored high bits), the conversions, the moves, the
division unit's Q publication and the integer forms' wrapping; and
a purpose-built probe (a scan grouping COP2 words by the two
dispatch tables) showing the largest observed families — vmove
(83), vmulax (81), vmadday (77), vmaddw (57) and vnop (88) — all
covered now.

The slice's second discovery is load-bearing for everything after
it: the unsupported count drops from 1,426 to **672**, and the scan
exposes that the last 0xAF0 bytes of the text section
(0x616F28..0x617A14, 700 words) are a **data table**, not code —
its words only decode as instructions by coincidence. Excluding it,
the real code region (0x00100000..0x616F1C) has **81 unsupported
words**, all in three known families: the trapping arithmetic (70:
DADDI 23, DSUB 16, ADDI 15, SUB 8, DADD 6, ADD 2), the MMI2/MMI3
parallel multiply (7) and COP0's missing BC0F (2), plus two words
at unassigned function 0x28.

Limits, honestly kept: the reference itself notes its macro-mode
flag handling is approximate — this model mirrors that behavior
bit for bit rather than the presumed hardware; VU0 micro execution
(VCALLMS) and the VU0 data memory forms remain unmodeled and stop
with context; the translator does not emit the macro operations
yet, with its targets to say when they matter.

## Step 3 — arithmetic that stops instead of writing, and the four words that remain (M24)

M24 adds 349 operations in total and closes the two families M23
counted:

- **The six trapping arithmetic forms**: ADD, SUB, DADD, DSUB,
  ADDI and DADDI. The overflow checks mirror the reference bit for
  bit: the 32-bit method compares bit 31 against bit 32 of the
  64-bit sum of the sign-extended operands, the 64-bit method tests
  the sign-bit identity, and the subtract forms negate the right
  operand before the check — which also reproduces the reference's
  edge-case behavior for the most negative operand (a documented
  quirk, not a hardware-accurate check). The check runs before any
  register write, even when the destination is the zero register.
  When it fires, the step returns the stable Exception outcome at
  the offending word without touching the destination; the handler
  itself is not modeled, like SYSCALL and BREAK.
- **Eight parallel multiply/divide operations**: the halfword forms
  PMADDH, PMSUBH and PMULTH (eight signed halfword products filling
  the eight 32-bit lanes of the HI/LO pair, with the reference's
  packing of the low accumulator words into the destination), and
  the word forms PMULTW, PMULTUW, PMADDUW, PDIVW and PDIVUW (two
  32-bit lanes per pass, the second pass over the "1" accumulator
  pair; the divides publish quotient and remainder with the
  reference's special cases for $80000000/-1, zero divisors and
  unsigned overflow wraparound).
- The remaining unmodeled family members are recorded, not
  silently dropped: PMADDW and PMSUBW (which carry the reference's
  "division voodoo" corrections), PHMADH and PHMSBH, and PDIVBW —
  none of them used by the pinned text.

Evidence, as cited: decode and disassembly fixtures including the
observed words PMADDH at `0x70421409`, PMULTH at `0x704E0709`,
PMULTUW at `0x7181C329`, and the hand-assembled trapping and
parallel forms; interpreter scenarios with hand-computed values
(the trapping ops write their results, each overflow flavor stops
with the Exception outcome and the destination intact, PMADDH fills
the eight lanes and packs the destination, PMSUBH subtracts them
back to zero, the word forms store the signed/unsigned products in
both accumulators and both destination halves, and the divides
cover quotient, remainder, the second lane pair and the
zero-divisor path).

The payoff is the count the whole arc was driving toward:
unsupported words over the whole file drop from 672 to **497** —
and in the real code region (0x00100000..0x616F1C) only **4 words
remain**:

- two BC0F (`0x4100fffa`, twice), whose condition is the
  reference's DMA-derived COP0 condition line; the model has no DMA
  controller, so these stop with context instead of guessing;
- two words at unassigned function 0x28 (`0x00001028`, twice),
  inside the exception handler's register-dump code, unreachable in
  a model without the handler.

The remaining 493 words live inside the text section's trailing
data table (700 words at 0x616F28..0x617A14) and are not
instructions.

Limits, as recorded: the trapping forms stop at the boundary —
CP0 Cause/EPC/EXL are not fabricated because the handler never
runs in this model; BC0F/BC0T/BC0FL/BC0TL stay unsupported pending
an honest DMA model, since the observed sites rely on that
condition.

## What this arc does not claim (later reframing)

- **M25 — the translator reaches the macro and trapping
  operations.** The runtime executor is made public
  (`ee::execute_plain_effect`) and the translator falls back to it
  for every decoded plain operation with no inline form yet: the
  whole VU0 macro table, the COP2 moves and quad accesses, the
  remaining MMI forms and the trapping arithmetic. A trapping
  overflow stops the module at the instruction's address, exactly
  where the interpreter stops. The M22/M23 statement that "the
  translator does not emit the macro operations yet" is therefore
  superseded — not by inlining the whole table, but by executing
  the interpreter's own code path inside translated modules. See
  `m25-translator-runtime-fallback.md`.

Nothing here contradicts that later slice: this arc made the macro
table, the quad accesses, and the trapping/parallel operations
decodable and interpretable with hand-checked semantics; whether a
translated module *inlines* them or *calls the shared executor* is
a separate code-generation question M25 answers without changing a
single semantic this arc pinned.

## Connection to our implementation

The three sources pin behaviors, encodings, and unit names — not
file paths — so the table maps each piece to its representation
and its source instead of inventing locations:

| Piece | Representation (as cited) | Source |
| --- | --- | --- |
| VU0 macro-mode state | 32 vector regs × four 32-bit lanes + integer file + clip flag; reg 0 hardwired (vector (0,0,0,1.0), integer 0, writes ignored) | M22 |
| `QMFC2` / `QMTC2` | full 128-bit moves between both GPR halves and the vector file | M22 |
| `CFC2` / `CTC2` | recip-20 mantissa mask / constant exponent; 17/26/29 read-only; FBRST-28 `0x0C0C` mask + VU0-reset clears file; CMSAR1-31 and VU1 FBRST bits stop; CLIP_FLAG-18 dual write | M22 |
| `LQC2` / `SQC2` | 16-byte quad moves, 16-byte alignment; load into reg 0 accesses then discards | M22 |
| `VNOP` | `0x4a0002ff`, SPECIAL2 index 47; most frequent macro word (88) | M22 |
| Macro dispatch | opcode 0x12, rs 0x10–0x1F; 0x00–0x3B standard table, 0x3C–0x3F packed `(word & 3) \| ((word >> 4) & 0x7C)`; float write mask = low 4 bits of rs | M22/M23 |
| Float model + flags | denormal flush, ±(2−2⁻²³)·2¹²⁷ clamp; per-lane MAC bits (x→bit 3 … w→bit 0), status aggregation, sync into VI[16]/VI[17] | M23 |
| Macro arithmetic | VADD/VSUB/VMUL + VMADD/VMSUB (element/broadcast/accumulator), VMAX/VMINI by integer repr, VOPMULA/VOPMSUB | M23 |
| Division/random/integer | VDIV/VSQRT/VRSQRT publish Q via VI[22] (0x10/0x20 bits), VWAITQ no-op; VRNEXT/VRGET/VRINIT/VRXOR + LFSR; VIADD/VISUB/VIADDI/VIAND/VIOR, low-half writes, sign-extended 5-bit immediate | M23 |
| Trapping arithmetic | ADD/SUB/DADD/DSUB/ADDI/DADDI; reference bit-for-bit checks before any write; Exception outcome, dest intact, handler unmodeled | M24 |
| Parallel mul/div (8) | PMADDH/PMSUBH/PMULTH (8 halfword lanes); PMULTW/PMULTUW/PMADDUW/PDIVW/PDIVUW (two lanes/pass, "1" pair second; divide special cases) | M24 |
| Observed words | `0x4BE1E1BC` vmulax, `0x4B010841` vaddy, `0x4BF8A33C` vmove, `0x4A6103BE` vrsqrt, `0x4BC532FE` vopmula, `0x4BE1097D` vftoi4; `0x70421409`/`0x704E0709`/`0x7181C329` parallel forms | M23/M24 |
| Count + data table | 2,269 → 1,426 → 672 → 497; trailing 700-word table at 0x616F28..0x617A14; real region ends with 4 words (2× BC0F `0x4100fffa`, 2× `0x00001028`) | M22/M23/M24 |

## Understanding checkpoint

1. A load into vector register 0 still performs the memory access
   and discards the result. Why is "access, then discard" the
   correct semantics rather than skipping the access — and which
   `LQC2`/`SQC2` fixture pins it?
2. The packed dispatch index `(word & 3) | ((word >> 4) & 0x7C)`
   folds the destination field into the operation index. What goes
   wrong if a decoder treats functions 0x3C–0x3F through the
   standard table instead?
3. The float model flushes denormals and clamps infinities at one
   specific magnitude. Where do those choices come from, and why
   does the source record the reference's own flag handling as
   approximate?
4. The M23 scan reclassifies the text's last 700 words as a data
   table. What evidence distinguishes "code that happens to decode"
   from real instructions — and how does the count change on each
   side of that line?
5. A trapping add checks overflow before writing, even when the
   destination is the zero register. Explain why the check order
   matters observably, and what the Exception outcome preserves.
6. M22/M23 say the translator does not emit the macro operations
   yet; M25 runs them inside translated modules via the shared
   executor. Explain why that reframing changes no semantic this
   arc pinned — and which guarantee the shared code path gives
   that duplicated logic could not.
