# Persistent project instructions

This file is auto-loaded at the start of every agent session. It is the working
agreement for GT4Recomp in this fork and the entry point to persistent project
knowledge. Keep it current whenever the rules change.

## Mission and context

GT4Recomp statically recompiles the pinned Gran Turismo 4 (USA) v2.00 R5900
code ahead of time into C++20 for Windows x86-64, supplying the PS2 services
execution requires. Learning and evidence quality take priority over speed.
This repository (github.com/moesuito/gt4-staticrecomp) is the working fork.
Upstream (github.com/Kaezzey/gt4-staticrecomp) is a read-only reference: we do
not push or open pull requests there.

## Roles and autonomy

- The agent is the engineer and decision maker for technical work: it plans,
  implements, verifies, documents, commits, branches and pushes to `origin`.
- The human owner sets goals and priorities and reviews plain-language
  summaries. Never require code knowledge from the owner.
- Commits, branches and pushes are authorized and expected. Keep `main` green
  and pushed. Do not rewrite pushed history.

## Documentation is part of the work

Assume the chat may be compacted or lost at any moment; nothing important may
live only in chat. Write knowledge into the repository as it is produced, not
at the end of the session.

- `docs/STATUS.md` — read this first in a new session: current state,
  environment, open items, next actions. Update whenever the state changes.
- `docs/journal/YYYY-MM-DD.md` — append-only session log: what was done, what
  was discovered, what failed and why, what comes next.
- `docs/decisions/` — numbered records for choices with trade-offs, continuing
  the existing sequence.
- `docs/reverse-engineering/` — evidence documents per technique or milestone:
  hashes, commands, observed results and limits.
- `docs/lessons/` — worked explanations (EXPLAIN) per milestone. Tracked in
  git in this fork; upstream kept them local.
- `docs/inputs/` — only hash-pinned, distributable metadata; never payload bytes.

A change is only done when its documentation is updated. Before ending a
session: update `docs/STATUS.md`, append the journal entry, commit and push.

## Evidence discipline

- Label claims Confirmed / High confidence / Hypothesis / Unknown.
- Record input hashes, guest addresses or file offsets, tool and version,
  observed state, comparison method and the next experiment
  (see `docs/requirements.md` for the full convention).
- Prefer independent verification (Ghidra, a second tool, a hand-computed
  value). A self-written implementation can share a bug with itself.
- Never guess in code: unsupported instructions and services stop with useful
  context; returning success cannot establish correctness. No silent fallbacks.
- Pin versions and hashes; reject changed inputs.

## Inputs and repository hygiene

- Game payloads, BIOS, extracted files, captures and translated code live only
  under ignored directories (`private/`, `iso/`, `generated/`), never in git.
- Verify inputs against `docs/inputs/` manifests before use; verification never
  updates a manifest.
- Check `git status --short` before every commit; never stage local game data,
  generated output or tooling.

## Code standards

Explicit owner requirement, recorded 2026-09-13:

All code written for this project must be HUMAN READABLE. Always assume a
human will read, review, and learn from it. Never justify obscure or compressed
code by assuming it will not be inspected by a person.

Use descriptive names, straightforward control flow, and clear formatting.
Prefer explicit, understandable steps over clever expressions or unnecessary
abstractions. Explain non-obvious intent and assumptions where they matter.
This requirement also applies to generated C++ and project tooling scripts.

The owner's exception permits assuming that test files and test scripts may
not be read by a human. This does not extend to general project scripts.

Additional standards:

- C++20, MSVC x64, Ninja, CMake 3.24+; keep the build warning-free and green.
- New behavior needs automated tests; extend the existing CTest and Python
  fixtures instead of adding parallel harnesses.
- Keep guest state explicit: fixed-width integers, controlled guest addresses,
  defined wrapping; no host undefined behavior as a stand-in for guest
  semantics.

## Git workflow

