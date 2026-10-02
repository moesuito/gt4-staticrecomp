# 0012 — Semaphore handles carry bits 0 and 1

Status: implemented 2026-10-02 for the M30 tenth slice
(`docs/reverse-engineering/m30-slice10-semaphore-handles-and-the-delay-library.md`).

Context: the ninth slice's frontier had every thread waiting on semaphores
created by the game's delay helper (0x005AED18). Tracing the library that
schedules the delay's callback showed two operations on the value the helper
passes as the callback's "common" argument — the semaphore id:

- 0x005B8B68 decides whether to activate the node with `flags & 1`, where
  `flags` is the caller's value with bit 1 set (`ori v0, v1, 0x2`).
- The dispatcher 0x005B8ED8 calls the node's handler with that same value as
  its fourth argument; the handler 0x005AEF58 runs `iSignalSema(argument)`.

Both operations are only harmless when the kernel's semaphore handle already
has bits 0 and 1 set: otherwise the activation is skipped for half the
handles, and the signal targets `handle | 2` instead of `handle`.

Decision:

- **The model hands out semaphore ids with bits 0 and 1 set**: the first id
  is 3 and each next id advances by 4 (3, 7, 11, ...). The exact real-kernel
  handle format is not documented in the pinned sources; this is the shape
  the game's own code demonstrably requires. Everything else about the ids
  is unchanged (they are opaque values compared for equality).

Alternatives considered:

- **Keeping sequential ids.** The game's own bit operations would keep
  corrupting them; half of the delay nodes would never activate and every
  signal would miss its semaphore.
- **Masking the bits inside the library.** Not possible: the library is
  guest code; the model cannot change it.

Consequences and limits:

- The boot run advances from 3,645 to 9,765 services (672,586 interpreted
  steps) with the same differential pass; the game creates and uses many
  more semaphores (up to id 147) and its thread set evolves.
- The delay callback chain still does not complete; the recorded frontier is
  the timer library's node processing (evidence document). The handle shape
  is one necessary condition, not the whole chain.
- If a future kernel-service milestone documents the real handle format, the
  allocation must be revisited against that evidence.
