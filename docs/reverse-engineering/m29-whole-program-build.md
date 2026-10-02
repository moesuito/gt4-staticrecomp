# M29 — module-size policy and the whole-program build

Date: 2026-10-02. Inputs: the pinned CORE. Follow-up to M28: the translator
could dispatch indirect targets inside a module, but the module size was
fixed at 256 functions and there was no whole-program mode.

## What changed

- **`--functions N`** sets the module's function limit (default 256) for
  every mode; the survey uses it too, which separates "the tree does not
  translate" from "the policy is too small".
- **`--all`** translates **every direct-call target in the text plus the ELF
  entry as one whole-program module** (the seed set the survey enumerates),
  bounded by the instruction budget and the function limit.
- **Direct calls whose target lies outside the file-backed text** (five in
  the pinned CORE, pointing into the data segment) now stop at the call like
  the other unknown-target boundaries, instead of aborting the walk. The
  walk's failures also name the function they happened in.
- **The COP1 branch conditions** (`bc1f`/`bc1t`/`bc1fl`/`bc1tl`) joined the
  emitter — a latent gap the whole-program run exposed (no earlier target
  used them): the condition reads FCR31's condition bit, exactly like the
  interpreter.

## The whole-program build (pinned CORE, 2026-10-02)

```
gt4translate CORE.GT4 --functions 20000 --all 2000000 generated/whole-program.hpp
```

- **15,068 functions, 924,991 instructions, 146.4 MB, 2.57M lines, 136
  seconds** of generation time.
- The generated header **passes an MSVC syntax check** (`cl /Zs`) in **27.5
  seconds**: the whole game is well-formed C++ within the compiler's limits.
  A full code-generation build (925k instructions of machine code) was not
  attempted yet; it is the next scale question.
- The module's own dispatch table covers all 15,068 entries, so every
  indirect target that is part of the game's direct-call closure resolves at
  run time; only genuinely dynamic targets (or the five out-of-text calls)
  stop at the boundary.

## Survey with the policy lifted

```
survey: entries=15067 translated=14991 functions_in_trees=331035
covered instructions: 871317 of 1334917 words in the file-backed text
reason: 76 x The call tree exceeds the instruction budget
```

- **14,991 of 15,067 entries translate (99.5%)** and the union of their trees
  covers **871,317 instructions (65.3% of the file-backed text)**.
- The only remaining rejections are the survey's own per-tree instruction
  budget (20,000) — a reporting parameter, not a translation limit.

## Evidence

- The Python CLI test exercises `--functions` in both modes (a small limit
  fails the whole-program walk fast with the policy message; the normal mode
  keeps working).
- CTest 25/25; Python 73 collected (67 run, 6 skip).

## Limits recorded

- The whole-program module is a single 146 MB translation unit; a real build
  will want splitting or streaming (the tool builds the text in memory).
- Five calls leave the text (the data-segment region); they stop as
  boundaries until the data region is understood.
- The 76 survey rejections only exist because each tree is walked with a
  20,000-instruction budget; the `--all` walk has no such limit.
