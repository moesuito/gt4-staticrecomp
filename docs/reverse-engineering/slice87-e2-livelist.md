# Slice 87: E2 adjudicated on the ROM worker + final minimal live shopping list

Date: 2026-10-04. Baseline: main at 442e1c5 (slice 86, clean tree, no code
touched this slice). Task (slice 87): (1) re-read the 41h worker 0x80004A40
in BIOS 90001-v18 (read-only) to adjudicate E2 — the slice-84 journal says
the delete-woken waiter resumes with v0=-2 intact (no error); slice-85 §3
assumed a -1 release; the wake value is -1, -2, 0 or other? Record the exact
instruction path and close E2 as Confirmed or reformulate the gap; (2) the
FINAL minimal live shopping list (exact experiment: breaks per stage, reads
per stage, positive controls), ready for a future interactive session — no
live executed (slice-79 blockers stand, re-checked read-only below).
PROHIBITED and kept: no behavior change, no conclusion without a
discriminant, no game/BIOS bytes in git/docs, no live without a positive
control. No commit, no push, no branches.

Method: read-only disassembly of the pinned BIOS
(`private/pcsx2/bios/SCPH-90001_BIOS_V18_USA_230.ROM0`) and of the live
kernel image in the existing reference capture
(`private/pcsx2/menu-eeMemory.bin`, slot-9 EE RAM) with a minimal host
MIPS reader (approved host temp dir only, never in git). Only addresses,
counts, handle values, relations and short instruction-shape quotations
below (established practice since slice 82). Confidence labels per
project rule.

## (1) E2 adjudication — the wake value is -2 (park value, untouched), not -1

### Identity: the live worker IS the ROM worker (Confirmed)

The 94-word function at live RAM 0x80004A40 (0x4A40..0x4BB8, jr ra +
addiu sp,sp,160 epilogue — exactly the 94 instructions slice 84 counted)
matches byte-for-byte at ROM0 file offset 0x397490 (full 376-byte match,
prologue `addiu sp,sp,-160` / saves through the epilogue). The wait worker
0x80004CF0 likewise matches at ROM0+0x397740. So the re-read below is the
90001-v18 code itself, and the RAM image the reference executed is that
same code. (Side correction with its own cross-check, High confidence:
the kernel tables live one 64 KiB segment below where slices 80–84 cited
them — sema table 0x8001F800 not 0x8002F800, thread table 0x8001AC00 not
0x8002AC00, free stack/count 0x8001ABFC/0x8001ABF8 not 0x8002ABxx. Proof:
MIPS `sw/lw` offsets are SIGNED, and the create worker 0x800049B8
independently derives the sema slot as `(slot-0x8001F800)>>5` (32-byte
structs) while the thread table at 0x8001AC00 holds exactly 256 x 76-byte
entries ending where the sema table begins. Nothing in any verdict ever
depended on the segment; all addresses below use the corrected bases.)

### The park: waiter saves v0 = -2 (Confirmed, two exact sites)

- Wait worker 0x80004CF0: fast path takes the count (sema+4 > 0 →
  `move v0,s3`, i.e. returns the sema id, stores count-1) and returns;
  park path (sema+4 ≤ 0, `blez` at 0x80004D38) enqueues the thread
  (waiters++ at sema+20, queue insert, thread+32 = sema id) and returns
  **-2** at 0x80004D90 (`addiu v0,zero,-2`, sole return of the park path).
- Wait trampoline 0x80003440: 0x80003450 `addiu v1,zero,-2`, 0x80003454
  `bne v0,v1` → fast-return (ERET 0x800034B0 with v0 = id); taken-iff--2
  path parks (saves the current context — with v0 = -2 — dispatches,
  ERET 0x80003490). So a parked waiter sleeps with saved v0 = -2. This is
  the "-2 do park" the slice-84 journal cites, now pinned to instructions.

### The delete wake: no -1 is ever written toward a waiter (Confirmed, census)

