# M30, forty-fourth slice — why the relocation pointer is odd

Date: 2026-10-02. Inputs: the pinned CORE and ISO. Follow-up to the
forty-third slice (the next wall: an unaligned fault at pc 0x00491798 on
0x009cf08f after 15,010,045 services). Verdict: the odd pointer is
**derived by walking a data buffer as pointer tables — never stored** —
and the relocate-over-data call cannot happen on a working console, so
the divergence is model-fed upstream (high confidence; the exact
divergent write is the next slice's experiment, not a guess shipped here).

## The fault context (temporary register/memory dump, since removed)

A fault-time dump of the live registers and the memory around the
argument gave:

```text
faultregs pc 0x491798 v0 0x9cf08f a0 0x9cf08f a1 0xffff a2 0x84b5b8
          a3 0x0 ra 0x491e90 sp 0x1fffe50 fp 0x0
faultmem a0 0x9cf080: 8080808 55550808 8080808 8080808
                      1010102 1010101 1010101 1010101
faultmem sp 0x1fffe50: 9a9400 0 0 0 491e90 0 0 0
```

Reading it:

- `ra = 0x00491e90` is the return into the trampoline 0x00491e80 (which
  does `jal 0x00498b28` at 0x00491e88): at fault time the live stack is
  inside **0x00498b28**, reached through the trampoline from the
  relocate loop. 0x00498b28 is a sibling of 0x00491798 with the same
  move-relocation shape (`s1 = s0 - *(s0+4)`, fixups, a table loop over
  the halfword count).
- The reported pc (0x00491798) is stale: the translator publishes the pc
  at halt points only, so a fault deep in translated code reports the
  last boundary-era pc, not the faulting instruction.
- `a0 = v0 = 0x009cf08f` with `a2 = 0x0084b5b8` matches the loop's table
  step (`lw a0, 0(v0)` loads a table entry; `a2` is the entry cursor):
  **a table entry holds the odd value 0x009cf08f**.
- The 8 words at 0x009cf080 are small-byte patterns
  (`08080808… 55550808… 0101010x…`), not pointers and not heap metadata.

## The odd value was never stored (the write watch)

A temporary write watch over 0x009CF060–0x009CF0C0 (same design as slice
42, deduplicated by pc+address, since removed) captured the region's
complete history in the faulting run — 109 lines:

1. `0x0048EF90` zeroed 0x009CF068–0x009CF0C7 with doubleword zeros (the
   allocator path: heap `0x00575c78` plus SIF setup `0x005524b8`).
2. `0x00573750` filled the window **byte by byte** with the structured
   pattern (runs of `0x08`, `0x55 0x55` marker pairs, counting runs like
   `05 03 02 01 01 01 01 00 00 01 01 02 03…`). 0x00573750 is a
   vtable-stepped transforming byte copy (`sb` loop with a per-chunk
   `jalr`), i.e. a decode/convert-while-copying routine; one byte
   (0x009CF075) came separately from `0x005734BC`.

No writer ever stored an odd pointer (or any pointer) there. The
"pointer" 0x009cf08f/0x009cf08b is **computed by the relocation walk**
(base + table offsets into this data), exactly the failure mode the
slice-23 fault showed for a packed stream (there: 13-byte records whose
odd position left a static object's pointer odd).

## The buffer is generated data, not disc bytes

The distinctive 33-byte run (`05 03 02 01 01 01 01 00 00 … 55 55 …`)
occurs **nowhere in the 5 GB ISO**. The content is produced in memory by
the transforming copy, not read verbatim from disc — so this is not a
corrupt disc read; it is a data buffer the game built and something then
mistook for a relocatable object.

## The +0x94 writers cannot mint odd values

The relocate input arrives through the lookup chain
(0x0048fb58 → 0x00491d90 → dispatcher 0x004ace58 → copy of
`+0x84…+0x97` including the result `+0x94`). A scan of the generated
code finds 4 stores to stream `+0x94` in the whole object family:
two zeroings (`0x00490df0`, `0x004ad218`), the handler completion with a
heap object (`0x004ad840`), and a field copy `+0x10 → +0x94`
(`0x004af1b0`). None produces odd values by construction — so the odd
value is a miscopied field or a non-stream object in the chain, i.e. the
lookup designated the wrong object (or the right object in a state the
hardware never produces).

## Verdict: model-fed divergence upstream (high confidence)

The packed/transformed data is deterministic from the game's own
routines, and the shipped game boots on hardware — a relocate-over-data
call with these values cannot happen on a working console. The
divergence is therefore in the model's upstream state (which object the
lookup designated, or the state it was in), not in the relocate routine
itself, which is working as written. Deliberately **no model change** is
shipped here: skipping odd relocations would fabricate hardware behavior
the evidence does not support.

## The next experiment (slice 45)

Pin the divergent lookup: capture the dispatcher query (the key handed
to 0x004ace58/0x004ae1f8) for the fatal call and what it should have
returned — via query logging at the dispatcher, or against the console —
then fix the model state that misdirects it.

## Verification

- CTest 36/36 and Python 73 (67 run, 6 skip), green on the final tree.
- All temporary instruments (the fault register/memory dump, the write
  watch with its driver pc publishing) are removed; `git status` clean of
  them and the tree holds docs only for this slice.
