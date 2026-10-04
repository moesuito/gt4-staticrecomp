# Slice 69 (P06) - DMA with real payload, tags and completion

Date: 2026-10-04. Inputs: pinned CORE, pinned ISO, main at abf7126.
Plan: PLAN.md section 6, P06. Decision: 0033.
Scope: VIF0/VIF1/GIF + SIF register starts in the boot's reach, real
subset transfer, chain walk with coherent registers, completion only
on conclusion, VIF/GIF consumer integration when reached (not
reached: the sink retains). No JR/ERET, RPC, clock or mask work.

## References pinned first (all corroborating, no single source)

- Tag ids and 64-bit tag layout: ps2sdk `ee/dma/include/dma_tags.h`
  (REFE 0 .. END 7; QWC 0-15, PCE 26-27, ID 28-30, IRQ 31, ADDR
  32-62 masked 0x7FFFFFFF, SPR 63) and the `DMATAG` macro pack.
- Chain semantics: PS2Tek DMAC Chain Mode @295bc61 (source-chain
  table, TTE upper bytes before QWC, IRQ+TIE ends after QWC, END
  keeps TADR, resume rule) and PCSX2 `hwDmacSrcChainWithStack` /
  `hwDmacSrcChain` / `vif1SetupTransfer` (same table, CALL/RET stack,
  TIE+IRQ order, TAG kept for Soul Calibur II/III).
- Registers: PS2Tek DMAC I/O (CHCR bits, MADR +0x10, QWC +0x20 low
  16, TADR +0x30, ASR0/ASR1 +0x40/+0x50, D_STAT CIS/mask layout) and
  ps2sdk `ee/dma/src/dma.c` (`dma_channel_send_chain` programs QWC 0,
  MADR 0, TADR data, CHCR DIR/chain/TTE/TIE/STR - exactly the boot's
  0x1C5/0x185 values; `dma_channel_wait` polls STR).
- Completion-always: ps2autotests dmac/tagintr @97469ff via 0029.
- Normal QWC 0 = 0x10000: PCSX2 DmaExec hardware-tested comment.

## What changed (5 files, no harness, no branches)

- `include/gt4recomp/ee_device.hpp`: channel offsets, CHCR/tag
  constants, `StartRecord` (programmed regs, tags, bytes, first/last
  tag ids, completed), sink + FNV-1a accessors, `rebind_memory`.
- `src/ee/device.cpp`: normal engine, source-chain walker, six
  loud-stop families, QWC 16-bit masking, TAG/ASP evolution,
  completion only on conclusion, diagnostics restart on restore.
- `tests/unit/ee_device_test.cpp`: live normal transfer (payload,
  MADR walk, QWC drain, STR clear, cause, start log), CNT..END,
  boot-value 0x1C5 IRQ+TIE cut with TTE order, IRQ-without-TIE,
  CALL/RET round-trip, QWC-0 rule (1 MiB), six loud stops
  (DIR, QWC-set chain, unmapped tag, interleave, NEXT-loop budget,
  failed-start log), restore restarts diagnostics.
- `tests/unit/ee_timer_test.cpp`: chain block maps its tag page
  (TADR 0x100000); zero tags walk REFE and complete, assertions
  unchanged.
- `tools/gt4boot/main.cpp`: BootDevices `rebind_dma` after the
  GuestState move; `--threads` prints MADR/QWC/TADR, start/tag/byte/
  sink/hash totals, per-shape `[first..last xN]` histogram, full
  SIF0 registers and D_STAT.

## The slice's own bug, caught loud

First boot leg faulted: `DMA channel at 0x10008000: tag at
0x006DE5A0 inside a device window` - a dangling `GuestMemory*`:
`map_into` ran on the pre-move local inside `make_boot_state`.
Fixed with `rebind_memory`/`rebind_dma` after the move (one call
site covers all four legs). Unit tests bind memory that outlives
the channel, unaffected. Lesson recorded: back-pointers across a
move need a named rebind step, not a comment.

## Captured: the boot's first DMA starts (90k services, disc)

Driver leg, deterministic across runs (module calls 215,013,
byte-identical to pre-P06):

