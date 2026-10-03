# Project status

Updated 2026-10-03 after M32 slice 16 — every completion path mapped.
By code reading: the unlink is pure list surgery (no signal); four
unlink callers exist (walk dispatch — the only signaling context,
re-arm cancel, delete/cancel, update/refresh) and none of the six waits
suffered cancellation (nodes intact, workers still waiting); the
iSignalSema endpoint is reachable only via the walk's indirect dispatcher
call. So the due test plus walk dispatch is the only signaling path in
the codebase — the paradox is about its inputs/runs, not hidden
signalers. Next is slice 17: a temporary pc-triggered trace in the
reference interpreter at `0x005b822c` logging current, target, COUNT,
overflow, and head per walk test. This is the first document to read in
a new session; it is kept current as work proceeds. Details live in the
linked evidence documents.

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
- M30 slice 3 (2026-10-02): **the thread scheduler and the semaphore
  services** — `RegisterContext` save/restore (all per-thread register files;
  memory shared), the `Kernel` model with the deterministic cooperative
  scheduler of decision 0005 (priorities 0-127, highest first, creation-order
  ties, switches on blocking and on a strictly higher-priority thread
  becoming ready, no timer preemption), `ServiceOutcome` so a handler can
  report a context switch, and the full thread/semaphore service set (with
  the negative `i*` aliases). **The boot now handles SetupThread, SetupHeap
  and both CreateSema calls and stops at the kernel-patch wall: SetSyscall
  (0x74) at 0x005B7554, with the full state identical to the interpreter
  after 942,761 instructions.** The scheduler is unit-verified (`ee_kernel`,
  no game data); the fifth slice exercised it end to end on the game's own
  thread creation. CTest 32/32;
  Python 73 (67 run,
  6 skip) (`docs/reverse-engineering/m30-thread-scheduler.md`,
  `docs/decisions/0005-thread-scheduler.md`).
- M30 slice 4 (2026-10-02): **the kernel-patch services** — `SetSyscall`
  (0x74) records the patch and writes the guest handler into a synthetic
  syscall table at physical 0x1000; a patched syscall dispatches to the guest
  handler through a return stub (the model's private service 0x100) that
  restores the caller's ra and resume address, mirroring the kernel's EPC
  return; `GuestMemory` gains an opt-in KSEG0 alias (the SDK's search reads
  0x80000000); guest faults now name the pc. **The boot patches FindAddress
  (0x83 → 0x005B73C8) and Copy (0x5A → 0x005B7390), runs both searches
  through the game's own helper — which finds the installed values in the
  synthetic table and derives its base at 0x80001000 — and stops at the
  second stub return with the full state identical to the interpreter after
  954,146 instructions.** The next wall is the **EE timer hardware**:
  0x005B7A40 reads TIM3_MODE (0x10001810) and stops with the explicit fault;
  the timer/alarm subsystem needs its own decision. CTest 30/30; Python 73
  (67 run, 6 skip) (`docs/reverse-engineering/m30-kernel-patches.md`,
  `docs/decisions/0006-kernel-patches.md`).
- M30 slice 5 (2026-10-02): **timer registers, interrupt handlers — and a
  translator bug fixed**. An explicit MMIO device window plus a KSEG0/KSEG1
  segment alias carry `ee::TimerUnit` (the four timers' 32-bit registers;
  untouched reads as "not started"; no ticking and no interrupts, decision
  0007) and the kernel stores AddIntc/AddDmac handler registrations
  (0x10-0x17 and the `i*` aliases) with enable/disable accepted. The CP0
  Status baseline is corrected to the M14 live capture 0x70030c11 (IE/EIE
  set), which the SDK's own thread setup checks. **The wider run exposed a
  real translator bug: a `jr ra` followed by more code emitted no `return;`
  and fell through into the next block (DIntr 0x005B72A8 executed both
  return paths); the emitter now returns after every `jr ra`, with the
  differential regression `ee_translation_5b72a8` (two states).** The boot
  now runs the whole `_InitSys` tree and **the game's thread creation end to
  end** — CreateSema, CreateThread, ReferThreadStatus, StartThread,
  GetThreadId, ChangeThreadPriority, WaitSema — **the cooperative scheduler
  switches to the new thread and back**, and the run stops at
  GetOsdConfigParam (0x4B, pc 0x005ADD54) with the full state identical to the
  interpreter after 961,937 instructions. CTest 32/32; Python 73 (67 run,
  6 skip) (`docs/reverse-engineering/m30-timer-and-interrupts.md`,
  `docs/decisions/0007-timer-registers.md`).
- M30 slice 6 (2026-10-02): **the OSD configuration and the register-bank
  device model** — GetOsdConfigParam (0x4B) writes the ConfigParam word and
  SetOsdConfigParam (0x4A) stores it retaining every field, which is exactly
  what the SDK's early-kernel probe at 0x005B7620 tests (write version=1,
  read it back); the initial USA default is a documented model value
  (decision 0008). `GuestMemory` now maps **multiple device windows** and
  `RegisterBank` gives the DMAC (0x1000E000) and SIF0 CHCR (0x1000C000)
  blocks 32-bit storage; `TimerUnit` sits on the same bank. **The boot now
  passes the whole init chain and stops at SifSetDChain (0x78) at pc
  0x005AE084 — the IOP wall** (the public `sceSifInitCmd` shows the following
  CMDINIT wait would spin forever without an IOP model) — with the full state
  identical to the interpreter after 6,321,377 instructions. CTest 32/32;
  Python 73 (67 run, 6 skip)
  (`docs/reverse-engineering/m30-osd-and-the-iop-wall.md`,
  `docs/decisions/0008-osd-and-device-banks.md`).
- M30 slice 7 (2026-10-02): **the SIF layer, the model IOP and interrupt
  injection** — SifSetReg/GetReg (0x79/0x7A) over the SIF register block and
  the kernel's software system registers, SifSetDChain (0x78) writing SIF0's
  CHCR = 0x184, SifStopDma (0x6B), synchronous SifSetDma (0x77) with
  SifDmaStat (0x76) always done, a model IOP seeded as initialized whose
  stub answers the SIFCMD INIT_CMD with SET_SREG(RPCINIT), and the driver's
  **interrupt injection** (the kernel saves the interrupted context and
  installs the registered handler's frame; the handler returns through the
  model's stub). The uncached KUSEG mirror (0x20000000) joined the segment
  alias. **The boot completes the SIFCMD handshake, runs the game's own DMA
  handler as an injected interrupt for the first time, initializes RPC and
  stops at a clean `NoRunnableThread` boundary waiting for the IOP's RPC
  bind reply (pc 0x005ADCE4)** — the interpreter reference at 6,322,280
  instructions with the full state identical. CTest 32/32; Python 73
  (67 run, 6 skip)
  (`docs/reverse-engineering/m30-sif-and-interrupt-injection.md`,
  `docs/decisions/0009-sif-and-interrupt-injection.md`).
- M30 slice 8 (2026-10-02): **the model IOP's RPC layer, the peripheral
  windows and idle VBlank delivery** — RPC bind/call replies with the packet
  layouts confirmed against the live transfers, the version query answered
  with the game's own compatibility constant (0x00275520), the IOP reset
  completing the BOOTEND handshake, Get/SetOsdConfigParam2 (0x6E/0x6F) with
  the four-byte Config2Param, GsGetIMR/GsPutIMR (0x70/0x71), SetGsCrt (0x02),
  the EE scratchpad (0x70000000) and GS block (0x12000000, storage) as
  memory regions, the GIF/VIF/FIFO/IPU/DMA/INTC/SIO windows as storage
  banks, PCCR (CP0 25) as storage, the VU1 FBRST bits recorded instead of
  fatal, and the **idle VBlank source** (INTC cause 2 with the status bit,
  every registered handler chained in registration order, re-dispatch on
  return, a 60-interrupt budget). **The boot now reaches the game's running
  state: 3,000 services handled, the interpreter reference at 7,515,389
  instructions, full state identical.** CTest 32/32; Python 73 (67 run,
  6 skip)
  (`docs/reverse-engineering/m30-slice8-rpc-and-vblank.md`,
  `docs/decisions/0010-rpc-vblank-and-device-windows.md`).
- M30 slice 9 (2026-10-02): **timer ticks and DMA channel completions at
  idle** — enabled timers advance one frame of their clock source per idle
  interrupt, set the compare flag and raise their INTC cause (T0-T3 =
  9-12); the VIF0/VIF1/GIF DMA channels complete a started transfer at once
  (STR clears; TIE raises causes 4/5/9); the INTC and DMAC handler tables
  are separate and the model IOP's SIF replies dispatch through the DMAC
  channel 5 path with the DMAC status bit, matching `sceSifInitCmd`; the
  idle budget rises to 6,000. **The game's TIM2 handler now runs every idle
  frame and reprograms COMP; the differential passes at 3,000 services with
  the interpreter reference at 7,508,945 instructions and the full state
  identical.** The open frontier is the game's delay/software-timer callback
  chain. CTest 32/32; Python 73 (67 run, 6 skip)
  (`docs/reverse-engineering/m30-slice9-timer-and-dma-completions.md`,
  `docs/decisions/0011-timer-ticks-and-dma-completions.md`).
- M30 slice 10 (2026-10-02): **semaphore handle bits and the delay library**
  — the delay helper's callback path was traced end to end (0x005AED18 →
  0x005B8F38 → 0x005B8C60 → 0x005B8B68 → the dispatcher 0x005B8ED8 →
  0x005AEF58 `iSignalSema`); the library ORs 2 into the semaphore handle and
  tests its bit 0, so the model hands out ids **3, 7, 11, ...** (decision
  0012). **The long run advances from 3,645 to 9,765 services (672,586
  interpreted steps)** with the differential passing at 3,000 services
  (interpreter 7,508,945 instructions, state identical). The remaining
  frontier is the timer library's node processing: at the stop the active
  list holds two nodes whose descriptor handler fields are 5 and 7, and the
  delay descriptors are not active. CTest 32/32; Python 73 (67 run, 6 skip)
  (`docs/reverse-engineering/m30-slice10-semaphore-handles-and-the-delay-library.md`,
  `docs/decisions/0012-semaphore-handle-bits.md`).
