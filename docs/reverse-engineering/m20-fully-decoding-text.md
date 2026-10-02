# M20 — Unaligned 64-bit access, the last decode gaps, and a fully decoding text

Date: 2026-10-02. Inputs: the pinned CORE. References: PCSX2 master
(`R5900OpcodeImpl.cpp` for the LDL/LDR/SDL/SDR merge tables), fetched
2026-10-01; used as documentation of hardware behavior.

## What was added (215 operations in total)

- **LDL/LDR/SDL/SDR**: the unaligned doubleword family, the 64-bit sibling of
  the M16 word family, with the reference merge tables (LDL/SDL shift the
  incoming value left through the low bytes 56..0; LDR/SDR shift it right into
  the low bytes 0..56; the masks preserve the bytes that are not replaced).
  655 words of the first 200,000 text words were blocked on exactly these
  four instructions.
- **BLEZL/BGTZL** (the likely zero-compare branches), **DADDIU** (the 64-bit
  immediate add) and **NOR** — the decode gaps the scan surfaced next.
- **PREF** decodes as the no-op hint it is, like CACHE.

The translator emits the whole unaligned family (its preamble now carries the
word and doubleword merge tables) with helpers mirroring the interpreter.

## The pinned text now decodes completely

A scan of the whole file-backed text (5,339,668 bytes = 1,334,917 words, four
350,000-word chunks spanning 0x00100000..0x00617E54):

```text
chunk 0x100000: 0 unsupported
chunk 0x1500000: 0 unsupported
chunk 0x2800000: 0 unsupported
chunk 0x3B00000: 0 unsupported
TOTAL unsupported: 0
```

Every word of the game's executable image is named and classified by the
model. That is a decode-coverage statement, not an execution one: the hints
are modeled as no-ops (documented) and behavior still depends on the modeled
services — but nothing in the image is unknown to the pipeline any more.

The session's progression on the first 200,000 words: 655 rejected words
before M20 → 63 after the doubleword family → 10 after BLEZL/BGTZL/DADDIU/NOR
→ 0 after PREF. The ten sampled regions started the session at 71.

## Evidence

- Hand-computed interpreter fixtures for the doubleword family: the ldr/ldl
  pair reassembles the eight bytes at an unaligned address (0xEEFF001122334455)
  and the sdr/sdl pair writes a value back across the same window
  (0xDDEEFF0011667788 / 0x99AABBCCDDAABBCC), every merged word checked against
  hand arithmetic. The first expectations written for the test were wrong in
  the arithmetic itself; the debug step that found it is recorded in the
  journal — the implementation matched the reference formulas in both cases.
- Decoder/disassembler fixtures for the eight new encodings.
- The Python CLI tests no longer have a decode-level unsupported example (none
  exists in the pinned text): they now exercise the trap boundary
  (0x001001c8, "ending=exception") and assert the clean region around the
  unaligned family; the translator's rejection test uses a trap-only seed
  ("no reachable instructions").
- CTest 21/21; Python 71 collected (65 run, 6 skip).

## Survey after the slice

The 0x58ce48 call tree remains blocked at the `beql …; break` idiom
(0x005baea4, a trap in a likely delay slot) — the recorded next structural
piece, with the design already sketched. Nothing else in its tree is unknown
to the decoder.
