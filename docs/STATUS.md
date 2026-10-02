# Project status

Updated 2026-10-01 after M13 — the first major technical landmark. This is the
first document to read in a new session; it is kept current as work proceeds.
Details live in the linked evidence documents.

## Where we are

- Target: Gran Turismo 4 (USA) v2.00, serial SCUS-97328, pinned in
  `docs/inputs/usa-v2.00.json`; the local ISO matches the manifest.
- Curriculum and acceptance table: `docs/requirements.md`.
- M0-M6 BUILD/VERIFY complete:
  - M0 core/CLI/CMake; M2 disc verification; M3 reference ELF (upstream run);
    M4 native image and analysis ELF (byte-identical to the pinned hash here);
    M5 decoder; M6 disassembler.
  - The decoder covers 39 operations. Ghidra re-verification 2026-10-01:
    417 matched (352 non-NOP), 71 unsupported, 0 mismatches
    (`docs/reverse-engineering/m6-disassembly.md`).
- M7 (2026-10-01): flow classification, delay-slot-aware basic blocks
  (`gt4blocks`) and deterministic CFG traversal (`gt4cfg`); real seeded run:
  15 blocks, 71 instructions, 20 edges
  (`docs/reverse-engineering/m7-control-flow.md`).
- M8 (2026-10-01): evidence-backed function map with `elf-entry`, `seed` and
  `direct-call` evidence (`gt4funcs`); real closures over several seeds
  (`docs/reverse-engineering/m8-function-map.md`).
- M9 (2026-10-01): explicit guest state and memory model with
  context-carrying errors (`docs/reverse-engineering/m9-guest-state.md`).
- M10 (2026-10-01): one-instruction-at-a-time test interpreter — delay slots,
  likely-branch nullification, link registers, stable stops
  (`docs/reverse-engineering/m10-interpreter.md`).
- M11 (2026-10-01): generated straight-line suites — seeded Python generator
  with an independent reference model; 40 programs covering all 20 ops
  (`docs/reverse-engineering/m11-synthetic-programs.md`).
- M12 (2026-10-01): branching suites — loops, conditional skips, likely/link
  branches, call/return; all 14 branch ops plus jal/jr
  (`docs/reverse-engineering/m12-branching-programs.md`).
- **M13 (2026-10-01): the first real GT4 function compiled natively.**
  Candidate `0x00577878` (4-instruction `direct-call` leaf, delay-slot store)
  translated by the new `gt4translate` into C++; verified identical to the
  interpreter on **6 input states** — all 32 registers, the entire memory
  image, and the continuation. Translator slice 2 adds conditional branches,
  likely/link forms, in-function loops and multiple returns; verified on
  `0x005c11a8` (a lazy initializer) with 6 states across both branch outcomes
  (`docs/reverse-engineering/m13-first-function.md`).
- EXPLAIN: lessons written for M6, M7 and M8 (`docs/lessons/`); the M9-M13
  lessons and retroactive M2-M5 notes remain open.
- Next technical milestone work: M14 — automate observation/snapshots toward
  PCSX2 comparison; extend the translator along the M7 CFG shapes.

## Environment (this machine, `C:\Antigravity\gt4-staticrecomp`)

- Build: VS 2022 Build Tools 17.14 + MSVC 19.44 + Ninja 1.13.2 + CMake 4.3.1;
  commands in `AGENTS.md` and `README.md`.
- Tests: 14/14 CTest (the two translation tests exist only where the local
  CORE does); Python suite 52 collected (46 run, 6 skip without the M3
  reference ELF).
- Local inputs (ignored): ISO at the repository root;
  `private/fingerprint-check/CORE.GT4` (2,020,861 bytes, hash matches the
  pinned manifest); `private/reconstructed/SCUS_973.28.elf` (6,123,004 bytes,
  SHA-256 equals the pinned native ELF).
- Tooling venv: `private/tooling-venv` (pycdlib 1.20.0).
- Ghidra 12.1.3 + Temurin JDK 21.0.12.1+1 under `private/tooling/`; hashes and
  provenance in `docs/environment.md`.
- Disposable Ghidra project directory: `%TEMP%\GT4Recomp-M7`.

## Open items

- The M3 reference ELF (PDTools GT4ElfBuilderTool, hash-pinned in
  `docs/inputs/usa-v2.00-reference.json`) is not regenerated here, so 6
  optional native CLI tests skip. Rebuilding it is an optional future task.
- Retroactive lesson notes for M2-M5 are not written; the M9-M13 lessons are
  pending.
- 71 unsupported words: COP1 (34), MMI (31) and five single encodings,
  deferred to M15-M17 by the curriculum.

## Next actions

1. M14: automate observation/snapshots toward PCSX2 comparison; extend
   `gt4translate` further (calls next), each verified the same way as M13.
2. The M9-M13 lessons and retroactive M2-M5 notes if useful.
3. Keep the journal and this file current after every working session.

## Journal

- [2026-10-01](journal/2026-10-01.md) — fork setup, environment validation,
  decoder expansion, Ghidra verification, working rules, M6-M8 lessons, M7
  slices 1-2, M8 function map, M9 state model, M10 interpreter, M11/M12
  synthetic suites, M13 first natively compiled function.
