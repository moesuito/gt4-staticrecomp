# M22 — VU0 macro mode, part 1: state, moves and the quad memory accesses

Date: 2026-10-02. Inputs: the pinned CORE. References: PCSX2 master
(`VU0.cpp` for the macro move semantics, `VU.h` for the register numbers and
the register file shape), fetched 2026-10-02; used as documentation of
hardware behavior.

## What was added (224 operations in total)

- **VU0 macro-mode state**: 32 vector registers of four 32-bit lanes, the
  integer register file, and the clip flag. Register 0 is hardwired: the
  vector constant reads as (0, 0, 0, 1.0) and ignores writes, the integer
  register reads zero and ignores writes — matching the reference and the
  hardware.
- **QMFC2/QMTC2**: the full 128-bit moves between both GPR halves and the
  vector register file.
- **CFC2/CTC2** with the reference's special cases: the reciprocal register
  (20) reads through its mantissa mask (`& 0x7FFFFF`, upper word untouched)
  and keeps its exponent constant on writes (`| 0x3F800000`); MAC_FLAG (17),
  TPC (26) and VPU_STAT (29) are read-only; FBRST (28) masks to `0x0C0C` and
  its VU0-reset bit clears the whole VU0 register file; CMSAR1 (31) stops
  with context (running a VU1 sub-routine is not modeled) and the VU1 bits
  of FBRST stop the same way; CLIP_FLAG (18) reaches both the shadow register
  and the VI entry, like the reference's fall-through; other registers store.
- **LQC2/SQC2**: the 16-byte quad moves between memory and the vector file,
  requiring 16-byte alignment, with the reference's detail that a load into
  the constant register still performs the access and discards it.
- **VNOP** decodes as the reference no-operation — its encoding
  (`0x4a0002ff`, SPECIAL2 index 47) is the single most frequent macro word in
  the text (88 occurrences).

## The macro dispatch, decoded for the next slice

The VU macro encoding (opcode 0x12, rs 0x10-0x1F) dispatches: functions
0x00-0x3B through the standard table (VADD/VMUL/VMADD/... element-wise,
VADDq/VADDi/... broadcast forms, VIADD/VIAND/VIOR, VCALLMS); functions
0x3C-0x3F through a **packed secondary index**: `(word & 3) | ((word >> 4) &
0x7C)` — which mixes the function's low bits with the fd field, exactly as
the reference computes it. The observed text's dominant macro families map
to: VMULAx/y/z/w (93+82+51+15), VNOP (88), and scattered VMULA/VMADDA/VSUBA
accumulate forms. That table (with the vector flag semantics) is the next
slice.

## Evidence

- Decoder and disassembler fixtures for all seven encodings.
- Interpreter fixtures: the 128-bit round trip through both GPR halves; a
  quad loaded from and stored to memory (`lqc2`/`sqc2` with hand-set bytes);
  CFC2 sign-extension of a control value; and the constant register reading
  as (0, 0, 0, 1.0).
- Application to the pinned text: the unsupported word count over all
  1,334,917 words drops from **2,269 to 1,426** (the moves, the quad accesses
  and VNOP account for the 843). The first 350,000 words remain at zero.
- CTest 23/23; Python 71 collected (65 run, 6 skip).

## Limits recorded

- The macro arithmetic (VADD/VMUL/VMADD/VDIV/... including the flags) is not
  modeled yet; those words stop with context. VCALLMS (running VU0 micro
  code) and the VU1 controls stop the same way. The translator does not emit
  the COP2 operations yet — its next targets will say when they matter.
- **Superseded 2026-10-02 (M30 slice 8):** the VU1 control bits of FBRST
  (`ctc2` to VI28 with bits 0x100/0x200) no longer stop. GT4 resets VU1 as
  part of its display setup, and since the model executes no VU1 microcode
  the reset has no target state: the bits are recorded and execution
  continues (decision 0010).
