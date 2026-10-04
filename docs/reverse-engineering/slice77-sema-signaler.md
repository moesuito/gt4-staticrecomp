# Slice 77: the sema-768103 signaler — a delay expiry on TIM2 (decision: none — observation only)

Date: 2026-10-04. Baseline: main at d4e0182 (slice 76).
Task: name the exact writer that wakes thread 3 off semaphore 768103 in the
5M window — pc, signaling thread, service/handler of origin, and the causal
chain that fired it (timeout? reply? device event? another worker?). Method:
temporary bounded pre-op logging in `src/ee/kernel.cpp` (every
signal/wait/create/delete with pc, ra, v1, args, current thread, handler
state and the top deferred call), gated by `GT4_SEMA_TRACE`, capped at
500,000 lines, fully reverted after extraction; one resume leg from the NEW
post-v3 checkpoint `build/slice76-ckpt-5000000.bin` (+2000 services, allowed
by gate 0028). PROHIBITED and kept: no fabricated traffic/events, no behavior
change (stats triple identical on three legs), no causal claim without a
discriminant, no pre-v3 checkpoint anywhere.

## Run identity

- run_id: slice77-20261004 (one instrumented resume leg + one clean recheck;
  no fresh long leg needed — the window is fixed by slice 76).
- Model compatibility: time=3 interrupt=3 rpc=1 translation=2 (unchanged).
- Inputs (verified, `scripts/gt4disc.py verify`: PASS, SCUS-97328 /
  VER 2.00, ISO + all three file fingerprints match):
  ISO 5,314,478,080 bytes; `private/fingerprint-check/CORE.GT4`
  2,020,861 bytes; entry 0x00100008.
- Config: MSVC 19.44 x64 Debug, Ninja, CMake; service clock
  1 ms/service, idle 1 frame/interrupt, budget 2M idle / 200M steps.
- Neutrality proof (the instrumentation changes nothing): the instrumented
  resume exits 0 with boundary `syscall 0x00001604 / 0x100`, stats
  **4691 module calls / 108775 interpreted steps / 2000 services** and thread
  3 live — the exact triple of the uninstrumented slice-76 R1 leg. A
  post-revert clean recheck repeats the same triple, exit 0. The wake is
  deterministic across three legs (R1, instrumented, clean recheck).

## The answer (Confirmed)

The waker of thread 3 off semaphore 768103 is the game's own **delay
dispatcher firing a delay expiry inside the TIM2 interrupt handler**:

- service: **iSignalSema** (v1 0xffffffbd) at stub pc **0x005adcd4**;
- guest caller: **ra 0x005aef68**, the return slot of the wrapper at
  **0x005AEF58** (`jal 0x005adcd0`, delay slot `daddu a0,a3,zero` — takes the
  sema id in a3; `gt4disasm`, read-only);
- handler context: **active, one deferred call, INTC cause 11 (TIM2),
  handler 0x005b8158** (the registered TIM2 handler of the slice-76 known
  set), interrupting **thread 4** at 0x005adce8;
- sema state at the signal: **count 0, waiters 1, first waiter thread 3** —
  wakes exactly thread 3 (status 0x4 wait 2/768103 becomes ready);
- leftover arg registers a1 = a2 = **0x24000 = 147456 = exactly
  `service_time_slice`** (one service = 1 ms of BUSCLK, decision 0016).
  Across all 77 iSignals, a1/a2 are only 0x24000/0x48000/0x6c000
  (1x/2x/3x the service slice): the delay periods in BUSCLK ticks.
- It is the **first semaphore event of the window** (trace line 1 of 488):
  the resume's first sema service completes the cycle sampled mid-flight
  at the 5M stop.

Timer and base: **timer T2** (MODE 0x0382 = CUE|CMPE|OVFE, CLKS/256,
COMP reprogrammed by the handler every frame — slice 15/76 evidence),
**base = the service clock** (1 ms = 147,456 BUSCLK ticks per handled
service; T2 advances 576.05 ticks/service measured in slice 15; the delay
library schedules in BUSCLK ticks of elapsed time — slice 14/15 evidence).
Cause of firing: **a delay node whose period expired** (timeout/delay
vencido), not a reply, not a device event, not another worker.

## The full causal chain (Confirmed by trace + disassembly)

Pool worker (thread 3, entry 0x005786f0) calls the delay helper at
0x005AEDxx, which per `gt4disasm` (read-only):

1. `jal 0x005adca0` = CreateSema (return 0x005aed68 — all 76 in-window
   creates carry ra 0x005aed68, from pool threads 3/4);
