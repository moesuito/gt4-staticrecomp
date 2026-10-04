# M30 lesson — handshakes, the register mirror, and the service clock

Prepared 2026-10-04. BUILD/VERIFY: passed for slices 13–15; see the
[M30 slice-13 evidence](../reverse-engineering/m30-slice13-service-handshakes.md),
[slice-14 evidence](../reverse-engineering/m30-slice14-sif-register-mirror.md),
[slice-15 evidence](../reverse-engineering/m30-slice15-service-clock.md),
and [decision 0014](../decisions/0014-service-handshakes.md) /
[decision 0015](../decisions/0015-sif-register-mirror-and-liblgdev-sync.md) /
[decision 0016](../decisions/0016-service-clock.md). EXPLAIN: this
is the worked explanation; tutoring review pending.

## Objective and motivation

The handler-execution fix left the boot *running but waiting*: the
game lives in library wait/retry loops, and every loop is gated on
a service handshake the model does not satisfy. This arc teaches
the project's handshake discipline: trace each retry loop to the
exact check that fails it, answer from the game's own constants
and checks — never invented values — and when the waits turn out
to be *time*, give the model a clock both engines can share. It
ends with the 1,000,000-service run stopping at a service
boundary with the worker threads ready.

The motivating shape is the retry loop at `0x00577570`: call the
file open at `0x005B6C68`, delay 2,000, retry while the open
fails. The open fails before sending anything. Something in the
setup disagrees with the model, and the disagreement has an
address.

## Step 1 — answer version checks with the game's own words

The file-open path binds the file server (sid `0x80000006`) and
checks its version query (RPC `0xFF`) against the constants at
`0x0065829C` ("3000") or `0x0066835C`. The model answered zeros;
the open failed. The rule for every handshake in this arc: the
model reads the answer out of guest memory, so the answer and
the game's check stay in lockstep — hardcoding would drift from
the pinned image, and an unmapped constant stops loudly instead
of answering zeros. The SIF manager (sid `0x80000001`, RPC
`0xFF`) answers the word at `0x0066829C` (`0x00275520`) plus the
flag 2 its client checks.

With the version answered, the opens flow (RPC 0, 512 in, 8 out)
and the boot walks a chain of further handshakes, each answered
the same way — from the game's own checks:

- **Deci2Call (0x7C)** is accepted with the reference emulator's
  returns (1 for defined calls, -1 beyond `0x10`): no debug host
  is attached, exactly as on a console without one.
- The disc subsystem's status (sid `0x80001300`, RPC `0x80001363`,
  144 bytes) answers first word `0x310` — the *lowest* value the
  check `(reply[0] >> 4) == 0x31` at `0x0058F840` accepts.
- The fileio/CDVD negotiation (sid `0x80000400`, RPC `0xFE`)
  answers the *minimum* versions the checks accept (second word
  `0x20A`, third `0x20E`): minimums keep the game on the oldest
  protocol variant it supports, which is the variant the
  empty-result model can represent without inventing semantics.
- The model IOP's server table grows 16 → 80 slots (handles at
  `0x00020000 + slot*0x10`, buffers and connections alongside,
  the command buffer address kept free between them) — because
  running out is a model failure, not a game condition, and the
  game binds string-coded servers (`0x50636476` among them).

The run that results binds the disc subsystem, negotiates,
creates its 11-thread worker pool with string-coded servers
("Pusb", "PUPS", "MGBP", …), and reaches the 200,000,000-step
limit inside the `0x0058F000` subsystem init — executing, not
deadlocked.

## Step 2 — mirror the register the spin waits on, then pass the device sync

The next stop is a spin with a readable shape: `while
(0x005B0880(1) == 0)` at `0x00590A18` — a getter for the
command layer's register array (base `0x008869C0`). Its
function `0x00590978` registers a handler for cid `0x80000018`,
sends command cid `0x80000001` with payload `{1, 1}`, and spins
until register 1 is non-zero. The handler table at `0x00886840`
resolves the cids (entry 0 = `0x005B0870`, entry 1 =
`0x005B0850`, the set-register writer storing into
`register[packet[0x10]]`; the public `sifcmd-common.h` names
the same numbering), so only an incoming SET_SREG can write
what the init waits on. The model therefore mirrors an incoming
SET_SREG back through the EE command buffer — the EE's own
handler path, which also carries the game's other register
traffic, instead of writing the array directly. Live memory
confirms the real handshake's result: registers 0 and 1 both
set. Writing the array from the model's side was considered and
rejected as the larger, less faithful change.

Cleared of the spin, the boot hits the device library's
deliberate trap: `0x00560778` binds server `0x046D046D`
(liblgdev, banner "version 1.11.036" at live `0x006C8D40`),
sends RPC 12 (576 bytes each way), and accepts only status
`0x010B2400` at reply+4 (completed; `0x010Bxxxx` takes a partial
path; anything else falls into the trap). The model answers the
completed code with the rest zeroed — the real structure comes
from the IOP module the model does not execute. The reward is
the device polling round: PCDV, MGBP, PUPS, liblgdev RPCs
flowing steadily to the service limit (1,000,000 services,
1,710,779 module calls, 46,608,011 interpreted steps).

