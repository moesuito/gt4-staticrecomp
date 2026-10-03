# M32, twenty-ninth slice — the SIF pump path mapped for synthesis

Date: 2026-10-03. Inputs: the pinned CORE and ISO; a 2,000-service
resumed leg from `build/ckpt-1980k.bin` (`--threads`, four `--dump`
views); whole-text byte scans; `gt4disasm` reads. Read-only: no
probes with state, no model change. This is the evidence companion
to decision 0026 (which ranks the first originating event); the
verdict there draws only on what is mapped here.

## Stop-time state at the 1980k frontier (Confirmed — leg census)

Same 17-thread shape as every prior census: thread 2 in `WaitSema`
on sema 11 (entry `0x005ae9a0`); threads 4, 6, 8, 11, 18 in
`WaitSema` on binary delay semaphores; the other eleven asleep
(main in the flag chain, eight engine workers in the condvar spine,
thread 3 in SDK sleep, thread 9 in its own worker). Limit-hit at
`no-runnable-thread 0x00001604`; 2,000 module calls, 184,001
interpreted steps; 1 deferred call, 1 pending interrupt.

- Pump queue `[0x00886740]`: first byte 0 — provably empty; stale
  bytes follow (`RPC_END 0x80000008`, record 5: leftover template,
  slice 1's reading stands).
- SIFCMD area `[0x00886800]`: length `0x14`, `INIT_CMD 0x80000002`
  at +8; `[0x00886818] = 0x20886740` (queue pointer, mirror alias of
  `0x00886740`); `[0x00886824] = 0x00886840` (handler table);
  `[0x00886828] = 0x00000020` (table bound 32);
  `[0x00886834] = 0x008869C0` (software-register array base).
- Handler table `[0x00886840]`: `{0x005b0870, 0x00886818, 0}`,
  `{0x005b0850, 0x00886818, 0}`, then zeros through `[0x008868A0)`
  (one further word `0x005b1328` at `+0x60`, outside the bound).
- Job ring `[0x00885EE8]`: consumer = producer = `0xb5`, all 512
  slots `{0,3}` — silent, as in slices 2–3.
- DMAC channel 5 handler `0x005b0e30` registered; service `-0x78`
  (`sif_set_d_chain`) registered in the model.

## The pump, end to end (Confirmed — disassembly)

`0x005b0e30` (DMAC ch5 completion handler):

1. `s1 = 0x00886818`; `a3 = [s1]` (queue base); `count = [a3]`;
   zero count exits at `0x005b0f64` (today's always-taken path).
2. Else clears `[a3]`, copies `(count + 0x1E) >> 4` quadwords
   (`lq`/`sq`) from the queue to its stack frame.
3. `jal 0x005ae090` — not a dispatcher but the SDK stub for service
   `-0x78` (SifSetDChain re-kick; the model handles it, setting
   SIF0 CHCR `STR` — the slice-48 "vestigial" bit's live source).
4. `v1 = [sp+8]` (third drained word); if `0 <= v1 < [s1+0x10]`
   (0x20): `entry = [s1+0xC] + (v1 & 0x7fffffff) * 0xC`; if
   `[entry] != 0`, gp-swapping `jalr [entry]` with `a0` = stack
   copy, `a1` = `[entry+4]`, `gp` = `[entry+8]`; gp restored after.
   (A second table at `[s1+0x18]`/`[s1+0x14]` serves the
   negative-ack path; both tables resolve into the same array.)
5. `sync`, `ei`, return 0.

Dispatch-index derivation (Confirmed): index = drained word 2 &
`0x7fffffff`. Index 1 (`SET_SREG 0x80000001 & mask`) selects entry 1
(`0x005b0850`, arg `0x00886818`, gp 0); index 0 selects entry 0
(`0x005b0870`). Indices 2–31 are zero-filled and skipped.

## The two reachable handlers are register accessors (Confirmed)

- `0x005b0850`: `[0x008869C0 + ([a0+0x10] << 2)] = [a0+0x14]` —
  indexed software-register write (reg = drained word 4, value =
  drained word 5). This is the slice-14 `SET_SREG` path.
- `0x005b0870` and neighbors (`0x005b0880/0x005b0898/0x005b08B8`):
  register read/compute helpers over the same `0x008869C0` array
  (`[a1+8] = [a0+0x10]`, `a0 = base + (idx << 2)`, …).
- None signals a semaphore, wakes a thread, posts a job, or touches
  anything outside the register array. A pump packet therefore
  changes register bytes and nothing else — verified against the
  waiter census (no parked thread polls these registers; the
  command-layer spin at `0x00590A18` has no thread in it).

## Thread 2's ring layout (Confirmed — creator `0x005aea78`)

One-shot guarded creator (`[0x006602A8]`): `CreateSema` id stored at
`[0x00885EE0]` (= sema 11); `CreateThread(entry 0x005AE9A0, stack
0x00885AE0 size 0x400, arg 0x006E5DF0)`. Ring `[0x00885EE8,
0x00886EE8)` (512 `{op,arg}` slots) sits directly after the stack.
Op 0 = deferred wake (`{0,5}` flickered a sleeper in slice 3);
all 181 historical jobs are `{0,3}` (wake thread 3 — plausibly the
delay system waking its waiter, making the ring producer most
likely the delay path itself, not SIF).

## Failed hunts (recorded, not fudged)

- `0x885EE8`/`0x885AE0` as immediates: zero hits — the ring base is
  runtime-derived (stack-adjacent), so the producer cannot be found
  by constant scan. Producer identity stays Unknown.
- `0x869C0`-area immediates: three hits, all false positives
  (`0x2869C0`/`0x4869C0` data addresses, one code address) — the
  register array is reached via computed `base + (idx << 2)` only,
  so readers cannot be inventoried statically either.
- `jal` to walk/walk-dispatch/dispatcher (`0x005b822c/0x005b8238/
  0x005b8158/0x005b8ed8`): zero direct sites — all indirect, as
  slice 16 established.

## Grades

- Confirmed: every byte, address, and code claim above (leg dumps,
  disassembly, byte scans, service-table rows).
- High confidence: a pump packet's wake effect is zero by
  construction (accessor-only handlers + accessor-only table +
  no polling waiter).
- Unknown: the ring producer's identity; SIF-register readers in
  general; what the stale queue/template bytes once carried.

Next: decision 0026 ranks the first event on this evidence.