| ch | starts | shapes | tags | bytes | sink hash | stop CHCR/TADR |
|----|-------:|--------|-----:|------:|----------:|----------------|
| VIF0 | 1 | [ref..end] x1 | 3 | 2,712 | 0xe44b9f8d | 0x70000045 / 0x006DE5C0 |
| VIF1 | 1,334 | [cnt..end] x1333, [ref..end] x1 | 4,011 | 582,104 | 0x9c364d2d | 0x700000C5 / 0x0077ECD0 |
| GIF | 1,333 | [next..end] x1333 | 2,666 | 0 | 0x811c9dc5 (empty) | 0x70000085 / 0x0077ECE0 |
| SIF0 | 0 | - | - | - | - | 0x184, MADR/QWC/TADR zero |

- Every live chain ends in END reached by walking, none by
  invention; TAG fields read 0x70000000 (last tag END, IRQ 0) with
  STR clear. GIF moves nothing: 1,333 stereotyped NEXT(QWC0) to
  END(QWC0) pairs, one per VIF1 frame upload (1,333 = 1,333).
- D_STAT 0x00270027: CIS bits 0/1/2 (VIF0/VIF1/GIF) and 5 (SIF0
  service traffic) pending with masks open - pending conserved per
  0029, delivery guest-gated. DMAC handlers 0/1/2 registered at
  0x004AB6D8; INTC 4/5 and 9 never touched by completions.
- SIF payloads keep moving through the SifSetDma service copies
  (unchanged); no guest register-level SIF start exists.
- Stop-time tag dumps on the reused 0x0077ECxx ring show QWC-0 END
  leftovers and one REFE-54: corroboration only, not attribution
  (the engine's per-start shape log is the attribution).
- What is transferred vs the declared limit: every programmed byte
  the subset covers lands in the sink in transfer order (unit-pinned
  byte-exact; boot totals + hashes above). NOT transferred: VIF
  unpack/GIFtag parsing (no consumer yet - graphics stages), REFS
  stall handshake, MFIFO, scratchpad-selected addresses, resume
  rule, interleave. The sink retains runtime-only until a streaming
  consumer replaces it.

## Differential status, honestly

- 1,200 / 1,500 / 1,600 services no-disc: driver == interpreter
  WITH the engine walking (VIF0 1x, VIF1 3-6x, GIF 2-5x).
- 1,650 / 1,700 / 2,000 / 3,000 / 90,000 (disc and no-disc): `state
  differs in the guest memory window`.
- Control: `git stash` to pristine abf7126, rebuild, same 1,650 and
  90,000 legs fail identically (same message; 90k module calls
  215,013 in both). PRE-EXISTING, not a P06 regression. First
  window 1,600-1,700 services; both with and without disc.
- Gate blindness (observed, CTest 4.3.3): `gt4boot_services` passes
  by `PASS_REGULAR_EXPRESSION "services handled 90000"` while the
  process exits 1 - the regex overrides the exit code, so 51/51
  coexists with the broken differential. P08 owns making the gate
  honest; P09 owns locating the divergence (JR/ERET territory,
  explicitly not touched here).

## Verification

- Full build warning-free (MSVC 19.44, Ninja, Debug; gt4boot via
  its on-demand target).
- CTest 51/51, 65 s.
- Python: 73 collected, 67 run, 6 skip, OK, exit 0 (~60 s; two
  known socket ResourceWarnings).
- Added-lines ASCII scan: 838 added lines, 0 bytes above 127 (the
  3 + 16 non-ASCII bytes in the two touched files pre-date the
  slice: em-dashes and section signs in old comments).
- No commit, no push, no branches (working tree handed over).

## What P07 inherits

- A working packet tap: per-start shapes, counts, bytes and hashes
  for every VIF/GIF start, ready for the RPC-traceable inventory
  to correlate SIF traffic against.
- The blind gate: P07's "first unknown" work needs the differential
  to fail loudly first, or new RPC findings will hide behind the
  same regex.
- The 1,600-1,700 RAM-window divergence as the first open
  driver-vs-interpreter item (pre-existing, DMA-exonerated).
- Open DMA items, all declared: sink streaming consumer, REFS
  stall, MFIFO/DMAE gates, resume rule, scratchpad selection,
  interleave, per-start tag-ID histograms if the consumer wants
  more than end shapes.
