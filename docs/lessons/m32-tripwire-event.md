# M32 lesson — the specified event: a pump packet, a tripwire, and an honest negative

Prepared 2026-10-04. BUILD/VERIFY: slices 29–31; see the
[M32 slice-29 evidence](../reverse-engineering/m32-slice29-pump-path.md),
[slice-30 evidence](../reverse-engineering/m32-slice30-first-event-live.md),
[slice-31 evidence](../reverse-engineering/m32-slice31-tripwire-watch.md),
and [decision 0026](../decisions/0026-first-originating-event.md).
EXPLAIN: this is the worked explanation; tutoring review pending.

## Objective and motivation

The march proved delay firings sterile; the recon proved the
pipe dry and the job system unbuilt; the watch proved total
silence. The project now faces the synthesis question it
deferred through all of that: if nothing arrives on its own,
what is the first thing the model may *send* — without
fabricating a game decision? This lesson teaches the answer's
shape: rank every candidate against who it would wake, adopt
the only hardware-faithful traffic with a complete consumer
path, bound its effect honestly (mechanism, not unblock), and
pin the negative into the test suite so no later slice can
mistake path-liveness for progress. It ends with the async
path proven live and the census holding bit-identical.

The motivating constraint is negative: synthesizing a wake
fabricates which thread the game would wake — a game-side
decision the model cannot know. Anything the model originates
must therefore be *traffic*, never a verdict.

## Step 1 — map the pump, rank by who wakes whom

The SIF pump (`0x005b0e30`, DMAC channel 5's registered
handler) reads its queue pointer from `[0x00886818]`
(`0x20886740`, the mirror alias), drains a nonzero count to
its stack frame, re-kicks the chain through the `-0x78`
SifSetDChain stub, then dispatches table-driven: index =
drained word 2 `& 0x7fffffff`, entry = table + index × 12,
`jalr` with the stack copy, the entry's queue arg, and a
swapped gp. The table at `[0x00886840]` holds two reachable
entries (`0x005b0870`, `0x005b0850` — indexed software-register
accessors over the array at `0x008869C0`); indices 2–31 are
zero-filled and skipped. Index 1 is `SET_SREG & mask`, so a
packet reading `{count, 0, 1, 0, register, value}` stores
`value` at `SIFREG[register]` — the slice-14 SET_SREG path the
game itself uses.

Ranked against the waiter census, every alternative loses for
a stated reason: delay maturation needs no synthesis and is
proven sterile; a ring-job post would pick which thread wakes
(fabrication); VBlank is saturated and effect-free by game
gates; no parked thread waits inside a disc-read path (no live
request to complete); pad and GS-side have no consumer. The
pump packet is adopted *as mechanism*: it advances register
state and proves the async path live, while waking — provably,
by handler semantics plus the absence of any polling waiter —
nobody.

## Step 2 — specify bytes, cause, trigger; implement; assert the negative

Decision 0026 in full: count `0x18` at the queue base plus
words `{0, 1, 0, 1, 1}` (register 1, value 1 — reproducing the
live console state rather than inventing one); DMAC channel-5
completion through the existing queue machinery, handler
running in interrupt context under the standing no-nesting
rules; one-shot trigger at the first idle tick where the ch-5
handler is registered, the queue is empty, *and* the game's
dispatch entry is populated (that last gate added in
implementation, after a fresh leg showed a drained-but-
unhandled packet: never spend the one shot into an
unpopulated table); determinism as a pure function of the
guest service sequence, so both engines inject at the same
boundary. Snapshot carries the flag, with a tolerant read so
pre-decision checkpoints load as unsent.

The four-item bar, all met: gates green (42/42, including the
90k-service compare-interpreter differential with the trigger
live); stop-time dumps show the register write and the
drained queue with a clean handler return (resume-from-park
leg: `SIFREG[1] = 1`); **the census before and after is
identical thread-for-thread** — the load-bearing negative,
pinned by a CTest asserting the drained words, the idle
boundary, and thread 2 still hungry; kernel-level tests for
bytes, one-shot, all three guards, and snapshot round-trip
including a legacy blob.

## Step 3 — prove the one-shot across replay, then map no consumer

The flag lives outside guest RAM, so the replay proof reads
the checkpoint files directly (`GT4CPT1` framing → kernel
section → trailing word): 0 at a fresh-400 save, 1 at a
fresh-20000 save. `--verify-resume` across the firing
boundary reproduces bit-for-bit (22,000 services, digest
`0x86eb07b0a8a403c7`); a chained re-save keeps flag 1; the
unit proof (restored kernel never re-fires, save-load-save
identical) covers what behavior cannot show — a direct
re-fire would write identical bytes and be unobservable, which
is exactly why the proof reads the flag instead of watching
for effects.

The park re-census is identical to baseline (17 threads, same
waits), and the consumer hunt closes empty against the proven
handler semantics: the pump cannot post ring jobs, cannot
signal semaphores, cannot wake sleepers; no thread waits in an
RPC path; the table's upper indices stay empty. Verdict, with
three stated promoters for a future slice-32: the tripwire
trips, the ring producer is identified, or an RPC waiter
appears. Until one does, specifying traffic would repeat the
fabrication this whole arc refused.

## What this arc does not claim (kept explicit throughout)

- The register write lands where the phase is stable (park
  leg); on fresh boot the queue drains but the register reads
  0 through 90k — game-phase churn at dispatch, not a model
  defect, which is why CI pins the drain and the evidence leg
  pins the write.
- The fossil/poster line (ring producer hunt, era falsified,
  verified-quiet long legs) is a *later* arc that reuses this
  one's pump facts; it is referenced here only as the forward
  pointer the slices allow, not narrated.
- "Wakes nobody" is a property of this packet and this park,
  not a law: the tripwire exists precisely because a future
  event with a real consumer must fail loudly here first.

## Connection to our implementation

| Piece | File | Job |
| --- | --- | --- |
| One-shot packet + trigger | `src/ee/kernel.cpp` (`maybe_send_originating_packet`) | gated bytes + ch-5 completion, deterministic |
| Flag snapshot | `src/ee/kernel.cpp` (save/load, tolerant read) | no replay across resume |
| Unit coverage | `tests/unit/ee_kernel_test.cpp` | bytes, guards, snapshot incl. legacy |
| Census tripwire | `gt4boot_originating` CTest + `--threads` dumps | drain, idle, thread 2 hungry |
| Pump/handler facts reused | disassembly + SIF array (slice-29 map) | what traffic traverses |

## Understanding checkpoint

1. Posting `{0,N}` to the ring plus signaling sema 11 would
   wake thread 2's consumer immediately. Why is that
   fabrication while the SIF packet is legitimate traffic —
   state the exact line between them.
2. The trigger requires the dispatch entry populated, added
   after observing drain-without-handling. Reconstruct what
   the fresh-boot leg showed that forced the gate, and why a
   unit test alone could not have shown it.
3. A direct re-fire would be unobservable. Prove it: enumerate
   every observable the re-fire touches and show each is
   idempotent — then explain why the file-level flag read is
   the only proof that fits.
4. The CTest pins the queue drain, not the register write.
   Under what future observation would that choice be wrong,
   and what would have to change (code, test, or doc)?
5. The census tripwire asserts nobody woke. Which three future
   developments would each fail it loudly, and why is loud
   failure the desired behavior in each case?
6. "Mechanism, not unblock" — write the one-paragraph version
   of this lesson you would give a skeptic claiming the packet
   constitutes progress.
