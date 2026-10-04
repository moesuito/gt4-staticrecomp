# M30 lesson — timers, interrupts, and the OSD block: hardware as storage, then as init

Prepared 2026-10-04. BUILD/VERIFY: passed for slices 5 and 6; see the
[M30 slice-5 evidence](../reverse-engineering/m30-timer-and-interrupts.md),
[slice-6 evidence](../reverse-engineering/m30-osd-and-the-iop-wall.md),
and [decision 0007](../decisions/0007-timer-registers.md) /
[decision 0008](../decisions/0008-osd-and-device-banks.md). EXPLAIN:
this is the worked explanation; tutoring review pending.

## Objective and motivation

The patch arc ends at a new kind of address: `0x005B7A40` reads
`TIM3_MODE` (`0x10001810`), and the model maps no hardware at all.
The game is now doing what real console software does at startup —
initializing timers and alarms, installing interrupt handlers, and
reading the console's OSD settings — and every one of those touches
a register with side effects the model cannot see. This arc teaches
the project's hardware policy in its cheapest form first: model a
register as *storage* (writes read back, untouched reads zero),
register the handlers as *bookkeeping*, and stop exactly where a
value would have to change on its own. It ends with the game's own
thread creation switching back and forth through the cooperative
scheduler — the first end-to-end exercise of the previous lesson's
machine — and stops at the IOP wall, which is a subsystem, not a
bug.

The motivating failure is a read past the end of the modeled
world: `Guest fault at pc 0x005b7a40: Guest access at 0x10001810
(width 4) is outside the mapped region`. A fault that names its
pc and address is a work order, not a crash.

## Step 1 — one device window, then a list of them

`GuestMemory::map_mmio(base, size, read, write)` routes a declared
range to callbacks that see the access width; everything else
stays strictly bounded RAM. Mapping the timer page as plain RAM
was considered and rejected: silent side effects — the
timer/alarm code would read back values no hardware would produce,
and the model would never know. The KSEG0-only alias from the
patch arc becomes a segment alias for KSEG0 *and* KSEG1
(`0x80000000`/`0xA0000000` → `address & 0x1FFFFFFF`), enabled only
by the boot tool, for one evidenced reason: the timer code writes
registers through the uncached alias (`0xB0001000`).

`ee::TimerUnit` owns the four timers' 32-bit registers (bases
`0x10000000`/`0x10000800`/`0x10001000`/`0x10001800`; COUNT +0x00,
MODE +0x10, COMP +0x20, HOLD +0x30). Reads return what was
written; untouched registers read zero — exactly the "not
started" value `InitAlarm` checks. The counters do not tick, MODE
overflow/compare bits are plain storage, and no timer interrupt
is ever raised. Only 32-bit accesses are modeled; any other width
stops with the address. The sixth slice generalizes the window
into a list: the timer, the DMAC block (`0x1000E000`, 0x100
bytes) and the SIF0 channel control (`0x1000C000`, 0x100 bytes)
each get one, with `TimerUnit` refactored onto the shared
`RegisterBank` class (32-bit storage, untouched reads zero).

## Step 2 — handlers as bookkeeping, and the baseline the SDK checks

`AddIntcHandler`/`...2` (0x10), `RemoveIntcHandler` (0x11),
`AddDmacHandler` (0x12), `RemoveDmacHandler` (0x13),
`EnableIntc`/`DisableIntc` (0x14/0x15) and the DMA twins, plus
the negative `i*` aliases, are stored with sequential ids while
enabling is accepted with no effect — because nothing can fire
yet. Registrations without delivery look useless; they are the
wiring the delivery slices will plug into, and recording them
now is what lets the boot proceed past setup.

The CP0 Status baseline becomes the M14 live capture
`0x70030c11` (IE and EIE set, interrupt mask, CU2), replacing the
`0x40000000` placeholder — because the SDK's own thread setup
checks `Status.IE` and `EIE` before the crt0 reaches its `ei`,
and a placeholder that fails the game's own checks is not
neutral. The interpreter's COP0 fixture now hand-computes
against the capture. The rule generalizes: live-observed state
beats invented constants, and "the game checks it" is the test
for which constants matter.

## Step 3 — the translator bug the wider run caught

Tracing why timer initialization left EIE clear, the
differential showed module and interpreter diverging at register
12: the interpreter executed EIntr's `ei` (pc `0x005B7304`) and
restored EIE; the module never did. The generated DIntr
(`0x005B72A8`) executed *both* return paths in one call: for
`FlowKind::Return` the emitter wrote `state.set_pc(read_gpr64(31))`
but no `return;`, so code fell through into the next block —
here the taken path at `0x005B72EC`, which set a0 = 0 and
returned a second time. DIntr reported previous-EIE 0 instead of
1, the caller skipped EIntr, and the next check failed with EIE
clear. The fix is one line with a large blast radius: the
emitter appends `return;` after every `jr ra`, pinned by the
differential regression `ee_translation_5b72a8` on two input
states (enabled/disabled) comparing every register and CP0 plus
the hand-computed contract.

