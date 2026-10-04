# M32 lesson — the delay march and the tripwire: honest maturation, then specified traffic

Prepared 2026-10-04. BUILD/VERIFY: slices 2–22, 29–31; see the
M32 slice docs (`docs/reverse-engineering/m32-slice*.md`) and
[decision 0023](../decisions/0023-async-iop-framing.md) /
[decision 0024](../decisions/0024-chained-checkpoints-and-idle-budget.md) /
[decision 0025](../decisions/0025-coalesce-pending-interrupts.md) /
[decision 0026](../decisions/0026-first-originating-event.md).
EXPLAIN: this is the worked explanation; tutoring review pending.

## Objective and motivation

M30 ends at a verdict, not a wall: the 243.7M-service stop is
*event starvation* — every thread waits on signals only
unmodeled events could send — not a circular deadlock. M32 is
chartered to supply the missing traffic, and this lesson is
about what happened when the project went looking for it:
first a long, honest march toward the one event the machine
seemed to await (a timer firing), then the discovery that
firings do not unblock anything, then a specified, synthesized
event with a tripwire that fails loudly the day anything wakes.
The through-line is negative results promoted to decisions.

The framing bet, recorded before the work: the first
originating traffic enters through SIF — an inbound packet for
the pump (provably empty queue) or a call into an EE-side
server loop. Thread 2, parked on semaphore 11, is shaped like
one. Both halves of that bet lose instructively.

## Step 1 — map the parked dispatcher before touching it

Thread 2 is not an EE-side RPC loop: it is the kernel's
deferred thread-ops dispatcher — wake/rotate/suspend over a
512-slot `{op,arg}` ring at `0x00885EE8`, consumer = producer =
`0xb5`, every slot `{0,3}`, created once by `0x005aea78`. Two
probe legs prove parked-stability rather than fragility: a
verbatim `{0,3}` replay re-parks in 2 services (consumed,
balanced), and a `{0,5}` sleeper wake flickers out in 8 (the
worker identifies itself, finds no work, sleeps). The pump
queue is provably empty (first byte zero at `[0x00886740]`);
the SIFCMD table and the 24 bound servers are inventoried (no
PADMAN — input waits on game progress, not model work).

Meanwhile the six semaphore-waiters unwind to one shape: all
blocked in `WaitSema` with the same return address
(`0x005aedc0`), each waiting on its own id — a one-shot
wait-then-delete handshake over delay-library work, with the
completion a delay-queue firing that signals the sema. The
bind machinery behind them gets mapped as a bonus (client
node, completion sema, `RPC_BIND` through the SIFCMD sender,
wait, delete) and exonerated: the stall is downstream, in
timed waits.

## Step 2 — prove the gate, then walk to it honestly

The TIM2 handler tests each node with `sltu current, target`
and leaves the whole walk on the first not-due node, where
`current = ((overflow << 16) | COUNT) << ((MODE&3)*4)` and
`target = scheduled + base - accumulated`. A poke experiment
makes the gate exact and binary: COUNT `+0x73800000` runs
bit-identical to baseline (below threshold); `+0x73E70000`
moves the machine (one `iSignalSema`, thread 3 readied, COMP
reprogrammed to `0x240`, game code rewriting COUNT to re-arm).
Below threshold nothing happens; above it the machinery runs.
Forecast, with bars: ~1.9e9 ticks past the checkpoint at
~0.59e9 per 12k leg means ~3–4 chained legs, no rate guessing.

The honest maturation path is tooling, not heroics: `--resume`
combines with `--checkpoint-at` (exact-count clean stops, new
chain CTest), and the idle budget rises 200,000 → 2,000,000 —
the guard is the model's own artifact, real hardware has none,
and the poke put maturation just past the old value. Three
chained 60k legs march COUNT forward identically each time.
The slice keeps its false alarm on record: the first long leg
after the raise stopped bit-identical at the old wall — a stale
binary (the build tail had hidden that `gt4boot.exe` never
relinked). Confirm the relink before reading run behavior;
`ctest` rebuilds, a bare `--target` run may mislead.

## Step 3 — attribute every firing, then learn what firings are worth

