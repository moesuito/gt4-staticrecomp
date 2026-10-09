# M30, tenth slice — semaphore handle bits and the delay library

> **Historical interpretation corrected in slice 96 (2026-10-09):** the
> `ori ... 0x2` discussed below updates **timer+0xC flags**, not
> descriptor+0xC's callback common/semaphore. The bit-0 test also concerns
> timer state. Therefore that instruction sequence does not prove a
> `semaphore | 2` requirement. Preserve the original report below as history,
> not current evidence for semaphore IDs. Allocation follows decision 0039;
> reviewed disassembly and live one-shot measurements are in
> `slice96-update-loop-and-delay-balance.md`.

Date: 2026-10-02. Inputs: the pinned CORE. Follow-up to the ninth slice
(`m30-slice9-timer-and-dma-completions.md`), whose frontier was every thread
waiting on semaphores created by the game's delay helper. This slice traces
that library to its signal path, finds that the game's own code manipulates
the low bits of the kernel's semaphore handles, and changes the model's
handle shape accordingly. The differential still passes: 3,000 services,
interpreter reference at 7,508,945 instructions, full state identical; a
longer run now reaches 9,765 services (672,586 interpreted steps).

## The delay path, traced end to end

- **The helper** (0x005AED18): `CreateSema` (id in `s0`), `x = 0x005B8D88(0,
  delay)`, `0x005B8F38(x, 0x005AEF58, sema)`, `WaitSema(sema)`,
  `DeleteSema(sema)`. The second argument is the callback; the third is the
  "common" (the semaphore).
- **The scheduler** (0x005B8F38) pops a descriptor node from the free list at
  0x0088C340, stores the callback at node+8 and the common at node+0xc,
  obtains a timer node id from 0x005B8540/0x005B84D0 (popping the timer node
  from 0x006592F0+0x14), then calls 0x005B8C60 → 0x005B8B68.
- **The arm function** (0x005B8B68) derives the timer node from the id
  (`node = (id >> 10) << 6`), stores the scheduled time at +0x20, the
  dispatcher address (0x005B8ED8) at +0x28, `gp` at +0x2c and the descriptor
  node at +0x30; it sets bit 1 of the descriptor's +0xc and, when bit 0 of
  that same value is set, inserts the timer node into the active list through
  0x005B8098 (a sorted insert on `scheduled + base - accumulated`).
- **The timer handler** (0x005B8158) walks the active list and calls
  `[node+0x28](a0 = node+8 | ..., a1 = node+0x20, a3 = node+0x30)` — the
  dispatcher with the descriptor node as its fourth argument.
- **The dispatcher** (0x005B8ED8) calls `[descriptor+8]` (the caller's
  callback) with `a3 = [descriptor+0xc]` (the caller's common). For the delay
  that is **0x005AEF58**, which runs `iSignalSema(a3)` — the wakeup the
  waiting thread needs.

The descriptor's +0xc is the caller's common value **with bit 1 forced**
(`ori v0, v1, 0x2`), and the insertion tests **bit 0** of the same value.
With the model's former sequential semaphore ids (1, 2, 3, ...) half of the
nodes never activated and every signal targeted `sema | 2`. The game's own
code therefore requires handles whose bits 0 and 1 are already set.

## What changed

- **Semaphore ids are 3, 7, 11, ...** (decision 0012): the model's allocation
  hands out ids with bits 0 and 1 set, so the library's `| 2` is a no-op and
  its bit-0 activation test always passes. Kernel unit tests that pinned the
  first id were updated.
- The delay-signal trace (temporary, removed) confirmed the callback is not
  reached yet even after the change.

## Observed progress

```
build/gt4boot.exe private/fingerprint-check/CORE.GT4 --services 20000 --threads
boundary: no-runnable-thread 0x00001604
stats: module calls 10160, interpreted steps 672586, services handled 9765
thread 1: wait 2/143, thread 2: wait 2/11, thread 3: wait 2/147
timer 2: count 0x03B5A880, comp 0x0046C0F3
```

The game now creates and uses many more semaphores (up to id 147) and runs
about 2.7 times further than before the change. The waits at the stop are
still semaphore waits.

## The remaining frontier

At the stop, the timer library's active list (0x006592F0+0x18) holds two
nodes (0x00889F40 and 0x00889F80) with descriptor handler fields 5 and 7,
common 3, dispatcher 0x005B8ED8 and scheduled times 0x48000 and 0x24000;
the free list at 0x0088C340 has nodes available. The delay descriptors are
not in the active list, so either their timer nodes were never inserted, or
they were inserted and removed without the dispatcher reaching the callback.
The next experiment is to watch the active-list head (0x006592F0+0x18) at
the moment the delay helper schedules and at each TIM2 handler pass — the
model can log guest writes to that word, or the run can be traced with a
breakpoint-like hook. The two nodes' handler fields (5 and 7) also suggest
that their descriptors' +8 fields are not function pointers, so the timer
handler's dispatch condition for them may differ from the delay's.

## Verification

- CTest **32/32** (`ee_kernel`'s semaphore tests now pin the id shape);
  Python 73 (67 run, 6 skip).
- `gt4boot --compare-interpreter` at 3,000 services: interpreter reference
  at 7,508,945 instructions, full state identical (registers, HI/LO, FPU,
  VU0, CP0, pc, memory digest).
