# M30, thirty-third slice — the handler registration, and a correction

Date: 2026-10-02. Inputs: the pinned CORE and ISO and the live PCSX2 dump.
Follow-up to the thirty-second slice. **No model behavior changed**: this
slice finds the registration code and corrects the previous slice's
reading of the failure.

## The registration code

The game's own early init (0x00100D30–0x00100DB4, in the first four
kilobytes of text) constructs and registers the two archive handlers
through the wrapper 0x004ACC28 (which calls the constructor 0x004ACA40):

- **0x00617AB0** with a1 = 0, a3 = 0x0069BB98, **t0 = 0** (no prefix
  list), t1 = 1;
- **0x00617BB0** with a1 = 1, a3 = 0x0069BBA8, **t0 = 0x00617AA8** (the
  prefix list), t1 = 0.

So the model's +0xF4 = 0x00617AA8 on the first handler is **exactly the
game's own registration**, not corruption — and it points at the array at
0x00617AA8 whose first entry is the `/mpeg` string. The constructor also
builds the string "/" at 0x004ACA68 (0x006B00D0) and passes it to the init
0x004B1C10, which is why both the model's and the live handlers carry
**+0xAC = "/"**.

## The field comparison at the fault

The handler objects in the model and the live dump are nearly identical:

| field | model | live |
| --- | --- | --- |
| +0xA4 (vtable) | 0x00688C58 | 0x00688C58 |
| +0xAC | **0x006B00D0 ("/")** | 0x006B00D0 ("/") |
| +0xB8 (archive LBA) | 0x001BEF0 | 0x001BEF0 |
| +0xC8 (buffer) | 0x0090EA80 | 0x0090EA80 |
| +0xCC (size) | 0x59440 | 0x59440 |
| +0xF4 (first handler's list) | 0x00617AA8 | 0x00617AA8 |

The only differences are minor state fields (the first handler's +0xE4:
model 0, live 2; its +0xEC: live 0x001418C0 — the second layer's base —
unread in the model's line; the third handler's +0xF8/+0xFC tail).

## The corrected failure chain

The thirty-second slice read the failure as a wrong prefix list; the
comparison shows the prefix state is **the same as the console's**. The
match chain is:

1. node 0x00617BB0 (the layer-1 archive handler): its list {"/mpeg"}
   does not match the sound paths → match returns 0;
2. node 0x0084B480 (the raw-disc/PCDV handler): its match is a stub that
   always returns 0;
3. node 0x00617AB0 (the layer-0 archive handler): +0xF4 = 0, so the
   fallback compares its **+0xAC = "/"** with the path — a prefix match
   that succeeds for any path starting with "/" (the compare 0x004AE9E8
   returns 1 when the prefix is exactly "/").

So the parse **finds** a handler; the failure is inside the handler itself:
**0x004B1730** (the vtable-0x688C58 handler method at +0x40) returns 0 —
the archive's file open for `/sound/gt4sys.ins` fails in the model. The
next slice disassembles 0x004B1730 and follows its failure path.

## Verification

- No model behavior changed; the temporary dump is removed and the tree is
  clean.
- CTest 34/34; Python 73 (67 run, 6 skip); the differential passes at
  3,000 services with the interpreter reference at 7,570,583 instructions
  and the full state identical.
