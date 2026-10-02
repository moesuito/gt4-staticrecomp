# HANDOFF — continuing GT4Recomp on the secondary machine

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
| `private/pcsx2/` | Live-RAM dumps from M14 and, now, the **PCSX2 savestates** (`private/pcsx2/sstates/`: slot 9 = the PINE/menu state, slot 1 = the owner's, plus a backup) so the folder is self-contained for offline savestate decoding. |
| `private/disassembly/` | Ghidra listings, comparison TSVs and logs from the M6/M16 verification runs (evidence). |
| `generated/whole-program.hpp` | The M29 whole-program module: 15,068 functions, 924,991 instructions, 146.4 MB. Regenerable; ignored by git. |
| `build/` | The previous build tree (0.34 GB). **Recreate it** (section 4): it contains absolute paths from this machine. |

Repository total: **7.42 GB** (with hidden files, including the savestates).

## 3. External tools to install on the new machine

| Tool | Version observed here | Why / notes |
| --- | --- | --- |
| Windows 10/11 x64 | — | The project targets MSVC x64 + Ninja. |
| **Visual Studio 2022 Build Tools** | 17.14; `cl` 19.44.35222 (x64) | Install the **"Desktop development with C++"** workload (MSVC v143 + Windows 10/11 SDK). Default path used by every command: `C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools`. VS Community/Professional with the same workload also works — adjust the `Launch-VsDevShell.ps1` path. |
| CMake | 4.3.1 (project needs ≥ 3.24) | On `PATH`. |
| Ninja | 1.13.2 | On `PATH`. |
| Git | 2.53.0.windows.1 | For the repo; sign in to GitHub (`moesuito`) to push. |
| Python 3.14 | 3.14.2 at `C:\Python314` | Only for the Python suite and the copied venv. If installed elsewhere, recreate the venv (section 4). |
| *(optional)* PCSX2 | 2.9.93 (this machine: `F:\Games\PS2`) | Only for live-observation work (PINE on port 28011). The savestates are already copied into `private/pcsx2/sstates/`, so offline savestate decoding works without PCSX2; the emulator itself is only needed for new live captures. |

Nothing else: Java is not needed system-wide (JDK 21 is bundled), Ghidra is
bundled, ImHex/LLVM are not used.

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
  1. **Full code generation** of the whole-program module (or split/stream it
     first). Expect: `/bigobj` will likely be required (15k+ inline functions
     produce many COMDAT sections), many GB of RAM, a long single-TU compile;
     if it is impractical, add a split/stream mode to `gt4translate` (write
     chunks of functions to several headers) — that is a translator change,
     not a semantic one.
  2. Then the **driver and BIOS services**: turn the stopped boundaries
     (syscalls, jump-table dispatch, VCALLMS, the five out-of-text calls)
     into execution. See the M29 doc's "Reading the numbers" section.
  3. Lessons M9–M29 are still pending (`docs/lessons/`).

## 6. Rules and gotchas

- **Never commit** anything from `private/`, `generated/`, `build/` or the
  ISO — they are gitignored; check `git status --short` before every commit.
- `upstream` is a read-only reference: never push or open PRs there.
- `build/` must be recreated after moving the folder (absolute paths).
- The whole-program module is derived from game code: keep it in
  `generated/` and never commit it.
- Resource guidance for the heavy compile: 64 GB RAM and a fast NVMe make
  the difference; the generator itself builds the 146 MB text in memory
  (a few GB) and takes ~2.5 minutes.
- If the machine's Windows SDK/MSVC differ, keep the build **warning-free**:
  the project treats warnings as signals, not noise.
