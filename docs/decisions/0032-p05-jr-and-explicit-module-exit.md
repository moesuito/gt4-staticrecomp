# 0032 - JR target capture and explicit module exit reasons (P05)

Date: 2026-10-04. Status: accepted (implemented in slice 68).
Predecessors: 0028 (compatibility policy), 0029 (P01+P02
origin/pending/mask/dispatch), 0030 (P03 unified advance machine),
0031 (P04 handler arguments and idle).
Plan: PLAN.md section 6, P05 (depends on P01-P03; independent of devices).
Closes PLAN.md C08/C09 (GPT F05/F06).

## Context

The Return emitter ran the delay slot before reading ra, while the
interpreter captures the jump target first: for `jr ra` with
`addiu ra, zero, 0x2000` in the slot and ra starting at 0x3000, the
compiled module jumped to 0x2000 and the interpreter to 0x3000
(GPT section 29.4, reproduced through the real emitter). Fixing only
the order was not enough: the driver inferred a return from
`PC == RA`, so a corrected return landing on an ordinary word
classified as InstructionStop, an applied ERET never continued (the
module derives the pc, the classifier looked for an ERET word at the
destination), and a BREAK with PC equal to RA repeated the same entry
until the budget ran out (10 module calls, 0 bridge steps in the
probe). The pc and the registers cannot name the stop; the module
must report it.

## Decision

1. **Capture before the slot.** The Return emission now reads ra into
   a function-top variable before the delay slot statement, exactly
   like the interpreter captures the target before running the slot
   and like the jalr/jr emission already did. ERET has no delay slot
   (applies immediately in both engines). A slot that rewrites ra
   changes the resumed register, not the jump destination
   (P05 acceptance: old destination kept, final RA updated).
2. **The module reports a BoundaryKind.** Every generated function
   returns the existing stop vocabulary instead of void: Returned for
   an applied `jr ra`, ExceptionReturn for an applied ERET,
   Syscall/Break/UnsupportedWord at the stopping word,
   IndirectTransfer for a transfer left to the bridge (unknown
   indirect target, and a direct call outside the file-backed text,
   whose link and slot are still to apply), and InstructionStop for a
   trapping arithmetic overflow (matching boundary_from_step).
   StepLimit, NoRunnableThread, Unmapped and IllegalDelaySlot stay
   driver-side: the module never emits them, and the budget
   reason remains the driver's own StepLimit.
3. **Callers propagate without overwriting.** Direct and indirect
   calls emit `exit_X = callee(state); if (exit_X != Returned ||
   pc != resume) return exit_X;`, and computed jumps tail-call
   `return detail::call_entry(state, target)`. A non-return reason
   reaches the driver unchanged (a trap inside an internal call is
   reported, not swallowed); a plain return that landed elsewhere
   still ends the caller. No service, link write or delay slot the
   module applied is ever repeated: the unknown-target path stops
   before writing the link or running the slot, and the bridge resumes
   exactly at the stop word.
4. **The driver trusts the report.** The module path switches on the
   returned reason: Syscall runs the service once (then pc + 4),
   Returned/ExceptionReturn/IndirectTransfer continue the loop,
   anything else is reported via report_boundary (kind, pc, guest
   word, v1 service). classify_boundary loses the `PC == RA` arm and
   stays as the word view for tests and reports; a return is never
   guessed from registers again.
5. **A pending transfer at an entry reaches the bridge.**
   `Driver::bridge_step_due_` skips one module entry after an
   IndirectTransfer: re-entering the same entry could only repeat the
   identical stop, so the bridge owns the next step even when the
   transfer word is itself a module entry. Returned and applied-ERET
   stops still re-enter normally (their pc moved to a genuine
   continuation). The new fixture exposed this: every synthetic stop
   sits at an entry, and without the guard the driver re-ran the
   entry instead of bridging.
6. **Synthetic fixtures go through the real emitter.**
   `gt4translate --synth spec output` translates original
   hand-assembled words (base, seeds, max, functions, words) with the
   same walk and emission as game code; the generated header is
   marked non-game content and is committable. No CORE is read on
   this path.

## Consequences

- The regenerated whole-program module covers the same code
   (15,068 functions, 924,991 instructions): codegen changed,
   coverage did not.
- Three Python CLI assertions that pinned the old emission (void
   functions, post-slot ra read) now pin the capture and the
   BoundaryKind returns. Updated with evidence per the 0029
   precedent, not silently.
- The driver stop at an outside-text direct call now bridges
   (pending transfer) instead of stopping as InstructionStop; the
   90,000-service boot prefix does not reach one, so the pinned
   frontier is unchanged.
- Out of scope, unchanged: DMA payload/tags (P06), RPC content
   (P07), quanta/clock, interrupts/masks.
- Known inherited edges, documented not fixed: a syscall in a delay
   slot is handled with the generic pc + 4 rule on the module leg
   while the bridge leg keeps its pending stop (the fixture pins the
   unhandled stop rule, not the registered slot case); a transfer
   whose slot faults discards the module-side capture (both engines
   stop at the same word with the same state; resuming past a fixed
   fault is future work); the trailing fell-off-extent guard is
   unreachable in every fixture and in the boot differential and
   stops cleanly instead of leaving a stale pc.

## Verification

New `ee_module_exit` CTest (11 legs, real emitter output compiled
in, driven through Driver against the interpreter: JR with ra
rewritten in the slot, ERET EXL/ERL to one common destination, trap
with PC == RA, plain-path syscall unhandled and handled-once,
known/unknown indirect, likely-branch taken slot-syscall and live
path, inner-trap propagation) plus extended `ee_driver` unit rows
(word view without inference, emitter-faithful ERET, pending entry
guard). Full CTest 51/51 (includes `gt4boot_services` 90,000 with
disc and the compare-interpreter differential, plus all
resume/autosave fixtures); Python 73 collected, 67 run, 6 skip,
exit 0 with the two known socket ResourceWarnings; build
warning-free (MSVC 19.44, Ninja, Debug). Evidence:
`docs/reverse-engineering/slice68-p05-jr-exits.md`.
