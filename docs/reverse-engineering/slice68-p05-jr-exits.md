# Slice 68 - P05: JR capture and explicit module exit reasons

Date: 2026-10-04. Plan: PLAN.md section 6, P05 (slice 68 = P05).
Decision: docs/decisions/0032-p05-jr-and-explicit-module-exit.md.
Baseline: main 6fcca01 (P04 committed), 50/50 CTest + Python 73 (6 skips).

## References (all read, none assumed)

- PLAN.md section 6 P05 (delivery, acceptance, fixtures) and C08/C09.
- GPT_FEEDBACK.md sections 9-10 (F05/F06) and 29.4 (the authorized
  probes: JR words 03E00008/241F2000 with ra 0x3000, ERET 42000018 in
  EXL/ERL, BREAK 0000000D with pc == ra, the corrected-order control).
- In-repo: tools/gt4translate/main.cpp (emit_unit_body, collect_units),
  src/ee/driver.cpp + include/gt4recomp/ee_driver.hpp (classify_boundary,
  run loop), src/ee/interpreter.cpp (step order, the reference),
  src/ee/flow.cpp (Return vs IndirectJump, ERET has no slot),
  tests/unit/ee_driver_test.cpp, tests/python/test_translate_cli.py,
  tools/gt4boot/main.cpp (make_boot_module), docs/decisions/0031.

## What changed (files and lines)

- include/gt4recomp/ee_driver.hpp: BoundaryKind documented as the
  module's explicit exit report (Returned = applied jr-ra with the
  pre-slot target; IndirectTransfer = nothing applied, bridge
  completes; driver-side kinds named); ModuleEntry::execute and
  Module::call_entry return BoundaryKind; new Driver member
  `bridge_step_due_`; classify_boundary documented as the
  register-free word view.
- src/ee/driver.cpp: new file-local report_boundary (kind, pc, guest
  word, v1 service from state); classify_boundary without the
  `PC == RA` arm; run() switches on the module's returned reason
  (Syscall handled once, Returned/ExceptionReturn/IndirectTransfer
  continue, rest reported); IndirectTransfer arms
  `bridge_step_due_` so the bridge owns the next step.
- tools/gt4translate/main.cpp: Return captures ra into a function-top
  variable before the slot; every exit site returns a BoundaryKind
  (halts by operation, unsupported slots, likely trap slots by slot
  word, trapping-overflow inline, direct/indirect call propagation,
  computed-jump tail call, trailing fell-off-extent guard);
  detail/public call_entry return the reason; generated functions are
  `inline ee::BoundaryKind`; generated header includes ee_driver.hpp;
  new `--synth spec output` mode with an ASCII spec loader; usage
  text updated.
- tools/gt4boot/main.cpp: one line, return the translated call_entry.
- tests/unit/ee_driver_test.cpp: fake modules return reasons;
  stop_at_eret emulates the emitter (derives the pc, clears the
  level) instead of parking at the eret word; the "returned" classify
  row becomes trap-equals-ra; new driver legs (trap pc == ra,
  EXL/ERL to a common destination, known-jr propagation of Returned
  and of Break, pending transfer at its own entry).
- tests/data/synth-module-exits.txt (new): 32 original synthetic
  words, 10 seeds, the authority for the layout below.
- tests/unit/ee_module_exit_test.cpp (new): 11 legs, real emitter
  output compiled in, Driver vs interpreter on full effect plus stop
  reason.
- CMakeLists.txt: build-time custom command (gt4translate --synth)
  plus `ee_module_exit` CTest, outside the CORE guard (no game
  content).
- tests/python/test_translate_cli.py: three assertions re-pinned to
  the new emission (capture order, BoundaryKind signatures and
  propagation).

## The exit protocol

| Module stop | Reported reason | Driver action |
|---|---|---|
| `jr ra` applied (target read before the slot) | Returned | continue at the captured pc |
| ERET applied (pc from EPC/ErrorEPC, level cleared) | ExceptionReturn | continue at the derived pc |
| syscall word, service not yet run | Syscall | run the service once, continue at pc + 4; unhandled service ends the run |
| trap word (break) | Break | report |
| unknown indirect target, or direct call outside the text (link and slot untouched) | IndirectTransfer | bridge first (no re-entry), then continue |
| unmodeled word, or unmodeled delay slot | UnsupportedWord | report |
| trapping arithmetic overflow | InstructionStop | report (same mapping as boundary_from_step) |
| work budget exhausted | StepLimit | driver-side only; the module never emits it |

Internal calls propagate: `exit_X = callee(state);
if (exit_X != Returned || pc != resume) return exit_X;`.
The delay-slot, link and service effects a module applied are never
re-applied: unknown-target paths stop before writing anything, and
the bridge resumes exactly at the stop word.

## The entry-aliased pending transfer (found by the fixture)

First version of the driver trusted the reason but re-entered the
module whenever the stop pc was an entry. Every synthetic stop sits
at an entry (each case is directly callable), so leg 8 re-ran
function_00100038 instead of bridging: module calls grew without any
bridge step. In game modules this shape needs a transfer word that
is also a function entry; the old code had the same hole (it only
survived because real stops land mid-function). `bridge_step_due_`
closes it: after IndirectTransfer the bridge owns the next step.
Pinned by the new `stop_at_self_transfer` driver row (one module
call, four bridge steps, service once) and by leg 8 below.

