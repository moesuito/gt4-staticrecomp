# In-Game Settings Menus and Wheel Support — Feasibility

2026-10-14, slice 151. Read-only study; nothing implemented.

## In-game settings menus (retail build)

- Retail GT4 uses Adhoc bytecode version 5. Nenkai's GTAdhocToolchain compiles
  v5 only experimentally (v7, used by GT4 Online/Spec II, is well supported)
  and disassembles rather than decompiles. Menus are project/widget files plus
  scripts inside GT4.VOL. No documented retail menu mod was found; Spec II's
  menus are on the Online build and cannot be reused directly.
- Adhoc native methods are registered by name at static init (gt4-decomp
  `knowledge/script-engine.md`, `config/adhoc_methods.txt`). A new native,
  recognized by our runtime, is the clean bridge from a menu item to a host
  setting. This port does not need code injection for it.
- Our runtime has no loose-file overlay yet; one at the disc-read boundary would
  let new menus and texture packs live outside GT4.VOL (as GT4Hooks HostFS does
  on GT4 Online).
- Recommendation: host options (upscaling, VSync, texture packs, resolution)
  live in a launcher/config first; in-game menu items are a later addition
  through a v5 script change, a native bridge and the file overlay.

## Wheel support (Logitech G27 with force feedback)

- GT4 supports Logitech Driving Force Pro/GT Force-class USB wheels. PCSX2's
  `pcsx2/USB/usb-pad` (studied for facts only) documents the input report
  (buttons, 8-bit steering/throttle/brake) and the 8-byte force-feedback command
  set (download/play/stop effects, springs, dampers, friction, auto-centre,
  900-degree range command).
- The donor runtime only stubs OHCI registers (`runtime/src/iop/hw.cpp`); there
  is no USB device model. GT4 loads USBD, PDIUSB and USTORAGE ([IOP map](IOP-MAP.md)).
- Options: (a) OHCI plus a virtual Driving Force Pro, as PCSX2 does (largest);
  (b) high-level emulation at the PDIUSB/pad boundary: feed host wheel state and
  translate the game's force-feedback packets to SDL3 haptics (recommended);
  (c) native input after decompilation (force feedback still needs the game's
  output packets).
- Real wheels through SDL3 (G27, G29, Thrustmaster, Fanatec). Clutch and
  H-shifter are probably not used by GT4 (anecdotal).

## Current wheel target and research assignment (owner, 2026-10-26)

- Guest device target: **Logitech Driving Force Pro (DFP)**, 900-degree steering with the game's
  native wheel force-feedback commands. Earlier references to DFGT in task planning were a naming
  mistake; a physical DFGT may still be a host device through SDL3. Exact DFP descriptor/revision
  and negotiation remain research items, not an implementation claim.
- Planned release: **1.3.1**, subject to research and owner acceptance. 1.3.0 stays focused on
  Spec II and performance. The 1.2.5 adapted-input draft stays private, without publication.
- Reuse the existing SDL3 enumeration, capture, calibration, UI and safe output lifecycle where
  suitable. Replace the gamepad-state bridge and rumble-derived output for native wheel mode.
- Research-only contract: [TASK-123](tasks/TASK-123-DFP-WHEEL-RESEARCH.md). Identify the actual
  USBD/PDIUSB/LGDEV and EE/IOP paths before choosing a hook. Do not assume HLE is sufficient.
- Both game targets need independent evidence. No new wheel implementation is part of this update.
