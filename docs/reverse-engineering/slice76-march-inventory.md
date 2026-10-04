# Slice 76: march the fresh prefix past 1M with inventory per stop (decision: none — observation only)

Date: 2026-10-04. Baseline: main at f5559b9 (P09 + IOP-image fix).
Task: long fresh legs from the entry past 1M (plus resumes ONLY from
new post-v3 checkpoints, never forensics), each with `--threads`
(census + RPC inventory), `--dump` of the four timer windows, and a
new checkpoint per stop. Per stop: services, boundary, threads/census,
new-vs-known RPC pairs, DMAs, and any naturally woken thread / new
guest request / expected packet. Target: the first naturally occurring
new RPC pair or the first natural wake — or how far the stationary
phase extends. PROHIBITED and kept: no fabricated traffic/events, no
behavior change, no causal claims without a discriminant, no old
checkpoints as evidence.

## Run identity

- run_id: slice76-20261004 (legs K-O fresh, P fresh running; resumes
  R1-R3 from new checkpoints; one process each).
- Model compatibility: time=3 interrupt=3 rpc=1 translation=2
  (unchanged from slice 75).
- Inputs (verified, `scripts/gt4disc.py verify`: PASS, SCUS-97328 /
  VER 2.00, ISO + all three file fingerprints match):
  ISO 5,314,478,080 bytes; `private/fingerprint-check/CORE.GT4`
  2,020,861 bytes; entry 0x00100008.
- Config: MSVC 19.44 x64 Debug, Ninja, CMake; service clock
  1 ms/service, idle 1 frame/interrupt, budgets 2M idle / 200M steps
  default. The 5M/4M legs pass `--steps 400000000` (harness ceiling
  only: projected ~283M translated calls + interpreted steps at 5M
  exceeds the 200M default; no semantic flag touched).
- Build: `ninja: no work to do` under VsDevCmd `-arch=x64`, clean
  `git status` (binary == HEAD sources). Provenance strings in the
  logs still read commit 837dc30 (stale build metadata, same quirk as
  slice 75), but the binary PROVABLY carries the f5559b9 fix: the
  3000+400 verify-resume from `build/slice75-ckpt-3000.bin` (post-v3,
  allowed) exits 0, `resume states identical (3400 services cumulative
  since boot, digest 0x9e98abf812eac06d)` — red without the fix per
  slice 75.
- No pre-v3 checkpoint used anywhere: every long leg is a fresh boot
  (`leg == cumulative` on all stats lines); `build/ckpt-*.bin`
  (2026-10-03) untouched, forensic per gate 0028. Resumes only from
  slice75-ckpt-3000.bin and the new slice76 checkpoints below.
- All long legs run `--quiet` (suppresses only the per-service trace
  line) with `--threads`, four timer dumps
  (`--dump 0x10000000/0x10000800/0x10001000/0x10001800 0x40`, length is
  hex = 64 bytes: COUNT/MODE/COMP/HOLD), and `--checkpoint-at N`
  into ignored `build/`. Logs ~27 KB each (vs 56 MB non-quiet 1M).

## Known set carried in (slice 75, 1M leg, confirmed at 1.2M)

- Boundary: syscall 0x00001604 service 0x100, fresh.
- RPC: 22 pairs / 32 unknown calls, all caller pcs 0x005AE064 on
  thread 1 (single SIFRPC wrapper). Pair list (sid fn calls):
  BKUP-0 x1, ESUP-0 x1, PCDV-4 x1, PUST-0 x1, PcdvSec-0 x1, THUP-0 x1,
  bsuP-0/1/2/4/5 x1 each, SIFMAN-0x17 x1, SIFMAN-0xFF x2, FILEIO-0 x22,
  FILEIO-0xFF x1, 0x80000400-1 x8, -0x15 x8, -0xFE x1, 0x80000592-0 x2,
  0x80001300-0x...301 x2, -0x...304 x1, -0x...363 x1. 23 binds, 23 SIF
  servers (MPG1/MPG2, PBGM, SPUP, SPUT, SMUP, VOIC, PRTS, 0x8000131c/
  0x8000131e/0x8000131f bound but never called).
- Threads: 13; waits 1: 2/63, 2: 2/11, 9/13: 1/0; deferred 1, pending 0.
- Handlers: INTC 11->0x005b8158, 2->0x004ab430, 5->0x004ab548,
  0->0x004ab668, 2->0x00551728; DMAC ch5->0x005b0e30,
  ch0/1/2->0x004ab6d8.
- DMA: VIF0 1 start [ref..end] 3 tags 2712 B (madr 0x006de5d0, sink ==
  bytes, hash 0xe44b9f8d); VIF1 15019 starts / 45066 tags / 6384544 B
  [cnt..end x15018 + ref..end x1]; GIF 15018 starts / 30036 tags / 0 B
  [next..end]; SIF0 chcr 0x184; SPR0/SPR1 zero.
