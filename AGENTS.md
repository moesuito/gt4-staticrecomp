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

Live state: `docs/STATUS.md`. As of 2026-10-02:

- M0-M30 slice 13 BUILD/VERIFY complete; decoder covers 349 operations; the
  only unsupported words left in the real code region are two DMA-dependent
  BC0F and two unassigned encodings inside the exception handler (the text's
  trailing 700 words are a data table). The translator handles 99.5% of the
  direct-call targets, and `--all` generates the whole game as one module
  (15,068 functions, 924,991 instructions, 146 MB, MSVC syntax-checked). The
  boundary driver executes translated modules as programs; the BIOS service
  layer models SetupThread, SetupHeap, FlushCache, the thread/semaphore
  scheduler, kernel patches, the timer registers (now ticking at idle), the
  OSD configuration, the DMAC/SIF register banks, the SIF services with a
  model IOP that answers the SIFCMD init handshake and the RPC
  bind/call/version/reset protocol, the GS and extended OSD services, the
  peripheral windows, and the DMA channels (a started transfer completes at
  once and raises its cause); the driver injects interrupts (SIF DMA
  completions, DMA channel completions and, at idleness, VBlank and the
  timer compares with the registered handler chains); the step-by-step
  interpreter bridges the boundaries a module cannot pass. `gt4boot` runs
  the whole game as one module from the ELF entry through the whole init
  chain, the game's thread creation, the SIFCMD handshake, the IOP reset,
  the RPC initialization and the game's runtime threads (the cooperative
  scheduler and the injected interrupts exercised end to end) with the state
  identical to the interpreter (7,573,241 instructions at the 3,000-service
  comparison point; injected handlers run with no nesting and no preemption
  per decision 0013, the version queries answer the game's compatibility
  constants, Deci2Call is accepted and the model IOP holds 80 RPC servers
  per decision 0014, so the boot binds the disc subsystem, negotiates the
  fileio/CDVD versions and runs an **11-thread worker pool to the
  200,000,000-step limit** inside the 0x0058F000 subsystem init); `--threads`
  prints the kernel's thread table, the handler tables, the DMA/timer state
  and the deferred-call counts after a run.
- Next: M30 slice 14 — the 0x0058F000 subsystem init loop and the
  string-coded servers it drives (the live PCSX2 emulator is the oracle for
  the real replies).
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
