# M30, twenty-eighth slice — the stream's writers by call site

Date: 2026-10-02. Inputs: the pinned CORE and ISO. Follow-up to the
twenty-seventh slice (the stream's content). **No model behavior changed**:
this slice maps every writer of the sound library's buffers by call site
and records what the instrument can and cannot see.

## The writers, by call site

A temporary watch reported the executing pc, the guest ra (the return
address, i.e. the call site) and a1 with every write into the sound
library's buffers (0x00847180..0x00847620 and the statics 0x00623A40..):

| pc | ra | writes | what it is |
| --- | --- | --- | --- |
| 0x00462670 | 0x00462780 | 69 | the assign body's memcpy — the call at 0x00462778 inside 0x00462738 |
| 0x00462900 | 0x00462FE4 | 12 | the string-object method called from 0x00462FD4 |
| 0x00463600 | 0x00463764 | 10 | a sound-library function |
| 0x00462710 | 0x00462780 | 4 | the assign's own bookkeeping (the statics 0x00623A40) |
| 0x00462EC8 | 0x0046304C | 1 | the name lookup called from the sound init |
| 0x004627B0 | 0x00462FC0 / 0x0046305C | 2 | the setters of the two streams (0x00847180/0x008475C0) |
| **0x00100008** | 0x0 | **148** | **the patched syscall stubs: the SDK's own string code writes into the same region** |

So the stream is filled by the sound library's assign chain **and** by the
SDK string routines reached through the kernel's patched syscall table.

## What the instrument cannot see

The pc/ra/a1 values are set by the driver only when it starts a **module
entry**; the translated module's own inner calls (the memcpy 0x005A4724
among them) do not update them. The ra at the assign entry is reliable (it
names the call at 0x00462778 inside 0x00462738), but the argument
registers can read stale or zero for inner calls — so a1 cannot be trusted
to identify each source. The next instrument must either read the guest
arguments at a boundary where the register file is current, or watch the
formatter's own arena instead of the assign.

## The path, restated with call sites

The sound library's assign chain is now fully mapped: the getter
0x00462588, the formatter 0x0044D740 (arena-allocated), the assign
0x00462670 (whose body memcpys `*(source+8)` bytes), the statics
0x00623A3C (position) and 0x00623A40 (size), the two stream buffers
(0x00847180 with size 0x400 and 0x008475C0 with size 0x1000), and the
callers inside the sound init 0x00463000 (the three bank names) and
0x004630BC (the flag-1 `/sound/roadnoiz.es` assignment that faults).

## Verification

- No model behavior changed; the temporary instruments are removed and the
  tree is clean.
- CTest 34/34; Python 73 (67 run, 6 skip); the differential passes at
  3,000 services with the interpreter reference at 7,570,583 instructions
  and the full state identical.