- Timers: T0/T1/T3 all zero; T2 MODE 0x0382 (CUE|CMPE|OVFE, CLKS/256),
  COMP 0 (COUNT is stop-phase, wraps at 16 bits; see below).

## Long legs (all fresh, `--disc` pinned ISO, exit 0)

| Leg | Services | Boundary | Module calls | Bridge steps | Threads | RPC pairs / unknown | Pending | Checkpoint |
|---|---|---|---|---|---|---|---|---|
| K 1.2M | 1200000 | syscall 0x00001604 / 0x100 | 2816109 | 65279183 | 13 | 22 / 32, list identical | 0 | slice76-ckpt-1200000.bin (33606280 B) |
| L 1.5M | 1500000 | syscall 0x00001604 / 0x100 | 3519098 | 81556309 | 13 | 22 / 32, list identical | 0 | slice76-ckpt-1500000.bin (33606280 B) |
| M 2M | 2000000 | syscall 0x00001604 / 0x100 | 4690771 | 108685302 | 13 | 22 / 32, list identical | 0 | slice76-ckpt-2000000.bin (33606280 B) |
| N 3M | 3000000 | syscall 0x00001604 / 0x100 | 7034089 | 162943094 | 13 | 22 / 32, list identical | 0 | slice76-ckpt-3000000.bin (33606280 B) |
| O 5M | 5000000 | **syscall 0x005adcd4 / 0xffffffbd (iSignalSema)** | 11720710 | 271457654 | 13 | 22 / 32, list identical | **1** | slice76-ckpt-5000000.bin (**33606336 B, +56**) |
| P 4M | 4000000 | syscall 0x00001604 / 0x100 | 9377407 | 217200540 | 13 | 22 / 32, list identical | 0 | slice76-ckpt-4000000.bin (33606308 B, +28) |

Pair-list equality at every stop is by exact diff of the
`sid/fn/calls/class/candidate` prefix (Compare-Object: empty) against
the 1.2M list, which itself is line-identical to the slice-75 1M
known set above. Binds (23) and SIF servers (23) byte-identical on
every leg. Handlers frozen on every leg (same 5 INTC + 4 DMAC).

DMA progression (all legs): VIF0 frozen at exactly 1 start
(2712 B, hash 0xe44b9f8d); VIF1 18027 / 22538 / 30059 / 45097 / 75175
starts (deltas 3008, 4511, 7521, 15038, 30078: ~1 frame per 66.5
services, linear through 5M); GIF = VIF1 - 1 starts, 0 bytes, hash
0x811c9dc5 frozen; SIF0 chcr 0x184; SPR channels zero. VIF1 TADR/MADR
rotate between two chain heads (0x006dece0/0x0077ece0 families);
sink == bytes on every leg.

Timer windows per stop (T0/T1/T3 all-zero on every leg — untouched):

| Leg | T2 COUNT | T2 MODE | T2 COMP |
|---|---|---|---|
| K 1.2M | 0xB240 | 0x0382 | 0x0000 |
| L 1.5M | 0x6A40 | 0x0382 | 0x0000 |
| M 2M | 0xF240 | 0x0382 | 0x0000 |
| N 3M | 0x0240 | 0x0382 | 0x0000 |
| O 5M | **0x2240** | **0x0782 (EQUF set)** | **0x2240** |

COUNT deltas mod 0x10000 are non-linear across legs (0xB800,
0x8800, 0x1000, 0x2000 per 300k/500k/1M/2M) because the TIM2 handler
acks/reprograms every frame — COUNT is stop-phase, recorded raw, no
claim built on it. MODE/COMP are the programmed values: frozen
(0x0382/0) through 3M, reprogrammed at 5M (handler wrote COMP 0x2240;
EQUF = compare reached, unacked at the stop).

## The 5M stop: first mid-cycle sample (observation, then proof)

Leg O differs on four correlated fields; everything else frozen:

1. Boundary `syscall 0x005adcd4 service 0xffffffbd` (-0x43 =
   iSignalSema, `kernel.cpp:186`). Disassembly of the neighborhood
   (`gt4disasm`, read-only) shows a stub table: 0x005adcd0 =
   `addiu v1,-0x43; syscall; jr ra`, so 0x005adcd4 is the iSignalSema
   stub's syscall — the run stopped with handler-context signaling in
   flight. (Same lookup names the other stop pcs: 0x005adce8 = return
   slot of the WaitSema stub at 0x005adce0 — where ALL sema-waiters
   park; 0x005adcc8 = return of the SignalSema stub; 0x005adb38 =
   return of the 0x29/ChangeThreadPriority stub; 0x005adbc8 = return
   of the 0x32/SleepThread stub, where sleep-waiters 9/13 park.)