## Fixture legs (ee_module_exit, all green)

Window 0x00100000-0x00103FFF; D = 0x00103000 (addiu, unsupported);
U = 0x00100FF0 (addiu, unsupported); R = 0x00100050 (unsupported).

| Leg | Setup | Module exit | Driver run | Interpreter |
|---|---|---|---|---|
| jr, slot rewrites ra | ra = D | Returned, pc = D, ra = 0x2000 | 1 call + 2 steps, UnsupportedWord at D + 4 | 2 Executed + tail, same stop; full state equal |
| eret EXL | Status EXL, EPC = D | ExceptionReturn, pc = D, Status 0 | 1 + 2, UnsupportedWord at D + 4 | same; equal |
| eret ERL | Status ERL, ErrorEPC = D | ExceptionReturn, pc = D, Status 0 | 1 + 2, UnsupportedWord at D + 4 | same; equal |
| break, pc == ra | ra = break word | Break | 1 + 0, Break at the word | Exception/Break; equal |
| syscall, no service | v1 = 0x42 | Syscall at the word | 1 + 0, Syscall with service 0x42 | Exception/Syscall; equal; nothing ran |
| syscall, handled | v1 = 0x42, service answers 7 | (through driver) | service once, v0 = 7, 1 + 2, UnsupportedWord past pc + 4, tail ran once | n/a (handler is driver-side) |
| known indirect | t0 = leaf, ra = R | Returned, pc = R, v0 = 7, v1 = 9 | 1 + 1, UnsupportedWord at R | 5 Executed + stop; equal |
| unknown indirect | t0 = U, ra = R, v0 = 0 | IndirectTransfer at the transfer, v0 = 0, ra intact | 1 + 4, UnsupportedWord at U + 4, v0 = 7 | 3 Executed + stop; equal |
| beql taken, syscall slot | t0 = 0 | Syscall at the slot | 1 + 0, Syscall with slot pc + v1 | branch Executed + slot Exception; equal |
| beql not taken | t0 = 1, ra = R | Returned, pc = R, t0 = 2 | 1 + 1, UnsupportedWord at R | nullified slot + return; equal |
| inner trap via dispatch | t0 = leaf2, ra = R | Break at leaf2 | 1 + 0, Break at leaf2 | jr + nop + Break exception; equal |

"Full state equal" means all 32 GPRs, all of CP0, the pc and every
window word. The fixture words never touch the FPU file or HI/LO.

## Verification (observed, not claimed)

- Build warning-free (MSVC 19.44, Ninja, Debug); new code comments
  in ASCII only (added-lines scan: 0 non-ASCII bytes).
- CTest 51/51 serial: the 50 prior tests plus `ee_module_exit`.
  `gt4boot_build` recompiled the whole-program module with the new
  emitter (same coverage: 15,068 functions, 924,991 instructions);
  `gt4boot_services` 90,000 with disc and compare-interpreter green
  (about 16 s), so the translator-vs-interpreter differential is
  exact under the new protocol and the prefix reaches no
  outside-text direct call and no translated ERET.
- Python 73 collected, 67 run, 6 skip, OK, with the two known socket
  ResourceWarnings.
- Direct executables: `ee_driver_tests` ("driver entries, services
  and bridge behave as specified"), `ee_module_exit_tests`
  ("synthetic module exits match the interpreter on 11 legs: effect
  and stop reason").

## P05 acceptance (PLAN.md section 6)

- JR keeps the old destination with the updated final RA: leg 1.
- BREAK with PC equal to RA stops as BREAK: leg 4 (module, driver
  and classifier rows).
- ERET EXL/ERL continues at the destination: legs 2-3, common
  destination, bridge steps observed (not zero).
- Real emitter output, compiled, executed by the Driver: every leg
  calls generated functions and the real entry table.
- Comparison covers effect and stop reason: full-state compare plus
  per-leg exit/boundary assertions on both engines.
- Allowed-path syscall: legs 5-6 (stop rule unhandled, exactly-once
  handling with the tail applied once).
- Known/unknown indirect without repeating the slot: legs 7-8-11
  (slot-once effects, propagation without overwrite).

## What P06 inherits

- DMA payload/tags untouched: transfers still complete
  instant-at-start with cause raised; VIF0/VIF1/GIF still use the
  old family wiring (C04/C05 open).
- RPC content untouched (C12 open): generic zero answers remain.
- Clock/quanta and interrupt masks untouched (P03 done, P06 must not
  regress the unified advance machine).
- Documented edges for later slices: slot-syscall handling
  asymmetry (module leg applies pc + 4, bridge leg keeps the pending
  stop); transfer-slot faults discard the module-side capture (same
  stop word and state on both engines, resume-after-fix open);
  outside-text direct calls now bridge instead of stopping (no
  prefix impact); the fell-off-extent guard is dead in all current
  evidence.
