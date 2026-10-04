# 0030 — One timer advance machine for both quanta (P03)

Date: 2026-10-04. Status: accepted (implemented in slice 66).
Predecessors: 0016 (service clock), 0028 (compatibility policy),
0029 (P01+P02 timer/interrupt contracts).
Plan: PLAN.md section 6, P03 (depends on P01+P02).

## Context

P01+P02 left two separate advance call sites with their own quanta:
`advance_service_time` added one millisecond of BUSCLK ticks per handled
service (147,456 ticks, with per-timer fractional remainders), while the
idle path (`advance_timers`) added one frame of each timer's clock source
per call with no remainder at all, plus an explicit VBlank queue attempt.
The CLKS=3 idle step (262) silently dropped ~0.23 counter ticks per frame,
and a partial service accumulation could never complete a frame together
with an idle call. PLAN.md P03 requires one shared advance machine,
combined compare/overflow handling without lost crossings, remainders kept
across paths, COMP reprogramming honored, no skipped service
opportunities on large jumps, and gate/ZeroReturn modes treated or
declared as an explicit limit.

## Decision

1. **One machine, two quanta.** New `Kernel::advance_busclk(state,
   busclk_ticks)` moves every counting timer by its clock's share of the
   delta and accumulates VBlank toward one frame on the same shared
   state. `advance_service_time` is `advance_busclk(service_time_slice)`
   and the idle `advance_timers` is `advance_busclk(busclk_per_frame)`.
   The quanta (1 ms/service, 1 frame/idle) are unchanged; only the
   mechanism is shared. Both engines call the two quanta in the same
   order, so the translator-vs-interpreter differential stays exact.
2. **Shared remainders, shared accumulator.** The per-timer CLKS
   division remainders and the frame accumulator are now common to both
   paths (members renamed to `timer_remainders_` and
   `busclk_accumulator_`). The snapshot keeps the model-2 wire order
   (one accumulator word, then four remainder words); only the meaning
   widened, which is why `time_model` goes 2 -> 3. Every pre-change
   checkpoint is forensic automatically through the 0028 gate.
3. **Exact division, documented HBLANK rate.** CLKS 0-2 divide by
   1/16/256; CLKS 3 divides by 9372, the integer nearest the true
   BUSCLK/HBLANK ratio (147456000/15734 = 9371.6...). The shared
   remainder absorbs the drift. The long-run rate is a deterministic
   model policy, not a hardware claim. The old idle path's flat 262 for
   CLKS 3 is replaced by 262 plus the preserved remainder (one extra
   tick about every 4.4 frames): the only intended counting change,
   deterministic on both engines.
4. **No double VBlank.** The idle source's explicit per-call VBlank
   queue attempt is removed: one frame-sized delta completes exactly one
   frame on the shared accumulator (it always rests below one frame, on
   every path including restores), so each idle call still offers
   exactly the one VBlank opportunity the old source offered. VBlank
   still requires a registered handler, and repeats still coalesce.
5. **Crossings are never lost inside an advance.** `TimerUnit::add_ticks`
   (unchanged from 0029) lands exactly on each compare passage and each
   wrap, reports both edges together, and a compare at or behind the
   counter waits for the next wrap. One advance queues the timer's
   single INTC cause once; repeats coalesce onto it (one status bit per
   cause, hardware-faithful). A giant advance therefore sets one sticky
   bit however many wraps it spans: the guest's extended-time routine
   counts every wrap only with its acknowledge-between-advances rhythm,
   which the new tests pin (3 wraps in, 3 overflows counted).
6. **Reprogramming steers the next advance.** Every advance reads the
   live COUNT/COMP/MODE, so a handler that rewrites COMP or
   acknowledges a flag between two advances changes the next one's
   trajectory. Decomposition with no guest writes between pieces is
   exact and pinned at both levels: `add_ticks(N)` equals any split of N
   (COUNT, flags, edge-OR), and `advance_busclk(T)` equals any split of
   T across the service/idle quanta (whole kernel snapshot blob plus the
   full timer register photo, byte for byte). Dispatch opportunities
   between events still belong to the driver loop (unchanged); P04 owns
   handler arguments and the idle return.
7. **Gate holds, ZeroReturn runs.** A live 90,000-service disc boot
   (stop-time `--dump` of all four timer windows) shows the game
   programs exactly one timer: T2 as CUE|CMPE|OVFE, CLKS=BUSCLK/256,
   COMP 0 (MODE 0x0382, no flags pending). GATE and ZeroReturn are not
   programmed through 90k services. ZeroReturn is fully treated in
   `add_ticks` (reset gated on CMPE, multi-period advances, per-period
   edges with the acknowledge rhythm, all pinned by tests). Gated timers
   hold their count for every GATE/source/mode mix: without an
   HBLANK-signal model the hold stays the explicit limit, now with live
   evidence that the game does not use it in the observed prefix. The
   DMAE/D_ENABLER gate stays unmodeled (0029 item 7, unchanged).

## Consequences

- Quanta, snapshot wire order, dispatch, coalescing, JR/ERET (P05), DMA
  payload/tags (P06), RPC content (P07) and handler a1/a2 (P04) are
  untouched.
- The VBlank framing visible to the game is unchanged when only one
  path contributes (exact divisions, accumulator resting at zero), and
  differs only where the paths interleave (shared remainder and shared
  frame completion): deterministic on both engines, covered by the
  differential and the resume/autosave fixtures.
- P04 inherits: the dispatch opportunities the reprogram tests use
  (handler frame, return, second dispatch), still with the P04-owned
  argument/return semantics pending.

## Verification

Extended fixtures ee_timer (ZeroReturn rhythm, 8-row decomposition
battery, all gate mixes, COMP reprogram) and ee_kernel (frame-split
identity across three wirings, kernel-level combined edges, dispatch +
reprogram + second dispatch, extended-time count vs single sticky bit,
shared accumulator, gate/ZRET through the machine) with no parallel
harness. CTest 50/50 (includes gt4boot_services 90,000 with disc and
the compare-interpreter differential, plus all resume/autosave
fixtures); Python 73 collected, 67 run, 6 skip, exit 0 with the two
known socket ResourceWarnings. Live MODE probe at the 90k stop
documents the game's timer programming. Evidence:
docs/reverse-engineering/slice66-p03-advance-machine.md.