Delete worker 0x80004A40 (entry checks re-confirmed with one decode fix:
0x80004A8C is `bgezl` (likely, rt=3), so a live slot (sema+4 ≥ 0) executes
its delay-slot waiter-count load and proceeds, while a free slot (-1)
falls through to the -1 fail return — deleter gets -1 iff id ≥ 256
(0x80004A54/70) or slot free, exactly the journal's claim):

- Wake loop 0x80004AD0–0x80004B68 per waiter: pop (jal 0x80005AF8),
  unlink (jal 0x80005B88), waiter-count-- (0x80004B00/0B0C at sema+20),
  status 12→8 by direct store (0x80004B1C `beql` + 0x80004B20
  `sw s6(8),0(s0)`), status 4→READY via jal 0x80005A58 (whose store is
  value 2) plus reschedule flag (0x80004B48 `beql` + 0x80004B54
  `sw s7(1)→[0x80015B74]`), free path (0x80004B6C: -1 marker at sema+4,
  LIFO push to [0x8001ABFC]), deleter return id (0x80004B80 `move v0,s5`).
- Complete -1 census over 0x80003400–0x80005C00 (every
  `addiu *,zero,-1`): inside the delete worker exactly two — 0x80004A98
  (fail return register) and 0x80004B74 (free-slot marker value); inside
  the whole wake loop and all three wake-path helpers (pop/unlink/ready/
  insert): ZERO. Every -1 in the region is an error-return register or a
  free marker — each goes to the syscall's invoker, never to a waiter.
- Complete store census on the delete path (worker + helpers): sema
  count, TCB+0 status (8 direct; 2 via the ready helper — the only
  ready-marking available, so it IS the 4→READY store by elimination plus
  reference progress), reschedule flag, free-stack links, queue links
  (offsets 0/4 only). No store targets any saved-context/saved-v0 area
  (thread contexts cannot live in the 76-byte TCBs — 35 registers do not
  fit — and no delete-path store leaves the enumerated set).

Therefore the woken waiter resumes with its park-time saved v0 = -2
untouched. The game's wait wrapper 0x00578500 (re-verified via gt4disasm:
retry to the queue-op 0x00578530 iff v0 == -1, else fall-through return
v0) sees -2 ≠ -1 and proceeds; the gate 0x00548660 (slice 86: never
checks v0) runs unconditionally. **E2 verdict: slice 84 CONFIRMED, slice
85 §3's "-1 release" REFUTED. The wake value is -2 — neither -1, nor 0,
nor a fresh error.** Draft 0037 §2-F's error-code clause is contradicted
(E1 already contradicted its re-resolve clause in slice 86).

### Residuals (non-blocking, named exactly)

- R1 (micro, does not touch E2): the waiter-index derivation inside the
  wake loop is unreduced on paper (0x80004AD8 `subu v0,v0,s4`,
  0x80004ADC/0AE8 result-discarded `mult`s — zero mfhi/mflo in the whole
  0x80004900–0x80005C00 region, verified by encoding scan — 0x80004AE0
  `sra s1,v0,2`, 0x80004AF0 `addu a0,s0,s4` with s0's writer unidentified
  in the static read). It cannot resurrect -1: every value stored toward
  waiters on this path is 8, 2, 1, counts or links (census above), and the
  saved-v0 slot is unreachable from the enumerated stores. Close-out: live
  break at 0x80005A7C reading at/s0/a1 + waiter-resume v0 (folded into the
  list below as optional G3), or a static data-flow from the dispatcher.
- R2 (micro-gap, no consumer): fast-path WaitSema returns the sema id
  (0x80004D40 `move v0,s3`), not 0 as our model returns. No game-side
  consumer distinguishes (the wrapper tests only == -1), so no behavior
  change and no fix — recorded so a future reader does not "correct" the
  model toward 0 on authority.
- R3/R4 (out of scope, noted once): create's `sw a0,4(a0)` shape at
  0x800049E4 was not chased; the result-discarded `mult` idiom across
  workers (barrier? compiler artifact?) was not explained.

### What changes in DRAFT 0038 (proposed delta, record untouched)

