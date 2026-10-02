# M30, twenty-seventh slice — the stream's content reconstructed

Date: 2026-10-02. Inputs: the slice-26 write-watch log (temporary
instrument, removed) and the pinned CORE. Follow-up to the twenty-sixth
slice. **No model behavior changed**: this slice reconstructs what the
sound library copied into the stream and names the copying path.

## The reconstructed stream

The write-watch log records every byte written into the sound library's
buffers, so the stream's content can be replayed exactly:

```
0x8475C0: 00 00 00 00 D4 00 00 00 0D 00 00 00 00 00 00 00
0x8475D0: 00 D4 00 00 00 0D 00 00 00 00 00 00 00 00 D4 00
0x8475E0: 00 00 0D 00 00 00 00 00 00 00 00 00 00 00 00 0D
0x8475F0: 00 00 00 40 ...
```

The **13-byte records** are `{00 00 00 00 D4 00 00 00 0D 00 00 00 00}` —
the first three are byte-identical, and the **fourth** (at 0x8475E7, the
faulting position) differs: `{00 ×8, 0D, 00 00 00, 40}`. The fault hits
while the fourth record's relocation runs.

The second stream (0x847180, the sound library's other arena) received the
same header shape and then, at 0x8471D0, six **relocated pointers** all
equal to 0x008471A0 — the relocation idiom writing a structure's fields
back with the new base.

## The copying path

The assign (0x00462738) does not copy the caller's string directly: it
first asks the getter 0x00462588 for the destination's current data, then
calls **0x0044D740**, a printf-style formatter that allocates its result
from the SDK's arena (0x004AEFF0 / 0x004AE1F8), and only then assigns the
formatted object through 0x00462670 (whose body memcpys `*(source+8)`
bytes). So the 13-byte records are **formatted arena objects**, not the
file-name strings the callers pass: the sound bank names
(`/sound/gt4sys.ins`, `gt4race.ins`, `gt4count.ins`) go through the
formatter first, and the fourth assignment (`/sound/roadnoiz.es`, the
flag-1 relocating one) is the one that faults.

## What the next slice must find

What the formatter produces for those names (the 13-byte shape
`{..., D4, ..., 0D, ...}` and the fourth record's `{..., 0D, ..., 40}`)
and why the total copied before the fault is 39 bytes in the model against
32 in the console's buffer. The instrument to use is one that also reports
the call site (ra) and the source pointer of each assign, or a watch on the
formatter's arena.

## Verification

- No model behavior changed; the tree is clean.
- CTest 34/34; Python 73 (67 run, 6 skip); the differential passes at
  3,000 services with the interpreter reference at 7,570,583 instructions
  and the full state identical.
