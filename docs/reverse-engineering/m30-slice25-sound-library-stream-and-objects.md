# M30, twenty-fifth slice — the sound library's stream and its objects

Date: 2026-10-02. Inputs: the pinned CORE, the analysis ELF and the live
PCSX2 memory dump. Follow-up to the twenty-fourth slice (the stream of
13-byte records and the static object's odd data pointer). **No model
behavior changed**: this slice names the builder and the objects.

## The builder

The static object at 0x00623A50 is written by the engine's **sound
library** at 0x00462xxx:

- The setter 0x004627B0 writes the pair of statics the string objects take
  their defaults from: **0x00623A3C (the data pointer) and 0x00623A40 (the
  size)**; the setter 0x00462798 writes 0x00623A38 (a third default).
  Its callers are 0x00391CB4, 0x004612BC, 0x00462E44, 0x00462FB8 and
  0x00463054.
- **0x00463000 is the sound library's init** (guarded by the flag at
  0x00623A74): it sets the statics to {data pointer 0x008475C0, size
  0x1000}, assigns three bank names to a static array of string objects at
  0x008505C0 (+0x00, +0x24, +0x48) — `/sound/gt4sys.ins` (0x006AB898),
  `/sound/gt4race.ins` (0x006AB8B0), `/sound/gt4count.ins` (0x006AB8C8) —
  and then assigns **`/sound/roadnoiz.es` (0x006AB8E0) to the static
  object at 0x00623A50 with the flag 1**, which is the *relocating*
  assignment (the faulting path).
- The same init parses `/sound/gt4se.inf` (0x006AB8F8) through 0x004AE230
  and relocates the result with the same relocation idiom (0x004630E0:
  `old = *(s4+8); *(s4+8) = s4;` and every pointer at +0x10 stride 8 gains
  `s4 - old`), storing it at 0x00623A74.

## The stream

The statics pair means the string objects point **into a stream** at
0x008475C0 whose write position is 0x00623A3C and whose free space is
0x00623A40. The write watch of slice 24 caught the stream's records
(13 bytes each, cursor counting down by 13: 0x1000 → 0xFF3 → 0xFE6 →
0xFD9) — three records consumed 39 bytes, which is exactly the offset of
the object's odd data pointer (0x008475C0 + 0x27 = 0x008475E7) and the
difference in its size (0x1000 − 0xFD9 = 0x27). So the object's fields are
simply a *view* into the stream: the assign copies the current position and
free space into the object.

## What the next slice must find

Why the model's stream position is odd while the console's same-class
object ends up even (the live object at 0x00623A50 holds an even
0x008483C0 with size 0x200, and the live memory never contains
0x008475E7). Candidates: the record grammar (13 bytes = an archive entry
plus a tag byte; a 12-byte record would keep positions even), the stream's
contents (which differ if a sound file's data differs), or the path that
reaches the flag-1 assignment. The sound library's own records and the
archive's entry layout are the place to start.

## Verification

- No model behavior changed; the temporary instruments are removed and the
  tree is clean.
- CTest 34/34; Python 73 (67 run, 6 skip); the differential passes at
  3,000 services with the interpreter reference at 7,570,583 instructions
  and the full state identical.
