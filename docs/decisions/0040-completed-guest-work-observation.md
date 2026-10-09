# 0040 — Count completed guest work without changing the clock

Status: accepted and verified, 2026-10-09, slice 98.

## Context

The service clock charges 1 ms even when reference dispatch takes only a
few nominal microseconds. Static module instruction counts and module calls
cannot describe loops, internal calls or nullified slots. Before deriving
another time policy, both execution engines need an identical dynamic unit.

## Contract

- Observation is opt-in. A host-owned GuestWorkCounter attaches to GuestState
  and outlives that attachment. It is not part of RegisterContext, the guest
  comparator or checkpoint serialization. Thread/interrupt register restores
  cannot rewind it. A new boot invocation loading a checkpoint starts a new
  observation interval; applying a snapshot onto an already observed live
  object conserves its attached counter and total.
- completed_instructions counts successful instruction effects, including
  transfers and executed slots. accepted_services is the syscall subset:
  the service owner records an accepted call exactly once, after the handler
  accepts it, including context-switch/jump/idle outcomes.
- Unsupported/trapping effects, unhandled/budget-limited syscalls, repeated
  stops and nullified slots contribute zero. A native unknown-target exit
  applies nothing and contributes zero there; the interpreter bridge records
  the eventual transfer and slot. Standalone copies count only when executed.
- Each transfer is recorded after its own decision/link/target capture, before
  its slot/callee; a later slot/callee stop does not erase completed work.
- Diagnostic overflow throws explicitly instead of wrapping. Because recording
  follows completion, that error is fatal for observation, not a resumable
  guest boundary. A failed instruction itself is not recorded.
- No clock conversion, delivery point or scheduler behavior changes. Future
  temporal integration must define serialized residue, precise event exits,
  branch/slot exclusion and semantic identities separately.

## Alternatives / limits

Counting in execute_plain_effect would miss inline translated instructions
and double-count interpreter/runtime fallbacks. Counting in the emitter and
Interpreter::step instead attaches to actual successful dynamic execution.
An external counter avoids enlarging every thread save or changing an existing
checkpoint contract for diagnostic data. Its totals are compared explicitly
by work-enabled tests/CLI, not silently treated as guest state.

This does not emulate BIOS handler instruction work: an accepted model service
counts one syscall word, not the reference BIOS's internal implementation.
It does not establish instruction cycles or physical-console fidelity. Existing
treated syscall-in-delay-slot parity remains a separate semantic audit target;
observation must not turn that unsupported inference into a clock contract.

Evidence: `docs/reverse-engineering/slice98-completed-guest-work.md`.

## Verification

26 independently hand-counted synthetic paths plus acceptance, isolation,
segmentation and DMA-poll controls pass. Work-enabled real boot differential
at 90k services: 22,566,319 completed instructions / 90,000 accepted services
in both engines, with the existing full-state comparison also identical.
Fresh 10k counted/plain output matches after removing only the two observation
lines. CTest 53/53 and Python 84 (6 skips) green; no acceptance weakened.
No timer policy, guest checkpoint bytes or semantic compatibility changed.
