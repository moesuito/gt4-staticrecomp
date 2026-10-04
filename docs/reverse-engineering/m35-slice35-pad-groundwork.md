# M35, thirty-fifth slice — pad groundwork: vocabulary mapped, spec deferred

Date: 2026-10-03. Inputs: the pinned CORE (data-record strings,
whole-text byte scans, disassembly), standing leg evidence
(slices 1/16/24/29). Read-only throughout: no product change, no
instruments, no legs needed (prior inventories cover all
frontiers). Charter: survey pad vocabulary + consumer side, then
spec a neutral-present model or justify deferral.

## Verdict: phase-gated, spec deferred (justified)

The survey does not support even a neutral-present spec: no pad
server sid, no pad RPC numbers, and no bind site exist anywhere
reachable. The exact promoter is named below; the existing
`--threads` sid inventory is already the tripwire, so nothing new
needs building.

## Vocabulary (Confirmed — data-record reads with contexts)

- IOP module load list at `0x0068BC80`: `pad2/ds2u_d.irx`,
  `sio2man.irx`, `mcman.irx`, `mcserv.irx`, `sio2d.irx`,
  `dbcman.irx`, `libsd.irx`, `usbd.irx`, `inet.irx`,
  `netcnf.irx`, `inetctl.irx`, `dev9.irx` (+ `-no_decode`,
  `-no_auto` flags).
- SDK versions: `PsIIlibpad2 3020`, `PsIIlibvib 3020`,
  `PsIIlibmc 3020`; libpad2 alignment error string
  (`0x006CF398`: "buffer addr is not 64 byte align").
- Controller type names at `0x006A65E8`: `CONTROLLER`,
  `JOYSTICK`, `SINGLESHOCK`, `DUALSHOCK`, `DUALSHOCK 2`,
  `COUGAR`, `JAGUAR`, `CHEETAH`; wheel scope via `GTForce` +
  `OptionRaceInput` strings nearby.
- IOP side is UP: the slice-16 disc walk already served real sizes
  for SIO2MAN/MCMAN/MCSERV/SIO2D/DBCMAN/DS2U_D/LIBSD/USBD — the
  pad-capable IOP stack loads in early boot.

## Consumer side (Confirmed present-but-unreached / Unknown)

- `0x0043CE08`: controller identification by name — walks the
  device-name list with `jal 0x0057F238` (strcmp shape) per entry
  (`0x006A6618/28/38/40…`), argument in s0. Zero direct `jal`
  sites in all text (whole-text byte scan): indirect-only, like
  every other late-phase entry point in this project.
- Pad sids (`0x80000100/101/102`): zero literal words in text and
  data — but calibration scans show even *bound, active* sids
  (SIFMAN, fileio, PCDV, PRTS) never appear literally either:
  sids are constructed dynamically, so absence proves nothing
  either way (method limit recorded, not fudged).
- Pad RPC numbers: unrecoverable statically (no bind/call sites,
  no stubs shapable without the protocol). Unknown.
- Parked boot (all frontiers): 24 bound sids, none pad; zero SIF
  pad traffic; no pad waiter (pad is polled — it needs no waiter,
  only a reader that never asks).

## Why deferral, and the exact promoter

A neutral-present server needs, minimum: its sid, its version
answer, and its RPC dispatch shape. All three are Unknown — a
registration now would invent the protocol (evidence discipline
forbids it, and decision 0026's fabrication rule applies
verbatim). The promoter is precise: the first EE-side padman
bind, i.e. sid `0x80000100`/`101` appearing in a future leg's
`--threads` server inventory, expected from the menu/race input
phase that owns the identify function and the wheel strings. No
new tripwire is needed — the inventory listing from decision
0023 already prints every bound sid permanently.

## Grades

- Confirmed: every string, address, table, and the identify
  function's shape/callerless state above.
- High confidence: IOP pad stack up, EE pad side unreached;
  deferral is correct (spec would be invention).
- Unknown: pad RPC numbers; which phase binds first (menu vs
  race boot); the identify function's s0 contract.

## Verification and hygiene

- No code touched, no instruments written, no runs made, no
  rebuild needed.
- No scratch (nothing created outside this doc).
- Gates unaffected; review + commit per rules.

Next (recommended): milestone event work per decision 0023; when
the sid inventory shows padman, the spec slice writes itself —
sid + version query from the bind handshake + the RPC table from
the game's own call sites, exactly the decision-0026 playbook.
