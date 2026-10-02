# M30, twenty-fourth slice — the engine's stream and its static object

Date: 2026-10-02. Inputs: the pinned CORE and ISO, the live PCSX2 memory
dump and the analysis ELF. Follow-up to the twenty-third slice, which
established that the boot's fault (an unaligned word access at 0x008475EB
through the pointer relocation 0x005595C8) is guest data, not a translation
divergence. **No model behavior changed**: this slice identifies the data
and the object behind the fault.

## What the write watch showed

A temporary watch on every write into the engine's static buffer
(0x00847580..0x00847700) and its static string object (0x00623A40..80),
run to the fault:

1. The buffer is cleared first (8-byte zero writes over its whole span).
2. Later, **byte** writes build a stream of **13-byte records** starting at
   0x008475CA: each record carries 0xD4 at +1 and 0x0D at +5 (the observed
   bytes: `00 D4 00 00 00 0D 00 00 00 00 00 00` at 0x8475D1, the same
   pattern at 0x8475DE and 0x8475EB, and 0x40 at 0x8475F3).
3. The **cursor** at 0x00623A40 counts the stream down by 13 per record:
   0x1000 → 0xFF3 → 0xFE6 → 0xFD9.
4. The static object at 0x00623A50 (the class whose vtable 0x00688868 is
   built at 0x004628C8, 0x0046132C and 0x00604D58) receives
   **+4 = 0x008475E7** (the data pointer, **odd**) and **+0xC = 0xFD9**
   (4057 — the stream's remaining size), and +0x6C = the vtable 0x00688868.

## What the live game shows

The live dump's object at 0x00623A50 holds **+4 = 0x008483C0** (even),
+8 = 0x60, **+0xC = 0x200** (512), +0x10 = 0x0008B060, +0x14 = 0x0001A7D0,
+0x1C = 0x00688868 — **the same class**, but an even data pointer and a
different size. The live memory never contains the value 0x008475E7. The
live buffer at 0x00847580 holds "INST" at 0x008475C0 and a structure at the
**aligned** 0x008475E0 ({0x4B4, 0x16F80, 0, "SShd", ...}).

## What the engine is doing

- The file-server trace shows only the version query (RPC 0xFF) before the
  fault — **no file reads** — so the stream is built from data the engine
  already has (the inner archives it read through the PCDV).
- The library at 0x00462xxx is the engine's file/name layer: 0x00462EC8
  looks a name up (the shared string compare at 0x005A5CF0) over the buffer
  at 0x00847580; 0x004612D0 builds a sound file name (the format strings
  `/sound/gt4race2.ins` at 0x006AB850 and `.ins` at 0x006AB4E0) and assigns
  it through 0x00462738 → 0x00462670.
- The fault's path is 0x004627E8 (the assign with the flag 1) → 0x00462670
  → the assign body 0x004625A8 → the relocation 0x005595C8, whose first
  read `*(a0 + 4)` lands at 0x008475EB because the object's data pointer is
  0x008475E7.

## What the next slice must find

The code that **builds the 13-byte record stream** (its records look like
an archive entry plus a tag byte; the odd stride makes every second stream
position odd) and why the console's object ends up with an even pointer —
the stream length, the record grammar, or the path that reaches this code.
The watched writes carry no pc, so the next instrument must tie the writes
to the code (a pc-carrying watch or a static scan for the 13-byte stride).

## Verification

- No model behavior changed; the temporary instruments are removed and the
  tree is clean.
- CTest 34/34; Python 73 (67 run, 6 skip); the differential passes at
  3,000 services with the interpreter reference at 7,570,583 instructions
  and the full state identical.
