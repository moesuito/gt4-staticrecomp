# 0016 — The clock advances with handled services

Status: implemented 2026-10-02 for the M30 fifteenth slice
(`docs/reverse-engineering/m30-slice15-service-clock.md`).

Context: the boot now drives its device polling round continuously (decision
0015), and the main thread waited on a delay semaphore that only the game's
timer library can signal. The model advanced its clock only at idleness
(decision 0011: one frame per idle interrupt), so with a busy polling round
the delay never expired and the main thread starved: at the 60,000-service
limit it still waited on the same delay it had entered thousands of services
earlier.

Decision:

- **Every handled service advances the model's time base by one millisecond
  of BUSCLK ticks** (`Kernel::advance_service_time`, 147,456 ticks per
  service). The game's delay library schedules in exactly these units: its
  timer nodes' base values are BUSCLK ticks of elapsed time (slices 11 and
  14).
- **Both engines call the same method at their service boundaries**: the
  driver through the new `RunOptions::advance_time` hook, the interpreter
  reference directly after each handled service. The clock is therefore a
  function of the guest's service sequence, not of the engine, and the
  differential stays exact.
- **Timers follow their clock selector**: the slice divides by 1, 16, 256 or
  the horizontal-blank ratio (CLKS 0-3), with a per-timer fractional
  remainder so no tick is lost. The compare flag sets when the counter
  *crosses* the compare value, like the hardware, so a handler that
  reprograms COMP keeps its period; a compare value already behind the
  counter does not fire again.
- **One VBlank per frame of accumulated slices** (2,457,600 ticks), with the
  same registration rule as the idle source (only when a VBlank handler is
  registered).

Alternatives considered:

- **An instruction-count-based clock**: the translated module does not report
  how many instructions a module call executed, so the two engines would
  accumulate different time and the differential would fail. A guest-visible
  time base must attach to something both engines observe identically.
- **A cycle-accurate clock**: out of scope by design (no EE cycle accounting
  exists in the model).
- **Keeping only the idle advance**: the measured starvation above; the
  polling round never idles.

Consequences and limits:

- The clock is a modeling shortcut, not cycle accuracy: its quantum is one
  millisecond per service, so a computation that runs long without any
  service still freezes the clock, and a service that really takes
  microseconds advances a full millisecond. The idle path keeps its
  frame-per-interrupt jump (decision 0011); the two paths are documented
  shortcuts for the same physical clock.
- Measured on the boot: TIM2's counter advances 576.05 ticks per service at
  CLKS = BUSCLK/256 — one millisecond to within 0.01% — and the game's own
  timer library reprograms COMP while the run proceeds (its count/COMP
  values move together).
- The main thread's delays now expire: its wait target moved from the stuck
  delay to new delays (semaphore ids 667 -> 16807 at 60,000 services, a new
  delay again at 1,000,000), and the long run ends at a service boundary
  with most of the worker threads ready instead of all waiting.

Verification:

- CTest **32/32** (the clock's timer crossing, silence behind the counter and
  the frame's VBlank in `ee_kernel`); Python 73 (67 run, 6 skip).
- `gt4boot --compare-interpreter` at 3,000 services: interpreter reference
  at 7,570,583 instructions, full state identical. The reference count
  changed from the previous slice because the queued VBlank and timer
  causes now reach busy execution, and both engines followed the change
  identically.

## Slice 96 evidence amendment (2026-10-09; policy unchanged)

With corrected interrupt-model-4 preemption, the update loop's conditional
1000-us delay blocks with exactly 147456 BUSCLK ticks still remaining.
WaitSema's own service charge consumes that entire interval, so TIM2
interrupts the newly selected root before any guest instruction; the
callback wakes and immediately restores the higher-priority update loop.
Confirmed in 270/270 fresh-run waits. Excluding handler/private-return
charges produces the same result in 273/273 waits: the decisive charge
belongs to ordinary WaitSema. The old later-boot outcomes above predate
the preemption correction and cannot establish current correctness.

Evidence: `docs/reverse-engineering/slice96-update-loop-and-delay-balance.md`.
An independent no-card/software reference snapshot instead corroborates
the same routine's blocking VBlank path; syscall duration/phase alignment
are still unknown. The instruction-accounting objection above is an
engineering limitation of the current emitter, not evidence that service
counting models physical time. Investigate a shared work-accounted clock
and independent relative timing; no guessed quantum or replacement policy
is adopted in this evidence-only slice.

## Slice 97 reference measurement amendment (2026-10-09; policy unchanged)

Pinned stock PCSX2 v2.9.114 measured a root retry **2000-us** WaitSema:
1111 EE cycles from syscall arrival to blocking dispatch pre-ERET, about
3.77 us nominal emulator time. Armed timer remaining 294912 -> 294400
BUSCLK ticks; nearly all 2 ms remains. Selected context is idle, not root.
Actual update/frame SleepThread dispatch measured 1630 cycles, again idle.
Neither sample captures the matched update 1000-us wait/root execution.
No universal service cost, instruction cost or physical-console timing
is inferred. Evidence and a shared dynamic-work accounting proposal:
`docs/reverse-engineering/slice97-reference-delay-timing.md`.
Production continues using this decision until independently verified
replacement semantics and accounting/delivery tests justify changing it.
