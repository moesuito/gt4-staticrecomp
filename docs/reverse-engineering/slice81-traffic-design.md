# Slice 81: M32 originating-traffic design — waiter-first specification (decision: DRAFT 0037, no implementation)

Date: 2026-10-04. Baseline: main at 41f795d (slice 80, clean tree, no code
touched this slice). Task (slice 81): design the M32 originating traffic
(spec-first, waiter-first like 0026) — what the first event must carry,
ranked candidates with evidence, falsifiable acceptance criteria for a
future implementation slice, and the prohibited fabrications. PROHIBITED
and kept: no new traffic injected, no behavior change, no conclusion
without a discriminant. Context: P10 needs traffic that advances the
workers to the phase steps; the recreation path (BUILD entry #27, gate
[B+0x3C]) waits on its trigger; three latches hold; TEARDOWN is
unreachable; the reference's old waiter unparked by an unknown route
(H1 vs H2, discriminants armed in slice 80).

Method: waiter-first inventory of the stop state (slices 76–78) crossed
with fresh whole-text scans over the pinned input
(`private/fingerprint-check/CORE.GT4`, never bytes in git — only
addresses, counts, handle values and relations below) via
`build/gt4disasm.exe` plus a read-only inflate+scan in host Python
(same record layout as `src/executable/core_image.cpp`: auth blocks,
count, entry 0x00100008, three records; text base 0x00100000,
1,334,917 words; data record base 0x00617A80, 779,132 bytes).
Every load-bearing count below comes from a full-word scan, not a
window. Confidence labels per project rule. The scan script lives only
in the approved host temp dir (not in git); its output transcript was
kept out of the tree with it.

## (1) Waiter-first inventory — the dependency chain from the stop state

Ordered head-first; each waiter's predicate, observed value, and what
could move it (Confirmed from slices 76–78 unless noted):

1. **W_B (thread 1, the head).** Gate 0x00548660's second wait on
   B+0x00 (handle 0x13F = sema 63, gen 1). Needs one unit on sema 63
   from a legitimate signaler. All four in-code signalers are dead
   from the stop state (tail parked behind the gate, pulse consumed,
   callback/self-signal behind an unsubmitted job — slice 78).
2. **Gate internals.** Run only after W_B passes: `+0x3C=1` at
   [0x0086CBBC], publish four args to +0x80/+0x84/+0x88/+0x8c,
   starter 0x00576ad8, dispatch(B,3,1) submit, signal(A). Observed:
   +0x3C=0, args zero (never ran). Verbatim body quoted in §3-E4.
3. **Completion callback 0x005780f8 → signal(B+0x00).** Needs a
   submitted job (B+0x34≠0) plus an arriving completion. Observed:
   +0x34=0 at 3M and byte-identically at 5M (never submitted).
4. **Gate-worker 0x00548428 → signal(B+0x40).** Needs [B+0x3C]≠0 AND
   an invocation through the worker table. Observed: word 0 (would
   no-op even if invoked); zero direct callers.
5. **+0x40 waiter 0x00548718.** Needs B+0x40 (0x143 = sema 67)
   signaled — only the gate-worker signals it. Zero direct callers.
6. **BUILD re-run (re-stamp B/C + fresh ids).** Needs dispatcher
   0x00548950 invoked with (a0=1, a1=0xFFFF). Re-stamping alone
   cannot release the already-issued W_B wait (the paradox — H1/H2).
7. **TEARDOWN (delete B/C +0x00).** No reachable invoker; and under
   current model semantics it could not complete while thread 1
   waits (delete refuses with waiters; wrapper retries forever).

## (2) Fresh static evidence (all Confirmed by whole-text scan)

### E1 — call-site closure (jal-word counts over all 1,334,917 words)

