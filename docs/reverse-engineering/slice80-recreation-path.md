# Slice 80: the semaphore re-creation path, hunted statically (decision: none — observation only)

Date: 2026-10-04. Baseline: main at 591f3e5 (slice 79, clean tree, no code
touched this slice). Task (slice 80): static hunt, in our binary, for the
path that re-creates semaphores — no live run, no behavior change. Context:
at menu phase the reference shows re-stamped slots (A→id 30, B→id 13, both
gen 1) with a job past the gate (B+0x34=1 and four live args); our boot
starves with A=B=0x13F (sema 63, gen 1). Scope, all static plus the current
boot's observed state: (1) who creates beyond init 0x00548500, who re-stamps
slots, who submits jobs (+0x34), who opens the gate; (2) the chain above it:
which function decides re-create vs wait, and what INPUT it waits for;
(3) whether our boot to 5M reaches that path; (4) the missing trigger named
precisely, or proof it is never produced. No commit, no push, no branches.

Method: whole-text scans over the pinned input
(`private/fingerprint-check/CORE.GT4`, never bytes in git — only addresses,
counts, handle values and relations below) via `build/gt4disasm.exe` plus a
read-only inflate+scan in host Python (zlib raw-deflate + record parse, same
layout as `src/executable/core_image.cpp`; text base 0x00100000, 5,339,668
bytes; entry 0x00100008). Every load-bearing count below comes from a full
1,334,917-word scan, not from a window. Confidence labels per project rule.

## (1) Creator inventory — the 13 create-sites (Confirmed by whole-text scan)

The create wrapper 0x005782e8 (`create(init=a0, max=a1)`, stamps
`id | (counter<<8)` with the per-id counter at 0x00874550+id*4) has exactly
13 static `jal` sites in the whole text:

| Site | (a0, a1) | Result stamped to | Role |
|---|---|---|---|
| 0x005034C8 | (0, 0xFF) | obj+0x2180 (other subsystem) | counting sema, unrelated |
| 0x005483A0 | (0, 0xFF) | B+0x40 (0x0086CB80+0x40) | twin: inner create of the B-builder 0x00548368 (same function, NOT a second builder) |
| 0x00548518 | (1, 1) | A-slot 0x0064C3C8 (`sw v0,0x0(s0)`) | OUR init 0x00548500 — the only stamper of A (see writer scan) |
| 0x0054A260 | (0, 0xFF) | near-A slot 0x0064C3F4 area | counting sema, neighbor slot, not A |
| 0x00551774 | (0, 0xFF) | other-subsystem slot | unrelated |
| 0x005552E8 | (0, 0xFF) | param object +0x4F8 | unrelated |
| 0x00557B34 | (0, 0xFF) | param object +0x288 | unrelated |
| 0x00557CF0 | (0, 0xFF) | param object +0x768 | unrelated |
| 0x00557D14 | (0, 0xFF) | param object +0x788 | unrelated |
| 0x00558620 | (1, 1) | slot 0x0064D25C | binary sema, different subsystem/slot |
| 0x0055EB74 | (0, 0xFF) | other-subsystem slot | unrelated |
| 0x00565728 | (1, 1) | param object fields | binary sema, CONDITIONAL: only when `[s0+0x60]==0` (guard at 0x00565700) |
| 0x00578068 | (1, 1) | job obj +0x00 (param) | the GENERIC factory 0x00578048 (see below) |

So only three (1,1) shapes exist anywhere near our protocol: our init (A),
an unrelated slot, a conditional param-object site — plus the generic job
factory. All other sites create (0,0xFF) counting semaphores.

The generic factory 0x00578048(obj): sets +0x38=0x00699E60, creates (1,1),
stamps +0x00, zeroes +0x2C/+0x34/+0x30. It has exactly 10 static callers:
0x004ED05C, 0x00547CEC, 0x00548374 (B-builder 0x00548368, a0=B passed
through), 0x00548464 (C-builder 0x00548458), 0x0054F224, 0x00550AEC,
0x0055117C, 0x005540F4, 0x00554264, 0x0055E3A4. Backward a0-write scan shows
each passes its own subsystem object (register passthrough, B/C set
explicitly by our dispatcher); none is near an A-slot computation, and no
caller passes the A slot (High confidence; the A-loader sites below are all
far from the 8 foreign call sites).

