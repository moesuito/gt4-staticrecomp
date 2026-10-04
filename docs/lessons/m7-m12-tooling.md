# M7–M12 lesson — the reading tools: flow, map, state, interpreter, suites

Prepared 2026-10-04. BUILD/VERIFY: passed 2026-10-01 for M7–M12;
see the M7/M8/M9/M10/M11/M12 evidence
(`docs/reverse-engineering/m7-control-flow.md`,
`m8-function-map.md`, `m9-guest-state.md`, `m10-interpreter.md`,
`m11-synthetic-programs.md`, `m12-branching-programs.md`).
EXPLAIN: this is the worked explanation; tutoring review pending.
(Relation to `docs/lessons/m7.md`: that lesson teaches control
flow itself; this one teaches the six-milestone arc that made
every later translation verifiable.)

## Objective and motivation

Before this arc, the project can decode words and disassemble
regions, but it cannot say how code *moves*, where functions
*are*, what values *mean*, or what happens when code *runs*.
Six milestones build that floor in dependency order — flow,
then map, then state, then interpreter, then generated proof —
and every later lesson stands on it: the translator walks
blocks, the function map seeds modules, the interpreter is both
the reference every module is verified against *and* the bridge
that resumes past boundaries. The arc's through-line is stated
up front: static-reading tools with evidence grades, so that
nothing downstream ever trusts an unrecorded assumption.

The motivating failure is hypothetical but load-bearing: a
recompiler that emits straight-line C++ for a branch without
honoring its delay slot reads correctly and behaves wrongly —
and the bug surfaces far from its cause.

## Step 1 — how code leaves normal execution (M7)

`classify` answers one question per word — how does execution
leave here? — across eight kinds (fall-through, branch, jump,
call, return, indirect-jump, exception, unsupported), and
`build_basic_block` walks forward consuming the ending
transfer's delay slot into the block. The worked real block:

```text
.\build\gt4blocks.exe private/fingerprint-check/CORE.GT4 0x5a3140 40
block=0x005a3140 end=0x005a3170 instructions=12 ending=branch
target_known=1 target=0x005a31b0 continuation=0x005a3170
```

Twelve instructions, one transfer, two static successors, no
guesses. The honesty is in the edges: `jr ra` is a return by
ABI convention (register 31) — documented heuristic, not
proof; a branch inside a delay slot is architecturally
undefined, so the walk stops with `branch-in-delay-slot` and
claims no successors; an unsupported delay slot keeps the
transfer's facts and flags what it cannot read. Then
`build_control_flow_graph` walks breadth-first from seeds
(target first; calls recorded, not followed; outside-text
successors counted, never followed), and the real run shows
why functions cannot be assumed local: 15 blocks, 71
instructions, 20 edges, with a jump leaving the region for
startup code and three register calls (`0x5a3184`, `0x5a3190`,
`0x5a31c0`) going nowhere statically.

## Step 2 — entries from evidence only (M8)

The function map creates entries from exactly three evidences
— `elf-entry` (the declared address), `seed` (a caller-provided
start), `direct-call` (a JAL target observed inside an analyzed
block) — and each function is a bounded *reachable set*, never
a claimed boundary. Register calls, returns, exceptions and
unsupported words never create entries; a jump target is a flow
edge, not a function, because the M7 walk showed seeded code
jumping across regions into startup. The real closure:

```text
.\build\gt4funcs.exe private/fingerprint-check/CORE.GT4 0x5a3140 50 500
funcmap functions=10 blocks_sum=37 instructions_sum=173 discovered_calls=8 pending=0 limited=0
```

Ten functions discovered transitively with zero pending, from
the real entry (a single unsupported block — startup needs
COP1/MMI before the map can bootstrap from it) through
2-instruction thunks to an 18-instruction straight-line body.
Reachable sums are not partitions (regions overlap), caps queue
`pending` instead of truncating silently, and a late-invalid
seed is still reported — a regression fixture the slice's own
tests caught.

## Step 3 — what values mean, before anything runs (M9)

A 32-entry 64-bit register file (R0 hard zero, 32-bit writes
sign-extend by the CPU's rule, 5-bit indices that throw past
31, a plain 32-bit pc) and one byte-addressable little-endian
region (natural alignment per width, whole-access bounds with
uint64 anti-wrap math). No host undefined behavior anywhere on
the path: unsigned shifts, masked sign extension, no
reinterpretation of the byte buffer as a wider type. Errors
are context, not crashes — every invalid access throws naming
address and width — so the interpreter milestone can later
decide which failures become guest exceptions and which are
hard stops. Deliberately absent: HI/LO (arrive with the ops
that use them), exceptions, interrupts, caches, a second
region — and alignment errors *throw* instead of modeling the
hardware's address-error exception, a recorded divergence, not
an oversight.

## Step 4 — one instruction at a time, hand-checked (M10)

