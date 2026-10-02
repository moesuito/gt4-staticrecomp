# M30, twenty-sixth slice — the sound library's assign fills the stream

Date: 2026-10-02. Inputs: the pinned CORE and ISO, the analysis ELF and the
live PCSX2 memory dump. Follow-up to the twenty-fifth slice (the sound
library's stream at 0x008475C0 and the static object at 0x00623A50).
**No model behavior changed**: this slice pins *who* writes the stream and
*what* the faulting path is.

## The pc-carrying watch

A temporary watch carried the executing pc (a global set by the driver at
every module entry and interpreter step) and reported every write into the
sound library's buffers (0x00847180, 0x008475C0.. and the statics
0x00623A40..):

- **Every byte of the stream is written by the module entry 0x00462670** —
  the string/blob assignment function itself (its body 0x004625A8 calls the
  shared memcpy 0x005A4724). There is no separate writer: the assignment
  copies the source into the stream.
- The stream's free-space static (0x00623A40) is updated from the same
  function (pc 0x00462710): 0x1000 → 0xFF3 → 0xFE6 → 0xFD9, i.e. the
  position advances by the copied sources' lengths.
- The writes' bytes are binary (`00 D4 00 00 00 0D ...`), not text: the
  sources copied into the stream are **blobs** (the engine's parsed sound
  data), not file-name strings.

## The faulting path

The static object's assignment with the flag 1 (0x004627E8 → 0x00462670 →
the body 0x004625A8) does three things in order: it **relocates the
source** (0x005595C8), **memcpys the source into the destination**
`*(s2 + 4)` — the stream's current position — and then **relocates the
destination in place** (0x00462618 → 0x005595C8 with a0 = 0x008475E7).
That last relocation's first read `*(a0 + 4)` is the fault: the position
is odd.

The position 0x008475E7 is 0x008475C0 + 0x27: the entries copied before it
total **39 bytes**. The live game's same buffer holds "INST" and a
structure at the **aligned** 0x008475E0 — a preceding total of **32
bytes**. So the model and the console differ in the *sources copied before
the fault*, not in the mechanism: the assign's memcpy and the relocation
are the game's own code.

## What the next slice must find

Which sources (the blobs the engine builds from the sound data) are copied
into the stream before the faulting one, and why their total length is 39
bytes in the model against 32 on the console. The candidates are the
engine's parse of the sound files it has already read (the inner archives
through the PCDV) and any service answer that sizes or locates that data.

## Verification

- No model behavior changed; the temporary instruments are removed and the
  tree is clean.
- CTest 34/34; Python 73 (67 run, 6 skip); the differential passes at
  3,000 services with the interpreter reference at 7,570,583 instructions
  and the full state identical.
