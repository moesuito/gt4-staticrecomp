# 0028 — Checkpoint baseline and model-compatibility policy (P00)

Date: 2026-10-04. Status: accepted (implemented in slice 64).
Predecessors: 0022 (checkpoint/resume semantics), 0024 (chained,
leg-relative counts), 0027 (autosave). Plan: PLAN.md §6 P00.

## Context

Checkpoints carried only a format magic (`GT4CPT1`) and a service count.
A git SHA alone cannot express compatibility (docs-only commits would
invalidate everything), and nothing distinguished the file's wire format,
who wrote it, and which model semantics produced the state. The
leg-relative recount rule (0024) was also easy to misread from log lines
alone (PLAN.md §3.4). P01/P02 are about to change timer/interrupt
semantics, so the baseline and the refusal rule must exist first.

## Decision

1. **Three separate identities.** Wire format (`GT4CPT2` magic),
   provenance (`GT4PROV1` section: commit, binary, verified CORE hash,
   disc note, time policy — diagnosis only, never a gate), and semantic
   compatibility (four u32: time, interrupt, RPC, translation).
2. **Semantic gate, editorial freedom.** Only the four domain versions
   gate a restore, checked after the file parses and before anything is
   applied, with per-domain detail. A new commit or comment keeps old
   files loadable; a semantic change bumps exactly its domain.
3. **Old files are forensic.** `GT4CPT1` (no semantic identity) is
   refused with a naming message, never restored, never silently
   reinterpreted.
4. **Restore bypasses guest-write effects.** `RegisterBank::restore_registers`
   writes storage directly instead of sharing `write_register`; the DMA
   channel already bypassed. The device contract keeps its write path —
   the fix is on the restore path, so P01/P02 flag/mask work cannot leak
   into restores. Pinned by a discriminating test (armed `STR|TIE`
   restores verbatim without firing).
5. **Logs name their scope.** Fresh vs leg-relative vs cumulative on the
   stats, save, resume and verify lines; a `run provenance` header on
   every run. Provenance stays deterministic (no timestamps/host paths)
   so the autosave byte-identity CTest keeps passing.

## Consequences

- P01 (timer) bumps `time_model`, P02 (interrupt) bumps
  `interrupt_model`; both must document it, and every pre-change
  checkpoint becomes forensic automatically.
- Disc-hash-at-open verification stays open (provenance says
  "unverified at open"); time-policy promotion to semantics is P03's
  call.
- Out of scope: any semantic, DMA, JR or RPC behavior change.

## Verification

Unit fixtures `ee_checkpoint` (provenance codec, magic pin, editorial
acceptance, 4 domain refusals, forensic refusal) and `ee_device`
(restore bypass); CTest 50/50; Python 73 (6 skips); live
save/resume/refusal probes; forensic inventory intact. Evidence:
`docs/reverse-engineering/slice64-p00-baseline.md`.
