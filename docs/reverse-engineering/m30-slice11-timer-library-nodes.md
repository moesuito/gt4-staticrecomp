# M30, eleventh slice — the timer library's nodes and the due condition

Date: 2026-10-02. Inputs: the pinned CORE. Follow-up to the tenth slice
(`m30-slice10-semaphore-handles-and-the-delay-library.md`), whose frontier
was the delay callback never firing even though the handle shape was fixed.
This slice traces the library's node lifecycle with model instrumentation,
finds that the delay nodes **are** scheduled and active, and isolates the
remaining wall: the TIM2 handler's due condition never passes for any active
node. No model behavior changed except the idle budget; the run reaches the
30,000-service limit in about four seconds with the differential still
passing at 3,000 services.

## The library's two node types

- **Descriptor nodes** (0x10 bytes, pool free list at 0x0088C340): +4 id,
  +8 callback, +0xc common (the semaphore for the delay).
- **Timer nodes** (0x40 bytes, pool free list at 0x006592F0+0x14): +8 id
  bits, +0xc flags (**bit 0 = active, bit 1 = armed**), +0x10 base time,
  +0x18 accumulated time, +0x20 scheduled time, +0x28 dispatcher address
  (0x005B8ED8), +0x2c gp, +0x30 descriptor pointer.

The delay's schedule path: 0x005B8F38 pops a descriptor from 0x0088C340 and
a timer node id from 0x005B8540/0x005B84D0, arms the timer node through
0x005B8C60 → 0x005B8B68 (scheduled time at +0x20, dispatcher at +0x28,
descriptor at +0x30, bit 1 set), then calls 0x005B87D8 → **0x005B8728**,
which stores the current library time at +0x10, sets bit 0 and inserts the
node into the active list through 0x005B8098 (a sorted insert on
`scheduled + base - accumulated`). Note that 0x005B8F38 ignores both calls'
return values, so a failed start still leaves the delay helper waiting.

## The delay nodes are active

Instrumenting the delay helper's `WaitSema` block (temporary, removed)
showed the library state at the exact wait moments:

```
[delay-block] id=143 ctl: c=2 10=1 14=0x00889f80 18=0x00889f40 1c=-1
  active 0x00889f40: idbits=5 flags=3 base=0x23730000 acc=0 sched=0x48000 desc=0x0088bf40
[delay-block] id=147 ctl: c=3 10=2 14=0x00889fc0 18=0x00889f40 1c=-1
  active 0x00889f40: idbits=5 flags=3 base=0x23730000 acc=0 sched=0x48000 desc=0x0088bf40
  active 0x00889f80: idbits=7 flags=3 base=0x46c08000 acc=0 sched=0x24000 desc=0x0088bf50
```

Both delay nodes are in the active list with flags 3 (active + armed) and
descriptors 0x0088BF40/0x0088BF50. The scheduling and insertion work.

## The remaining wall: the due condition

The TIM2 handler (0x005B8158) walks the active list; for each node it
computes a target from `scheduled + base - accumulated` and a "current" value
from TIM2's count (and the overflow counter at 0x006592F0), then calls the
dispatcher only when the current value reaches the target. At every stop the
active nodes still have `accumulated = 0` and `flags = 3`: the handler never
treated them as due, and the same is true for the two audio nodes that share
the list. A longer virtual time does not change it: with the idle budget
raised to 200,000 the run reaches the 30,000-service limit (1,947,454
interpreted steps, about four seconds) with the same waits.

Two hypotheses for the next experiment:

1. The handler's time base uses the **overflow counter** and the exact
   `<< (CLKS * 4)` scaling; the model's TIM2 never overflows (a 32-bit wrap
   at 9,600 counts per idle frame needs ~450,000 frames). The library's base
   value 0x23730000 (a fixed epoch) and the scheduled values suggest a time
   scale the model has not matched yet.
2. The handler may stop iterating at the first not-due node (the loop jumps
   to the epilogue when `current < target`), so a wrong first node hides all
   others.

The next slice should instrument the handler's intermediate values for one
active node (its target and current) — a breakpoint-like hook at 0x005B822C
would show both in one run.

## Model change

- The idle budget rose from 6,000 to **200,000** consecutive idle interrupts
  without a runnable thread (about an hour of virtual frames). Idle
  interrupts are cheap; the budget only bounds a truly stuck machine, and
  the smaller value stopped runs that were still inside the game's wait
  loops.

## Verification

- CTest **32/32**; the differential at 3,000 services: interpreter reference
  at 7,508,945 instructions, full state identical.
- The long run: `--services 30000` reaches the service limit in 3.8 seconds
  with 1,947,454 interpreted steps.