- M30 slice 11 (2026-10-02): **the timer library's nodes and the due
  condition** — model instrumentation at the delay helper's `WaitSema` block
  showed the library's two node types (0x10-byte descriptors at the
  0x0088C340 free list; 0x40-byte timer nodes at 0x006592F0+0x14) and that
  **the delay nodes are scheduled and active** (flags 3, descriptors
  0x0088BF40/0x0088BF50). The remaining wall is the TIM2 handler's due
  condition: every active node keeps `accumulated = 0` and `flags = 3`, so
  the dispatcher is never reached; a longer virtual time does not change it.
  The idle budget rose to 200,000. CTest 32/32; the differential passes at
  3,000 services (interpreter 7,508,945 instructions, state identical); a
  30,000-service run takes about four seconds
  (`docs/reverse-engineering/m30-slice11-timer-library-nodes.md`).
- M30 slice 12 (2026-10-02): **handler execution** — a watch at the TIM2
  handler's due comparison showed its body never ran, and the deferred-call
  state showed a stuck handler frame. Two defects: queued causes were
  injected before every bridge step (starving the first handler at one
  instruction per delivery) and a handler's signal could switch threads
  mid-handler, abandoning its frame. Fix (decision 0013): **no nested
  injections** (`start_interrupt` refuses while a handler call is active)
  and **no preemption inside a handler** (`preempt_if_outranked` refuses;
  the switch happens in `deferred_return`). **The game's delay machinery now
  works and the boot runs continuously: 1,000,000 services, 33,650,798
  interpreted steps, about 29 seconds, no deadlock**; the differential
  passes at 3,000 services with the interpreter reference at 7,554,609
  instructions and the full state identical. `gt4boot --threads` now prints
  the handler tables and the deferred-call/pending-cause counts. CTest
  32/32; Python 73 (67 run, 6 skip)
  (`docs/reverse-engineering/m30-slice12-handler-execution.md`,
  `docs/decisions/0013-handler-execution.md`).
