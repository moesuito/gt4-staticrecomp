# 0014 — The boot's service handshakes and the model IOP's server table

Status: implemented 2026-10-02 for the M30 thirteenth slice
(`docs/reverse-engineering/m30-slice13-service-handshakes.md`).

Context: after the handler-execution fix the game ran continuously but lived
in delay/retry loops. Tracing them showed the file-open path failing at a
version check, and once that was answered the boot walked through a chain of
service handshakes the model did not satisfy: a debug interface, a
subsystem status query, a fileio/CDVD version negotiation, and more servers
than the model's table held.

Decision:

- **Version queries answer the game's own compatibility constants.** The
  model reads them from the guest memory, so the answer always matches what
  the game's own check compares:
  - the SIF manager (sid 0x80000001, RPC 0xFF) answers the word at
    0x0066829C (0x00275520) plus the flag 2 its client checks (the previous
    zeros only passed because the manager's check is non-fatal);
  - the file server (sid 0x80000006, RPC 0xFF) answers the four bytes at
    0x0065829C ("3000"), which the file-open check at 0x005B6368 compares.
  An unmapped constant stops the model instead of answering zeros.
- **Deci2Call (0x7C)** is accepted with the reference emulator's returns
  (1 for the defined calls, -1 beyond 0x10): no debug host is attached, and
  the game's debug output has no destination, exactly as on a console
  without one.
- **The disc subsystem's status query** (sid 0x80001300, RPC 0x80001363)
  answers 144 bytes whose first word is 0x310, the lowest value the game's
  check accepts (`(word >> 4) == 0x31` at 0x0058F840). The rest of the
  payload is zero. The real status structure comes from the game's IOP
  server, which the model does not execute.
- **The fileio/CDVD version negotiation** (sid 0x80000400, RPC 0xFE) answers
  12 bytes with the minimum versions the checks accept (second word 0x20A,
  third word 0x20E; 0x0058D674 and 0x0058D694). Answering the minimums makes
  the game choose the oldest protocol variant it supports, which is the
  variant the model can answer with empty results.
- **The model IOP's server table holds 80 servers.** The game binds more
  servers than the previous 16 slots (including one with a bogus string
  sid), and running out is a model failure, not a game condition. Handles
  live at 0x00020000 + slot*0x10, buffers at 0x00030000 + slot*0x1000 and
  connections at 0x00090000 + slot*0x1000 — all below the game's image,
  keeping the 0x00080000 command-buffer address free between them.

Alternatives considered:

- **Hardcoding the constants** instead of reading the game's data: the
  values would drift from the pinned image; reading them keeps the answer
  and the game's check in lockstep (and an unmapped read stops loudly).
- **Answering the negotiation with "large" versions**: minimums keep the
  game on the oldest protocol, which the empty-result model can represent
  without inventing newer semantics.
- **Growing only the handles region**: the buffers and connections need
  address space too, so the whole scratch layout moved together.

Consequences and limits:

- The file-open path proceeds, the boot binds the disc subsystem's servers
  (0x80001300/0x8000131C/0x8000131E/0x8000131F), the fileio/CDVD
  negotiation passes, and the game creates its worker-thread pool: an
  11-thread runtime with string-coded servers ("Pusb", "PUPS", "MGBP", ...)
  before the run reaches the 200,000,000-step limit inside the 0x0058F000
  subsystem init.
- The model still answers every other RPC function with an empty result; the
  next frontier is the retry loop around 0x00590A18 that follows.
- The 80-slot table is a model ceiling; exceeding it stops with context.

Verification:

- CTest **32/32**; Python 73 (67 run, 6 skip).
- `gt4boot --compare-interpreter` at 3,000 services: interpreter reference
  at 7,573,241 instructions, full state identical.
