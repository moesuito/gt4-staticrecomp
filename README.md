# GT4Recomp

A learning-first, GT4-specific static recompilation project targeting C++20
and Windows x86-64. The intended result translates the selected game's R5900
code ahead of time and supplies the PS2 services that execution requires.

## Build

[M6 disassembly](docs/reverse-engineering/m6-disassembly.md) adds `gt4disasm` for
selected real GT4 ranges. Ten regions were inspected: after the 2026-10-01
decoder expansion and Ghidra re-verification, 417 of 488 words match Ghidra
(352 non-NOP) and 71 remained explicitly unsupported. At that milestone the
decoder supported 39 operations, including the REGIMM branch family,
JALR/SYSCALL, LD/SD/SB/LH and
SLT/SLTU/DADDU. Later expansions added LB/LBU/SRA/SLTI/SLTIU/XORI, corrected
SLT/SLTU/SLTI/SLTIU to their 64-bit comparison semantics, added the COP1/MMI
extensions, the unaligned-access and multiply/divide families, COP0 with the
break boundary, the 64-bit shift family, the VU0 macro instruction set with
its vector state, moves, quad accesses and the full macro arithmetic, and the
trapping arithmetic with the parallel multiply/divide family (349 operations
in total, with PCSX2 used as the semantic reference; CACHE
and PREF decode as the no-op hints they are). An honest whole-text scan puts
the remaining unsupported words at 497 of 1,334,917 — 467 of them inside the
text's trailing 700-word data table (a table, not code), leaving 30
unsupported words in the real code region (26 COP2 macro function-0x38
words in real code plus the known four: two DMA-dependent BC0F and two
words at an unassigned encoding inside the exception handler — slice-58
address audit); the first
350,000 words — every sampled region — decode
cleanly. The game's startup executes in
the interpreter from
its ELF entry to the first BIOS syscall — and, through `gt4translate`, as a
native C++ module verified identical to the interpreter after 942,695
instructions; tail thunks and syscall boundaries translate too, stopping at
service calls exactly like the interpreter, and the translator reaches the
VU0 macro and trapping operations through the verified runtime executor
(the module 0x0056DF58 verifies that path end to end). A whole-text survey
(`gt4translate --survey`) reports that **99.5% of the game's 15,067
direct-call targets translate**, covering 65.3% of the text's instructions,
and the `--all` mode generates the **whole game as one module (15,068
functions, 924,991 instructions, 146 MB) that passes an MSVC syntax check**.
The full CTest set (53/53) and the Python suite (73 collected, 67 run,
6 skip without the M3 reference ELF) pass locally; the boot runs as a
native module with the translator-vs-interpreter differential green. Tutoring remains pending; see the
[M6 lesson](docs/lessons/m6.md).

[M4 native reconstruction](docs/lessons/m2-m5-foundation.md) now reads the pinned CORE into
our own C++ executable-image model and writes an analysis ELF. All three payloads,
entry and declared memory ranges match the M3 reference policy; alignment is
corrected. Ghidra verified imported payloads and the declared zero-fill range.
Tutoring remains pending. This is analysis output, not a verified bootable port.

Use an x64 Visual Studio Developer PowerShell with MSVC, the Windows SDK,
CMake (3.24+), and Ninja available. The first configure downloads hash-pinned
zlib 1.3.1 into the ignored build directory. No PS2 runtime or reference builder
is needed by our native reconstruction path.

```powershell
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug -DCMAKE_CXX_COMPILER=cl
cmake --build build
ctest --test-dir build --output-on-failure
.\build\gt4recomp.exe
```

For an offline configure, append
`-DGT4_ZLIB_ARCHIVE=C:/absolute/path/to/zlib-1.3.1.tar.gz` to the configure command.
The archive is still hash-checked. This workspace has a copy under
`private/dependencies/`. CTest now covers image reconstruction and ELF output as
well as instruction decoding and the original build smoke checks, plus the
full boot, checkpoint and RPC suites; no pixel work is established yet.

## Disassemble a selected region

```powershell
.\build\gt4disasm.exe private/fingerprint-check/CORE.GT4 0x10011c 31
python scripts/sample_disassembly.py
```

The count is instructions. The CLI prints the listing to stdout and unsupported
opcode counts to stderr. The script saves ten selected regions under ignored
`private/disassembly/`; it needs only standard Python. See the
[M6 evidence](docs/reverse-engineering/m6-disassembly.md) for Ghidra comparison commands.

## Inspect one basic block

```powershell
.\build\gt4blocks.exe private/fingerprint-check/CORE.GT4 0x5a3140 40
```

The third argument is the instruction limit. The walk ends at the first control
transfer and includes its delay slot; the stderr summary records the ending
kind, static target, continuation address and stop reason. Unsupported words
stop the walk with context. See the
[M7 evidence](docs/reverse-engineering/m7-control-flow.md).

