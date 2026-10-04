# Slice 86: P10 traffic with F's mechanism banked — paper verdict, static remainder, no implementation

Date: 2026-10-04. Baseline: main at 8aac47b (slice 85, clean tree, no code
touched this slice). Task (slice 86): P10 design work with F's mechanism
banked — (1) on paper, accept the wake-all + retry-re-resolve chain as F's
first event per draft 0037 (promote that part of the DRAFT to a 0038
decision marked accepted ONLY if every falsifiable acceptance in the draft
is satisfiable without exception — otherwise keep DRAFT and say what is
missing); (2) the cheap static remainder of the invoker hunt (wider lui
windows, jalr census, consumer hunt of the late cluster — D6 stays open);
(3) ONLY with (1) closed and a discriminant in hand, implement the smallest
F traffic that releases the waiter with a legitimate producer in the model,
with the draft's acceptances armed. PROHIBITED and kept: every item of the
draft's 8 prohibitions; traffic without waiter-first; changing delete
semantics (D4 stays a named gap with no behavior change until invoker
evidence). No commit, no push, no branches.

Method: read-only disassembly of the pinned input
(`private/fingerprint-check/CORE.GT4` — only addresses, counts, handle
values and relations below) via `build/gt4disasm.exe`, plus a read-only
inflate+scan host Python script (zlib raw-deflate + record parse, same
layout as `src/executable/core_image.cpp`; text base 0x00100000,
1,334,917 words; data record base 0x00617A80, 779,132 bytes). Scan scripts
and disassembly dumps live only in the approved host temp dir, not in git.
No live legs (the remainder is static-only by scope; the stop-state endpoint
stands on the 5M legs of slices 78/82 and the gate suites below).
Confidence labels per project rule.

## (1) The paper chain — re-derived verbatim, with a new correction

### Wait wrapper 0x00578500 and delete wrapper 0x005783A0 (Confirmed, full bodies)

Both wrappers share one shape (register names differ only in the resolve slot):

- `jal resolve 0x00578290` once; resolved handle cached in s0 (wait) — a
  -1 resolve returns -1 immediately (one early exit).
- s1 = -1 held as the retry comparator.
- Loop: `jal queue-op 0x00577F80`, then the syscall (`jal WaitSema
  0x005ADCE0` / `jal DeleteSema 0x005ADCB0`, target handle in a0's delay
  slot), then `beq v0, s1 -> loop-top`.
- Fall-through returns v0 unchanged — whatever the service returned,
  including any non--1 wake value.

The load-bearing NEW fact: **neither retry re-invokes resolve**. The wait
retry returns to the queue-op at 0x00578530 (not to resolve at 0x00578510);
the delete retry returns to the queue-op at 0x005783D0 (not to resolve at
0x005783B0). Both retries re-issue the syscall with the SAME cached handle
s0. A fresh handle enters only through a NEW wrapper call made after BUILD
re-stamps the slots — re-drive from above, never retry from within.

Consequences (game-side, Confirmed):

- Under refusal semantics (our model, `src/ee/kernel.cpp:943` — re-read
  this slice, refusal intact, see §4) a delete against a parked waiter
  returns -1 forever: queue-op + DeleteSema on the same handle, i.e. the
  hang drafts 0037/80/83 describe. Unchanged.
- Under wake-all real semantics (slice 84, ROM worker 0x80004A40) the
  FIRST delete succeeds — waiters woken, id returned — so the retry loop
  never iterates at all. The "retry re-resolves the fresh handle" step of
  draft 0037 §unlock-1 and slice 85 §3 exists in NEITHER semantics: it is
  not the mechanism by which any fresh handle arrives.

### Gate 0x00548660 (Confirmed, full body re-read)

