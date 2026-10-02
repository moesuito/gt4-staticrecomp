# M11 synthetic programs evidence — generated straight-line suites

2026-10-01: BUILD/VERIFY passed. EXPLAIN lesson pending.

## Why a generator, and why an independent model

The milestone asks for *generated* synthetic programs. The project rule is
that a self-written interpreter and a self-written generator can share a bug,
so the generator must not be its own oracle: `scripts/synth_programs.py`
implements the execution rules a second time, independently and in Python
(explicit masks, no shared code), computes the expected final state of every
generated program, and writes it into a committed fixture
(`tests/data/synth-straight.txt`). The C++ interpreter then executes the same
programs and must reproduce every expected register, memory byte and pc.
Agreement between two implementations is the evidence — the Ghidra idea
applied to execution.

## Format (v1)

Line-based text, trivial to parse in the C++ test without new dependencies:

```text
# synth-straight v1 seed=<seed> count=<count>
program pNNN
data  <addr8> <hexbytes>      (pre-filled window, optional)
init  rN <hex64>              (initial register value, optional)
word  <hex8>                  (program words, in order from 0x00100000)
expect rN <hex64>             (final register value; r0 is always checked)
expect_mem <addr8> <width> <hex>
expect_pc <hex8>
end
```

## What the generator emits

- 40 programs (seed 20261001), 12-24 instructions each, over the full
  straight-line subset: eight register ALU operations, two shifts, four
  immediate forms and six memory forms. Generation fails unless every
  operation appears (`require_full_coverage`), so the committed fixture is
  guaranteed to exercise all 20 implemented straight-line operations.
- Working registers 1-15 start from 3-6 random 64-bit patterns (including
  negative sign-extended values). Memory-using programs set `r16` to
  `0x00100800` and pre-fill 32 deterministic bytes; stores write into the same
  window.
- Expectations cover every register the program touches, every byte it
  writes, and the final pc.

## Real evidence

```powershell
.\build\ee_synth_tests.exe tests/data/synth-straight.txt
# -> 40 synthetic programs passed
```

CTest runs the same fixture as the `ee_synth` test. The Python suite verifies
that the committed fixture is byte-for-byte what the generator produces, and
anchors the model on hand-computed rules and encodings (cross-checked against
the hand-verified words already in the decode fixtures).

## Incidents

- The first Python test run failed: the coverage guarantee was applied to
  every `generate` call, so the tiny 2-3 program fixtures used by the unit
  tests raised. The guarantee is now an explicit `require_full_coverage`
  option, used for the committed fixture only. Caught by the suite before any
  commit, consistent with the project's testing discipline.

## Verification

- 11/11 CTest (the new `ee_synth` executes 40 programs of 12-24 instructions);
  Python suite 44 collected (38 run, 6 skip without the M3 reference ELF).
- The fixture is self-contained and committed, so CTest needs no Python at run
  time; regenerating with the recorded seed reproduces it exactly.

## Limits

Straight-line only in this slice; branching programs (loops, likely branches,
calls) are M12. The generator's randomness is deterministic (seeded) and not
exhaustive: this is volume plus implementation independence, not formal
verification. Memory coverage is one 32-byte window, and nothing here executes
real game code.
