# Project status

Updated 2026-10-01 after M8 and the M7/M8 lessons. This is the first document
to read in a new session; it is kept current as work proceeds. Details live in
the linked evidence documents.

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
- M8 (2026-10-01): evidence-backed function map with `elf-entry`, `seed` and
  `direct-call` evidence and bounded reachable sets; new `gt4funcs` frontend.
  Real seeded closure: 10 functions, 8 direct calls, `pending=0`
  (`docs/reverse-engineering/m8-function-map.md`).
- EXPLAIN: lessons written for M6, M7 and M8 (`docs/lessons/`); retroactive
  notes for M2-M5 and tutoring review remain open.
- Next technical milestone work: M9 — explicit guest state and memory model.

## Environment (this machine, `C:\Antigravity\gt4-staticrecomp`)

- Build: VS 2022 Build Tools 17.14 + MSVC 19.44 + Ninja 1.13.2 + CMake 4.3.1;
  commands in `AGENTS.md` and `README.md`.
- Tests: 8/8 CTest; Python suite 37 collected (31 run, 6 skip without the M3
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
- Retroactive lesson notes for M2-M5 are not written.
- 71 unsupported words: COP1 (34), MMI (31) and five single encodings,
  deferred to M15-M17 by the curriculum.

## Next actions

1. Start M9: explicit guest state and memory model (registers, guest
   addresses, deterministic tests) per the curriculum.
2. Retroactive lesson notes for M2-M5 if useful.
3. Keep the journal and this file current after every working session.

## Journal

- [2026-10-01](journal/2026-10-01.md) — fork setup, environment validation,
  decoder expansion, Ghidra verification, working rules, M6-M8 lessons, M7
  slices 1-2, M8 function map.