| Target | jal sites | Note |
|---|---|---|
| pulse 0x00548750 | 0 | indirect-only; single firing proven by trace (slice 78) |
| dispatch 0x00578168 | 79 | full list below; ours: 0x00548570 (dry-run), 0x0054861c, 0x005486e4 (gate), 0x00548810 |
| TEARDOWN wrapper 0x00548A00 | 0 | unreachable corroborated |
| BUILD wrapper 0x005489E0 | 0 | table-only corroborated |
| gate-worker 0x00548428 | 0 | indirect-only corroborated |
| callback 0x005780f8 | 0 | runtime-registered (no static edge) |
| gate 0x00548660 | 1 | caller 0x00548648 = the tail call inside 0x00548640 (fall-through label, cf. slice 80) |
| publisher 0x005485E0 | 0 | NEW: indirect-only, never ran — its caller joins the open list |
| +0x40 waiter 0x00548718 | 0 | NEW: indirect-only |
| B-function 0x005487C0 | 1 | NEW: sole caller 0x004ACB70 (handler-registry neighborhood) |
| dispatcher 0x00548950 | 2 | callers 0x005489EC (BUILD+0xC) and 0x00548A0C (TEARDOWN+0xC) — wrappers own the dispatcher |
| init 0x00548500 | 1 | NEW: sole caller 0x00101938 (early boot) — re-creation did NOT come through init |
| teardown-B 0x005483C0 | 1 | caller 0x005489C0 (inside the dispatcher) — sole-caller claim confirmed |
| wait wrapper 0x00578500 | 111 | matches slice 78 exactly (method cross-check) |
| signal wrapper 0x00578480 | 31 | matches slice 78 exactly |
| queue op 0x00577F80 | 67 | — |
| starter 0x00576AD8 | 19 | — |

The 79 dispatch callers (verbatim, for the record): 0x004ED1D8,
0x004ED37C, 0x004ED464, 0x004ED548, 0x004ED94C, 0x004EDA34, 0x004EDB20,
0x004EDC04, 0x004EDCB8, 0x004EDD3C, 0x00547EC4, 0x00547FE8, 0x00548040,
0x005480D0, 0x0054815C, 0x005481B0, 0x00548570, 0x0054861C, 0x005486E4,
0x00548810, 0x0054F3F8, 0x0054F490, 0x0054F624, 0x00550C20, 0x00550CC8,
0x00550E38, 0x00550EC8, 0x00550F54, 0x00551050, 0x005510B4, 0x00551294,
0x005512F8, 0x00551374, 0x005513DC, 0x00551444, 0x005514AC, 0x005543E8,
0x00554450, 0x005544C0, 0x0055459C, 0x00554600, 0x00554648, 0x0055E4A8,
0x0055E804, 0x0055E860, 0x0055E8C8, 0x0055E928, 0x00578268, 0x0060FBDC,
0x0060FC3C, 0x0060FC8C, 0x0060FCDC, 0x0060FD2C, 0x0060FD7C, 0x0060FE78,
0x0060FEF8, 0x0060FF98, 0x0061009C, 0x00610160, 0x0061036C, 0x006103E4,
0x00610448, 0x00610504, 0x0061058C, 0x006105EC, 0x00610670, 0x00610764,
0x006107C0, 0x00610880, 0x006108DC, 0x00610950, 0x006109B8, 0x00610A18,
0x00610A78, 0x00612050, 0x006120A4, 0x006121E8, 0x00612254, 0x006122AC.

### E2 — boot-step table re-verified (tripwire re-read)

Words at 0x00617400: 60 code-address entries [0..59] then terminator
(0, 0xFFFFFFFF) at [60,61]. [26] = 0x00548328 (neighbor build),
[27] = 0x005489E0 (BUILD) — "entry #27" is 0-based index 27 (the
28th entry). Neighbors [25] = 0x00547CA0, [28] = 0x0054F708.

### E3 — zero static referrers to either table or the wrappers

- `lui *,0x61`: **0 sites** in the whole text. The lone
  `addiu *,*,0x7400` (at 0x002CA668) builds an unrelated 0x002Cxxxx
  address triple (a3/t0/t1 = 0x002C7400/0x002C7428/0x002C7450) for
  `jal 0x00325010` — a coincidence, NOT a table reference.
- Pointer-constant scan (text AND file-backed data) for 0x00617400
  (step-table base), 0x006897E0/0x00689800 (worker-table base),
  0x005780F8 (callback), 0x00578168 (dispatch), 0x00548660 (gate):
  **0 hits each, everywhere.**
- 0x005489E0 occurs exactly once: at 0x0061746C — the table's own
  entry [27] (self-consistent, not a referrer).
- 0x00548A00 occurs exactly once: at 0x0061799C — inside the
  trailing data region, the dismissed 0x617900-array coincidence
  (corroborates slice 80; NOT a table entry, NOT a caller).
