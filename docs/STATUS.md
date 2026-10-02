# Project status

Updated 2026-10-01 after M7 slice 1. This is the first document to read in a
new session; it is kept current as work proceeds. Details live in the linked
evidence documents.

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
- M7 slice 1 (2026-10-01): flow classification and delay-slot-aware basic
  blocks; new `gt4blocks` frontend; Ghidra re-run identical after the shared
  target-helper refactor (`docs/reverse-engineering/m7-control-flow.md`).
- M7 slice 2 (2026-10-01): deterministic CFG traversal over static successors
  with recorded direct call targets and bounded work; new `gt4cfg` frontend.
  Real seeded run: 15 blocks, 71 instructions, 20 edges, 1 open end.
- EXPLAIN: `docs/lessons/m6.md` written; lesson for M7 pending after its
  remaining slices.
- Next technical milestone work: M7 slice 2 — CFG traversal over block
  successors, then the evidence-backed function map (M8).

## Environment (this machine, `C:\Antigravity\gt4-staticrecomp`)

- Build: VS 2022 Build Tools 17.14 + MSVC 19.44 + Ninja 1.13.2 + CMake 4.3.1;
  commands in `AGENTS.md` and `README.md`.
- Tests: 7/7 CTest; Python suite 34 collected (28 run, 6 skip without the M3
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
- Retroactive lesson notes for M2-M5 are not written; M6 has a lesson.
- 71 unsupported words: COP1 (34), MMI (31) and five single encodings,
  deferred to M15-M17 by the curriculum.

## Next actions

1. Evidence-backed function map (M8): seed traversal from recorded call
   targets and known entry points, then classify the discovered regions.
2. M7 lesson after the slice set stabilizes.
3. Keep the journal and this file current after every working session.

## Journal

- [2026-10-01](journal/2026-10-01.md) — fork setup, environment validation,
  decoder expansion, Ghidra verification, working rules, M6 lesson, M7 slice 1.
