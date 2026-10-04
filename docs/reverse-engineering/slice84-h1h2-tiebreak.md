# Slice 84: H1/H2 tiebreak attempt — the old waiter's fate in the reference (decision: none — observation only)

Date: 2026-10-04. Baseline: main at 3360b2b (slice 83, clean tree, no code
touched this slice). Task (slice 84): break the H1/H2 tie of slice 80 —
did the old waiter (A=B=0x13F, thread 1 inside WaitSema on sema 63) unpark
in the reference (H1: woken with error when the reference deletes/recreates
the semaphores) or never park (H2: the menu phase comes by a path that does
not pass the node) — using ONLY existing offline observation (savestates in
`private/pcsx2/`, prior captures) + static disassembly of our binary + the
local ROM BIOS read-only; plus close/grade the D4 gap (real-BIOS
DeleteSema-with-waiters semantics vs PS2Tek). PROHIBITED and kept: no
behavior change, no live PCSX2 (slice-79 blockers ×4 unresolved), no
conclusion without a discriminant, no game/BIOS bytes in git. No commit, no
push, no branches.

Method: re-reads of the pinned input (`private/fingerprint-check/CORE.GT4`
— only addresses, counts, handle values and relations below) via
`build/gt4disasm.exe`; re-reads of the existing reference capture
(`private/pcsx2/menu-eeMemory.bin`, slot-9 EE RAM) with host Python
`struct`; read-only disassembly of the BIOS path as captured live (RAM
vectors + dispatch tables in the same snapshot) and of the ROM vectors in
`private/pcsx2/bios/SCPH-90001_BIOS_V18_USA_230.ROM0` (the slice-79
reference line) with a minimal host MIPS reader. Helper scripts live only
in the approved host temp dir, not in git. Confidence labels per project
rule.

## (1) The waiter's fate — what the offline evidence can and cannot say

### Re-verified endpoint (Confirmed, independent re-read of slot-9 RAM)

| Location | Reference value |
|---|---|
| slot A `0x0064C3C8` | **0x11E** = id 30, gen 1 |
| job B `0x0086CB80+0x00` | **0x10D** = id 13, gen 1 |
| job B `+0x34` | **1** (submitted) |
| job B `+0x38` | 0x6897f8 (same object slot as our stop) |
| job B `+0x3C` | 0 |
| job B `+0x40` | 0x10E (id 14, gen 1) |
| job B `+0x80/+0x84/+0x88/+0x8c` | 0x1bef0 / 0x59440 / 0x90ea80 / **1 (four live)** |
| sibling C `+0x00/+0x34/+0x38/+0x40` | 0x10F / 0 / 0x6897d8 / 0x86cd00 |
| gen-counter table `0x00874550` | **22 nonzero, ALL = 1**: ids 6–16, 26, 29–37, 42 — incl. 13/14/15/30; ids **63/71/75 = 0** |

Corroborating color (low weight): the snapshot CPU runs game text at pc
0x00568b94 with epc 0x005ADCC8 — the `jr ra` of the signal stub
0x005ADCC0 (sys 0x42) — i.e. the menu thread was inside a SignalSema call;
semaphores are actively signaled at menu phase.

### The gate RAN in the reference — task-H2 bypass FALSIFIED (Confirmed)

Re-disassembled verbatim (`gt4disasm`, read-only):

- Publisher 0x005485E0: hardcoded object B; `sw s1,+0x80; sw s2,+0x84`
  (the CALLER's a0/a1 — exactly two fields); `dispatch(B,2,1)`; return.
- Gate 0x00548660: `wait(A)`; `wait(B)` (W_B parks on the second);
  `+0x3C=1`; publishes **four** args (`sw s3,+0x80; sw s2,+0x84` — the
  second in the starter-`jal` delay slot so it executes; `sw s4,+0x88`
  in the next `jal` delay slot; `sw s5,+0x8c`); starter 0x00576ad8;
  `dispatch(B,3,1)` (submit); `signal(A)`; return.

Slice 80's whole-text writer closure (cited, not re-run): `+0x80..+0x8C`
are written ONLY by these two functions. Since the publisher writes only
`+0x80/+0x84`, the reference's live `+0x88 = 0x90ea80` / `+0x8c = 1` can
only have come from the gate. (B is a static object, so the four args
prove the gate ran ≥ once on it — whether pre- or post-recreation is not
established.) H2 as stated for this slice — "the menu phase comes by a
path that does not pass the node" — is therefore FALSIFIED. What survives
is H2′: the reference passed the node but never parked (pulse-first
interleave), vs H1: it parked and the delete rescued it.

Residual H3 (publisher submitted the menu job over early-gate arg
residue): unfavored — it needs D6's unknown caller (zero static referrers
of any kind, never entered in the 20k live leg) PLUS a fresh unit on the
fresh B sema, and explains nothing the gate-submit does not. Submitter =
the gate at High confidence, not Confirmed.

