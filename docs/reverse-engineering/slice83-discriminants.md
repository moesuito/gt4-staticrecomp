# Slice 83: discriminants D2/D4/D6 — publisher hunt, delete semantics, cheap D1–D3 remainder

Date: 2026-10-04. Baseline: main at a3bd3ce (slice 82, clean tree).
Task (slice 83): the remaining discriminants of draft decision 0037's
program (D5 closed in slice 82): D6 (publisher 0x005485E0's caller —
static hunt + live-boot confirmation where reachable), D4 (real-BIOS
DeleteSema-with-waiters semantics — model vs external evidence, gap
named, no behavior change), D2 (whatever of D1–D3 is cheap and
high-leverage). PROHIBITED and kept: no behavior change, no fabricated
traffic, no conclusion without a discriminant, no game bytes in git.
No commit, no push, no branches.

Method: whole-text scans over the pinned input
(`private/fingerprint-check/CORE.GT4` — only addresses, counts, handle
values and relations below) via `build/gt4disasm.exe` plus a read-only
inflate+scan host Python script (zlib raw-deflate + record parse, same
layout as `src/executable/core_image.cpp`; text base 0x00100000,
1,334,917 words; data record base 0x00617A80, 779,132 bytes). The scan
script lives only in the approved host temp dir, not in git. Live legs
use a temporary env-gated (`GT4_PUB_TRACE`) read-only pc/ra/a0/a1 log
at driver module entries and bridge steps, reverted via `git checkout`
after extraction, with a neutrality triple. External evidence via
public references (PS2Tek, ps2sdk, ps2autotests file listing); grades
per project rule. Confidence labels per project rule.

## (1) D6: the publisher's caller hunt

### Publisher body, verbatim (Confirmed, `gt4disasm`)

