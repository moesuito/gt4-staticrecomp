# 0041 — Qualify the reference before changing branch/event semantics

Date: 2026-10-09, slice 99. Status: accepted for evidence discipline;
production event/clock contract unchanged.

## Context

Completed work agrees on hand-counted and boot paths, but does not prove
interrupt eligibility after every word. The model's ordinary not-taken
branch leaves pending-transfer clear; unconditional exclusion initially
looked plausible. Independent PCSX2 v2.9.114 source review invalidates that
shortcut: interpreter BEQ/BNE-false explicitly tests before the next word,
other ordinary-false families differ, while normal dynarec paths emit the
executed slot before the branch event test. These are not a single contract.
Synchronous exception EPC/BD and BIOS-selected return are separate questions.
Source does not prove console behavior or live dynarec slot-syscall outcome.

## Decision

Do not fix the ordinary-false flag solely from "exclude branch/slot". Do not
make handled likely-slot paths agree by guessing slot+4, reexecuting branch
or applying a lost destination. Qualify engine, branch family, HLE exception
and execution mode per segment. Record vector entry, EPC/BD, slot effect,
selected return EPC and continuation independently. Distinguish predictions
from live observations. This does not adopt either emulator engine as a
hardware oracle or discard the current full-state differential gate.

Current-behavior fixtures are characterization, not a work-clock acceptance
criterion. Change their expectations with a separately justified production
change; never preserve a proved error just to keep these fixtures green.

## Trade-offs / consequences

A uniform paired-slot policy is simpler and may later prove appropriate, but
adopting it now would hide reference differences instead of verifying them.
Leaving production unchanged delays a time policy, preserving the verified
baseline and avoiding unjustified compatibility/serialized-state changes.

Equal stopped-slot snapshots do not serialize/compare the interpreter's
hidden pending target. Tests expose three native/bridge eligibility gaps and
one exact native callee poll/restore edge. A later resumable-slot design needs
explicit pending context, checkpoint cleanliness, comparator coverage and
translation/interrupt identities, not just a BoundaryKind and PC.

Matched update 1000-us/root interval is still required before work->time.
No guessed quantum, WaitSema special case or reversal of correct preemption.
Evidence: `docs/reverse-engineering/slice99-branch-slot-event-audit.md`.

Slice100 application (2026-10-09): plain dynarec vector/ERET control succeeded,
but syscall-slot continuations retained EXL and bypassed the handler; pinned
BEQ source explains an exception-selected PC overwritten before dispatch.
The release interpreter ignored build-guarded per-word breakpoint probes;
late BEQL run paused in BIOS, not fixture latch. No universal exclusion,
branch/return policy or clock conversion inferred. Evidence:
`docs/reverse-engineering/slice100-live-branch-controls.md`.