## Follow static control flow

```powershell
.\build\gt4cfg.exe private/fingerprint-check/CORE.GT4 0x5a3140 200
```

The third argument caps the visited blocks. Branches and jumps are followed;
direct call targets are recorded but not followed; returns, indirect jumps,
exceptions and unsupported words end a path. The tool prints one line per block
and a stderr summary (blocks, instructions, edges, call targets, open ends,
outside-text edges, limit flag). See the
[M7 evidence](docs/reverse-engineering/m7-control-flow.md).

## Translate one real function

```powershell
.\build\gt4translate.exe private/fingerprint-check/CORE.GT4 0x577878 64
```

The translator emits a C++ header for one real function and its direct call
tree — plain instructions, conditional branches (including likely and link
forms), in-function jumps, `jal` calls (translated recursively; forward
declarations make cycles work) and multiple `jr ra` returns — statement by
statement, with the original assembly as comments. Indirect calls, exceptions
and unsupported words are rejected with context. The output is derived from
game code: keep it in ignored directories and never commit it. The translation
tests generate headers into the build tree and compare the translated modules
against the interpreter on six input states each. See the
[M13 evidence](docs/reverse-engineering/m13-first-function.md).

## Observe a running PCSX2

```powershell
& 'private\tooling-venv\Scripts\python.exe' scripts/pcsx2_pine.py info
& 'private\tooling-venv\Scripts\python.exe' scripts/pcsx2_pine.py verify-elf private/reconstructed/SCUS_973.28.elf 0x100000 0x517a14
```

Needs a local PCSX2 with PINE enabled (set `EnablePINE = true` in
`PCSX2.ini`) and the pinned disc running. The tool reads EE memory over PINE
and never writes it; only the explicit `save-state`/`load-state` commands
change emulator state. See the
[M14 evidence](docs/reverse-engineering/m14-live-observation.md).

## Read a PCSX2 savestate

```powershell
& 'private\tooling-venv\Scripts\python.exe' scripts/pcsx2_savestate.py registers "$env:USERPROFILE\Documents\PCSX2\sstates\SCUS-97328 (77E61C8A).09.p2s" --with-memory
```

Savestates are ZIP containers; this reads the CPU registers (pc, all 32 GPRs,
HI/LO, key CP0 registers) from the raw internal freeze stream and can extract
any entry (for example `eeMemory.bin`, the full 32 MiB EE RAM) — fully offline.
See the [M14 evidence](docs/reverse-engineering/m14-live-observation.md).

## Discover an evidence-backed function map

```powershell
.\build\gt4funcs.exe private/fingerprint-check/CORE.GT4 0x5a3140 50 500
```

Seeds are the ELF entry address plus the given start; every direct `jal` target
found inside an analyzed function becomes a new candidate. Returns, indirect
calls and unsupported words never invent entries. Each line reports one
function's bounded reachable set; the stderr summary aggregates counts and the
pending queue. See the
[M8 evidence](docs/reverse-engineering/m8-function-map.md).

## Reconstruct an analysis ELF

With the verified CORE copy from M2:

```powershell
New-Item -ItemType Directory -Force private/reconstructed | Out-Null
.\build\gt4core.exe --reference-analysis private/fingerprint-check/CORE.GT4 private/reconstructed/SCUS_973.28.elf
```

The output must not already exist. The explicit flag adopts the M3 builder's
unproven BSS/reginfo choices for analysis only. See the [M2-M5 foundation notes](docs/lessons/m2-m5-foundation.md)
for the implementation walkthrough, comparison commands and limitations.

## Verify the selected disc

The [M2-M5 foundation notes](docs/lessons/m2-m5-foundation.md) include Python environment setup and the
12 standalone synthetic tests. With that environment available:

```powershell
& '.\private\tooling-venv\Scripts\python.exe' scripts/gt4disc.py verify 'Gran Turismo 4 (USA) (v2.00).iso'
```

The [pinned manifest](docs/inputs/usa-v2.00.json) contains sizes, hashes and
metadata only. Verification fails on changed input and never updates it.

## Local data and source control

- `include/`, `src/`, `tools/`, `tests/`, and `docs/`: original source and notes.
- `build/`: ignored host compiler output.
- `iso/`, `private/`: ignored local inputs, extracted files, BIOS, and captures.
- `generated/`: ignored translated game code and other derivative output.

Keep sensitive inputs in the ignored directories; extensions alone cannot
identify every BIOS or extracted asset. Ignore rules do not protect files
already tracked or force-added. Review staged content before your commits.

The initial workspace was not a Git repository. Git initialization, staging,
and commits are left to the owner. No PCSX2 or PS2Recomp execution core is
included or required by this build.
