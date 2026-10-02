# M15 — COP1, MMI, and the game's startup under the interpreter

Date: 2026-10-01. Inputs: the pinned CORE from `docs/inputs/usa-v2.00.json`;
the analysis ELF and Ghidra setup are those of `m6-disassembly.md`.

## What was added

The decoder/disassembler/interpreter/state went from 45 to 173 operations:

- **COP1 scalar FPU (35)**: `mfc1` `cfc1` `mtc1` `ctc1`, `lwc1` `swc1`,
  `add.s` `sub.s` `mul.s` `div.s` `sqrt.s` `abs.s` `mov.s` `neg.s` `max.s`
  `min.s` `rsqrt.s`, the accumulator forms `adda.s` `suba.s` `mula.s`
  `madda.s` `msuba.s` and `madd.s` `msub.s`, compares `c.f` `c.eq` `c.lt`
  `c.le`, conversions `cvt.s.w` `cvt.w.s`, and `bc1f` `bc1t` `bc1fl` `bc1tl`
  (classified as branches, read from the FCR31 condition bit).
- **MMI (73)**: the four parallel tables' lane arithmetic (wrap, signed
  saturating and unsigned saturating), compares, min/max, absolute, logic,
  immediate and variable shifts, extract/unpack, interleave/copy/rotate,
  `pmfhi` `pmflo` `pmthi` `pmtlo` `pmfhl` `pmthl` `qfsrv`, plus
  `mfhi1` `mflo1` `mthi1` `mtlo1`.
- **Special registers**: `mfhi` `mflo` `mthi` `mtlo` with the full second
  HI/LO bank, `mtsa` `mtsab` `mtsah` and `sync`.
- **LQ/SQ** (128-bit moves, silently 16-byte aligned) with the guest state's
  upper-half GPR storage; MMI and LQ/SQ touch all four 32-bit lanes while
  ordinary 64-bit results leave the upper half unchanged.

## References (independent sources)

- PCSX2 master, fetched 2026-10-01: `pcsx2/R5900OpcodeTables.cpp`
  (encodings), `pcsx2/FPU.cpp` (COP1 behavior), `pcsx2/MMI.cpp` (MMI
  behavior), `pcsx2/R5900OpcodeImpl.cpp` (HI/LO, MTSA family, SYNC). Used as
  documentation of hardware behavior; no code was copied.
- Ghidra 12.1.3 `MIPS:LE:64:64-32addr` for the base-language comparison.

Behaviors mirrored deliberately (recorded because they diverge from plain
IEEE 754):

- Denormal inputs read as signed zero; infinite or NaN inputs read as the
  largest finite value with the sign preserved.
- Infinite results clamp to the largest finite value and set O|SO; denormal
  results flush to signed zero and set U|SU; zero/zero sets I|SI and a nonzero
  dividend divided by zero sets D|SD, both producing a saturated quotient with
  the exclusive-or sign.
- `sqrt.s` reads only `ft`; `rsqrt.s` computes `fs / sqrt(ft)`; compares
  set/clear only the C bit (bit 23); `c.f` clears it.
- `cfc1` reads FCR0 as 0x2e00 and FCR31 sign-extended; `ctc1` writes only
  FCR31. The disassembler names the control registers `fir`/`fcsr` and prints
  the `sync` completion code, matching the base language.
- MMI saturations use the reference's exact comparisons (for example
  `psubsw` clamps at `>= 0x7fffffff`, `paddsw` at `> 0x7fffffff`); `pmfhl`
  implements all five layouts including the `.slw`/`.sh` clamps; `qfsrv`
  takes its amount from the `mtsa` cache.

## The startup block now decodes completely

The previously rejected 71 words of the entry window resolve to:

- `0x00100008` (69 words): 29× `padduw rN, r0, r0` clearing r1..r29, then
  `mthi`/`mthi1`/`mtlo`/`mtlo1`, `mtsah r0, 0`, 32× `mtc1 r0, fN`, one
  `adda.s` clearing the FPU accumulator, `sync`, and `ctc1 r0, f31`.
- `0x0010011c` (31 words): the .bss align/clear loops
  (`sq zero, 0(v0)` in 16-byte steps plus `sb` tails) from 0x006D5E00 to
  0x008A215C.
- `0x00100198` and `0x001001e8`: argument setup, the syscall boundary at
  0x001001C8, and later `jal` chains that stop at the unmodeled COP0 `ei`.

Verified numbers (reproducible):

- Ghidra comparison over the ten M6 regions, the four candidate ranges and
  the five startup regions — 548 rows:
  `matched=511 non_nop=442 unsupported=3 r5900_only=34 mismatched=0`.
  The `r5900_only` bucket is explicit in `CompareDisassembly.java`: rows whose
  mnemonic is an R5900 extension the base language cannot express (MMI,
  accumulator FPU forms, RSQRT.S, the shift cache, the HI/LO second bank,
  LQ/SQ — whose opcode MIPS64r2 reuses for SPECIAL3 `ext`). Those are
  covered by the reference tables instead. Two earlier rows were formatting,
  not semantics: `fcsr`/`fir` naming and the `sync` completion code.
- Remaining unsupported in the sampled regions: `div`, `break`, and the COP0
  `ei` — the multiply/divide family and COP0 are later slices.
- CTest 17/17 including the new `ee_startup_run` test (needs the local CORE):

```text
startup ran 942695 instructions from 0x100008 to the first BIOS syscall
(0x1001c8) with .bss cleared
```

The test pre-fills the .bss neighborhood with junk, so the clearing is
observed: the window comes back zero, the margin bytes survive, and the
HI/LO banks, FCR31, the FPU accumulator and all 32 FPU registers stay
cleared. The prologue's expected effects are also covered by a unit fixture
rebuilt from the encodings (no game words committed).

## Limits recorded

- No MULT/MULTU/DIV/DIVU/MADD/MADDU or the MMI multiply/divide family
  (PMULT*/PMADD*/PHM*/PDIV*/PLZCW) yet.
- `qfsrv` with a cached shift of 128 or more throws; `pmfhl`/`pmthl`
  selectors beyond the defined layouts throw — no silent fallbacks.
- FPU flag maintenance covers C/O/U/SO/SU and the D/I special cases; inexact
  is not tracked for ordinary operations (the reference does not either).
- The translator does not emit the new operations yet; the interpreter is the
  execution engine for this code. Extending `gt4translate` to the startup
  block is the next slice.
