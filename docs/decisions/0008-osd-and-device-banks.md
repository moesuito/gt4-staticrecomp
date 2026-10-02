# 0008 — The OSD configuration and the register-bank device model

Status: implemented 2026-10-02 for the M30 sixth slice
(`docs/reverse-engineering/m30-osd-and-the-iop-wall.md`).

Context: the boot read the OSD configuration (GetOsdConfigParam 0x4B, and its
probe writes it back through SetOsdConfigParam 0x4A) and then reached the
SIF initialization, which reads the DMAC status (0x1000E010) and the SIF0
channel control (0x1000C000) before calling SifSetDChain (0x78).

Decision:

- **The kernel holds the OSD configuration word** (ConfigParam, the public
  ps2sdk bitfield: spdifMode, screenType, videoOutput, japLanguage,
  ps1drvConfig, version, language, timezoneOffset). `GetOsdConfigParam`
  writes it to the guest address; `SetOsdConfigParam` stores what the guest
  wrote, **retaining every field**. That retention is exactly what the SDK's
  probe at 0x005B7620 tests: it writes `version = 1` and reads it back to
  tell a late kernel (retained) from an early Japanese one (always 0). The
  model answers "late kernel", matching the pinned USA BIOS (SCPH-90001).
- **The initial word is a documented USA default** (`0x00012011`: SPDIF
  disabled, 4:3, RGB, non-Japanese, version 1 = OSD2, English, GMT). The
  fields are not exercised by the verified path; only the retention bit is
  evidence-backed. A later slice that uses language/aspect must pin the
  values against live evidence.
- **Device windows became a list.** `GuestMemory::map_mmio` appends a window
  instead of replacing one; the timer, DMAC (0x1000E000, 0x100 bytes) and
  SIF0 channel control (0x1000C000, 0x100 bytes) blocks each get their own.
  DMAC and SIF are plain `RegisterBank`s (32-bit storage, untouched reads
  zero); `TimerUnit` now sits on the same bank class.

Alternatives considered:

- **Fabricate the IOP-side SIF status** (set the CMDINIT flag so the SDK's
  wait does not spin). Rejected: the register values and the IOP protocol
  are unverified; a guessed value would silently change the boot path.
- **Treat SetOsdConfigParam as a no-op** (rejected: the probe's whole point
  is the write/read round trip, and the kernel contract is storage with
  validity until reset).
- **Read the initial OSD word from the M14 RAM dump** (an attempt: the
  candidate pattern scan over the kernel area found 282 plausible words with
  no way to identify the config block; rejected as evidence — the default is
  labeled a model value instead).

Consequences and limits:

- The boot now reaches the SIF initialization. `sceSifInitCmd` (the game's
  0x005AE080) enables the SIF0 channel, registers its DMA handler, then
  either sends a command or **waits for the IOP's CMDINIT flag** — with no
  IOP model that wait spins forever, so **SifSetDChain (0x78) is the
  boundary**: the run stops there with the full state compared and recorded.
- The IOP interface (SIF registers with real semantics, SIF DMA submission
  and completion, the RPC layer) is the next milestone-sized subsystem; its
  decision will start from the public `sifcmd.c`/`sifdma.h` sources and the
  live emulator as the oracle.
- Device register banks have no side effects; any code that depends on one
  (a set-then-read status flip, a counter) stops at the boundary where the
  value must change and does not.
