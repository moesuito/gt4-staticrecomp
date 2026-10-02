# 0007 — The EE timer registers, the device window and interrupt handlers

Status: implemented 2026-10-02 for the M30 fifth slice
(`docs/reverse-engineering/m30-timer-and-interrupts.md`).

Context: the boot reached the SDK's InitAlarm/InitTimer, which read TIM3_MODE
(0x10001810) to test whether the alarm system was already initialized, wrote
TIM2 through its uncached KSEG1 alias (0xB0001000), copied the alarm code to
0x80076000, installed interrupt handlers and read the OSD configuration.
The model mapped no hardware registers and could not answer any of it.

Decision:

- **One explicit device window in `GuestMemory`** (`map_mmio(base, size,
  read, write)`, callbacks taking the access width). Accesses inside the
  window route to the callbacks; everything else stays strictly bounded RAM.
  The former KSEG0 alias becomes a **segment alias** for KSEG0 and KSEG1
  (0x80000000/0xA0000000 → `address & 0x1FFFFFFF`), enabled only by the boot
  tool, because the timer code writes registers through the uncached alias.
- **`ee::TimerUnit`** owns the four timers' 32-bit registers (bases
  0x10000000/0x10000800/0x10001000/0x10001800; COUNT +0x00, MODE +0x10,
  COMP +0x20, HOLD +0x30). Reads return what was written and untouched
  registers read as zero — exactly the "not started" value InitAlarm checks.
  **The counters do not tick, the MODE overflow/compare bits are plain
  storage, and no timer interrupt is ever raised.** Only 32-bit accesses are
  modeled; any other width stops with the address.
- **Interrupt and DMA handler registrations** (`AddIntcHandler`/`...2` 0x10,
  `RemoveIntcHandler` 0x11, `AddDmacHandler` 0x12, `RemoveDmacHandler` 0x13)
  are stored in the kernel with sequential ids; `EnableIntc`/`DisableIntc`
  (0x14/0x15) and their DMA twins accept the call with no effect because
  nothing can fire. The negative `i*` aliases register the same handlers.
- **The CP0 Status baseline is the M14 live capture, 0x70030c11** (IE and
  EIE set, interrupt mask, CU2). The earlier 0x40000000 placeholder made the
  SDK's thread setup fail its own `Status.IE`/`EIE` checks before the crt0
  reaches its `ei`; the live running state is the correct starting point and
  the interpreter test now hand-computes against it.
- **Translator fix found on this path**: a `jr ra` followed by more code in
  the same extent emitted `set_pc(ra)` without `return;`, so generated code
  fell through into the next block and executed a second return (DIntr
  0x005B72A8 returned 0 instead of the previous EIE state). The emitter now
  emits `return;` after every `jr ra`; a dedicated differential regression
  covers it (`ee_translation_5b72a8`, two input states).

Alternatives considered:

- **Map the device page as plain RAM** (rejected: silent side effects; the
  timer/INTC code would read back values no hardware would produce and the
  model would not know it).
- **A full ticking timer with interrupt delivery** (deferred: the verified
  path only configures the timers; a clock source and delivery order need
  live evidence, and guessing them would be worse than an explicit limit).
- **Model GetOsdConfigParam as part of this slice** (rejected: the OSD
  block's field layout and values need their own evidence; it is the next
  wall and a boundary today).

Consequences and limits:

- Any guest code that *waits* for a timer interrupt or an alarm callback will
  not observe one: it stops at the boundary where the wait becomes a
  blocking service, with the module/bridge state intact. This is recorded,
  not hidden.
- Handler registrations are bookkeeping only; the handler code is never
  invoked. Removal validates the registration id and cause.
- The device window is singular by design; the next device (INTC registers,
  DMAC, SIF) extends `TimerUnit`-like units and must not overlap it.
- The whole-program-under-the-driver run found the translator bug that 25
  hand-picked differential modules had missed; the lesson is recorded in the
  evidence document (widen the verified surface, do not only deepen it).