s1 = A-slot base (lui 0x65, addiu -0x3C38 = 0x0064C3C8), s0 = B
(lui 0x87, addiu -0x3480 = 0x0086CB80). `wait([s1])`, `wait([s0])` (W_B
parks on the second) — then, with NO check of either wait's return value
(the next instruction is `addiu v0,zero,1`): `+0x3C=1`, publish four args
(s3→+0x80, s5→+0x8c, s2→+0x84, then s4→+0x88 in the starter-jal delay
slot), starter 0x00576AD8,
dispatch(s0,3,1) with t0=0x40, signal([s1]) via 0x00578480, return.
A woken W_B — with ANY return value other than -1 — runs the whole gate
body unconditionally. (A -1 return would ALSO run it: the gate never
branches on v0. The -1 retry lives one level down, inside the wrapper.)

### Cited without re-derivation

Dispatcher 0x00548950 + BUILD/TEARDOWN wrappers (slice 80, dispatcher
owner-verified; slice 82: all 370 run targets template-checked BUILD
wrappers, TEARDOWN nowhere in the run; walker 0x005BC4C8 + one-shot guard
0x005BC588 + single feeder chain, D5 closed). Publisher body (slice 83
correction stands: hardcoded B, caller's a0/a1 published, dispatch(B,2,1)).

### Acceptance audit for F (draft 0037 §2-F + general + negative)

F's clause: "F (only if D4 + invoker close): waiter's WaitSema returns the
documented error code; retry re-resolves the fresh handle; retry loop
terminates (no infinite -1 loop)."

- **E1 — "retry re-resolves the fresh handle": CONTRADICTED (new, this
  slice).** Both wrappers retry to the queue-op with the cached handle
  (§1 bodies above). The clause as written is not satisfiable because the
  described step does not exist in the code. A rewrite would have to say:
  "a fresh handle enters only via a new wrapper call after BUILD
  re-stamps" — which is re-drive from above, i.e. a DIFFERENT chain from
  the one the draft accepts.
- **E2 — "WaitSema returns the documented error code": CONTESTED inside
  our own record.** Slice 84's committed journal corrects slice 83: the
  ROM wake delivers NO error (waiter's saved v0 intact, -2 from the park;
  no -1, none needed). Slice 85 §3 instead assumes "-1 release the wait
  wrapper is written to expect". Both cannot hold; adjudication needs a
  BIOS-side re-read (out of this slice's static-game-side scope). Note the
  game side needs nothing: per the gate body above, the waiter proceeds
  with or without an error value.
- **E3 — parenthetical "only if D4 + invoker close": UNMET (load-bearing).**
  D4's mechanism is banked (ROM wake-all, slice 84, owner-checked); the
  invoker is still missing — §2 repeats every scan wider than slices
  81/83 and all come back negative. With no invoker, no trigger function
  of the guest service sequence is specifiable, so the general acceptance
  (one-shot, pure function of the service sequence, populated-consumer
  gate, stop-time dumps, census CHANGED for the unblocker) is not even
  exercisable. An unexercised acceptance is not a satisfied one.
- Negative assertion: holds trivially (no traffic shipped, census
  untouched) — and stays armed as the bar for any future slice.

A/B/C unchanged: A (BUILD re-run) is spent — slice 82 proved its one-shot
trigger fired once and cannot re-fire in this boot, so candidate A as
FUTURE traffic is dead; B stays gated behind an observed +0x34=1 (never);
C stays double-blocked (parked pump, word 0).

**Verdict (1): DRAFT KEPT — no promotion.** Three independent exceptions
(E1 new and game-side-decisive; E2 contested-record; E3 load-bearing).
Decision 0038 is therefore written as a DRAFT record (not adopted, no
implementation): it banks the corrected chain and names exactly what
blocks acceptance. What is missing, in order: (i) a reachable invoker
(any edge: jal, pointer, true materialization, table membership, or a
live entry with positive control); (ii) staged anchors D2 or a live
delete-watchpoint with positive control (temporal order of
teardown-vs-build in the reference); (iii) BIOS-side adjudication of the
-1-vs-intact-v0 wake value, or acceptance reworded to the no-error
variant the game side already supports; (iv) only then, the in-model
delete change, still gated behind (i) per the task's explicit rule.

## (2) Static remainder — every scan wider, all negative, D6 stays open

Whole-text jal census (97,269 jal sites over 1,334,917 words):

| Target | jal sites | Note |
|---|---|---|
| publisher 0x005485E0 | 0 | unchanged (81/83) |
| TEARDOWN wrapper 0x00548A00 | 0 | unchanged (80/81/83) |
| BUILD wrapper 0x005489E0 | 0 | table-only (see pointers) |
| late gate 0x0060FF18 | 0 | |
| late waiter 0x0060FFD0 | 0 | |
| late pulse 0x00610000 | 0 | |
| late submitter 0x00610050 | 0 | |
| gate-worker 0x00548428 | 0 | |
| +0x40 waiter 0x00548718 | 0 | |
| callback 0x005780F8 | 0 | |
| pulse 0x00548750 | 0 | |
| gate 0x00548660 | 1 | tail 0x00548648 (81-E1/83) |
| dispatcher 0x00548950 | 2 | BUILD+0xC / TEARDOWN+0xC (81-E1) |
| init 0x00548500 | 1 | 0x00101938 (81-E1/83) |
| teardown-B/C | 1 each | dispatcher only (81-E1) |
| builders B/C | 1 each | dispatcher only (80) |
| bfunc 0x005487C0 | 1 | 0x004ACB70 (81-E1) |
| wait wrapper | 111 | matches 78/81 exactly |
| delete wrapper | 11 | matches 83 exactly |
| resolve 0x00578290 | 4 | matches 83 exactly |
| dispatch 0x00578168 | 79 | matches 81-E1 exactly |
| step/worker table bases | 0 | |

Full-word pointer scan (text AND file-backed data): BUILD wrapper occurs
exactly once (0x0061746C, its own table entry [27]); TEARDOWN wrapper
exactly once (0x0061799C, the data-table coincidence — corroborates
80/81-E3); teardown-B/C and gate-worker only inside the worker table
(0x00689804/0x006897E4/0x0068980C — corroborates 80/83); 0x00578288 ×9 in
data (table memberships); publisher, dispatcher, both table bases, and
all four late-clone addresses: zero anywhere else.

lui composition, WIDENED (slice 83 capped at same-register low-use within
8 words): every same- AND cross-register addiu/ori use within SIXTY-FOUR
words of every lui with high ∈ {0x54, 0x55, 0x60, 0x61} (55/34/22/92 sites
— 0x54/0x55 match 83, 0x61 matches 82's correction). Result: ZERO true
compositions of any hunted target (publisher, both wrappers, dispatcher,
tables, all four late clones). One candidate adjudicated FALSE by
reaching-def: lui s1,0x61 @0x00101E70 + addiu a0,s1,0x50 @0x00101EC8 would
compose 0x00610050, but s1 is redefined at 0x00101E7C
(addiu s1,s1,0x7d98 → 0x00617D98, a data pointer passed to 0x00577xxx
helpers) BEFORE that use — the stale-pair class slice 82 recorded
honestly for its five. Remaining limit (stated, cheap scope): exotic
arithmetic composition beyond lui+addiu/ori is not excluded; BSS/stack/
argument pointers are invisible to all these scans (81-E3 caveat stands).

jalr census: 6,198 jalr sites whole-text (+30,809 jr), spread over every
64 KiB page (top pages ≈150–257 each) — ubiquitous, as expected. Zones:
startup [0x00100000,0x00108000) 30; gate-neighborhood
[0x00548000,0x00549000) ZERO; lib-neighborhood [0x00578000,0x00579000) 4
(including 0x00578118 — the callback's documented indirect call through
+0x38, 81-E4); registry/archive [0x004AC000,0x004B4000) 86. No
discriminant: any invoker hides among thousands of indirect calls (same
method limit as 80–83, now quantified).

Object-base closure (first-use compositions; slice 80's window counts 9
B sites with the same neighborhood confinement — the one extra is its
0x00107F94 feeder, outside my first-use pairing): B-base (lui 0x87,
addiu -0x3480) 8 sites — 0x00548528/0x005485B0 (init area), 0x005485E0
(publisher itself), 0x00548668 (gate), 0x00548720 (+0x40-waiter area),
0x005487CC (bfunc area), 0x00548978/0x005489B8 (dispatcher area): all
audited neighborhoods, NONE in the late region (consistent with 80's 9,
same neighborhoods). A-base exactly the known 5 (init, gate, pulse, late
clones 0x0060FF50/0x00610010 — the dumps below re-confirm the late two).
C-base 7 sites, all audited. So no late-region code can statically name
B; the late clones read the A slot and take their object as a parameter.

Late-cluster bodies mapped (consumer-hunt context; zero edges of any
scanned kind to all four — hunt negative):

- 0x0060FF18 (gate twin, parameterized): s0 = a0 (NOT hardcoded B);
  waits [A-slot] then [s0]; +0x3C=1; publishes s2→+0x80, s4→+0x8c,
  s5→+0x84, then s3→+0x88 in the starter-jal delay slot, with
  (s2,s3,s4,s5) = (a2,a1,t0,a3) — a DIFFERENT arg mapping from the gate;
  starter, dispatch(s0,3,1)/t0=0x40, signal(A). A gate that serves
  whichever object its caller passes.
- 0x0060FFD0 (+0x40-waiter twin): s0 = a0; wait([s0+0x40]); return
  [s0+0xC0]<1. Twin of 0x00548718 with object param.
- 0x00610000 (pulse variant): s0 = A-slot; wait([s0]); s1 = [a0+0x3C]
  (the CALLER's object gate-word); signal([s0]); returns s1 — a pulse
  that reports a parameterized gate-word.
- 0x00610050 (large submitter, 0x50 frame): s0 = a0; wait([s0]); s2 =
  wait-return & -0x40 (consumes the masked return — the only clone that
  touches it); dispatch(s0,4,0)/a3=s2 (NON-submit like bfunc); then
  [s2]/[s2+4] threaded into the return. Role Unknown.

Tripwire re-reads (unchanged values, cited for the next slice):
gate-word [0x00616F24] = -1 with NULL before; table2[26..28] =
0x00548328/0x005489E0/0x0054F708 with (0,0xFFFFFFFF) terminator;
worker-table 0x00689804/0x0068980C = teardown-B/gate-worker.

**Verdict (2): D6 STAYS OPEN.** No direct static edge of any widened kind
reaches the publisher, TEARDOWN, or any late clone. The only invoker
shape left is jalr-indirect or a runtime pointer — the exact residual
slices 80–83 named, now with wider windows and a 6,198-site census behind
it. (Documentation defect noted for a future repair slice: the committed
slice-84 body ends mid-table with a literal truncation marker; its
mechanism claims survive via the committed journal entry, STATUS line,
and commit title, all of which this slice cites — the full 41h walk does
not. Nothing in this verdict depends on the truncated bytes: E3 alone
blocks promotion.)

## (3) No implementation (gated)

(1) did not close and no discriminant (invoker, staged anchor, live
watchpoint) is in hand, so per the task's own gate nothing ships.
Additionally the task explicitly keeps D4 a named gap without behavior
change until invoker evidence: `Kernel::delete_sema` refusal with
waiters (v0=-1 to the deleter; v0=0 on success) re-read intact at
`src/ee/kernel.cpp:943`–953. All eight draft prohibitions kept; no
waiter-first violation possible with zero traffic; the negative census
assertion stands untouched (no legs run beyond the gate suites).

## Gates and hygiene

- Tree holds only the two docs (this evidence + decision 0038), STATUS,
  and the journal. Scan scripts + disassembly dumps live only in the
  approved host temp dir, never in the repo.
- No payload bytes in docs/git: addresses, counts, handle values,
  relations, short instruction-shape quotations (established practice
  since slice 82).
- Full gates on the unmodified tree (VsDevCmd `-arch=amd64` chained):
  configure+build green; **CTest 53/53**; **Python 73 collected — OK
  (skipped=6)**.
- No commit, no push, no branches.
