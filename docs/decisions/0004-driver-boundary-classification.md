# 0004 — The driver classifies module stops from the guest state

Status: implemented for the M30 first slice on 2026-10-02; the resume
question was resolved by the interpreter bridge in the second slice (see the
consequences and `docs/reverse-engineering/m30-bios-services-and-bridge.md`).

Decision: a translated module stays a plain function over `GuestState`; the
driver calls the entry that owns the current pc and classifies the stop from
the state alone (the word at the pc, the link register, the memory window).
Every classification is a named boundary; nothing is guessed past one. The
generated code is unchanged, so the existing differential tests keep working
and no stop bookkeeping enters the guest model.

Alternatives considered:

- **A stop-reason field in `GuestState`** written by generated code at every
  boundary (rejected: a host diagnostic would become part of the guest model
  and would have to be excluded from every state comparison).
- **Generated functions returning a stop enum** instead of `void` (rejected
  for this slice: it changes every signature and every translation test for a
  disambiguation that only matters for trapping overflows; revisit if the
  ambiguity starts to bite).
- **Running the interpreter alongside the module and comparing step by step**
  (rejected: it would make the driver's answer depend on the very
  implementation it is supposed to replace).

Consequences:

- A normal `jr ra` return is recognized because the translator leaves pc at
  ra. A trapping stop whose address equals ra would be misreported as a
  return; no such case exists in the pinned code (recorded in the M30 doc).
- A stop at an ordinary instruction is reported as `InstructionStop` with the
  word attached; the cause (trapping overflow) is not recoverable from the pc
  alone. The interpreter path does not have this limit: it uses the step
  outcome (`boundary_from_step`).
- **Resolved in the second slice: the interpreter is the bridge.** The
  translator still ends the enclosing function at a syscall and does not
  translate the continuation, so after a service runs there is no module
  entry at pc+4. The driver now continues through the step-by-step
  interpreter — the reference the module was verified against — until the
  next module entry. This composes through calls and returns (a resumed
  callee returning to its caller needs no entry at the return address) and
  required no translator change. The two designs that were candidates remain
  future performance options, not correctness requirements: (a) resume
  entries for every halt address, (b) an inline syscall runtime call in
  generated code. Both shrink the interpreted gaps; the bridge stays as the
  universal fallback for boundaries the module cannot pass.