- M30 slice 13 (2026-10-02): **the boot's service handshakes** — the
  file-open retry loop was traced to the file server's version check
  (sid 0x80000006 RPC 0xFF against the constant at 0x0065829C, "3000"); the
  model now answers the version queries with the game's own compatibility
  constants (also for the SIF manager), accepts **Deci2Call (0x7C)** with
  the reference emulator's returns, answers the disc subsystem's status
  query (sid 0x80001300 RPC 0x80001363, first word 0x310 — the lowest value
  the game's `(word >> 4) == 0x31` check accepts) and the fileio/CDVD
  version negotiation (sid 0x80000400 RPC 0xFE, minimums 0x20A/0x20E), and
  grows the RPC server table to **80 slots**. **The boot now binds the disc
  subsystem, passes the negotiation and creates its worker-thread pool (an
  11-thread runtime with string-coded servers), ending at the step limit
  (200,000,000) inside the 0x0058F000 subsystem init**; the differential
  passes at 3,000 services with the interpreter reference at 7,573,241
  instructions and the full state identical. CTest 32/32; Python 73 (67
  run, 6 skip)
  (`docs/reverse-engineering/m30-slice13-service-handshakes.md`,
  `docs/decisions/0014-service-handshakes.md`).
- M30 slice 14 (2026-10-02): **the SIF register mirror and the loading
  path** — the command-layer spin at 0x00590A18 was traced to the software
  register 1, which only an incoming `SET_SREG` (cid 0x80000001) can write
  through the library handler at 0x005B0850 (array at 0x008869C0; the live
  memory shows registers 0 and 1 both set); the model now **mirrors an
  incoming `SET_SREG` back to the EE** through the command buffer. The next
  wall, the deliberate trap of the game's device library, is cleared by
  answering the **liblgdev device sync** (server 0x046D046D RPC 12,
  576/576 bytes) with the completed status **0x010B2400** the game's check
  at 0x005608BC accepts (the module banner "liblgdev version 1.11.036" sits
  at live 0x006C8D40). **The boot now leaves the command-layer spin, binds
  the disc device library and runs its device polling round to the service
  limit: 1,000,000 services, 1,710,779 module calls, 46,608,011 interpreted
  steps — no step-limit stop**; the differential passes at 3,000 services
  with the interpreter reference at 7,573,241 instructions and the full
  state identical. CTest 32/32; Python 73 (67 run, 6 skip)
  (`docs/reverse-engineering/m30-slice14-sif-register-mirror.md`,
  `docs/decisions/0015-sif-register-mirror-and-liblgdev-sync.md`).
- M30 slice 15 (2026-10-02): **the service clock** — the model's time base
  now advances **one millisecond of BUSCLK ticks per handled service**
  (`Kernel::advance_service_time`, the delay library's unit), called from
  both engines at their service boundaries through the new
  `RunOptions::advance_time` hook, so the clock is a function of the guest's
  service sequence and the differential stays exact. Timers follow their
  CLKS selector with per-timer fractional remainders, the compare flag sets
  on **crossing COMP** (a handler that reprograms COMP keeps its period),
  and one VBlank joins the queue per frame of slices. Measured: TIM2
  advances **576.05 ticks per service** (one millisecond at CLKS =
  BUSCLK/256, within 0.01%) and the game's own library reprograms COMP while
  the run proceeds; the main thread's starved delay semaphore (667) gives
  way to new delays, and the **1,000,000-service run ends at a service
  boundary with the worker threads ready** (1,193,971 module calls,
  32,878,366 interpreted steps). The differential passes at 3,000 services
  with the interpreter reference at 7,570,583 instructions and the full
  state identical. CTest 32/32; Python 73 (67 run, 6 skip)
  (`docs/reverse-engineering/m30-slice15-service-clock.md`,
  `docs/decisions/0016-service-clock.md`).
- M30 slice 16 (2026-10-02): **the disc image backs the file service** — the
  boot's file opens were the game loading its IOP modules by name
  ("cdrom0:\IRX\SIO2MAN.IRX;1" first) and failing on the model's empty reply
  (handle 0 -> 0xFFFEFFFD at 0x005B6D6C). The model now reads the **pinned
  ISO** (a host-side ISO9660 reader: primary descriptor, on-demand directory
  walk, streamed reads, game path spellings) and answers the open with the
  real size (SIO2MAN 6,641; MCMAN 96,181; MCSERV 7,385; SIO2D 11,289;
  DBCMAN 15,653; DS2U_D 11,821; LIBSD 30,085; USBD 34,993); a path the disc
  lacks — or no image at all — answers handle 0 like a console without a
  disc; the tool takes `--disc <iso>` and both engines get the same image.
  **The boot now walks its module list instead of retrying one load**; the
  differential passes at 3,000 services with the interpreter reference at
  7,570,583 instructions and the full state identical. CTest **33/33** (the
  new `disc_image` test: synthetic image, path spellings, read tail, the
  pinned ISO's ELF magic); Python 73 (67 run, 6 skip)
  (`docs/reverse-engineering/m30-slice16-disc-image.md`,
  `docs/decisions/0017-disc-image-file-service.md`).
- M30 slice 17 (2026-10-02): **the archive path's reconnaissance** — the
  engine carries **two file layers** (the file server and the game's own
  PCDV CD path), selected by each load task's flag at [task+0xB0]; the
  object table at 0x0063A078 holds per-object vtables at +0xA8. With the
  disc in place the boot has reached its **movie phase**: the SDK's load
  structures name the file **`/mpeg`** (the string at 0x0068BB90). The
  PCDV protocol's shape is documented: RPC 3 = send 64/recv 64 (the reply
  lands at 0x0086CC40 and is the library's entry buffer; the scan at
  0x00548E98 accepts entries whose first byte is 1 and advances by the byte
  at +0x21), RPC 1 = send 64/recv 0 (the completion poll), the request
  `{0x10, 0x800, destination}` (a 2048-byte sector read), and the
  completion flag (bit 1 of the descriptor's +0x19 at 0x005491A0). The
  game's data volume **GT4.VOL** (2,459,502,592 bytes, extent 105879) is
  decoded: the header (magic 0xACB990AD, version 0x00020002, the name-table
  offset 0x0100BA61, 23 root children), entries `{name_offset, count,
  0x14}` with child offsets, and **names stored as text XOR 0xFF**
  (NUL-terminated, sorted): the root lists "advertise", "bgm", "car",
  "character", ... **No model behavior changed**; the next slice implements
  the GT4.VOL reader and answers the PCDV protocol from it. CTest 33/33;
  Python 73 (67 run, 6 skip); the differential passes at 3,000 services
  with the interpreter reference at 7,570,583 instructions and the full
  state identical
  (`docs/reverse-engineering/m30-slice17-archive-path.md`).
- M30 slice 18 (2026-10-02): **the GT4.VOL reader** — the library now
  parses the game's data volume: the header (magic 0xACB990AD, version
  0x00020002, the name-table pointer, a child count whose list holds
  `count - 1` offsets — the header counts itself like an entry), entries
  `{name, count, value}` (a directory's items are child offsets; a file's
  `value` is its byte size) and **names as text XOR 0xFF** with three
  tagged pointer tables. Parsing is **lazy** (children on demand with
  entry/children caches and a rejected-items set; the metadata window is
  read once) and **validated** (a child candidate must carry a known tag
  and decode to printable text). `DiscFileSliceSource` presents the ISO's
  `GT4.VOL;1` as a byte source and `Iso9660Image` reports file extents.
  Verified against the pinned archive: the **22 root categories**
  (advertise, bgm, car, character, config, crs, database, fep, font, icon,
  menu, mpeg, music, narration, projects, race, rtext, script, sound,
  specdb, text, tire), the `mpeg/gt4` chain and `mv0010`'s value
  0x01200004 (18,874,372 bytes; count 204,931), plus `gtloading.img` =
  0x21A0 (8,608). **A file entry's data records are not yet pinned** (671
  of `mv0010`'s 204,930 items coincide with real entries), so the reader
  reports names and sizes and stops there; the next slice pins the records
  and answers the PCDV protocol. CTest **34/34** (the new `gt4_volume`
  test); Python 73 (67 run, 6 skip); the differential passes at 3,000
  services with the interpreter reference at 7,570,583 instructions and
  the full state identical
  (`docs/reverse-engineering/m30-slice18-gt4-volume-reader.md`,
  `docs/decisions/0018-gt4-volume-reader.md`).
- M30 slice 19 (2026-10-02): **the file item records, probed** — `mv0010`'s
  item list begins with **97 clean three-word records** (`mv0011` .. `mv0107`
  in the tag-0 name table, each `{name, position, packed size}`: e.g.
  `mv0012` x 0x00034501, y 0x17104004 whose upper bits are 1,511,488) and
  then **mixes kinds**: one-word values that are valid tree entries
  (0x15C0 "arcade", 0xA1B4 "CarSelectionRoot.gpb"), name pointers from the
  tag-1/tag-2 tables and small fields (0x00000007, 0x00000FEC). **Three
  candidate grammars were tested against the pinned archive and none
  closes** (187,055/17,248 and 190,839/11,411 splits end with the cursor
  drifted), so **no parser was shipped** and the reader keeps its
  documented limit. The pinned three-word shape is exactly what the PCDV
  answers need (a name's position and size); the next slice should pin the
  record boundary from the game's own consumer (the PCDV library's scan and
  the engine's descriptor use at 0x004B1C70). No model behavior changed:
  CTest 34/34; Python 73 (67 run, 6 skip); the differential passes at 3,000
  services with the interpreter reference at 7,570,583 instructions and the
  full state identical
  (`docs/reverse-engineering/m30-slice19-file-item-records.md`).
- M30 slice 20 (2026-10-02): **the game's own CD driver reads the disc** —
  the PCDV read (sid 0x50434456 RPC 3) request is `{LBA, byte count, EE
  destination}` (the boot's first is `{0x10, 0x800, 0x0084E080}`) and the
  library checks the block's bytes at +1 against the ISO9660 "CD001"
  signature (0x00548E90), so the positions are **disc LBAs**. The kernel now
  answers from the disc image's raw sectors (`set_disc_sectors`; a machine
  without a disc answers zeros; a read outside the image stops loudly) and
  **the boot's driver walks the ISO's file system**: the observed reads are
  **LBA 0x10 (the primary volume descriptor)** and **LBA 0x105 (the root
  directory, the ISO's root extent 261)**, and the run's work changes
  (71,124 module calls at 60,000 services versus 80,089 before). The
  **external GT4FS reference** (github.com/Razer2015/GT4FS) corroborates
  the format family — TOC header (magic "RoFS", version 3.1), entries
  `{parent node (BE), name, type byte, file {page offset, date, size} /
  directory node id}` and `file offset = DataOffset + pageOffset *
  PageLength` — while the pinned volume is the older uncompressed 2.2
  variant (names XOR 0xFF) the reference cannot read; its layout is the
  guide for pinning the file records next. CTest **34/34** (the kernel test
  covers the read service); Python 73 (67 run, 6 skip); the differential
  passes at 3,000 services with the interpreter reference at 7,570,583
  instructions and the full state identical
  (`docs/reverse-engineering/m30-slice20-pcdv-disc-reads.md`,
  `docs/decisions/0019-pcdv-disc-reads.md`).
- M30 slice 21 (2026-10-02): **the driver's disc walk and the volume's
  version family** — with the read service in place a 120,000-service run
  issues exactly two reads (**LBA 0x10** = the ISO's primary volume
  descriptor and **LBA 0x105** = the root directory, extent 261) and then
  stops: the driver parses the descriptor, follows the root extent and
  reads the root, but does not reach any file's extent (GT4.VOL's 105879
  would be next). The GT4FS packer writes the **same 2.2 version** the
  pinned volume carries and its offset encryption is
  `offset ^ index * 0x14AC327A + 0x14AC327A`; testing the pinned volume
  shows its TOC page table is the `count - 1` offsets at +0x20, its pages
  are the intervals (24, 16, 20, 24, 92, 268, ... bytes) and **none of them
  inflates** — Sony's 2.2 is uncompressed, which is why the empirical
  reader (names XOR 0xFF) reads it directly. Serving GT4.VOL at the
  requested offsets and treating the pages as deflate were both tried and
  rejected. No model behavior changed: CTest 34/34; Python 73 (67 run, 6
  skip); the differential passes at 3,000 services with the interpreter
  reference at 7,570,583 instructions and the full state identical
  (`docs/reverse-engineering/m30-slice21-driver-disc-walk.md`).
- M30 slice 22 (2026-10-02): **the disc's two volumes and the CD driver's
  volume protocol** — the live dump's library cache holds four blocks
  (keys 0x10, 0x105, 0x1418D0, 0x1419C5), each byte-identical to the
  image's sectors at 0x10, 0x105, 0x1418C0 and 0x1419B5: the game reads a
  **second ISO9660 volume** whose logical block 0 is 0x1418C0, and the
  image stores it **sixteen blocks early** (its system area is left out).
  The library's protocol: **RPC 2** registers the descriptor block with an
  index-weighted byte checksum (only for the first volume), **RPC 4**
  answers the registered volume's "volume space size" (which the engine
  stores at [task+0xEC] and uses as the next volume's start). The model now
  presents the disc's **logical blocks** (`DiscSectors` derives and
  validates the volumes from the image, rejecting an unexplained tail and
  passing single-volume images through), answers RPC 2 by recomputing the
  checksum from the served image (a mismatch stops loudly) and RPC 4 with
  the registered volume's size. **The boot now mounts both layers and reads
  the inner archives** (magic 0xACB990AD, version 3.1) — the layer-1 reads
  land exactly on the live cache's blocks — and stops at a new frontier: a
  guest fault (an unaligned word access at 0x008475EB, pc 0x00462670, after
  83,783 services) while the engine parses archive data. CTest 34/34;
  Python 73 (67 run, 6 skip); the differential passes at 3,000 services
  with the interpreter reference at 7,570,583 instructions and the full
  state identical (`docs/reverse-engineering/m30-slice22-dual-layer-and-pcdv-volume.md`,
  `docs/decisions/0020-dual-layer-disc-and-pcdv-volume-ops.md`).
- M30 slice 23 (2026-10-02): **the archive-parser fault diagnosed** — the
  boot's fault after 83,783 services (an unaligned word access at
  0x008475EB, reported at pc 0x00462670) is **not a translation
  divergence**: the reference interpreter faults at the same address and
  width (temporary instrument), so the cause is the **guest data the model
  provides**. The module entry 0x00462670 is the engine's string-object
  class; its assignment body calls 0x005595C8 — a **pointer relocation**
  (it reads `*(a0 + 4)` as the old base, computes the delta and rebases the
  pointers at +0xC/+0x14/+0x1C) — with a structure at an **odd** address
  (0x008475E7). The structure lives in the engine's static name buffer
  (base 0x00847580); the live dump holds the same kind of structure
  **aligned** at 0x008475E0, so its placement depends on data the engine
  processed (the string that precedes it). The engine's library here is its
  file/name layer (paths `/sound/gt4race2.ins` at 0x006AB850, `.ins` at
  0x006AB4E0). No model behavior changed: CTest 34/34; Python 73 (67 run, 6
  skip); the differential passes at 3,000 services with the interpreter
  reference at 7,570,583 instructions and the full state identical
  (`docs/reverse-engineering/m30-slice23-archive-parser-fault-diagnosed.md`).
- M30 slice 24 (2026-10-02): **the engine's stream and its static object** —
  a temporary write watch over the engine's static buffer (0x00847580) and
  its static object (0x00623A50) shows: the buffer is cleared first, then
  **byte** writes build a stream of **13-byte records** from 0x008475CA
  (0xD4 at +1 and 0x0D at +5 of each record), while the cursor at
  0x00623A40 counts down by 13 per record (0x1000 → 0xFF3 → 0xFE6 →
  0xFD9); the static object (the class whose vtable 0x00688868 is built at
  0x004628C8/0x0046132C/0x00604D58) receives **+4 = 0x008475E7** (the odd
  data pointer) and **+0xC = 0xFD9** (4057, the stream's remaining size).
  The live game's same-class object holds an **even** pointer (0x008483C0),
  size 0x200 and extra fields (0x60, 0x8B060, 0x1A7D0), and its buffer's
  structure sits aligned at 0x008475E0; the live memory never contains
  0x008475E7. The file-server trace shows only the version query (RPC 0xFF)
  before the fault — **no reads** — so the stream is built from data the
  engine already has. No model behavior changed: CTest 34/34; Python 73
  (67 run, 6 skip); the differential passes at 3,000 services with the
  interpreter reference at 7,570,583 instructions and the full state
  identical
  (`docs/reverse-engineering/m30-slice24-engine-stream-and-static-object.md`).
- M30 slice 25 (2026-10-02): **the sound library's stream and its objects** —
  the static object at 0x00623A50 belongs to the engine's **sound library**
  (0x00462xxx): the setter 0x004627B0 maintains the statics 0x00623A3C (a
  stream write position) and 0x00623A40 (its free space), and the init at
  **0x00463000** (guarded by the flag at 0x00623A74) sets them to
  {0x008475C0, 0x1000}, assigns the sound banks `/sound/gt4sys.ins`
  (0x006AB898), `/sound/gt4race.ins` (0x006AB8B0) and
  `/sound/gt4count.ins` (0x006AB8C8) to a static array of string objects at
  0x008505C0, then **assigns `/sound/roadnoiz.es` (0x006AB8E0) to the
  static object at 0x00623A50 with the relocating flag 1** (the faulting
  path), and parses `/sound/gt4se.inf` (0x006AB8F8) through 0x004AE230,
  relocating the result with the same idiom (0x004630E0). The object's odd
  data pointer (0x008475E7) is exactly the stream position after three
  13-byte records (0x008475C0 + 39) and its size (0xFD9) is 0x1000 − 39 —
  the object is a view into the stream. No model behavior changed: CTest
  34/34; Python 73 (67 run, 6 skip); the differential passes at 3,000
  services with the interpreter reference at 7,570,583 instructions and the
  full state identical
  (`docs/reverse-engineering/m30-slice25-sound-library-stream-and-objects.md`).
- M30 slice 26 (2026-10-02): **the sound library's assign fills the stream** —
  a pc-carrying write watch (a temporary global set by the driver at every
  module entry and interpreter step) shows **every byte of the stream at
  0x008475C0 is written by the module entry 0x00462670** (the string/blob
  assign's own memcpy 0x005A4724 — there is no separate writer), with the
  free-space static 0x00623A40 advancing by the copied sources' lengths
  (0x1000 → 0xFF3 → 0xFE6 → 0xFD9); the writes' bytes are binary
  (`00 D4 00 00 00 0D ...`), so the sources are **blobs** (parsed sound
  data), not names. The faulting flag-1 assign relocates the source,
  memcpys it into the stream's position, then **relocates the destination
  in place** (0x00462618 → 0x005595C8 with a0 = 0x008475E7) — that last
  relocation's first read `*(a0 + 4)` at the odd address faults. The
  position is 0x008475C0 + 0x27 (**39 bytes** copied before) while the live
  game's buffer holds 32 bytes before its aligned structure: the difference
  is in the *sources copied before the fault*, not the mechanism. No model
  behavior changed: CTest 34/34; Python 73 (67 run, 6 skip); the
  differential passes at 3,000 services with the interpreter reference at
  7,570,583 instructions and the full state identical
  (`docs/reverse-engineering/m30-slice26-assign-fills-the-stream.md`).
- M30 slice 27 (2026-10-02): **the stream's content reconstructed** —
  replaying the slice-26 write log shows the sound library's stream at
  0x008475C0 holds **13-byte records**
  `{00 00 00 00 D4 00 00 00 0D 00 00 00 00}`: the first three are
  byte-identical and the **fourth** (at the faulting 0x008475E7) differs
  (`{00 ×8, 0D, 00 00 00, 40}`), the fault hitting while its relocation
  runs; the second stream (0x00847180) received the same header shape and
  then six **relocated pointers** back to 0x008471A0. The assign does not
  copy the caller's string: it asks the getter 0x00462588 for the
  destination's data, calls **0x0044D740** (a printf-style formatter that
  allocates from the SDK's arena 0x004AEFF0/0x004AE1F8) and then memcpys
  the formatted object through 0x00462670 — so the records are **formatted
  arena objects**, not the file-name strings the callers pass. No model
  behavior changed: CTest 34/34; Python 73 (67 run, 6 skip); the
  differential passes at 3,000 services with the interpreter reference at
  7,570,583 instructions and the full state identical
  (`docs/reverse-engineering/m30-slice27-stream-content-reconstructed.md`).
- M30 slice 28 (2026-10-02): **the stream's writers by call site** — a
  temporary watch reported pc/ra/a1 with every write into the sound
  library's buffers: the assign body (0x00462670, called at 0x00462778
  inside 0x00462738) writes 69 bytes, the string method 0x00462900 twelve
  (called from 0x00462FD4), the sound function 0x00463600 ten (from
  0x00463764), the statics 0x00623A40 four (pc 0x00462710), the name lookup
  0x00462EC8 one (from 0x0046304C), the two stream setters 0x004627B0 twice
  — and **148 writes come from pc 0x00100008, the patched syscall stubs
  (the SDK's own string code writes into the same region)**. Lesson: the
  driver's watch globals are set only at module entries it starts, so the
  ra is reliable but the argument registers can be stale for inner calls;
  the next instrument must read arguments at a boundary or watch the
  formatter's arena. No model behavior changed: CTest 34/34; Python 73
  (67 run, 6 skip); the differential passes at 3,000 services with the
  interpreter reference at 7,570,583 instructions and the full state
  identical
  (`docs/reverse-engineering/m30-slice28-stream-writers-by-call-site.md`).
- M30 slice 29 (2026-10-02): **instruments and the formatter** — a second
  instrument read the guest argument registers (r4/r5/r6) from a temporary
  state pointer at every write and captured **zero** copies: the translated
  module keeps values in host registers and synchronizes the guest file
  only at boundaries, so mid-execution register reads are stale (the same
  reason the slice-28 a1 read zero). The assign chain's formatter
  (0x0044D740) is mapped as the SDK printf: context init 0x004AEFF0,
  format parse 0x004AE1F8, format 0x004AF3E8, result at `*(context+0x94)`,
  context release 0x004AF568/0x0044D460/0x004AF0A0. The three sound-bank
  assignments advance the stream by **13 bytes each** (not the names'
  lengths 17/18/19), so the copied objects are not the formatted names;
  identifying them needs the source object's content (observable in memory)
  or a static trace of the two sound functions that also write the stream
  (0x00462900 and 0x00463600, 22 of the 247 watched writes). No model
  behavior changed: CTest 34/34; Python 73 (67 run, 6 skip); the
  differential passes at 3,000 services with the interpreter reference at
  7,570,583 instructions and the full state identical
  (`docs/reverse-engineering/m30-slice29-instruments-and-the-formatter.md`).
- M30 slice 30 (2026-10-02): **the stream's serialized content, complete** —
  a write watch over the whole stream (0x00847180..0x008476C0, **1,344
  writes**) shows the stream at 0x008475C0 holds exactly **four 13-byte
  records** and nothing else: `{0, 212, 13, 0}` three times and
  `{0, 0, 13, 64}` for the fourth (the flag-1 `/sound/roadnoiz.es`
  assignment that faults); the rest of the region is zero, so the small
  fields (212, 64) are **not offsets into the stream**. Each record is the
  **head of a serialized object** the assign copies (flag, value, length
  13, value) — the assign copies the source object's first `*(source+8)`
  bytes — and the flag-1 assign then relocates the copy in place
  (converting the serialized values into absolute pointers); that
  relocation's first read at the odd 0x008475E7 (after 39 bytes) faults,
  while the console's buffer has 32 bytes before its aligned structure. The
  live dump's same buffer holds "INST" (the engine's uppercased extension)
  at the menu — a later moment. No model behavior changed: CTest 34/34;
  Python 73 (67 run, 6 skip); the differential passes at 3,000 services
  with the interpreter reference at 7,570,583 instructions and the full
  state identical
  (`docs/reverse-engineering/m30-slice30-stream-serialized-content.md`).
- M30 slice 31 (2026-10-02): **the records come from the null pointer** — a
  temporary instrument that tracks the last guest read and prints it with
  every stream write shows the copies' sources are **0x0..0xC**: the assign
  copies from the **null pointer** because the assign chain's
  formatter/resolver **0x0044D740 returned 0** (a failed parse — its
  disassembly returns 0 when 0x004AE1F8's parse of the format fails). The
  "13-byte records" are the **low memory's content** (0xD4 at +4, 0x0D at
  +8 — exactly slice 30's reconstruction; the memory scan also finds the
  fourth pattern at address 0x0), the length 13 comes from the low memory's
  byte at +8, and after three such copies the stream position is odd (39
  bytes), which makes the flag-1 assignment's relocation fault. The fault
  chain: **the resolver fails → the assign copies from null → the odd
  position → the relocation fault.** No model behavior changed: CTest 34/34;
  Python 73 (67 run, 6 skip); the differential passes at 3,000 services
  with the interpreter reference at 7,570,583 instructions and the full
  state identical
  (`docs/reverse-engineering/m30-slice31-records-from-the-null-pointer.md`).
- M30 slice 32 (2026-10-02): **the resolver's handler registry** — the
  failing parse (0x004AE1F8 → **0x004ACE58**) is a **handler-registry
  dispatch** over the global list at **0x006318B0** (per handler: a match
  at vtable+0x38, then the handler at vtable+0x40), and the model's
  registry at the fault is **identical** to the live dump's (nodes
  0x617BB0/0x84B480/0x617AB0, vtables 0x688C58/0x688B70). The match
  (**0x004ACBA0**) compares the path against the handler's **string list at
  +0xF4** (via 0x004AE9E8, a path-prefix compare) or, when +0xF4 is zero,
  the **single string at +0xAC** (fallback 0x004B0A38). The live handler
  0x617BB0 carries **+0xAC = "/"** (built at 0x004ACA68, the string
  0x006B00D0, the constructor 0x004ACA40's own constant), so the console
  matches any path starting with "/", while the model's handler has
  **+0xF4 = 0x00617AA8** (the global holding the `/mpeg` pointer) and the
  other handlers have +0xF4 = 0 — a different prefix list, so the
  sound-bank paths match no handler and the parse returns 0. The name
  strings were verified intact at the fault (`/sound/gt4race2.ins`,
  `%s%s%s.ins`, `/sound/gt4sys.ins` at 0x006AB850 onward). No model
  behavior changed: CTest 34/34; Python 73 (67 run, 6 skip); the
  differential passes at 3,000 services with the interpreter reference at
  7,570,583 instructions and the full state identical
  (`docs/reverse-engineering/m30-slice32-resolver-handler-registry.md`).
- M30 slice 33 (2026-10-02): **the handler registration, and a correction** —
  the game's own early init (0x00100D30–0x00100DB4) constructs and
  registers the two archive handlers through the wrapper 0x004ACC28 (which
  calls the constructor 0x004ACA40): **0x00617AB0** with no prefix list
  (t0 = 0) and **0x00617BB0 with t0 = 0x00617AA8** — the game's own
  registration, whose array starts with `/mpeg`; the constructor also
  builds "/" (0x006B00D0) and passes it to the init 0x004B1C10. The field
  comparison at the fault shows the model's and the live handlers are
  **nearly identical** (vtable 0x688C58, **+0xAC = "/"**, the archive
  bindings +0xB8 = 0x1BEF0 / +0xC8 = 0x90EA80 / +0xCC = 0x59440, the first
  handler's +0xF4 = 0x617AA8), so **slice 32's reading was wrong**: the
  prefix state matches the console's. The match chain is: node 0x617BB0's
  list {"/mpeg"} misses, node 0x84B480's match is a stub, and node
  0x617AB0's fallback (+0xAC = "/") **matches** any "/" path — so the
  parse finds a handler and the failure is inside the **handler method
  0x004B1730** returning 0 (the archive's file open for
  `/sound/gt4sys.ins`). No model behavior changed: CTest 34/34; Python 73
  (67 run, 6 skip); the differential passes at 3,000 services with the
  interpreter reference at 7,570,583 instructions and the full state
  identical
  (`docs/reverse-engineering/m30-slice33-handler-registration.md`).
- M30 slice 34 (2026-10-02): **the archive open handler's flow** — the
  vtable-0x688C58 open method **0x004B1730**: it allocates a stream
  (0x004AC660 — returning 0 is its only early exit), builds the full path
  from the handler's **+0xAC ("/")** and the requested path (0x004AE908),
  then **enqueues the stream via 0x004AD300**, which locks the handler's
  queue (+0x40), sets the stream's state (+0x80 = 0) and **waits on the
  condition 0x0057CB00** — the open is processed by the handler's own
  worker under the lock — and returns the stream; the result the caller
  reads lives in the **stream's +0x94**. The worker step **0x004AD4A0**
  walks a **sorted tree at the handler's +0x58**, comparing the stream's
  key pair (+0xA0, +0xA4) against each node's pair and descending (the
  archive's page tree, the GT4FS reference's B-tree), and sets the
  stream's state (+0x80 = 2) on a match. So the failure is either the
  stream allocation returning 0 or the worker leaving the result at 0. No
  model behavior changed: CTest 34/34; Python 73 (67 run, 6 skip); the
  differential passes at 3,000 services with the interpreter reference at
  7,570,583 instructions and the full state identical
  (`docs/reverse-engineering/m30-slice34-archive-open-handler.md`).
- M30 slice 35 (2026-10-02): **the stream pool and the queued open** — the
  stream factory 0x004AC660 pops from the **free list at 0x0084B528** (the
  raw-disc handler +0xA8) under a lock; at the fault that list is
  **identical to the console's** (`0x0084B528 → 0x62A0B4 → 0x62A078 → 0`;
  the streams are a static array at 0x0062A0xx) — so the **allocation is
  not the failure**. The handler's third argument is the **formatter's
  context** (the "stream"), and its enqueue call **0x0057CB00 is a doubly
  linked-list append**, not a wait: the stream is **queued to the
  handler's pending list at +0x40** and the handler's own **worker thread**
  processes it later; the formatter then reads the result from the
  **stream's +0x94**. So the failure is the worker leaving +0x94 at 0 —
  its search over the tree at the handler's +0x58 (the archive's page
  tree) finding nothing, or the worker never processing the queued stream;
  the stream's search key (+0xA0/+0xA4) is zeroed by the context
  constructor. No model behavior changed: CTest 34/34; Python 73 (67 run, 6
  skip); the differential passes at 3,000 services with the interpreter
  reference at 7,570,583 instructions and the full state identical
  (`docs/reverse-engineering/m30-slice35-stream-pool-and-queued-open.md`).
