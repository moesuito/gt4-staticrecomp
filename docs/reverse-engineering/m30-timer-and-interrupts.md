# M30, fifth slice — timer registers, interrupt handlers, and a translator bug

Date: 2026-10-02. Inputs: the pinned CORE. Follow-up to the fourth slice
(`m30-kernel-patches.md`): the boot stopped reading TIM3_MODE (0x10001810),
and the model mapped no hardware. This slice models the timer registers and
the interrupt-handler services, corrects the CP0 starting state, and fixes a
translator bug the wider run exposed — the game's thread creation now runs
and the cooperative scheduler is exercised end to end.

## What changed

- **The device window** (`GuestMemory::map_mmio`) routes a declared address
  range to callbacks that see the access width; everything else stays
  strictly bounded RAM. The former KSEG0 alias became a **segment alias**
  covering KSEG0 and KSEG1 (`0x80000000`/`0xA0000000` → `address &
  0x1FFFFFFF`), enabled only by `gt4boot`, because the timer code writes
  registers through the uncached alias (0xB0001000).
- **`ee::TimerUnit`** stores the four timers' 32-bit registers (COUNT/MODE/
  COMP/HOLD at each timer's base). Untouched registers read zero — the "not
  started" value InitAlarm checks — and written values read back. No
  ticking, no compare/overflow flags, no interrupts (decision 0007).
- **Interrupt and DMA handler services** join the kernel: AddIntcHandler/
  AddIntcHandler2 (0x10), RemoveIntcHandler (0x11), AddDmacHandler (0x12),
  RemoveDmacHandler (0x13), EnableIntc/DisableIntc (0x14/0x15), EnableDmac/
  DisableDmac (0x16/0x17) and the negative `i*` aliases. Registrations are
  stored with sequential ids; enabling is accepted and nothing fires.
- **The CP0 Status baseline is the M14 live capture (0x70030c11)**, not the
  earlier 0x40000000 placeholder. The SDK's thread setup checks `Status.IE`
  and `EIE` before the crt0 reaches its `ei`, and the live running state is
  the evidence-backed starting point; the interpreter's COP0 fixture now
  hand-computes against it.
- **The translator now emits `return;` after every `jr ra`** (see below).

## The translator bug, found by widening the run

While tracing why the timer initialization left EIE clear, the differential
harness showed the module and the interpreter diverging at register 12: the
interpreter executed EIntr's `ei` (pc 0x005B7304) and restored EIE; the
module never did. Instrumenting the driver, the module dispatch and the
generated DIntr (0x005B72A8) showed the function executing **both** of its
return paths in one call:

- Root cause: for `FlowKind::Return` the emitter wrote
  `state.set_pc(read_gpr64(31));` but no `return;`, so generated code fell
  through into the next block — here, the taken path at 0x005B72EC, which set
  a0 = 0 and returned a second time. DIntr therefore returned a previous-EIE
  value of 0 instead of 1, the caller skipped EIntr, and the SDK's next check
  failed with EIE clear.
- Fix: the emitter appends `return;` after the `jr ra` statement.
- Regression: `ee_translation_5b72a8` runs the fixed function on two input
  states (interrupts enabled/disabled) and compares every register and CP0
  with the interpreter, plus the hand-computed contract (v0 reports the old
  EIE, EIE is cleared).
- Why 25 hand-picked differential modules missed it: none had a `jr ra`
  followed by more code in the same extent. This is the first payoff of
  running the whole program under the driver — the lesson recorded for the
  documentation style is "widen the verified surface, do not only deepen it".

## The verified run

```
build/gt4boot.exe private/fingerprint-check/CORE.GT4 --services 400 --compare-interpreter
...
boundary: syscall 0x005add54 service 0x4b
stats: module calls 42, interpreted steps 15699, services handled 45
interpreter: 961937 instructions, state identical (registers, HI/LO, FPU, VU0, CP0, pc, memory digest)
```

- The boot now runs the whole `_InitSys` tree: the alarm patch, the timer
  initialization (its T3/T2 register reads and writes, the di/eret
  register-write helper, FlushCache, AddIntcHandler2 + EnableIntc), and then
  **the game's thread creation**: CreateSema 0x40, CreateThread 0x20,
  ReferThreadStatus 0x30, StartThread 0x22, GetThreadId 0x2F,
  ChangeThreadPriority 0x29 and WaitSema 0x44. The cooperative scheduler of
  decision 0005 switches to the new priority-0 thread, which blocks on its
  semaphore, and switches back — its first end-to-end exercise on real game
  code, with the interpreter reference agreeing on the full state.
- The run stops at the next unmodeled service: **GetOsdConfigParam (0x4B)**
  at pc 0x005ADD54, called from 0x005B7620 (which also calls
  SetOsdConfigParam 0x4A) while the init reads the console's OSD settings.
- `gt4boot_services` now pins this frontier (`--services 400`, 45 handled).

## The next wall: the OSD configuration

- 0x005B7620 calls `_print`-style debug output? No: the boundary pc belongs
  to the 0x4B (GetOsdConfigParam) wrapper; the function builds a request on
  the stack, calls the service and inspects a bit field
  (`(value >> 13) & 7 < 1`).
- Modeling it needs the OSD config block's layout and plausible values
  (region/language/aspect/TV mode) — the pinned BIOS is `SCPH-90001` (USA)
  and the live savestate's RAM may carry the block. That is its own small
  decision; the call is an explicit boundary today.

## Limits recorded

- No timer ticks, no compare/overflow flags, no interrupt or alarm callback
  delivery; a guest that waits for one stops at the wait's boundary.
- Only 32-bit accesses to the timer window are modeled; other widths throw
  with the address.
- The device window is one range; overlapping devices are not supported yet.
- The regression test covers DIntr's shape; other multi-return shapes rely
  on the same emitter fix and the whole-program run.

## Evidence

- CTest **32/32**: the new `ee_translation_5b72a8` differential regression and
  `ee_timer` (MMIO routing, KSEG0/KSEG1 aliasing, width rejection) plus the
  existing suite; `gt4boot_services` runs the new frontier with the
  interpreter comparison.
- Python 73 collected (67 run, 6 skip) — unchanged.
