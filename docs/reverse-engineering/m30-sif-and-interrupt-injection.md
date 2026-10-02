# M30, seventh slice — the SIF layer, the model IOP and interrupt injection

Date: 2026-10-02. Inputs: the pinned CORE. Follow-up to the sixth slice
(`m30-osd-and-the-iop-wall.md`): the boot stopped at SifSetDChain (0x78), and
the public `sceSifInitCmd` showed the following IOP handshake would spin
forever without an IOP model. This slice builds the SIF service layer, a
model IOP for the SIFCMD init handshake, and the interrupt-injection
mechanism that delivers its reply.

## What changed

- **SIF register services** (0x79/0x7A): hardware indices 1-4 map to the SIF
  register block at 0x1000F200 (MSCOM/SMCOM/MSFLG/SMFLG), software system
  registers (0x80000000+) live in the kernel; `SifSetDChain` (0x78/`i` alias)
  writes SIF0's CHCR = 0x184 and `SifStopDma` (0x6B) clears it.
- **Synchronous SIF DMA** (0x77/`i`): the descriptors' ranges are copied in
  guest memory and an id is returned; `SifDmaStat` (0x76/`i`) always reports
  done (-1).
- **The model IOP** seeds itself on the first SIF call: SMFLG =
  SIFINIT|CMDINIT|BOOTEND, SMCOM = 0x00080000. When a transferred packet's
  destination is the IOP command buffer, the stub reads the SIFCMD header:
  for `SIF_CMD_INIT_CMD` it writes `SET_SREG(RPCINIT=1)` into the EE buffer
  named in the request and queues the SIF0 DMA interrupt.
- **Interrupt injection**: `RunOptions::start_interrupt` lets the driver (and
  the reference loop) deliver queued causes at unit boundaries. The kernel
  saves the full interrupted context, installs the registered handler's
  frame (a0 = cause, gp/sp inherited, ra = the return stub, EIE clear) and
  the existing private return service restores the context when the handler
  returns. The deferred-call stack now carries both patched syscalls and
  interrupts, so a handler may block like any other guest code.
- **The uncached KUSEG mirror** (0x20000000-0x3FFFFFFF) joins the segment
  alias, because the SIF buffers are `UNCACHED_SEG` addresses.

## The verified run

```
build/gt4boot.exe private/fingerprint-check/CORE.GT4 --services 800 --compare-interpreter
...
boundary: no-runnable-thread 0x005adce4
stats: module calls 95, interpreted steps 18023, services handled 94
interpreter: 6322280 instructions, state identical (registers, HI/LO, FPU, VU0, CP0, pc, memory digest)
```

The service trace tells the story end to end:

- `0x78` **SifSetDChain** — the wall from the previous slice; the SIF0
  channel is enabled.
- `0x12`/`0x16` **AddDmacHandler / EnableDmac** — the game registers its
  SIF0 DMA handler (the one that will parse the IOP's reply).
- `0x7a` ×3 **SifGetReg** — the CMDINIT handshake check reads the model
  IOP's SMFLG and then the IOP command buffer address (SMCOM).
- `0x79` ×2 **SifSetReg** — the game stores the software SUBBADDR/MAINADDR.
- `0x77` **SifSetDma** — the SIFCMD `INIT_CMD` packet travels to the IOP
  buffer; the stub writes the `SET_SREG(RPCINIT)` reply into the EE buffer
  and queues the interrupt.
- `0xffffff88` (= **iSifSetDChain**) and `0x100` — **the injected interrupt
  ran the game's own DMA handler** (which re-enables the chain) and returned
  through the model's stub, restoring the interrupted context. This is the
  first real interrupt delivered in the project.
- `0x7a`, `0x77`, `0x79`, `0x40` (CreateSema), `0x77`, `0x44` (WaitSema) —
  the RPC layer initializes, sends a bind packet and waits for the IOP's
  reply. None comes, so the waiting thread is the last runnable one and the
  run stops at **`NoRunnableThread`** — a clean, deterministic boundary.
- The interpreter reference reaches the same stop after 6,322,280
  instructions with every compared field identical.

## The next wall: RPC replies

- `sceSifBindRpc` sends `SIF_CMD_RPC_BIND` (0x80000009) and waits on a
  semaphore that the SIFCMD reply path signals. The model IOP answers only
  the init handshake today, so the RPC bind has no reply and the run stops at
  the wait. The next slice extends the stub to answer binds (and then calls),
  which in turn needs the game's RPC server table — an IOP-module-level
  behavior, not a hardware one.
- The public `sifrpc.c` is the protocol evidence; the live PCSX2 with PINE is
  the oracle for the actual replies the game expects.

## Limits recorded

- The IOP model is the handshake only; other SIFCMD commands are transferred
  without a reply.
- Interrupts are checked at unit boundaries and come only from the model's
  own DMA completions; there is no timer or external interrupt source yet.
- The SIF DMA is a synchronous local copy; no IOP timing or scheduling is
  modeled.
- `SifSetReg`/`SifGetReg` return the value written/read; the verified callers
  ignore the set result (documented, not evidence of the hardware's return).

## Evidence

- CTest **32/32** (`ee_kernel` now covers the SIF register round trip, the
  model IOP flags, the INIT_CMD stub and the full interrupt frame/return
  cycle); Python 73 collected (67 run, 6 skip).
- The whole-run differential comparison at the new frontier.
