# 0011 — Timer ticks and DMA channel completions at idle

Status: implemented 2026-10-02 for the M30 ninth slice
(`docs/reverse-engineering/m30-slice9-timer-and-dma-completions.md`).

Context: after the eighth slice the boot reached the game's runtime but
stopped with every thread waiting on semaphores created by the game's delay
helper (0x005AED18: CreateSema, schedule, WaitSema, DeleteSema). The
evidence showed two missing device behaviors: the EE timers never counted
(the game's TIM2 handler reads `MODE & EQUF` and the timer library's clock
is TIM2's count), and the VIF0/VIF1/GIF DMA channels never completed (their
CHCR start bits stayed set and their handlers never fired).

Decision:

- **The timers tick at idle.** When the machine goes idle, every timer whose
  MODE has the count-up enable (CUE, 0x80) advances its COUNT by one frame of
  its clock source — BUSCLK, BUSCLK/16, BUSCLK/256 or the horizontal blank
  rate for CLKS values 0-3 (147,456,000 Hz at 60 Hz: 2,457,600, 153,600,
  9,600 and 262 counts). The compare flag (EQUF, 0x400) is set and, when the
  compare interrupt is enabled (CMPE, 0x100), the timer's INTC cause is
  raised (TIM0=9, TIM1=10, TIM2=11, TIM3=12). Overflow wraps the 32-bit
  count and sets OVFF (0x800) when the overflow interrupt is enabled.
  There is no cycle-accurate clock: one compare per idle frame is the
  model's granularity, and the guest's handler sees the count move a whole
  frame's worth of clocks.
- **DMA channels complete a started transfer at once.** A write to a
  channel's CHCR that sets the start bit (STR, 0x100) clears it immediately
  — so polling code sees the transfer finish — and, when the transfer
  interrupt is enabled (TIE, 0x80), raises the channel's INTC cause
  (VIF0=4, VIF1=5, GIF=9). The model has no transfer engine: no data moves,
  no FIFO fills, no chain is walked. SIF0 stays plain storage because its
  "arming" write (`SifSetDChain` writes 0x184) is not a transfer; the SIF
  completions come from the model IOP instead.
- **INTC and DMAC handler tables are separate.** `AddIntcHandler` (0x10)
  registrations live in the INTC list keyed by cause; `AddDmacHandler` (0x12)
  registrations live in a DMAC list keyed by channel. The model IOP's SIF
  replies now raise a **DMAC channel 5 completion**: the DMAC status register
  (0x1000E010) gets the channel's bit and the channel's handlers run — which
  is exactly the path the SDK's `sceSifInitCmd` sets up
  (`AddDmacHandler(5, ...)`). Before this split the two kinds shared one list
  and a cause-5 interrupt could run the SIF handler and the VIF1 handler
  together.
- **The idle budget rises to 6,000.** The game's retry and delay loops wait
  for frames of virtual time to elapse; 6,000 consecutive idle interrupts
  (about 100 seconds of NTSC frames) without a runnable thread is the new
  guard before the driver reports the no-runnable-thread boundary. The cost
  of an idle interrupt is small (one handler invocation).

Alternatives considered:

- **A cycle-accurate timer clock.** The model has no cycle counter; counting
  instructions would differ between the translated module and the interpreter
  and break the differential comparison. The frame-granular tick is
  deterministic and both engines see it identically.
- **Distinguishing "armed" from "started" channels by watching TADR.** The
  observed arming write (SIF0's 0x184) is distinguishable by channel, so the
  simpler channel-scoped rule was kept and documented.
- **Calling the game's SIF handler from the INTC cause 5 path.** That was the
  old accidental behavior; the SDK's own registration (AddDmacHandler(5))
  proves the completion belongs to the DMAC dispatch.

Consequences and limits:

- The game's TIM2 handler now runs every idle frame: it reads COUNT/MODE,
  services its stream list and reprograms COMP (observed: COMP moved from
  0x240 to 0x46C0F3). The VIF1/GIF chains complete and their handlers run.
- The differential still passes: 3,000 services, interpreter reference at
  7,508,945 instructions, full state identical.
- **Open frontier**: the game's own delay/software-timer callback chain does
  not fire yet. The delay helper schedules through the game's timer library
  (0x005B8F38 → 0x005B8C60 → 0x005B8B68) and waits on a semaphore; at the
  stop the library's active list holds only its two audio stream nodes and
  the waiters remain. The next experiment is to find which library path links
  and fires the delay node (the evidence trail is in the slice-9 document).
- Timers tick only at idleness, never during a long computation, and the
  count advance is one frame per idle interrupt, not a real clock.
