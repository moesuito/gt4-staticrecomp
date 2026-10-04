# Slice 78: who can release sema 63 (the main thread's wait) — the A=B knot (decision: none — observation only)

Date: 2026-10-04. Baseline: main at 2859cac (slice 77).
Task: with temporary bounded reverted instrumentation (the slice-76/77
reusable method: temporary trace + fresh legs + neutrality proof), find
(1) sema 63's exact state and every referrer, (2) the complete predicate
pinning thread 1, (3) a natural writer in reach, or — failing that — the
exact reason no producer can fire (no fabricated traffic/events, no
behavior change, no claim without a discriminant, no forensic
checkpoints).

## Run identity

- run_id: slice78-20261004 (legs F20 fresh 20k, R3 resume 3M+2000, R5
  resume 5M+2000, F5M fresh 5M; one process each).
- Model compatibility: time=3 interrupt=3 rpc=1 translation=2 (unchanged;
  quoted in every leg's provenance line).
- Inputs (verified this session, `gt4disc.py verify` against
  `docs/inputs/usa-v2.00.json`: PASS, SCUS-97328 / VER 2.00, ISO + all
  three file fingerprints match):
  ISO 5,314,478,080 bytes; `private/fingerprint-check/CORE.GT4`
  2,020,861 bytes; entry 0x00100008.
- Config: MSVC 19.44 x64 Debug, Ninja, CMake; service clock
  1 ms/service, idle 1 frame/interrupt, budgets 2M idle / 200M steps
  default. The 5M leg passes `--steps 400000000` (harness ceiling only,
  same as slice 76 leg O).
- Instrumentation (ALL reverted after extraction via
  `git checkout -- src/ee/kernel.cpp tools/gt4boot/main.cpp`; only docs
  survive): (a) `src/ee/kernel.cpp`: pre-op log of ONLY id-63
  create/wait/signal with pc, ra, a0, count, waiters, max, init, current
  thread, deferred count, pending count — gated by `GT4_S63_TRACE`,
  capped at 20,000 lines, file output only
  (`build/slice78-s63.log`, ignored); (b) `tools/gt4boot/main.cpp`:
  read-only `--threads` extension printing the sema table's waited
  entries + sema 63 and thread 1's saved ra/sp/stack. Hunt artifacts
  (logs, disassembly scratch, this session's ctest/python transcripts)
  under ignored `build/`, deleted after extraction except the quoted
  evidence below.
- Neutrality proof: R5 triple **4691 module calls / 108775 interpreted
  steps / 2000 services** with tracing ON = the exact slice-76 R1 /
  slice-77 triple on the uninstrumented tree; F20 (50985/1254673/20000)
  and R3 (4681/108349/2000) likewise match their uninstrumented
  counterparts. No checkpoint written (resumed slice 76's post-v3
  checkpoints only, allowed by gate 0028).

## (1) Sema 63's exact state (Confirmed, four stops)

Stop-time state, instrumented `--threads` dump plus the pre-op trace:

| Stop | count | max | init | waiters | attr | option | waiter |
|---|---|---|---|---|---|---|---|
| fresh 20k | 0 | 1 | 1 | 1 | 0 | 0 | thread 1 (2/63) |
| resume 3M+2000 | 0 | 1 | 1 | 1 | 0 | 0 | thread 1 (2/63) |
| resume 5M+2000 | 0 | 1 | 1 | 1 | 0 | 0 | thread 1 (2/63) |
| fresh 5M | 0 | 1 | 1 | 1 | 0 | 0 | thread 1 (2/63) |

Binary semaphore (max 1, init 1), currently empty, exactly one waiter
(thread 1) at every stop from 20k to 5M. Thread 1's saved block point is
identical at all four stops: pc 0x005adce8 (WaitSema stub return),
ra 0x00578540, sp 0x01fffcd0, stack 0x01ff8000 size 0x8000. The fresh-5M
leg ends mid-limit-cycle (boundary: syscall 0x005adcd4 = iSignalSema,
11,720,710 module calls, 271,457,654 interpreted steps, 5,000,000
services) with thread 3 on sema 768103 — the slice-77 standing cycle,
not a new state.

Whole-life trace of id 63 (the filter log over ALL legs — TEN lines
total, two identical five-line sequences, all cur=1 thread-context,
def=0 pend=0):

1. `create id=63 pc=0x005adca4 ra=0x00578340 a0=0x01ffff20 count=1 max=1 init=1`
2. `wait id=63 ra=0x00578540 a0=0x3f count=1` (immediate, 1→0)
3. `signal id=63 ra=0x005784e0 a0=0x3f count=0` (0→1, no waiter)
4. `wait id=63 ra=0x00578540 a0=0x3f count=1` (immediate, 1→0)
5. `wait id=63 ra=0x00578540 a0=0x3f count=0` (BLOCKS)

Attribution of the ten lines (Confirmed by construction): a resumed leg
restores post-create state and CANNOT emit `create id=63`; only two
fresh legs ran (F20, F5M). The log holds exactly TWO `create` lines, so
F20 contributed five lines, F5M five, and R3/R5 ZERO. Hence zero id-63
events in (20k, 3M], [3M, 3.002M] except none, (3.002M, 5M], and
(5M, 5.002M] — the last window by slice 77's independent full census
(no signal outside the pool one-shots and 119/127/131). Sema 63's
whole-life event set through 5.002M services is exactly the five init
events above. No poll/refer/delete on 63 anywhere.

## The handle-stamping library (Confirmed by read-only disassembly)

Sema ids go 3, 7, 11, … but the 0x00578xxx family never passes raw ids.
Three wrappers (all `gt4disasm`, CORE.GT4):

- create 0x005782e8: takes (init=a0, max=a1), zeroes an ee_sema_t on
  the stack, CreateSema with retry, then stamps a handle
  `id | (counter<<8)` with a per-id counter at **0x00874550 + id*4**
  (returns e.g. 0x13F = 63 | (1<<8) for sema 63). 13 static jal sites.
- resolve 0x00578290: handle → raw id (`id = handle & 0xff`,
  generation `handle>>8` checked against the same table; -1 on
  mismatch). 4 sites (the three wrappers + one).
- wait 0x00578500: resolve, WaitSema(0x005adce0) with retry-on-error
  (ra 0x00578540). 111 static jal sites.
- signal 0x00578480: resolve, then IE-aware SignalSema(0x005adcc0) in
  thread context vs iSignalSema(0x005adcd0) in handler context
  (ra 0x005784e0), retry-on-error. 31 static jal sites.

The delay-helper family (0x005AEDxx, slice 77) calls the stubs
DIRECTLY with raw ids — no resolver, no aliasing. Only wrapper clients
(111/31 sites) go through handles.

## Referrers of sema 63 (Confirmed: trace + dumps + disassembly)

- Creator: init function **0x00548500** — `create(1,1)` at
  0x00548518, stores the stamped handle 0x13F to the A-slot
  **0x0064C3C8** (`sw v0, 0x0(s0)`), then binds the PCDV RPC server
  twice (0x00578148 with a1="PCDV"=0x50434456 for objects 0x0086CB80
  and 0x0086CC80), waits on *(0x0086CC80), primes +0x40, calls
  0x00578168. (Twin site 0x005483a0 passes (0,0xff) = the sema-11
  shape (max 255/init 0) — NOT 63's.)
- Handle slots holding 0x13F at the stop (dumped): A=0x0064C3C8 AND
  B=0x0086CB80+0x00 (the job object's completion slot). The C-slot
  *(0x0086CC80) holds **0x147 = sema 71, generation 1** (NOT 0x13F —
  the discriminant of this slice): the second job object is a SIBLING
  on sema 71 (its +0x40 = 0x14b = sema 75's handle, same callback
  0x005780f8 and queue shape), outside the 63 set. So exactly TWO
  slots name sema 63: A and B.
- Waiters of the 63-handle: 0x005485e0's publisher wait
  (*(0x0086CB80)), 0x00548660's W_A (*(0x0064C3C8)) and W_B
  (*(0x0086CB80)), 0x00548750's pulse wait (on A). (0x00548550's
  C-wait reads *(0x0086CC80) = 0x147, i.e. sema 71 — corrected by the
  C-slot discriminant; it is not a 63 waiter.) Only THREE waits ever
  issued syscalls (trace lines 2/4/5).
- Signalers of the 63-handle in code: 0x005486ec (0x00548660's tail,
  on A), 0x00548778 (0x00548750's pulse, on A), 0x0057812c (async
  completion callback 0x005780f8, on obj+0x00), 0x005781fc
  (0x00578168's self-signal after a successful submit, on obj+0x00).
  Only ONE ever fired (trace line 3) — attributed below.

## (2) The complete predicate pinning thread 1 (Confirmed)

Call chain from thread 1's stop-time stack (dump 0x01fffcc0: ra slots
validated as post-jal returns — 0x005486b0←jal 0x00578500,
0x00548650←jal 0x00548660, resolver-ra 0x00578518 stale below sp):

- PCDV read path (0x00548cc8: builds a 2048-byte sector request,
  `jal 0x00548640`) → 0x00548640 → 0x00548648: `jal 0x00548660` →
  0x00548660's SECOND wait (ra 0x005486b0 = return of the
  `jal 0x00578500` at 0x005486a8) → WaitSema(63), count 0 → BLOCKED.

0x00548660's body (the predicate): `wait(A)` [passed, consumed the
last unit], `wait(B)` [starves — THE PARK], then (never reached)
`+0x3C=1`, publish four args to +0x80/+0x84/+0x88/+0x8c, starter
0x00576ad8, dispatch 0x00578168(obj,3,1), `signal(A)`, return.

Object state at the stop (dumped 0x0086CB80 at 3.002M AND at 5M,
byte-identical — no transient pass through 5M): +0x00=0x13F,
+0x34=0, +0x38=0x006897f8, +0x3C=0, +0x40=0x143, +0x80/+0x84/+0x88/+0x8c
all zero — the stores AFTER the gate never ran.

The knot: **A and B hold the SAME handle (0x13F = sema 63)** while the
sema was created init=1. Units ever: init(1) + the single re-supply(1)
= 2. Takes: pulse-wait, W_A, W_B-attempt = 3. The re-supply's unit is
consumed by W_A; W_B starves with count 0 / waiters 1.

Static writers of the gated fields: +0x3C is set only by 0x00548660
itself (parked before) and cleared+signaled only by 0x00548428 (zero
direct jal sites — indirect/worker-side, never observed); +0x80..+0x8c
only by the two publishers (0x005485e0 never ran — no 4th wait;
0x00548660 parked before). So no static writer can advance the
predicate from the stop state.

## (3) The single producer, attributed by elimination (Confirmed)

The trace's lone signal(63) (line 3: wrapper ra, 0→1, no waiter) is
the init-time **pulse 0x00548750 → 0x00548778** (wait A, signal A on
the 63-handle: it consumed the init unit and re-supplied it; W_A took
the re-supply, W_B starves). Elimination:

- The tail 0x005486ec CANNOT be line 3: it fires only after passing
  BOTH waits, but passing W_B needs a second unit that only line 3
  itself could supply — circular. (Only one signal exists in 5M
  services.)
- The callback 0x0057812c needs a submitted job with +0x34≠0: +0x34=0
  at 3.002M and byte-identically at 5M — never submitted.
- The self-signal 0x005781fc needs the submit call it follows; the
  only submitter in reach (0x00548660's dispatch) is parked before it.
- The C-path is out of the 63 set entirely (C-slot = 0x147, sema 71).

All four code paths that can signal the handle are dead from the stop
state: tail parked before, callback jobless, self-signal unsubmitted,
pulse already consumed (one-shot at init). The causal path of 63
(create/wait/signal wrappers + EE job queue + SDK queue 0x005B19B0
head: EE-side node build, no SIF/RPC in the visible prefix) contains
no unmodeled service — so the block is guest-side (A=B=63 with init
1), not a model gap. Naming a missing (SID,fn) pair would be
fabrication.

## Residuals and caveats (honest)

- Sema 71's own lifecycle (the pulse's sibling object, the C-wait's
  target) was NOT traced (filter was 63-only): Unknown, and not needed
  for the 63 verdict — the C-path cannot name 63 regardless of its
  state.
- Handle aliasing (mechanical, in-code): the resolver truncates ids to
  a byte before the generation check, so a wrapper-signal with
  handle&0xff==0x3F and generation==table[63](=1) would hit sema 63
  even when created for id 319/575/…. VOID in practice iff no
  wrapper-create fires late: in (5M,5.002M] all 76 creates carry the
  delay-helper ra (stub-direct, immune — slice 77). Discriminant armed:
  any future signal(63) names its ra in the filter log.
- B+0x00 copier (who copied 0x13F into 0x0086CB80+0x00): open
  micro-item; value confirmed by dump at two stops.
- 0x00548750's own invoker is indirect (no direct jal site observed):
  open micro-item; its single firing is proven by the trace, its caller
  is not needed for the verdict.

## What this means for P10

P10 opens only with the main thread's sema-63 wait resolving. It
cannot resolve from inside the observed machine: the wait needs a
second unit on sema 63, and every in-code signaler is dead from the
stop state (tail behind the gate, pulse consumed, callback/self-signal
behind a job submit that only the parked path performs — a guest-side
circular wait). The ranked breaker is originating async traffic (M32:
an IOP completion submitting the job object, letting the callback
0x005780f8 signal obj+0x00) or a later boot phase outside the 5M
window re-supplying the sema. No model change ships from this slice.

## Gates and hygiene

- Full CTest 53/53 + Python 73 (67 run, 6 skip) on the reverted tree;
  `gt4disc.py verify` PASS (SCUS-97328 / VER 2.00).
- Build note: the first rebuild used VsDevCmd without `-arch` (which
  defaults to x86) and `gt4boot_build` failed its link with x86/x64
  LNK4272 conflicts; re-running configure+build+ctest with
  `-arch=amd64` is green. One `gt4boot_resume_verify` failure in the
  first full run passed in isolation and the repeat full run is 53/53
  (parallelism/state flake, recorded honestly).
- Revert: `git checkout -- src/ee/kernel.cpp tools/gt4boot/main.cpp`;
  hunt artifacts under ignored `build/` deleted after extraction;
  `git status` holds only this doc + the journal. No commit, no push,
  no branches.
- Provenance: legs F20/R3/R5 and the draft ran in the prior session;
  F5M (fresh 5M), the revert, the rebuild, the gates and this final
  write ran in this session. A concurrent session appended a slice-78
  journal summary from the same machine state; its numbers (F5M triple,
  C=0x147, byte-identical dumps, full census) were cross-checked
  against the legs quoted here.
