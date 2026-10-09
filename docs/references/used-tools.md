# Used tools register — what was used, for what, with what result

Checked 2026-10-08 against the tree at `main` = `702fbc1` (docs refresh);
boot/reference/gate rows updated 2026-10-09 through slice 93.
Companion to `README.md` in this directory (which registers documentation
references): this file records *tool usage* — own, external, and
evaluated-but-refused — each with version, purpose, concrete result and
evidence path. Status words: **used** (ran here, result recorded),
**evaluated** (studied, partially or conditionally adopted), **refused**
(studied, not adopted, reason recorded).

Conventions: game payloads, BIOS, captures and generated code live only
under ignored directories (`private/`, `iso/`, `generated/`, `build/`)
and never enter git; only hashes, addresses, counts and relations below.
Tool versions are pins for repetition, not endorsements.

## A. Own tools (built in this repo)

| Tool | Purpose | Inputs → outputs | Key result + evidence |
| --- | --- | --- | --- |
| `gt4recomp` | Project-identity smoke CLI only (7 lines, no guest logic) | no args → one-line banner | `cli_smoke` CTest pins the banner (`docs/lessons/m0-m1-scaffolding.md`) |
| `gt4core` | Reconstructs the pinned CORE into the reference-analysis ELF (explicitly adopts the reference's unproven BSS/reginfo policy) | `gt4core --reference-analysis CORE.GT4 out.elf` → ELF + record report | Analysis ELF 6,123,004 bytes, SHA-256 equals pinned native ELF (`docs/lessons/m2-m5-foundation.md`) |
| `gt4disasm` | Disassembles N words at a guest address (M6 decoder front-end) | `gt4disasm CORE.GT4 address count` → stdout listing | M6: 417 matched / 0 mismatched (10 regions); M16: 594 / 0 + 34 R5900-only rows (`docs/reverse-engineering/m6-disassembly.md`, `m16-unaligned-and-multiply.md`) |
| `gt4blocks` | One delay-slot-aware basic block (M7) | `gt4blocks CORE.GT4 address limit` → listing + block summary | Seeded run: 15 blocks, 71 instructions, 20 edges (`docs/reverse-engineering/m7-control-flow.md`) |
| `gt4cfg` | Deterministic CFG traversal (M7) | `gt4cfg CORE.GT4 address max-blocks` → successor lists | Same M7 seeded run, traversal side (same evidence doc) |
| `gt4funcs` | Evidence-backed function map, `elf-entry`/`seed`/`direct-call` closures (M8) | `gt4funcs CORE.GT4 start max-fns max-blocks` → call-target lists | Real closures over several seeds (`docs/reverse-engineering/m8-function-map.md`) |
| `gt4translate` | Function + call-tree → C++ header with explicit `BoundaryKind` exits; `--survey`, `--all`, `--synth` modes | `gt4translate CORE.GT4 …` → `.hpp` module | First real function 0x00577878 (6 states identical); `--all`: 15,068 functions, 924,991 instructions (`docs/reverse-engineering/m13-first-function.md`, `m26-translation-survey.md`, `m29-whole-program-build.md`, `slice68-p05-jr-exits.md`) |
| `gt4run` | Boundary driver as a program for the startup module (M30 slice 1) | `gt4run CORE.GT4 [--compare-interpreter]` → boundary + state report | ELF entry → first BIOS syscall 0x001001C8, state identical after 942,695 instructions (`docs/reverse-engineering/m30-driver-first-slice.md`) |
| `gt4boot` | Whole game as one translated module: init, threads, SIF/RPC, disc, archive, sound, font; checkpoints, `--dump`, `--threads`, `--strict-rpc`, autosave | `gt4boot CORE.GT4 [--services N] [--steps N] [--disc ISO] [--compare-interpreter] …` | Old sema-63 knot fixed by decision 0039; slice 93 measures saturated device semaphore (300 signals/92 waits). Diagnostic smaller service quantum allows workers to run, but is not adopted (`docs/reverse-engineering/slice93-scheduler-and-device-semaphore.md`) |
| `scripts/gt4disc.py` | Read-only ISO inspect/verify (never writes data or baselines) | `verify ISO [--manifest]` → PASS/FAIL vs `docs/inputs/usa-v2.00.json` | Local ISO matches manifest, SCUS-97328 / VER 2.00 (`docs/reverse-engineering/input-identity.md`) |
| `scripts/pcsx2_pine.py` | Read-only PINE client (needs `EnablePINE=true`; never writes emulator memory) | `info/read/save-state/load-state/verify-elf` | Live text image == our image, 5,339,668 bytes, equal hashes; reginfo 24/24 (`docs/reverse-engineering/m14-live-observation.md`) |
| `scripts/pcsx2_savestate.py` | Offline savestate ZIP reader (`info`/`registers`/`extract`) | savestate path → CPU summary or entry bytes | Menu-savestate decode; eeMemory re-verified with 0 differences (same M14 doc; `slice79-pcsx2-observation.md`) |
| `scripts/synth_programs.py` + `sample_disassembly.py` + `inspect_reference.py` + `verify_native_image.py` | Seeded synthetic fixtures (committed, never game bytes); region capture; M3/M4 comparison helpers | → `tests/data/*.txt`, `private/disassembly/`, comparison reports | 40 straight-line + branching suites (M11/M12); 417/0 Ghidra run; native image byte-identical to pinned hash |
| CTest suite | 57 `add_test` lines → 53 unique names (if/else disc branches register once) | `ctest --test-dir build` | **53/53 green**, rerun slice 93 on restored production model; Python 73 (67 run, 6 skip). Areas: 15 EE units, identity/smoke, image/disc/volume, 15 translation, 17 gt4boot-lifecycle, 1 synth module-exit |
| Generated whole-program header | Translator output (ignored build tree, never committed) | `--all` → `translated-whole-program.hpp` | 15,068 functions, 924,991 instructions, 146.4 MB, ~136 s; MSVC syntax check 27.5 s (`docs/reverse-engineering/m29-whole-program-build.md`) |

## B. External tools used

| Tool | Version / location | Purpose | Concrete result + evidence | Limits found |
| --- | --- | --- | --- | --- |
| MSVC / VS Build Tools | 19.44 x64, VS 2022 Build Tools 17.14 (`cl 19.44.35222` per HANDOFF) | C++20 build | Green from 2/2 to 53/53 | Bare shell lacks SDK headers AND defaults to x86 (`LNK4272`); always chain `VsDevCmd -arch=amd64`; `cmake --build build` alone does not relink `gt4boot.exe` — use `--target gt4boot` (`docs/environment.md`) |
| CMake | 4.3.3 (min 3.24) | Configure | Green configures; hash-pinned zlib FetchContent | Absolute paths in cache — recreate `build/` after folder moves; `--fresh` after failed configure |
| Ninja | 1.13.2 | Build backend | Green builds | Order-only libs (see MSVC row) |
| Git | 2.54.0; remotes `origin` (push) + `upstream` (read-only) | Small verified slices | `main` green and pushed | `git status --short` before every commit; never stage payloads; never rewrite pushed history |
| Python + venv | 3.14.5 in ignored `private/tooling-venv/` (`pycdlib==1.20.0`, `zstandard==0.25.0`) | Observation + fixtures only | M2 manifest PASS; synthetic fixtures caught an ELF-offset and a savestate-tag bug before live use | Recreate venv if base path differs |
| Ghidra + JDK | Ghidra 12.1.3 (pinned zip + SHA-256) + Temurin JDK 21.0.12.1+1 under `private/tooling/`, `JAVA_HOME` per command | Independent decoder oracle (`MIPS:LE:64:64-32addr`, one-insn-at-a-time) | M6 417/0; M13-extended 475/0; M15 511/0 (+34 R5900-only); M16 594/0 (+34). First M6 run's 9 mismatches all Ghidra aliases | Match proves neither execution nor code-vs-data; normalization allowlist is explicit; generic MIPS ≠ full R5900 |
| PCSX2 + PINE + savestates + BIOS | Actual bundled executable v2.9.114 (folder label v2.9.94 is stale); older live nightly 2.9.93; PINE TCP 28011; reference BIOS v02.30 | Live oracle: pinned ISO, staged no-card/software-rendered boot, offline RAM/regs comparison | Slice 88 confirms notice → movie → main menu without input; slice 93 compares device flag/callback head from t0014 with model (`slice88-live-no-card-session.md`, `slice93-scheduler-and-device-semaphore.md`) | Freeze layout tied to build; PINE has no pause/step/breakpoint/regs; host timestamps do not establish phase alignment; BIOS semaphore counters not decoded; process now closed, not an unresolved launch/configuration blocker |
| pycdlib | 1.20.0, pinned in `scripts/requirements.txt` | Read-only ISO observation (decision 0002) | Manifest PASS (SCUS-97328 / VER 2.00) | FS parsing not independently hash-checked; no crypto/ISO dep in C++ runtime |
| zlib | 1.3.1, hash-pinned FetchContent (+ offline archive) | Native raw-DEFLATE for image reconstruction | M4 native ELF imports to Ghidra, payloads/zero-fill verified | Runtime BSS layout unresolved; loading not proven |

## C. References consulted / evaluated-but-refused

| Reference | Pinned revision | Used for | Adopted vs refused |
| --- | --- | --- | --- |
| PCSX2 source (Counters, Hw, DMA, SIF, debugger, PINE) | `81526d4` | 16-bit timer + W1C; INTC-vs-DMAC routing; masked QWC; QWC0→0x10000; ZeroReturn/gate notes; debugger/PINE limits | Adopted as named contract source in code comments; refused as execution engine, as perfection claim, and as full remote debugger |
| PS2SDK | `ac92a9f` | Extended time; SIF/RPC split; thread layout; handler `(cause,arg,addr)` + shared syscall numbers (a1/a2 by reference); DMA tag IDs; libmc error table (`-1`=change, `-2`=unformatted, `type=0`=no card) | Adopted for layouts/names/numbers; refused as automatic proof of GT4's exact SDK revision |
| PS2Tek | src `1c91660`, chain `295bc61` | Timer/DMAC/chain-mode corroboration; channel map; CHCR/TADR/ASR layout | Adopted with implementation/test corroboration; refused as sole authority (RE doc, not Sony manual) |
| ps2autotests | `97469ff` | TIE-never-gates-completion (tagintr, all 4 combos); sleep/wakeup hypothesis | Adopted narrowly per case; refused as suite import or homebrew-ELF runner |
| GT4FS | `master` (unpinned lead) | Inner-archive v3.1 cross-check | Adopted as parser cross-check; refused as outer-2.2/CD-delivery proof |
| PDTools / GT4Hooks | master / `d89e76b` | Reconstruction existence; candidate names (Online US `SCUS_974.36`) | Evaluated only; never copy addresses by name (retail is `SCUS_973.28`) |
| PS2Recomp / ps2xIOP | `c5a9d02` | Hybrid-IOP architecture study | Reference only; R3000A-interpreter path deferred to causal need |
| Play!, N64Recomp, ImHex, SDL3, Vulkan, Ghidra EE Reloaded | various / unpinned | Second opinions, future tracks, RE aids | Evaluated; ImHex never located; SDL3/Vulkan future-gated; Reloaded uninstalled (SaveStateImporter ≠ replay) |
| PCSX2-MCP fork | third-party, 29 stars (unpinned) | Stock-vs-modified question for the joint session | Refused for now: stock covers S0–S3; revisit on scripted-break, volume-logging, or proven stock miss (`docs/plans/pcsx2-tooling-note.md`) |
| OPUS IRX map | disc bytes (not a repo pin) | 24-SID→module ground truth; `rt_ac` note | Adopted as candidate map (item-by-item reproduction pending); sub-proposals refused per PLAN §4.4 (forced TAG END, generic -1, a2-by-name, zeroed thread-id, SID-found = done, alignment sizes, GS≡VIF/VU, 5–10× claim, 85k-unlocks-assets) |

## Pin ledger (repeat exactly these)

`PCSX2 81526d4` · `PS2SDK ac92a9f` · `PS2Tek-src 1c91660` + `Chain-Mode 295bc61` · `ps2autotests 97469ff` · `PS2Recomp c5a9d02` · `N64Recomp ffb39cd` · `N64ModernRuntime cdf5abb` · `GT4Hooks d89e76b` · `EE-Reloaded ae013ee`. Unpinned leads (pin before using as implementation contract): PDTools/master, GT4FS/master, Play!, SDL3 wiki, Vulkan docs, PCSX2-MCP fork. Ghidra zip SHA-256 and zlib URL_HASH in `docs/environment.md` and `CMakeLists.txt:6-11`.
