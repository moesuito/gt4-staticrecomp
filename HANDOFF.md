# HANDOFF — continuing GT4Recomp on the secondary machine

> ARCHIVAL (2026-10-08): this file describes a machine move at M29
> (25/25 CTest). Do not use it for current state or model switching.
> Current resumption guide: **`docs/RETOMADA.md`** (slice100, 2026-10-09),
> alongside `docs/STATUS.md` and `AGENTS.md`. `PLAN.md` is strategic context.
> Kept for its historical copy-folder inventory (§2) only; the versions,
> deletion/rebuild recipe and next-M30 work below are not current instructions.

> **Resumo rápido (para o dono)**
> - Esta pasta foi feita para ser **copiada inteira** (não clonada): ela já
>   contém o jogo (ISO + CORE.GT4 + ELF reconstruído), o Ghidra + JDK 21, a
>   venv Python e o histórico do Git com os dois remotes. São ~7,4 GB.
> - Na máquina nova, **instale apenas**: Visual Studio 2022 Build Tools (com a
>   carga de trabalho C++), CMake, Ninja, Git e Python 3.14 em `C:\Python314`
>   (ou recrie a venv — comando abaixo). O resto está dentro da pasta.
> - O primeiro passo é **recriar a pasta `build/`** (a atual tem caminhos
>   absolutos desta máquina). Use o zlib offline que eu deixei em
>   `private/tooling/downloads/` — comando exato abaixo.
> - Próximo trabalho técnico (M30): compilação completa do módulo do jogo
>   inteiro (146 MB) ou dividir/streamar antes; depois o motorista e os
>   serviços de BIOS. Ver `docs/STATUS.md`.

---

## 1. Start here (agent)

Read, in this order:

1. `AGENTS.md` — auto-loaded working agreement (mission, autonomy, evidence
   discipline, documentation rules, git workflow, communication with the
   owner in Portuguese).
2. `docs/STATUS.md` — live state, environment, open items, next actions.
3. `docs/journal/2026-10-02.md` — the session log through M29.
4. `docs/requirements.md` — the milestone table M0–M29 with acceptance
   evidence.
5. This file — what is inside the folder, what to install, first-run steps.

State at handoff: `main` green (25/25 CTest, Python 73 collected / 67 run / 6
skip) and pushed to `origin`; check `git log -1` — the handoff commits sit
after `20ba459`.

## 2. What is already inside the copied folder (no installation needed)