Per-object builders (PCDV subsystem, all Confirmed by disassembly):

- 0x00548368(B): factory(B) → +0x00; +0x38=0x006897F8; +0x30=0x40;
  +0x2C=B+0x80; +0x3C=0; create(0,0xFF) [the twin site] → +0x40.
- 0x00548458(C): factory(C) → +0x00; +0x30=0x40; +0x38=0x006897D8;
  +0x2C=C+0x40; return. NOTE: no inner (0,0xFF) create and no +0x40 store —
  C+0x40's stamper (0x14B at our stop) is an open micro-detail (candidates:
  the queue op 0x00579D30 or a pump-side write; NOT init, which stores the
  POINTER C+0x80 there before its dry-run submit).
- 0x005483C0(B,flag) / 0x00548498(C,flag): repoint +0x38 (=0x006897D8),
  mask flag&1, then 0x00578090 → resolve(+0x00) → DELETE wrapper 0x005783A0
  (resolve, retry DeleteSema 0x005ADCB0 while v0==-1). Flag even: delete and
  return (result unchecked). Flag odd: `j 0x005C1628` (fatal).
- 0x005482D0(a0,a1): the same build/teardown shape one level up for the
  NEIGHBOR subsystem (objects at 0x0086CA40): a1 must be 0xFFFF, a0=1 builds
  via 0x00547CE0, a0=0 tears down via 0x00547D20. Wrappers 0x00548328 (a0=1)
  / 0x00548348 (a0=0) sit 0x20 apart — the same (X, X+0x20) pairing as below.

Slot-writer closure (Confirmed: `addiu`-immediate scan over the whole text
plus an `ori`/`lw`/`sw`-offset scan):

- A-slot base (`lui 0x65` + `addiu -0x3C38`): exactly 5 sites — 0x0054851C
  (init; the ONLY store, `sw v0,0x0(s0)`), 0x00548698 (gate W_A, loads),
  0x00548760 (pulse, loads), 0x0060FF50 (late gate clone, loads), 0x00610010
  (late pulse clone, loads). No other computation of A exists.
- B base (`-0x3480` from 0x87): 9 sites, all in 0x00107F94 / 0x005485xx /
  0x005486xx / 0x005487xx / 0x005489xx (init binds, gate, waiters, teardown
  calls). C base (`-0x3380`): 8 sites, same neighborhoods.
- `ori`/`lw`/`sw` with low-16 C3C8/CB80/CC80: exactly 1 each in the whole
  text, at 0x002EA3BC/0x002F24A8/0x002F27B4 — all with base `lui 0x84`
  (targets 0x0083C3C8/0x0083CB80/0x0083CC80), NOT our slots. Ruled out.
- Gated fields: +0x34 is written ONLY by dispatch 0x00578168
  (`sw (a2&1), +0x34`); +0x3C is set ONLY by the gate 0x00548660 itself and
  cleared ONLY by worker 0x00548428; +0x80..+0x8C ONLY by the two publishers
  (0x005485E0 never ran — zero callers — and the parked gate).

Who submits (+0x34) and who opens the gate (Confirmed by disassembly):

- Dispatch 0x00578168(obj,a1,a2,t0): +0x34 = a2&1, queue-op 0x00579D30 in a
  retry loop, then re-reads +0x34 and, if nonzero, self-signals obj+0x00 via
  0x00578480. 79 static callers; ours pass a2=1 (submit) except init's C
  dry-run (a1=a2=0 → +0x34 stays 0, no signal).
- Completion callback 0x00578100(obj): indirect call through +0x38, then if
  +0x34≠0 signals obj+0x00. Needs a submitted job + arriving completion.
- Worker 0x00548428(obj): `lw v0,+0x3C; if zero → return; else +0x3C=0 and
  signal(+0x40)`. Zero direct callers; its address is stored in the worker
  table at data 0x0068980C (see below). For B the check reads [0x0086CBBC].
- +0x40 waiter 0x00548718(obj): waits +0x40, returns `[+0xC0]<1`.