`Interpreter` fetches, decodes (M6), classifies (M7), executes,
advances — with the semantics each fixture confirms: 64-bit
comparisons with sign-bit REGIMM tests, likely-branch
nullification (untaken skips the delay slot), JAL linking
pc+8, JALR reading its target before writing rd, delay slots
executing before taken transfers, transfers inside delay slots
stopping as `IllegalDelaySlot`, syscalls and unsupported words
as stable stops (re-stepping repeats until the caller changes
something). The worked fixture fits in seven words
(`0x2408FFFF…0x240D1234`: signed vs unsigned `slt`, a
nullified `beql` delay slot, pc landing past the skipped
word). The slice's own incident is kept because it proves the
discipline: a hand-encoding slip (`0x3C081000` is `lui t0,
0x1000`, pointing the store out of the region) crashed the
first run — and the M9 context-carrying error named the exact
address, so the fixture, not the library, got corrected in
minutes. The same incident installed CRT-error-to-stderr
routing, so a crash fails the run instead of blocking it.

## Step 5 — generated volume with an independent oracle (M11–M12)

A self-written interpreter plus a self-written generator can
share a bug, so the generator must not be its own oracle:
`scripts/synth_programs.py` reimplements execution rules
independently in Python (explicit masks, no shared code) and
writes expected final states into committed fixtures
(`tests/data/synth-straight.txt`, 40 programs of 12–24
instructions over all 20 straight-line ops, seed 20261001;
`synth-branching.txt`, 30 programs over all 14 branch ops plus
jal/jr). CTest executes the fixtures; the Python suite
regenerates them byte-for-byte. Countdown loops, conditional
skips with nullification, and call/return shapes (jal link in
r31, `jr ra` after its delay) all terminate by construction,
with exact step counts asserted including delay slots. Three
development defects failed loudly at the right layer (a pc
that never advanced, a renamed accessor, a call shape assuming
word zero) — none reached a commit, each reported with the
offending address. The stated limit travels with the method:
volume plus implementation independence is not formal
verification, and a shared misunderstanding of the
architecture would fool both implementations alike — which is
what the later observation milestones (PCSX2 captures) exist
to check.

## What later evidence reframed (not smoothed over)

- The interpreter outgrew "test executor" twice: it became the
  reference every translated module is verified against *and*
  the bridge that resumes past boundaries the module cannot
  pass (M30 slices 1–2). Neither role was designed here; both
  were possible only because single-stepping with stable stops
  was already exact.
- Fixture style scaled up unchanged: whole-program
  differentials (`--compare-interpreter`) are the M11 idea
  (two implementations must agree) applied to the real game
  instead of generated programs.
- The abort-dialog routing installed after the M10 fixture
  slip is still load-bearing years of slices later: headless
  runs must fail as text, never hang on a dialog.
- M7's own lesson (`docs/lessons/m7.md`) teaches control flow;
  this lesson teaches the arc. Overlapping the worked example
  (`0x5a3140`, 12 instructions) is deliberate cross-reference,
  not duplication.

## Connection to our implementation

| Piece | File | Job |
| --- | --- | --- |
| Classification + blocks + CFG | `src/ee/flow.cpp`, `include/gt4recomp/ee_flow.hpp`, `tools/gt4blocks`, `tools/gt4cfg` | kinds, delay slots, deterministic traversal |
| Function map | function-map tooling + evidence kinds | entries from proof, bounded extents |
| State + memory | `src/ee/state.cpp`, `include/gt4recomp/ee_state.hpp` | registers, regions, context errors |
| Step executor | interpreter + `tests/unit/ee_interpreter_test.cpp` | exact single steps, hand-computed rows |
| Generated suites | `scripts/synth_programs.py`, `tests/data/synth-*.txt`, synth CTests | independent oracle, committed fixtures |

## Understanding checkpoint

1. A branch inside a delay slot ends the block as
   Unsupported with no successors. Why stop rather than
   model it — and what would a model need that the project
   does not have at M7?
2. A jump target is a flow edge, explicitly not entry
   evidence. Reconstruct the M7 observation that forced
   that rule, with addresses.
3. R0 ignores writes; 32-bit writes sign-extend. Give one
   concrete guest computation each rule changes the outcome
   of, versus the "obvious" (zero-extend/real-register)
   alternative.
4. The M10 fixture slip crashed on the *memory* scenario with
   an exact address. Why did the error's context make the
   fix a minutes-long fixture correction instead of a
   library investigation?
5. The generator "must not be its own oracle". Explain the
   shared-bug failure mode with a concrete example (an
   off-by-one in branch-target math), and why byte-for-byte
   regeneration does and does not address it.
6. Steps 3 and 5 both defer something to "later observation"
   (address-error exceptions; architectural misunderstandings
   fooling both implementations). Why do those two deferrals
   have the same shape, and what single milestone answers
   both?