2. Thread 3: `status 0x4, wait 2/768103` (ThreadWaitSema = 2,
   `ee_kernel.hpp:42-46`), pc 0x005adce8 — a pool worker (entry
   0x005786f0, live 0x1/0x2 through every previous stop) blocked in
   WaitSema. `wait_sema` only blocks on an EXISTING semaphore
   (`kernel.cpp:985-1002`: missing id = error, no block), so sema
   768103 exists; ids go 3,7,11,... (`next_semaphore_id_ += 4`,
   `kernel.cpp:929-930`), so ~192k CreateSemas precede it — the
   Create/Delete churn (76/78 in the +2k trace below) corroborates.
   768103 mod 4 = 3, consistent with the handle-bit scheme.
3. `pending interrupts: 1` (was 0 on every previous leg and every
   slice-75 leg); deferred calls still 1.
4. T2 MODE 0x0782 / COMP 0x2240 (above); checkpoint 56 bytes larger.

No new RPC pair (exact list identical, 22/32), no new bind/server/
handler, no VIF0 change: the guest asked the model for nothing new —
the event is all inside the EE (timer + scheduler + a worker wait).

## The first natural wake: thread 3 blocked@5M -> live@5.002M (CONFIRMED)

Resume R1 from the NEW 5M checkpoint (post-v3, allowed):
`--resume build/slice76-ckpt-5000000.bin --services 2000 --quiet
--threads --dump 0x10001000 0x40` (leg-relative; cumulative 5002000),
exit 0:

- Boundary back to idle `syscall 0x00001604 / 0x100`; pending 0;
  deferred 1.
- **Thread 3 live**: `status 0x1, wait 0/0, prio 0, pc 0x005adce8`
  (thread 4 back at the familiar `0x2 prio 1 pc 0x005adb38`
  configuration seen at 1.2M/3M). All historic waits intact
  (1: 2/63, 2: 2/11, 9/13: 1/0).
- T2 back to MODE 0x0382 / COMP 0 (COUNT 0xB640): the EQUF/COMP state
  was acked/reprogrammed inside the window.
- VIF1 +30 starts in 2000 services (~1/66.7): frame rhythm unchanged.
- `rpc inventory: 0 pairs, 0 unknown calls` — EXPECTED, not a finding:
  telemetry is per-leg and excluded from snapshots (slice 73), so a
  resumed leg reports only its own traffic: the guest made ZERO SIF
  RPC calls in (5M, 5.002M]. Labeled leg-relative in the log.

Discriminant: the (blocked, 2/768103) -> (live, 0/0) pair across two
adjacent deterministic states with no injected traffic/event is the
first naturally occurring wake transition observed (prior slices:
zero unblocks, zero signal/wakeup calls reaching a waiter — slice
48/49). The exact signaling service is NOT identified at argument
level (existing flags log service+pc only, and wait-state snapshots
do not name the signaler): wake = Confirmed; signaler = inference
(see trace). No causal claim beyond the state pair.

## Service mix around the wake (non-quiet resume 5M+2000, R2)

2000 service lines, exact census (sums to 2000):

| Service | PC | Count | Meaning |
|---|---|---|---|
| 0x2F | 0x005adb94 | 935 | GetThreadId (idle self-poll, NOT a wakeup path) |
| 0x100 | 0x00001604 | 394 | idle private service |
| 0x44 | 0x005adce4 | 166 | WaitSema (stub at 0x005adce0) |
| 0x29 | 0x005adb34 | 153 | ChangeThreadPriority (explains pool prio shuffling) |
| 0x42 | 0x005adcc4 | 91 | SignalSema |
| 0x41 | 0x005adcb4 | 78 | DeleteSema |
| 0xffffffbd | 0x005adcd4 | 77 | iSignalSema (handler context) |
| 0x40 | 0x005adca4 | 76 | CreateSema |
| 0x64 | 0x005adf24 | 30 | FlushCache |

Create/Delete ~balanced (76/78): semaphores churn continuously,
which is why a fresh id like 768103 exists. Wake-candidate paths in
the window reduce to the sema signals (77 i + 91 thread-context;
WakeupThread 0x33 and Sleep 0x32 do not appear; GetThreadId only
reads). High-confidence (not confirmed): thread 3 was released by
one of these 168 signals — the delay machinery's iSignalSema at the
dispatcher's stub is the structural suspect (the 5M boundary itself
landed on it), but the trace carries no argument registers, so the
sema-768103 signaler is unnamed. No new instrumentation built to
chase it (out of scope; would need arg capture).

## 3M neighborhood control (resume R3: 3M+2000, quiet+threads+dump)

Idle stop, thread 3 live (0x1/0/0), thread 4 at 0x2/prio1/0x005adb38,
T2 0x9640/0x0382/0, pending 0, rpc leg-relative 0. The 3M state is
the familiar idle configuration 2000 services later — the mid-cycle
regime had not been sampled there.

