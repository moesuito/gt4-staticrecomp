# M30, twenty-ninth slice — instruments and the formatter

Date: 2026-10-02. Inputs: the pinned CORE and ISO. Follow-up to the
twenty-eighth slice. **No model behavior changed**: this slice records what
the two write-watch instruments can and cannot see and maps the formatter
that the assign chain uses.

## The live-argument instrument fails on translated code

A second instrument read the guest argument registers (r4/r5/r6) from a
temporary state pointer at every write. It captured **zero** copies: the
translated module keeps values in host registers and only synchronizes the
guest register file at boundaries, so mid-execution reads are stale. The
same reason explains the zero a1 seen in slice 28. **Lesson: a memory write
watch can rely on memory contents, not on the guest register file; only the
interpreter bridge keeps the registers current.**

## The formatter, mapped

The assign chain's middle step (0x0044D740) is the SDK's printf:

- 0x004AEFF0 initializes a formatter context on the stack;
- 0x004AE1F8 parses the **format** (its first argument, the caller's
  source);
- 0x004AF3E8 formats with the three arguments (the getter's result and the
  assign's a2/a3);
- the **result** is read from `*(context + 0x94)` and returned; a failed
  parse returns 0;
- 0x004AF568 / 0x0044D460 / 0x004AF0A0 release the context.

So the assign copies a **formatted arena object**, and the caller's string
is only the format.

## The open question, sharpened

The three sound-bank assignments advance the stream by **13 bytes each**
(the free-space static 0x00623A40 goes 0x1000 → 0xFF3 → 0xFE6 → 0xFD9) —
not by the names' lengths (17/18/19). The formatted result for a name
without specifiers would be the name itself, so the copied objects are
**not** the formatted names. Identifying them needs the assign's source
object directly (its content lives in memory and is observable), or a
static trace of the two sound functions that also write the stream
(0x00462900 and 0x00463600, which account for 22 of the 247 watched
writes).

## Verification

- No model behavior changed; the temporary instruments are removed and the
  tree is clean.
- CTest 34/34; Python 73 (67 run, 6 skip); the differential passes at
  3,000 services with the interpreter reference at 7,570,583 instructions
  and the full state identical.
