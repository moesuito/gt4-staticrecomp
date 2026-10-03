# M33, slice 27 — the write-watch leg: total silence, probe verified

Date: 2026-10-03. Inputs: the pinned CORE and ISO; three `gt4boot`
legs from `build/ckpt-1980k.bin` (2,000 services each) with a
temporary two-word write watch, since removed; stop-time `--dump`
reads. No model change: the watch was purely observational and every
line of it is reverted.

Charter: watch `[0x006207F4]` (main's flag word) and `[0x0087E190]`
(the job loop's request slot, `0x0087E180+0x10`) for any poster. Any
writer names itself by pc (trace next); total silence returns the
main-unpark question to decision 0023's async-event framing.

## Verdict: total silence, with a verified probe (Confirmed)

Final leg (identical run shape to every prior parked leg: limit-hit,
2,000 module calls, 184,001 interpreted steps, 2,000 services all
`0x100`):

- `TEMPORARY watch hits: 0, dropped: 0` — zero stores touched either
  word across the whole leg, and the 64-hit cap never engaged.
- `TEMPORARY watch selftest: PASS` — see below.
- `--dump` confirms both words still 0 (`006207f4: 00000000`,
  `0087e190: 00000000`).

No poster exists in the parked machine: not to main's flag, not to
the loop slot. The park is total. The main-unpark question returns to
M32's async-event framing (decision 0023): the missing second flag
post needs originating IOP/pad/USB traffic only milestone work
provides.

## The watch design (all reverted; recorded so the next probe reuses it)

- Hook: one `note_temporary_store(physical, width, address)` call at
  the end of each of the five RAM store paths (`write_byte`,
  `write_halfword`, `write_word`, `write_doubleword`, `write_bytes`;
  MMIO windows untouched). Verified by grep that no other code
  writes `region.bytes` directly, and that `restore_memory` funnels
  through `write_bytes`.
- Alias-correct: overlap is tested on the physical range against
  physical targets (both targets `< 0x20000000`, so identity; a KSEG0
  write like `0x81000102` is caught — proven by the selftest).
- pc attribution: a temporary global set by the driver at each
  module entry (outermost-function granularity for translated runs)
  and before each interpreter step (exact pc). Kernel/model stores
  during a service attribute to the triggering guest pc — noted, not
  a confounder here (zero hits).
- Read-back is guarded: `contains(target, 4)` first (mapping only),
  then `read_word` — the targets are word-aligned by construction,
  so the slice-24 unaligned-read crash class cannot recur.
- Hits capped at 64 with a dropped counter; the watch arms after the
  resume restore so snapshot bytes never log.

## Probe verification (why the zero counts)

A zero from an unverified probe is worthless, so the leg carries its
own control: after the leg report, `gt4boot` exercises scratch memory
through all five hooked paths (word, byte-into-word, KSEG0-aliased
halfword, doubleword, bulk) with a marker pc `0x12345678` and checks
exact pcs, addresses, widths and read-back values
(`0xAABBCCDD / 0x00001100 / 0x2233CCDD / 0x55667788 / 0x5566EFBE`).
Result `PASS` — the hook fires on every path, aliases resolve, values
are exact, nothing is dropped. One selftest bug of mine (an 8-wide
write at `...1FC`, caught by the model's own alignment check before
any hook ran) was fixed by moving it to the aligned `...200` with
recomputed expectations; the leg evidence was unaffected throughout.

## Grades and limits

- Confirmed: zero stores to either word over 2,000 services with a
  PASS-verified probe; both words still 0 by dump; run shape
  bit-identical to uninstrumented legs (no perturbation).
- High confidence: the park is total — with VBlank/timer handlers
  running every idle tick and still zero stores, no in-model source
  can post; only outside-world traffic (decision 0023's ranking:
  async IOP, then input, then GS-side) can break it.
- Unknown (unchanged): who the poster will be once traffic exists;
  the break-trap codes; the dispatcher's WakeupThread targets.

## Verification and hygiene

- All 152 temporary insertions across 4 files reverted via checkout
  (`ee_state.hpp`, `state.cpp`, `driver.cpp`, `gt4boot/main.cpp`);
  `TEMPORARY` grep-clean in source (0 hits) and relinked
  `build/gt4boot.exe` (`TEMPORARY watch` absent); working tree holds
  only this doc plus the journal appendix.
- Three scratch logs deleted after extraction.
- Gates left for review (no commit per slice rules).

Next (recommended): close the M33 recon line as answered (dry pipe,
unbuilt job system, printer-owned feeders, neutral handlers, silent
watch) and return to milestone work per decision 0023 — the next
originating event the model should produce. If a cheap stimulus is
wanted first, script the lowest-ranked deliverable stimulus from
slice 22's table against the same watch: any hit at all reopens the
trace with a named writer pc.
