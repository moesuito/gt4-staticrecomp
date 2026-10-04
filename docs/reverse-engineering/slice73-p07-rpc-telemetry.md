# Slice 73 - P07 RPC telemetry (observe, classify, stop loudly)

Date: 2026-10-04. Plan item: P07 (RPC rastreavel e estrito), PLAN.md
section 6. Decision: `docs/decisions/0035-p07-rpc-telemetry.md`.
Scope kept: inventory, honest per-pair classes, strict flag; no reply
byte changed; DMA/JR/clock/interrupts untouched; no fabricated
traffic; no SID-identification declared a protocol (plan section
4.4, item 6).

## What was built

- `Kernel::record_rpc_bind` / `record_rpc_call` (src/ee/kernel.cpp):
  every bind and call lands in telemetry maps with caller pc,
  thread, send/recv/result sizes, receive and server buffers, and
  the first four request words. Read-only against the guest.
- `Kernel::classify_rpc_pair` + `rpc_pair_class_name` +
  `rpc_pair_candidate_name` + `rpc_pair_note`: the five P07 classes
  with the consumer evidence per answered pair; named-but-unproven
  SIDs stay candidates, and the two disputed identities
  (0x80000400, 0x80001300) say so.
- `Kernel::set_strict_rpc` + `--strict-rpc` (tools/gt4boot/main.cpp):
  off by default (boot behavior identical); on, the first unknown
  pair throws before any reply is written, with pc, thread, SID,
  function, sizes, both buffers, the sd handle and request words.
- `--threads` prints the bind table and the per-pair table from the
  run itself; completion reads `sync-end-packet+dmac5`, callback
  reads `none-tracked` (that is the whole model today).
- Telemetry never enters the snapshot blob: `rpc_model` stays 1, no
  checkpoint bump, and both engines record identically, so the
  differential cannot see it.

## Measured legs (disc boot, MSVC 19.51 x64 Debug)

| Leg | Boundary | Module calls | Pairs | Unknown calls |
|---|---|---:|---:|---:|
| 400 services (+disc and no-disc) | syscall 0x005ADB94/0x2F | 983 | 1 | 0 |
| 3000 services | syscall 0x00001604/0x100 | 11160 | 22 | 32 |
| 20000 services | syscall 0x00001604/0x100 | 50985 | 22 | 32 |
| 90000 services | syscall 0x00001604/0x100 | 215013 | 22 | 32 |

The 90k module-call count (215013) is exactly the slice-71 census
leg, so this is the same execution the differential pins. The pair
set is stable from 3000 services on: init-phase traffic only. The
300k-service journal census (docs/journal/2026-10-02.md:956-959)
lists the later-phase pairs for the next slice: PCDV 1 (2686),
PBGM 8 (2472), SPUP 4 (2385), LGDEV 6 (2293), PCDV 3 (2225),
MCSERV 1/0x15 (537/479), LGDEV 0x0F/0x0D (287/278).

## The 90k inventory (all 22 pairs)

Classes: implemented-verified 2 pairs (23 calls), compat-constant
4 pairs (5 calls), provisional-explicit 0 observed, unknown 16
pairs (32 calls). Every caller pc is 0x005AE064 on thread 1: all
RPC traffic funnels through the game's single SIFRPC wrapper, so
the pc does not discriminate callers yet; SID, function, sizes and
buffers do.

| SID | Fn | Calls | Class | Send | Recv | Result |
|---|---|---:|---|---|---|---|
| 0x50434456 | 0x00000004 | 1 | implemented-verified | 64 | 64 | 8 |
| 0x80000006 | 0x00000000 | 22 | implemented-verified | 512 | 8 | 16 |
| 0x80000001 | 0x000000FF | 2 | compat-constant | 8 | 8 | 8 |
| 0x80000006 | 0x000000FF | 1 | compat-constant | 0 | 4 | 4 |
| 0x80000400 | 0x000000FE | 1 | compat-constant | 48 | 12 | 12 |
| 0x80001300 | 0x80001363 | 1 | compat-constant | 144 | 144 | 144 |
| 0x80000400 | 0x00000001 | 8 | unknown | 48 | 4 | 0 |
| 0x80000400 | 0x00000015 | 8 | unknown | 48 | 4 | 0 |
| 0x80000592 | 0x00000000 | 2 | unknown | 4 | 16 | 0 |
| 0x80001300 | 0x80001301 | 2 | unknown | 144 | 144 | 0 |
| 0x424B5550 | 0x00000000 | 1 | unknown | 12 | 0 | 0 |
| 0x45535550 | 0x00000000 | 1 | unknown | 12 | 0 | 0 |
| 0x50555354 | 0x00000000 | 1 | unknown | 64 | 64 | 0 |
| 0x50636476 | 0x00000000 | 1 | unknown | 64 | 0 | 0 |
| 0x54485550 | 0x00000000 | 1 | unknown | 12 | 0 | 0 |
| 0x62737550 | 0x00000000..05 | 1 each | unknown | 0 | 0 | 0 |
| 0x80000001 | 0x00000017 | 1 | unknown | 2076 | 4 | 0 |
| 0x80001300 | 0x80001304 | 1 | unknown | 144 | 144 | 0 |

Top unknowns by calls: (0x80000400, 1) x8 and (0x80000400, 0x15)
x8 on the disputed MCSERV identity, then (0x80000592, 0) x2 and
(0x80001300, 0x80001301) x2. Notable singles: (0x80000001, 0x17)
with a 2076-byte send on the established SIF manager, and the fn-0
pings on the string-coded workers (BKUP, ESUP, PUST, THUP, bsuP)
plus the Pcdv secondary channel. 23 SIDs are bound (all count 1
from thread 1 except 0x80000001 and 0x80000592 at count 2); the
bound-but-never-called SIDs (MPG1, MPG2, PBGM, PRTS, SPUP, SPUT,
VOIC, SMUP) show lifecycle without traffic: bound does not mean
used.

## The strict stop (first unknown, boot order)

`--services 3000 --strict-rpc` exits 1 with:

```text
FAILURE: Strict RPC stop: unknown pair sid 0x80000592 fn 0x00000000
at pc 0x005AE064 thread 1 (send 4 recv 16 recvbuf 0x006570C0
server-buf 0x00031000 sd 0x00020040 request 00000000 00000000
00000000 00000000)
```

The default leg answers the same pair silently and continues: the
only behavior change is the flag.

## P07 acceptance status

- First unknown identifiable: yes (16 pairs named, strict stops at
  the first in boot order).
- No silent global zeroed reply without name and owner: yes, every
  pair carries a class, a candidate name and a note; the generic
  path is now observed per pair.
- Valid error reply distinct from unimplemented: partial (the
  handle-0 open and pre-registration volume zeros are named inside
  implemented pairs; no pure absence-or-failure pair observed).
- Bind lifecycle and reset: binds counted per SID; reset behavior
  unchanged and unmodified by this slice.
- Provisional pairs unobserved at 90k (PCDV 1, LGDEV 12/4): their
  classes stand on earlier-slice evidence; the next slice extends
  the legs past 90k into the polling round.
