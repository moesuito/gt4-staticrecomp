# M30, thirty-fourth slice — the archive open handler's flow

Date: 2026-10-02. Inputs: the pinned CORE. Follow-up to the thirty-third
slice (the match chain reaches the layer-0 archive handler). **No model
behavior changed**: this slice maps the open handler the failure lives in.

## The open handler 0x004B1730

The vtable-0x688C58 handler method at +0x40:

1. takes the stream object (its third argument) or **allocates one**
   (0x004AC660) — when the allocation returns 0 the method returns 0 (its
   only early exit);
2. stores the handler in the stream's +0x38, allocates a path buffer
   (0x004AC778 of strlen(path) + 2), and builds the full path from the
   handler's **+0xAC ("/")** and the requested path (0x004AE908);
3. **enqueues the stream to the handler** via **0x004AD300**, which locks
   the handler's queue (+0x40), sets the stream's state (+0x80 = 0) and
   **waits on the condition** 0x0057CB00 — the open is processed by the
   handler's own worker under the lock;
4. returns the stream object; the **result** the caller reads lives in the
   **stream's +0x94**.

So the failure the formatter sees (0x0044D740 reads `*(stream+0x94)`) is
either the stream allocation returning 0 or the worker leaving the result
at 0.

## The worker's search

The worker step **0x004AD4A0** walks a **sorted tree at the handler's
+0x58**, comparing the stream's key pair (+0xA0, +0xA4) against each node's
pair and descending — the archive's page tree (the GT4FS reference's
B-tree). On a match it stores the node and sets the stream's state (+0x80
= 2); the search's structure (the pair comparison, the left/right descent)
is the archive lookup the open performs.

## What the next slice must find

Whether the stream allocation or the worker's tree search is the failing
step, and why. The instrument to use: watch the stream's +0x94 (the
result) and +0x80 (the state) — the stream object is allocated
dynamically, so the watch must key on the handler's queue or on the
allocation (0x004AC660's return) rather than a fixed address. The archive
buffer holds the same bytes as the console's, so the candidates are the
stream's key (built from the path) and the tree's state.

## Verification

- No model behavior changed; the tree is clean.
- CTest 34/34; Python 73 (67 run, 6 skip); the differential passes at
  3,000 services with the interpreter reference at 7,570,583 instructions
  and the full state identical.