### Counter + allocator evidence FAVORS H1's order (Hypothesis, not a discriminant)

Create wrapper 0x005782e8, verbatim tail: `counter = ([0x00874550+id*4]+1)
& 0x7FFFFFFF` (with a wrap-to-1 guard); `handle = id | (counter<<8)`;
store counter. So each wrapper-create of an id bumps its counter: first
create stamps gen 1, a second create of the SAME id would stamp gen 2
with counter 2. Observed: every nonzero counter is 1, none ≥ 2 — no BIOS
id was ever wrapper-created twice.

The BIOS worker for sys 0x40 (0x800049B8, §2) pops the free stack at
`[0x8002ABFC]` (LIFO reuse; empty stack = failure, no fresh-bump logic —
so the stack is pre-populated at boot and every id comes from it). Under
LIFO, a teardown-then-rebuild order would most plausibly re-issue the
just-freed id E (top of stack) and stamp gen 2 / counter 2 — not observed.
The observed shape (fresh ids 13/14/15, each counter 1) fits a
build-before-teardown order: the fresh semas were popped while the early
ones were still allocated (E not yet on the free stack), and the old ones
— with the waiter still parked — were deleted after. That is H1's order.
Caveat (why this is not a discriminant): the game counter table sees only
wrapper creates; interleaved DIRECT-CREATE syscall users (the counter gaps
17–25, 27–28, 38–41 prove they exist) can bury E under other freed ids, so
teardown-before-build survives with extra assumptions. Supporting,
not closing.

### Endpoint-equivalence (the honest negative result)

Both surviving hypotheses predict IDENTICAL menu-time observables: fresh
gen-1 ids, `+0x34 = 1`, four live args, all counters 1, gate residue
cleared or not. The H1/H2′ difference (waiter parked vs not at delete
time) is purely temporal and leaves no residue distinguishable at the menu
endpoint with the two anchors in hand (both are menu-phase; slice 79).
So no offline discriminant closes the tiebreak: the missing experiment is
named in §4.

## (2) Delete-with-waiters — handler 41h of the local BIOS (D4 CLOSED)

### How the 41h path was found (all read-only)

ROM vectors spin by default (each ExcCode entry at 0xBFC00480+ is an
infinite loop), so the live path installs RAM vectors: the slot-9 RAM
holds the kernel-installed general dispatcher at 0x80000180 (`Cause &
0x7c` → table at **0x80015900**), whose ExcCode-8 (syscall) entry points
at **0x80000280**: negative-v1 aliases are negated (`bltzl` + `subu`),
v1 == 124 takes a special jump, else `table[0x80015500 + v1*4]` is called.
The live syscall table (RAM 0x80015500) cross-checks perfectly against the
game's own stub cluster (each stub is `addiu v1,NR; syscall; jr ra`):

| sys | Live target | Role (game-stub cross-check) |
|---|---|---|
| 0x40 | 0x800049B8 (direct worker) | Create (stub 0x005ADCA0; wrapper 0x005782e8 calls it) |
| 0x41 | trampoline **0x80003540** → worker **0x80004A40** | Delete (stub 0x005ADCB0) |
| 0x42 | trampoline 0x800034C0 → worker 0x80004BC0 | Signal (stub 0x005ADCC0; wrapper picks 0x42 vs −0x43 on Status.IE) |
| 0x43 | 0x80004BC0 direct | −0x43 negated = iSignal (disp
...[truncated 7373 chars]