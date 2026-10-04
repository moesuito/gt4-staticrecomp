# 0038 (DRAFT — not adopted, no implementation): F's first-event chain, corrected and banked (paper only)

Date: 2026-10-04. Status: **DRAFT**. Evidence:
`docs/reverse-engineering/slice86-p10-traffic.md` (verbatim wrapper/gate
bodies, widened invoker scans, acceptance audit), draft 0037 (the chain
under review), slice 84's committed record (ROM wake-all mechanism;
note: the slice-84 body in git ends mid-table with a truncation marker —
its mechanism claims are cited from the committed journal entry, STATUS
line, and commit title). Parent method: 0026 (mechanism with bounded
effect + negative assertion); this record banks F's mechanism on paper
with the same discipline — except acceptance is WITHHELD (see verdict).

## Verdict: not accepted — DRAFT kept, three exceptions

The wake-all + retry-re-resolve chain is NOT accepted as F's first event.
Draft 0037 stays DRAFT. Promotion to accepted requires ALL of the
following to be resolved without exception; none is:

1. **E1 (game-side, slice 86): the retry does not re-resolve.** Both the
   wait wrapper 0x00578500 and the delete wrapper 0x005783A0 resolve once
   (0x00578290 → cached s0) and retry to the QUEUE-OP (0x00578530 /
   0x005783D0) with the same handle while v0==-1. Under real wake-all
   semantics the first delete succeeds, so no retry runs at all. A fresh
   handle enters only via a NEW wrapper call after BUILD re-stamps —
   re-drive from above, a different chain from the draft's step 1.
2. **E2 (record-contested): the error-code clause.** Draft §2-F demands
   "WaitSema returns the documented error code"; slice 84's correction
   says no error is delivered (saved v0 intact, -2) and none is needed
   (the gate never checks — slice 86 re-verified); slice 85 §3 assumed
   -1. Adjudication needs a BIOS-side re-read, or the acceptance reworded
   to the no-error variant.
3. **E3 (load-bearing): no invoker.** Zero edges of every widened kind
   (jal, pointer, lui-composition to 64 words, table membership) reach the
   publisher, TEARDOWN, or any late clone; 6,198 jalr sites quantified as
   the hiding space. Without an invoker no trigger is specifiable and the
   general acceptance (one-shot pure-function trigger, populated-consumer
   gate, census CHANGED + dumps) is unexercisable.

## What is banked (paper, Confirmed unless noted)

- ROM delete wakes ALL waiters, frees LIFO, returns the id (slice 84,
  owner-checked; cited from the committed record per the note above).
- Wait wrapper returns any non--1 wake value to its caller; the gate runs
  its whole body (+0x3C=1, publish four, starter, submit, signal(A))
  unconditionally (slice 86 verbatim).
- BUILD re-run is spent (one-shot guard fired, slice 82): candidate A as
  future traffic is dead; the reference's fresh ids need a route this
  boot never takes.
- The invoker hunt is exhausted through 64-word lui windows + full
  pointer/jal/jalr census (slice 86): D6 stays open (publisher caller
  Unknown; TEARDOWN unreachable; late cluster role Unknown).

## What acceptance needs, in order

(i) A reachable invoker (any edge, or a live entry with positive
control). (ii) Staged anchors D2 or a live delete-watchpoint with
positive control (reference teardown-vs-build order). (iii) E2
adjudicated or the acceptance reworded. (iv) ONLY then, the in-model
delete change — still gated behind (i): D4 stays a named gap with no
behavior change (`src/ee/kernel.cpp:943` refusal intact).

## Consequences and non-adoption

- Adopting nothing today: no code, no traffic, no behavior change. The
  model still refuses delete-with-waiters; TEARDOWN under it still hangs
  (retry to queue-op, -1 forever) — the draft's PARKED analysis stands.
- What it unlocks: the next slice inherits the corrected chain wording,
  the exact missing pieces above, and the re-armed tripwires (0x0086CBBC,
  B+0x00, generation table).
- Explicit non-goals (unchanged): pad traffic, delay-model changes, ring
  posts, second-pulse re-supply, model-side signals, guest-slot writes.
