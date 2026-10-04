# M30 lesson — the SIF layer, the model IOP, and the first running boot

Prepared 2026-10-04. BUILD/VERIFY: passed for slices 7 and 8; see the
[M30 slice-7 evidence](../reverse-engineering/m30-sif-and-interrupt-injection.md),
[slice-8 evidence](../reverse-engineering/m30-slice8-rpc-and-vblank.md),
and [decision 0009](../decisions/0009-sif-and-interrupt-injection.md) /
[decision 0010](../decisions/0010-rpc-vblank-and-device-windows.md).
EXPLAIN: this is the worked explanation; tutoring review pending.

## Objective and motivation

Slices 1–6 built a machine that boots the game through its early
kernel setup and then hits a new kind of wall: not an undecoded
instruction, but a *conversation partner that does not exist*. The
game enables the SIF0 DMA channel and waits for the IOP's handshake;
without an IOP model, the handshake spins forever. This arc teaches
the project's second founding move (the first was tracing guest
waits in the delay arc): when the game talks to hardware or
firmware that is not there, model the *smallest peer the protocol
requires* — seeded state, one answered command, and a delivery
path for the reply — and let the game's own code do the rest. It
ends with the boot in its running state: 3,000 services, the
interpreter reference at 7,515,389 instructions, full state
identical.

The motivating failure is concrete: the sixth slice stops at
`SifSetDChain` (0x78), and the public `sceSifInitCmd` source shows
exactly what follows — enable the channel, register its DMA
handler, handshake through the SIF registers, wait for CMDINIT.
Every one of those steps needs something the model does not have
yet.

## Step 1 — the register layer and the synchronous DMA

The SIF services split into two kinds, and the split matters
because only one of them ever touches hardware timing:

- `SifSetReg` (0x79) / `SifGetReg` (0x7A): public indices 1–4 name
  MAINADDR/SUBADDR/MSFLAG/SMFLAG, stored at `0x1000F200 +
  (index-1)*0x10` so syscalls and direct MMIO reads see the same
  storage; indices `0x80000000+` are software system registers
  living in the kernel.
- `SifSetDChain` (0x78 and its negative alias) writes SIF0's CHCR
  (`0x1000C000`) = `0x184`, exactly the value the public header
  documents; `SifStopDma` (0x6B) clears it.
- `SifSetDma` (0x77) walks the `SifDmaTransfer_t` descriptors and
  copies each range byte by byte inside guest memory, returning an
  id; `SifDmaStat` (0x76) always reports done (-1), the value the
  SDK's polling loops expect. No transfer is ever pending.

The honesty rule appears immediately: the synchronous copy is
documented as what it is — no IOP timing, no scheduling — and
`SifSetReg`'s return is recorded as ignored-by-callers rather than
claimed as hardware truth.

## Step 2 — seed an initialized IOP, answer one command

A minimal IOP model is a deliberate choice, stated as such: it
represents a console whose IOP has completed boot, not a guessed
register dump. On the first SIF service the kernel seeds SMFLAG
with SIFINIT|CMDINIT|BOOTEND and SMCOM with the shared
command-buffer address `0x00080000`. When a transferred packet's
destination is the IOP command buffer, the stub inspects the
SIFCMD header: for `SIF_CMD_INIT_CMD` it writes a
`SET_SREG(RPCINIT=1)` reply into the EE buffer named in the
request and queues the SIF0 DMA interrupt. Every other command
transfers without a reply — exactly as an IOP that does not
implement them.

The uncached KUSEG mirror (`0x20000000–0x3FFFFFFF` → physical &
mask) joins the segment alias here for one reason only: the SIF
buffers are `UNCACHED_SEG` addresses and the guest dereferences
them. Model what the guest touches, nothing more.

## Step 3 — deliver the reply as a real interrupt

```text
0x78 SifSetDChain ............ the old wall, channel enabled
0x12/0x16 .................... game registers its SIF0 DMA handler
0x7a x3 ...................... CMDINIT handshake reads model SMFLG/SMCOM
0x79 x2 ...................... game stores SUBBADDR/MAINADDR
0x77 ......................... INIT_CMD travels; stub replies + queues interrupt
0xffffff88 (= iSifSetDChain) .. THE GAME'S OWN DMA HANDLER RUNS, re-enables,
                                returns through the model stub
```

That `0xffffff88` line is the arc's payoff and deserves a slow
read: it is not a model function. It is the *game's* handler,
installed by the game, invoked by the model's injection with the
cause in a0 and the interrupted context saved, running real guest
code (it re-enables the chain), and returning through the model's
private return service, which restores the interrupted context.
The kernel queues causes; the driver and the reference loop
deliver them at clean unit boundaries through
`RunOptions::start_interrupt`; the deferred-call stack now carries
patched syscalls *and* interrupts, so a handler may block like any
other guest code. The alternative considered and rejected was
calling the DMA handler directly inside `SifSetDma`: the handler
is guest code, and calling it re-entrantly without a saved
interrupted context would corrupt the running thread and could
never support blocking.

## Step 4 — answer the RPC layer, then give the machine a heartbeat

The next wall arrives on schedule: `sceSifBindRpc` sends
`SIF_CMD_RPC_BIND` (`0x80000009`) and waits on a semaphore the
reply path signals. The model IOP logged every transfer reaching
its command buffer, and the layouts match the public
`sifrpc-common.h` field for field — bind (64 bytes, rec_id,
pkt_addr, rpc_id, cd, sid at +32), call (`0x8000000A`, recvbuf at
+40, sd at +52), reset (`0x80000003`, 104 bytes carrying
`rom0:UDNL cdrom0:\IOPRP300.IMG;1`, the string the public
`iopcontrol.c` builds). The answers invert the same evidence:

- Binds get the 64-byte `SifRpcRendPkt_t` end packet: the
  request's handles echoed plus a non-null model server handle
  and buffers; the game's client signals its semaphore.
- The first call (sid `0x80000001`, RPC `0xFF`) answers
  `{0x00275520, 2}`: the second word is 2 because the game's
  client checks `word[1] == 2`; the first is the game's own
  compatibility constant at `0x0066829C`, which the game compares
  its IOP version against. Only the version query has evidence
  for its answer; every other RPC function returns empty results,
  and the game's reaction is observed instead of invented.
- Reset records the image path and completes the modeled reboot
  before the first SIF register read after the reset — exactly
  the bit the game's `SifIopSync` poll waits for.

Then the peripheral windows, each a storage decision with its
reason: the GS block (`0x12000000`, 8 KiB) holds what the guest
writes including 64-bit accesses; GIF/VIF/FIFO/IPU/DMA/INTC/SIO
are 32-bit banks; PCCR (CP0 25) stores (the model's timers don't
count, so frame-time accumulators stay zero — counting
instructions would differ between module and interpreter and
break the differential); VU1 FBRST bits are recorded, not fatal
(GT4 resets VU1 in display setup; with no microcode executed,
"resetting VU1" has no target state).

And the heartbeat: when a service reports no runnable thread, the
driver asks for one idle interrupt. The kernel raises VBlank
(INTC cause 2), sets the status bit, saves context, and injects
*every* registered handler for the cause in registration order —
the first handler alone (`0x004AB6D8`) woke nothing; the chain
(`0x004AB6D8` then `0x004AB430`, which reads PCCR, clears it
twice, and counts frames at `0x70002000`) woke the game's
threads. One handler per cause would have been a guess; chaining
in registration order is what the hardware does. The budget (60
consecutive idle interrupts, then stop) is a guard, not a
frequency. The verified run ends not at a wall but in the runtime
cycle: `WaitSema`/`SleepThread`, injected handler return,
VBlank handler's `iGetThreadId`/`iPollSema`/`WakeupThread`, back.

## What later evidence reframed (not smoothed over)

- The slice-7/8 docs call `0x005ae090` the pump's dispatcher.
  Decision 0026's later mapping corrected this: `0x005ae090` is
  the `-0x78` SifSetDChain *stub*, and the pump's real dispatch
  is its own table-driven `jalr`. The mechanism described here
  (stub call inside the pump) is unaffected; only the label was
  wrong.
- "Interrupts only at idleness, budget 60" was the whole delivery
  story then. It grew afterward — timer ticks with compare
  delivery, DMA channel completions, coalesced queues, a 2M
  budget, a service clock — each its own evidenced slice. This
  lesson teaches the shape (queue → unit boundary → frame →
  chained return); the later slices teach the traffic.
- The 3,000-service "running state" is a comparison point that
  later legs ran orders of magnitude past (243M services). It was
  never a destination, only the first place the two engines
  agreed bit-for-bit.
- Slice 8's recorded wall — the first RPC call expecting real
  data — was genuinely resolved later by the file/disc work
  (decisions 0014 onward), not worked around.

## Connection to our implementation

| Piece | File | Job |
| --- | --- | --- |
| SIF services + software regs | `src/ee/kernel.cpp` (`sif_set_reg`, `sif_get_reg`, `sif_set_d_chain`, `sif_stop_dma`, `sif_set_dma`, `sif_dma_stat`) | register block, synchronous copy, always-done |
| Model IOP seeding + stub | `src/ee/kernel.cpp` (`ensure_sif_ready`, `run_iop_stub`) | initialized flags, INIT_CMD reply, queued completion |
| RPC replies + reset + OSD2/GS | `src/ee/kernel.cpp` (bind/call answers, version table, reset, `set/get_osd_config2`, `gs_get/put_imr`, `set_gs_crt`) | end packets, compatibility constant, storage windows |
| Interrupt plumbing | `src/ee/kernel.cpp` (`queue_interrupt`, `start_interrupt`, `inject_interrupt`, `install_handler_frame`, `deferred_return`) + `src/ee/driver.cpp` (`start_interrupt` option) | queued causes, saved contexts, chained returns |
| KUSEG mirror | `src/ee/state.cpp` (segment alias) | uncached SIF buffers dereference |
| Frame/return cycle test | `tests/unit/ee_kernel_test.cpp` | SIF round trip, IOP flags, INIT stub, handler chain, idle budget |
| Whole-run agreement | `tools/gt4boot/main.cpp` (`--compare-interpreter`) | 3,000 services identical, memory digest included |

## Understanding checkpoint

1. `SifDmaStat` always returns done (-1). Why is a constant the
   honest answer here, and what future evidence would force it to
   change?
2. The stub answers INIT_CMD but transfers every other command
   without a reply. Explain why "an IOP that does not implement
   them" is evidence-shaped rather than a guess, and what breaks
   if a needed command stays silent (use the bind wait as the
   example).
3. The first VBlank handler in the chain wakes nothing; the chain
   wakes the threads. Why is per-cause chaining in registration
   order the correct model, and what would a single-handler model
   have missed?
4. A direct call of the DMA handler inside `SifSetDma` was
   rejected. Reconstruct the corruption precisely: whose context
   breaks, and why can the rejected form never support blocking?
5. The version answer `{0x00275520, 2}` comes from the game's own
   image and checks. Why is reading the expected answer out of
   the game's code legitimate evidence, while fabricating other
   RPC results is not?
6. Slice 8's idle budget (60) later became 200,000 and then 2M.
   What does the budget guard against, and why is raising it not
   the same as claiming a modeled frequency?
