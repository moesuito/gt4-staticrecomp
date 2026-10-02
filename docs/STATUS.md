# Project status

Updated 2026-10-02 after M30 slice 15 — the service clock: the model's time
base now advances by one millisecond of BUSCLK ticks per handled service
(the delay library's unit), with both engines calling the same kernel method
at their service boundaries so the differential stays exact; timers follow
their CLKS selector and fire on crossing COMP, and one VBlank joins the
queue per frame of slices. The delays that starved the main thread now
expire: the 1,000,000-service run ends at a service boundary with the worker
threads ready (1,193,971 module calls, 32,878,366 interpreted steps). The
differential passes at 3,000 services (interpreter reference at 7,570,583
instructions, full state identical). This is the first document to read in a
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
- M14 (2026-10-01): live observation through PCSX2 PINE — the reconstructed
  text image matches live GT4 RAM byte-for-byte (5,339,668 bytes, equal
  hashes), reginfo 24/24; data-record differences are runtime writes. Slice 2
  decodes the menu savestate offline: pc, all 32 GPRs, HI/LO and key CP0
  registers (the savestate's own eeMemory re-verifies the text image with 0
  differences). Savestate anchors: PINE slot 9 and the owner's slot 1
  (`docs/reverse-engineering/m14-live-observation.md`).
- EXPLAIN: lessons written for M6, M7 and M8 (`docs/lessons/`); the M9-M30
  lessons and retroactive M2-M5 notes remain open.
- Next technical milestone work: **the device polling round's replies** — the
  boot drives the liblgdev RPCs 6, 13 and 15 and the string-coded servers
  ("Pusb", "PUPS", "MGBP", "PCDV") with a steady polling round; the next
  slice decides whether the empty replies hold the game back and answers the
  first of them whose reply the game acts on (with the live PCSX2 emulator
  as the oracle for the real replies).

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

1. M30 slice 16: **the device polling round's replies** — decide whether the
   steady liblgdev (RPCs 6/13/15) and string-coded server (RPCs 1/3/4/8)
   round is held back by the model's empty replies, and answer the first of
   its calls whose reply the game acts on (the live PCSX2 emulator is the
   oracle for the real replies); the acceptance evidence is
   `gt4boot --compare-interpreter` through the round with the state
   identical.
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
  steps; TIM2 measured at 576.05 ticks per service).
