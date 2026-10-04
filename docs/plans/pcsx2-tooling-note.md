# PCSX2 tooling: stock vs modified build (2026-10-04)

Question from the owner: is the normal PCSX2 build enough, or should
we try an experimental/modified build (it is open source)? Researched
2026-10-04 against the official docs and one third-party project.
Verdict first, evidence below.

## Verdict

**Stock build for the joint session; modified build only on demand.**
Nothing in S0–S3 needs code changes to the emulator, and building (or
adopting) a fork has real costs. Revisit only when a concrete question
hits a wall the stock tools cannot cross.

## What the stock build already gives (Confirmed, official docs)

- GUI debugger (`Debug -> Open Debugger`, needs advanced settings):
  R5900 + R3000 layouts, disassembly, function stubbing, register
  view AND modify (GPR/CP0/FPR/FCR/VU0/GS), memory search, symbol
  import (ELF symtab, MIPS mdebug, `.sym` text files), breakpoint
  dialogs with conditions/logging/hit limits, expressions with
  register and memory reads. Sources: `pcsx2.net/docs/advanced/debugger`
  (+ the breakpoint-dialog behavior in GPT_FEEDBACK §17).
- CLI automation for repeatable launches: `-nogui` (implies `-batch`),
  `-statefile`, `-slowboot`/`-fastboot`, `-debugger` (break on entry),
  `-elf`, `-logfile`. Source: `pcsx2.net/docs/advanced/cli`.
- PINE (TCP 28011): memory R/W, status/identification, save/load
  states. No pause/step/breakpoint/registers — confirmed limit.

That covers the whole playbook: S0 (RAM+regs+hash), S1 (staged
breaks/reads), S2 (watchpoint + v0 read), S3 (entry breaks with ra
logging). The known weak spots (read-watchpoints unreliable in some
paths, memcheck limits under interpreter/HLE) are handled by our
standing rule: positive control first, always.

## What a modified build would add (and cost)

- A third party already did it: hkmodd/PCSX2-MCP (29 stars) injects a
  DebugServer (TCP 21512) into PCSX2 plus a Node MCP bridge: pause,
  resume, step, conditional breakpoints, watchpoints, full register
  R/W, thread lists, backtraces, IOP module lists — scriptable, even
  agent-drivable. Pre-built release exists.
- Costs/risks: third-party trust; version skew vs our 2.9.94 setup
  (savestate compat untested); GUI still needed to boot/play;
  maintaining a fork (or even just tracking one) is ongoing work.
- If we ever need SOURCE-level instrumentation (SIF packet logging at
  volume, DMA writer attribution, scripted bisection), the right move
  is a minimal patch on upstream PCSX2 built once (VS2022 + Qt + deps,
  hours the first time) and kept as external tooling under `private/`
  — never vendored into this repo (GPL-3.0 only matters on
  distribution; local observation use is fine).

## Triggers to revisit (any one suffices)

1. A shopping-list item provably needs scripted breakpoints/regs and
   the GUI session cannot deliver it (e.g. timing-sensitive capture).
2. A question needs volume logging SIF/DMA that PINE cannot produce.
3. The joint session shows the stock debugger missing a specific hit
   (record which control failed first).

Until then: stock, interactive, calibrated.
