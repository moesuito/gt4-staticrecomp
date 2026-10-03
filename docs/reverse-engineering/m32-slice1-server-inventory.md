# M32, first slice — the server inventory and the arrival path

Date: 2026-10-03. Inputs: the pinned CORE and ISO, resumed from the
243.7M-service checkpoint (`build/ckpt-243m.bin`) for 12,000 services.
This slice opens M32 (a real IOP) with a read-only map: which SIF
servers the game has bound, where inbound traffic would land, and what
the parked dispatcher thread is. No model change beyond a diagnostic
listing; the next slice knows exactly what to trace.

## What the stop holds (Confirmed)

- The same deterministic stop as slices 48–50: `no-runnable-thread
  0x00001604`, 6,563 module calls, 368,172 interpreted steps, 11,723
  services. The run is bit-identical with the new diagnostic included.
- 17 threads (ids 1–14, 16, 17, 18). Thread 15 is gone: one created
  worker exited, consistent with the one-shot workers of slice 49.
- Thread 2 (the SIF/RPC waiter) sits in `WaitSema` on sema 11 with entry
  `0x005ae9a0`. Its code is a semaphore-gated byte-dispatch loop: it
  waits, bumps a counter (`[s1] & 0x1ff`), fetches a byte from a table
  at its argument block (`s1+8`/`s1+9`), and branches on values 0/1/2
  into calls including `0x005adbd0`. Hypothesis: this is a job or RPC
  dispatch loop; its argument block is not recoverable post-hoc, so the
  next slice traces it live.
- 24 SIF servers bound (new `--threads` listing):

  Custom: `0x046d046d`, `0x424b5550`, `0x45535550`, `0x4d504731`,
  `0x4d504732`, `0x5042474d`, `0x50434456`, `0x50555354`,
  `0x50636476`, `0x534d5550`, `0x53505550`, `0x53505554`,
  `0x53545250`, `0x54485550`, `0x564f4943`, `0x62737550`.
  System: `0x80000001`, `0x80000006`, `0x80000400`, `0x80000592`,
  `0x80001300`, `0x8000131c`, `0x8000131e`, `0x8000131f`.

- The model fully answers 3 of them (PCDV ops 2/3/4, PRTS ops 3/4/7,
  the fileio open); the rest get the generic version/zero replies.
- **No PADMAN (`0x80000100`/`0x80000101`) is bound.** The game has no
  pad consumer at this phase, so controller input cannot be what the
  parked machine waits for. M35 stays queued behind game progress, not
  behind model work.
- SIF0 `chcr` is still `0x184` (STR set, started and never completed) —
  the vestigial channel of slice 48, still innocent.

## Where inbound traffic would land (Confirmed + Hypothesis)

- The pump `0x005b0e30` reads its queue pointer from `0x886818`, which
  holds `0x20886740` — the mirror alias of `0x00886740`. That buffer's
  first byte is 0, so the count is zero and the pump is a no-op. The
  empty queue is now direct memory evidence, not just code reading.
- `0x886800` is the SIFCMD area (length `0x14`, `INIT_CMD` `0x80000002`
  at +8). The scratch at `0x886740` holds a stale reply-shaped packet
  (`RPC_END` `0x80000008`, record 5) — Hypothesis: a leftover reply
  template, not live traffic.
- `0x886840` holds `{handler, queue}` pairs (`0x005b0870`/`0x005b0850`
  with queue `0x00886818`, more entries at `0x8868a0` with queue
  `0x00888240`) — Hypothesis: the SIFCMD handler-registration table.
- On ASCII names: only PCDV and PRTS are pinned by prior decisions.
  The other sids read as plausible fourcc in one byte order or the
  other (e.g. `0x424b5550` as "BKUP", `0x4d504731` as "MPG1"), but the
  two pinned names use opposite orders, so per-server construction
  differs. Nothing new is pinned; the servers are tracked by number.

## What this rules in and out

- In: the first originating traffic enters through the SIF path — an
  inbound packet into the pump's queue (presently provably empty) or an
  IOP-initiated call into an EE-side server loop (thread 2 is shaped
  like one). M32 starts there.
- Out: pad input (no bound server, no consumer), device interrupts
  (slice 50: all handlers effect-free), another sync reply (the final
  leg issues no SIF calls at all).

## Verification

- `ee_kernel_tests` green (new check: the bound server appears in
  `sif_server_sids()`); the resumed probe ends on the identical
  boundary and stats with the listing enabled.
- Full gates green on the final tree: CTest 40/40, Python 73 (67 run,
  6 skip).
