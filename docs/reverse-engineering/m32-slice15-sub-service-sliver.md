# M32, fifteenth slice — the walk runs constantly; the sliver is jumped over

Date: 2026-10-03. Inputs: the pinned CORE and ISO; handler-start
counters (temporary, since removed), a three-leg poke bracket (temporary
`--poke-count`, since removed), and fresh disassembly reads. No model
change. This slice settles reachability affirmatively — and thereby
moves the paradox to the operands' scale, with a mechanism that fits
every leg in both directions.

## The walk runs constantly (Confirmed — counters, since removed)

12,000 timer-11 handler starts per 12k leg — once per service, the
whole leg. Gate (`MODE & 0x400`, live `0x782`), head (`0x00889f80`,
verified live every leg), overflow branch (skipped, `MODE & 0x800`
clear) all check out. The due test executes against live values
constantly — and never dispatches.

## The bracket (Confirmed)

From `ckpt-1580k.bin`, poking COUNT then running short legs:

- P1 (0xFDCF63C0, poke 2's exact start, 3,000 svc): nothing. Same start
  value, no reproduction — the firing is not a function of the start
  level.
- P2 (0xFFA235C0, 500 svc): ended 3,216 combined-ticks short of the
  computed threshold. A near miss by the math's own numbers.
- P3 (same poke, 800 svc): COUNT wrapped to `0x001e6dc0` mid-leg, no
  firing — the sliver and the ceiling both swept silently.

## The mechanism that fits everything (high confidence)

The firing sliver (combined within ~2–5k of the 32-bit ceiling) is
narrower than one frame's COUNT advance (erratic 3k–170k per service
across legs). Consecutive walk runs step clean over it; only freak
slow-advance alignments land inside (slice 5's poke, slice 12's D9).
The code, the fields (re-verified at the arm site `0x005b8b68`:
sched@+0x20, disp@+0x28, gp@+0x2c, desc@+0x30, flags@+0xc), the gate,
the head, and the dispatcher (`0x005b8ed8`: desc callback plus
free-list management, no direct callers — the walk's `jalr` only) all
check out. Nothing is broken; the timeouts are simply unreachable by
time in any faithful model — on hardware too, at these scales.

## Reframe (Hypothesis, now leading)

Completion must come via cancellation: unlink + signal without the due
test, driven by the awaited subsystem's real event (the reply whose
arrival retires the wait). That reframes M32's real work back to its
charter — originating IOP→EE traffic — instead of clock-watching.
Slice 16 identifies each wrapper's request (the `0x5b8d88`/`0x5b8f38`
arguments recoverable from the checkpoint stacks) to name the reply
path worth modeling asynchronously. The poke-2/D9 consumptions stay
attributed to an unknown path (cancellation candidate), and the
overflow counter's sporadic climbs (95 → 101) stay a minor Unknown.

## Verification

- All probe instruments removed (grep-clean); product code untouched.
- Deterministic prefixes reproduce; saves need clean Syscall stops.
- Full gates run on the final tree before commit.
