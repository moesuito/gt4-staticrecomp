# Project status

Updated 2026-10-02 after M30 slice 2 — the BIOS service layer and the
interpreter bridge: the whole game runs as one translated module from the ELF
entry through SetupThread and SetupHeap with the state identical to the
interpreter, stopping at the thread/semaphore scheduler. This is the first
document to read in a new session; it is kept current as work proceeds.
Details live in the linked evidence documents.

## Where we are

- Target: Gran Turismo 4 (USA) v2.00, serial SCUS-97328, pinned in
  `docs/inputs/usa-v2.00.json`; the local ISO matches the manifest.
- Curriculum and acceptance table: `docs/requirements.md`.
- M0-M6 BUILD/VERIFY complete:
  - M0 core/CLI/CMake; M2 disc verification; M3 reference ELF (upstream run);
    M4 native image and analysis ELF (byte-identical to the pinned hash here);
    M5 decoder; M6 disassembler.
  - The decoder covers 349 operations (line-filtered count; earlier documents
    cited 175, which counted comment fragments). Whole-text scan, corrected
    after M24: 497 unsupported of 1,334,917 words — 493 of them inside the
    700-word **data table** that occupies the text section's last bytes
    (0x616F28..0x617A14) and only **4 real code words**: two BC0F whose
    condition needs a DMA model and two words at unassigned function 0x28
    inside the exception handler; the first 350,000 words — every sampled
    region — decode cleanly. (An
    earlier "zero unsupported" claim was a false positive from chunk ranges
    beyond the text end; the ERET sighting exposed it. Lesson: check the
    tool's exit status, not only its output.) Ghidra verification: the M6
    ten-region run matched 417 with 0 mismatches; the M16 listing (startup
    regions, candidates and the unaligned-access neighborhoods) matched 594
    with 0 mismatches and 34 R5900-only rows verified against the reference
    tables instead (`docs/reverse-engineering/m6-disassembly.md`,
    `docs/reverse-engineering/m16-unaligned-and-multiply.md`).
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
  image, and the continuation. Translator slices 2 and 3 followed: branches
  (likely/link), loops, multiple returns (verified on `0x005c11a8`), and
  direct call trees translated into one module (verified as a 5-function
  chain, `0x0010c0c0`, on 6 states). Slice 4 followed: six observed integer
  operations (LB/LBU/SRA/SLTI/SLTIU/XORI) with Ghidra-verified decoding
  (extended run: 475 matched, 0 mismatches), the SLT/SLTU/SLTI/SLTIU
  comparison semantics corrected to 64-bit (shared-speculation bug found by
  cross-checking PCSX2), and a fourth function verified (`0x00549378`)
  (`docs/reverse-engineering/m13-first-function.md`).
- M15 COP1/MMI: 35 FPU and 73 MMI operations, LQ/SQ, the HI/LO second bank
  and the MTSA shift cache decode and execute (PCSX2 is the semantic
  reference). The interpreter runs the game's real startup: 942,695
  instructions from the ELF entry (0x00100008) to the first BIOS syscall
  (0x001001c8), clearing the .bss window — asserted by the new
  `ee_startup_run` CTest with a junk pre-fill. Bit-for-bit agreement with
  Ghidra on the base language; the 34 extension rows are verified against
  the reference tables (`docs/reverse-engineering/m15-cop1-mmi.md`).
- M15 slice 2: `gt4translate` emits the prologue's operations plus LQ/SQ and
  supports a halt address; the startup now also runs as a 112-instruction
  native module whose final state matches the interpreter exactly after
  942,695 instructions (`ee_translation_startup` CTest). MOVZ/MOVN added from
  observed use.
- M16 unaligned access and multiply/divide: LWL/LWR/SWL/SWR (reference
  mask/shift tables; LWR's nonzero shift keeps the register's upper half via
  the new `write_gpr_low32`), LWU/LHU/SH, MULT/MULTU/DIV/DIVU with the quirk
  cases, MADD/MADDU, the compact second-bank forms and PLZCW — 188 operations
  total (corrected count). Hand-computed fixtures cover all four alignments,
  the load pair, divide-by-zero signalling and the second bank; Ghidra
  matched=594, mismatched=0. Fifth verified function: 0x00572438 (a
  plzcw/movn/movz bit-count helper) translates and matches the interpreter on
  6 states (`ee_translation_572438`). Gaps recorded: 0x579780 (break in a
  likely delay slot), 0x58ce48's tree (transfer below the entry at 0x5b27f8),
  the MMI parallel multiply family, BREAK and COP0
  (`docs/reverse-engineering/m16-unaligned-and-multiply.md`).
- M17 tail thunks and syscall boundaries: the translator walks functions
  entered above their own transfers (scan from the lowest reachable address,
  a goto to the entry, empty runs collapsed into one comment) and turns
  syscalls found by the walk into stop points whose halt propagates through
  call sites (`if (pc != link) return;`). CACHE decodes as a no-op hint (189
  operations). New `ee_translation_thunk`: the tail thunk 0x005b27f8 matches
  the interpreter on 5 states, both stopping at service 0x42. The 0x58ce48
  call tree advanced through movn, lwl, the thunk and cache; its next gap is
  a branch targeting a delay slot at 0x005b0fcc
  (`docs/reverse-engineering/m17-thunks-and-syscall-boundaries.md`).
- M18 critical edges: a delay slot that is also a transfer target now gets a
  standalone copy at its own address (the edge path falls through); the
  transfer jumps past it on the normal path, and branch-decision variables
  are hoisted to the top of the generated function (forward gotos may not
  skip initializations; MSVC C2362). The cache-flush loop 0x005b0f78 (43
  instructions, the critical edge in its middle) matches the interpreter on 7
  states, all registers and the continuation compared
  (`ee_translation_cacheflush`). The 0x58ce48 call tree now stops at COP0
  `mfc0` (Status) in 0x005b72f8
  (`docs/reverse-engineering/m18-critical-edges.md`).
- M19 COP0/BREAK/shifts: the CP0 register file joins the state (from the
  live-observed Status 0x40000000): `mfc0`/`mtc0` with the reference's masks
  and protections, gated `ei`/`di` toggling Status.EIE, `break` trapping like
  a syscall (an automatic halt in translated code), and the 12-operation
  64-bit/variable shift family (206 operations). Hand-computed fixtures cover
  the masks, the supervisor-mode gate, Config protection and the shift sign
  fills; the translator emits the CP0 operations with mirroring helpers. The
  CLI tests moved their unsupported-word example to `ldl` at 0x001041f4 (a
  scan found 655 unsupported words in the first 200k, concentrated in the
  unaligned 64-bit family). Next recorded gaps: the `beql …; break` trap in a
  likely delay slot (0x005baea4) and `ldl`/`ldr`/`sdl`/`sdr`
  (`docs/reverse-engineering/m19-cop0-and-shifts.md`).
- M20 complete decode: LDL/LDR/SDL/SDR with the reference merge tables (the
  655-word blocker), BLEZL/BGTZL, DADDIU, NOR and the PREF hint close every
  remaining decode gap (215 operations). A scan of the whole file-backed text
  (1,334,917 words, four chunks) reports zero unsupported words: every word
  of the game's executable image is now named and classified (decode
  coverage; the hints are documented no-ops). The first-200k progression:
  655 → 63 → 10 → 0. Hand-computed doubleword fixtures caught my own
  arithmetic slips in expected values — the implementation matched the
  reference formulas. The CLI tests were repurposed (no decode-level
  unsupported example exists any more). The 0x58ce48 tree's only remaining
  block is the `beql …; break` trap in a likely delay slot
  (`docs/reverse-engineering/m20-fully-decoding-text.md`).
