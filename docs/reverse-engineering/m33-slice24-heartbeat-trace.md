# M33, slice 24 — trace the heartbeat: the dispatcher's worker thread is never created

Date: 2026-10-03. Inputs: the pinned CORE and ISO, the whole-program
translation (`build/translated-whole-program.hpp`, 3,529,934 lines),
ten `gt4boot` legs from `build/ckpt-1980k.bin` (2,000 services each,
`--threads` plus temporary register/stack instruments, all since
removed), and stop-time `--dump` reads. No model change, no product
code change: this slice ships docs only.

Charter: find what invokes the frame dispatcher region (around
0x00587b30 — it wakes workers by table id, joins on completion semas,
signals onward) per frame on a running engine, and which link is cut.
Candidates: VBlank tick, main loop, SIF completion.

## Answer first

Nobody invokes the dispatcher, because its worker thread does not
exist yet. The chain that creates it was mapped end to end and its
first step never runs:

```
0x0019a698 (lazy-init wrapper; 4 sibling callers)
  -> 0x001c9468 (late setup; sole caller of the next)
    -> 0x00101c50 (one-shot init, flag [0x00617d88]; sole caller of the next)
      -> 0x00583718 (subsystem setup; sole caller of the next, gated)
        -> 0x00586da0 (job-system initializer)
          -> CreateThread(entry 0x00587bb0, the worker sleep-loop)
```

Stop-time state (Confirmed): the one-shot flag `[0x00617d88]` is 0,
the job global `0x0087E180` is 64 zero bytes, and no thread has entry
`0x00587bb0`. So the cut link is **upstream of all three candidates**:
the boot parks before reaching this late lazy init. Of VBlank tick /
main loop / SIF completion, none is the proximate invoker — the
dispatcher System they would drive was never built. The dispatcher's
own per-frame invoker (who calls 0x005878f8 once the system exists)
stays Unknown; the next slice finds it from the loop thread's waker.

## The dispatcher region, as mapped (Confirmed — disassembly)

- `0x005878f8` (the dispatcher): takes a request struct (a0) and a
  table id base (a1); gates on `[0x0087E180] >= 0`; validates the
  request's kind byte and bitmask fields (error codes `0x81050001`,
  `0x07`, `0x16`, `0x86`, `0x10`); then either runs the local 10-way
  switch `0x00587c08` (jump table at `0x006CE970`) or takes the wake
  path: memcpy the payload to the table, `WakeupThread` by table id
  (`0x00587b4c`, `0x00587b68`), join on completion semas with
  `WaitSema` (`0x00587b5c`, `0x00587b70`), `SignalSema` onward
  (`0x00587b80`, `0x00587b94`, `0x00587bf4`), return. It never sleeps
  itself.
- `0x00587bb0` (the worker sleep-loop): `SleepThread`, then
  `dispatch = 0x00587c08([0x0087E180+0x10])`, `SignalSema` on
  `[s0+8]`/`[s0+c]`, repeat. This is the sleeper the charter's "then
  sleeps" refers to.
- `0x00587c08` (the switch): dispatches on the request's first byte
  (10 cases) through the table at `0x006CE970`; case 0 calls
  `0x00589678`, others `0x00587f88`/`0x00588878`/`0x0058b210`.
- Services used, read off the SDK wrappers: `0x32` SleepThread,
  `0x33` WakeupThread, `0x42` SignalSema, `0x44` WaitSema,
  `0x20` CreateThread, `0x40` CreateSema, `0x41` DeleteSema.

## Zero direct callers (Confirmed — whole-translation grep)

- The only `jal` into `0x00587xxx` in all 3.5M translated lines is the
  dispatcher's own internal `0x00587a2c -> 0x00587c08`.
- The address `0x005878f8` never materializes as data; `0x00587bb0`
  materializes exactly once: the initializer builds it as a
  `CreateThread` entry (`lui v0,0x58; addiu v0,v0,0x7bb0` at
  `0x00586e50-0x00586e5c`, `jal 0x005adaa0` = service `0x20`).
- The six `WakeupThread` (`0x005adbd0`) sites sit in `function_0054e340`,
  `function_00554910`, `function_005549a0`, `function_00576640`
  (the condvar wake-next) and the dispatcher itself — none is a VBlank
  handler or in a VBlank handler's direct body.

## No thread is parked in the dispatcher machinery (Confirmed — two legs)

Temporary `--threads` instruments (since removed) printed each parked
thread's ra (the exact sleep/wait call site) and the 16 stack words at
sp over two 2,000-service legs from `ckpt-1980k.bin`:

- 8 engine workers (threads 5, 7, 10, 12, 13, 14, 16, 17):
  ra `0x0057689c` (the shared condvar sleep helper `0x005767e0`);
  saved ra one frame up `0x00574e60` (return into the spine
  `0x00574e30`); saved ra two frames up `0x00574ecc` (return into the
  wrapper `0x00574eb0`); third frames differ per thread (`0x0054a140`,
  `0x005475a8`, `0x00553c68` — three distinct worker mains sharing one
  sleep spine). Their live s1 values equal their thread ids.
- Main thread 1: ra `0x0057689c`, third frame `0x00109818` — the SDK
  flag-callback chain (`0x001097f0` family). Still slice 22's
  candidate 1, unfed.
- Thread 9: its own worker `0x00553b80` (stack shows the trampoline
  return `0x0057871c` two frames up; block `0x00871070`, arg
  `0x008fc690`).
- Threads 4, 6, 8, 11, 18: ra `0x005aedc0` — the delay library's
  `WaitSema` site. Thread 2: ra `0x005ae9e8` — its job-ring loop,
  producer still silent. Thread 3: ra `0x004a1098` — the SDK-flavored
  scratchpad sleep (unchanged).
- Zero words in `0x00587000..0x00588000` on any of the 17 stacks:
  no thread sleeps inside, or was ever called from, the dispatcher
  region on its current chain.

Side find (Confirmed): `0x005786f0` is the generic worker-thread
trampoline — it calls `[arg+0x38]([arg+0x3c])` via `jalr`
(ra `0x0057871c`), then cleanup helpers and thread exit. All engine
workers share it as their entry.

## The creation chain (Confirmed — disassembly + two stop-time dumps)

- `0x00586da0` writes `[0x0087E180+4]`, zeroes `+0x10`, creates two
  semaphores into `+0x8`/`+0xc` (the ids land via delay-slot stores
  `0x00586e00`/`0x00586e30`, which execute whether the `bgez` takes or
  not), then `CreateThread(entry 0x00587bb0, ...)`. Its sole caller
  is `0x00583718:0x00583800`, itself gated on `0x00585268 >= 0` (which
  fails only if `0x005843a0` returns 0).
- Sole caller of `0x00583718`: the one-shot `0x00101c50:0x00101cac`
  (flag `[0x00617d88]`, retry loop on negative returns).
- Sole caller of `0x00101c50`: `0x001c9468:0x001c9544` (late setup:
  float constants, table refs near `0x00693f00`, calls `0x001cb1d0`
  then `0x00594470`).
- Sole caller of `0x001c9468`: the lazy-init wrapper
  `0x0019a698:0x0019a6d8` (mallocs `0x7c` via `0x005c1498` on first
  use; four sibling wrappers at `0x0019a7f8/8c8/940/9a8` call it).
- Stop-time dumps: `[0x00617d88] = 0` and `0x0087E180` all zeros, so
  the one-shot never ran and the initializer's first store never
  happened. The boot parks before reaching this lazy init.

## Grades and limits

- Confirmed: everything above names its probe (translated-code grep
  lines, `gt4disasm` addresses, dump bytes from deterministic legs).
- High confidence: the cut is upstream starvation (the boot never
  reaches the lazy init), not a broken link inside the dispatcher
  system — the system was never built.
- Unknown: who calls `0x005878f8` per frame once the system exists
  (no direct-call evidence; presumably indirect — thread entry,
  table, or `jalr` — like the `0x005786f0` trampoline convention), and
  which of the four `0x0019a7xx` wrappers the game's next phase calls
  first.
- Method note: `thread.context.gpr[]` reads the parked registers
  (ra = exact sleep call site); the frameless syscall wrappers mean
  sp points at the caller's frame, so `[sp+0x30]` is the sleep
  helper's saved ra. Two instruments crashed on unaligned guest reads
  before guards were added (`contains` checks mapping, not
  alignment) — fixed, then all removed.

## Verification

- Product code untouched (`git status` clean; `TEMPORARY` grep-clean
  in source and in the relinked `build/gt4boot.exe`).
- All nine scratch logs deleted after extracting the evidence above.
- Gates left for review (no commit per slice rules).

Next: from the four `0x0019a7xx` lazy wrappers, find which subsystem
the boot reaches for first (what unparks main / feeds the flag queue),
or — once the loop thread exists — trace who writes
`[0x0087E180+0x10]` and wakes it: that poster is the true per-frame
heartbeat source the three charter candidates were circling.
