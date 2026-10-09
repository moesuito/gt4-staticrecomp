# 0037 (DRAFT — not adopted, no implementation): the first unblocking traffic — waiter-first design

> **Superseded (2026-10-09, slice 89 / decision 0039):** the park was a
> model-side semaphore id collision, not a missing deliverable event. With
> the ids fixed the boot passes the gate with no synthesized traffic. This
> draft stays as history for the waiter-first method and its prohibitions.

Date: 2026-10-04. Status: **DRAFT**. Evidence:
`docs/reverse-engineering/slice81-traffic-design.md` (waiter inventory,
whole-text scans, latch instructions, submitter closure),
`docs/reverse-engineering/slice80-recreation-path.md` (BUILD entry #27,
three latches, H1 vs H2),
`docs/reverse-engineering/slice78-sema63-hunt.md` (the A=B knot, dead
signalers), decision 0026 (waiter-first method + tripwire), decision
0023 (async framing), PLAN P10 (first causal service). Parent method:
0026 specified a mechanism with a bounded effect and a negative
assertion; this draft does the same for the unblocking traffic —
except the honest outcome here is stricter (see verdict).

## Central verdict (waiter-first, like 0026)

The head waiter is thread 1's **W_B** on B+0x00 (sema 63). From the
stop state, **no single deliverable event releases it**: every
in-code signaler is dead (slice 78), re-stamping cannot release an
already-issued wait (the paradox), the gate-worker no-ops on
[B+0x3C]=0, and TEARDOWN is unreachable and would jam on
delete-with-waiters. Therefore the "first originating event" is NOT
specified as one packet that unblocks the boot. It is specified as:

(a) the **ordered unlock sequence** the reference provably traversed
(fresh ids + re-stamped slots + submitted job + live args at menu);
(b) **ranked traffic candidates**, each with its waiter, packet
shape, legitimate model producer, and bounded observable effect —
each a mechanism first, an unblocker only if the census says so;
(c) **falsifiable acceptance criteria** for the future
implementation slice; (d) **prohibited fabrications**; (e) the
**discriminant program** that must run before (or with) any traffic.

## (1) Ranked candidates

### A (rank 1): phase-step advance — the BUILD re-run's trigger chain

- **Waiter it must move first:** none directly — it re-stamps the
  slots (A→new, B→new) so that the H1 error-wake path (below) can
  re-resolve and re-wait on fresh semaphores.
- **Shape:** invocation of dispatcher 0x00548950 with (a0=1,
  a1=0xFFFF) — via the BUILD wrapper (table entry 0x00617400[27])
  or direct. Carries no bytes; it is a sequencer step.
- **Model producer:** NONE nameable yet — that is the point. Both
  table bases have zero static referrers (slice 81-E3); the walker
  reads its pointer from BSS/argument/stack. The design's first
  deliverable is the walker hunt (D5), not traffic.
- **Why rank 1:** the only route the reference provably took
  (menu: fresh low ids, re-stamped slots, job through the gate).
- **Observable if it ever runs:** gen-2 stamps (0x23F-class) in
  A/B, generation-table bumps, W_B still parked on old sema 63
  until the H1 wake; +0x34 stays 0 until the gate passes.

### B (rank 2): IOP completion for a submitted job (callback 0x005780F8)

- **Waiter:** the job object's +0x00 waiter (post-gate: W_B's
  successor on the fresh handle).
- **Shape:** the completion matching the submitted request (args
  from +0x80/+0x84/+0x88/+0x8c after the gate publishes them) —
  PRTS/PCDV-shaped, in the family of the decision-0021 answers.
- **Model producer:** the model IOP block-cache path (sid
  0x53545250, already modeled) — legitimate ONLY after a real
  guest submit (+0x34=1 observed in a dump). Never before: with
  +0x34=0 the callback runs and signals nothing.
- **Why rank 2:** it is the SECOND event by construction — gated
  behind a submit that only the parked gate (or the never-ran
  publisher) performs.
- **Observable:** callback runs, obj+0x00 signaled, the census
  CHANGES (the tripwire fires loudly — that is the signal, cf. 0026
  verification item 3 inverted).

### C (rank 3): gate-worker invocation with [B+0x3C]≠0

- **Waiter:** B+0x40's waiter (0x00548718) — deep downstream.
- **Shape:** a worker-table job dispatching 0x00548428 with our
  object after the gate has set +0x3C=1.
- **Model producer:** whichever pump walks 0x006897E0 — identity
  Unknown (D5); delivery reuses the 0026 DMAC-ch5 → handler
  machinery only if that pump proves to be the SIF pump.
- **Observable:** +0x3C cleared to 0, B+0x40 signaled, +0x40
  waiter released. Two independent blocks today (walker parked,
  word 0) — mechanism, not unblock, like 0026.

### D (REJECTED): thread-2 ring post

Posting `{op,arg}` + signaling sema 11 picks which thread wakes — a
game-side decision the model cannot know (0026 precedent); M32
slices 2–3 proved re-park/flicker; ring ops (wake/rotate/suspend)
do not reach phase steps. Fabrication.

### E (REJECTED): model-synthesized signal / second pulse on sema 63