- M21 trap slots and the largest module: the `beql …; break` idiom is
  represented end to end (flow flag, interpreter Exception stop when taken /
  slot skip when not, translator `if (taken) { set_pc(slot); return; }`);
  `eret` executes with the CP0-derived target and level clear and is a
  derived-pc boundary in translated code; translator emission gaps closed
  (lhu/lwu/sh, dsubu, mult/multu/div/divu + second bank via mirroring
  helpers, mfhi/mflo/mfhi1/mflo1) — 217 operations. **Two new verified
  modules: 0x00579780 (66 instructions, 6 states) and 0x0058ce48 — 57
  functions, 2,588 instructions, the largest verified translation so far**
  (4 states, lazy-initializer runs stopping at the first BIOS service).
  The whole-text scan correction is recorded above and in the M20 doc
  (`docs/reverse-engineering/m21-trap-slots-and-the-largest-module.md`).
- M22 part 1 (2026-10-02): VU0 macro-mode state — 32 vector registers of four
  32-bit lanes with register 0 hardwired to the constant (0, 0, 0, 1.0), the
  integer file with VI0 zero, and the clip flag — plus the moves and memory:
  QMFC2/QMTC2 (the full 128 bits through both GPR halves), CFC2/CTC2 with the
  reference's exact special cases (the reciprocal register's mantissa mask
  and constant exponent, read-only MAC_FLAG/TPC/VPU_STAT, the FBRST mask
  whose VU0-reset clears the file, CLIP_FLAG's double write, context stops
  for the VU1 controls), LQC2/SQC2 (16-byte alignment, constant-register
  loads access and discard) and VNOP (the most frequent macro word). 224
  operations; unsupported words over the whole text: 2,269 → **1,426**. The
  macro dispatch is decoded for the next slice: functions 0x00-0x3B through
  the standard table, 0x3C-0x3F through the packed `(word & 3) | ((word >> 4)
  & 0x7C)` index; VMULAx/y/z/w and VNOP dominate the observed families
  (`docs/reverse-engineering/m22-vu0-macro-moves.md`).
