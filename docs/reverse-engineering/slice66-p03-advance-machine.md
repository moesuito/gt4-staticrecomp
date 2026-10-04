# Slice 66 — P03: one timer advance machine for both quanta

Date: 2026-10-04. Plan: PLAN.md section 6, P03 (slice 66 = P03).
Decision: docs/decisions/0030-p03-advance-machine.md.
Baseline: main efc08af (P01+P02 committed), 50/50 CTest + Python 73 (6 skips).

## What changed (files and lines, approximate)

- include/gt4recomp/ee_kernel.hpp: new public `advance_busclk(state,
  busclk_ticks)` with the unified-machine contract; `advance_service_time`
  and `deliver_idle_interrupt` comments updated; `service_ticks_` renamed
  to `busclk_accumulator_` and `service_timer_remainders_` to
  `timer_remainders_` with the wire-order note; `advance_timers` comment
  fixed (it also un-glues a comment that had merged with the pump-packet
  note).
- src/ee/kernel.cpp: `advance_timers` is now `advance_busclk(...,
  busclk_per_frame)`; `advance_service_time` is now
  `advance_busclk(..., service_time_slice)`; the single machine does the
  CLKS division with shared remainders, edge queueing, and the shared
  VBlank accumulator; the idle source's explicit VBlank block is removed
  (exactly one frame completes per idle call by the accumulator
  invariant); save/load use the renamed members in the same wire order.
- include/gt4recomp/ee_timer.hpp, src/ee/timer.cpp: gate comments updated
  from "P03 territory" to the decision-0030 explicit limit. No behavior
  change in `add_ticks`.
- include/gt4recomp/ee_checkpoint.hpp: `time_model` 2 -> 3 with the slice
  note. Pre-change checkpoints refuse through the 0028 gate.
- tests/unit/ee_timer_test.cpp: ZeroReturn multi-period plus acknowledge
  rhythm, 8-row add_ticks decomposition battery, all five gate mixes,
  COMP reprogram steering. Extended in place (no parallel harness).
- tests/unit/ee_kernel_test.cpp: one-frame identity across three
  service/idle wirings (snapshot blob plus register photo), kernel-level
  combined compare+overflow with coalescing, dispatch plus COMP
  reprogram plus second dispatch, extended-time count (3 wraps in, 3
  counted) vs the single sticky bit of one giant advance, shared VBlank
  accumulator across late registration, gate hold and ZeroReturn restart
  through the machine. Extended in place.

## The unified machine (design sketch)

~~~text
advance_service_time(state)          advance_timers(state) [idle]
        | 147456 BUSCLK ticks                | 2457600 BUSCLK ticks
        v                                      v
              +------ advance_busclk(delta) ------+
              | per timer (if CUE):              |
              |   remainder += delta             |
              |   step = remainder / divisor     |
              |   remainder %= divisor           |
              |   add_ticks(step) -> edges       |
              |   edge => queue INTC 9+index     |
              | accumulator += delta             |
              | while accumulator >= frame:      |
              |   accumulator -= frame           |
              |   VBlank => queue cause 2        |
              |   (only if handler registered)   |
              +----------------------------------+
divisors: CLKS 0/1/2/3 -> 1/16/256/9372 (9372 ~= 147456000/15734,
remainder absorbs the drift; deterministic model policy).
~~~

Decomposition holds because the remainder division is exact for any
split, `add_ticks` chunking is order-exact, and queue coalescing is
idempotent. Guest writes between pieces (COMP rewrite, W1C acknowledge)
intentionally steer the next piece: that is the handler-reprogramming
opportunity, and the driver loop between advances is where dispatch
happens (unchanged; P04 owns arguments and the idle return).

## Live evidence: what the game programs (90k-service disc boot)

Stop-time `--dump` of all four timer windows at the parked frontier
(boundary syscall 0x00001604 service 0x100; 197,330 module calls,
4,779,652 interpreted steps, 90,000 services):

| Timer | COUNT | MODE | COMP |
|---|---|---|---|
| T0 0x10000000 | 0 | 0 | 0 |
| T1 0x10000800 | 0 | 0 | 0 |
| T2 0x10001000 | 0xd640 | 0x0382 | 0x0000 |
| T3 0x10001800 | 0 | 0 | 0 |