Why 25 hand-picked differential modules missed it: none had a
`jr ra` followed by more code in the same extent. The lesson the
slice wrote down generalizes beyond this bug: widen the verified
surface, do not only deepen it — the whole program running under
the driver is a fuzzer no fixture list can match.

## Step 4 — the OSD word, the thread creation, and the IOP wall

The OSD configuration joins the kernel as storage with one
tested property: `GetOsdConfigParam` (0x4B) writes the word,
`SetOsdConfigParam` (0x4A) stores what the guest wrote retaining
*every* field — because the SDK probe at `0x005B7620` writes
`version = 1` and reads it back precisely to tell a late kernel
(retained) from an early Japanese one (always 0). The initial
word `0x00012011` (SPDIF disabled, 4:3, RGB, non-Japanese, OSD2,
English, GMT) is a documented model value for the pinned USA
BIOS; only the retention bit is evidence-backed, and a later
slice that reads language or aspect must pin the fields against
live evidence. The attempt to mine the M14 dump for the block
(282 plausible words, no way to identify it) was rejected as
evidence — another instance of labeling the model value instead
of laundering a guess.

Then the payoff run: the whole `_InitSys` tree, the alarm patch,
the timer init (T3/T2 reads and writes, the di/eret helper,
FlushCache, AddIntcHandler2 + EnableIntc), and the game's thread
creation — CreateSema, CreateThread, ReferThreadStatus,
StartThread, GetThreadId, ChangeThreadPriority, WaitSema — with
the scheduler switching to the new priority-0 thread, which
blocks on its semaphore, and switching back. Interpreter agrees
at 961,937 instructions down to the memory digest. The run then
reads DMAC `D_STAT` and SIF0 `CHCR` as zero ("no IOP activity,
channel stopped") and stops at `SifSetDChain` (0x78) at
`0x005AE084` — inside the game's `sceSifInitCmd`, whose next
steps (register the DMA handler, then spin on the IOP's CMDINIT
flag) need the IOP interface: registers with real semantics, DMA
submission and completion, the RPC layer. A subsystem, with
public sources (`sifcmd.c`) and the live emulator as oracle —
which is exactly what the next two lessons' arc builds.

## What later evidence reframed (not smoothed over)

- "No ticking, no delivery" was a staged limit, not a verdict:
  later slices gave timers frames, compare interrupts, DMA
  completions, and finally the service clock — each its own
  evidenced step. The storage-first policy is what made each
  step small.
- The OSD defaults grew too (ConfigParam2 `0x6E`/`0x6F`, version
  answers) — same pattern: retention first, fields when used.
- The widen-don't-deepen lesson recurs at whole-program scale
  (slice 26's survey, the M27–M29 boundary work): each new shape
  the translator meets in the wild gets the same treatment as
  DIntr's fall-through — represent, verify, pin a regression.

## Connection to our implementation

| Piece | File | Job |
| --- | --- | --- |
| Device windows + alias | `src/ee/state.cpp` (`map_mmio`, segment alias, boot opt-in) | routed ranges, strict RAM elsewhere |
| Timer storage | timer unit on `RegisterBank` | COUNT/MODE/COMP/HOLD per timer |
| Handler bookkeeping | `src/ee/kernel.cpp` (Add/Remove/Enable Intc+Dmac + aliases) | stored registrations, accepted enables |
| CP0 baseline | interpreter COP0 fixture + `ee_state` init | live `0x70030c11`, hand-computed checks |
| `return;` after `jr ra` | translator emitter + `ee_translation_5b72a8_test` | no fall-through, two-state regression |
| OSD word + retention | `src/ee/kernel.cpp` (`set/get_osd_config`, USA default) | probe passes, fields labeled model |
| DMAC/SIF banks | `RegisterBank` instances | zero reads, 32-bit-only |

## Understanding checkpoint

1. Mapping the timer page as plain RAM was rejected. Construct
   the failure concretely: which SDK check would pass wrongly,
   and why would the model never notice?
2. The KSEG0-only alias became KSEG0+KSEG1 for one evidenced
   reason. What was it, and why does the alias stay opt-in?
3. DIntr executed both return paths in one call. Explain the
   fall-through at the emitter level, and why 25 differential
   modules all missed it.
4. The SDK probe writes `version = 1` and reads it back. What
   two kernels does that distinguish, and which answer does the
   model give for the pinned BIOS?
5. `SifSetDChain` is a clean boundary here but was "the wall"
   one slice earlier and a modeled write two slices later. What
   changed at each step, and what stayed a documented limit
   throughout?
6. Handler registrations do nothing observable in this arc. Why
   record them anyway, and which later slice first makes one
   fire?
