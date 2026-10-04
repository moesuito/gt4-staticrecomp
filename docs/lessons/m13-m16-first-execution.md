# M13–M16 lesson — first real execution: one function, then the startup

Prepared 2026-10-04. BUILD/VERIFY: passed 2026-10-01 for M13–M16
(M17 folded below only as the bridge to call trees); see the M13
evidence (`docs/reverse-engineering/m13-first-function.md`, with
translator slices 2–4 in the same file), [M15 evidence
(`m15-cop1-mmi.md`)], [M16 evidence
(`m16-unaligned-and-multiply.md`)], and [M17 evidence
(`m17-thunks-and-syscall-boundaries.md`)]. EXPLAIN: this is the
worked explanation; tutoring review pending.

## Objective and motivation

The tooling arc ends with everything *except* execution: blocks,
maps, state, and an interpreter proven only on synthetic
programs. This arc teaches the project's first contact with real
game code — translate one real function, verify it against the
interpreter the way synthetic programs were verified, then grow
the subset (branches, calls, integer ops, FPU, vectors,
unaligned memory, multiply) until the game's own startup runs
942,695 instructions to its first syscall. The standing rule
throughout: the interpreter is the oracle, agreement is the
evidence, and a shared speculation between implementations is
the named enemy (it bites exactly once, below).

The candidate discipline comes first: scan the evidence-backed
closures for a small leaf, and reject syscall stubs, COP0
accessors, and anything with jump tables or undecodable words.
What survives is `0x00577878` — a four-instruction setter
storing `a1/a2/a3` into `[a0]/[a0+4]/[a0+8]` whose third store
is the `jr ra` *delay slot*. The first translated function
exercises memory semantics and the delay-slot rule together,
which is why it was chosen, not because it is small.

## Step 1 — one statement per instruction, six states of agreement

`gt4translate` decodes with the M6 decoder, classifies with the
M7 flow model, and emits one C++ statement per instruction with
the assembly as comments — delay slot before the return, in
execution order; `jr ra` becomes `set_pc(low 32 bits of r31)`.
Scope is a leash, not a limit: single-block leaves ending in
`jr ra`, everything else rejected with the offending address
and instruction, no partial output. The generated header is
derivative of game code, so CMake generates it into the ignored
build tree and never commits it.

Verification is the M11 pattern pointed at reality: 6 input
states (zero, negative, high-bit and mixed patterns; different
structure pointers; different `ra` and junk registers), two
independent paths (translated C++ on fresh state, interpreter
on the same four real words from the verified CORE), compared
on all 32 registers, the entire memory image byte-for-byte, and
the continuation:

```text
translated 0x00577878 matches the interpreter on 6 input states
```

The milestone asked for five states; the file records the
shared-speculation caveat openly — agreement is implementation
correctness, exactly like the Ghidra comparison was for
decoding, not hardware proof. That proof is what the later
observation milestones add.

## Step 2 — grow by observed use: branches, calls, integers

Each translator slice is driven by the next real function, not
by a completeness table:

- **Branches** (slice 2): normal/likely/link forms, in-function
  loops, multiple `jr ra` returns; reachable blocks from the
  M7 walker define the extent. Verified on the lazy initializer
  `0x005c11a8` (guard word at `0x006599b8`; three cold states
  run the init path, three warm skip it).
- **Direct calls** (slice 3): a function *and its tree* become
  one module — `jal` writes the link, runs the delay slot,
  calls the callee as an ordinary C++ function, with forward
  declarations for cycles; indirect calls, exceptions and
  unsupported words anywhere reject the tree. Verified on the
  five-function stack-framing chain `0x0010c0c0` (both saved-`ra`
  slots compared).
- **Integers** (slice 4): LB/LBU, SRA, SLTI/SLTIU, XORI — the
  families blocking real candidates. Verified on the
  eight-instruction SRA leaf `0x00549378` (including
  `0x80000000`, where the sign fill matters).