- M30 slice 36 (2026-10-02): **the handlers' state matches; the failure is in
  the stream's processing** — every compared handler field is **identical**
  to the live dump: the pending lists (+0x40) are **empty** (the queued
  open *was processed*, not stuck), the state (+0x50) is 0, the completed
  list (+0xAC) holds 0x6B00D0/0x6317C4 and the current object (+0x60) is
  0x688C40 — so the entire handler-side state (registry, prefixes, archive
  bindings, stream pool, queues) matches the console. The completion step
  is **0x004AED80**: it locks, pops the completed list (0x0057CB80 on the
  handler's +0xAC) and dispatches the handler's current object (+0x60)
  through its vtable+0x48 method. With the handler state matching, the
  difference is in the **stream's dynamic processing** — the stream is the
  formatter's context (on its stack) and the result the formatter reads
  (+0x94) stays 0 in the model while the console's open succeeds; the next
  slice watches that context (its +0x94/+0x80 writes) or keys on the
  context construction (0x004AEFF0). No model behavior changed: CTest 34/34;
  Python 73 (67 run, 6 skip); the differential passes at 3,000 services
  with the interpreter reference at 7,570,583 instructions and the full
  state identical
  (`docs/reverse-engineering/m30-slice36-handler-state-matches.md`).
- M30 slice 37 (2026-10-02): **the archive buffers are identical; the
  worker's insert** — the live dump's handler buffers hold **the same
  archives the model serves, byte for byte**: 0x0090EA80 (0x59440 bytes)
  == the image's logical block 0x1BEF0, and 0x00905B00 (0xF00 bytes) ==
  the image's logical 0x143B24 (file 0x143B14, the -16 layer shift); both
  start with the 3.1 header (0xACB990AD). So the difference is **not** the
  archive content. The sound names (`gt4sys`, `roadnoiz`, `gt4se`) appear
  plainly XOR-0xFF in the **outer** archives' name tables (GT4.VOL and
  GT4L1.VOL at 0xC01B/0xC025/0xC059); the inner archives' names are not
  raw-searchable (their pages are compressed), so that search is
  inconclusive. The worker step **0x004AD4A0** walks a sorted list at the
  handler's +0x58 by the stream's key pair (+0xA0/+0xA4); with an **empty
  list** (the state in both the model and the console) it takes the insert
  path **0x004AD53C**: it locks, sets the **stream's state (+0x80) to 2**
  and inserts the stream into the handler's list (0x0057CB28 with the
  stream's condition node at +0x3C). No model behavior changed: CTest 34/34;
  Python 73 (67 run, 6 skip); the differential passes at 3,000 services
  with the interpreter reference at 7,570,583 instructions and the full
  state identical
  (`docs/reverse-engineering/m30-slice37-archive-buffers-and-the-worker-insert.md`).