- `main` always builds and passes `ctest`; push it to `origin` regularly.
- Work in small verified slices. For multi-step work use a short-lived branch
  (e.g. `m7-control-flow`) and merge to `main` when green.
- Commit style follows the project history: short imperative subject lines with
  the evidence numbers that justify the change. One logical change per commit;
  code, tests and docs travel together.
- Fetch `upstream` occasionally; merging it is a deliberate, documented
  decision, never automatic.
- Push every verified, documented commit to `origin`.

## Quick reference

Live state: `docs/STATUS.md`. As of 2026-10-09 (slice 100, baseline `63a3fc8`, green):

Owner-requested model-switch handoff: `docs/RETOMADA.md` (2026-10-09).
It records the closed slice100, local artifacts, qualified limits and the
not-yet-started slice101; root `HANDOFF.md` is archival machine-move material.

- Recompiler pipeline M0–M29 complete: decoder covers 349 operations;
  whole-text scan finds 497 unsupported words of 1,334,917 (467 inside
  the trailing 700-word data table, 30 in real code: 26 COP2 macro
  function-0x38 words plus two DMA-dependent BC0F and two unassigned
  function-0x28 encodings — slice-58 address audit). The translator
  handles 99.5% of direct-call targets, and `--all` generates the whole
  game as one module (15,068 functions, 924,991 static instructions,
  ~216 MB with slice-98 observation calls).
- Boot + model: historical slices reached disc/archive/sound/font, but the
  slice-95 corrected interrupt return exposes an earlier timing frontier
  (details below). The translator-vs-interpreter 90,000-service gate is
  green; old 800k/5M horizons used different semantics and are historical.
  Contracts P00–P10 are done (decisions 0028–0036
  accepted; 0034/0037/0038 drafts open): checkpoints with semantic
  identity, 16-bit timers with W1C, separate INTC/DMAC domains, one
  advance machine, handler arguments with explicit idle, jr capture with
  explicit module-exit reasons, real DMA payload with chain walking, RPC
  telemetry with `--strict-rpc`, and a widened state comparator.
- Frontier: **update loop excludes READY main before any instruction** (slice 96).
  Corrected final interrupt-return preemption (decision 0013), exact PC
  preserved; fresh 10k/100k census now only threads 1–3, main READY/prio64,
  update RUN/prio0, 2 RPC pairs, zero GIF payload. Less boot progression is
  recorded honestly; old later phases depended on omitted preemption.
  Production still uses 1 ms/service; reference-backed time policy remains
  open. Semantic interrupt_model 4, time 3, kernel 2, RPC 1, translation 2;
  GT4CPT3/GT4KERN2 layout unchanged, old interrupt-model-3 checkpoints refused.
  Slice 96: all 270 one-shot 1000-us waits really block with a full 1 ms
  left; WaitSema's own 1 ms charge expires them and TIM2 interrupts root
  at its exact restored PC, before any guest instruction. Handler/return
  exclusion reproduces 273/273. Reference same loop has VBlank waiter;
  elapsed timing/phase alignment still unknown. No replacement adopted.
  Slice 97 reference: root retry 2000-us WaitSema dispatch 1111 EE cycles
  (~3.77 us nominal), 294400/294912 BUSCLK ticks remain; actual frame
  SleepThread dispatch 1630 cycles. Both select idle EPC 0x81FC0, not root.
  No matched reference update 1000-us/root-work interval. Offline timing
  reader pinned to audited save/build; no new clock policy. Reference closed,
  breakpoints removed and original slot9/backup restored with hashes.
  Slice 98: optional --count-work (decision 0040), 26 hand-counted paths;
  90k differential reports 22,566,319 completed / 90,000 accepted services
  identically, full state identical. 10k counted/plain output otherwise exact.
  Host counter outside contexts/checkpoints, no clock conversion. Preexisting
  handled likely-slot syscall gap explicitly tested: native5/1 vs reference1/0.
  Slice 99 source/fixture audit (decision 0041): pinned PCSX2 interpreter
  BEQ/BNE-false tests at next word; normal dynarec pairs branch/slot before
  event test. Other families differ; no universal exclusion fix adopted.
  Qt Step Into resumes to temporary breakpoint with selected motor, not
  forced interpreter/one-word execution. 24 characterization controls:
  six branch paths, direct/segmented event probes, three equal-state stopped
  slots with different pending ownership, native callee poll/restore work3->7.
  BIOS slot return/live trap context and matched update interval unknown;
  production binary identical to slice98, no new clock or compatibility.
  Slice100 live: plain dynarec GetThreadId vector -> ERET -> continuation
  captured, v0=1. Six syscall-slot paths: five EXL1/EPC=B/BD0/v0=-1
  continuations with no vector hit, BEQL false annuls. Six effect controls
  confirm slot/annulment. Pinned BEQ emitter overwrites exception-selected
  PC before dispatch; do not copy this reference defect into production.
  Latch-only and BEQL no-range-breakpoint repeats agree. Interpreter
  per-word breakpoint probe build-guarded and ignored by this binary;
  BEQL run logs cpuException's BD warning then later BIOS PC, not latch.
  Warning is PCSX2 output, not BIOS text; exact failure/selected return
  still unknown. 31 captures re-extracted; owner INI/BIOS/slot9+backup
  hashes preserved, isolated process closed normally. No runtime change.
  Evidence: `docs/reverse-engineering/slice100-live-branch-controls.md`.
