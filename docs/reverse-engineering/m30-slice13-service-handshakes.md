# M30, thirteenth slice — the boot's service handshakes

Date: 2026-10-02. Inputs: the pinned CORE. Follow-up to the twelfth slice
(`m30-slice12-handler-execution.md`), whose frontier was the game living in
library wait/retry loops once the handler execution was fixed. This slice
traces those loops to the service handshakes that gate the game's
initialization and answers them from evidence (decision 0014). The boot now
binds the disc subsystem, negotiates the fileio/CDVD versions, creates its
worker-thread pool (an 11-thread runtime) and reaches the 200,000,000-step
limit inside the 0x0058F000 subsystem init; the differential passes at
3,000 services with the interpreter reference at 7,573,241 instructions and
the full state identical.

## The retry loop, traced

- The hot sleep caller chain was `0x00577578 → 0x00577FB8 → 0x005AED18`
  (the delay helper): a retry loop at 0x00577570 that calls 0x005B6C68
  (the file open), delays 2,000 and retries while the open fails.
- 0x005B6C68 → 0x005B6A40 ensures the file server is bound (sid
  0x80000006) and checks its version (0x005B6368): the version query (RPC
  0xFF) must match the game's constants at 0x0065829C ("3000") or
  0x0066835C. With the model previously answering zeros, the open failed
  before sending and the loop never ended.
- With the version answered, the open calls flowed (sid 0x80000006 RPC 0,
  512 bytes in, 8 out), and the run advanced to a new wall: **Deci2Call
  (0x7C)**, the debug interface. It is accepted with the reference
  emulator's returns.
- The next subsystem init (0x0058F780) binds a family of servers
  (0x80001300, 0x8000131C, 0x8000131E, 0x8000131F), queries the disc
  subsystem's status (sid 0x80001300 RPC 0x80001363, 144 bytes) and checks
  `(reply[0] >> 4) == 0x31` at 0x0058F840. The model answers 0x310, the
  lowest accepted value.
- The fileio/CDVD version negotiation (sid 0x80000400 RPC 0xFE, 48 bytes in
  and 12 out) checks that the reply's second word is at least 0x20A and its
  third at least 0x20E; the model answers the minimums.
- Running out of the model's 16 RPC server slots was a model failure
  (`The model IOP ran out of RPC server slots`) after the game bound many
  servers, including a bogus string sid (0x50636476). The table now holds
  80 servers with a reorganized scratch layout.

## The resulting run

```
--services 60000 --threads        (ends at the step limit)
boundary: step-limit 0x00590A18
stats: module calls 39938569, interpreted steps 160061431, services handled 4235
thread 4..11: entry 0x005786f0, priorities 0..14 (the worker pool)
```

The RPC traffic at the tail is the game's subsystem family with string-coded
server ids — 0x5042474D ("MGBP"), 0x53505550 ("PUPS"), 0x62737550 ("Pusb"),
0x54485550 ("PUHT"), 0x45535550 ("PUSE"), 0x424B5550 ("PUBK"), 0x50555354
("TSUP") — with fixed-size requests and replies (12, 64, 128, 960 bytes).

## The next frontier

The run ends at the step limit inside the 0x0058F000 subsystem init
(0x00590A18), not at a deadlock: the game is executing. The next slice should
characterize the loop around 0x00590A18 and the string-coded servers it
drives, and answer the first of their calls whose reply the game acts on.

## Verification

- CTest **32/32** (the version-query answers and the Deci2Call returns in
  `ee_kernel`); Python 73 (67 run, 6 skip).
- `gt4boot --compare-interpreter` at 3,000 services: interpreter reference
  at 7,573,241 instructions, full state identical (registers, HI/LO, FPU,
  VU0, CP0, pc, memory digest).
