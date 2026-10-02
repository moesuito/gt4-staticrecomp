# M30, thirty-first slice — the records come from the null pointer

Date: 2026-10-02. Inputs: the pinned CORE and ISO. Follow-up to the
thirtieth slice. **No model behavior changed**: this slice identifies where
the stream's 13-byte records come from, and the answer is decisive.

## The source addresses

A temporary instrument tracked the last guest read and printed it with
every byte written into the stream. The 52 lines show a clean pattern:

```
[w] 0x8475c0 = 0x0  src=0x0     [w] 0x8475cd = 0x0  src=0x0
[w] 0x8475c1 = 0x0  src=0x1     [w] 0x8475ce = 0x0  src=0x1
[w] 0x8475c2 = 0x0  src=0x2     ...
[w] 0x8475c4 = 0xd4 src=0x4     [w] 0x8475d1 = 0xd4 src=0x4
[w] 0x8475c8 = 0xd  src=0x8     [w] 0x8475d5 = 0xd  src=0x8
...
[w] 0x8475e7 = 0x0  src=0x0     (the fourth record, same pattern)
[w] 0x8475eb = 0x0  src=0x4
[w] 0x8475ef = 0xd  src=0x8
[w] 0x8475f3 = 0x40 src=0xc
```

The copy sources are **0x0 through 0xC** — the low memory. The assign is
copying from the **null pointer**: its source object (the formatter's
result) is **0**, so the assign copies `*(0 + 8)` = 13 bytes starting at
address 0. The "records" are simply the low memory's content: 0xD4 at +4,
0x0D at +8 — the values slice 30 reconstructed. The earlier memory scan
agrees: the "fourth" pattern exists at address 0x0.

## Why the source is null

The assign chain (0x00462738) takes the source from the **formatter
0x0044D740**, which returns the formatted object or **0 on a failed
parse** (its disassembly: `s0 = 0x4AE1F8(context, format); if (s0 == 0)
return 0`). In the model that formatter/resolver **fails**: the chain
receives 0, the assign copies from null, the records are the low memory,
their length field (13) comes from the low memory's byte at +8 (0x0D), and
the third copy lands at the odd stream position that makes the flag-1
assignment's relocation fault.

So the whole fault chain is:

1. the formatter/resolver 0x0044D740 fails (returns 0) for the sound-bank
   sources;
2. the assign copies 13 bytes from address 0 into the stream;
3. after three such copies the stream position is odd (39 bytes);
4. the flag-1 assignment relocates its copy in place and faults on the odd
   address.

## What the next slice must find

**Why 0x0044D740 fails.** Its parse (0x004AE1F8) and the formatter context
(0x004AEFF0) are the SDK's; the next slice reads them (and the format
strings' state in guest memory at the fault) to see what the resolver
expects — a valid format, a mounted volume, or a table the model has not
provided yet.

## Verification

- No model behavior changed; the temporary instruments are removed and the
  tree is clean.
- CTest 34/34; Python 73 (67 run, 6 skip); the differential passes at
  3,000 services with the interpreter reference at 7,570,583 instructions
  and the full state identical.
