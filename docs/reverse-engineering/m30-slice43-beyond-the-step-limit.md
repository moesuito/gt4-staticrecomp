# M30, forty-third slice — beyond the step limit: the steady pump and the next wall

Date: 2026-10-02. Inputs: the pinned CORE and ISO. Follow-up to the
forty-second slice (the PRTS block cache; the boot reaches the 200M-step
limit with 3,648,011 services handled).

## The question

The boot ends at the step budget (`step-limit 0x0055e790`) with millions
of services handled. Is there a next wall (a fault, a stalled service, the
end of the boot's init) past a bigger budget, or is the machine merely
doing long, healthy work?

## Tooling: `gt4boot --steps N`

The step budget was a compile-time constant (200M). It is now a flag
(default unchanged), verified by the fast CTest `gt4boot_steps`
(`--steps 1000` stops at the step limit after 6 module calls and 994
interpreted steps). All exploration below uses it.

## The stop point is mid-copy, not a stall

The 200M-step stop pc (0x0055e790) sits in the bulk-copy loop of
**0x0055e6d0**, a per-packet routine of the 0x0055xxxx client library:
wait for completion (0x00578500), size it (0x005b72a8, a Status-bit
spin-wait with interrupts disabled), copy ~960 bytes (an unaligned head
bounded by `s1 + 0x3a0` set in the branch delay slot, then the aligned
bulk loop), re-enable interrupts (`ei`), set the done flag and acknowledge
with RPC op 4. Stopping inside a ~30-iteration loop that runs per packet
is expected under a step budget, not evidence of a hang.

## The service mix is stationary: a healthy streaming pump

Over the full 200M-step run (3,648,011 services, 42 distinct numbers) the
mix at the start matches the mix at the end (first 200k vs last 200k
services are proportionally identical). Decoded against the kernel's
service table:

| Service | Meaning | Count | Note |
|---|---|---|---|
| 0x100 | patched-syscall return | 1,313,536 | the bridge traffic of every patched call |
| 0x2f (+-0x2f) | get_thread_id | 714,574 | workers polling their own id |
| 0x44 | wait_sema | 297,181 | lock-step with signal_sema (0x42 + -0x43: 297,663) |
| 0x32 | sleep_thread | 210,760 | parked workers |
| 0x40 / 0x41 | create_sema / delete_sema | ~190,100 each | per-packet semaphore lifecycle |
| 0x77 (+-0x78) | sif_set_dma / sif_set_d_chain | ~78,700 | the RPC transport underneath |

(The `0xffffffxx` numbers are the patched table's alternate codes:
-0x43 = signal_sema, -0x2f = get_thread_id, -0x34 = wakeup_thread,
-0x78 = sif_set_d_chain.)

The stop-time thread table agrees: 5 threads asleep in the SleepThread
stub (pc 0x005adbc8), 2 in the WaitSema stub (pc 0x005adce8), 7 ready or
running, zero deferred calls, zero pending interrupts, DMA channels idle,
timer 2 counting toward its compare. A running multithreaded game, not a
deadlock. (The parked pcs are the return addresses of the game's own
syscall stubs at 0x005adb80..0x005adc08, one 8-byte stub per number.)

## Almost no disc traffic: the pump is RPC chatter, not media streaming

A temporary LBA trace (since removed) over the first 400k and 1.5M
services found only **8 disc reads in 1.5M services**: the 7 PCDV
boot/mount reads plus a single PRTS block (0x1BFC6, 0xAD50). The
semaphore churn is the SIF RPC machinery (each RPC mints and releases a
sync semaphore), not disc I/O. The game is doing long CPU/RPC work with
barely any disc reads — so the "wall", if any, lies further out, not in
the streaming path.

## The next wall: an unaligned fault at ~15M services (confirmed)

A 10x run (`--steps 2000000000`, 13.5 min) ends before the budget with:

```text
FAILURE: Guest fault at pc 0x00491798: Guest access at 0x009cf08f (width 4)
is not naturally aligned
```

after **15,010,045 services** — with the same steady mix to the end.
The fault function (0x00491798–0x004918a8) is a **pointer relocation
after a move**, the same family as the slice-23 fault: `s2 = s0 -
*(s0+4)`; if the object moved, rewrite the base at +0x4 and add `s2` to
the embedded pointers at +0x0/+0x14/+0x18 plus the two tables walked
through helpers 0x00491e80/0x00491990 (count from the halfword at
+0x10). The faulting access is consistent with an **odd structure
pointer** (`s0+4` or `s0+8` landing on 0x009cf08f on the first loads).

Callers: one direct translated call site at **0x0048fb94**
(`jal 0x00491798`, delay slot `a0 = s5`), where `s5` is the return of
**0x00491d90** (which fills a stack struct through 0x004ae230 and
returns `*(sp+0x10)`); plus the module's internal dispatch entry for
indirect (register/jump-table) callers. Whether the odd pointer is the
game's own bug or model-fed bad data is the next slice's experiment.

## Determinism (high confidence)

A second full 2B-step run from the clean tree (reverted instruments,
regenerated translation) faults at the **same pc (0x00491798), same
address (0x009cf08f) and same service count (15,010,045)**. The wall is
deterministic guest behavior, not build flakiness. (The interpreter
differential cannot reach this depth: ~6.5B instructions would take days.)

## Verification

- CTest 36/36 (including the new `gt4boot_steps`) and Python 73 (67 run,
  6 skip), green on the final tree.
- All temporary instruments (the LBA/RPC traces, the second trace binary)
  are removed; the tree holds only the `--steps` flag and its test.
- Committed and pushed; `main` green.
