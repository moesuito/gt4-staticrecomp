# M30 lesson — the delay library: traced callbacks, handle bits, and handlers that run

Prepared 2026-10-04. BUILD/VERIFY: passed for slices 10–12; see the
[M30 slice-10 evidence](../reverse-engineering/m30-slice10-semaphore-handles-and-the-delay-library.md),
[slice-11 evidence](../reverse-engineering/m30-slice11-timer-library-nodes.md),
[slice-12 evidence](../reverse-engineering/m30-slice12-handler-execution.md),
and [decision 0012](../decisions/0012-semaphore-handle-bits.md) /
[decision 0013](../decisions/0013-handler-execution.md). EXPLAIN: this
is the worked explanation; tutoring review pending.

## Objective and motivation

Slices 1–9 built a machine that boots the game into its runtime and
then parks: every thread waits on a semaphore, and the semaphores
belong to the game's delay helper — "sleep N, then continue". The
model answered the helper's syscalls faithfully, yet the waits never
ended. This arc teaches the project's central debugging motion:
trace the guest's own wait machinery end to end, find where the
model's half breaks the guest's assumptions, and fix the model —
never the game. It ends with the boot running continuously for a
million services.

One failure motivates the first half: the model's semaphore ids
(1, 2, 3, …) *look* fine and compare correctly, but the game's
library does arithmetic on the handle bits. A second failure
motivates the rest: interrupt handlers that run one instruction at
a time and frames abandoned mid-handler. Both bugs looked, from the
outside, like a parked game.

## Step 1 — trace the delay path end to end

The delay helper at `0x005AED18` is five calls:

```text
CreateSema -> id in s0
x = 0x005B8D88(0, delay)
0x005B8F38(x, 0x005AEF58, sema)   ; callback, common
WaitSema(sema)
DeleteSema(sema)
```

Follow the arguments, not the names. `0x005B8F38` pops a
descriptor node from the free list at `0x0088C340`, stores the
callback at node+8 and the common at node+0xc, obtains a timer
node id, and arms it through `0x005B8C60 → 0x005B8B68`, which
stores the scheduled time at +0x20, the dispatcher address
`0x005B8ED8` at +0x28, `gp` at +0x2c and the descriptor at +0x30.
The TIM2 handler (`0x005B8158`) walks the active list and calls
`[node+0x28]` — the dispatcher — which calls `[descriptor+8]`
(the callback) with `a3 = [descriptor+0xc]` (the common). For a
delay that callback is `0x005AEF58`, and `0x005AEF58` runs
`iSignalSema(a3)`. The chain is: helper → scheduler → arm →
timer handler → dispatcher → callback → signal. Every link was
read off disassembly; the temporary signal trace (since removed)
confirmed the shape while showing the callback still unreached.

## Step 2 — read what the game does to the handle

Two operations in that chain touch the semaphore id the helper
passed as "common":

- The arm function decides with `flags & 1`, where flags is the
  caller's value with bit 1 forced (`ori v0, v1, 0x2`).
- The dispatcher hands that same value to the callback, which
  signals it.

With sequential ids (1, 2, 3, …), half the nodes never activate
and every signal targets `handle | 2` — a semaphore that does not
exist. The game demonstrably requires handles with bits 0 and 1
already set, so the model hands out 3, 7, 11, … (decision 0012).
The exact real-kernel format is undocumented in the pinned
sources; this is the shape the game's own code requires, and the
decision says so plainly, including the revisit condition. The
kernel unit tests that pinned id 1 were updated — a test that
pins an invented value must move when evidence arrives.

Honest status at the time: the handle fix advanced the boot from
3,645 to 9,765 services, but the callback still did not fire. One
necessary condition is not the whole chain; the slice wrote down
exactly what remained (two active nodes the delay never joined).

## Step 3 — name the two node types, then watch the due test

Descriptor nodes (0x10 bytes, free list `0x0088C340`): id at +4,
callback at +8, common at +0xc. Timer nodes (0x40 bytes, free list
`0x006592F0+0x14`): id bits at +8, flags at +0xc (bit 0 = active,
bit 1 = armed), base at +0x10, accumulated at +0x18, scheduled at
+0x20, dispatcher at +0x28, gp at +0x2c, descriptor pointer at
+0x30. Instrumenting the helper's `WaitSema` block showed both
delay nodes scheduled and active (`flags = 3`) with their
descriptors attached — scheduling and insertion work. What never
passed was the TIM2 handler's due condition (target from
`scheduled + base - accumulated` against current from TIM2's
count): `accumulated` stayed 0, flags stayed 3, at every stop.