## (2) The decision chain — BUILD vs TEARDOWN, and the INPUT each waits for

Dispatcher 0x00548950(a0, a1) — the ONLY function that invokes the
B/C builders (Confirmed: sole caller of each of 0x00548368, 0x00548458,
0x005483C0, 0x00548498):

- `bne a1, 0xFFFF → do nothing` (both wrappers pass 0xFFFF, always taken).
- a0=1 (wrapper 0x005489E0): BUILD — factory-creates B/C +0x00 semaphores
  (FRESH kernel ids by construction) plus B's +0x40, re-stamps the slots.
  THIS is the static re-creation path: re-running it manufactures new ids.
- a0=0 (wrapper 0x00548A00): TEARDOWN — DELETES B/C +0x00 semaphores via the
  delete wrapper (flag 2 → even → delete+return; an odd flag would jump to
  the fatal 0x005C1628). So the a0=0 phase is not "submit" but teardown.

Corrections to mid-slice hypotheses (recorded honestly): the 0x617900 array
holding 0x00548A00 is NOT a phase-2 table — it sits inside the trailing data
region (0x616F28..0x617A14), its neighbors are not code addresses, and no
consumer references it (the `lui 0x61` + offset-0x7400/0x7900 walker search
found only the handler-registry 0x7Axx references). Its 0x00548A00 word is a
data-table coincidence (a nearby (X, X+0x20) alignment with the genuine table
is structural noise from the subsystem's consecutive layout, 0/60 index match
when aligned). Likewise 0x00548648 is a fall-through label inside 0x00548640,
not a separately-called function (no verdict change). The late cluster
0x0060FF18 (gate clone, obj param) / 0x0060FFD0 (+0x40 waiter clone) /
0x00610000 (pulse clone, obj param) / 0x00610050 (4-arg submitter clone) has
ZERO direct callers and no stored-pointer occurrence anywhere: role Unknown
(possibly a later-phase or dead parameterized twin); NOT the recreation path
by any reachable edge found.

What survives as genuine phase sequencing: BUILD wrapper 0x005489E0 IS entry
#27 of a real 60-entry boot-step table at text 0x00617400 (all entries valid
code addresses, terminated by 0 + 0xFFFFFFFF), between the neighbor
subsystem's build step (0x00548328) and downstream steps. TEARDOWN wrapper
0x00548A00 has no reachable invoker found: zero direct callers, no genuine
table entry, no stored pointer. The OTHER invoker-shaped object is the worker
table at data 0x006897E0..0x00689830 (entries {handler, 0}: 0x00578288,
0x0060FDF0, 0x005483C0 @0x689804, 0x00548428 @0x68980C, 0x00610240,
0x0054DE50…): whichever pump walks it can invoke teardown-B (0x005483C0) and
the gate-worker (0x00548428) with a job object — but that pump is the
stable-parked one (M32: provably empty pump queue, thread-2 ring all {0,3},
six workers one-shot-then-parked).

The INPUTS, named precisely (Confirmed):

- BUILD/TEARDOWN choice: registers a0 (1 vs 0) and a1 (must be 0xFFFF) of
  0x00548950 — i.e. WHICH phase-table entry the sequencer runs. It waits for
  no guest memory value; the trigger is boot-phase movement.
- Gate-worker 0x00548428: the word at [a0+0x3C] — for B, address 0x0086CBBC —
  must be NONZERO (else immediate no-op return). Observed 0 at every stop
  (3.002M and 5M, byte-identical dumps): even if invoked, it would no-op.
- Gate 0x00548660: kernel sema units behind B+0x00 (no guest address).
- Teardown delete: unchecked on the even-flag path; the delete wrapper
  retries while the service returns -1.

## (3) Our boot to 5M: the path is NOT reached (Confirmed, three locks, no new leg needed)

- No re-CREATE: slots still hold gen-1 stamps (A=B=0x13F, C=0x147) and the
  parked W_B wait resolved its handle (thread 1 sits INSIDE WaitSema on
  kernel id 63) — so the generation table still reads 1 for id 63. The
  factory bumps the counter on every create; a second create would stamp
  0x23F (or break the parked resolve). Slice 78's whole-life id-63 trace
  (exactly the five init events, nothing in (20k,5.002M]) corroborates.
