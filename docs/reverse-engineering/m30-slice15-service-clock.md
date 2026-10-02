# M30, fifteenth slice — the clock advances with handled services

Date: 2026-10-02. Inputs: the pinned CORE. Follow-up to the fourteenth slice
(`m30-slice14-sif-register-mirror.md`), whose run drove the game's device
polling round but left the main thread waiting on a delay semaphore that
never expired. This slice gives the model a clock that advances while code
runs, driven by the service sequence so the differential stays exact. The
long run now ends at a service boundary with the worker threads ready; the
differential passes at 3,000 services with the interpreter reference at
7,570,583 instructions and the full state identical.

## The starvation

At the 60,000-service limit the thread table showed the main thread (entry
0x00000000) waiting on semaphore 667 — a delay-library semaphore — while the
device polling round ran on. The delay library signals its semaphore from
the timer library's dispatcher, which runs from the TIM2 handler when a
node comes due; the due condition compares the TIM2 counter against the
node's scheduled value. The model only advanced the timers at idleness
(decision 0011), and the polling round never idles: the delay could not
expire, and re-running with a larger limit changed nothing.

## The clock

`Kernel::advance_service_time` advances the model's time base by one
millisecond of BUSCLK ticks per handled service, and both engines call it at
their service boundaries (the driver through `RunOptions::advance_time`, the
interpreter reference directly). The slice:

- divides per timer by its CLKS selector (1, 16, 256, the horizontal-blank
  ratio) with a per-timer fractional remainder;
- sets a timer's compare flag only when its counter *crosses* COMP, matching
  the hardware so a handler that reprograms COMP keeps its period;
- counts the slices toward one VBlank per frame (2,457,600 ticks), with the
  idle source's registration rule.

The idle path keeps its frame-per-interrupt jump. Both are documented
shortcuts for the same physical clock (decision 0016).

## Measured evidence

A temporary dump inside the advance (removed before the commit) printed
TIM2's registers every 20,000 slices in a 60,000-service run:

```
[time] slices=20000 tim2: count=0x00c77880 comp=0x00000000 mode=0x00000782
[time] slices=40000 tim2: count=0x01774080 comp=0x01774080 mode=0x00000782
[time] slices=60000 tim2: count=0x02270880 comp=0x0227fb00 mode=0x00000782
```

The counter delta is 0x00AFD000 = 11,521,024 ticks per 20,000 slices =
576.05 ticks per service — one millisecond at CLKS = BUSCLK/256 (576 ticks
per millisecond) to within 0.01%. COMP moves with the counter, so the game's
own timer library was reprogramming the compare while the run proceeded.

The thread table confirms the delays now expire: the main thread's wait
target went from the stuck semaphore 667 (before, at 60,000 services) to
16807 (after, same limit) and to further delays later; the 1,000,000-service
run ends at a service boundary (pc 0x005AEB78, service 0xFFFFFFD1) with most
of the worker threads ready instead of all waiting.

```
--services 1000000 --threads        (after the clock)
boundary: syscall 0x005aeb78 service 0xffffffd1
stats: module calls 1193971, interpreted steps 32878366, services handled 1000000
thread 1: status 0x2 (ready), entry 0x00000000
thread 3..12: ready or running, entry 0x005786f0 (the worker pool)
timer 2: count 0x226cc2c0, mode 0x00000782
```

## The next frontier

The device polling round remains the game's activity; with the clock in
place, the next slice characterizes whether its calls' empty replies hold
anything back (the liblgdev RPCs 6/13/15 and the string-coded servers'
RPCs 1/3/4/8) and answers the first of them whose reply the game acts on,
using the live PCSX2 emulator as the oracle for the real replies.

## Verification

- CTest **32/32** (the clock's compare crossing, the silence behind the
  counter and the frame's VBlank in `ee_kernel`); Python 73 (67 run, 6
  skip).
- `gt4boot --compare-interpreter` at 3,000 services: interpreter reference
  at 7,570,583 instructions, full state identical (registers, HI/LO, FPU,
  VU0, CP0, pc, memory digest).
