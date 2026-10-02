# 0004 — The driver classifies module stops from the guest state

Status: implemented for the M30 first slice on 2026-10-02; the resume
mechanism past a syscall remains open.

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
  alone.
- **Open: resuming past a syscall.** The translator ends the enclosing
  function at the syscall and does not translate the continuation, so there
  is no module entry at pc+4 after a service runs. The two candidate designs
  are (a) the translator emits a resume entry for every halt address it can
  continue past, or (b) the syscall becomes an inline runtime call
  (`ee::execute_syscall`) that a registered service layer resolves, letting
  the generated code continue at pc+4. Option (b) keeps one function per
  entry and matches the interpreter's single-step model; option (a) is a
  smaller change to the translator. This must be decided before the BIOS
  services slice, with the differential tests as the acceptance evidence.