Two dead ends worth keeping: the slice first suspected the nodes
were never inserted (wrong — the watch showed them active), and
hypothesized an overflow-counter time base the model could not
match yet. The recommended next probe — a breakpoint-like hook at
the due comparison `0x005B822C` logging target and current — is
what actually broke the case open, but not the way expected.

## Step 4 — the watch that never fired, and the two real defects

The hook at `0x005B822C` never fired not because the comparison
failed but because the handler's loop body never executed — while
the handler was "delivered" ~4,700 times per 8,000-service run.
The deferred-call state showed why: one handler call stuck on the
stack forever, with causes queued behind it. Two model defects,
both in how injected handlers execute:

1. **Starvation by queued causes.** The driver injected before
   every bridge step, so with two causes queued the TIM2 handler
   ran one instruction, got preempted by VBlank, and never reached
   its body — busy-looking starvation.
2. **Mid-handler thread switches.** A handler's `iSignalSema` /
   `iWakeupThread` preempted to the woken thread through the
   scheduler, abandoning the handler frame; the stale frame then
   blocked all later injections.

The fix (decision 0013): no nested injections (`start_interrupt`
refuses while a handler call is active — the frame clears EIE,
and EXL says the same on hardware) and no preemption inside a
handler (the switch waits for `deferred_return`, matching the
kernel's deferral out of exception context). Result: the TIM2
handler runs to completion every idle frame, due comparisons
pass, callbacks fire, and the boot runs 1,000,000 services
(33,650,798 interpreted steps, ~29 s) with the differential still
passing at 3,000 services.

## What this arc does not claim (later evidence)

Slice 12's triumph is real — the machinery works — but later
slices reframed what firings *achieve*: the M32 march proved
delay firings are a lottery on COUNT alignment, and thread 3's
full lifecycle proved a firing can be sterile (wake, find
nothing, clean up, sleep deeper). Nothing here contradicts that:
this arc made the waits *capable* of ending; whether an ending
*advances the boot* is a separate question the project answered
later, honestly, against this arc's optimism. Similarly, the
differential reference drifts across these slices (7,508,945 →
7,554,609 instructions) because fixed behavior changes path
lengths — drift with a passing differential is expected, not
suspicious.

## Connection to our implementation

| Piece | File | Job |
| --- | --- | --- |
| Semaphore ids 3, 7, 11, … | `src/ee/kernel.cpp` (`create_sema`) | handles the game can bit-test and signal |
| No nested injections | `src/ee/kernel.cpp` (`start_interrupt`) | one handler runs at a time |
| Deferred switch | `src/ee/kernel.cpp` (`preempt_if_outranked`, `deferred_return`) | wakes wait, switches after return |
| Idle delivery accounting | `src/ee/kernel.cpp` (`deliver_idle_interrupt`) | counts only real deliveries |
| Handler tables + deferred counts | `tools/gt4boot/main.cpp` (`--threads`) | the state that exposed both defects |
| Guards and switch behavior | `tests/unit/ee_kernel_test.cpp` | non-nesting, deferred switch |

## Understanding checkpoint

1. The dispatcher calls the callback with `a3 = [descriptor+0xc]`.
   Why does a sequential id of 2 break *both* activation and
   signaling, and why does id 3 fix both?
2. The temporary signal trace "confirmed the shape while showing
   the callback still unreached". What does a trace prove when its
   target never fires, and what did the slice do next instead of
   trusting it?
3. The due-comparison hook never fired. List two hypotheses that
   explain a silent hook, and the evidence that selected between
   them here.
4. A handler's `iSignalSema` wakes a higher-priority thread. Why
   must the switch wait for the handler's return, and where does
   the woken thread go meanwhile?
5. Slice 12 ends with the boot running a million services; M32
   slice 20 ends with marching paused as sterile. Explain why both
   verdicts can be true without contradiction.
6. The decision records the real handle format as undocumented.
   What future evidence would force revisiting decision 0012, and
   which test would have to move with it?
