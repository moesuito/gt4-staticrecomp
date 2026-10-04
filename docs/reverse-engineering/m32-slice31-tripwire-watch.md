# M32, thirty-first slice — tripwire watch: one-shot proven, no consumer yet

Date: 2026-10-03. Inputs: the pinned CORE and ISO; chained
checkpoint legs (fresh 400 → save, fresh 20000 → save, resume +
verify + chained re-save, park re-census); a Python checkpoint
reader (recipe below, scratch deleted). Read-only throughout: no
product change, no instruments, no model change.

Charter: (A) prove the one-shot flag across chained replay;
(B) map the next waiter the live pump path could consume, or
justify stand-watch.

## (A) Chained-replay proof (Confirmed)

- Fresh 400-service leg → `ckpt-31a.bin`: flag **0** (SIF/table
  not up yet; pre-fire baseline).
- Fresh 20000-service leg → `ckpt-31b.bin`: flag **1** (fired
  during the leg; matches the drained queue seen at 20k).
- `--verify-resume ckpt-31b +2000`: **states identical**
  (22000 services, digest `0x86eb07b0a8a403c7`) — chained resume
  across the firing boundary reproduces the direct run
  bit-for-bit.
- Resume 31b +2000 → `ckpt-31c.bin`: flag still **1** — the flag
  survives chained save/load; combined with the unit proof
  (restored kernel never re-fires, save-load-save identical), a
  replay is excluded two independent ways. (Direct re-fire would
  in any case be unobservable — idempotent bytes — which is why
  the file-level read, not behavior, carries this proof.)
- All legs exit 0 with clean checkpoint saves.

Reader recipe (deleted after use; retype from the doc): file =
`GT4CPT1` + u64 services + u32-len sections (context, kernel,
banks); kernel section starts `GT4KERN1`; the flag is its last
u32 (absent in pre-decision blobs, which read as unsent).

## (B) Next-waiter map: no consumer yet (Confirmed census, justified stand-watch)

Park re-census from `ckpt-1980k.bin` is identical thread-for-thread
to every historical census (17 threads: T2 on sema 11; T4/6/8/11/18
on the same five delay semas; eleven sleepers at the same pcs —
`0x005adbc8` sleepers, `0x005adce8` sema-waiters). Against what pump
traffic could post next:

- Thread 2's ring: pump handlers are proven register accessors;
  they cannot post `{op,arg}` jobs. Producer still Unknown (most
  likely the delay system itself). NO.
- Delay semaphores: only walk dispatch signals, proven sterile.
  NO.
- Condvar sleepers + main's flag + thread 9/3: all need game-side
  announcers (engine WakeupThread, poster re-invocation, own
  paths). The pump reaches none. NO.
- No thread waits inside an RPC/bind/call path (no new wait shape
  vs any prior census), so even a future RPC reply has nobody to
  consume it. The pump table's indices 2–31 remain zero-filled.

Verdict: **no consumer yet — tripwire stands watch**. No slice-32
event is specified because there is nothing for one to wake;
specifying traffic without a waiter would repeat the fabrication
decision 0026 already rejected. What promotes a slice-32: (1) the
census tripwire trips (any wait change in a future leg); (2) the
ring producer is identified statically; (3) a waiter appears in an
RPC wait. Until then, milestone event work continues elsewhere.

## Grades

- Confirmed: flag 0→1 across the firing boundary (file reads);
  verify-resume identical at 22000; chained re-save keeps flag 1;
  census identical to baseline; table/queue/register bytes as in
  slice 30.
- High confidence: no consumer exists (exhaustion over the census
  against the proven handler semantics).
- Unknown (unchanged): ring producer identity; SIF-register
  readers at large.

## Verification and hygiene

- No code touched (read-only slice); no instruments written.
- Scratch deleted: 3 run logs, verify log, recensus log, 3
  checkpoints (~100 MB), the reader script, the `.cmd` runner.
- Gates unaffected (nothing to gate); review + commit per rules.

Next (recommended): milestone work per decision 0023; the tripwire
(CTest `gt4boot_originating` + parked censuses) fails loudly the
day anything wakes — chase that signal, not a schedule.
