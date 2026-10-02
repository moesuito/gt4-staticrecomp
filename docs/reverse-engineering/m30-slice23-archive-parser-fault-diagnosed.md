# M30, twenty-third slice — the archive-parser fault diagnosed

Date: 2026-10-02. Inputs: the pinned CORE and ISO, the live PCSX2 memory
dump and the analysis ELF. Follow-up to the twenty-second slice. **No model
behavior changed**: this slice establishes what the new fault is and is
not, so the next one can fix the right thing.

## The fault is guest data, not translation

The twenty-second slice left the boot stopping at a guest fault after
83,783 services (an unaligned word access at 0x008475EB, reported at pc
0x00462670). Running the **same sequence in the reference interpreter**
(temporary instrument, removed) faults at the **same address and width** —
so both engines reach the same state and the cause is the **guest data the
model provides**, not a divergence in translated code. (Had the interpreter
run past it, the module would have been the suspect.)

## What the faulting code is

The module entry 0x00462670 is the engine's **string-object class** (its
constructor/assignment pair, body at 0x004625A8, static defaults at
0x00623A38/3C/40). The assignment body calls 0x005595C8 with the object's
data pointer; that function is a **pointer relocation**: it reads
`*(a0 + 4)` as the structure's old base, computes the delta `a0 - old`, and
rebases the pointers at +0xC, +0x14 and +0x1C. Its first read is the
faulting instruction.

The state at the fault (dumped with a temporary instrument):

| register | value | meaning |
| --- | --- | --- |
| a0 (r4) | 0x008475E7 | the structure, at an **odd** address |
| s0 (r16) | 0x008475E7 | the same pointer after the first copy |
| s2 (r18) | 0x00623A50 | the static string object |
| ra (r31) | 0x00462620 | the assignment body's return address |
| sp (r29) | 0x01FFFE90 | the guest stack |

The structure sits in the engine's **static buffer** (base 0x00847580, from
the code at 0x00462EEC/0x00462F2C). The live game's dump holds a *relocated*
structure in the same buffer at the **aligned** address 0x008475E0 (with
"INST" before it and the fields {0x4B4, 0x16F80, 0, "SShd", ...}); the
model's structure is at **0x008475E7 — seven bytes later** — so its
placement depends on data the engine processed (the length of a string that
precedes it). The engine's library here is its file/name layer (the path
strings it uses include `/sound/gt4race2.ins` at 0x006AB850 and `.ins` at
0x006AB4E0), so the boot is preparing a sound file when the structure is
built.

## What the next slice must find

Which code builds that structure in the buffer (and from which data), and
why the model's offset is odd while the console's is aligned. Candidate
sources, in order of suspicion: the string the engine places before the
structure (its length moves the structure), the archive data the engine
parsed just before (the inner archives, version 3.1), and any service
answer that sizes or locates that data.

## Verification

- No model behavior changed; the temporary instruments are removed.
- CTest 34/34; Python 73 (67 run, 6 skip); the differential passes at
  3,000 services with the interpreter reference at 7,570,583 instructions
  and the full state identical.
- The boot still stops at the same fault (83,783 services), now with its
  cause named.
