# Slice 90: the post-gate state — the machine marches, then settles into a render loop

Date: 2026-10-09. Baseline: `main` = `180e1b3` (slice 89). No model code
changed here (documentation + exploration only). Task: march past the
200M-step budget and characterize the frontier the slice-89 fix exposed.

## The long march (1G steps)

Command: `gt4boot --steps 1000000000 --disc <iso> --threads` (fresh run).

- **18,499,142 services** handled, 41,166,056 module calls,
  958,833,944 interpreted steps; boundary `step-limit 0x00001600` — the
  step budget, not a park.
- VIF1/GIF frame loop at a constant rate: VIF1 starts 160 at 10k services,
  529 at 30k, 1,635 at 90k, 5,507 at 300k, 36,851 at 2M, **341,054 at the
  stop** (≈18.4 chains per 1,000 services, a 60fps-class frame loop);
  144.6 MB processed (sink == bytes; GIF consumes the VIF1 output).

## The steady state (frozen from ≤10k services)

- **Threads**: 16 threads; thread 1 sleeps (`ThreadWaitSleep`, id 0),
  thread 2 waits on sema 11, threads 9/13 sleep, workers 5–16 stay ready,
  threads 3/4 alternate as the running thread (entry 0x005786f0 — the RPC
  workers). The topology is identical at 10k, 30k, 90k, 300k, 1.2M, 2M and
  18.5M.
- **RPC frozen**: 25 pairs, 33 unknown calls, with identical call counts at
  10k and 18.5M (FILEIO open 22, PCDV reads 8, MCSERV 8+8, SIFMAN, DBCMAN,
  …) — **all RPC traffic happened before ~10k services**; nothing new
  afterwards.
- **Service mix** (trace tail at 1.2M): `GetThreadId` (0x2f) + semaphore
  ops (0x40/0x41/0x42/0x44 and iSignalSema −0x43) + `ChangeThreadPriority`
  (0x29) + SIF DMA (0x77/−0x78) + idle/VBlank (0x100) — a frame loop with
  worker churn and SIF handshakes, no file/CDVD traffic.

## Interpretation (labeled)

- **Confirmed**: the old event-starved park is gone; the machine runs to the
  step budget with frames flowing. The gate passed at ~3.3k services and
  the game's last RPC traffic finished before ~10k.
- **High confidence**: the game settles into a screen/frame loop it never
  leaves — the main thread sleeps and the workers render; the reference at
  the same point (post-gate, t≈14 s in the no-card session) proceeds:
  notice → movie (with real disc reads) → menu. The divergence window is
  **services ~3.5k–10k**: the last RPC and the freeze.
- **Hypothesis (next hunt)**: the freeze is a wait the model never
  completes — candidates: the card-service (MCSERV) completion path (its
  replies are the model's silent empty results), a movie-start path, or a
  phase event. The main thread's sleep origin and its intended waker are
  the first targets.

## Hygiene findings

- **Provenance commit is baked at configure time** (`CMakeLists.txt` runs
  `git rev-parse HEAD` there): the slice-89 binaries reported commit
  442e1c5 until a reconfigure. Refreshed by re-running the documented
  configure; the binaries now report `180e1b3`. Future slices should
  reconfigure when the recorded commit matters (or generate the header at
  build time — a candidate cleanup).
- The reference's post-gate state (dense t=14 s) is archived as
  `private/pcsx2/sstates/slice88-live-no-card/t0014-post-gate.p2s` for the
  next comparison.

## Next

1. Compare the model's ~10k state with the reference's t=14 s state **by
   meaning** (structures, not kernel ordinals): what does the reference
   have that the model lacks at the freeze?
2. Trace services ~3.5k–10k to identify the main thread's sleep origin and
   the intended waker.
3. Check the thread-id space for the same class of bug the semaphores had
   (open from slices 88/89).
