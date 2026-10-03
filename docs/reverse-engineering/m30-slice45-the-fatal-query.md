# M30, forty-fifth slice — pin the divergent lookup: it asked for /fonts/system.fnt

Date: 2026-10-03. Inputs: the pinned CORE and ISO. Follow-up to the
forty-fourth slice (the odd pointer is derived by walking data as
pointers; the lookup designated the wrong object). This slice captures
the fatal lookup's query, verifies the file exists, and narrows the
divergence to the loaded object's offset tables.

## The captured query (temporary entry log, since removed)

A temporary entry hook on the five chain functions (driver loop top plus
interpreter pre-step, capped, with `a0`/`a1` and their memory) fired only
14 lines in the whole 15M-service run — these paths run rarely and fully
translated most of the time:

- Three healthy `0x00498B28` relocates of `Tex1` objects (early boot,
  from `0x00103B68`).
- One manager call `0x0048FB58` (`a0` = zeros + float `640.0`, `a1` =
  caller scratch).
- One fatal `0x00491798` relocate (`a0` = `0x009A9400`, an even `FT01`
  object, via the direct caller `0x0048FB9C`).
- No `0x00491D90`/`0x004AE1F8` entries: the fatal lookup ran entirely
  inside translated code.

The manager's `a1` (the query scratch) decodes to:

```text
2F 66 6F 6E 74 2F 73 79 73 74 65 6D 2E 66 6E 74 00
"/fonts/system.fnt"
```

The fatal lookup asked for **the system font file**.

## The file exists in the archive

`system.fnt` occurs (XOR-0xFF encoded, like all GT4.VOL names) in the
fonts name table on both layers, alongside `menu.fnt`, `race.fnt`,
`unicode.kf`, `jis2uni.dat` and the `*.kf` files. The lookup key is
valid; the archive holds the file.

## The mechanism, now complete

The FT01 object at `0x009A9400` (`+0x0` = magic, `+0x4` = 0,
`+0x10` = count 10, `+0x14/+0x18` = `0x30/0x58`) is relocated by
`0x00491798`: with old base 0 the delta is the base itself, so the
`0x30/0x58` fields are first "relocated" into self-pointers
(`0x9A9430/0x9A9458`) then walked as offset tables — the entries,
turned absolute by the same delta (e.g. offset `0x358B` → `0x9CF08B`),
lead into the heap data buffer of slice 44, and the sibling routine's
first load faults exactly at `0x009CF08F` (`0x9CF08B + 4`, matching the
reported address arithmetically). Low memory `0x0–0x100` is all zeros
(dumped at the fault), ruling out garbage tables there. One reporting
anomaly stays open and non-load-bearing: `a0` reads `0x9CF08F`, 4 above
the implied entry value.

## Verdict and next experiment (slice 46)

The chain from a valid query (`/fonts/system.fnt`, present in the
archive) to a faulting relocate is fully mapped; the remaining question
is whether the loaded FT01 object's offset tables (`+0x14/+0x18` and the
`+0x30…` entries) match the file on disc. Slice 46: trace the font
file's load (its PRTS/fileio reads near the fault), read the file's
header bytes from the image, and compare them against the live object —
a mismatch pins the loader/model divergence; a match points at lifecycle
timing (relocate before fixup). No model change is shipped here.

## Verification

- CTest 36/36 and Python 73 (67 run, 6 skip), green on the final tree.
- All temporary instruments (the chain-entry log in the driver loop and
  the interpreter step) are removed; the tree holds docs only.