## Onset bracket: 4M idle (CLOSED — then the onset hypothesis refuted)

Leg P (4M, fresh, exit 0): idle stop `syscall 0x00001604 / 0x100`,
thread 3 LIVE (`0x1/0/0`, pc 0x005adcc8 — the SignalSema return slot
this time), thread 4 at `0x2 prio 0 pc 0x005adce8`, T2 COUNT 0x1240 /
MODE 0x0382 / COMP 0, T0/T1/T3 zero, pending 0, deferred 1, 22/32
pairs (exact list identical), 23 binds, 23 servers, handlers frozen,
VIF0 1 start, VIF1 60137 starts / 180420 tags / 25514576 B
(linear: +15040 over 3M-4M), GIF 60136 starts / 0 B. Checkpoint
33606308 B (+28 vs the 33,606,280 standard; 5M was +56 — size tracks
live-state content such as the grown sema table, recorded raw).

So the six fresh stops read: idle x5 (1.2M, 1.5M, 2M, 3M, 4M) +
mid-cycle x1 (5M). The first hypothesis — worker-block regime onsets
in (4M, 5M] — is REFUTED by the trace equivalence below. The 4M leg
stands as the 6th idle sample, not a bracket wall.

## The regime is one standing cycle from 3k to 5M (trace equivalence)

Non-quiet +2000-service windows from three post-v3 checkpoints
(slice75-ckpt-3000.bin is GT4CPT2 post-v3, allowed; slice76 3M/5M
checkpoints new) — exact service census per window (each sums to
2000), same 9 services at the same stub pcs:

| Service @ pc | 3k+2k (3k-5k) | 3M+2k | 5M+2k | Meaning |
|---|---|---|---|---|
| 0x2F @ 0x005adb94 | 936 | 934 | 935 | GetThreadId (idle self-poll) |
| 0x100 @ 0x00001604 | 393 | 394 | 394 | idle private service |
| 0x44 @ 0x005adce4 | 166 | 167 | 166 | WaitSema |
| 0x29 @ 0x005adb34 | 155 | 154 | 153 | ChangeThreadPriority |
| 0x42 @ 0x005adcc4 | 87 | 90 | 91 | SignalSema |
| 0x41 @ 0x005adcb4 | 78 | 77 | 78 | DeleteSema |
| 0x40 @ 0x005adca4 | 78 | 77 | 76 | CreateSema |
| 0xffffffbd @ 0x005adcd4 | 78 | 77 | 77 | iSignalSema |
| 0x64 @ 0x005adf24 | 29 | 30 | 30 | FlushCache |

Counts agree within +-2 across eras 3,000x apart. The delay/sema
churn (Wait ~166, signals ~168, Create/Delete ~balanced 77/77) was
ALREADY the regime at 3k services — the running state the boot had
just reached (slice M30-8). Consequences, stated carefully:

- REFINED (replaces the (4M,5M] onset hypothesis): no phase onset
  anywhere in [3k, 5M]. The worker-block/wake cycle, the sema churn,
  the TIM2 ack/reprogram rhythm and the idle poll are ONE standing
  limit cycle spanning at least 3,000 -> 5,000,000 services (three
  orders of magnitude). Its true onset is at or before 3k — outside
  this slice's window (the boot reaches running state by 3k; earlier
  eras are init, different mix by construction).
- The 5M mid-cycle stop is stop-phase sampling of that cycle (first
  of 7 fresh stops to land in-cycle: 6 idle misses + 1 hit is
  consistent with brief in-cycle bursts — the +2k windows show the
  burst content directly). NOT a new phase.
- Stationary-phase extension: no new RPC pair, no new bind/server/
  handler, no new guest request of any kind through 5M (5x the
  slice-75 range); the only firsts are the first IN-CYCLE stop and,
  through it, the first OBSERVED natural wake (thread 3).
- A further +66k recurrence probe was considered and SKIPPED as
  low-value: the trace equivalence already proves the churn
  structurally at three eras; one more stop sample would add a 7th
  idle/mid-cycle coin flip, not mechanism.

## Gates

- Full CTest + Python: 53/53 Passed (48.5 s, VsDevCmd x64,
  `ninja: no work to do`) + Python 73 run OK skipped=6 (61.7 s,
  exit 0; um `exit=1` intermediario foi artefato do pipe
  `2>&1 | Select-Object` do PowerShell, nao falha — rerun com log em
  arquivo: exit 0). `gt4disc.py verify` PASS.
- Journal append in docs/journal/2026-10-04.md (done).
- No commit, no push, no branches. Scratch logs (6 quiet legs ~27 KB
  each + 3 traces) + 6 new checkpoints (33.6 MB each, ~200 MB) under
  ignored `build/`; `git status` holds only the two docs; no payload
  in git.
