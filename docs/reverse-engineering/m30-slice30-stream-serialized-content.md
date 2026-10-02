# M30, thirtieth slice — the stream's serialized content, complete

Date: 2026-10-02. Inputs: the pinned CORE and ISO and the live PCSX2 dump.
Follow-up to the twenty-ninth slice. **No model behavior changed**: this
slice completes the stream's reconstruction and compares it with the live
game's buffer.

## The complete reconstruction

A write watch over the whole stream (0x00847180..0x008476C0) captured
**1,344 writes**. The stream at 0x008475C0 holds exactly four 13-byte
records and nothing else; the rest of the watched region is zero:

```
0x8475C0: 00 00 00 00 D4 00 00 00 0D 00 00 00 00   { 0, 212, 13, 0 }
0x8475CD: 00 00 00 00 D4 00 00 00 0D 00 00 00 00   { 0, 212, 13, 0 }
0x8475DA: 00 00 00 00 D4 00 00 00 0D 00 00 00 00   { 0, 212, 13, 0 }
0x8475E7: 00 00 00 00 00 00 00 00 0D 00 00 00 40   { 0,   0, 13, 64 }
```

So the records' small fields (0xD4 = 212, 0x40 = 64) are **not offsets into
the stream**: no string data exists at 0x008475C0 + 0xD4. The three bank
assignments produce identical records; the fourth (the flag-1
`/sound/roadnoiz.es` assignment that faults) differs in its second and
fourth words.

## The live comparison

The live dump's same buffer at the menu holds a *different* shape: "INST"
(the engine's uppercased extension, the sound library's case-folding code
at 0x00462EC8) followed by zeros and {00 05 00 00}. It is a later moment
(the menu, long after the boot), so it shows what the stream looks like
once the sound system is running — not the boot-time serialization the
model is in.

## What the records are

The assign copies the source object's first `*(source+8)` bytes — for
these records 13 — so each record is the **head of a serialized object**:
{+0: a flag (0), +4: a value (212 or 0), +8: the length (13), +0xC: a value
(0 or 64)}. The flag-1 assign then relocates the copy in place, converting
the serialized small values into absolute pointers; that relocation's first
read at the odd position (0x008475E7, after 39 bytes of preceding records)
faults, while the console's buffer has 32 bytes before its aligned
structure.

The next slice should identify the source objects the assign serializes
(their content is observable in memory) and why the model's serialized
head is 13 bytes where the console's layout leads to an even position —
the candidates are the sound library's own object constructors
(0x00462900, 0x00463600) and the objects the sound init builds for the
three bank names.

## Verification

- No model behavior changed; the temporary instrument is removed and the
  tree is clean.
- CTest 34/34; Python 73 (67 run, 6 skip); the differential passes at
  3,000 services with the interpreter reference at 7,570,583 instructions
  and the full state identical.
