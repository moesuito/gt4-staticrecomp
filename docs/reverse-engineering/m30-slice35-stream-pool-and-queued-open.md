# M30, thirty-fifth slice — the stream pool and the queued open

Date: 2026-10-02. Inputs: the pinned CORE and ISO and the live PCSX2 dump.
Follow-up to the thirty-fourth slice (the open handler's flow). **No model
behavior changed**: this slice rules out the stream allocation and pins how
the open is queued.

## The stream pool is not the failure

The stream factory 0x004AC660 pops a stream from the **free list at the
raw-disc handler +0xA8** (0x0084B480 + 0xA8 = 0x0084B528) under a lock and
initializes it. At the fault the list is:

```
model: 0x0084B528 -> 0x0062A0B4 -> 0x0062A078 -> 0
live:  0x0084B528 -> 0x0062A0B4 -> 0x0062A078 -> 0
```

**Identical to the console** — two streams free, the same addresses (the
streams are a *static array* at 0x0062A0xx). So the allocation does not
fail: the open's first early exit is not the one taken.

## The open is queued, not waited

The handler's third argument is the **formatter's context** (0x004AEFF0's
object): the open method stores the handler at +0x38 and, via 0x004AD300,
locks the handler's list and calls **0x0057CB00** — which is a **doubly
linked-list append** (`*(node+8) = 0; *(node+4) = *(list+4); ...`), not a
wait: the stream is **queued to the handler's pending list at +0x40**, and
the handler's own **worker thread** processes it later. The formatter then
reads the result from the **stream's +0x94**.

## So the failure is the worker's result

With the allocation ruled out, the failure is the worker leaving the
stream's +0x94 at 0 — the worker step 0x004AD4A0's search over the tree at
the handler's +0x58 (the archive's page tree) finding nothing, or the
worker never processing the queued stream. The stream's search key
(+0xA0/+0xA4) is zeroed by the context constructor, so the next slice
should instrument the worker's search (its key comparison and descent) and
the handler's pending list at the fault — and compare the stream's path
buffer (+0x48, built by the open) with the archive's tree.

## Verification

- No model behavior changed; the temporary dump is removed and the tree is
  clean.
- CTest 34/34; Python 73 (67 run, 6 skip); the differential passes at
  3,000 services with the interpreter reference at 7,570,583 instructions
  and the full state identical.
