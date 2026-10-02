# M30, fourteenth slice — the SIF register mirror and the loading path

Date: 2026-10-02. Inputs: the pinned CORE. Follow-up to the thirteenth slice
(`m30-slice13-service-handshakes.md`), whose run ended at the step limit
inside the game's SIF command-layer initialization. This slice traces that
spin to the software-register handshake, adds the register mirror, and
clears the next wall: the liblgdev device sync. The boot now binds the
game's disc device library, passes the sync and runs its device polling
round to the service limit (1,000,000 services with 1,710,779 module calls
and 46,608,011 interpreted steps). The differential passes at 3,000 services
with the interpreter reference at 7,573,241 instructions and the full state
identical.

## The spin at 0x00590A18

The stop was `while (0x005B0880(1) == 0)` — a getter for the library's
register array (base 0x008869C0, one word per index).

The containing function 0x00590978 is the game's command-layer init:

- 0x005909E0 registers a handler for cid 0x80000018 (the game's message
  dispatcher 0x00590B80) through the library's `AddCmdHandler` 0x005B0BB0
  (entry stride 0x0C, 32 entries at 0x00886840 — the live dump shows the
  handler table and the register array pointers in the library data at
  0x00886818);
- 0x00590A10 sends the command cid 0x80000001 with a 24-byte packet whose
  payload words are {1, 1};
- 0x00590A18-0x00590A20 spins until register 1 is non-zero;
- 0x00590A44 registers a handler for cid 0x80000014 (0x00590A60) through the
  game's own handler table (0x0088C348).

The library's handler table (live memory 0x00886840) resolves the cids:
entry 0 = 0x005B0870 (stores the packet's word at +0x10 into the library
data at +8 — the change-address handler) and entry 1 = 0x005B0850 (stores
the packet's word at +0x14 into the register array at
`register[packet[0x10]]` — the set-register handler). The ps2sdk reference
(`common/include/sifcmd-common.h`) names the same numbering:
SIF_CMD_CHANGE_SADDR = 0x80000000, SIF_CMD_SET_SREG = 0x80000001,
SIF_CMD_INIT_CMD = 0x80000002.

Only an incoming SET_SREG command can write the register the init waits on,
so the model mirrors an incoming SET_SREG back to the EE through the command
buffer. The live memory confirms the real handshake's result: registers 0
and 1 are both 1 at 0x008869C0/0x008869C4 in the menu state.

## The liblgdev device sync

With the spin cleared, the run reached a new trap: the deliberate
`beq zero, zero` spin at 0x005608DC-0x005608F8 (and the identical ones at
0x005607E8-0x005607FC, 0x00560890-0x005608A8 and 0x00560958-0x0056096C).

The containing function 0x00560778 is the game's device-library request:

- 0x005607D4 binds the server 0x046D046D (the module banner "liblgdev
  version 1.11.036, built on Jan 27 2005 at 19:19:19" sits at live
  0x006C8D40, referenced by the library structure at 0x00654A84-0x00654A8C);
- 0x0056087C sends RPC 12 with 576 bytes in and 576 bytes out, the buffer
  being 0x00873F40 (the request is zero except a cleared status word);
- 0x005608B4-0x005608CC accepts the reply's status word at +4: the exact
  value 0x010B2400 takes the completed path (0x00560908) and any
  0x010Bxxxx takes the partial path (0x00560900); anything else falls into
  the trap.

The model answers 0x010B2400. Its caller is the loader's request loop at
0x005552B8 (queue entries of 0x114 bytes at +0x100, dispatching states 0/1/2
through 0x005555C0 and 0x00555678, collecting results for 0x00561300).

## The resulting run

```
--services 1000000 --threads        (ends at the service limit)
boundary: syscall 0x005adbd4 service 0x33
stats: module calls 1710779, interpreted steps 46608011, services handled 1000000
--services 300000
stats: module calls 515380, interpreted steps 14055538, services handled 300000
```

The call distribution over 300,000 services is a steady device polling
round: 0x50434456 RPC 1 (2,686 calls), 0x5042474D RPC 8 (2,472),
0x53505550 RPC 4 (2,385), 0x046D046D RPC 6 (2,293), 0x50434456 RPC 3
(2,225), 0x80000400 RPCs 1/0x15 (537/479), 0x046D046D RPCs 0x0F/0x0D
(287/278), 0x80000006 RPC 0 (21), plus the rest of the smoke tests at
fewer than ten calls each.

## The next frontier

Decide whether that polling round is forward progress or a wait, and answer
the first of its calls whose reply the game acts on (the live PCSX2 emulator
is the oracle for the real replies). The candidates are the liblgdev RPCs 6,
13 and 15 and the string-coded servers' RPCs 1/3/4/8.

## Verification

- CTest **32/32** (the SET_SREG mirror in `ee_kernel`, including the queued
  SIF0 interrupt, and the liblgdev sync status); Python 73 (67 run, 6 skip).
- `gt4boot --compare-interpreter` at 3,000 services: interpreter reference
  at 7,573,241 instructions, full state identical (registers, HI/LO, FPU,
  VU0, CP0, pc, memory digest).
