# M25 — the translator reaches the macro and trapping operations

Date: 2026-10-02. Inputs: the pinned CORE.

## What changed

- **The runtime executor is public**: `ee::execute_plain_effect(GuestState&,
  const DecodedInstruction&)` runs one already-decoded instruction's register
  and memory effect without touching the pc, returning false when a trapping
  overflow fires. It is the same code path the interpreter uses, so a
  translated module cannot drift from the interpreter on those operations.
- **The translator falls back to it** for every decoded plain operation that
  has no inline C++ form yet: the whole VU0 macro table, the COP2 moves and
  quad accesses, the remaining MMI forms (pmaxw/pminw/pcpy* and friends) and
  the trapping arithmetic. The emitted statement is a checked call —
  `if (!ee::execute_plain_effect(state, ee::decode(0x…u))) { state.set_pc(0x…);
  return; }` — so a trapping overflow stops the module at the instruction's
  address, exactly where the interpreter stops; every other operation always
  completes. Operations the decoder does not implement still reject the whole
  module at generation time, as before.
- The generated header now includes `gt4recomp/ee_interpreter.hpp`; the
  inline forms for the common operations stay in place, so the fallback only
  carries the long tail.

## Evidence

- **A new verified module**: `0x0056DF58`, a 133-instruction vector
  convert/scale loop whose body runs the MMI forms (pextlw, pmaxw, pminw,
  pcpyld, pcpyh, pmfhl, pcpyud, pmadduw, pmulth) and the division staging; 32
  of its instructions reach the runtime executor. The differential test
  (`ee_translation_56df58`) runs three input states — one and two outer
  passes, and a zero-count pass that skips the inner loop — and compares all
  32 registers, the pc, both HI/LO banks and the whole scratch memory window
  (inputs, the tail loop's array, the final store target and the saved
  registers) against the interpreter. All three match.
- CTest 24/24 (the new test included); Python 71 collected (65 run, 6 skip).

## Limits recorded

- The fallback decodes the instruction at run time on every execution; a
  future slice can emit the decoded form or inline tables where speed
  matters. Correctness is not affected.
- Operations outside the decoder's subset still reject the module (VCALLMS
  and the VU0-memory forms, BC0F, the two unassigned words); indirect calls
  remain unsupported by design.
