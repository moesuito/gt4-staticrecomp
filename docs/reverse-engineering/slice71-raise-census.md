# Slice 71: raise census — sizing the delivery-granularity contract (2026-10-04)

Status: observation complete, no behavior change. All instrumentation was
temporary, bounded and reverted (`git checkout` of the four touched
sources; logs and scripts lived under ignored `build/` and were deleted
after extraction). Only this document, the decision-0034 draft and the
journal entry survive.

## Claim

Over a 90,000-service disc boot (driver engine), **21.8% of interrupt
raises happen inside translated code and 78.2% at loop-top** — and the
inside-module class is exactly one guest function programming DMA:

- **2,668 in-module raises, all DMAC, all inside `function_004aba50`**
  (VIF1/GIF chain programming, ch1 x1334 + ch2 x1333 + ch0 x1), all
  delivered at the module-exit continuation on the driver
  (0x004a1274 / 0x004a1288) versus the next instruction (0x004abae4)
  on the interpreter. Every in-module raise diverges in delivery pc.
- **9,559 loop-top raises** (SIF DMAC-5 completions from service
  handlers x89, VBlank INTC-2 from time advances x5327, TIM2 INTC-11
  compare edges x4143) deliver at **identical pcs, handlers, sps and
  threads on both engines**. Zero divergence in this class.
- **Zero bridge-step raises** in 90k services: the interpreter bridge
  never programs DMA; it needs no poll points.
- The divergent class's handler (`0x004ab6d8`) issues **zero guest
  syscalls** (only the stub 0x100 returns): aligning the delivery pc
  aligns the class completely. The syscall-issuing handlers (timer,
  SIF, VBlank) are all loop-top-aligned already.
- Divergent deliveries are **transient**: 10 of them happened before
  the green 1605 stop and left no trace (later execution reuses the
  stack); only a stop landing inside the contaminated window (1606)
  observes them.

Confidence: Confirmed (dual-engine TSV census, line-by-line pairing;
neutrality proved by byte-identical leg stats to slice 70).

## Method (transient, bounded, reverted)

Temporary hooks, all reverted after extraction (four files, restored
with `git checkout`: `src/ee/kernel.cpp`, `src/ee/driver.cpp`,
`tools/gt4boot/main.cpp`, `include/gt4recomp/ee_kernel.hpp`):

- `queue_interrupt` / `queue_dmac_completion` log one TSV line per
  raise: engine (D/R), kind (I/D), number, pc context (exact step or
  service pc vs enclosing module entry), inside-module / inside-step
  flags, new-vs-coalesced, queue depth. Never touches state.
- `inject_interrupt` logs the delivery: interrupted pc, handler, sp,
  thread, deferred depth. `deferred_return` logs chain steps and the
  final return (resume pc, sp, thread, Jumped vs NoRunnableThread).
- `Driver::handle_syscall` and `run_reference` log every handled
  service (number, pc, pre-increment count), so syscalls issued
  *inside* a handler are visible as service lines between its
  inject and hreturn lines.
- Gated on `GT4_CENSUS71=<log path>`; unset means a null check per
  hook and unchanged behavior. Unbuffered writes, 2M-line cap
  (never hit: largest log 8.3 MB).

Legs (inherited tree e20fa17, honest rebuild, MSVC 19.44 x64):

- **A (green neutrality)**: `--services 1605 --compare-interpreter`
  with disc + census: exit 0, state identical, stats identical to
  slice 70's clean leg (8197 module calls, 258622 bridge steps,
  7520925 interpreter instructions). Instrumentation changes nothing.
- **B (boundary)**: `--services 1606 --compare-interpreter` with
  disc + census: exit 1, `state differs at register 29`, 8200 calls /
  258680 steps — the exact slice-70 signature.
- **C (volume)**: `--services 90000` driver-only with disc + census:
  exit 0, syscall 0x1604/0x100, 215013 module calls, 5034301 bridge
  steps, 90000 services. 8.3 MB log, no truncation.

## Census tables (leg C, driver engine, 90k services)

Raises: 12,227 total.

| provenance | kind | number | count | new / coalesced |
|---|---|---|---|---|
| in-module (entry `004aba50`) | Dmac | ch 0 (VIF0) | 1 | new |
| in-module (entry `004aba50`) | Dmac | ch 1 (VIF1) | 1334 | new |
| in-module (entry `004aba50`) | Dmac | ch 2 (GIF) | 1333 | new |
| loop-top (service/idle) | Dmac | ch 5 (SIF) | 89 | new |
| loop-top (service/idle) | INTC | 2 (VBlank) | 5327 | 5326 / 1 |
| loop-top (service/idle) | INTC | 11 (TIM2) | 4143 | 4141 / 2 |
| bridge-step | — | — | **0** | — |

