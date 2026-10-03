# M32, fourth slice — what the six workers wait for

Date: 2026-10-03. Inputs: the pinned CORE and ISO; the 243.7M
checkpoint read directly (a temporary parser, since deleted) plus the
game's own code. Read-only; no model change.

## Unwinding the waiters (Confirmed)

All six sema-waiters (threads 3, 4, 6, 8, 11, 18, same entry
`0x005786f0`) share one program counter shape: blocked in `WaitSema`
with **identical return address `0x005aedc0`** — the same wrapper. That
wrapper (`0x005aedb8`: wait on `s0`, then delete it) is a one-shot
handshake wait, and for every thread the waited id equals its own `s0`
(e.g. thread 3: `s0 = wait = 0xAF3543 = 11482435`; all six check out):
each worker posted a request and waits for its completion signal.

The wrapper's function (`0x005aed40`) shows the request shape: create a
sema, call `0x005b8d88(0)`, then `0x005b8f38(result, 0x005AEF58, sema)`
— and `0x005b8f38` is the delay-library path of slice 10 (through
`0x005B8C60`/`0x005B8B68` to dispatcher `0x005B8ED8` and the
`iSignalSema` endpoint `0x005AEF58`). The completion this waits for is
a delay-queue firing that signals the sema.

## The stall is the delay dispatcher (high confidence)

Slices 10–11 left exactly this frontier open: delay nodes stay
`accumulated = 0`, the TIM2 due condition never fires, the dispatcher
is never reached. The six waits are its downstream victims. Supporting
evidence, exact-value level:

- Thread 3's stale `a1 = 0xfffffe40` **is** the live TIM2 compare;
  thread 11's stale `a1 = 0x782` **is** the live TIM2 mode (both from
  the stop-time `--threads` readout). The waits are timer-conditioned.
- All six semaphores: binary, `count 0`, `init 0`, one waiter each —
  never signaled, exactly what an unfired delay queue leaves behind.
- The VBlank chain pairs worker to handler by code family (thread 4 ↔
  `0x00551728` via `0x551xxx` frames; 6 ↔ `0x0054a1d8`; 8 ↔
  `0x0055eb08`; 11 ↔ `0x00555290`; 18 ↔ `0x00557af0`; 3 carries timer
  values). The handlers run every frame and reprogram/check timer
  state — e.g. `0x00551728` gates real work on `[0x65c714] != 0`
  (zero at the stop, so effect-free). Handler and waiter belong to the
  same timer-driven subsystem; the unmet condition sits between them.

## Correction to an earlier hypothesis

Slice 2's framing suspected the workers' SIF binds might stall. They
don't: the shared stack spine (`0x577fb8` → `0x5782d0` → `0x578518`,
the retry loop around the bind helper `0x005b17d0`) is stale history —
24 servers stand bound. The bind machinery itself is now mapped as a
bonus: `0x005b17d0` allocates a client node, `CreateSema`s a completion
sema, sends `RPC_BIND` (`0x80000009`) through the SIFCMD sender
(`0x005b0db0` → `0x005b0c78`, descriptors on-stack, cache writeback via
`0x005b0f78`, kick via the `0x77`/`0x76` syscalls), waits the sema,
deletes it. The live stall is downstream of all of this, in the timed
waits.

## What unblocks the boot (next)

The curriculum already lists it: the counting timer with
interrupt/delay delivery (the delay-node due condition and the
dispatcher chain of decisions 0011/0012 territory). Slice 5 delivers
the smallest piece that fires one delay node — or proves which exact
condition is unmet — most directly by experiment, since the registers
(TIM2 count/mode/comp) and the node layout are all observable live.

## Verification

- Register/waits/sema/stack quadruples agree per thread (6/6); stub
  numbers rechecked against the service table (`0x40/0x41/0x44` at
  `0x005adca0/0xb0/0xe0`).
- Temporary parser deleted; product code untouched (docs only).
- Full gates run on the final tree before commit.