Attribution is complete down to the worker: six
node→descriptor→sema→worker rows (e.g. node `0x0088a000` →
descriptor `0x0088bf70` → sema 11482435 → thread 3, consumed
one-shot with idbits and flags cleared), and the base time
source (`0x005B8728` stamps from `0x005B8400`, computing exactly
the handler's formula — no scale break). Then the hunt turns
quantitative: legs D4–D17 sweep the firing band repeatedly —
including a full COUNT wrap mid-leg with no dispatch — and the
pattern that emerges is *lottery*, not level: the firing sliver
is sub-service while COUNT advances thousands per service, so
each band approach is a ticket, occasionally won. The 463k-deep
interrupt backlog explains the misses mechanically (fresh
timer interrupts queued ~140 legs behind stale frames), and its
fix is a model change with hardware semantics: coalesce INTC
causes on enqueue (a cause already pending stays one entry,
like a status bit; repeats carry no payload), while DMAC
completions keep stacking. Verification leg: pending 467,116
→ 1, handlers live every frame. Two self-inflicted delays are
recorded so later slices avoid them: erasing one dup per
enqueue (7k/leg — pointless, erase all matches), and another
stale-binary ambush.

Lockstep follows (100,000 services = 100,000 module calls, all
`0x100`: the machine matures the clock and nothing else), then
the natural firing the forecast promised: node `0x0088a000`
consumed one-shot mid-D9, thread 3 wait → running → asleep,
its sema deleted — a closed loop, and a sterile one (repeated
self-identification, no RPC, no pad, no onward signals). The
mechanism note corrects slice 3 in passing: redispatch after
handlers happens when the interrupted thread is not running,
which is why inside-handler signals dispatch on their own. The
outlook, stated as hypothesis with measured odds (~3 legs per
firing, five waiters): each firing retires one waiter into a
sleeper. The third wrap-miss closes the march: firings convert
sema-waiters into sleepers and nothing else — a deeper park,
not progress — so marching pauses until an event-side reason
restarts it.

## Step 4 — audit announcers, pick the cheapest probe, follow it

Every parked thread classified with its announcer — or marked
Unknown with the lead recorded: thread 2 wants its silent
producer; delay waits want dispatch (proven lottery-sterile);
condvar sleepers want engine completions and thread-2 jobs (all
silent for lack of input, not plumbing); sleeps want
`WakeupThread` (same silence). No thread waits on anything the
model answers but fails to announce: the machine is starved of
*originating* work from every direction at once. Of three
candidates — main's flag wait (unknown cost), the `0x587b`
frame dispatcher (object-graph forensics), the stuck SIF0
channel — the channel falsifies in 2 minutes (STR set, all
addresses zero: vestigial, doubly confirmed) and the pick is a
fourth: check whether the engine *emits* anything at all. The
pipe is dry (zero DMA starts in 12,000 services, all channels
idle at every stop): confirmed deep park, and the missing
heartbeat — who invokes the frame dispatcher per frame — is
the single upstream cause of parked workers and dry pipe alike.

## Step 5 — the specified event the negatives earned

The heartbeat trace finds the dispatcher's worker thread was
never created (one-shot flag 0, job global zeros — the creation
chain mapped to lazy printer-channel init that never runs);
all four `jal` targets into the region are internal; no thread
sleeps inside it. The lazy wrappers resolve to USB-printer
channels — peripheral work, not the boot's path. The switch
cases prove domain-neutral (thread sync, priority, cache,
break-traps; no print verbs, no frame verbs). A write-watch leg
over the flag word and the loop slot records zero stores with
a PASS-verified probe: total silence. Each negative narrows the
next step until only one synthesizable, hardware-faithful event
remains with a complete game-side consumer path — and decision
0026 specifies it end to end: one SIF pump SET_SREG packet
(count `0x18`, words `{0,1,0,1,1}`), DMAC-ch5 cause, first-idle
one-shot trigger as a pure function of the service sequence,
with a four-item verification bar whose load-bearing member is
a negative assertion (census identical before/after). Its
implementation runs the async path live (drain → re-kick →
table dispatch → register write → clean return) while the
census holds bit-identical — mechanism, not unblock, pinned
into the test suite so no later slice mistakes path-liveness
for progress. The chained-replay proof (flag 0→1 across the
firing boundary, verify-resume identical, re-save keeps flag)
and the ring-producer negative (indirect-only feeders, fossil
dynamics) close the arc.

## What this arc does not claim (kept explicit throughout)

- The poke's threshold response is binary *at the poked legs*;
  extrapolating rates carries the wide bars the slices state.
- Coalescing changes delivery order for *different* causes not
  at all; same-cause dups are the only thing erased, by
  hardware analogy, with a unit test pinning both halves.
- The sterile verdict covers the observed firings; it does not
  prove no firing could ever matter — it proves marching buys
  retirements at lottery odds, which is why the march paused.
- The fossil/poster question belongs to the arc's own later
  slices (33–34), referenced here only as the forward pointer
  the charter allows: era falsified by verified-quiet legs,
  resolutions ranked, tripwire standing watch.

## Connection to our implementation

| Piece | File | Job |
| --- | --- | --- |
| Chained checkpoints + chain test | `tools/gt4boot` (`--resume` + `--checkpoint-at`, `--verify-resume`) | exact links, bit-identical replays |
| Idle budget + coalescing | `src/ee/kernel.cpp` (`idle_interrupt_budget`, same-cause drop) | bounded guards, live handlers |
| Delay/walk machinery (observed) | game code via disassembly + temp instruments | gate formula, attribution rows |
| Specified packet + trigger | `src/ee/kernel.cpp` (one-shot SET_SREG, ch-5 completion, snapshot flag) | first modeled traffic, deterministic |
| Census tripwire | `gt4boot_originating` CTest + `--threads` dumps | register set, nobody woke |
| SIFCMD table + server inventory | `--threads` sid listing | permanent arrival-path map |

## Understanding checkpoint

1. The poke's two legs differ by one COUNT increment and
   produce bit-identical vs. moved machines. Why does that
   binary response prove the *gate* rather than merely
   correlating with it — and what did it rule out about rates?
2. Chained checkpoints preserve progress but "cannot extend a
   single idle stretch". Explain the counter mechanics, and why
   raising the budget was the adoption while chaining stayed
   tooling.
3. A 463k-deep FIFO makes every fresh timer interrupt wait
   ~140 legs. Derive that number from the census, and explain
   why coalescing fixes phasing without changing delivery
   order across causes.
4. Thread 3's lifecycle is wait → running → asleep with its
   sema deleted. Why is that closed loop *sterile* rather than
   progress, and what single observation would have flipped it?
5. The announcement audit found "no sync-without-async gap".
   What would such a gap have looked like, and which census
   class came closest to fearing one?
6. Decision 0026's verification bar centers a negative
   assertion (census unchanged). Why is the negative
   load-bearing while the register-write assertion is not —
   and what future observation is the tripwire positioned to
   catch?
