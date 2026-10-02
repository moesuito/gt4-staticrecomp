# 0009 — The SIF layer, the model IOP and interrupt injection

Status: implemented 2026-10-02 for the M30 seventh slice
(`docs/reverse-engineering/m30-sif-and-interrupt-injection.md`).

Context: the boot stopped at SifSetDChain (0x78). The public `sceSifInitCmd`
shows the next steps: enable the SIF0 channel, register its DMA handler, then
handshake with the IOP through the SIF registers and wait for CMDINIT; the
RPC layer that follows sends command packets and waits for IOP replies. A
minimal IOP model is required for any of it to complete.

Decision:

- **SIF register services.** `SifSetReg` (0x79) and `SifGetReg` (0x7A) map
  the public indices 1-4 (MAINADDR/SUBADDR/MSFLAG/SMFLAG) onto the SIF
  register block at 0x1000F200 + (index-1)*0x10, so syscalls and direct MMIO
  reads see the same storage; the software system registers (0x80000000+)
  live in the kernel. `SifSetDChain` (0x78 and its negative alias) writes
  SIF0's CHCR (0x1000C000) = 0x184, exactly the value the public header
  documents; `SifStopDma` (0x6B) clears it.
- **Synchronous SIF DMA.** `SifSetDma` (0x77) walks the `SifDmaTransfer_t`
  descriptors, copies each range byte by byte inside the guest memory, and
  returns an id. `SifDmaStat` (0x76) always reports "done" (-1), the value
  the SDK's polling loops expect. No transfer is ever pending.
- **The model IOP is initialized.** On the first SIF service the kernel seeds
  SMFLG with SIFINIT|CMDINIT|BOOTEND and SMCOM with a shared command-buffer
  address (0x00080000). This is a deliberate model choice: it represents a
  console whose IOP has completed boot, not a guessed register dump.
- **The IOP stub.** When a transfer's destination is the IOP command buffer,
  the kernel inspects the SIFCMD header. For `SIF_CMD_INIT_CMD` (the SIFCMD
  init handshake) it writes a `SET_SREG(RPCINIT=1)` reply into the EE buffer
  named in the request and queues the SIF0 DMA interrupt. Other commands are
  transferred without a reply, exactly as an IOP that does not implement
  them.
- **Interrupt injection.** The kernel queues causes; the driver (and the
  differential reference) delivers them at clean unit boundaries through
  `RunOptions::start_interrupt`: the full interrupted context is saved on a
  deferred-call stack, a handler frame is installed (pc = the registered
  handler, a0 = cause, gp/sp inherited, ra = the return stub, EIE clear), and
  the handler returns through the existing private return service, which
  restores the interrupted context. Blocking inside a handler works like any
  other guest code.
- **The uncached KUSEG mirror** (0x20000000-0x3FFFFFFF → physical & mask) joins
  the segment alias: the SIF buffers are `UNCACHED_SEG` addresses and the
  guest dereferences them.

Alternatives considered:

- **A full IOP HLE** (implement every module the game talks to). Deferred:
  it is a per-command effort; the stub keeps each new command an explicit,
  documented step instead of a speculative monolith.
- **Calling the DMA handler directly inside `SifSetDma`.** Rejected: the
  handler is guest code; calling it re-entrantly without a saved interrupted
  context would corrupt the running thread and cannot support blocking.
- **Fabricating specific SIF register values from the emulator.** Rejected as
  evidence: only the semantic bits (IOP initialized) are modeled, and they
  are labeled as such.

Consequences and limits:

- The SIFCMD init handshake completes; the RPC layer then binds and calls.
  **The model IOP does not answer RPC packets yet**, so `sceSifBindRpc`
  blocks on its semaphore and the run stops at a clean
  `NoRunnableThread` boundary — the recorded next wall.
- Interrupt delivery is only checked at unit boundaries (no asynchronous
  timer or external source); a handler that blocks suspends its thread with
  the interrupted context still on the deferred stack.
- The DMA is synchronous and local; no real IOP runs, and no SIF0 timing is
  modeled.
