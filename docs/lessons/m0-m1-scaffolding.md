# M0–M1 lesson — scaffolding: the build, the CLIs, and the rules before the code

Prepared 2026-10-04. BUILD/VERIFY: this slice is the floor the
gates stand on — `project_identity` (name is "GT4Recomp"),
`cli_smoke` (the identity line), the per-area unit tests, and
the Python suite run from its own venv. Sources: the repo
itself — `CMakeLists.txt`, `tools/`, `tests/`, `src/`,
`include/`, `.gitignore`, and the M0–M1 rows of
`docs/requirements.md`. EXPLAIN: this is the worked
explanation; tutoring review pending.

Every load-bearing statement below traces to a file or
directory listing read for this slice. No game-code claims
appear here; there was no game code yet when this floor was
laid.

## Objective and motivation

Every later slice assumes a machine that builds warning-free,
names its tools uniformly, verifies its inputs before touching
them, and keeps payloads out of git — and assumes the reader
knows where each of those lives. M0 builds that floor
(library, CLIs, CMake/CTest, ignore rules, documentation);
M1 teaches the console being targeted (EE, IOP, VU, GS and
data movement) through tutoring. This lesson records the
floor as found: what each part is, which test pins it, and
which conventions later work presupposes.

## Step 1 — the build file (three libraries, gated tools)

`CMakeLists.txt` (cmake_minimum_required 3.24, C++20 with
extensions off throughout) defines three static libraries:

- `gt4recomp_core` — `src/common/project.cpp` only: the
  project identity.
- `gt4recomp_decode` — fourteen files under `src/ee/`
  (decode, disassemble, flow, functions, state, checkpoint,
  interpreter, driver, services, kernel, timer, device,
  vu_macro): the whole guest-execution side behind one link
  line, which is why every unit test links exactly one of
  these three libraries for its area.
- `gt4recomp_executable` — four files under
  `src/executable/` (core_image, analysis_elf, disc_image,
  gt4_volume) plus pinned zlib 1.3.1 via FetchContent with a
  `URL_HASH SHA256` pin and examples off.

The tool executables sit behind `if(WIN32)`: `gt4core`,
`gt4disasm`, `gt4blocks`, `gt4cfg`, `gt4funcs`,
`gt4translate` (plus `gt4run`/`gt4boot` further down the
file), all sharing `tools/common/` (`verified_core`,
number/hex parsing, boundary text) and built
`NOMINMAX`/`WIN32_LEAN_AND_MEAN`-clean, with `/bigobj` for
the whole-program consumer. The documented build is VS
Developer PowerShell, Ninja, Debug, `cl` — bare shells lack
the system headers, which is why every build/test session
since has run inside one `VsDevCmd` invocation.

## Step 2 — the two smallest tests (identity, pinned twice)

- `project_identity` builds `tests/unit/project_test.cpp`
  (eleven lines) asserting `project_name() == "GT4Recomp"`
  against `src/common/project.cpp` returning exactly that.
- `cli_smoke` runs the `gt4recomp` tool
  (`tools/gt4recomp/main.cpp`, seven lines) and requires its
  stdout to contain `GT4Recomp: learning laboratory`.

Honest fossil note, read off the file: the tool still prints
"no guest execution implemented." The sentence was true when
M0 landed and is stale now — kept because no slice has needed
to touch it, not because it describes the project. A stale
string in a passing smoke test is a reminder of what smoke
tests do and do not prove: the binary runs and names itself,
nothing more.

## Step 3 — the two suites and their wirings

C++ side: one test binary per area
(`ee_disassemble_tests`, `ee_flow_tests`, …,
`ee_checkpoint_tests`, …), each linked to exactly one
library, registered with `add_test` under a short name
(`ee_decode`, `ee_checkpoint`, `gt4boot_checkpoint`, …).
Game-data tests gate on local files:
`GT4_LOCAL_CORE`/`GT4_LOCAL_ISO` point at the pinned disc
artifacts; the disc-image test runs synthetic-only without
the ISO, and the originating/pad-era tests exist only with
it. Fixture inputs live in-tree (`tests/data/`, e.g. the
synth straight/branching programs passed as argv).

