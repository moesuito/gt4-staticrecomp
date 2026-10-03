# M32, third slice — the first packets: replay and a sleeper wake

Date: 2026-10-03. Inputs: the pinned CORE and ISO, resumed from the
243.7M checkpoint. Temporary `--feed-job` / `--feed-wake` instruments
(since removed) replayed one producer step through the ring slice 2
mapped. Both legs are verified mechanism, not model: the tree keeps no
new behavior.

## Replay `{0,3}` (Confirmed)

One verbatim producer step — `{0,3}` at slot 181, producer `181→182`,
`SignalSema(11)` — plus the dispatch the driver would have run:

- `fed job {0,3} at slot 181, signal outcome 0 (Handled), dispatched 1`
- Thread 2 ran exactly its loop: `service 0x33` (`WakeupThread` from
  `0x005adbd4`) then `service 0x44` (back to `WaitSema`). Consumer
  `0xb5→0xb6`, balanced again. **2 services, 24 interpreted steps.**
- The machine re-parked (`no-runnable-thread`, now at the loop's wait).
  `WakeupThread(3)` left no visible unblock: thread 3 still sema-waits
  (its stored credit absorbs it).

The push-button works end to end: write + count + signal → dispatch →
wake → wait. The parked state is stable under its own historical input.

## Wake `{0,5}` (Confirmed)

The same step aimed at sleeper thread 5:

- Thread 2 dispatched `WakeupThread(5)`; then **five `GetThreadId`
  (`0x2f`) calls and one `SleepThread` (`0x32`)** — 8 services, 336
  steps — and the machine re-parked with thread 5 asleep again
  (its stored wakeup reads 0: spent).
- So a sleeper's program on waking is a brief flicker (identify itself
  repeatedly, find no work, sleep). No new SIF, RPC, pad, or disc
  traffic; no unmodeled wall. Hypothesis on intent, Confirmed on the
  trace.

## Probe methodology note (Confirmed)

The first replay attempt omitted the dispatch: the signal readied
thread 2 (status `0x4→0x1`) but the idle spin at `0x1604` never yields,
so the leg burned its whole service budget still running without ever
scheduling it. Dispatching pre-run (exactly what the driver does on a
`Switched` outcome) fixed it. This is **not** a scheduler gap: in real
flows the signal lands inside a syscall/handler where the dispatch
follows. Buried-in-the-queue injections (slice 50) stay buried for the
same structural reason.

## Verdict (high confidence)

The machine is not fragile-parked: feeding its own historical jobs
changes nothing downstream, and waking a sleeper only flickers. The
boot genuinely needs *new* input — job shapes the producer never sent
here, or a producer that fires again — which is milestone work (the
wild producer of slice 2, i.e. real IOP-side events), not another
replay. Next is slice 4: name the writer.

## Verification

- Both probe legs end re-parked with counters balanced; the
  instruments are removed (grep-clean); the checkpoint file is
  untouched (all probes ran on resume).
- Full gates green on the final tree: CTest 40/40, Python 73 (67 run,
  6 skip).
