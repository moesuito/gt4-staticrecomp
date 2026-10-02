# M28 — the module dispatches its own indirect targets

Date: 2026-10-02. Inputs: the pinned CORE. Follow-up to M27, which turned
runtime-target transfers into stopping boundaries.

## What changed

Every generated module now carries a **dispatch table over its own function
entries** (`detail::has_entry` and `detail::call_entry`, one case per
translated function), and the indirect transfers use it:

- **`jalr rd, rs`**: the target is read first (a shared `rd == rs` encoding
  still jumps to the old value), then the link goes to `rd` (pc+8), then the
  delay slot runs, then the entry is called. A callee that stops at a
  boundary propagates through the caller (`if (pc != link) return;`); a
  callee that returns lets the caller continue inline, exactly like a direct
  call. When the target is not one of the module's entries, the module stops
  at the transfer, before the link or the delay slot — the M27 boundary
  remains as the honest fallback.
- **Computed `jr rs`**: the same dispatch; the target returns through this
  function's `ra`, so the module returns after the call.
- `eret`, the unmodeled words and unmodeled delay slots keep their M27
  behavior (boundaries at the exact interpreter stop point).

## Results (pinned CORE, 2026-10-02)

```
survey: entries=15067 translated=14938 functions_in_trees=316092
covered instructions: 864507 of 1334917 words in the file-backed text
reason: 119 x The call tree exceeds the function limit
reason: 5 x The call tree exceeds the instruction budget
reason: 5 x start-validation edge cases
```

The translated-entry count is unchanged (the same trees); the covered
instructions rose from 858,621 to **864,507** because the computed-jump words
and their delay slots are now emitted instead of being erased as boundaries.

## Evidence

- The `ee_translation_101c28` differential test now runs two states:
  - **known target** (`a0 = 0x0058B268`, a module entry): the module reads the
    target, writes the link, runs the delay slot, dispatches into the callee
    and propagates its break boundary — all 32 registers, the pc and the
    scratch memory compare equal with the interpreter at the same stop pc;
  - **unknown target** (`a0 = 0x00100400`): the module stops at the jalr with
    the pc at the transfer, identical to the interpreter's prefix.
- CTest 25/25; Python 72 collected (66 run, 6 skip).

## Limits recorded

- The table covers the module's own entries. A whole-program build will want
  a global registry across modules; until then, a target translated in
  another module still stops the boundary.
- A computed `jr` into a *local* block (a jump table inside the same
  function) is not a function entry and keeps stopping at the transfer.
- The "callee returns and the caller continues inline" branch uses the same
  emitted machinery as direct calls, which the earlier differential tests
  cover; the known-target state exercises the dispatch, link and delay-slot
  ordering.