- M23 (2026-10-02): the **full VU0 macro instruction set** — 111 new
  operations (335 total) filling both dispatch tables: the float model
  (denormal flush, overflow clamp at the reference's default settings), the
  MAC/status flag registers with their exact syncs, the element/broadcast/
  accumulator arithmetic (VADD/VSUB/VMUL/VMADD/VMSUB in every variant),
  VMAX/VMINI by integer representation, VOPMULA/VOPMSUB, the conversions,
  VCLIPw, VMOVE/VMR32, the division unit with Q publication, VMTIR/VMFIR,
  the random generator and the 16-bit integer forms. VCALLMS/VCALLMSR and
  the VU0-memory forms stay Unsupported with context. The unsupported word
  count drops from 1,426 to 672, and the scan exposed that the text
  section's last **700 words (0x616F28..0x617A14) are a data table** — not
  code; excluding it, the real code region holds only **81 unsupported
  words** in known families. CTest 23/23; Python 71
  (`docs/reverse-engineering/m23-vu0-macro-arithmetic.md`).
- M24 (2026-10-02): the **trapping arithmetic** (ADD/SUB/DADD/DSUB/ADDI/
  DADDI) with the reference's exact overflow checks, stopping with the stable
  Exception outcome at the offending word when they fire, and **eight
  parallel multiply/divide operations** (PMADDH/PMSUBH/PMULTH over the eight
  halfword lanes; PMULTW/PMULTUW/PMADDUW/PDIVW/PDIVUW over the HI/LO pair,
  special cases included) — 349 operations. Whole-file unsupported words:
  672 → **497**; the real code region (0x00100000..0x616F1C) now holds only
  **4 words**: two BC0F that need a DMA model for their condition, and two
  words at unassigned function 0x28 in the exception handler. CTest 23/23;
  Python 71 (`docs/reverse-engineering/m24-trapping-and-parallel-multiply.md`).
- M25 (2026-10-02): `ee::execute_plain_effect` exposes the verified executor
  and the translator falls back to it for every decoded plain operation
  without an inline form — the whole VU0 macro table, the COP2 moves and
  quad accesses, the remaining MMI forms and the trapping arithmetic (whose
  statement stops the module at the instruction's address when the overflow
  fires). **A new verified module, 0x0056DF58 (133 instructions, 32
  runtime-executed)**, a vector convert/scale loop, matches the interpreter
  on 3 input states with all registers, both HI/LO banks, the pc and the
  whole scratch window compared. CTest 24/24; Python 71
  (`docs/reverse-engineering/m25-translator-runtime-fallback.md`).
- M26 (2026-10-02): `gt4translate --survey` walks every direct-call target in
  the text with the standard call-tree validation. **9,345 of 15,067 entries
  translate (62%)**, their trees covering 399,046 instructions (~30% of the
  real code region); **93% of the rejections are indirect control flow**
  (4,576 `jalr`, 1,055 computed jumps), the rest non-code `jal` targets and
  validation edges. Instruction coverage is effectively complete; control-flow
  structure is the limiting factor. CTest 24/24; Python 72
  (`docs/reverse-engineering/m26-translation-survey.md`).
- M27 (2026-10-02): **indirect control flow became a boundary** — `jalr` and
  computed `jr` stop the module at the transfer exactly where the interpreter
  stops (delay slot included, link register untouched); unmodeled words
  (VCALLMS, the unassigned encodings) stop at the word; an unmodeled delay
  slot stops at the slot after the transfer's state effects. A
  boundary-at-entry function translates as a stopping stub. **99.1% of the
  direct-call targets translate (14,938 of 15,067); covered instructions rose
  from 399,046 to 858,621 (64.3%)**; all remaining rejections are module-size
  policy. New verified module 0x00101C28 (a `jalr` trampoline, three target
  values). CTest 25/25; Python 72
  (`docs/reverse-engineering/m27-indirect-flow-boundaries.md`).
- M28 (2026-10-02): **the module dispatches its own indirect targets** — a
  per-module entry table (`has_entry`/`call_entry`) backs `jalr` (target read
  before the link to rd, delay slot, dispatch, boundary propagation or inline
  continuation) and computed `jr`; an unknown target keeps the M27 boundary
  stop before the link or delay slot. Covered instructions rose from 858,621
  to 864,507. The 0x00101C28 test verifies the known-target dispatch and the
  unknown-target boundary. CTest 25/25; Python 72
  (`docs/reverse-engineering/m28-module-dispatch.md`).
- M29 (2026-10-02): **module-size policy and the whole-program build** —
  `--functions N` and `--all` (every direct-call target plus the ELF entry as
  one module); direct calls leaving the text stop as boundaries; the COP1
  branch conditions joined the emitter. **The whole game generates: 15,068
  functions, 924,991 instructions, 146.4 MB in 136 s, and passes an MSVC
  syntax check in 27.5 s.** A full Debug compile of the whole module (dispatch
  referenced, `/bigobj`) takes **38.9 s at 0.53 GB peak RAM** (96.7 MB
  object). The survey with the policy lifted translates
  14,991 of 15,067 entries (99.5%), covering 871,317 instructions (65.3%);
  the remaining rejections are the survey's own per-tree budget. CTest 25/25;
  Python 73 (`docs/reverse-engineering/m29-whole-program-build.md`).
- M30 slice 1 (2026-10-02): **the boundary driver** — `ee::Driver` executes a
  translated module as a program and classifies where it stops from the guest
  state (syscall with the service in v1, break, eret, unknown indirect
  target, unsupported word, jr-ra return, trapping stop, unmapped pc); the
  translated startup runs through the driver from the ELF entry to the first
  BIOS syscall (0x001001C8, service 0x3C = SetupThread) with the full final
  state identical to the interpreter after 942,695 instructions. `gt4run` is
  the driver as a program. CTest 27/27; Python 73 (67 run, 6 skip)
  (`docs/reverse-engineering/m30-driver-first-slice.md`).
- M30 slice 2 (2026-10-02): **the BIOS service layer and the interpreter
  bridge** — `ServiceTable` maps the number in v1 to a handler (SetupThread
  0x3C returns the stack pointer from the public ps2sdk ABI, SetupHeap 0x3D
  validates the heap request, FlushCache 0x64 is the documented no-op). When
  the module stops at a jr-ra return, an unknown indirect transfer or an eret,
  the step-by-step interpreter continues to the next module entry (the
  reference every module was verified against); a syscall with a handler runs
  inline; everything else is the reported boundary. The generated modules
  expose their entry table publicly (`translated::has_entry`/`call_entry`).
  **`gt4boot` runs the whole game as one module from the ELF entry through
  SetupThread and SetupHeap with the full state identical to the interpreter
  after 942,726 instructions — all registers, FPU, VU0, CP0, pc and a digest
  of the full 32 MiB of RAM — stopping at the next wall: CreateSema (0x40)
  in the InitThread tree at 0x005ADCA4.** The whole-program module builds on
  demand (140 s, 84.5 MB). CTest 29/29 (`ee_driver`, `gt4boot_build`
  fixture, `gt4boot_services`); Python 73 (67 run, 6 skip)
  (`docs/reverse-engineering/m30-bios-services-and-bridge.md`,
  `docs/decisions/0004-driver-boundary-classification.md`).
- M14 (2026-10-01): live observation through PCSX2 PINE — the reconstructed
  text image matches live GT4 RAM byte-for-byte (5,339,668 bytes, equal
  hashes), reginfo 24/24; data-record differences are runtime writes. Slice 2
  decodes the menu savestate offline: pc, all 32 GPRs, HI/LO and key CP0
  registers (the savestate's own eeMemory re-verifies the text image with 0
  differences). Savestate anchors: PINE slot 9 and the owner's slot 1
  (`docs/reverse-engineering/m14-live-observation.md`).
- EXPLAIN: lessons written for M6, M7 and M8 (`docs/lessons/`); the M9-M30
  lessons and retroactive M2-M5 notes remain open.
- Next technical milestone work: **the EE thread and semaphore scheduler** —
  the boot stops at CreateSema (0x40) in the InitThread tree, and the public
  ps2sdk `InitThread` shows the shape (CreateThread/StartThread/
  ChangeThreadPriority/GetThreadId plus a WaitSema loop), so the next unit is
  a deterministic cooperative scheduler; then the kernel-patch services
  (Copy/FindAddress/SetSyscall) and the remaining boundaries.

## Environment (this machine, `C:\Antigravity\gt4-staticrecomp`)

- Build: VS 2022 Build Tools 17.14 + MSVC 19.44 + Ninja 1.13.2 + CMake 4.3.1;
  commands in `AGENTS.md` and `README.md`.
- Tests: 29/29 CTest (the translation tests, `gt4run` and `gt4boot` exist
  only where the local CORE does; `gt4boot_build` builds the whole-program
  module on demand, 140 s); Python suite 73 collected (67 run, 6 skip without
  the M3 reference ELF; the savestate test finds the repository copy first).
- Local inputs (ignored): ISO at the repository root;
  `private/fingerprint-check/CORE.GT4` (2,020,861 bytes, hash matches the
  pinned manifest); `private/reconstructed/SCUS_973.28.elf` (6,123,004 bytes,
  SHA-256 equals the pinned native ELF).
- Tooling venv: `private/tooling-venv` (pycdlib 1.20.0).
- Ghidra 12.1.3 + Temurin JDK 21.0.12.1+1 under `private/tooling/`; hashes and
  provenance in `docs/environment.md`.
- Disposable Ghidra project directory: `%TEMP%\GT4Recomp-M7`.
- PCSX2 nightly 2.9.93 at `F:\Games\PS2` on the original machine; the owner
  added a pre-configured **PCSX2 v2.9.94 under `private/pcsx2/`** (with its
  `pcsx2-config` and BIOS), all ignored by `/private/`. PINE stays on port
  28011 (`EnablePINE = true`; the original ini is kept as
  `.bak-gt4recomp`). Savestates: the copies that travel in
  `private/pcsx2/sstates/` (slot 9 = PINE/menu, ours; slot 1 = owner) and the
  live `Documents/PCSX2/sstates`; `scripts/pcsx2_savestate.py` decodes their
  CPU state offline.
- Live RAM dump (ignored): `private/pcsx2/text-ram.bin` and
  `private/pcsx2/menu-eeMemory.bin`; distributable metadata in
  `docs/inputs/usa-v2.00-live-ram.json`.

## Open items

- The M3 reference ELF (PDTools GT4ElfBuilderTool, hash-pinned in
  `docs/inputs/usa-v2.00-reference.json`) is not regenerated here, so 6
  optional native CLI tests skip. Rebuilding it is an optional future task.
- Retroactive lesson notes for M2-M5 are not written; the M9-M30 lessons are
  pending.
- Unmodeled words left in the real code region (4): two BC0F (their condition
  is the DMA-derived COP0 line) and two words at unassigned function 0x28
  inside the exception handler. The text section's trailing 700 words are a
  data table and are excluded from instruction counting. VCALLMS/VU0-memory
  forms stay out of scope by design (VU micro execution).
- Live single-stepping is unsolved (savestate parsing covers offline
  snapshots); the freeze layout is coupled to the emulator build.
- The driver's classification is an inference from the stop pc: a jr-ra
  return is recognized because pc equals ra (a trapping stop at that exact
  address would be misreported; none observed). The interpreter path uses the
  step outcome instead and is exact. Recorded in
  `docs/decisions/0004-driver-boundary-classification.md`.
- The interpreter bridge resolves boundaries by interpreting the gaps between
  module entries (correctness first; nine instructions in the boot run). The
  two performance alternatives (resume entries per halt address, inline
  syscall calls in generated code) remain open.
- **The boot now stops at the thread and semaphore scheduler**: CreateSema
  (0x40) at 0x005ADCA4, called from the InitThread-equivalent at 0x005B7310.
  A single-threaded model cannot answer WaitSema, so the next unit needs a
  deterministic cooperative scheduler; the public ps2sdk `thread.c` documents
  the shape. FlushCache is registered but was not reached before this stop.
- A module call runs to its own boundary and cannot be interrupted; the work
  budget counts interpreted instructions and module calls, so a loop inside a
  module is not bounded by it. No such loop has been hit before a boundary.

## Next actions

1. M30 slice 3: **the EE thread and semaphore scheduler** — CreateSema,
   CreateThread/StartThread/ExitThread, GetThreadId/ChangeThreadPriority,
   SignalSema/WaitSema/PollSema, modeled from the public ABI with a
   deterministic cooperative scheduler; the acceptance evidence is the same
   differential harness (`gt4boot --compare-interpreter`) extended past
   0x005ADCA4.
2. The kernel-patch services (Copy/FindAddress/SetSyscall) and what they mean
   when the model is the kernel.
3. Performance: resume entries or inline syscall calls to shrink the
   interpreted gaps; jump-table dispatch for computed `jr` into local blocks.
4. The M9-M30 lessons and retroactive M2-M5 notes if useful.
5. Keep the journal and this file current after every working session.

## Journal

- [2026-10-01](journal/2026-10-01.md) — fork setup, environment validation,
  decoder expansion, Ghidra verification, working rules, M6-M8 lessons, M7
  slices 1-2, M8 function map, M9 state model, M10 interpreter, M11/M12
  synthetic suites, M13 first natively compiled function, M14 live PCSX2
  observation and savestate register decoding, M15 COP1/MMI decoding and
  execution with the game's startup running in the interpreter and
  recompiled natively (verified identical after 942,695 instructions), M16
  unaligned access, multiply/divide and PLZCW with a fifth verified function,
  M17 tail thunks and syscall boundaries (the thunk 0x005b27f8 verified
  stopping at service 0x42) and the cache hint, M18 critical edges (a branch
  targeting a delay slot, verified with the 0x005b0f78 cache-flush loop),
  M19 COP0/BREAK and the 64-bit shift family, M20 the unaligned 64-bit
  family and the last decode gaps, M21 trap slots in likely delay slots,
  ERET and the largest verified module (57 functions, 2,588 instructions).
- [2026-10-02](journal/2026-10-02.md) — M20, M21, M22 (the VU0 macro state,
  its moves and the quad memory accesses), M23 (the full VU0 macro
  instruction set; the text's trailing data table discovered), M24 (the
  trapping arithmetic and the parallel multiply/divide family), M25 (the
  translator reaches the macro and trapping operations; the 0x0056df58
  module verified) and M26 (the whole-text translation survey: 62% of the
  entries translate; indirect control flow blocks the rest) and M27
  (indirect control flow became a boundary: 99.1% of the entries translate)
  and M28 (the module dispatches its own indirect targets) and M29 (the
  whole-program build: 15,068 functions, 924,991 instructions, MSVC
  syntax-checked), plus the scan correction trail. M30 followed the same day:
  slice 1 is the boundary driver (the translated startup reaches the first
  BIOS syscall with state identical to the interpreter; `gt4run` runs it as a
  program), and slice 2 is the BIOS service layer plus the interpreter bridge
  (`gt4boot` runs the whole game as one module through SetupThread and
  SetupHeap with state identical to the interpreter, stopping at CreateSema
  in the thread/semaphore init).