| Path | What it is |
| --- | --- |
| `.git` | Full history, remotes `origin` (fork, push) and `upstream` (read-only reference). Pushing needs Git + GitHub credentials. |
| `Gran Turismo 4 (USA) (v2.00).iso` | The pinned game disc (4.95 GB). Input for disc verification. |
| `private/fingerprint-check/CORE.GT4` | The pinned game executable (2,020,861 bytes). Every translation test uses it. |
| `private/reconstructed/SCUS_973.28.elf` | The analysis ELF (M3 reference). Some Python tests use it. |
| `private/tooling/ghidra_12.1.3_PUBLIC` + `private/tooling/jdk-21` | Ghidra and its JDK, bundled and used with `$env:JAVA_HOME` (no system install). |
| `private/tooling/downloads/` | Installer caches: Ghidra zip, Temurin JDK zip, and **`zlib-1.3.1.tar.gz`** (hash-pinned, for the offline first CMake configure). |
| `private/tooling-venv` | Python venv with `pycdlib 1.20.0` and `zstandard 0.25.0`. Works if base Python 3.14.2 is at `C:\Python314`; otherwise recreate (section 4). |
| `private/pcsx2/` | Live-RAM dumps from M14; the **PCSX2 savestates** (`sstates/`: slot 9 = the PINE/menu state, slot 1 = the owner's, plus a backup); the **BIOS dumps** (`bios/`, 32 MB) and the **PINE-enabled config** (`pcsx2-config/PCSX2.ini`) so live observation only needs the emulator install. |
| `private/disassembly/` | Ghidra listings, comparison TSVs and logs from the M6/M16 verification runs (evidence). |
| `generated/whole-program.hpp` | The M29 whole-program module: 15,068 functions, 924,991 instructions, 146.4 MB. Regenerable; ignored by git. |
| `build/` | The previous build tree (0.34 GB). **Recreate it** (section 4): it contains absolute paths from this machine. |

Repository total: **7.54 GB** (with hidden files, including the savestates,
BIOS dumps and the generated whole-program module + its measurement object).

## 3. External tools to install on the new machine

| Tool | Version observed here | Why / notes |
| --- | --- | --- |
| Windows 10/11 x64 | — | The project targets MSVC x64 + Ninja. |
| **Visual Studio 2022 Build Tools** | 17.14; `cl` 19.44.35222 (x64) | Install the **"Desktop development with C++"** workload (MSVC v143 + Windows 10/11 SDK). Default path used by every command: `C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools`. VS Community/Professional with the same workload also works — adjust the `Launch-VsDevShell.ps1` path. |
| CMake | 4.3.1 (project needs ≥ 3.24) | On `PATH`. |
| Ninja | 1.13.2 | On `PATH`. |
| Git | 2.53.0.windows.1 | For the repo; sign in to GitHub (`moesuito`) to push. |
| Python 3.14 | 3.14.2 at `C:\Python314` | Only for the Python suite and the copied venv. If installed elsewhere, recreate the venv (section 4). |
| *(optional)* PCSX2 | 2.9.93 (this machine: `F:\Games\PS2`) | **Not needed for the first M30 slices** (the driver work verifies against the interpreter). It becomes useful for the BIOS-services layer: a live comparison oracle through PINE (port 28011) and new savestates deeper into the boot. The savestates, **the BIOS dumps** and **the PINE-enabled config** are already copied into `private/pcsx2/` (see the note below); only the emulator install itself would be external. |

Nothing else: Java is not needed system-wide (JDK 21 is bundled), Ghidra is
bundled, ImHex/LLVM are not used.

**PCSX2 setup (only if/when live observation is needed).** Install nightly
**2.9.93** (same version, so the copied savestates load) anywhere, then point
it at the copied data:

- BIOS: `private/pcsx2/bios/` (the set in use is
  `SCPH-90001_BIOS_V18_USA_230.ROM0`).
- Config: `private/pcsx2/pcsx2-config/PCSX2.ini` is the PINE-enabled config
  (`EnablePINE = true`, `PINESlot = 28011`); adjust the `Bios =` path to the
  copied folder.
- Game: the ISO already in the repo folder.
- Savestates: `private/pcsx2/sstates/` (slot 9 = PINE/menu, slot 1 = owner's)
  can be placed in the emulator's sstates folder to resume that moment.

## 4. First-run checklist (on the new machine)

Open a **VS Developer PowerShell** (Start menu → "Developer PowerShell for VS
2022"), `cd` to the copied folder, then:

```powershell
# 1. Sanity: the repo is intact and clean at the handoff commit.
git status --short
git log --oneline -1        # expect the handoff commit or later

# 2. Recreate the build (the copied one has absolute paths from the old machine).
#    CMake wants forward slashes in the archive path; a backslash path breaks
#    its string parsing (verified here).
Remove-Item -Recurse -Force build
$zlib = ((Get-Location).Path + '/private/tooling/downloads/zlib-1.3.1.tar.gz').Replace('\','/')
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug -DCMAKE_CXX_COMPILER=cl `
      -DGT4_ZLIB_ARCHIVE="$zlib"
cmake --build build
ctest --test-dir build --output-on-failure        # expect 25/25 passed

# 3. Python suite (uses the copied venv).
private\tooling-venv\Scripts\python.exe -m unittest discover -s tests/python
# expect: Ran 73 tests ... OK (skipped=6)
```

If the venv does not start (base Python path differs):

```powershell
# Install Python 3.14 to C:\Python314 first, or use the base python you have:
<base-python>\python.exe -m venv private\tooling-venv
private\tooling-venv\Scripts\python.exe -m pip install pycdlib==1.20.0 zstandard==0.25.0
```

The zlib archive is hash-pinned by CMake (`SHA256=9a93b2b7…72df23`); with
internet the `-DGT4_ZLIB_ARCHIVE` flag can be omitted (it defaults to the
zlib.net URL). This whole checklist was verified from a clean build directory
here: configure + build + 25/25 CTest in about 17 seconds.

Optional — Ghidra verification (fully relative to the repo root):

```powershell
New-Item -ItemType Directory -Force (Join-Path $env:TEMP 'GT4Recomp-M6') | Out-Null
$env:JAVA_HOME = "$PWD/private/tooling/jdk-21"
& "$PWD/private/tooling/ghidra_12.1.3_PUBLIC/support/analyzeHeadless.bat" `
    (Join-Path $env:TEMP 'GT4Recomp-M6') Disassembly `
    -import "$PWD/private/reconstructed/SCUS_973.28.elf" -overwrite `
    -processor 'MIPS:LE:64:64-32addr' -noanalysis `
    -scriptPath "$PWD/scripts/ghidra" `
    -postScript CompareDisassembly.java "$PWD/private/disassembly/listing.txt" `
        "$PWD/private/disassembly/comparison.tsv" `
    -log "$PWD/private/disassembly/ghidra.log"
```

## 5. Where the project stands (M0–M29) and the next work

- The decoder covers **349 operations**; the only unmodeled words in the real
  code region are 2 DMA-dependent `bc0f` and 2 unassigned encodings in the
  exception handler (the text's trailing 700 words are a data table).
- The interpreter runs the game's real startup (942,695 instructions) and
  every translation is verified differentially against it; the largest
  verified module has 57 functions / 2,588 instructions.
- The translator handles **99.5% of the 15,067 direct-call targets**; runtime
  targets dispatch through each module's own entry table; everything else
  stops at an explicit boundary (never a guess).
- **`--all` generates the whole game as one module** (15,068 functions,
  924,991 instructions, 146.4 MB) in ~136 s; an MSVC **syntax check passes in
  27.5 s**.
- **Next (M30)**, in priority order:
  1. **The driver**: a runner that executes a translated module as a program
     and resolves the boundaries it stops at. First verifiable slice: run the
     startup (or the whole-program module) from the ELF entry through the
     driver to the first syscall, with the state identical to the
     interpreter's at the same stop.
  2. **BIOS services**: implement the syscall handler layer so the game gets
     past its first service call (start with the services the startup uses).
  3. **Jump-table dispatch** (computed `jr` into local blocks) and the other
     boundary kinds; VCALLMS stays out of scope until a VU0 micro interpreter
     exists.
  4. Lessons M9–M29 are still pending (`docs/lessons/`).

## 6. Rules and gotchas

- **Never commit** anything from `private/`, `generated/`, `build/` or the
  ISO — they are gitignored; check `git status --short` before every commit.
- `upstream` is a read-only reference: never push or open PRs there.
- `build/` must be recreated after moving the folder (absolute paths).
- The whole-program module is derived from game code: keep it in
  `generated/` and never commit it.
- Resource guidance for the heavy compile (all measured on this machine):
  the whole-program module is a 146 MB single TU; a Debug `/Od` compile with
  the dispatch referenced emits everything in **~39 s at 0.53 GB peak RAM**
  (96.7 MB object, 45,345 sections). 32 GB is comfortable; a Release `/O2`
  build is unmeasured and will be heavier.
- If the machine's Windows SDK/MSVC differ, keep the build **warning-free**:
  the project treats warnings as signals, not noise.