- Worker-table entries confirmed in file-backed data: 0x005483C0 at
  0x00689804, 0x00548428 at 0x0068980C (exactly slice 80's map).

Consequence (Confirmed with a stated caveat): **no DIRECT static
edge — jal, lui-materialized address, or pointer constant in text or
file-backed data — reaches the boot-step table, the worker-table
base, the BUILD/TEARDOWN wrappers, the pulse, the publisher, the
gate-worker, the callback, or the +0x40 waiter.** Any invoker uses
an indirect call or a runtime pointer. Caveat: BSS-resident globals
(0x008xxxxx objects are zero-init, not file-backed) and
stack/argument-passed pointers are invisible to these scans — the
claim covers text + file-backed data only.

### E4 — latch instructions quoted verbatim (`gt4disasm`, read-only)

- Gate 0x00548660: s1 = 0x0064C3C8 (A), s0 = B (0x87−0x3480);
  `wait(A)` [0x005486A0], `wait(B)` [0x005486A8] (W_B parks on the
  second); then `+0x3C=1` [0x005486B4], publish
  +0x80/+0x84/+0x88/+0x8c, starter 0x00576AD8 [0x005486CC],
  dispatch(obj,3,1) with t0=0x40 [0x005486DC–0x005486E4],
  signal(A) [0x005486EC].
- Worker 0x00548428: `lw v0,+0x3C; beql v0,zero → return` (the
  no-op on the observed 0); else `+0x3C=0`, signal(+0x40).
- Dispatch 0x00578168: `+0x34 = a2&1` [0x005781A4], then the queue
  op 0x00577F80 path.
- Callback 0x005780F8/0x00578100: indirect call through +0x38
  [0x00578108–0x00578118], then `if (+0x34≠0) signal(obj+0x00)`
  [0x00578120–0x0057812C].
- Pulse 0x00548750: wait(A) [0x00548768], signal(A) [0x00548778] —
  the one-shot re-supply shape, spent at init.
- Publisher 0x005485E0 (never ran): waits param-obj+0x00
  [0x005485FC], publishes a0/a1 → +0x80/+0x84, dispatch(obj,2,1)
  [0x0054861C] — a genuine second submitter of B-shaped objects,
  with its caller unknown (E1: zero jal).
- B-function 0x005487C0 (sole caller 0x004ACB70): s0 = B
  [0x005487DC], waits *(B) [0x005487F8], dispatch(obj,4,0)
  [0x00548810] — a2=0, audited NON-submit. Runtime reachability:
  Unknown (a silent a2=0 dispatch emits no sema event, so the
  slice-78 trace can neither confirm nor deny it ran).
- Init dry-run at 0x00548570: a1=a2=a3=t0=0 — not a submit.

### E5 — submitter inventory for object B (complete for the stop state)

Paths that set B+0x34=1: (i) the gate's dispatch(B,3,1) — parked
behind W_B; (ii) the publisher's dispatch(param,2,1) — never ran,
caller unknown, obj is a parameter (B only if the caller passes B).
Non-submits audited: init dry-run (0,0), bfunc (4,0). The remaining
74 dispatch sites sit outside the A/B neighborhoods, and B-base
computations exist only in the audited neighborhoods (slice 80) —
so no unaudited site can statically name B (the param-obj residual
above excepted). Submitter set: CLOSED subject to that residual.

## (3) Delta vs slices 78/80 (what this slice added)

1. Init exonerated as the re-creation route (sole caller
   0x00101938; init stamps A only) — D1 splits into init vs
   dispatcher legs.
2. Publisher + +0x40 waiter + gate-worker + callback + pulse: all
   indirect-only (0 jal) — the runtime-pointer shape is now the
   rule, not the exception, around the PCDV gate.
3. Both table bases have zero static referrers — the phase
   sequencer and the worker-table pump are statically invisible;
   their triggers can only be named by live observation (draft D5).
4. B-function 0x005487C0 mapped (B-wait + non-submit, caller in
   registry code) — audited out as a submitter; reachability Unknown.
5. Table entry numbering pinned (0-based 27 = 28th) and terminator
   re-read; the 0x00548A00 coincidence re-found at exactly
   0x0061799C.

## (4) Open micro-items (carried + new)

Carried: B+0x00 copier (slice 80; narrowed to post-builder),
pulse invoker, H1 vs H2. New: publisher's caller; table walkers'
identity (E3 caveat); bfunc runtime reachability. None blocks the
draft — each is assigned a discriminant there (D1–D6).

## Gates and hygiene

- Static only: disassembly + whole-text scans of the pinned input;
  no source, config, or test change; `git status` holds only this
  doc, the draft decision, and the journal.
- No payload bytes in docs/git: addresses, counts, handle values,
  relations.
- Full gates on the unmodified tree (VsDevCmd `-arch=amd64`
  chained): configure+build green (ninja: no work to do);
  **CTest 53/53** (93.58 s); **Python 73 collected — OK
  (skipped=6)** (62.07 s).
- No commit, no push, no branches.