Max queue depth at raise: 3. Coalescing: 3 of 12,227. Injections:
12,224 (3 still pending at the stop: VBlank x1, TIM2 x2); returns
12,223 with 1 handler in flight. Chain steps: 5,224, all VBlank
cause-2 continuing to a second handler `0x00551728`.

Deliveries by handler (handlers keep their own arguments; chains run
in registration order):

| cause | handler(s) | invocations | genuine in-handler guest syscalls |
|---|---|---|---|
| Dmac 0/1/2 | `0x004ab6d8` | 2668 | **none** (stub 0x100 returns only) |
| INTC 2 (VBlank) | `0x004ab430` + `0x00551728` (5224 of 5326 chained) | 5326 | 2 spans: `iGetThreadId` + `iWakeupThread` (-0x2F/-0x34); 1 thread switch in 12,223 returns |
| INTC 11 (TIM2) | `0x005b8158` | 4141 | `iSignalSema` (-0x43) in 3375 (82%) |
| Dmac 5 (SIF) | `0x005b0e30` | 89 | `iSifSetDChain` (-0x78) 89/89 + `iSignalSema` 85/89 |

Handler-frame hygiene: sp at return equals sp at injection in all
12,223 paired returns; every outcome is `Jumped` (restored or
dispatched, never stranded). Service 0x100 in the tables is the
model's private patch/interrupt return stub — the return mechanism,
not guest logic — and is excluded from the "genuine syscalls" column.

## Dual-engine pairing (legs A and B)

Raise sequences are identical on both engines (leg A: D5 29, I2 23,
I11 17, D1 5, D2 4, D0 1 on each side). Reference-side in-module
raises carry the exact store pc: always `0x004abae0` (the STR `sw`
of slice 70), 10 times in leg A, 11 in leg B (the 6th VIF1 start).

Loop-top deliveries match exactly on every field (interrupted pc,
handler, sp, thread) — including injections over idle (thread 0)
and the VBlank two-handler chain. No loop-top divergence exists.

In-module deliveries differ every time:

| raise | driver delivery | reference delivery |
|---|---|---|
| D0/D1/D2 inside `004aba50` | module-exit continuation (`0x004a13a4`, `0x004a1274`, `0x004a1288` — two call sites), sp at frame top | next instruction `0x004abae4`, sp inside the frame (0x20 lower) |
| leg B 6th D1 | `0x004a1274`, sp 0x6de6d0 | `0x004abae4`, sp 0x6de6b0 |

Raw driver pattern around each pair (no guest service between raise
and inject — the delay is the remainder of the translated function):

```
raise  D N=1 PC=004aba50 M=1
inject D N=1 IPC=004a1274 H=004ab6d8 SP=006de6d0 T=3
service svc=100 PC=00001604        (stub return of handler 1)
hreturn N=1 RPC=004a1274 SP=006de6d0 T=3 O=J
raise  D N=2 PC=004aba50 M=1
inject D N=2 IPC=004a1288 H=004ab6d8 SP=006de6d0 T=3
```

Leg A ends green *despite* 10 divergent deliveries: the handler
frames are symmetric and later guest execution reuses the same stack
slots, erasing the contamination before the service-1605 boundary.
Leg B's stop lands one service later, inside the fresh window — that
is the whole of incident 1606. So divergent delivery is the steady
state (100% of in-module raises), and observability is the accident
of where the stop lands.

## Dimension of the contract work (input to draft decision 0034)

1. **One emitter, one function, three channels.** Every mid-module
   raise in 90k services comes from guest `function_004aba50`
   programming VIF1/GIF (+1 VIF0): ~2 raises per execution, ~1335
   executions in 215,013 module calls (0.6%). No other guest code
   programs a completing DMA channel mid-module today.
2. **One pure handler.** The divergent class lands on `0x004ab6d8`,
   which issues zero guest syscalls and always restores sp. Aligning
   *where* it runs aligns *everything* it does. No in-handler
   divergence needs chasing for this class.
3. **The impure handlers are already aligned.** Timer (iSignalSema),
   SIF (iSifSetDChain + iSignalSema) and VBlank (rare
   iGetThreadId/iWakeupThread, 1 switch in 90k) all raise at
   loop-top and deliver identically on both engines. A contract that
   covers only synchronous DMA completions covers all observed
   divergence.
4. **No backlog, no bridge work.** Queue depth never exceeds 3;
   coalescing is 3 in 12,227; the bridge emits zero raises. Poll
   points are needed only around guest-programmed DMA starts, not in
   the bridge and not at idle.
5. **Self-healing is luck, not a mechanism.** The green 1605 proves
   contamination is usually overwritten, but nothing guarantees the
   next phase's stops land as forgivingly. The contract must align
   delivery, not rely on reuse.

## What was NOT done (scope guard)

No behavior change of any kind (no RPC/clock/mask/JR/DMA edits, no
fabricated traffic, no generic success); no contract implementation —
only the census and the draft. P07 telemetry and P09 comparisons stay
gated behind the green differential. The `WILL_FAIL` markers stay.