- M30 slice 38 (2026-10-02): **the stack watch captures the open; the
  completion never runs** — a temporary write watch followed a narrow
  window around the guest sp (the driver publishes the current sp) and
  reported each *new* (pc, address) pair (deduplicated against the idle
  loops). The trace shows the open through the formatter's context at
  0x1FFFDB0 in order: the handler store (0x004B176C), the enqueue
  (0x004AD330 sets the state +0x80 to 0; 0x0057CB00 appends to the pending
  list), the formatter's bookkeeping (0x0044D7C0/8), the **next-stage step
  (0x004AD690 sets the state to 1 and appends the stream to the +0x4C
  list)** and the **path string `'gt4sys.ins`** built at 0x96DAF2 (the
  bytes 27 ac 67 74 34 73 79 73 2e 69 6e 73 73) — then the formatter's
  cleanup (0x004AF568/0x0044D460/0x004AF0A0) runs and it reads the result.
  **The stream's +0x94 is never written** (only its initial clear). The
  completion function is **0x004AD890 → 0x004AD808** — `*(stream+0x94) =
  the vtable+0x20 method's result` (wrapped by 0x005750C0) — a virtual
  method of the handler (vtable+0x24 = 0x004AD868) that the worker pipeline
  invokes when the open finishes; in the model it never runs. No model
  behavior changed: CTest 34/34; Python 73 (67 run, 6 skip); the
  differential passes at 3,000 services with the interpreter reference at
  7,570,583 instructions and the full state identical
  (`docs/reverse-engineering/m30-slice38-stack-watch-and-the-missing-completion.md`).