## Step 3 — give starving delays a clock both engines share

At the 60,000-service limit the main thread still waits on the
same delay semaphore it entered thousands of services earlier:
the model advanced its clock only at idleness, and the polling
round never idles. The delay library signals from the timer
library's dispatcher, which runs on TIM2 due crossings — and due
needs *time passing while code runs*.

`Kernel::advance_service_time` advances one millisecond of
BUSCLK ticks (147,456) per *handled service*, called
identically by both engines at their service boundaries (the
driver through `RunOptions::advance_time`, the interpreter
directly). The choice of unit is the whole decision: an
instruction-count clock would differ between engines and break
the differential; a cycle-accurate clock does not exist in the
model; services are the one thing both engines observe
identically. Timers divide by their CLKS selector (1, 16, 256,
horizontal-blank ratio) with fractional remainders so no tick is
lost; the compare flag sets only on *crossing* COMP (a handler
that reprograms COMP keeps its period); slices accumulate
toward one VBlank per frame (2,457,600 ticks) under the idle
source's registration rule.

Measured, not asserted: a temporary dump showed TIM2 advancing
576.05 ticks per service at CLKS = BUSCLK/256 — one millisecond
to within 0.01% — with the game's own library reprogramming
COMP as the run proceeds. The main thread's wait target moves
(stuck 667 → 16807 → new delays), and the million-service run
ends at a service boundary (pc `0x005AEB78`) with workers ready.
The differential reference drifts 7,573,241 → 7,570,583
instructions *because* queued VBlank/timer causes now reach
busy execution in both engines — drift with a passing
differential, as always, not rot. The documented shortcut
stands: one millisecond per service freezes a clock for code
that runs long without services, and a microsecond service
advances a full millisecond; the idle path keeps its
frame-per-interrupt jump.

## What later evidence reframed (not smoothed over)

- The polling round's empty replies were genuine frontier at
  the time ("answer the first call whose reply the game acts
  on, live oracle") and were genuinely resolved afterward by
  the file/disc work — the lesson's "next frontier" pointers
  are how the project queues work, not hedges.
- The SET_SREG mirror came back in a new role much later:
  decision 0026's synthesized first event reuses the same pump
  queue, SIF0 completion, and dispatch table the mirror path
  mapped — same protocol family, opposite direction (model
  writes queue bytes the pump drains, instead of answering
  through the command buffer). The table-entry/consumer
  readiness gate of that decision is a later refinement, not a
  correction.
- The service clock became load-bearing infrastructure: every
  differential since depends on both engines calling it
  identically, and decision 0026's trigger cites 0016 as its
  determinism model. A clock both engines share is the reason
  later event work can be deterministic at all.

## Connection to our implementation

| Piece | File | Job |
| --- | --- | --- |
| Version/version answers | `src/ee/kernel.cpp` (RPC dispatch, guest-memory constants) | checks pass from the game's own words |
| Deci2Call/status/negotiation | `src/ee/kernel.cpp` (0x7C returns, 0x310 floor, 0x20A/0x20E minimums) | oldest acceptable protocol paths |
| 80-slot server table | `src/ee/kernel.cpp` (handles/buffers/connections layout) | binds never fail on capacity |
| SET_SREG mirror + liblgdev sync | `src/ee/kernel.cpp` (mirror packet + SIF0 completion, `0x010B2400`) | spin cleared, device bound |
| Service clock | `src/ee/kernel.cpp` (`advance_service_time`, CLKS division, crossing COMP, VBlank slices) | time both engines share |
| Trigger hook | `RunOptions::advance_time` + interpreter call site | identical invocation, exact differential |
| Handshake/version tests | `tests/unit/ee_kernel_test.cpp` | answers, returns, clock crossing |

## Understanding checkpoint

1. The retry loop calls the open, delays 2,000, retries while it
   fails. Why does answering the version query break the loop,
   while answering zeros sustained it — and what does that imply
   about where "retry forever" logic belongs in the model?
2. Minimum versions keep the game on its oldest protocol. Why
   is the oldest the one the empty-result model can represent,
   and what would "large" versions have forced the model to
   invent?
3. Register 1 could have been set from the model's side
   directly. Reconstruct why the mirror through the EE command
   buffer is the smaller change, naming the second beneficiary.
4. A microsecond service advances a full millisecond of BUSCLK.
   Give a concrete guest behavior this shortcut mis-times in
   each direction, and explain why the differential still
   passes exactly.
5. The reference drifts 7,573,241 → 7,570,583 across the clock
   slice. What changed in the engines' shared behavior to move
   the count, and why is that drift healthy?
6. Decision 0026 reuses this arc's pump machinery in the
   opposite direction. What does the mirror direction assume
   that the synthesized direction must additionally guarantee
   — and which added gate provides it?
