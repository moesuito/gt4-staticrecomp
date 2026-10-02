# M30, forty-first slice — the worker runs; the result field is the wall

Date: 2026-10-02. Inputs: the pinned CORE and ISO. Follow-up to the
fortieth slice (the context's virtual table and the wait). **No model
behavior changed**: this slice reads the handler class's full virtual
table, names every pipeline step, and — with a new stop-time memory view —
**corrects the thirty-eighth to fortieth slices**: the worker *does* run,
the state *does* reach 3, and the open's pipeline completes. What stays at
zero is the **result field the formatter reads**.

## The instrument: `gt4boot --dump ADDRESS LENGTH`

`gt4boot` gained a stop-time memory view: `--dump ADDRESS LENGTH` prints
LENGTH bytes (hex, eight words per line) of guest memory at the stop, and
also when the guest faults — the views are what explain the fault. The
handler objects, the formatter's context and the stack frames can then be
read off after a run (`--services N` bounds the run).

## The handler class, from its constructor

**0x004AD1C8** (the base constructor, vtable 0x688D48) initializes:

| offset | field |
| --- | --- |
| +0x10 | the handler's mutex (0x00574D78) |
| +0x40, +0x4C, +0x58 | three lists, each {head, tail, tag 0x688C40} (0x0060A480) |
| +0x64 | the worker's condition (0x00574D78) |
| +0x94 | the result (cleared) |
| +0x98, +0x9C | a 64-bit pair (cleared) |
| +0xA0 | the stop flag |

**No thread is created here**: the worker loop is a virtual method the
engine calls on a thread it already owns.

## The archive handler's virtual table (0x00688C58)

| slot | method | what it is |
| --- | --- | --- |
| +0x10 | 0x004ACB18 | the loader loop |
| +0x18 | 0x004B1F38 | the read-side entry |
| +0x20 | 0x004AD868 | the step before the completion (0x578B50/0x578610) |
| +0x28 | 0x004AD890 → 0x004AD808 | **the handler's completion**: `handler+0x94 = 0x5750C0(handler+0x98, ...)` — the file object |
| +0x30 | 0x004AD8C0 | the stop: +0xA0 = 1, signal the condition |
| +0x38 | 0x004ACBA0 | the path match (the prefix rule) |
| +0x40 | 0x004B1730 | **the open**: takes the stream, stores the handler (+0x38) and the path buffer (+0x48), enqueues to +0x40, returns the stream |
| +0x50 | 0x004AD8F8 | **the worker loop** |
| +0x58 | 0x004AD9D0 | **the first drain** (the +0x4C list) |
| +0x60 | 0x004B0B48 | the first stage's work (the lookup, command 0) |
| +0x68 | 0x004B0BD8 | the second work (command 2) |
| +0x70 | 0x004ADBB8 | **the second drain** (the sorted tree +0x58) |
| +0x78 | 0x004ADC40 → 0x004AF780 | the second stage's work (attaches the stream to the result object) |
| +0x80…+0xA0 | 0x004B0C48, 0x004B0D90, 0x004B0E28, 0x004B0FD8, 0x004ADCB8 | the read-side methods |
| +0xC8 | 0x004B1800 | the search wrapper: resolves the stream's path handle and calls +0xE0 |
| +0xE0 | 0x004B1F90 | the archive search: 0x004B3350 normalizes the path (0x004B2050) and searches the directory object at handler+0xC4 (0x004B3270) |

The pipeline in full: the open (vtable+0x40) enqueues the stream to +0x40;
the formatter's process step (0x004AD648) moves it to +0x4C, sets the
state to 1 and signals the handler's condition; the worker loop
(0x004AD8F8) wakes on that condition, drains +0x4C with the first drain
(0x004AD9D0: the once-only gate 0x004AF520, the work method for the
command, then 0x004AD6D8 moves the stream to the sorted tree +0x58 and
sets the state to 2), drains the tree with the second drain (0x004ADBB8:
the work method +0x78, then pops the tree and calls the **stream's**
completion, context vtable+0x48 = 0x004AF4A8, which sets the state to 3
and wakes the formatter). Both drains also call 0x004AD118 and reset the
gate (+0x98) so the next stage can run.

## What the model's state says at 83,782 services

The new `--dump` at the stop (the last clean boundary before the fault)
reads:

- the formatter's context at **0x01FFFDB0** (the "stream"): handler
  +0x38 = 0x617AB0, command +0x74 = 0, **state +0x80 = 3**, +0x84 = 2,
  +0x4C = the formatter's result descriptor 0x01FFFE60, **result
  +0x94 = 0**, gate +0x98 = 0, vtable +0xA8 = 0x688ED0;
- the handler **0x617AB0**: the three lists (+0x40, +0x4C, +0x58) are
  **empty**, +0x94 = 0x0096DEF0 (a file object the handler's completion
  created), +0xA4 = 0x688C58, +0xAC = 0x6B00D0 (the "/" prefix);
- a thread's wait frame sits on the stack at 0x90C92C (thread id 14)
  referencing 0x617C14 (the *other* handler's condition), consistent with
  a sleeping worker of a different handler.

So the open's pipeline **ran end to end**: the enqueue, the first drain
(the lookup work), the move to the tree, the second drain, the stream's
completion (state 3) and the formatter's wake-up. The formatter then read
its context's **+0x94 = 0** — the value the assign chain copies from — and
the null copies followed.

## Correction of the thirty-eighth to fortieth slices

The stack watch of the thirty-eighth slice followed the *sound thread's*
stack pointer, so the worker thread's writes were outside its window. The
conclusion "the completion never runs" was wrong: the step that was
missing from the trace is the **stream's** completion (0x004AF4A8), not
the handler's (0x004AD890), and it ran on the worker thread. The fortieth
slice's "the state never reaches 3" is corrected the same way.

## The open question for the next slice

The first stage's work (0x004B0B48) copies the search's result into the
stream: on success `stream+0x84..0x93` receive the request's first four
words, `stream+0x94` receives the request's +0x10 and `stream+0xA4` the
offset. The search (0x004B1F90) writes the request's +0, +4, +8, +0xC and
+0x14 — **not +0x10** — and the request's +0x10 is zeroed by 0x004AF6B8.
The model's context +0x94 = 0 is consistent with that; the *console's*
must carry the file object the assign chain reads. The next slice must
find which step is supposed to write that field (the search's result
object at handler+0xC4, the entry pointer, or the completion's file
object) and why the model's value is zero — comparing the request and the
directory object with the console's live state.

## The differential at the fault's doorstep

`gt4boot --services 83782 --compare-interpreter` runs the whole boot —
the entire sound phase, the archive open, the worker pipeline — in both
engines: **24,114,381 interpreter instructions, the full state identical
(registers, HI/LO, FPU, VU0, CP0, pc and the memory digest)**. The model
is therefore faithful to the reference at the point of the failure: the
zero result is what the guest code produces from these inputs, so the
remaining question is which *input* (the path string or the directory
object) differs from the console — not a scheduler or translation bug.

## Verification

- No model behavior changed; the temporary instruments are removed and
  the tree is clean.
- CTest 34/34 plus the new `gt4boot_dump` test (the `--dump` option);
  Python 73 (67 run, 6 skip); the differential passes at 3,000 services
  with the interpreter reference at 7,570,583 instructions and at 83,782
  services with 24,114,381 instructions, the full state identical in both.
