# Slice 65 — P01+P02: timer registers and interrupt origin/pending/mask/dispatch

Date: 2026-10-04. Plan: PLAN.md section 6, P01 and P02 (slice 65 = P01+P02).
Decision: docs/decisions/0029-p01-p02-timers-interrupts.md.
Baseline: main dcb8fc3 (P00 committed), 50/50 CTest + Python 73 (6 skips).

## Contracts (with pinned sources)

Width, flags and acknowledge follow PCSX2 Counters.cpp/H at
81526d4dc7cc70e4ae75abb35a789417456c6d43 (count/target masked to 16
bits, overflow past 0xFFFF, `modeval &= ~(value & 0xC00)` W1C plus low
10 control bits, flags set only when their interrupt is enabled,
ZeroReturn reset gated on the target interrupt) corroborated by PS2tek
EE Timers at source revision 1c9166066c3ab9ad089a4d12088acaa9476df4b6
(four 16-bit timers, MODE bits 0-11, edge-triggered delivery) and by
PS2SDK timer.c at ac92a9f657d2e531dd8f060250b07f2a5ac6dea5 (InitTimer
sets CUE|CMPE|OVFE with COMP 0xFFFF and calls EnableIntc; EndTimer
stops by writing both flags as 1; the handler gates on EQUF; extended
time is `(overflows << 16) | COUNT` shifted by `CLKS * 4`).
INTC_STAT W1C plus INTC_MASK low-16 toggle come from PCSX2 HwWrite.cpp
at the same pin; D_STAT low-half W1C plus high-half toggle from PCSX2
Dmac.cpp at the same pin. TIE never gates completion per
ps2autotests dmac/tagintr at 97469ffbed8631277b94e28d01dabd702aa97ef3.

## What changed (files and lines, approximate)

- include/gt4recomp/ee_timer.hpp, src/ee/timer.cpp: TimerUnit is now
  four typed 16-bit timers (COUNT/MODE/COMP/HOLD), named MODE bits,
  guest W1C path, internal add_ticks with exact-landing advance and
  edge reporting, verbatim restore.
- include/gt4recomp/ee_device.hpp, src/ee/device.cpp: new IntcUnit
  (STAT/MASK) and DmacStatusUnit (CIS/CIM) with guest/internal/restore
  separation; DmaChannel always reports its DMAC channel on STR.
- include/gt4recomp/ee_kernel.hpp, src/ee/kernel.cpp: unit wiring,
  Enable/Disable setting mask bits (out-of-range fails), in-place
  coalescing in both domains, eligibility-checked dispatch (handler,
  mask, CP0 IE/EIE/EXL/ERL/INTx), 16-bit advances through the unit.
- include/gt4recomp/ee_checkpoint.hpp: time_model and interrupt_model
  1 -> 2 (pre-change checkpoints forensic via decision 0028).
- tools/gt4boot/main.cpp: VIF0/VIF1/GIF report DMAC 0/1/2 through
  raise_dmac_completion; all four run sites wire the units identically.
- tests/unit/ee_timer_test.cpp, ee_device_test.cpp,
  ee_kernel_test.cpp: extended in place (no parallel harness).

## Old expectations updated with evidence (PLAN.md 21.4)

1. Timer MODE 0x1234 round-trip -> control-only values round-trip
   (bits above 9 are flags/reserved, not storage).
2. Timer snapshot 0x11111111/0x782/0x7D573500 verbatim -> masked
   0x1111/0x382/0x3500 (0x782 names EQUF as 1, which acknowledges).
3. Idle EQUF every frame regardless of COMP -> EQUF only on a real
   crossing (the C02 defect); idle test now uses COMP 0x2000 and reads
   COUNT 0x2580.
4. DMA silence without TIE -> completion always reported (both TIE and
   non-TIE starts report); TIE stays stored for P06.
5. Coalescing erase-and-push -> coalesce in place (order across
   distinct causes preserved); DMAC completions coalesce like INTC.
6. Enable/Disable accepted no-ops -> they set/clear mask bits and
   reject out-of-range causes; every dispatch test now opens its mask.
7. SIF SET_SREG pending_before + 1 -> coalesced onto the pending SIF0
   entry (one CIS bit per channel).

## Test results and counts

- ee_timer: width (COUNT/COMP/MODE), W1C preserve/clear isolation,
  add_ticks crossings (plain, behind-counter silence, 0xFFF0 wrap,
  combined compare+overflow, stopped, gated, ZeroReturn), restore
  verbatim. All pass.
- ee_device: bank and channel restore bypass unchanged; new INTC
  (W1C, toggle on/off, mask/status independence, armed restore) and
  DMAC (CIS W1C, CIM toggle, independence, verbatim restore). Pass.
- ee_kernel: idle timer through the INTC contract; pending without
  handler plus enable-after-pending; CP0 gate close/reopen; mask
  contracts and guest-toggle separation; GIF completion on DMAC 2 with
  INTC 9 untouched; DMAC coalescing; in-place order; SIF path with
  mask open; service-clock compare silence and VBlank framing; the
  0xFFF0 + 576 -> 0x0230 wrap with OVFF and no EQUF. Pass.
- Full runs: CTest 50/50 (includes gt4boot_build plus the 90,000
  service disc differential, the 20,000-service originating pin
  unchanged, and all resume/autosave fixtures); Python 73 collected,
  67 run, 6 skip, with the two known socket ResourceWarnings.

## P01 acceptance

- High COUNT/COMP bits never extend the counter: covered (masked
  writes and reads, masked snapshot/restore).
- Zero flag write preserves pending: covered (MODE/STAT/D_STAT).
- One write acknowledges the pertinent flag: covered (EQUF/OVFF,
  INTC cause, DMAC channel, each in isolation).
- Restore free of execution effects: covered (verbatim restores for
  all three units; restores queue nothing).

## P02 acceptance

- Occurrence without handler stays in the status: covered (INTC and
  DMAC, queue plus status bit, dispatch refuses without dropping).
- Enabling later serves the pending: covered.
- Acknowledging one cause clears no other: covered (all three units).
- GIF completion never calls Timer0: covered (DMAC 2 sets CIS 2,
  INTC 9 stays clear; BootDevices rewired 4/5/9 -> 0/1/2).
- No ineligible delivery: covered (handler, mask, CP0 gate; masked
  and gated occurrences wait).
- Internal pending changes bypass the guest W1C/toggle path:
  covered by construction (internal setters) and by the armed
  restores.

## Declared pending for P03 (not implemented here)

The 1 ms/service and 1 frame/idle quanta, the per-timer service
remainders, and the two separate advance call sites are byte-for-byte
the old policy. P03 inherits: the unified advance machine, remainder
handling across idle/service, large-jump event opportunities with
handler reprogramming between events, full gate/ZeroReturn modes, the
DMAE/D_ENABLER gate (explicitly not modeled), dispatch priority
beyond FIFO (a documented model policy), and the guest extended-time
routine over the corrected counters.

## What P03 inherits concretely

TimerUnit::add_ticks is clock-agnostic (ticks in, edges out); the
kernel owns quanta and remainders. The first P03 experiment can keep
this split and only replace how steps are cut and interleaved. The
fossil checkpoints (time/interrupt 1) stay readable as evidence but
never resume under model 2.