T2 MODE 0x0382 = CUE|CMPE|OVFE with CLKS=2 (BUSCLK/256), no GATE
(0x04), no ZRET (0x40), no flags pending. The game uses exactly the
SDK-shaped timer the delay library needs; GATE and ZeroReturn are not
programmed anywhere in the observed 90k-service prefix, and no timer
uses the HBLANK selector. Raw probe log (outside the repo):
`slice66-mode-probe.txt` in the session temp dir.

## Behavior deltas vs the old code (all deterministic, both engines)

1. CLKS=3 idle steps gain the preserved remainder (262 plus ~1 extra
   tick every 4.4 frames instead of flat 262). No live timer uses CLKS 3
   through 90k, so the live boot is unaffected.
2. Service and idle accumulations complete each other's frames (shared
   accumulator and remainders). Single-path sequences with exact
   divisions are bit-identical to before (all pre-existing service and
   idle tests pass unchanged).
3. The idle explicit VBlank attempt is gone, replaced by exactly one
   accumulator completion per idle call (invariant: the accumulator
   always rests below one frame, including across restores).

## Test results and counts

- ee_timer: all pre-existing rows pass unchanged; new ZeroReturn
  rhythm (3 periods in, 3 counted, count 0), 8 decomposition rows
  (whole vs split: COUNT, MODE, edge-OR), 5 gate mixes held, COMP
  reprogram (old point silent, new COMP fires). Pass.
- ee_kernel: all pre-existing rows pass unchanged (service compare
  silence, VBlank framing at 17 services, 0xFFF0 wrap, coalescing,
  SIF/originating/snapshot/volume suites); new unified identity (3
  wirings byte-identical), kernel combined edges with coalescing,
  two-dispatch reprogram scenario, 3-overflow count vs sticky bit,
  shared accumulator with late VBlank registration, gate/ZRET through
  the machine. Pass (8.7 s, dominated by the 209-chunk overflow loop
  and the three full-frame wirings).
- Full runs: CTest 50/50 (gt4boot_services 90,000 with disc and the
  compare-interpreter differential green in 15 s; all resume/autosave
  fixtures green on the unchanged wire order); Python 73 collected,
  67 run, 6 skip, exit 0, with the two known socket ResourceWarnings.
- Build warning-free (MSVC 19.44, Ninja, Debug).

## P03 acceptance (PLAN.md section 6, P03)

- 0xFFF0 + 576 -> 0x0230 with overflow correct: covered (pre-existing
  kernel row through the new machine, plus the decomposition battery's
  P03 row at unit level).
- COMP not crossed produces no EQUF: covered (pre-existing rows, plus
  the behind-counter decomposition row and the reprogram silence check).
- Combined compare and overflow classified correctly: covered (unit
  combined row, kernel single-cause single-entry row, both flags named).
- The guest extended-time routine grows coherently: covered by the
  mechanism it relies on (acknowledge rhythm counts 3/3 wraps with
  exact COUNT; the single-sticky-bit contrast documents why the rhythm
  is required). The routine itself is guest code, unchanged.
- Decomposition without guest intervention is verifiable: covered
  (8-row unit battery; three-wiring kernel identity over snapshot blob
  plus register photo).
- With reprogramming handlers, the scheduler offers the predicted
  opportunities between events: covered (dispatch, guest COMP rewrite,
  silence before arrival, fire on arrival, second dispatch with context
  restored twice).
- Gate/ZeroReturn modes the game uses are treated or an explicit
  limit: covered (ZeroReturn fully treated and pinned; GATE hold for
  all mixes pinned and declared the limit, with live evidence the game
  does not program it through 90k; DMAE/D_ENABLER still unmodeled per
  0029).

## Out of scope, unchanged (verified by green suites)

JR/ERET (P05), DMA payload/tags (P06), RPC content (P07), handler
a1/a2 (P04), thread accounting, quanta values, snapshot wire order,
dispatch and coalescing policy. No TAG END artifact, no generic
success, no bytes over 127 in new code comments.

## What P04 inherits concretely

The K3 test is the seam: a timer cause dispatches its handler frame,
the stub return restores the interrupted context, and a second event
dispatches again. What is still P04-owned: the handler's a1 argument
(currently the cause is passed as a0 and the registered argument is
stored but not delivered), the a2/addr confirmation, the
interrupted-idle context representation, GetThreadId in that context,
and dispatch-on-return policy (no nesting/preemption preserved until
evidence demands review). The unified machine gives P04 exact,
split-independent event timing to build on.
