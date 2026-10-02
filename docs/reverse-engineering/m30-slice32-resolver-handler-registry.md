# M30, thirty-second slice — the resolver's handler registry

Date: 2026-10-02. Inputs: the pinned CORE and ISO and the live PCSX2 dump.
Follow-up to the thirty-first slice (the resolver 0x0044D740 returns 0).
**No model behavior changed**: this slice maps what the failing parse does
and compares the registry it walks with the console's.

## The parse is a handler-registry dispatch

The parse 0x004AE1F8 forwards to **0x004ACE58**, which:

1. locks the mutex at 0x00631880;
2. walks the global handler list at **0x006318B0**;
3. for each handler object calls its **match** (vtable + 0x38) and, when it
   returns non-zero, its **handler** (vtable + 0x40) with the path;
4. returns the handler's result, or 0 when no handler matched.

## The registry matches the console

The handler list in the model at the fault is **identical** to the live
dump's:

| node | object | vtable |
| --- | --- | --- |
| 0x00617BB0 | 0x00617BB0 | 0x00688C58 |
| 0x0084B480 | 0x0084B480 | 0x00688B70 |
| 0x00617AB0 | 0x00617AB0 | 0x00688C58 |

So the failure is not a missing registration: the objects are there.

## The match compares path prefixes

The vtable-0x688C58 handlers' match is **0x004ACBA0**: it compares the path
(its second argument, the format string) against the **string list at
object + 0xF4** (via 0x004AE9E8, a path-prefix compare that treats '/' and
the trailing-slash forms specially). When +0xF4 is zero it falls back
(0x004B0A38) to the **single string at object + 0xAC**. The vtable-0x688B70
handler's match (0x004AC5B0) always returns zero (a stub at this revision).

The fields differ between the model and the console:

| object | field | model at the fault | live dump |
| --- | --- | --- | --- |
| 0x00617BB0 | +0xF4 | 0x00617AA8 (the global holding the `/mpeg` pointer) | (not the same list) |
| 0x00617BB0 | +0xAC | (not dumped) | **0x006B00D0 — the string "/"** |
| 0x0084B480 | +0xF4 | 0 | — |
| 0x00617AB0 | +0xF4 | 0 | — |

The console's handler matches **any path starting with "/"** — the model's
first handler points its prefix list at 0x00617AA8 instead (the movie
path's global), so the sound-bank paths match no handler and the parse
returns 0.

## The handler constructor

**0x004ACA40** constructs the vtable-0x688C58 handlers: it sets
`+0xA4 = 0x688C58` (the vtable), `+0xF0` and `+0xF4` from its arguments,
and passes the **string "/" built at 0x004ACA68 (0x006B00D0)** to the init
0x004B1C10. So the "/" prefix the console's handler carries is the
constructor's own constant; the model's handler was constructed (or
re-constructed) with a different prefix list.

## What the next slice must find

Which code constructs (or re-registers) the handler 0x00617BB0 with the
"/" prefix, and why the model's instance ends up with 0x00617AA8 instead —
the candidates are the engine's mount paths (the ISO, the GT4.VOL archives
and the PCDV) and the order in which the model runs them. Also verify the
name strings: the fault dump showed them intact
(`/sound/gt4race2.ins`, `%s%s%s.ins`, `/sound/gt4sys.ins` at 0x006AB850
onward), so the formats themselves are not the problem.

## Verification

- No model behavior changed; the temporary dump is removed and the tree is
  clean.
- CTest 34/34; Python 73 (67 run, 6 skip); the differential passes at
  3,000 services with the interpreter reference at 7,570,583 instructions
  and the full state identical.
