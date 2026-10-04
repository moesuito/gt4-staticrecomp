# 0033 - DMA with real payload, tags and completion (P06)

Date: 2026-10-04. Status: accepted (implemented in slice 69).
Predecessors: 0011 (idle DMA completions), 0021 (PRTS block cache),
0028 (checkpoint baseline and compatibility), 0029 (P01+P02
origin/pending/mask/dispatch), 0030 (P03 advance machine),
0031 (P04 handler arguments and idle), 0032 (P05 JR and module exit).
Plan: PLAN.md section 6, P06 (no recorte alcancado pelo boot).
Closes PLAN.md C04/C05 in the DMA-transfer half (routing already
fixed by 0029; this slice owns payload, tags and completion).

## Context

Since slice 9 the VIF0/VIF1/GIF channels completed a started transfer
at once with no transfer engine: STR cleared, the channel's DMAC cause
reported, TIE stored but never gating (0029). The boot programs these
channels for real (VIF1 0x1C5, GIF 0x185 observed since slice 9), so
every completion confirmed a transfer that never happened. P06
requires the transfer, the chain walk, coherent registers, completion
only on conclusion, and a loud stop with context wherever the
implemented subset ends - with the explicit prohibition on inventing
TAG END, on completing without transferring, and on routing completions
through the VIF command IRQs (INTC 4/5) or Timer0 (INTC 9).

## Decision

1. **The channel owns a source-chain engine.** `DmaChannel` reads tags
   and payloads from the mapped guest memory on a CHCR STR write.
   Register layout and chain semantics follow PS2Tek DMAC I/O and
   Chain Mode (revision 295bc61) with the tag ids of ps2sdk
   `dma_tags.h` (REFE 0, CNT 1, NEXT 2, REF 3, REFS 4, CALL 5, RET 6,
   END 7; tag bits 0-15 QWC, 28-30 ID, 31 IRQ, 32-62 ADDR aligned,
   63 SPR): CHCR +0x00 (DIR 0, MOD 2-3, ASP 4-5, TTE 6, TIE 7, STR 8,
   TAG 16-31), MADR +0x10, QWC +0x20 (low 16 bits, like PCSX2's
   masked QWC write), TADR +0x30, ASR0/ASR1 +0x40/+0x50.
2. **Normal mode moves QWC quadwords from MADR** into the channel's
   device sink, with PCSX2's hardware-tested DmaExec rule (QWC 0 moves
   0x10000). Chain mode walks the source chain at TADR with the
   reference update table (CNT/NEXT/REF/REFS/CALL/RET/END, END
   leaving TADR on the tag, RET with an empty stack ending, the
   IRQ+TIE combination ending after the payload and the chain
   update). TTE moves the tag's upper 8 bytes before the payload.
   REFS moves its payload like REF; its D_CTRL stall handshake stays
   an explicit limit.
3. **Completion only on conclusion.** `raise_` (the channel's DMAC
   cause, never an INTC cause) fires after the implemented transfer
   concludes: STR cleared, QWC 0, MADR/TADR post-transfer, CHCR TAG
   field set to bits 16-31 of the last tag read, ASP tracked. The
   guest's STR poll and the DMAC CIS bit therefore answer the real
   operation. Pending still exists without a handler and the mask
   still decides delivery (0029, unchanged).
4. **Unknown stops loudly with context, never completes.** DIR clear,
   interleave/reserved modes, a chain start with QWC set (the PS2Tek
   resume rule, unmodeled), scratchpad-selected addresses, unaligned
   or unmapped tag and data addresses, a device-window source, CALL
   with a full stack, and walks past 65536 tags throw naming the
   channel, the pc-side registers and the offending address. A failed
   start keeps STR set (like a hung channel) and stays in the start
   log with completed false. No TAG END is ever invented.
5. **SIF keeps its service path.** SIF0 stays a plain bank holding the
   model's own SifSetDChain enable (0x184); every SIF payload moves
   through the synchronous SifSetDma service copies (unchanged). The
   90k boot shows SIF0 MADR/QWC/TADR all zero: no guest register
   start exists to complete.
6. **Diagnostics are not state.** The start log (programmed registers,
   tags walked, bytes moved, first/last tag ids, completed) and the
   retained sink bytes with their FNV-1a hash are runtime capture for
   the M33 packet work. Snapshots keep the bank-only format and a
   restore restarts diagnostics empty, so checkpoint compatibility is
   unchanged and no model version moves (0028 gate: same register
   vocabulary, transfers re-run deterministically from registers
   plus RAM, and no checkpoint can capture a mid-transfer state
   because transfers complete inside one MMIO write).
7. **Memory lifetime is explicit.** The engine reads through a
   back-pointer set by `map_into` and re-pointed by `rebind_memory`
   after `make_boot_state` moves the memory into the GuestState (the
   slice's own dangling-pointer fault, found loud on the first boot
   leg, fixed the same way in tests by construction).

## Consequences

- The boot's guest-visible DMA contract is now real transfers: 90k
  services with the disc move VIF0 1x [ref..end] (3 tags, 2,712
  bytes), VIF1 1334x ([cnt..end] x1333 plus [ref..end] x1, 4,011
  tags, 582,104 bytes) and GIF 1333x [next..end] (2,666 tags,
  0 bytes: stereotyped empty chains, one per VIF1 frame upload).
  CHCRs read 0x70000045/0xC5/0x85 (TAG names END), D_STAT reads
  0x00270027 (CIS 0/1/2/5 pending with masks open).
- The module-executed guest path is unchanged by real values:
  module calls at 90k are byte-identical to pre-P06 (215,013), same
  stop, same service shape.
- Out of scope, unchanged: JR/ERET (P05), RPC content (P07),
  quanta/clock (P03), interrupts/masks (P01+P02), VIF/GIF payload
  consumers (graphics stages). Known inherited limits: sink
  retention is runtime-only until a streaming consumer replaces it;
  REFS stall, MFIFO, D_CTRL/DMAE gates and the resume rule wait for
  observed need; stop-time tag dumps on the reused ring buffer stay
  corroboration only.
- Two pre-existing findings, reproduced on the pristine tree and
  handed to P07-P09, are NOT P06 regressions: a driver-vs-
  interpreter RAM-window divergence first visible at 1,600-1,700
  services (DMA walks are identical through 1,500), and the
  `gt4boot_services` CTest passing by regex while the process exits
  1 on that divergence (the gate is blind to the differential
  outcome; P08 owns making it honest).

## Verification

Extended `ee_device` unit rows (normal payload + register walk,
CNT..END, boot-value IRQ+TIE cut with TTE order, IRQ-without-TIE
continuation, CALL/RET stack round-trip, QWC-0 normal rule, six
loud-stop rows, restore restarts diagnostics) plus the re-pinned
`ee_timer` chain-completion rows; full build warning-free (MSVC
19.44, Ninja, Debug); CTest 51/51; Python 73 (67 run, 6 skip, OK,
exit 0); added-lines ASCII scan clean. Boot legs: 1,200/1,500/1,600
identical driver-vs-interpreter (with DMA walking), 1,650/1,700/
2,000/3,000/90,000 diverging exactly like pristine main. Evidence:
`docs/reverse-engineering/slice69-p06-dma-payload.md`.
