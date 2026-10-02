# M30, ninth slice — timer ticks and DMA channel completions

Date: 2026-10-02. Inputs: the pinned CORE. Follow-up to the eighth slice
(`m30-slice8-rpc-and-vblank.md`), whose frontier was three threads waiting on
semaphores created by the game's delay helper. This slice gives the model a
counting timer source and DMA channel completions, and separates the INTC
and DMAC handler tables. The differential still passes: 3,000 services,
interpreter reference at 7,508,945 instructions, full state identical.

## The evidence

The `gt4boot --threads` diagnostic at the frontier (before this slice):

```
thread 1: status 0x4, wait 2/36, prio 64, entry 0x00000000, pc 0x005adce8
thread 2: status 0x4, wait 2/3,  prio 0,  entry 0x005ae9a0, pc 0x005adce8
thread 3: status 0x4, wait 2/37, prio 0,  entry 0x005786f0, pc 0x005adce8
dma vif0 chcr 0x10000045   dma vif1 chcr 0x100001c5   dma gif chcr 0x10000185
timer 2: count 0x00000000, mode 0x00000782, comp 0x00000240
```

- A `WaitSema` trace with caller addresses showed both waits inside
  0x005AED18, the delay helper (CreateSema at 0x005AED60, schedule at
  0x005AED94, WaitSema at 0x005AEDB8).
- The VIF1 and GIF CHCRs had the start bit (0x100) set: transfers started and
  never completed.
- TIM2 was programmed with MODE 0x782 (CLKS=2 = BUSCLK/256, CUE, CMPE, OVFE)
  and COMP 0x240: a 1 kHz software timer tick.
- The INTC handler registrations (with their callers): TIM2 cause 11 at
  0x005B8158 (from 0x005B7C68, the timer/audio library init), DMAC channel 5
  at 0x005B0E30 (from 0x005B0A58, `sceSifInitCmd`), INTC causes 0/1/2 at
  0x004AB6D8 and 0x004AB430/0x004AB668 (display setup), INTC cause 5 (VIF1)
  at 0x004AB548.
- The game's timer library clock (0x005B8400) reads T2_COUNT, adds the
  overflow counter at 0x006592F0 and shifts by `2*CLKS`: the game's time is
  TIM2's count.
- The TIM2 handler (0x005B8158) checks `MODE & 0x400` (EQUF) and returns
  immediately when clear; when set it services its stream list, calls
  0x005B7F08 (which programs COMP from the remaining time) and clears EQUF by
  writing MODE through the KSEG1 mirror 0xB0001010.

## What changed

- **Timer ticks at idle** (decision 0011): enabled timers advance one frame
  of their clock source per idle interrupt, set EQUF and raise their INTC
  cause. After the change the game's TIM2 handler runs every idle frame and
  reprograms COMP (observed moving from 0x240 to 0x46C0F3).
- **DMA channel completions**: `DmaChannel` clears STR on a start write and
  raises the channel's cause when TIE is set (VIF0=4, VIF1=5, GIF=9). After
  the change the VIF1/GIF transfers complete (their CHCRs no longer hold
  STR) and the game's VIF1 handler runs.
- **Handler tables split**: INTC causes and DMAC channels have separate
  registration lists; the model IOP's SIF replies raise a DMAC channel 5
  completion with the DMAC status bit (0x1000E010 bit 5), which is the path
  `sceSifInitCmd` registers.
- **The idle budget** rose to 6,000 consecutive idle interrupts without
  progress (about 100 seconds of NTSC frames).

## Verified run

```
build/gt4boot.exe private/fingerprint-check/CORE.GT4 --services 3000 --compare-interpreter
boundary: syscall 0x005adbd4 service 0x33
stats: module calls 9361, interpreted steps 278120, services handled 3000
interpreter: 7508945 instructions, state identical (registers, HI/LO, FPU, VU0, CP0, pc, memory digest)
```

The injected-interrupt census over an 8,000-service run: INTC cause 11
(TIM2) 4,719 times, INTC cause 2 (VBlank) 4,719 times, DMAC channel 5 twelve
times, INTC cause 5 (VIF1) three times, INTC cause 9 (GIF) twice.

CTest **32/32** (`ee_timer` covers the DMA channel completion, `ee_kernel`
the DMAC dispatch with its status bit and the timer advance); Python 73
(67 run, 6 skip).

## The open frontier

The game's delay helper still waits: its semaphore is never signaled. The
trace of the library's structures at the stop:

- The delay helper schedules through 0x005B8F38, which pops a node from the
  free list at 0x0089C340, stores the caller's callback (0x005BEF58) at
  node+8 and the semaphore at node+0xc, then calls 0x005B8C60 →
  0x005B8B68, which stores the scheduled time at node+0x20, the library
  callback (0x005C8ED8) at node+0x28 and the node at node+0x30.
- The library's active list head is at 0x006592F0+0x18. At the stop it held
  only two audio stream nodes (callback 0x005B8ED8, scheduled 0x48000 and
  0x24000) — no delay node.
- The semaphore-signaling functions in the library region are at
  0x005BEC94, 0x005BED2C, 0x005BEDE4, 0x005BEE14, 0x005BF168, 0x005BF1AC and
  0x005BF25C; which of them the delay path reaches, and how the delay node is
  linked and fired, is the next experiment.

This is the recorded next wall: identify the library path that links and
fires the delay node (or find the actual waker of the delay semaphores), then
let the model's timer/event sources satisfy it.

## Limits recorded

- No transfer engine, FIFO behavior or chain walking; channels complete
  immediately, so timing-dependent code sees an ideal bus.
- Timers tick only at idleness, one compare per frame, with a frame-sized
  count jump.
- The model IOP answers only the version query; every other RPC function
  returns an empty result.
- The 6,000-interrupt idle budget is a guard, not a modeled frequency.
