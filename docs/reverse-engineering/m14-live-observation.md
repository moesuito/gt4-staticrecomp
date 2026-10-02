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

- This validates the loaded image, not our execution semantics.
- PINE exposes no CPU registers; register-level comparison needs savestate
  parsing (13.4 MB `.p2s`, version-specific format) or the debugger — a future
  slice.
- Our interpreter still stops at COP1/MMI words, so differential execution
  over real code needs broader decoding first.

Artifacts stay local: `private/pcsx2/text-ram.bin` (dump),
`private/pcsx2/ram-text-comparison.txt`; distributable metadata only in
`docs/inputs/usa-v2.00-live-ram.json`.
