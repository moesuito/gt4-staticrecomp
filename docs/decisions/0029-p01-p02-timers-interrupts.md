# 0029 — Timer registers and interrupt origin/pending/mask/dispatch (P01+P02)

Date: 2026-10-04. Status: accepted (implemented in slice 65).
Predecessors: 0007 (timer window), 0011 (idle ticks, INTC-routed DMA),
0016 (service clock), 0025 (INTC coalescing), 0028 (compatibility policy).
Plan: PLAN.md section 6, P01 and P02.

## Context

The P00 baseline (0028) exists so timer/interrupt semantics can change
with old checkpoints refused as forensic. The GPT review (C01-C07) showed,
with probes against the real API, that COUNT/COMP were 32-bit, EQUF was
raised every idle frame regardless of COMP, MODE writes had no acknowledge
contract, VIF0/VIF1/GIF completions rode INTC causes 4/5/9 gated on TIE,
pending was dropped when no handler was registered, and Enable/Disable
were no-ops. The old unit tests pinned several of those approximations.

## Decision

1. **16-bit COUNT/COMP with named MODE bits.** TimerUnit owns four typed
   timers. Reads return the low 16 bits; writes keep the low 16 bits.
   MODE low 10 bits are control (CLKS, GATE, ZRET, CUE, CMPE, OVFE),
   bits 10-11 are EQUF/OVFF with write-1-to-clear. Sources: PCSX2
   Counters.cpp/H at 81526d4dc7cc70e4ae75abb35a789417456c6d43 and
   PS2tek EE Timers at source revision
   1c9166066c3ab9ad089a4d12088acaa9476df4b6.
2. **Three paths never mix.** Guest MMIO writes apply masking plus W1C;
   tick advances set flags by direct OR and report 0-to-1 edges;
   restores write storage verbatim and queue nothing. A compare behind
   the counter waits for the next wrap (exact-landing advance, no
   persistent future bit needed). ZeroReturn resets only with CMPE set
   (PCSX2 hardware-tested note). Gated timers do not advance (P03 owns
   gate modes). Compare and overflow in one advance report both edges.
3. **INTC and DMAC are separate pending domains.** New IntcUnit
   (STAT W1C, MASK low-16 toggle, same PCSX2 HwWrite/Dmac sources) and
   DmacStatusUnit (CIS W1C, CIM toggle) own their windows; the kernel
   records every occurrence in the status bit and the queue whether or
   not a handler exists. Dispatch peeks without popping until the cause
   proves eligible (handler registered, mask open, CP0 gate open).
4. **Masks have two writers, one bit.** Enable/Disable syscalls set/clear
   the cause's mask bit (privileged path); guest MASK writes toggle it.
   Both reach the same unit bit with their own contract.
5. **Dispatch checks the CP0 gate.** IE and EIE set, EXL/ERL clear, plus
   INT0 for INTC and INT1 for DMAC (PS2tek Status contract; the boot
   baseline 0x70030c11 has the gate open).
6. **Coalescing in place, FIFO across causes.** A repeat of a queued
   cause/CIS keeps its position (this corrects 0025's erase-and-push,
   which moved repeats behind distinct causes); DMAC completions now
   coalesce too (one CIS bit per channel). Order across distinct causes
   is a documented model policy, not a hardware claim.
7. **DMA completions always exist and ride DMAC.** A started transfer
   clears STR and always reports channels 0/1/2 (VIF0/VIF1/GIF); TIE is
   stored, never a completion gate (ps2autotests dmac/tagintr @97469ff:
   CIS after termination in every TIE combination). A GIF completion
   never touches INTC 9 (Timer0). VIF command IRQs 4/5 stay reserved as
   INTC causes for P06. The global DMAE/D_ENABLER gate is explicitly
   not modeled yet (nothing in the verified path programs it).
8. **Versions.** time_model 1 -> 2, interrupt_model 1 -> 2. Every
   pre-change checkpoint is forensic automatically (0028 gate).

## Consequences

- Old expectations updated with evidence (PLAN.md 21.4): timer 32-bit
  reads, EQUF-every-frame, TIE-gated silence, erase-and-push order,
  DMAC stacking, no-op enables, and the SIF SET_SREG double-queue each
  have a named replacement in the slice-65 evidence.
- The 1 ms/service and 1 frame/idle quanta are untouched: P03 owns the
  unified advance machine, remainders across paths, gate/ZeroReturn
  refinements, and the extended-time routine. The wrap fixture
  (0xFFF0 + 576 -> 0x0230 with OVFF) passes through the existing
  service path, not through a new scheduler.
- Out of scope, unchanged: JR/ERET (P05), DMA tags/payload (P06), RPC
  content (P07), handler a1/a2 (P04), thread accounting.

## Verification

Extended fixtures ee_timer, ee_device, ee_kernel (no parallel
harness); CTest 50/50; Python 73 (67 run, 6 skip). Live legs:
gt4boot_services 90,000 with disc and compare-interpreter green, and
the 20,000-service originating pin unchanged. Evidence:
docs/reverse-engineering/slice65-p01-p02-timers-interrupts.md.
