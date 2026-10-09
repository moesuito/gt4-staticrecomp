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

Live state: `docs/STATUS.md`. As of 2026-10-09 (`main` = `3ddc495` + slice-88 docs, green):

- Recompiler pipeline M0–M29 complete: decoder covers 349 operations;
  whole-text scan finds 497 unsupported words of 1,334,917 (467 inside
  the trailing 700-word data table, 30 in real code: 26 COP2 macro
  function-0x38 words plus two DMA-dependent BC0F and two unassigned
  function-0x28 encodings — slice-58 address audit). The translator
  handles 99.5% of direct-call targets, and `--all` generates the whole
  game as one module (15,068 functions, 924,991 instructions, 146 MB).
- Boot + model: `gt4boot` runs the game as one module from the ELF entry
  through init, threads, SIF/RPC, disc, archive, sound and font phases;
  the translator-vs-interpreter differential is green (90,000 services
  pinned, prefix verified to 800k, fresh legs marched to a stationary
  5M limit-cycle). Contracts P00–P10 are done (decisions 0028–0036
  accepted; 0034/0037/0038 drafts open): checkpoints with semantic
  identity, 16-bit timers with W1C, separate INTC/DMAC domains, one
  advance machine, handler arguments with explicit idle, jr capture with
  explicit module-exit reasons, real DMA payload with chain walking, RPC
  telemetry with `--strict-rpc`, and a widened state comparator.
- Frontier: **the slice-89 fix holds and the frontier moved** — the boot
  passes the gate (~3.3k services) and runs 18.5M services (1G-step
  budget) with frames flowing, then settles into a steady render loop
  frozen from ~10k (main thread sleeping, workers rendering, all RPC
  traffic done; VIF1 341,054 chains / 144.6 MB). The reference proceeds
  (notice → movie → menu); the divergence window is services ~3.5k–10k
  (`docs/reverse-engineering/slice90-post-gate-steady-loop.md`).
- Gates: 53/53 CTest + Python 73 (67 run, 6 skip), re-run at slice 89
  (code unchanged since). Tripwires armed; M35's pad promoter watched
  (the first padman bind reopens input work).
- Next: **slice 91 — identify the post-gate wait** (compare the model's
  ~10k state with the reference's t=14s state by meaning; trace services
  ~3.5k–10k; the card-service completion path is a candidate); check the
  thread-id space for the same class of bug. Draft decisions 0037/0038
  are superseded (marked in their files).
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
