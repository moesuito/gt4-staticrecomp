# M32, seventeenth slice — the walk traced live: lottery confirmed

Date: 2026-10-03. Inputs: the pinned CORE and ISO; a pc-triggered trace
in the reference interpreter (temporary, since removed) over short legs
from `ckpt-1580k.bin`, plus fresh disassembly. No model change. This
slice watches the due test execute with live operands — and confirms
the lottery account in both directions while vindicating the field
mapping.

## The trace (Confirmed)

Every walk execution logs `(current, target, COUNT, overflow, head)`:

- 2,000 handler entries + 2,000 due tests per 2k leg (1:1, every
  service). The walk runs constantly — reachability settled for good.
- Target CONSTANT `0x0000010000024000`, head CONSTANT `0x00889f80`,
  overflow CONSTANT 101. The target equals `sched + base - acc`
  (`0x48000 + 0x000000FFFFFDC000 - 0`) by hand-checked hex addition —
  the slice-11 field mapping is vindicated exactly.
- Current matches `((overflow << 16) | COUNT) << 8` exactly at every
  sample — the formula is confirmed live, including the shift.

## Lottery, both directions (high confidence)

Firing needs combined within a razor edge below the 32-bit ceiling
(single-digit to ~17k ticks depending on node — `0x00889f80` sits
~3 ticks under). Per-frame COUNT advance is erratic (3k–170k per
service across legs), so consecutive walk runs step clean over the
edge except on freak slow alignments — which is precisely what the
slice-5 poke and slice-12 D9 caught. Nothing is broken; on hardware
the same arithmetic makes timeout firing a lottery there too. The
timeouts are vestigial as a completion path: real completion is
cancellation-plus-signal driven by each wait's actual event.

## Open: each wait's event (next)

The six waits are timeout-guarded event waits whose guards almost
never mature. Slice 18 maps the other half per worker: the dispatch
chain shared entry `0x005786f0` through `0x0057871c`/`0x005782d0`/
`0x00578518`/`0x00577fb8` reads as "issue request, then wait with
timeout" — disassemble what each family issues (SIF call? condition
check? job post?) to name the event whose arrival (or modeled arrival)
retires it.

## Verification

- All probe instruments removed (grep-clean); product code untouched.
- Deterministic prefixes reproduce; temporary logs deleted.
- Full gates run on the final tree before commit.