The pulse is one-shot and spent (trace: exactly one signal in 5M;
body: wait-A/signal-A). No second firing exists in any leg. A
model-side `signal(63)` or an invented re-supply fabricates a game
decision. Rejected unless the reference shows a second firing (no
anchor today — D2).

### F (PARKED): TEARDOWN-first under current model semantics

Invoking 0x00548A00 while thread 1 waits cannot release it here:
`delete_sema` refuses with waiters and wrapper 0x005783A0 retries
while v0==-1 — an infinite retry, i.e. a hang, not a release.
Parked behind the delete-semantics reference experiment (D4) AND a
reachable invoker (none exists). If D4 shows real-BIOS delete wakes
the waiter with error, F becomes the H1 first event and this draft
gets revised — that revision is designed now (acceptance §2-F).

### G (PARKED): VBlank / delay-march revival as unblock

Sterile precedent (slices 20, 31, 50; thread-3 wake → deeper park).
Time unblocks nothing.

## The unlock sequence (order constraints the evidence forces)

1. (H1 leg:) TEARDOWN-error-wake of W_B → wrapper retries →
   re-resolves the FRESH handle (BUILD must have re-stamped first)
   → re-waits. (H2 leg: no rescue was ever needed — our boot
   diverged earlier; then the target is the divergence point
   (D2/D5), not traffic.)
2. W_B passes → gate runs: +0x3C=1, publish, starter,
   dispatch(B,3,1) submit, signal(A).
3. Completion arrives (candidate B) → callback signals B+0x00.
4. Gate-worker invoked with +0x3C≠0 (candidate C) → signal(B+0x40).
5. +0x40 waiter passes; phase sequencer advances.

Any implementation slice must respect this order and assert each
step separately — no single event may claim to jump the chain.

## (2) Falsifiable acceptance criteria (for the future implementation)

General (all traffic, from 0026's amendment generalized): one-shot
per boot; trigger a pure function of the guest service sequence
(both engines inject at the same boundary — differentials green by
construction); never spend the shot where no consumer can run
(populated-consumer gate: dispatch entry / queue / table slot must
be live, else hold); additive (queue/table slots empty today);
stop-time dumps assert the effect; census before/after asserted
EQUAL for mechanisms (A, C) and asserted CHANGED for unblockers
(B post-submit, F-if-revised) — a changed census on a mechanism
criterion FAILS the run (tripwire), a changed census on an
unblocker criterion is the awaited signal.

- A: gen-2 stamps + generation bumps observed; W_B still parked
  (H1) — if W_B is released instead, H2 evidence: stop and report.
- B (only after an observed +0x34=1): callback ran (trace/dump),
  obj+0x00 signaled, waiter released, next work produced
  naturally (P10's fourth item); regression test pins the pair.
- C: +0x3C 1→0 with +0x40 signaled and no other state change; if
  the worker no-ops (word still 0), the criterion fails — do not
  "fix" the word from the model.
- F (only if D4 + invoker close): waiter's WaitSema returns the
  documented error code; retry re-resolves the fresh handle;
  retry loop terminates (no infinite -1 loop).
- Negative: any leg whose census changes without its criterion
  predicting it FAILS — the 0026 tripwire pattern extended.

## (3) What NOT to do (prohibited fabrications)

1. No model-side signal/wakeup on sema 63/11 (game-side decisions).
2. No ring posts choosing threads (0026 + M32-3 precedent).
3. No second pulse/re-supply without a reference firing.
4. No TEARDOWN invocation under refusal semantics (hang, not
   release); no softening delete semantics without D4 evidence.
5. No model writes to guest handle slots (re-stamping is guest
   code; the model delivers traffic only, never stamps).
6. No VBlank/delay revival marketed as unblock (sterile).
7. No H1/H2 claim without discriminants (D1–D4).
8. No traffic while bypassing the census tripwire — update the
   negative assertion first, loudly, in the same slice.

## (4) Discriminant program (before/with any traffic)

- D1 (slice 79, refined by 81-E1): late writer of A/B slots —
  split init leg (sole caller 0x00101938, stamps A only) vs
  dispatcher leg (re-stamps B/C). Needs intermediate anchors or a
  live watchpoint with positive control.
- D2: staged savestates between the 5M-equivalent and menu (bracket
  the re-creation).
- D3: live-boot recipe (inventoried; BIOS 90001-v18).
- D4 (NEW): real-BIOS DeleteSema-with-waiters semantics — waiter
  woken with error, or delete refused? Decides F. Experiment on
  PCSX2 or pinned SDK/BIOS documentation; Unknown until then.
- D5 (NEW): table-walker/pump identity — BSS-pointer watch or live
  trace (static scans exhausted: zero referrers in text+file-data).
- D6 (NEW): publisher 0x005485E0's caller (zero jal; never ran).

## (5) Consequences and non-adoption

- Adopting nothing today: no code, no traffic, no behavior change.
  This draft converts the stall into a specified work order: hunt
  walkers (D5/D6/D1) and semantics (D4) BEFORE synthesizing
  anything — waiter-first to the end.
- What it unlocks: the next observation slice knows exactly which
  pointer to watch and which question each watch answers; the next
  implementation slice inherits trigger rules, per-step criteria,
  and eight prohibitions.
- Explicit non-goals: pad traffic (still no consumer), delay-model
  changes (sterile), ring posts (fabrication).