And the cross-check catches a real bug: SLT/SLTU/SLTI/SLTIU
compare the **full 64-bit registers** (MIPS64, confirmed in
PCSX2's implementation as the independent source), while both
our interpreter and the Python reference compared 32 bits —
shared speculation that M11/M12 passed regardless, exactly the
charter's warned risk. Both fixed; the M11/M12 fixtures
regenerated (expected values changed where 64-bit values met
the comparisons); the generators now exercise the new ops. The
extended Ghidra run agrees instruction by instruction
(`matched=475 non_nop=406 unsupported=73 mismatched=0`), and
the invalidated negative fixture (`0x38081234`, now a valid
XORI) becomes positive coverage — the M6
fixture-invalidation lesson, again.

## Step 3 — the operations the startup needs, then the startup itself

COP1 scalar FPU (35: moves, arithmetic, accumulator forms,
compares, conversions, branches on the FCR31 condition bit),
MMI lane arithmetic (73: wrap/saturating variants, compares,
shifts, extract/unpack/interleave, the HI/LO second bank,
`mtsa` cache, `sync`), LQ/SQ quad moves with the upper-half
GPR storage — with the reference's exact special cases
(denormal flush, overflow clamp, `psubsw`/`paddsw` boundary
comparisons, `qfsrv` from the shift cache), documented because
several diverge from plain IEEE. Behaviors mirrored
deliberately, never copied. The entry window resolves: 29×
`padduw` clearing r1..r29, HI/LO/FPU accumulator clears,
`sync`, then the `.bss` align/clear loops (`sq zero` in
16-byte steps plus `sb` tails) from `0x006D5E00` to
`0x008A215C` — and the Ghidra comparison (548 rows: 511
matched, 34 R5900-only rows against the reference tables
instead, 0 mismatched) plus the junk-pre-filled `.bss` test
make the startup's 942,695 instructions to the first BIOS
syscall an observed clearing, not an assumed one. The startup
then translates as one 112-instruction module (loops and all,
with a halt address where the interpreter stops) and matches
on the full register files, both HI/LO banks, pc, and the
whole written window.

Unaligned access and multiply/divide follow the same pattern —
reference mask/shift tables (LWL/LWR/SWL/SWR, with LWR's
upper-half preservation forcing `write_gpr_low32` into the
state model), LWU/LHU/SH, MULT/DIV quirk cases (`0x80000000 /
-1` saturates; divide-by-zero signals; halves
sign-extended), MADD/MADDU plus compact second-bank forms,
PLZCW edges — hand-computed fixtures for every alignment and
edge, Ghidra 594 matched 0 mismatched, fifth function
`0x00572438` verified on 6 states. The recorded gaps read
like a work queue because they are one: `0x00579780`
(`break` in a likely delay slot), the `0x58ce48` tree entered
above its own back-edge, the parallel multiply family, BREAK,
COP0.

## Step 4 — the bridge to trees (M17, folded in)

Two structural changes, both about functions that do not fit
one extent: the walk may reach below the entry (tail thunks
jump down to shared stubs — scan from the lowest reachable
address, open with a `goto` to the entry, collapse empty runs
into range comments), and syscalls reached by the walk become
stop points propagating through call sites (`if (pc != link)
return;`). CACHE decodes as the reference's no-op hint. The
verifier is the tail thunk `0x005b27f8` (loads a word, jumps
into the BIOS trampoline, both sides stopping at service
`0x42`), and the `0x58ce48` tree advances gap by gap (movn,
lwl, thunk, cache) until the next recorded edge — a branch
targeting a delay slot at `0x005b0fcc`, the following slice's
problem, and eventually (standing project record) a 57-function
verified module.

## What later evidence reframed (not smoothed over)

- The VU0 macro set and the trapping/parallel arithmetic
  arrived afterward (M22–M24) the same way: observed use,
  reference tables, hand-computed edges — this arc's method,
  not a new one.
- The translator outgrew emission-per-operation (M25 runtime
  fallback), outgrew rejection (M27–M28 boundaries and
  dispatch), and outgrew per-tree budgets (M29 whole-program)
  — each step kept the differential harness this arc built.
- The candidate discipline (reject stubs/accessors/tables,
  verify leaves first) recurs every time a new family opens;
  the `.bss` junk-fill trick (prove clearing by pre-filling)
  recurs whenever a run must show work, not just arrival.

## Connection to our implementation

| Piece | File | Job |
| --- | --- | --- |
| Statement emitter | `tools/gt4translate` (+ CFG extents, halt addresses) | one statement per instruction, delay slots in order |
| Verified execution | `tests/unit/ee_translation_*` (startup, thunk, per-function) | 6-state differentials vs the interpreter |
| Integer/FPU/vector semantics | interpreter + `ee_state` (second bank, upper halves, FCR31) | reference-mirrored behavior |
| Fixture oracles | hand-computed rows + `scripts/synth_programs.py` | 64-bit fix propagated to both models |
| Independent reader | Ghidra runs over regions + candidate ranges | 475/511/594 matched, 0 mismatched |

## Understanding checkpoint

1. The first function stores its third value in the `jr ra`
   delay slot. Why did that make it the *best* first
   candidate rather than merely a small one?
2. The milestone asked for five states; the tests use six
   with junk registers. What does the sixth state plus junk
   buy that five clean states do not?
3. Both interpreter and Python model compared SLT at 32
   bits. Why did M11/M12 pass anyway — and what does that
   imply about what generated suites can and cannot catch?
4. The `.bss` test pre-fills with junk. Why is "comes back
   zero" without pre-fill not evidence of clearing, and what
   do the surviving margin bytes prove?
5. LWR with nonzero shift keeps the register's upper half.
   Why did that force a new state accessor instead of reusing
   the 32-bit write — and which later family depends on the
   same upper-half discipline?
6. M17's stop-propagation reads `if (pc != link) return;`.
   Explain what a pc mismatch means at that point, and why a
   resumed callee returning normally never triggers it.