0x005485E0: `lui s0,0x87; sd s1/s2/ra; addiu s0,s0,-0x3480`
(= 0x0086CB80 = job object B — hardcoded, NOT a parameter);
`jal 0x00578500; lw a0,0(s0)` (wait on B+0x00);
`sw s1,+0x80; sw s2,+0x84` (publish the CALLER's a0/a1 values);
`dispatch(B,2,1)` at 0x0054861C (`jal 0x00578168` with a1=2, a2=1);
restore and `jr ra`.

CORRECTION of slice 81-E4 (recorded honestly): the publisher does not
take a "param object" — the object is hardcoded B and the caller's
a0/a1 are published VALUES (B+0x80/+0x84). Consequence: slice 81-E5's
"param-obj residual" is CLOSED as an object question (the publisher
names B directly and would set B+0x34=1) but stays OPEN as a caller
question (who invokes it, with which two values). The publisher is a
genuine second submitter of B — behind an unknown caller.

### Static scans (Confirmed, whole text + file-backed data)

- `jal` census (all 1,334,917 words): publisher **0 sites**
  (re-verified independently). Cross-checks all match slices 78/80/81:
  wait-wrap 111, signal-wrap 31, dispatch 79, create-wrap 13,
  resolve 4, delete-wrap 11, init ← sole caller 0x00101938,
  dispatcher ← 2 (BUILD+0xC/TEARDOWN+0xC), B/C builders and
  teardown-B/C ← dispatcher only, gate ← tail only, gate-tail ← 3
  (below), pulse/waiter40/gate-worker/callback/BUILD/TEARDOWN = 0,
  bfunc ← 0x004ACB70.
- Pointer-constant scan (`word == target`, text AND file data):
  0x005485E0 appears **nowhere** (0 text, 0 data). Worker table
  re-read at 0x006897E0: `{0, teardownC 0x00548498, 0, 0x00578288,
  0, 0, 0x0060FDF0, 0, teardownB 0x005483C0, 0, gate-worker
  0x00548428, 0, 0}` — publisher not among the entries (matches
  slice 80's map). BUILD-wrap occurs only at table2[27] (0x0061746C);
  TEARDOWN-wrap only at the 0x0061799C data-table coincidence.
- `lui`-materialization scan: 55 `lui *,0x54` + 34 `lui *,0x55`
  sites whole-text; **zero** with a same-register low-0x85E0 use
  (`addiu`/`ori`) in the following 8 words — no direct static
  construction of 0x005485E0 (caveat: exotic constructions, e.g.
  arithmetic composition or wider windows, are not excluded).
- Table membership: publisher not in the 60-entry boot-step table,
  not in the 370-word run (D5), not in the worker table.

NEW adjacent static fact (refines slice 81-E1, which listed only the
gate's single caller): the gate-tail 0x00548640 itself has THREE
direct `jal` callers — 0x004B1D0C and 0x004B1D40 (archive region:
`jal 0x00548640` with file-object args in a0–a3, next calls into
0x004AC430/0x004B3840/0x004B3BD8) and 0x00548CC8 (the PCDV read
path — thread 1's own stop chain). So the gate path runs in our boot
via the tail; the publisher has no such edge anywhere.

### Live 20k leg (Confirmed, neutrality triple, reverted)

Temporary hook (driver module-entry + bridge-step, watchlist
{publisher 0x005485E0, BUILD 0x005489E0, gate 0x00548660}):
exactly ONE line in 20,000 services —
`bridge pc=0x005489e0 ra=0x005bc564` (walker-loop ra, walker-register
a0/a1 residue — the slice-82 indirect-entry shape). Publisher: ZERO
hits. Gate: does not surface on the bridge (it runs inside the
translated module after entry — method note, not blindness: the BUILD
line proves the hook fires on indirect entries, and static zero-`jal`
forces any publisher entry through exactly that bridge path).
Stats triple identical with/without the hook —
**50985 module calls / 1254673 steps / 20000 services**, matching
slice-78 F20 and slice-82 20k legs; boundary idle 0x1604/0x100.

### Verdict D6: caller Unknown (stays open)

No direct static edge of any scanned kind (jal, pointer word,
lui-materialization, table membership) reaches the publisher — only a
`jalr`-indirect call or a runtime-computed pointer can invoke it
(method limit, same as slices 80–82). It never entered in 20k live
(Confirmed by direct watch with a firing control) and never
submitted through 5M (B+0x34=0 at 3M and byte-identically at 5M, one
waiter on sema 63 — Confirmed circumstantial). What it would publish
(the caller's two values → B+0x80/+0x84) is therefore never observed.

## (2) D4: DeleteSema-with-waiters — model vs external evidence

### The model today (Confirmed, `src/ee/kernel.cpp:943`)

`delete_sema` refuses when `wait_threads != 0` (v0=-1 to the DELETER)
and deletes otherwise. The game's delete wrapper 0x005783A0
(verbatim disassembly): resolve via 0x00578290 → -1 returns -1 once;
queue op 0x00577F80; then `jal DeleteSema; beq v0,-1 → retry`
— a delete refusal retries FOREVER (hang, not release: draft
candidate-F PARKED analysis stands). The game's wait wrapper
0x00578500 (verbatim): resolve → -1 returns -1 once; queue op; then
`jal WaitSema; beq v0,-1 → retry` — the game was written against a
kernel where WaitSema CAN return -1. The model's wait path never wakes
with -1 and has no delete-release wake.

### External evidence (graded)

- PS2Tek, EE BIOS 41h (reverse-engineering reference per project
  convention — NOT a Sony manual): "Deletes the semaphore, forcing a
  thread reschedule. Threads waiting on the semaphore will either be
  released or suspended, depending on their status." No -1-for-waiters
  refusal is documented. WaitSema 44h is documented as returning
  void — so the -1 the game's wait wrapper checks is the kernel's
  wait-release error value delivered on the delete path (PS2Tek-
  consistent cause), or a vanished-id race between resolve and wait
  (second consistent cause; the model already returns -1 for a
  missing id).
- ps2sdk (pinned master `ee/kernel/include/kernel.h`, fetched):
  prototypes only (`s32 DeleteSema(s32)`, `s32 WaitSema(s32)`);
  `ee_sema_t` field order matches the model's offsets
  (count/max/init/wait_threads/attr/option); `kernel.S` holds thin
  syscall wrappers (slice 67). Grade: ABI only — no delete-with-
  waiters semantics anywhere in ps2sdk (the implementation is Sony's
  BIOS).
- ps2autotests (`tests/kernel/ee/thread/`, current master listing):
  delete/exit/resume/sleep/stat/suspend/terminate/wakeup — thread
  lifecycle only. NO semaphore delete-with-waiters case exists.
  Absence recorded, not a claim.
- PCSX2: no HLE for the EE kernel semaphores (uses the dumped BIOS)
  — so the local ROM set (90001-v18 line, slice 79) IS the ground
  truth. Disassembling the BIOS 41h path from the dumped ROM is the
  named next discriminant (not done here).

### Named gap, no behavior change (scope kept)

(i) The model refuses with waiters where PS2Tek says the real BIOS
deletes and releases/suspends the waiters, forcing a reschedule.
(ii) The model lacks the -1 wait-release wake the game's own wait
wrapper is written to expect. H1 consequence, now precise: IF the
BIOS-ROM discriminant confirms the -1 wait-release, candidate F
(TEARDOWN-first) becomes the H1 first event under draft §2-F's
already-designed acceptance (waiter's WaitSema returns the error,
retry re-resolves the fresh handle, loop terminates); H2 is
unaffected. D4 verdict: NARROWED, not closed.

## (3) D2/D3 remainder — cheap items only

- D2: savestate inventory re-read — `private/pcsx2/sstates/` still
  holds only slot 1 (+backup) and slot 9, both menu-phase (slice 79):
  still NO intermediate anchor. The `.ini` blocker re-verified
  (`Bios = F:\Games\PS2\BIOS`, absent; `[Filenames] BIOS` already
  names the 90001 ROM0 — the fix is a one-line repoint, not done
  here). Precise staged-save shopping list for the future live leg
  (guest-observable milestones, no service-count mapping across
  kernels): break pcs 0x00101938 (init runs), 0x005BC4C8 (BUILD
  pass), 0x00548660 (gate parks), 0x00568B94 (menu); compare at each
  stage A=0x0064C3C8, B+0x00/+0x34/+0x3C/+0x80s, sibling C+0x00, and
  the generation table 0x00874550 for ids 13/14/15/30/63/71/75. No
  live session attempted (interactive boot still blocked ×4 —
  honest).
- D1: cheap closures — init's sole caller re-verified (0x00101938,
  whole-text scan §1); the A-slot single-store stands (slice 80,
  cited not re-run). The late-writer question still needs
  intermediate anchors or a live watchpoint with positive control
  (unchanged).
- D3: recipe stands (slice 79 §1 + ini fix + break-pc list above).

## Consequences for draft 0037 (no adoption, no implementation)

- D6 caller still Unknown: the rank-2 submitter stays gated behind
  an unknown caller — the unlock sequence is unchanged; the
  publisher now names B directly (E5 residual reworded, not
  reopened).
- D4 narrowed: candidate F's revival condition is now precise
  (BIOS-ROM -1 wait-release); the model gap is named in (ii) above.
- D2 shopped, not filled; D5 closed (82); D1/D3 unchanged.
- H1 vs H2 stays open (needs D2 anchors or the D4 ROM discriminant).

## Gates and hygiene

- Tree holds only this doc + the journal: `git status` clean
  (instrumentation reverted via `git checkout -- src/ee/driver.cpp`;
  hunt artifacts — run logs, scan script output — kept out of git;
  run logs deleted after extraction; scan script lives in the
  approved host temp dir).
- No payload bytes in docs/git: addresses, counts, handle values,
  relations, public-reference quotations.
- Full gates on the reverted tree (VsDevCmd `-arch=amd64` chained):
  configure+build green; **CTest 53/53**; **Python 73 collected — OK
  (skipped=6)**. Instrumented legs were neutrality-tripled (20k leg
  identical with/without the hook; triple matches slice-78 F20 and
  slice-82 20k).
- No commit, no push, no branches.

## Build-system note (load-bearing for future instrumented slices)

`src/ee/*.cpp` edits rebuild `gt4recomp_decode.lib` but do NOT
relink `gt4boot.exe` (the ninja link edge lists the libs as
order-only): after touching lib sources, build `--target gt4boot`
explicitly (deleting a stale exe alone does not even re-trigger the
default build). Caught live this slice: the first instrumented leg
ran on a stale binary (empty log, string absent from the exe) and was
rerun properly after the explicit target build — the rerun's BUILD
line + identical triple are the quoted evidence. Also: `ctest`
exercises the default build, which does not include `gt4boot.exe`;
the CTest `gt4boot_*` fixtures drive their own target builds.
