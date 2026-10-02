# M30, sixth slice — the OSD configuration and the IOP wall

Date: 2026-10-02. Inputs: the pinned CORE. Follow-up to the fifth slice
(`m30-timer-and-interrupts.md`): the boot stopped at GetOsdConfigParam (0x4B)
while reading the console's settings. This slice models the OSD configuration
and the DMAC/SIF register blocks, and stops at the IOP wall.

## What changed

- **The OSD configuration** joins the kernel: `GetOsdConfigParam` (0x4B)
  writes the ConfigParam word to the guest address and `SetOsdConfigParam`
  (0x4A) stores what the guest wrote, retaining every field. The SDK's probe
  at 0x005B7620 writes `version = 1` and reads it back to distinguish a late
  kernel from an early Japanese one; the model answers "late kernel" for the
  pinned USA BIOS. The initial word is the documented USA default
  `0x00012011` (SPDIF disabled, 4:3, RGB, non-Japanese, OSD2, English, GMT),
  labeled a model value; decision 0008 records why the fields are not pinned
  yet.
- **Multiple device windows**: `GuestMemory::map_mmio` now appends windows
  instead of replacing one, and `RegisterBank` (32-bit storage, untouched
  reads zero, non-32-bit widths rejected with the address) carries the DMAC
  block (0x1000E000, 0x100 bytes) and the SIF0 channel control
  (0x1000C000, 0x100 bytes). `TimerUnit` was refactored onto the same bank.
- The boot reads DMAC status `D_STAT` (0x1000E010) and the SIF0 `CHCR`
  (0x1000C000) as zero — "no IOP activity, channel stopped" — and proceeds.

## The verified run

```
build/gt4boot.exe private/fingerprint-check/CORE.GT4 --services 600 --compare-interpreter
...
boundary: syscall 0x005ae084 service 0x78
stats: module calls 68, interpreted steps 17696, services handled 77
interpreter: 6321377 instructions, state identical (registers, HI/LO, FPU, VU0, CP0, pc, memory digest)
```

- The run passes the whole `_InitSys` tree, the game's thread creation (the
  cooperative scheduler switches as in the fifth slice), the crt0
  continuation, the deeper game initialization (all the way through
  ~6.3M interpreted instructions in the reference) and stops at
  **SifSetDChain (0x78)** at pc 0x005AE084.
- The state is identical in every compared field; `gt4boot_services` now
  pins this frontier (`--services 600`, 77 handled).

## The IOP wall, with evidence

- The stop is inside the SIF initialization. The public ps2sdk `sifcmd.c`
  `sceSifInitCmd` shows the exact shape: read `DMAC_COMM_STAT` (0x1000E010)
  and clear the SIF0 bit; if the SIF0 `CHCR` (0x1000C000) is not started,
  call `sceSifSetDChain` (syscall 0x78) — the stop; then
  `AddDmacHandler(DMAC_SIF0, ...)`, `EnableDmac`, read
  `SIF_SYSREG_SUBADDR`; if the IOP has not initialized yet, **spin on
  `SIF_REG_SMFLAG & SIF_STAT_CMDINIT`**.
- With no IOP model, that spin is unbounded, so `SifSetDChain` is the right
  stopping point: the run reports a clean boundary instead of looping. The
  game's own 0x5AE080 matches the SDK source line for line (the D_STAT read
  with bit 0x20, the CHCR check with bit 0x100, the AddDmacHandler call).
- The next subsystem is the IOP interface: SIF registers with real semantics
  (SMFLG/MSFLG handshakes), SIF DMA submission and completion, and the RPC
  layer the game builds on top. The public SIF sources and the live emulator
  (PCSX2, already configured) are the evidence sources.

## Limits recorded

- OSD field defaults are model values; a later slice that reads language or
  aspect must pin them against live evidence.
- Device banks are pure storage: no side-effect bits, no counters, no DMA
  completion. Code that depends on one stops at the boundary where the value
  would have to change.
- The `SifSetDChain` semantics (enabling the SIF0 channel: CHCR = 0x184 per
  the public header) are not modeled yet; the DMAC unit will own that write
  when the IOP slice needs it.

## Evidence

- CTest **32/32** (the `ee_kernel` test covers the OSD round trip and the
  version-retention probe; `ee_timer` covers two coexisting device windows
  and the register bank); Python 73 collected (67 run, 6 skip).
- The differential reference reaches the same stop after 6,321,377
  instructions with the full state identical.