- M30 slice 39 (2026-10-02): **the pump's state machine and the missing
  state 2** — **0x004AF268** drives the stream's state (+0x80): state 0 →
  0x004AD368 (then +0x84 = 1 and a vtable+0x40 call); state 1 → the
  once-only check **0x004AF520** (it reads the stream's +0x98, sets it to 1
  and returns true only the first time) then **0x004AD438** (which signals
  the condition 0x00574EE8 on the handler's +0x64); state 2 → the check
  then 0x004AD5E0. The completion that sets the result is **0x004AD890 →
  0x004AD808** (`*(stream+0x94) = the vtable+0x20 method's result`, wrapped
  by 0x005750C0), whose vtable entry (handler class +0x24) is
  **0x004AD868** — a virtual method with no direct caller, invoked by the
  pipeline when the open finishes. The slice-38 trace shows the open
  reaching **state 1** (the step 0x004AD690 inside 0x004AD648 sets +0x80 =
  1 and appends the stream to the handler's +0x4C list) and then the
  formatter's cleanup: the **state never reaches 2**, so the state-2 step
  and the completion never run. The +0x4C list is the stage the worker
  pipeline drains to advance to state 2. No model behavior changed: CTest
  34/34; Python 73 (67 run, 6 skip); the differential passes at 3,000
  services with the interpreter reference at 7,570,583 instructions and the
  full state identical
  (`docs/reverse-engineering/m30-slice39-pump-state-machine.md`).
- M30 slice 40 (2026-10-02): **the context's virtual table and the wait for
  state 3** — the context class (vtable **0x00688ED0**) maps to +0x08 = the
  destructor (0x004AF0A0), +0x10 = the *process* (**0x004AF1D8** — with the
  state at 0 and a handler bound it calls 0x004AD648), **+0x18 = the state
  machine (0x004AF268)**, **+0x20 = the wait (0x004AF3A0 — while the
  stream's state +0x80 is not 3 it blocks at 0x005767E0)**, +0x30 =
  0x004AF108, +0x38/+0x40 = callback dispatchers (0x004AF448/0x004AF478).
  The formatter's format step **0x004AF3E8** calls the context's process
  and then its wait: the open is **asynchronous** and the formatter
  **blocks until the stream's state reaches 3**, only then reading the
  result (+0x94). The once-only check **0x004AF520** is called from the
  pump *and* from the handler's own steps **0x004AD9F4** and **0x004ADBD4**
  — the worker-side processing that advances the state. The slice-38 trace
  shows the state reaching **1** (the process's next-stage step appends the
  stream to the handler's +0x4C list) and never 2 or 3, so those handler
  steps never run. No model behavior changed: CTest 34/34; Python 73
  (67 run, 6 skip); the differential passes at 3,000 services with the
  interpreter reference at 7,570,583 instructions and the full state
  identical
  (`docs/reverse-engineering/m30-slice40-context-vtable-and-the-wait.md`).
- M30 slice 41 (2026-10-02): **the worker runs; the result field is the
  wall** — the handler class's constructor (0x004AD1C8, base vtable
  0x688D48) initializes the mutex (+0x10), the three lists (+0x40, +0x4C,
  +0x58 — each {head, tail, tag 0x688C40}), the worker's condition (+0x64)
  and the result (+0x94), and creates no thread. The archive handler's
  vtable (0x00688C58) maps every step: +0x38 the path match, +0x40 the
  open (0x004B1730), +0x50 the **worker loop** (0x004AD8F8: waits on the
  condition, drains +0x4C), +0x58 the first drain (0x004AD9D0: the gate
  0x004AF520, the work method for the command, then 0x004AD6D8 moves the
  stream to the sorted tree +0x58 and sets state 2), +0x60 the lookup work
  (0x004B0B48 → +0xC8 0x004B1800 → +0xE0 0x004B1F90, the archive search
  that normalizes the path through 0x004B2050 and searches the directory
  object at handler+0xC4 through 0x004B3270), +0x70 the second drain
  (0x004ADBB8: work +0x78 = 0x004ADC40 → 0x004AF780, pop the tree, then
  the **stream's** completion 0x004AF4A8 sets state 3 and wakes the
  formatter), +0x28 the handler's completion (0x004AD890 → 0x004AD808:
  handler+0x94 = the file object). A new `gt4boot --dump ADDRESS LENGTH`
  stop-time memory view (which also prints when the guest faults) shows
  the model at 83,782 services: the formatter's context (0x01FFFDB0) has
  **state +0x80 = 3**, +0x84 = 2 and **result +0x94 = 0**; the handler
  0x617AB0's three lists are **empty** and its +0x94 = 0x0096DEF0. The
  open's pipeline therefore **ran end to end**, which **corrects slices
  38–40** ("the completion never runs / the state never reaches 3": the
  stack watch followed the sound thread's sp and missed the worker
  thread's writes). The differential at 83,782 services — the whole boot
  to the fault's doorstep — is **identical** (24,114,381 interpreter
  instructions, full state). The next slice must find which step should
  write the context's +0x94 (the lookup's request field is only zeroed)
  and which input differs from the console
  (`docs/reverse-engineering/m30-slice41-worker-and-the-result-field.md`).
- M30 slice 42 (2026-10-02): **the block cache (PRTS) and the sound open** —
  the open's result stayed zero because the game's own **block cache
  server** (sid 0x53545250, "PRTS" — the bind at 0x550D00) was not modeled.
  A temporary **write watch in the memory API** (every guest store passes
  through it; each new (pc, address) pair logged once) captured the open's
  exact sequence: the request copy (0x4B0EC8..0x4B0ED8), the block read's
  failure (`0x4B0F34 [descriptor+0] = 9`), the header magic mismatch
  (`0x44D6C4 [descriptor+0] = 2`), the release, the descriptor's status
  copied into the stream (0x4AF7BC), the drain's pop, the gate reset and
  the stream's completion (`0x4AF4D8 [stream+0x80] = 3`). The writer of the
  result is the descriptor callback **0x44D540** (second drain → 0x4ADC40 →
  0x4AF780 → the callback): **0x44D6BC `stream+0x94 = the file object`**
  after the header read (0x4AFA20 → the handler's vtable+0xB8 = 0x4B11A8)
  and the magic check ("INST" / 0x4E474E45); the failure paths store the
  error instead (0x44D5B0/0x44D6C0/0x44D6E8). A trace of every SIF RPC
  showed the cache client's protocol: RPC 3 `{LBA, size, flags}` (the reply
  is the handle the client checks is non-zero) and RPC 4/7
  `{handle, destination, size}` (the copy-out), with the sound file's block
  (0x1C2C0, 0x1A830) and its header copy-out (0x96DD80, 0x20) among them.
  **The fix** (decision 0021): `Kernel::answer_prts_read` reads the block
  from the same image as the PCDV reads and answers a fresh handle (a cache
  of at most eight blocks); `Kernel::answer_prts_copy` copies the cached
  block into the guest (clamped, validated). Unit tests cover the handle,
  the copy-out, an unknown handle, a read outside the image and a machine
  without a disc. The boot now runs **past the sound phase** to the step
  limit (3,648,011 services handled where the old run faulted at 83,783);
  the differential at 100,000 services is identical (43,082,342 interpreter
  instructions); CTest 35/35 with `gt4boot_services` pinning 90,000
  services with the disc
  (`docs/reverse-engineering/m30-slice42-block-cache-and-the-sound-open.md`,
  `docs/decisions/0021-prts-block-cache.md`).
- M30 slice 46 (2026-10-03): **the font file's load path — the copy-out
  cursor (the fix)**. The whole boot reads 12 disc blocks; the font loads
  as 3 late PRTS reads (186 KB last) with no fileio involvement, and
  streams out as seven 0x4000-byte copy-outs — but the request has no
  offset and the model re-served chunk zero, corrupting the object.
  `PrtsBlock` gains a cursor the copy-out serves from and advances
  (decision 0021 extended; tests cover sequences and exhaustion). The
  verification run passes the 15,010,045-service fault to its step limit
  with **41,919,339 services handled**, stopping cleanly
  (`docs/reverse-engineering/m30-slice46-the-copy-out-cursor.md`).
- M32 slice 16 (2026-10-03): **every completion path mapped**. The
  unlink is pure list surgery; four callers (walk dispatch — the only
  signaling context — plus re-arm cancel, delete/cancel,
  update/refresh); no wait suffered cancellation; the signal endpoint
  is reachable only via the walk's indirect call. Due test + dispatch
  is the only signaling path — paradox is about inputs/runs, not
  hidden signalers
  (`docs/reverse-engineering/m32-slice16-completion-paths.md`).
- M32 slice 15 (2026-10-03): **the walk runs constantly; the sliver
  is jumped over**. Handler-start counters: 12,000 timer runs per 12k
  leg, gate/head/fields live-correct, no dispatch. Three-leg poke
  bracket (no reproduce, 3.2k near miss, wrap-through silence); arm
  site and dispatcher re-verified; firing sliver narrower than one
  frame's advance. Verdict: timeouts unreachable by time — completion
  must come via cancellation; slice 16 names each wrapper's request
  (`docs/reverse-engineering/m32-slice15-sub-service-sliver.md`).
- M32 slice 14 (2026-10-03): **the band swept twice, still silent**.
  Legs D11–D13 (`ckpt-1380k/1480k/1580k.bin`); COUNT wrapped mid-D13
  across the full band with live delivery, no dispatch. Careful math
  says an 8M-tick every-frame window cannot be missed — and the poke
  fired below any threshold: the due test alone does not decide firing.
  Walk reachability (gate/head/branch) is the question; slice 7
  downgraded; forecasts withdrawn; marching paused
  (`docs/reverse-engineering/m32-slice14-band-swept-twice.md`).
- M32 slice 13 (2026-10-03): **the walk designates thread 6's node
  next**. Post-fire slot `0x00889f80` (one node tested per run —
  correcting the walk picture); job protocol exactly three ops
  (else-branch asserts); pump post-dispatch table-driven but dormant
  without inbound bytes (producer hunt ends structurally). Leg D10
  marched and saved (`ckpt-1280k.bin`); ~2.3 legs to the next firing
  (`docs/reverse-engineering/m32-slice13-designation.md`).
- M32 slice 12 (2026-10-03): **the natural firing**. Leg D9 matured
  node `0x0088a000` on its own (consumed one-shot, COMP → `0x240`,
  saved post-fire in `ckpt-1180k.bin`); thread 3's full lifecycle
  (wake → 19 id queries, no work → sema deleted → asleep) produced
  nothing new. Redispatch after handlers confirmed in code (corrects
  slice 3). Five nodes stand, ~3 legs per firing
  (`docs/reverse-engineering/m32-slice12-natural-firing.md`).
- M32 slice 11 (2026-10-03): **live-handler lockstep**. Leg D6
  (`ckpt-880k.bin`): 100k services with exactly 100k module calls; a
  12k tally leg shows every service is `0x100` — zero new work, only
  clock maturation (COUNT +~1e9/leg, combined ≈ 2.20e9 vs. ≈ 4.26e9:
  ~2 legs to go, likely mid-D8)
  (`docs/reverse-engineering/m32-slice11-lockstep.md`).
- M32 slice 10 (2026-10-03): **unstick deliveries**. Measured mix
  (346k starts, 3.2k ok per 12k leg; backlog 255k VBlank + 208k
  timer-11, zero DMAC); `queue_interrupt` coalesces same-cause entries
  (decision 0025, unit tested; DMAC keeps stacking). Verification leg:
  pending 467,116 → 1, handlers live every frame. Threads still wait
  (delays maturing); crossings can't be missed on phasing anymore
  (`docs/reverse-engineering/m32-slice10-coalesce.md`,
  `docs/decisions/0025-coalesce-pending-interrupts.md`).
- M32 slice 9 (2026-10-03): **the wrap swept the band, no firing**.
  Legs D4+D5 (`ckpt-580k/680k.bin`, limit-hit, saved); COUNT wrapped
  mid-D5 across the whole theoretical band with no dispatch, COMP
  untouched. Verdict: delivery phasing, not level (burst vs. desert);
  the 40-minute timeouts can't be a real boot path, so the waits guard
  events the model never delivers. Stuck VBlank frame parsed (cause 2,
  full chain pending); backlog at 463k. Next is slice 10: measure the
  mix, fix VBlank enqueue if confirmed, name each wait's real event
  (`docs/reverse-engineering/m32-slice9-wrap-miss.md`).
- M32 slice 8 (2026-10-03): **the steady march**. Three chained 100k
  legs (`ckpt-280k/380k/480k.bin`, all limit-hit and saved), COUNT
  +~313M per leg, module calls identical (55,987), steps ±80. Combined
  ≈ 3.82e9 vs. firing neighborhood ≈ 4.26e9: ~1.4 legs to go, likely
  leg D5. No probes, no model change
  (`docs/reverse-engineering/m32-slice8-steady-march.md`).
- M32 slice 7 (2026-10-03): **who fired**. Node → descriptor → worker
  mapped 6/6 (callback `0x005AEF58`, sema words name the waiters); the
  poked leg consumed `0x0088a000` one-shot (flags `3 → 0`, unlinked,
  `0x00889f80` the new tail), waking thread 3 — correcting slice 5's
  reschedule guess. Base clock `0x005B8400` shares the handler's units;
  leading hypothesis is a missed pre-wrap fuse; one tick-math
  non-reconciliation recorded honestly. Probes removed
  (`docs/reverse-engineering/m32-slice7-who-fired.md`).
- M32 slice 6 (2026-10-03): **the honest maturation path**. Ships
  `--resume` + `--checkpoint-at` (new `gt4boot_checkpoint_chain`
  CTest, 41 total) and the idle budget at 2M (decision 0024): three
  chained 60k legs run to their limits and save (`ckpt-60k/120k/180k`),
  COUNT +~190M per leg toward the threshold (~7–8 legs to go).
  Stale-binary false alarm documented (confirm relink before reading
  runs). Call-mix and base-scale puzzles stay open for slice 7
  (`docs/reverse-engineering/m32-slice6-long-legs.md`,
  `docs/decisions/0024-chained-checkpoints-and-idle-budget.md`).
- M32 slice 5 (2026-10-03): **a delay node fires**. The due test at
  `0x005B822C` (`current < target` exits the whole walk; COMP is armed
  as `target >> 8`) probed by COUNT threshold: `+0x73800000`
  bit-identical to baseline, `+0x73E70000` moved the machine (one
  `iSignalSema` on thread 3's sema, thread 3 ready, COMP → `0x240`).
  Natural maturation ≈ 1.9e9 ticks ≈ 3–4 chained legs, no model change.
  Probes removed; docs only
  (`docs/reverse-engineering/m32-slice5-delay-node-fires.md`).
- M32 slice 4 (2026-10-03): **what the six workers wait for**. Stack
  unwinds from the checkpoint: all six share return `0x005aedc0` (a
  one-shot wait-then-delete wrapper over delay-library work
  `0x005b8d88`/`0x005b8f38` → dispatcher → `iSignalSema`), each on its
  own binary sema (count 0, one waiter), each paired to a VBlank-chain
  handler by code family. The stall is the never-firing delay
  dispatcher (slices 10–11's open frontier), with timer-conditioned
  evidence (live TIM2 comp/mode values in stale registers). Corrects
  slice 2: SIF binds completed — the bind path (`RPC_BIND` through the
  SIFCMD sender) is mapped as a bonus; the stall is downstream. Docs
  only (`docs/reverse-engineering/m32-slice4-waiters-unwound.md`).
- M32 slice 3 (2026-10-03): **the first packets**. One verbatim
  producer step (`{0,3}` at slot 181 + signal + dispatch): thread 2 ran
  its loop (`WakeupThread`, back to wait) in 2 services and the machine
  re-parked with counters balanced. One step aimed at sleeper thread 5:
  five `GetThreadId` calls then `SleepThread` (8 services) — a
  flicker, no new traffic, no unmodeled wall. Verdict: the parked state
  is stable, not fragile; the boot needs genuinely new input. Probes
  removed
  (`docs/reverse-engineering/m32-slice3-first-packets.md`).
- M32 slice 2 (2026-10-03): **thread 2's job queue, mapped end to
  end**. Saved registers match the loop code (`s1 = 0x00885ee8`,
  `WaitSema(11)`); 512 `{op,arg}` slots, all historically `{0,3}`;
  control block `{11, 0, 181, 181}`; exactly one RAM pointer to the
  block (`[0x00885c80]`); the SDK creator (`0x005aea78`: `CreateSema`
  then `CreateThread`, called once from the init cluster) and the op
  map (0 = wake, 1 = rotate, 2 = suspend). No handler carries the block
  as its argument — the wild producer (181 jobs, now silent) stays
  unnamed
  (`docs/reverse-engineering/m32-slice2-job-queue.md`).
- M32 slice 1 (2026-10-03): **the server inventory and the arrival
  path**. From the 243.7M checkpoint: 24 SIF servers bound (16 custom +
  8 system; model fully answers PCDV, PRTS and the fileio open); no
  PADMAN bound, so M35 waits on game progress, not model work. The pump
  queue is provably empty (mirror alias resolved by dump); the SIFCMD
  table and thread 2's dispatch loop are mapped (decision 0023 frames
  the async work). `gt4boot --threads` lists bound SIF sids permanently
  (`Kernel::sif_server_sids()`, unit-tested)
  (`docs/reverse-engineering/m32-slice1-server-inventory.md`,
  `docs/decisions/0023-async-iop-framing.md`).
- M30 slice 50 (2026-10-04): **the smallest stimulus — nothing
  deliverable wakes it**. One-shot INTC 0/5 injections end bit-identical
  (buried behind the queue, and both handlers are device acks by code
  reading); the SSUP nonzero answer is never asked (no SIF traffic in
  the final leg); the full handler inventory (VBlank accounting with
  empty chain, timer accounting, GS/VIF1 acks, SIF pump over an empty
  queue) contains no wakeup path. The stop needs originating traffic
  only milestone work provides (M32 async IOP from the mapped
  pump/queue/dispatcher, or M35 pad). No model change. CTest 40/40,
  Python 73 (67 run, 6 skip)
  (`docs/reverse-engineering/m30-slice50-stimulus-probes.md`).
- M30 slice 49 (2026-10-04): **the missing event per waiter class**. 46
  live semaphores; the 7 waited ones all count 0 with one waiter each
  (no scheduler anomaly): thread 2 (SIF/RPC) on ancient counting sema
  11, six engine workers on late binary semaphores sharing one creator,
  plus 10 sleepers incl. main. No cycle: the dispatcher is starved of
  async arrivals (model IOP purely synchronous), workers of dispatch;
  input stays a live alternative for the menu-shaped wait. Next: pad
  probe, then synthesized async SIF completion
  (`docs/reverse-engineering/m30-slice49-waiter-classes.md`).
- M30 slice 48 (2026-10-03): **the wait-for graph — event-starved, not
  deadlocked**. From the 243.7M checkpoint, the final leg shows 3,123
  injections with zero unblocks and zero signal/wakeup/release calls;
  VBlank + timer-2 chains are effect-free (empty chained slot). 10
  sleepers, 7 sema-waiters, no cycle — the stop needs an unmodeled event
  (async IOP completions lead, then input, then GS-side; stuck SIF0 CHCR
  cleared as vestigial). The resumed leg reproduces the original ending
  bit-for-bit — the large-N checkpoint proof in passing. No model change
  (`docs/reverse-engineering/m30-slice48-wait-graph.md`).
- M30 slice 47 (2026-10-03): **no wall through 243M — the machine idles**.
  A 5x run (10B steps) ends early with **243,711,723 services handled**
  (241.8M module calls, 7.1B interpreted steps), exit 0, no fault: the
  boundary is **no-runnable-thread at 0x00001604** — every thread parked,
  idle delivery waking nothing, 245,036 interrupts pending. Either the
  boot at rest or a distributed stall; the wait-for graph decides
  (`docs/reverse-engineering/m30-slice47-no-wall-to-243m.md`).
- M30 slice 45 (2026-10-03): **pin the divergent lookup — it asked for
  /fonts/system.fnt**. A temporary entry log (14 lines in 15M services)
  captured the query bytes, the manager input and the ordered chain; the
  file exists in the archive's fonts table on both layers. The mechanism is
  complete: the even FT01 object's `0x30/0x58` fields relocate into
  self-pointers, walked as offset tables, and an entry turned absolute
  faults exactly at `0x009CF08F`; low memory is all zeros. Next is
  comparing the loaded object's offset tables against the file's header
  bytes (loader divergence vs lifecycle timing)
  (`docs/reverse-engineering/m30-slice45-the-fatal-query.md`).
- M30 slice 44 (2026-10-02): **why the relocation pointer is odd** — the
  odd value was never stored: a write watch over the fault object shows it
  zeroed by the allocator path and pattern-filled byte-wise by a
  transforming copy (no disc region matches), so the relocate family
  derived it by walking data as pointer tables (live stack reaches
  0x00498b28 through the 0x00491e80 trampoline; the reported pc is stale).
  The `+0x94` writers store only zeros, heap objects or field copies, so
  the lookup designated the wrong object: model-fed divergence upstream
  (high confidence), no model change shipped
  (`docs/reverse-engineering/m30-slice44-why-the-pointer-is-odd.md`).
- M30 slice 43 (2026-10-02): **beyond the step limit — the steady pump and
  the next wall**. The step budget is now a `gt4boot --steps N` flag
  (default unchanged; CTest `gt4boot_steps` pins `--steps 1000`). The
  200M-step stop (pc 0x0055e790) is mid-copy in the per-packet routine
  0x0055e6d0, not a stall. The service mix over 3,648,011 services is
  stationary and the stop-time threads are healthy — but only 8 disc reads
  occur in 1.5M services, so the pump is RPC chatter, not media streaming.
  A 10x run finds the next wall: an **unaligned fault at pc 0x00491798**
  (a move-relocation routine) on 0x009cf08f after 15,010,045 services,
  deterministic across two full runs (same pc, address and count); the
  direct caller is 0x0048fb94 (argument = the return of 0x00491d90)
  (`docs/reverse-engineering/m30-slice43-beyond-the-step-limit.md`).
- M14 (2026-10-01): live observation through PCSX2 PINE — the reconstructed
  text image matches live GT4 RAM byte-for-byte (5,339,668 bytes, equal
  hashes), reginfo 24/24; data-record differences are runtime writes. Slice 2
  decodes the menu savestate offline: pc, all 32 GPRs, HI/LO and key CP0
  registers (the savestate's own eeMemory re-verifies the text image with 0
  differences). Savestate anchors: PINE slot 9 and the owner's slot 1
  (`docs/reverse-engineering/m14-live-observation.md`).
- EXPLAIN: lessons written for M6, M7 and M8 (`docs/lessons/`); the M9-M30
  lessons and retroactive M2-M5 notes remain open.
- Next technical milestone work: **M32 — trace the walk live
  (slice 17)**. Slice 16 proved due test + dispatch is the only
  signaling path (unlink callers mapped, no cancellation suffered, no
  hidden signalers): instrument the reference interpreter with a
  temporary pc-triggered trace at `0x005b822c` logging current, target,
  COUNT, overflow, and head per test. M35 pad stays queued (no PADMAN
  bound at this phase); and the curriculum's remaining units (the OSD
  configuration services, the remaining BIOS services and the
  jump-table dispatch) stay listed in `docs/requirements.md`.

## Environment (this machine, `C:\Antigravity\gt4-staticrecomp`)

- Build: VS 2022 Build Tools 17.14 + MSVC 19.44 + Ninja 1.13.2 + CMake 4.3.1;
  commands in `AGENTS.md` and `README.md`.
- Tests: 32/32 CTest (the translation tests, `gt4run` and `gt4boot` exist
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
  module entries (correctness first; 278,120 instructions in the boot run).
  The two performance alternatives (resume entries per halt address, inline
  syscall calls in generated code) remain open.
- **The boot now reaches the game's running state**: the model IOP answers
  the RPC binds and calls (version query with the game's compatibility
  constant; empty results otherwise), survives the IOP reset, and the
  **idle VBlank source** (decision 0010) wakes the game's threads. The run
  hits the 3,000-service limit inside the runtime with the state identical
  to the interpreter at 7,508,945 instructions. The **timer tick and DMA
  completion** sources of decision 0011 run the game's TIM2 handler every
  idle frame (it reprograms COMP) and complete the VIF1/GIF chains, the
  **semaphore handle shape** of decision 0012 (ids 3, 7, 11, ...) carries
  the long run from 3,645 to **9,765 services**, and the **handler execution
  fix** of decision 0013 (no nested injections, no preemption inside a
  handler) unblocks the delay callbacks: the boot now runs **continuously
  (1,000,000 services, 33,650,798 interpreted steps, about 29 seconds)**.
  The **service handshakes** of decision 0014 then unblock the loading path:
  the version queries answer the game's own compatibility constants, the
  disc subsystem's status query and the fileio/CDVD negotiation pass, and
  the boot binds the disc subsystem's server family and creates its
  worker-thread pool — an **11-thread runtime with string-coded servers**
  — reaching the **200,000,000-step limit inside the 0x0058F000 subsystem
  init** (pc 0x00590A18). The **register mirror and the liblgdev sync** of
  decision 0015 then clear that init: the command-layer spin at 0x00590A18
  ends when the model mirrors the game's `SET_SREG` back, the device library
  binds (server 0x046D046D) and its sync passes with the completed status
  0x010B2400 — the boot then runs its **device polling round** (1,000,000
  services, 1,710,779 module calls, 46,608,011 interpreted steps). The
  **service clock** of decision 0016 gives the model a time base that
  advances while code runs (one millisecond per handled service, the delay
  library's unit, called identically by both engines), so the main thread's
  delays expire and the long run ends at a service boundary with the worker
  threads ready (1,193,971 module calls, 32,878,366 interpreted steps). The
  remaining frontier: the polling round's calls' real replies.
- The cooperative scheduler was **exercised end to end by the boot run** in
  the fifth slice (the game's own CreateThread/StartThread/ChangeThreadPriority/
  WaitSema sequence) and now runs up to twelve threads under VBlank and timer
  wakeups, with thread switches deferred while a handler runs (decision
  0013). No timer preemption is modeled (decision 0005); equal-priority
  dispatch is creation order, not the kernel's rotation; pending causes are
  delivered at driver unit boundaries (module calls and interpreted steps),
  so busy code is interrupted like the hardware, and the idle source's
  200,000-interrupt budget only bounds a machine where every thread waits.
- The `jr ra` fall-through bug found in the fifth slice shows the limit of
  hand-picked differential modules: widen the verified surface
  (`gt4boot --compare-interpreter`) when new control-flow shapes appear.
- A module call runs to its own boundary and cannot be interrupted; the work
  budget counts interpreted instructions and module calls, so a loop inside a
  module is not bounded by it. No such loop has been hit before a boundary.

## Next actions

1. M30 slice 22: **the library's parse of the root directory block** —
   trace the game's own driver parse (the library's scan at 0x00548E20 and
   the five-byte comparison at 0x00548E90) to see what it expects after
   reading the ISO's volume descriptor and root directory, and answer it
   from the disc; the acceptance evidence is
   `gt4boot --compare-interpreter --disc <iso>` through the loads with the
   state identical.
2. Performance: resume entries or inline syscall calls to shrink the
   interpreted gaps; jump-table dispatch for computed `jr` into local blocks.
3. The M9-M30 lessons and retroactive M2-M5 notes if useful.
4. Keep the journal and this file current after every working session.

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
  program), slice 2 is the BIOS service layer plus the interpreter bridge
  (`gt4boot` runs the whole game as one module through SetupThread and
  SetupHeap with state identical to the interpreter, stopping at CreateSema
  in the thread/semaphore init), slice 3 is the thread scheduler and the
  semaphore services (the boot now passes both CreateSema calls and stops at
  the kernel-patch wall, SetSyscall), and slice 4 is the kernel-patch
  services (SetSyscall, the synthetic table and the stub return; the boot
  patches FindAddress/Copy, runs the SDK's search through the game's own
  helper, and stops at the EE timer hardware), and slice 5 is the timer
  registers and interrupt handlers (the whole `_InitSys` tree and the game's
  thread creation run, the cooperative scheduler is exercised end to end, a
  `jr ra` translator bug is found and fixed, and the run stops at
  GetOsdConfigParam), slice 6 is the OSD configuration and the device
  register banks (the run stops at SifSetDChain, the IOP wall), slice 7
  is the SIF layer with the model IOP and interrupt injection (the SIFCMD
  handshake completes, the game's own DMA handler runs as an injected
  interrupt, RPC initializes, and the run stops at the RPC bind wait), and
  slice 8 is the model IOP's RPC layer with the peripheral windows and idle
  VBlank delivery (the boot reaches the game's running state; 3,000
  services handled with the state identical to the interpreter at 7,515,389
  instructions), and slice 9 is the timer ticks and DMA channel completions
  (the game's TIM2 handler runs every idle frame and the VIF1/GIF chains
  complete; the differential passes at 3,000 services with the interpreter
  reference at 7,508,945 instructions), and slice 10 is the semaphore
  handle bits and the delay library (ids 3, 7, 11, ... carry the long run
  from 3,645 to 9,765 services), and slice 11 is the timer library's nodes
  (the delay nodes are scheduled and active; the TIM2 handler's due
  condition never passes; the idle budget rose to 200,000), and slice 12 is
  the handler execution fix (no nested injections, no preemption inside a
  handler; the delay callbacks fire and the boot runs continuously —
  1,000,000 services, 33,650,798 interpreted steps), slice 13 is the
  boot's service handshakes (the version queries answer the game's
  constants, Deci2Call is accepted, the disc subsystem status and the
  fileio/CDVD negotiation pass, and the RPC server table holds 80 slots; the
  boot runs an 11-thread worker pool to the step limit inside the 0x0058F000
  subsystem init), and slice 14 is the SIF register mirror and the loading
  path (the command-layer spin ends when the model mirrors `SET_SREG` back;
  the liblgdev device sync answers the completed status 0x010B2400 and the
  boot runs its device polling round to the 1,000,000-service limit —
  1,710,779 module calls, 46,608,011 interpreted steps), and slice 15 is the
  service clock (the time base advances one millisecond of BUSCLK ticks per
  handled service, called identically by both engines; the main thread's
  starved delays expire and the long run ends at a service boundary with the
  worker threads ready — 1,193,971 module calls, 32,878,366 interpreted
  steps; TIM2 measured at 576.05 ticks per service), and slice 16 is the
  disc image backing the file service (the model reads the pinned ISO and
  answers the file server's open with the real sizes, so the boot walks its
  IOP module list: SIO2MAN 6,641; MCMAN 96,181; MCSERV 7,385; SIO2D 11,289;
  DBCMAN 15,653; DS2U_D 11,821; LIBSD 30,085; USBD 34,993), slice 17 is the
  archive path's reconnaissance (the engine's two file layers, the
  movie-phase `/mpeg` load, the PCDV protocol's shape and the GT4.VOL
  header, XOR-0xFF name table and directory tree; no model behavior
  changed), and slice 18 is the GT4.VOL reader (lazy, validated parsing;
  the 22 root categories, the `mpeg/gt4` chain and `mv0010`'s 18,874,372
  bytes verified against the pinned archive, with the file item records
  recorded as not yet pinned), and slice 19 probes those file item records
  (97 clean three-word records `mv0011`..`mv0107` with position and packed
  size, then a mix of entry references, name references and fields; three
  candidate grammars tested, none closes, so no parser shipped — the next
  slice pins the boundary from the game's own consumer), and slice 20 is
  the game's own CD driver reading the disc (the PCDV read answers from the
  disc image's raw sectors; the driver walks the ISO — LBA 0x10 the volume
  descriptor, LBA 0x105 the root directory — and the external GT4FS
  reference corroborates the format family and supplies the entry
  semantics for the next slice), and slice 21 is the driver's disc walk and
  the volume's version family (the driver reads the ISO's volume descriptor
  and root directory and then stops; the pinned volume is the uncompressed
  2.2 variant the GT4FS packer also writes, with its page table at +0x20
  and pages that do not inflate — deflate and archive-offset reads were
  tried and rejected).