- No invoker can reach TEARDOWN/BUILD-re-run: zero direct callers, no genuine
  table entry for the teardown wrapper, worker table never walked (parked
  pump), phase tables never re-walked (5M stop is the standing limit cycle,
  thread 3 on sema 768103 — slice 77).
- Even if reached, TEARDOWN could not complete in OUR model while thread 1
  waits: `delete_sema` (`src/ee/kernel.cpp:943`) refuses when
  `wait_threads != 0` (writes error 0xFFFFFFFF), and the delete wrapper
  0x005783A0 retries while v0==-1 — an infinite retry, not a release. (Real
  BIOS waiter semantics on delete: Unknown — reference-side, see H1/H2.)

New narrowing evidence (creation-order arithmetic, High confidence): B+0x40
= 0x143 (id 67) is the twin (0,0xFF) create = the 17th HLE sema; A's create
= id 63 = the 16th; ids admit NO create between them (HLE step is 4), yet the
B-builder 0x00548368 performs its factory create BEFORE its twin create in
program order. Therefore B+0x00 = 0x13F (A's handle) is a COPY written after
the B-builder ran — slice 78's open "B+0x00 copier" micro-item survives,
narrowed to post-builder. (The copier itself remains unidentified; no claim
beyond timing/value.)

## (4) The missing trigger (ranked; no fabrication)

- T1 (ranked first): originating async traffic that advances the worker
  threads to the phase steps — the pump queue is provably empty (M32 slice
  1), thread-2's ring is stable-parked (slices 2–3), the six workers are
  one-shot-parked (slice 4). Nothing in the model produces the arrival the
  worker table (0x00689800 region) or the phase sequencer waits for. This is
  the same drought slices 48–50 mapped (async IOP first, then input, then
  GS-side): slice 80 adds the exact consumer addresses that stay hungry.
- T2: a job arrival dispatching 0x005483C0/0x00548428 with our objects AND
  [B+0x3C]≠0 (observed 0 — the gate never passed, so the worker would no-op
  anyway; two independent blocks).
- T3: invocation of the late parameterized cluster (no edge found; Unknown
  whether live code at all).

The parked-waiter paradox (how the reference unparked W_B — re-stamping alone
cannot release an issued wait) stays Unknown with two hypotheses: H1 — the
reference tore down (real-BIOS delete woke the waiter with error) and the
wrapper's retry re-resolved the fresh handle (needs real-BIOS delete
semantics + a re-run of BUILD); H2 — the reference never parked (different
interleaving: the pulse re-supply landed before W_B, so no rescue was ever
needed and teardown+rebuild is routine lifecycle). Discriminants armed (need
intermediate anchors or a live watchpoint with positive control per slice-79
D1–D3; static evidence cannot close H1 vs H2 — and raw sema-id ordinals stay
non-comparable across kernels per slice 79).

What this means for P10: no model change ships from this slice (prohibited
and unwarranted). The recreation path is now statically mapped end to end
(BUILD re-run ⇒ fresh ids ⇒ re-stamped B/C; TEARDOWN ⇒ delete; gate-worker
⇒ [+0x3C] check at 0x0086CBBC). P10 still opens only with originating traffic
(M32): something must walk the worker table or re-drive the phase steps, and
the [+0x3C] word plus the gen-1 stamps are the tripwires that will prove it
(the next slice re-reads 0x0086CBBC, B+0x00 and the generation table after
any traffic/model change — any 0x23F-class stamp or nonzero +0x3C falsifies
today's endpoint).

## Gates and hygiene

- Static only: disassembly + whole-text scans of the pinned input; no source,
  config, or test change; `git status` holds only this doc + the journal.
- No payload bytes in docs/git: addresses, counts, handle values, relations.
- Full gates on the unmodified tree (VsDevCmd `-arch=amd64` chained):
  configure+build green; **CTest 53/53** (95.43 s); **Python 73 collected — OK
  (skipped=6)** (exit 0; an earlier exit=1 was a PowerShell pipe artifact with
  the register-dump stdout, cleared by redirecting to a file).
- No commit, no push, no branches.
