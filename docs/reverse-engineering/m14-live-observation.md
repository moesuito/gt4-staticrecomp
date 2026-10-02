# M14 evidence — first live observation through PCSX2 PINE

2026-10-01: BUILD/VERIFY passed for this slice. EXPLAIN lesson pending.

## Setup

- PCSX2 nightly `2.9.93` at `F:\Games\PS2` with BIOS dumps (v7/v12/v15/v18;
  v18 `SCPH-90001` configured). PINE was enabled by flipping `EnablePINE` to
  true in `Documents/PCSX2/inis/PCSX2.ini` (original kept as
  `PCSX2.ini.bak-gt4recomp`; PCSX2 persists the setting on clean exit).
- New tool `scripts/pcsx2_pine.py` implements the protocol from the PCSX2
  sources: batched requests (up to 32768 `Read64` per round trip), status,
  version, title, serial, savestate save/load, and `verify-elf` which compares
  a local ELF segment against live EE memory. It never writes memory; only the
  explicit `save-state`/`load-state` commands change emulator state.
- The protocol work is covered offline: the Python suite includes a fake PINE
  server exercising socket, batching and unaligned read slicing, plus encoders,
  reply parsing and ELF extraction (one parser bug, wrong `e_phoff`-relative
  offsets, was caught by the synthetic ELF fixture before any live use).

## Boot and identity

The pinned USA v2.00 ISO boots with `-batch -slowboot`; PINE reports title
`Gran Turismo 4` and serial `SCUS-97328` — the exact pinned revision.

## The comparison that matters

Live EE main RAM at the main menu, compared against our reconstructed analysis
ELF (byte-identical to the pinned M4 hash):

| Record | Range | Size | Result |
| --- | --- | ---: | --- |
| text | `0x00100000` | 5,339,668 | **5,339,668 matching, 0 differing** |
| reginfo | `0x006179fc` | 24 | 24 matching, 0 differing |
| data | `0x00617a80` | 779,132 | 736,471 matching, 42,661 differing |

The text dump and the ELF segment hash equal exactly:
`5a9a9107b146b7d533a2a2421cdf900a913d5ced4e97892798cfa810b3dd2d34`.

## The data segment: runtime writes, not a reconstruction error

A cold-boot time series of the data record explains the differences:

```text
t0 (serial visible)   351,057 differing   (still loading)
t0 + 15 s             351,057 differing
t0 + 45 s              42,606 differing   (steady state)
menu (separate boot)   42,661 differing
```

The count falls as loading completes and settles at the runtime-modified
steady state. The first differences are exactly what running code writes into
writable data: pointers into the image (`b0 7a 61 ...`), `0xff` sentinels.
Conclusion: after loading, the data record equals our reconstructed bytes
except where the game itself has written; there is no evidence of load-time
transformation of the static image.

## Reusable observation anchor

PINE savestate slot 9 (`SCUS-97328 (77E61C8A).09.p2s`, 13,453,263 bytes) holds
the main menu and is restorable with `load-state 9`; the owner independently
saved slot 1 of the same menu. Future experiments can start from an exact,
repeated state instead of re-navigating the game.

## Meaning

This is the strongest validation of the M4 reconstruction to date: not against
another tool, but against the game actually executing under an independent
emulator. It also establishes the observation channel (RAM reads, savestates)
that later milestones use for differential work.

## Limits

- This validates the loaded image and the register channel; it is not
  execution-proof of our semantics.
- Savestate parsing is offline and precise (see slice 2); live single-stepping
  remains unsolved.
- Our interpreter still stops at COP1/MMI words, so differential execution
  over real code needs broader decoding first.

Artifacts stay local: `private/pcsx2/text-ram.bin` (dump),
`private/pcsx2/ram-text-comparison.txt`; distributable metadata only in
`docs/inputs/usa-v2.00-live-ram.json`.

## Slice 2 — CPU state from savestates (2026-10-01)

A savestate turned out to be a ZIP container: a version entry, memory blobs
(`eeMemory.bin` is the full 32 MiB EE RAM) and `PCSX2 Internal
Structures.dat`, the raw freeze stream. `Freeze()` copies values verbatim, so
the stream contains host structs: the `cpuRegs` block follows a 32-byte
zero-padded tag and holds `struct cpuRegisters` — GPR[32] in 16-byte slots
(first 8 bytes are the 64-bit value), HI, LO, CP0 (32 words), then `pc` at
offset 680. New tool `scripts/pcsx2_savestate.py` reads it (`info`,
`registers`, `extract`); Python 3.14's zipfile reads the Zstandard entries
natively.

Real evidence from the menu savestate (slot 9):

- `pc = 0x00568b94`, word at pc `0x1520004a` (a `bne t1, zero, +0x4a` with
  t1 = 0), `ra = 0x00568acc` — both inside the loaded text;
- `sp = 0x0113fa90` inside RAM, `gp/fp/s0/s1` inside the data window
  (0x65xxxx-0x6ddxxx), `a1 = 0x70000000` (the hardware scratchpad);
- `cp0.status = 0x70030c11` (kernel mode, interrupts enabled).

Every value is consistent with a running game — the register channel works.
The savestate's own `eeMemory.bin` re-verified the text image offline:
5,339,668 bytes, 0 differing.

A fixture incident is recorded: the synthetic savestate fixtures initially
used a 31-byte tag (7 + 24 instead of 7 + 25 zeros), shifting every decoded
field by one byte; the synthetic tests caught it, the parser was correct (real
tags are 32 bytes), and the fixtures now mirror the real layout exactly
(biosdesc, tag offsets 0 and 322).

Limits: one snapshot; no instruction stepping (the emulator cannot be told to
advance exactly N instructions); the freeze layout is coupled to the emulator
build and must be re-verified per PCSX2 update; TLBs and the rest of CP0 are
not decoded yet.