2. `jal 0x005B8D88` then `jal 0x005B8F38` with **a1 = 0x005AEF58**
   (`lui a1,0x5b; addiu a1,a1,-0x10a8`): registers the iSignal wrapper as
   the delay node's completion callback, with the fresh sema id;
3. `jal 0x005adce0` = WaitSema (return 0x005aedc0 — all 76 blocking waits
   carry ra 0x005aedc0; count 0 so the worker blocks);
4. ... the TIM2 handler (cause 11, 0x005b8158) runs the timer/delay
   dispatcher, the node expires, the dispatcher calls 0x005AEF58 with the
   sema id in a3, which issues iSignalSema (ra 0x005aef68);
5. the helper resumes and `jal 0x005adcb0` = DeleteSema (return 0x005aedc8 —
   all 78 deletes carry ra 0x005aedc8): the one-shot idiom of slice M32-4
   (wait-then-delete over delay-library work).

Representative in-window lifecycle, sema 768107 (thread 3), four adjacent
trace events: create (pc 0x005adca4, cur 3) -> wait blocks (pc 0x005adce4,
cur 3) -> iSignal wakes waiter 3 (pc 0x005adcd4, cur 4 = the interrupted
thread, handler TIM2) -> delete (pc 0x005adcb4, cur 3). Sema 768103 is the
same cycle with create+wait before the 5M stop (outside the window) and
signal+delete inside it (trace lines 1 and 3, back to back modulo thread
4's own cleanup of its pre-window sema 768099 on line 2).

## Census of the window (488 sema events; the R2 service mixlays underneath)

- 168 signals = **77 iSignal + 91 Signal**, exactly the R2 counts.
  - All 77 iSignals: handler-active, cause 0x0b, handler 0x005b8158, ra
    0x005aef68 — the delay path, no exceptions. Woken waiter: thread 4
    x46, thread 3 x31 (the 31st is 768103 itself, created pre-window;
    the other 30 match the 30 in-window thread-3 cycles). Interrupted
    thread: 3 x46 / 4 x31 — whichever pool worker ran when the tick fired.
  - All 91 thread-context signals: cur 3, handler-inactive, waiter 0
    (count++ path, nobody released), count 0, targets only the long-lived
    shared semas 119/127/131 from three ra sites
    (0x00101778 x31, 0x004a1290 x30, 0x00107990 x30). They refill the fast
    path: thread 3's 90 waits on 119/127/131 all succeed immediately.
- 166 waits (R2: 166): 76 blocking (all ra 0x005aedc0, the delay helper;
  ids exactly the 76 created) + 90 immediate on 119/127/131.
- 76 creates (R2: 76): ids 768107..768407 step 4 (cur 4 x46 / 3 x30),
  all ra 0x005aed68.
- 78 deletes (R2: 78): the 76 in-window ids plus the two pre-window delay
  semas 768099 (thread 4) and 768103 (thread 3), all ra 0x005aedc8.
  (The helper's error-path delete site 0x005aedac never fires in-window.)

## What this means for P10 (the main-thread producer)

The signaler is **not the producer of the main thread — it is one more
cycle of the standing limit cycle**, now with its writer named:

- Thread 1 (main) still waits on sema 63 and thread 2 on sema 11 at every
  stop including this one; no signal in the window names either id, and no
  signal releases any waiter outside threads 3/4.
- The 77 delay expiries wake only pool workers (3/4) off their own private
  one-shot semas; the 91 thread signals release nobody. Zero unblocks
  outside the pool pair — consistent with slices 48/49 (no wakeup path to
  the main chain) and the slice-76 trace equivalence (same mix at 3k/3M/5M).
- P10 still has no causal chain from any modeled event to the main
  thread's readiness. The missing-events ranking of slice 49 stands
  (async IOP drought first, then input, then GS-side). The next hunt keeps
  its target: who can release **sema 63** (and 11), not who woke 3/4.

## Gates and hygiene

- Full CTest on the reverted tree: **53/53 Passed** (51.2 s, VsDevCmd x64,
  MSVC 19.44); Python **73 run OK skipped=6** (62.4 s, exit 0; same 2
  known socket ResourceWarnings); `gt4disc.py verify` PASS.
- Instrumentation reverted via `git checkout -- src/ee/kernel.cpp`
  (the only touched source); `git status`/`git diff` clean except the two
  docs below. Hunt artifacts deleted after extraction
  (`build/slice77-sema.log`, the instrumented resume log); the clean
  recheck log stays as an ordinary resume leg. No new checkpoint written
  (resumed slice 76's). No commit, no push, no branches.
- Journal append in docs/journal/2026-10-04.md (done).
