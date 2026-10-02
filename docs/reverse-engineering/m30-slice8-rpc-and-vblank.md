# M30, eighth slice — the IOP's RPC layer, peripheral windows and VBlank delivery

Date: 2026-10-02. Inputs: the pinned CORE. Follow-up to the seventh slice
(`m30-sif-and-interrupt-injection.md`), which stopped at the RPC bind wait
(no-runnable-thread at 0x005ADCE4). This slice answers the RPC layer, maps
the peripheral windows the boot walks through, and gives the machine its
first periodic interrupt source. The result: the boot reaches the game's
running state — 3,000 services handled, the interpreter reference at
7,515,389 instructions, full state identical.

## The packets the game actually sends

The model IOP logged every transfer that reached its command buffer; the
layouts match the public `sifrpc-common.h` structs field for field.

`SIF_CMD_RPC_BIND` (0x80000009), 64 bytes:

```
+00: 00000040 (psize 64)   +04: 00000000 (dest)
+08: 80000009 (cid)        +12: 00000000 (opt)
+16: 00000005 (rec_id)     +20: 20886a40 (pkt_addr)
+24: 00000002 (rpc_id)     +28: 008899c0 (cd)
+32: 80000001 (sid)        +36..: residual bytes of the reused packet
```

`SIF_CMD_RPC_CALL` (0x8000000A), 64 bytes, with the send data's destination
in the header (`dsize = 8`, `dest = 0x000B0000`, the model server's buffer):

```
+32: 000000ff (rpc_number)  +36: 00000008 (send_size)
+40: 00888f00 (recvbuf)     +44: 00000008 (recv_size)
+48: 00000001 (rmode)       +52: 000a0000 (sd)
```

`SIF_CMD_RESET_CMD` (0x80000003), 104 bytes: arg length 32 at +16, mode at
+20, then `rom0:UDNL cdrom0:\IOPRP300.IMG;1` — the game's `SifIopReboot`
string, `"rom0:UDNL " + the IOPRP path` exactly as the public
`ee/kernel/src/iopcontrol.c` builds it.

## The walls, in order, with their evidence

1. **RPC bind wait.** Answered with the `SifRpcRendPkt_t` end packet: psize
   64, cid `SIF_CMD_RPC_END` (0x80000008), the request's rec_id/pkt_addr/
   rpc_id/cd echoed, cid = the request's cid at +32, and a non-null model
   server handle plus buffers. The game's client then signals its semaphore
   and proceeds.
2. **First RPC call** (sid 0x80000001, RPC number 0xFF, 8 bytes in and out).
   The game's client code at 0x005B2A80 reads the reply, copies the first
   word to 0x008999E8 and sets a flag from `word[1] == 2` (0x005B2ACC), and
   later compares the copied version against the constants at 0x0066829C
   and `[0x00668350]` (0x005B2AF8). The model answers `{0x00275520, 2}`,
   the game's own compatibility constant (read from the pinned image).
3. **IOP reset.** The game sends the reset, re-writes SMFLAG
   (SIFINIT then CMDINIT) and polls `SMFLAG & BOOTEND` in `SifIopSync`
   (0x005B7148). The model records the image and completes the reboot before
   the first SIF register read after the reset.
4. **GetOsdConfigParam2** (0x6F) at 0x0058D088: the game reads **one byte at
   offset 1** and extracts the daylight-savings bit (used at 0x0058D480 to
   add 60 minutes to the timezone), the 12/24-hour bit and the date-format
   bits. The model's four-byte block follows the reference emulator's
   `Config2Param` (format, clock/date bits, version 2, true language) and
   drops/zero-fills accesses outside it.
5. **Second bind and call** (sid 0x80000592) and the **post-reset re-init**:
   the game re-runs the SIFCMD handshake, rebinds both servers and re-queries
   the version, then creates its worker threads (entries 0x005AE9A0 and
   0x005786F0) and hands the boot to them.
6. **GS register fault** at 0x12001000 (pc 0x0049FEE8): the display setup
   writes and reads back 64-bit GS registers. The GS block (0x12000000,
   8 KiB) is modeled as storage.
7. **GIF/VIF faults** at 0x10003000–0x10003FFF (pc 0x0049FF38, including
   `ctc2 FBRST` with the VU1 reset bit): the register windows become
   storage banks and FBRST records the VU1 bits instead of stopping.
8. **The idle state.** GetThreadId (0x2F) and SleepThread (0x32) lead to no
   runnable thread. The kernel dump showed the three threads waiting: main
   (sleep), the RPC event thread (semaphore), and the loader thread (sleep).
   The game had registered INTC handlers for causes 0, 1, 2, 5 and 11.
9. **The VBlank handler** (0x004AB430) reads CP0 register 25 (PCCR) and
   clears it twice, accumulates the values, reads a GS status bit, and
   increments a frame counter at 0x70002000. PCCR becomes storage.
10. **Chained handlers.** The first cause-2 handler (0x004AB6D8) alone woke
    nothing; the kernel calls every handler registered for the cause in
    turn, and with the chain (0x004AB6D8 then 0x004AB430) the game's threads
    woke and the boot proceeded.

## The idle VBlank model

When a service reports no runnable thread, the driver asks the kernel for an
idle interrupt. The kernel raises VBlank (INTC cause 2), sets the INTC status
bit, saves the interrupted context, and installs the first registered
handler's frame; each handler returns through the private stub, whose service
chains to the next handler and finally re-dispatches. If the handlers woke a
thread, it runs; if not, the model delivers at most 60 consecutive idle
interrupts and then reports the no-runnable-thread boundary. The differential
reference implements the same policy, written separately.

## Verified run

```
build/gt4boot.exe private/fingerprint-check/CORE.GT4 --services 3000 --compare-interpreter
...
boundary: syscall 0x005adce4 service 0x44
stats: module calls 9082, interpreted steps 285290, services handled 3000
interpreter: 7515389 instructions, state identical (registers, HI/LO, FPU, VU0, CP0, pc, memory digest)
```

The service trace at the end shows the runtime cycle: `WaitSema`/`SleepThread`
from a game thread, the injected `0x100` handler-return stub, the VBlank
handler's `iGetThreadId`/`iPollSema`/`WakeupThread`, and back.

CTest **32/32** (`gt4boot_services` now runs `--services 3000` with the
interpreter comparison; `ee_kernel` covers the handler chain, the idle
VBlank, its INTC status bit and the delivery budget); Python 73 (67 run,
6 skip).

## Limits recorded

- The model IOP answers only the version query; every other RPC function
  returns empty results. The first call that expects real data (the game's
  file/cdvd loading) is the recorded next wall.
- Interrupts are delivered only at idleness; no periodic tick interrupts a
  long-running computation.
- No transfer engine, FIFO behavior, GS behavior or display is modeled; the
  windows and the GS block are storage.
- The performance counters do not count; frame-time accumulators stay zero.
- The VBlank budget (60) is a guard, not a modeled frequency.
