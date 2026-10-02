# M30, nineteenth slice — the file item records, pinned as far as the evidence goes

Date: 2026-10-02. Inputs: the pinned ISO. Follow-up to the eighteenth slice
(`m30-slice18-gt4-volume-reader.md`), whose reader resolves names and sizes
but deliberately does not interpret a file entry's item records. This slice
probes those records. **No model behavior changed and no parser was
shipped**: three candidate grammars were tested against the pinned archive
and none of them closes, so the reader keeps its documented limit.

## What the records are

`mv0010`'s item list begins with a clean run of **97 three-word records**
whose first word is a name pointer (the tag-0 table at 0xCBxx):

```
name 0x0000CB13 "mv0011"  x 0x00034484  y 0x0003E640
name 0x0000CB1A "mv0012"  x 0x00034501  y 0x17104004
name 0x0000CB21 "mv0013"  x 0x0006270A  y 0x01540004
name 0x0000CB28 "mv0014"  x 0x0006518B  y 0x00CF0004
name 0x0000CB2F "mv0015"  x 0x00066B6C  y 0x01200004
name 0x0000CB36 "mv0016"  x 0x00068F6D  y 0x170E0004
...
```

- The names are consecutive (`mv0011` .. `mv0107`), each 7 bytes further in
  the name table, so the three-word stride is real for this run.
- `y` looks packed: most values carry 0x04 in the low byte and a size above
  it (`0x17104004 >> 8` = 1,511,488; `0x01540004 >> 8` = 87,040), while a
  few do not (`0x0003E640`, `0x0000B4B0`), so the packing is not simply
  `size << 8 | tag`.
- The entry's count (204,931) and value (18,874,372) are consistent with
  *items* and a *byte size*: 204,930 items and 18.9 MB of data.

After the 97th record the stream changes shape: one-word values that are
**valid tree entries** (0x15C0 = "arcade", 0xA1B4 = "CarSelectionRoot.gpb",
0xA1C4, 0xA1D4, ...), name pointers from the tag-1/tag-2 tables
(0x0100BEE8, 0x0200CAD5, 0x0200CF18, ...) and small values
(0x00000007, 0x0000003D, 0x00000FEC, ...). So a file entry's items mix at
least three kinds: entry references, name references and the three-word
records.

## The grammars tested, and why none closes

| Rule tried | Result |
| --- | --- |
| All items are three-word records | breaks at record 97 (`0x0000A1B4` is not a name) |
| Tag-0 names are three-word records, tag-1/2 names are three-word records, valid entries one word | 187,055 records, 627 references, 17,248 unknown; the walk ends at 0x23A738 where the flat list ends at 0x0CD1C4 — the cursor drifts |
| Tagged names are one-word references, tag-0 names are three-word records, valid entries one word | 190,839 name references, 2,070 records, 11,411 unknown; ends at 0x0D1270 — worse drift |

The drift means at least one item kind consumes a different number of words
than the tested rules assume. The unknowns (values that are neither valid
entries nor printable names, e.g. 0x00000007, 0x00000FEC) are the visible
symptom: they are almost certainly *fields* of a record whose boundary the
walk has already mis-taken.

## What this means for the PCDV answers

The engine's loader does not need the whole grammar at once: it asks for a
*name* and reads *sequentially* from the position the IOP returns (the
retry loop at 0x004B1C70 uses the descriptor's position at +2 plus the
task's offset, and the reads are 2048-byte sectors into the EE). The
three-word records that are pinned (the `mv00xx` run with `x` = position
and `y` = packed size) are exactly the shape those answers need, and the
*entry references* by offset and by name are one word each. The next slice
should pin the record boundary by reading the *game's own* consumer — the
PCDV library's scan and the engine's use of the descriptor — rather than
guessing further from the bytes, and then answer the first PCDV lookup and
read for `/mpeg`.

## Verification

- No code changed; the suite stays green: CTest **34/34**, Python 73 (67
  run, 6 skip), and `gt4boot --compare-interpreter --disc <iso>` at 3,000
  services with the interpreter reference at 7,570,583 instructions and the
  full state identical.
