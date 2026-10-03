# M32, second slice — thread 2's job queue, mapped end to end

Date: 2026-10-03. Inputs: the pinned CORE and ISO; the 243.7M
checkpoint read directly (a temporary parser, since deleted) plus the
game's own code. Read-only: no model change, no run needed for the
mapping itself. The replay proof is the third slice.

## The block (Confirmed — thread 2's saved registers match the code)

Thread 2's checkpoint context holds the dispatch loop's live state, and
every register agrees with the disassembly of `0x005ae9a0`:

- `s1 = 0x00885ee8` (the loop's argument block: `s2 = s1+8` is
  `0x00885ef0`, `s0 = s1+9` is `0x00885ef1` — both exact),
- `a0 = 11, a1 = 1` (blocked in `WaitSema(11)`),
- `ra = 0x005ae9e8` (the wait call's return — it looped before),
- counter `[s1] = 0xb5 = 181`, producer `[s1+4] = 181`: balanced, the
  ring is drained.

## The ring protocol (Confirmed)

- 512 two-byte slots at `s1+8`: `{op, arg}`. The consumer index is
  `(counter & 0x1ff) * 2`.
- The op map, by disassembly: **0 = `WakeupThread(arg)`** (via stub
  `0x005adbd0` = service `0x33`), **1 = `RotateReadyQueue(arg)`** (stub
  `0x005adb50` = `0x2b`), **2 = `SuspendThread(arg)`** (stub
  `0x005adc10` = `0x37`); anything else calls `0x005b07d0`. Every path
  loops back to the wait.
- History: **all 512 slots are `{0, 3}`** — the only job this boot ever
  carried is "wake thread 3", 181 times. Op 1/2 never fired here.
- Control block head at `0x00885ee0`: `{11, 0, 0xb5, 0xb5}` = sema id,
  unused, consumer, producer.
- Distribution: a whole-RAM pointer scan finds **exactly one** pointer
  into the block: `[0x00885c80] -> 0x00885ee8`, sitting alone in a
  zero-padded struct. No code references these addresses as immediates;
  the pointer travels at runtime.
- Thread 2 and sema 11 share `option = 0x006d2360`, which points into
  the SDK kernel library's own strings (`SceKerneltopThread`,
  `SceKernelDelayThread`): kernel infrastructure, created once at boot.

## The creator (Confirmed — static)

Function `0x005aea78` (the bytes right after thread 2's loop) builds
both objects: `CreateSema` (stub `0x005adca0` = `0x40`) whose id it
stores at `0x00885ee0`, then `CreateThread` (stub `0x005adaa0` =
`0x20`) with entry `0x005ae9a0` (built as `0x5b0000 - 0x1660`), stack
`0x00885ae0`/`0x400`, gp `0x006dddf0`, option `0x006d2360` — every value
matches the checkpoint exactly. `DeleteSema` (`0x005adcb0` = `0x41`) is
only its error path. Its single caller `0x005b7590` sits in the SDK init
cluster (next to `SetSyscall` stubs). So: thread 2 is the kernel's
deferred thread-ops dispatcher (wake/rotate/suspend on behalf of
producers that post jobs), not game logic.

## Ruled out as the producer (Confirmed)

- No registered interrupt/DMA handler carries a job-block address in
  its argument (all 13 parsed from the checkpoint: small ints and
  unrelated pointers). The SIFCMD accessor thunks (`0x005b0850`…)
  only shuffle packet words.
- The SIF pump's queue stays provably empty (slice 1).

## Still open: the wild producer

Something posted 181 `{0,3}` jobs during the traffic-heavy boot and
went silent. It writes the producer counter and slots, then signals
sema 11 — reachable only via the global handle. Thread 3 holds 1 stored
wakeup credit. Naming the writer is slice 4's job.

## Verification

- Every address cross-checks three ways (register, memory, code).
- Temporary checkpoint parser deleted after use; nothing to remove from
  the tree (this slice changed no product code).
