# M25–M29 lesson — scale-up: the shared executor, the survey, boundaries, dispatch, and the whole game

Prepared 2026-10-04. BUILD/VERIFY: as recorded in the sources —
M25: CTest 24/24, Python 71 (65 run, 6 skip); M26: CTest 24/24,
Python 72 (66 run, 6 skip); M27: CTest 25/25, Python 72 (66 run,
6 skip); M28: CTest 25/25, Python 72 (66 run, 6 skip); M29: CTest
25/25, Python 73 (67 run, 6 skip). See the
[M25 evidence](../reverse-engineering/m25-translator-runtime-fallback.md),
[M26 evidence](../reverse-engineering/m26-translation-survey.md),
[M27 evidence](../reverse-engineering/m27-indirect-flow-boundaries.md),
[M28 evidence](../reverse-engineering/m28-module-dispatch.md), and
[M29 evidence](../reverse-engineering/m29-whole-program-build.md).
EXPLAIN: this is the worked explanation; tutoring review pending.

Every load-bearing statement below traces to one of those five
documents. The forward pointer (M30's driver/bridge) is noted
explicitly at the end and is a pointer, not a new claim.

## Objective and motivation

M24 ended with decode effectively complete — 4 unmodeled words in
the real code region — yet only a fraction of the game translated:
each unknown word rejected its whole tree, and each indirect call
rejected its whole tree. This arc teaches the project's scale-up
motion: stop rejecting what you cannot see through, measure exactly
what remains, and turn each rejection class into a run-time
contract. It ends with the whole game as one module — 15,068
functions, 924,991 instructions — that is well-formed C++ under the
real compiler. The honestly recorded remainder (~31% of words the
direct-call walk cannot see) is classified, not hidden.

Three blockers motivate the five slices: a long tail of decoded-but
unemitted operations (M25), indirect control flow killing 93% of
the failed trees (M26→M27→M28), and a fixed module size with no
whole-program mode (M29).

## Step 1 — the long tail rides the interpreter's own code path (M25)

M25 makes the runtime executor public —
`ee::execute_plain_effect(GuestState&, const DecodedInstruction&)` —
which runs one already-decoded instruction's register and memory
effect without touching the pc, returning false when a trapping
overflow fires. It is the same code path the interpreter uses, so a
translated module cannot drift from the interpreter on those
operations.

The translator falls back to it for every decoded plain operation
with no inline C++ form yet: the whole VU0 macro table, the COP2
moves and quad accesses, the remaining MMI forms (pmaxw/pminw/pcpy*
and friends) and the trapping arithmetic. The emitted statement is
a checked call:

```text
if (!ee::execute_plain_effect(state, ee::decode(0x…u))) { state.set_pc(0x…); return; }
```

so a trapping overflow stops the module at the instruction's
address, exactly where the interpreter stops; every other operation
always completes. Operations the decoder does not implement still
reject the whole module at generation time, as before. The
generated header now includes `gt4recomp/ee_interpreter.hpp`; the
inline forms for the common operations stay in place, so the
fallback only carries the long tail.

Evidence is a new verified module: `0x0056DF58`, a 133-instruction
vector convert/scale loop whose body runs the MMI forms (pextlw,
pmaxw, pminw, pcpyld, pcpyh, pmfhl, pcpyud, pmadduw, pmulth) and the
division staging — 32 of its instructions reach the runtime
executor. The differential test (`ee_translation_56df58`) runs
three input states (one and two outer passes, and a zero-count pass
that skips the inner loop) and compares all 32 registers, the pc,
both HI/LO banks and the whole scratch memory window (inputs, the
tail loop's array, the final store target and the saved registers)
against the interpreter. All three match.

Limits, honestly kept: the fallback decodes the instruction at run
time on every execution — a future slice can emit the decoded form
or inline tables where speed matters, with correctness unaffected;
operations outside the decoder's subset still reject the module
(VCALLMS and the VU0-memory forms, BC0F, the two unassigned words);
and indirect calls remain unsupported by design — which is exactly
what the next slice measures.

## Step 2 — measure the whole text, and let the blocker name itself (M26)

M26 adds a survey mode (`gt4translate CORE.GT4 --survey`; the walk
logic factored into `collect_units` so the same validation runs for
one function and for the survey). Every direct-call target inside
the text becomes a candidate entry — 15,067 unique `jal` targets
over 97,269 call sites — each walked with the standard call-tree
translation bounded by 20,000 instructions and the 256-function
limit, with the outcome recorded: translated, or the rejection
reason grouped. The report also counts the union of instructions
the successful trees reach:

```text
survey: entries=15067 translated=9345 functions_in_trees=58397
covered instructions: 399046 of 1334917 words in the file-backed text
reason: 4576 x Indirect calls are not supported
reason: 1055 x Not supported by this translator
reason: ~100 x no reachable instructions (jal targets in data or misaligned spots)
reason: 5 x start validation edge cases
```

So 62% of the direct-call targets translate as standalone trees
(9,345 of 15,067), covering 399,046 instructions — about 30% of
the real code region (1,334,218 words). The dominant blocker is
**indirect control flow**: 4,576 trees fail on a `jalr` call and
1,055 on a computed `jr` (jump tables and function-pointer
dispatch) — together 93% of the rejections. The rest are `jal`
targets that are not code (data words hit by stray call sites) or
start-validation edge cases.

The source states the meaning plainly: instruction-level coverage
is effectively complete for the code region (four unmodeled words
total); what limits translation now is **control flow structure**,
not instruction semantics. And it names the next milestone in
advance: a table of statically known function entries (the survey
already knows them), translating `jalr`/computed `jr` as a
dispatch through it that stops with context on unknown targets.

Evidence: the output is reproducible
(`gt4translate private/fingerprint-check/CORE.GT4 --survey`, ~15
seconds); a Python CLI smoke test asserts the report shape and the
dominant blocker. Limits: an entry counts as translated only when
its whole direct call tree validates, so 62% is a lower bound on
function coverage; `jal` targets inside the trailing 700-word data
table are filtered by the text range, and stray call words naming
non-code appear as harmless "no reachable instructions".

## Step 3 — stop guessing runtime targets; stop at the transfer (M27)

M27 turns every runtime-target transfer into a boundary — the
module executes the identical prefix and halts with the pc at the
boundary, exactly where the interpreter stops, instead of rejecting
the whole tree:

- **Indirect calls (`jalr`)** stop before the call with the pc at
  the transfer (the delay slot belongs to the call, so it is a
  boundary word too; the link register is not written, because the
  driver executing the call writes it).
- **Computed jumps (`jr` through another register)** stop the same
  way.
- **Instructions the model does not execute** (VCALLMS/VCALLMSR,
  the unassigned encodings) stop at the word, mirroring the
  interpreter's Unsupported stop.
- **Unmodeled instructions in a transfer's delay slot** stop at the
  slot, after the transfer's own state effects: a likely branch
  skips the slot when not taken (the existing trap-slot emission),
  a `jal` writes its link first, and `jr ra` stops without state
  changes. A function whose first instruction is a boundary
  translates as a stub that stops at its own entry.

The continuation code after an indirect call is still translated,
so a driver can resume there — the module never guesses a runtime
target; it leaves the dispatch to a future driver.

Results on the pinned CORE:

```text
survey: entries=15067 translated=14938 functions_in_trees=316092
covered instructions: 858621 of 1334917 words in the file-backed text
reason: 119 x The call tree exceeds the function limit
reason: 5 x The call tree exceeds the instruction budget
reason: 5 x start-validation edge cases
```

**99.1% of the direct-call targets translate** (14,938 of 15,067),
up from 62%; covered instructions rise from 399,046 to **858,621
(64.3%)**. Every remaining rejection is module-size policy, not a
semantic gap: the 256-function and instruction-budget limits plus
five start-validation edges.

Evidence: a new verified module, `0x00101C28` — a two-instruction
trampoline ending in `jalr ra, a0`, run over three target-register
values (zero, code, scratch), comparing all registers, the pc and
scratch memory after stopping at the same jalr address as the
interpreter; the Python CLI suite checks that a formerly rejected
function (`0x5a3140`) translates with `state.set_pc(0x005a3194u);`
at its jalr, that the trap-only seed becomes a boundary stub, and
that the survey reports the outcome; a spot check of the `jr ra` +
VCALLMS-in-delay-slot idiom (`0x004A53F8`) shows the module
stopping at the slot (`set_pc(0x004a53fc)`), matching the
interpreter.

Limits: the boundary stops *before* the transfer, so a driver must
resolve the target (or run a VU0 micro interpreter for VCALLMS)
and resume from the stopped pc; modules keep the 256-function
limit, and whole-program builds will want a larger policy (the 119
rejections are exactly that).

## Step 4 — known targets dispatch inside the module (M28)

M28 replaces the stopping convention with an in-module dispatch
table for known targets. Every generated module carries a table
over its own function entries (`detail::has_entry` and
`detail::call_entry`, one case per translated function), and the
indirect transfers use it:

- **`jalr rd, rs`**: the target is read first (a shared `rd == rs`
  encoding still jumps to the old value), then the link goes to
  `rd` (pc+8), then the delay slot runs, then the entry is called.
  A callee that stops at a boundary propagates through the caller
  (`if (pc != link) return;`); a callee that returns lets the
  caller continue inline, exactly like a direct call. When the
  target is not one of the module's entries, the module stops at
  the transfer, before the link or the delay slot — the M27
  boundary remains as the honest fallback.
- **Computed `jr rs`**: the same dispatch; the target returns
  through this function's `ra`, so the module returns after the
  call.
- `eret`, the unmodeled words and unmodeled delay slots keep their
  M27 behavior (boundaries at the exact interpreter stop point).

Results: the translated-entry count is unchanged (the same trees),
while covered instructions rise from 858,621 to **864,507** —
because the computed-jump words and their delay slots are now
emitted instead of being erased as boundaries. The rejection rows
are unchanged (119 function-limit, 5 budget, 5 validation edges).

Evidence: the `ee_translation_101c28` test now runs two states —
known target (`a0 = 0x0058B268`, a module entry: target read, link
written, delay slot run, dispatch into the callee, break boundary
propagated — all 32 registers, pc and scratch memory equal with
the interpreter at the same stop pc) and unknown target
(`a0 = 0x00100400`: stop at the jalr, identical to the
interpreter's prefix).

Limits: the table covers the module's own entries — a
whole-program build will want a global registry, and until then a
target translated in another module still stops the boundary; a
computed `jr` into a *local* block (a jump table inside the same
function) is not a function entry and keeps stopping; the
"callee returns, caller continues inline" branch reuses the direct-
call machinery the earlier differential tests cover, while the
known-target state exercises the dispatch, link and delay-slot
ordering.

## Step 5 — lift the size policy and build the whole game (M29)

M29 adds the policy knobs and the mode: `--functions N` sets the
module's function limit (default 256) in every mode, and `--all`
translates every direct-call target in the text plus the ELF entry
as one whole-program module, bounded by the instruction budget and
the function limit. Two latent gaps surface and close: direct calls
whose target lies outside the file-backed text (five in the pinned
CORE, pointing into the data segment) now stop at the call like the
other unknown-target boundaries instead of aborting the walk (and
walk failures name their function), and the COP1 branch conditions
(`bc1f`/`bc1t`/`bc1fl`/`bc1tl`) join the emitter — a gap no earlier
target used, found by the whole-program run — reading FCR31's
condition bit exactly like the interpreter.

The whole-program build on the pinned CORE:

```text
gt4translate CORE.GT4 --functions 20000 --all 2000000 generated/whole-program.hpp
```

**15,068 functions, 924,991 instructions, 146.4 MB, 2.57M lines,
136 seconds** of generation. The header **passes an MSVC syntax
check** (`cl /Zs`) in **27.5 seconds**: the whole game is
well-formed C++ within the compiler's limits (full codegen
unattempted — the next scale question). The module's own dispatch
table covers all 15,068 entries, so every indirect target inside
the game's direct-call closure resolves at run time; only genuinely
dynamic targets (or the five out-of-text calls) stop at the
boundary. Measured separately: a Debug `/Od` compile with `/bigobj`
and the dispatch referenced emits all 15,068 functions in **38.9 s
at 0.53 GB peak RAM** (96.7 MB object, 45,345 sections); Release
`/O2` unmeasured.

The survey with the policy lifted reports **14,991 of 15,067
entries (99.5%)** and **871,317 covered instructions (65.3%)** —
the only remaining rejections are the survey's own per-tree
instruction budget (76 × 20,000), a reporting parameter, not a
translation limit.

The source then answers the owner's "what do these numbers mean"
question, and the lesson keeps that framing because it is the
honest way to read every count in this arc:

- **Entries** are the 15,067 addresses the game calls with a direct
  `jal` — the best available proxy for its function list, not a
  ground truth.
- **Covered instructions** is the union of addresses inside the
  successful trees (871,317; the whole-program module itself holds
  924,991, i.e. 69.3%). The missing ~31% is **not** "untranslatable
  code": it is code the direct-call walk cannot see — jump-table
  bodies reached only through computed `jr`, functions only ever
  called through pointers, and the 700-word data table (0.05%).
  The exact split between the first two is not measured yet.
- **Every gap is a stop with context at run time, not a silent
  guess**: computed `jr` into an untranslated local block,
  VCALLMS, the five out-of-text calls and every BIOS syscall stop
  where the interpreter stops. The next milestones (full codegen,
  driver, services) are about turning those stops into execution.

## What this arc does not claim (forward pointer)

- **M30's driver/bridge is the consumer of all of this.** The
  boundaries (syscall, trap, `eret`, indirect-transfer, unmodeled
  word), the resume points (continuations, dispatch entries), and
  the whole-program module are the exact surface M30 executes: a
  driver that runs a translated module as a program, classifies
  where it stops from the guest state, and continues — through a
  service handler or through the step-by-step interpreter — from
  the stopped pc. Nothing in this arc executes a program; that is
  deliberately the next arc's job. Forward pointer only.

Nothing here contradicts that later work: this arc made the long
tail executable without drift (M25), measured the field (M26),
stopped honestly at runtime targets (M27), dispatched known ones
inline (M28), and built the whole game as one well-formed module
(M29). Whether a stop *resumes into execution* is the separate
question M30 answers on top of these contracts.

## Connection to our implementation

The five sources name tools, modes, units, and orderings — not
file paths — so the table maps each piece to its mechanism and its
source instead of inventing locations:

| Piece | Mechanism (as cited) | Source |
| --- | --- | --- |
| Shared executor | `ee::execute_plain_effect` — same path as the interpreter; false on trapping overflow | M25 |
| Fallback emission | checked call stopping at the instruction's address; header gains `gt4recomp/ee_interpreter.hpp`; inline forms kept for the common ops | M25 |
| `0x0056DF58` module | 133 insns, 32 reach the executor; 3 states (1/2 outer passes, zero-count skip); regs + pc + HI/LO + scratch window | M25 |
| Survey | `gt4translate --survey` over 15,067 `jal` targets / 97,269 sites via `collect_units`; 20,000-insn + 256-function bounds; union-of-trees count | M26 |
| Survey result | 9,345 translated (62%), 399,046 insns (~30%); 4,576 `jalr` + 1,055 computed `jr` = 93% of rejections | M26 |
| Indirect boundaries | stop before the transfer at the interpreter's pc; link unwritten; delay-slot/unmodeled-word rules; entry-boundary stub | M27 |
| `0x00101C28` trampoline | `jalr ra, a0` over zero/code/scratch; same-pc stop; `0x5a3140` → `set_pc(0x005a3194u)`; `0x004A53F8` → `set_pc(0x004a53fc)` | M27 |
| Boundary result | 14,938 translated (99.1%), 858,621 insns (64.3%); rest is size policy | M27 |
| Module dispatch | `detail::has_entry`/`call_entry`; target-before-link, slot, call; `if (pc != link) return;`; unknown keeps M27 stop | M28 |
| Dispatch result | entries unchanged; 864,507 insns (jump words + slots now emitted) | M28 |
| Policy + `--all` | `--functions N` (default 256); `--all` = every `jal` target + ELF entry; out-of-text calls stop; failures name the function; COP1 branches join | M29 |
| Whole-program build | 15,068 functions, 924,991 insns, 146.4 MB, 2.57M lines, 136 s; `cl /Zs` 27.5 s; Debug `/Od` 38.9 s, 0.53 GB peak, 96.7 MB / 45,345 sections | M29 |
| Lifted survey | 14,991 (99.5%), 871,317 insns (65.3%); 76 rejections are the survey's own per-tree budget | M29 |

## Understanding checkpoint

1. The fallback emits a *checked* call that sets the pc and
   returns when the executor reports false. Why must the stop land
   at the instruction's own address, and what would break if the
   fallback ignored the return value?
2. The survey counts an entry as translated only when its whole
   direct call tree validates. Why is 62% a lower bound on function
   coverage rather than an exact figure — and what does the
   union-of-trees count leave out by construction?
3. `jalr` stops *before* the call with the link register unwritten.
   Whose job is the link write under this convention, and why
   would writing it in the stopped module be wrong?
4. M28 reads the `jalr` target before writing the link, noting the
   shared `rd == rs` encoding. Construct the failure: what target
   does the module jump to if it writes the link first when
   `rd == rs`?
5. Covered instructions rise from 858,621 (M27) to 864,507 (M28)
   with the entry count unchanged. What exactly do those ~5,900
   new words consist of, and why were they "erased" before?
6. M29 says the missing ~31% is not untranslatable code. Name its
   three components, explain why the direct-call walk cannot see
   the first two, and state what runtime behavior covers a `jr`
   into such code.