- Gates: 53/53 CTest + Python 91 (85 run, 6 skip), re-run at slice 100;
  no existing acceptance expression weakened.
  Tripwires armed; M35's pad promoter watched
  (the first padman bind reopens input work).
- Next: **slice 101 — separately qualified interpreter trap observation**.
  Pin any diagnostic/Devel reference build anew; capture first slot vector
  EPC/BD, BIOS-selected return and interpreter failure chain; include handled
  likely-slot gap. Do not call release interpreter Step Into a word trace.
  Match reference update 1000-us/root
  interval before any work->time policy; do not assume both PCSX2 engines agree.
  Account identically, including non-service computation;
  do not guess an instruction/cycle conversion or special-case WaitSema.
  Slice-94 attribution/exclusion rates concern the old interrupt_model 3,
  not the corrected trace. Do not undo correct preemption to recover older
  boot counters, adopt a guessed quantum, or resume
  diagnostic timing with production checkpoint identity. Service 0x100 at
  0x1604 is deferred return (Patch/Interrupt), NOT idle. Draft decisions
  0037/0038 are superseded. Serialize builds and tests using their binaries
  on Windows: parallel CLI tests can lock linker outputs.
- Build (VS Developer PowerShell):
  `cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug -DCMAKE_CXX_COMPILER=cl`
  then `cmake --build build` then `ctest --test-dir build --output-on-failure`.
- Python tests:
  `private/tooling-venv/Scripts/python.exe -m unittest discover -s tests/python`.
- Local inputs: ISO at the repository root; `private/fingerprint-check/CORE.GT4`;
  analysis ELF at `private/reconstructed/SCUS_973.28.elf`.
- Ghidra verification: `private/tooling/jdk-21` and
  `private/tooling/ghidra_12.1.3_PUBLIC`; full command in
  `docs/reverse-engineering/m6-disassembly.md`.

## Subagents

- Use subagents for exploration, research and parallelizable work. They start
  with fresh context: include every relevant path, constraint and expected
  output in the prompt.
- Review findings before acting or committing; never propagate an unverified
  claim.
- Persist knowledge that must survive: if a subagent produced something worth
  keeping, write it into the matching document.
- Only one writer at a time: serialize repository mutations across agents.

## Communication with the owner

- Speak Portuguese with the owner, in plain language, without unexplained
  jargon.
- Lead with what changed and what it means for the project. When a decision
  matters, state options, trade-offs and a recommendation.
- Do not ask the owner to review code; summarize behavior and evidence.