Python side: eleven `test_*.py` files under `tests/python/`
with no CTest registration — they run from the pinned venv
(`private/tooling-venv`) as `python -m unittest discover -s
tests/python`, conventionally reported as collected/run/
skipped (73 collected, 67 run, 6 skip at the current
frontier). The venv itself lives under ignored
`private/`, like every host-tooling install.

## Step 4 — the conventions later work presupposes

- **Payloads never enter git.** `.gitignore` keeps out
  `private/`, `generated/`, `*.iso`, `CORE.GT4`, `*.elf`,
  and `build/` (plus editor/bytecode droppings). Game bytes
  live only under ignored directories; the repo carries
  hashes and metadata (`docs/inputs/`). Every slice since
  has checked `git status --short` before committing.
- **Inputs are verified before use.** `tools/common/
  verified_core` exists so no tool trusts a renamed file;
  manifests pin sizes and hashes, and verification never
  updates a manifest.
- **Tools refuse instead of guessing.** The `gt4core`
  writer uses `CREATE_NEW` (atomically refusing existing
  files); checkpoint saves refuse dirty stops loudly. The
  no-silent-fallbacks rule is older than any guest
  semantics in the tree.
- **Live state has one address.** `docs/STATUS.md` is the
  first document of every session; the journal
  (`docs/journal/YYYY-MM-DD.md`) is append-only; decisions
  are numbered; evidence lives per-slice. "Done" includes
  the docs, the green gates, and the push.
- **M1, honestly:** the requirements row records a PS2
  architecture lesson (EE, IOP, VU, GS, data movement)
  delivered via tutoring alongside M0–M2. No repo artifact
  for it was found in this slice's listings — it lives in
  the curriculum table and in everything built on top of
  it, not in a file. That absence is stated, not filled.

## Connection to our implementation

| Piece | Location (as read) | Job |
| --- | --- | --- |
| Identity | `src/common/project.cpp` + `include/gt4recomp/project.hpp` | the name every gate asserts |
| Identity test | `tests/unit/project_test.cpp` → `project_identity` | pins the name |
| Smoke test | `tools/gt4recomp/main.cpp` → `cli_smoke` | pins the binary runs + names itself |
| Libraries | `CMakeLists.txt` (`gt4recomp_core/decode/executable`, C++20, extensions off) | one link line per area |
| Windows tooling | `tools/common/` + `if(WIN32)` block (`NOMINMAX`, `/bigobj`) | uniform CLI surface |
| Input gate | `tools/common/verified_core.*` + `docs/inputs/` manifests | hashes before bytes |
| Ignore rules | `.gitignore` (`private/`, `generated/`, ISOs, ELFs, `build/`) | payloads out of git |
| C++ tests | `tests/unit/*.cpp` → `add_test` short names | per-area gates |
| Python tests | `tests/python/test_*.py` via the `private/` venv, unittest discover | protocol/suite gates |
| Fixtures | `tests/data/` (synth programs as argv) | in-tree inputs |

## Understanding checkpoint

1. `cli_smoke` passes while printing a stale sentence ("no
   guest execution implemented"). Explain exactly what the
   test proves — and the class of regression it cannot catch.
2. Each unit test links exactly one of the three libraries.
   What does that linkage discipline buy when a test fails,
   and what would a test linking all three lose?
3. The disc-image test runs synthetic-only without the ISO
   but the originating test does not exist without it. State
   the rule that decides which pattern a new game-data test
   follows.
4. `CREATE_NEW` refuses an existing output instead of
   overwriting it. Connect this to the checkpoint save rule
   and name the shared principle.
5. The Python suite has no CTest registration. What breaks,
   concretely, if someone "fixes" that by registering
   `unittest discover` as one CTest — and what is the honest
   alternative?
6. M1 has no repo artifact. Argue why inventing one now
   (e.g. a summary architecture doc written from memory)
   would violate the project's evidence discipline — and
   what a legitimate M1 artifact would require.