0038 stays DRAFT — E1 (contradicted, game-side) and E3 (unmet,
load-bearing: no invoker) still block promotion independently. What this
slice moves: exception E2 goes from CONTESTED to ADJUDICATED against the
-1 clause. The acceptance's F-clause must be reworded to the no-error
variant the game side already satisfies, e.g.: "the waiter's WaitSema
resumes with its park value (-2, never -1); the wrapper falls through on
any non--1 value and the gate runs without checking v0 (slice 86 body);
no retry iterates under real semantics (first delete succeeds — plus E1:
retries never re-resolve)." The in-model delete change stays gated behind
the invoker (i) exactly as 0038 orders. (This document + STATUS carry the
adjudication; the 0038 file itself is left for the owner's amendment.)

## (2) Final minimal live shopping list (no live executed)

Blockers re-checked read-only this slice (nothing launched, nothing
installed, nothing repointed): B1 no pcsx2 process; B2 ini still
`Bios = F:\Games\PS2\BIOS` (absent) with the 90001-v18 set present at
`private/pcsx2/bios`; B3 v2.9.93 states vs v2.9.94 emu skew untested;
B4 no debugger/logpoint session exists. All four stand — the session
below is specified, not run.

Goal of the session (minimal: the SMALLEST set that can promote 0038):
G1 invoker edge or live entry for publisher / TEARDOWN / late cluster
(closes E3/D6 — the only load-bearing gap left); G2 staged order
(teardown-vs-build, supports the F chain); G3 (optional, non-blocking)
waiter-resume v0 read + R1 reduction. E2 needs NO live (closed above).

- S0 calibration (positive control FIRST, PLAN §9.2–9.3 — mandatory
  before any causal read): boot the pinned ISO (repo root, 5,314,478,080
  bytes) on 90001-v18 (repoint ini to `private/pcsx2/bios` first; record
  the skew test B3 result), run to menu, extract EE RAM + regs. PASS
  iff slot-A = 0x11E, B+0x00/+0x34/+0x80s = 0x10D/1/four-live,
  gen-table 22×1 with 63/71/75 = 0 (slice-79 values), AND offline regs
  == live PINE regs with the text window hashing the pinned payload
  (the already-proven control, re-run live). If S0 fails, STOP — the
  session is blind, no causal read is valid.
- S1 staged savestates (G2; exact breaks + reads per stage): save at
  (a) init 0x00101938, (b) BUILD pass 0x005BC4C8 / flag [0x0088D7C8],
  (c) gate park 0x00548660 (W_B inside WaitSema), (d) menu 0x00568B94.
  Reads per stage: A-slot 0x0064C3C8, B+0x00/+0x34/+0x38/+0x3C/+0x40,
  B+0x80/+0x84/+0x88/+0x8C, sibling C same words, gen-table
  0x00874550 (ids 13/14/15/30 vs 63/71/75), flag, gate-word
  [0x00616F24]. Positive control per stage: the (b) stop must show the
  370-target BUILD census shape (slice 82) — else the tracer is blind.
  Discriminant: any stage with fresh ids stamped while the old waiter
  is still parked ⇒ teardown-vs-build order (H1 arm); TEARDOWN entry
  hit at any stage ⇒ invoker found (G1, session succeeds early).
- S2 delete-watchpoint (G1/G2 sharp end; only after S0 passes): break
  at delete-worker loop top 0x80004B00 with sema+20 (waiters) > 0, or
  watch writes to the parked waiter's TCB+0; FIRST record a natural
  delay-cycle signal-wake (slice-77 0x005AEF58/iSignalSema path) as the
  wake-path positive control, THEN read the delete-wake resume v0.
  Expected per this slice: resume v0 = -2 (G3 doubles as E2's live
  corroboration; a -1 here would REOPEN E2 — the bet is explicit).
- S3 invoker hunt (G1; bounded): break/log on entries
  0x005485E0 (publisher), 0x00548A00 (TEARDOWN), late clones
  0x0060FF18/0x0060FFD0/0x00610000/0x00610050, logging caller ra;
  positive control = the 370 BUILD entries hitting during boot (if the
  tracer misses those, stop). Bound the hunt (e.g. to menu); a miss is
  a result (D6 stands with wider bounds), not a failure.
- Stop rules (unchanged): no permanent installs without documented
  need; no marathon without a passing positive control; kill the
  emulator afterwards; `git status` clean (states/captures/logs stay
  out of git); every causal claim needs its control in the same
  session. Tripwires armed: 0x0086CBBC, B+0x00, generation table (any
  0x23x stamp or +0x3C != 0 falsifies the standing endpoint).

## Gates and hygiene

- Tree holds only this doc + STATUS + journal (no code touched;
  `src/ee/kernel.cpp:943` refusal re-read intact this slice — behavior
  unchanged by construction, zero diff).
- No payload bytes in docs/git: addresses, counts, values, relations,
  short instruction-shape quotations only.
- Full gates on the unmodified tree (VsDevCmd `-arch=amd64` chained):
  configure+build green (ninja no work); **CTest 53/53** (94 s);
  **Python 73 collected — OK (skipped=6)** (exit 0 via redirect; the
  direct-pipe exit=1 is the known PowerShell artifact).
- Hunt artifacts (MIPS reader script, disassembly dumps) live only in
  the approved host temp dir, never in the repo.
- No commit, no push, no branches.
