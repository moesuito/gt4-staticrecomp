# 0010 — The model IOP's RPC layer, idle VBlank delivery and the peripheral windows

Status: implemented 2026-10-02 for the M30 eighth slice
(`docs/reverse-engineering/m30-slice8-rpc-and-vblank.md`).

Context: the seventh slice ended at the RPC bind wait: the game's SIF
manager binds its IOP servers and waits for replies the model did not send.
Behind that wait stood the game's whole runtime: a second SIFCMD handshake
after an IOP reset, the RPC layer the game's loading and display threads use,
and — once those run — the game's periodic tick, which the model had no
source for. This record covers the choices that carry the boot from the bind
wait into the running game.

Decision:

- **RPC bind and call replies.** When the game sends `SIF_CMD_RPC_BIND`, the
  model IOP creates (or reuses) a server slot and answers with the 64-byte
  `SifRpcRendPkt_t` end packet: the request's own handles echoed back plus a
  non-null server handle and the model's receive and connection buffers.
  Calls (`SIF_CMD_RPC_CALL`) are answered the same way, with the result
  bytes copied into the caller's receive buffer. The packet layouts were
  confirmed against the live transfers (sid at +32, cd at +28, recvbuf at
  +40, sd at +52) and the public `sifrpc-common.h`.
- **The model IOP's function table.** One behavior is evidenced so far: the
  game's first server (sid 0x80000001) answers its version query (RPC number
  0xFF) with eight bytes; the second word is 2 — the value the game's client
  checks, taken from its own code — and the first word is the game's
  compatibility constant at 0x0066829C (0x00275520), which the game compares
  its IOP version against. Anything else answers an empty result. The real
  value comes from the IOP module binary the model does not execute; the
  constant is where the game's own check says it must match.
- **IOP reset.** `SIF_CMD_RESET_CMD` records the requested image path (the
  game sends `rom0:UDNL cdrom0:\IOPRP300.IMG;1`) and completes the modeled
  reboot: because the game overwrites SMFLAG with SIFINIT/CMDINIT right
  after sending, the model announces SIFINIT|CMDINIT|BOOTEND before the
  first SIF register read that follows. That is exactly the bit the game's
  `SifIopSync` poll waits for.
- **Extended OSD configuration.** `SetOsdConfigParam2` (0x6E) and
  `GetOsdConfigParam2` (0x6F) move a four-byte block with the caller's size
  and offset: format, the clock/date bits (daylight savings, 12/24-hour,
  date format), version 2 and the true language. Reads past the block are
  zeros and writes past it are dropped, matching the reference emulator.
- **GS and system services.** `GsGetIMR` (0x70) and `GsPutIMR` (0x71) keep
  the 64-bit interrupt mask; Put returns the previous value. `SetGsCrt`
  (0x02) is accepted with no state — the model has no display.
- **Memory regions.** `GuestMemory` now holds several byte-addressable
  regions. The EE's 16 KiB scratchpad at 0x70000000 joins the 32 MiB main
  RAM, and the GS register block at 0x12000000 (8 KiB) is modeled as
  storage: the model stores what the guest writes, including the 64-bit
  register writes the 32-bit banks cannot hold, and returns it on reads. No
  GS behavior (drawing, masks, read-back semantics) is emulated.
- **Peripheral windows.** The GIF, VIF0, VIF1 (and their FIFOs), the IPU and
  its FIFO, the VIF0/VIF1/GIF DMA channels, the SPR DMA window, INTC and SIO
  are register banks: 32-bit storage, untouched reads zero. No transfer
  engine is modeled, so a channel never reports busy and a FIFO never fills.
- **Idle VBlank delivery.** When a service reports that no thread can run,
  the driver asks the kernel for one idle interrupt: the kernel raises
  VBlank (INTC cause 2), sets the INTC status bit the handler reads, and
  injects **every registered handler for the cause in registration order**,
  each returning through the model's stub. The return re-dispatches: the
  handler usually woke a thread, and the scheduler runs it; when nothing was
  woken, the budget (60 consecutive idle interrupts) stops the model instead
  of spinning forever.
- **PCCR (CP0 register 25) stores.** The EE performance counters are storage,
  like the timers of decision 0007: the model does not count cycles, so the
  game's frame-time accumulators stay zero. Counting instructions would
  differ between the translated module and the interpreter and break the
  differential comparison.
- **The VU1 bits of FBRST are recorded, not fatal.** The earlier policy
  stopped at `ctc2 FBRST` when the VU1 control bits were written (M22).
  GT4 resets VU1 as part of its display setup. Since the model executes no
  VU1 microcode, "resetting VU1" has no target state: the bits are stored
  and the VU0 reset bit keeps clearing the VU0 register file.

Alternatives considered:

- **Fabricated RPC results per server.** Only the version query has evidence
  for its expected answer; other calls answer empty results and the game's
  reaction is observed instead of invented.
- **A cycle-counting VBlank source.** Counting instructions cannot match
  between the translated module and the interpreter, and would break the
  differential comparison; the idle-triggered source is deterministic and
  semantically what the hardware does (the console idles until the frame
  ends).
- **Continuing to stop on the VU1 FBRST bits.** The stop was right while
  VU1's role was unknown; with GT4's use evidenced, a stop only blocks the
  boot without protecting any verified behavior.

Consequences and limits:

- The boot now runs into the game's runtime: 3,000 services handled, the
  interpreter reference at 7,515,389 instructions, full state identical.
  The game's threads tick under VBlank-driven wakeups.
- The model IOP still answers nothing else: every other RPC function returns
  an empty result, and the file/cdvd loading the game will attempt is not
  yet served. The recorded next wall is the first such call.
- Interrupts are delivered only when the machine goes idle; a long-running
  guest computation never sees a VBlank.
- The 60-interrupt budget is a guard, not a modeled frequency.
