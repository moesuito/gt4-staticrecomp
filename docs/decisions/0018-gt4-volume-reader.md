# 0018 — The GT4.VOL archive reader

Status: implemented 2026-10-02 for the M30 eighteenth slice
(`docs/reverse-engineering/m30-slice18-gt4-volume-reader.md`).

Context: the boot streams its movie (`/mpeg`) through the game's own CD
driver, whose replies carry the archive's file table. The archive's format
was decoded in the seventeenth slice; this slice turns that evidence into a
library reader so the model can answer with real names and sizes.

Decision:

- **`Gt4Volume` reads the pinned archive** through a `DiscByteSource`
  (`DiscFileSliceSource` presents the ISO's `GT4.VOL;1` as one), parsing:
  - the header (magic 0xACB990AD, version, three values, the name-table
    pointer, a child count whose list holds `count - 1` offsets — the same
    rule as an entry's item list, which also counts the entry itself);
  - entries `{name pointer, count, value}`; a directory's items are child
    entry offsets, a file's `value` is its byte size;
  - names as the archive's text XOR 0xFF, NUL terminated.
- **Parsing is lazy**: the constructor reads the header and the root
  entries; `children()` materializes one entry's child list on demand, so a
  lookup pays only for the path it walks (the pinned tree is far larger
  than any one walk). The metadata window (64 MB) is read once; parsed
  entries and children lists are cached, and item offsets that fail to
  parse are remembered so data records are never retried.
- **Validation instead of guessing**: a child candidate must start with a
  name pointer whose table tag is one of the three the volume uses and must
  decode to printable text; anything else is skipped (a file's data
  records). The file item records themselves are *not* modeled yet: the
  reader reports names and sizes and stops there, as the evidence document
  states.

Alternatives considered:

- **Eager full-tree parsing**: the first attempt walked the whole archive
  and did not finish (millions of repeated candidate parses); lazy parsing
  with caches is the structure the data demands.
- **Treating every file item as a child**: the pinned movie's items include
  values that coincide with real entries (the probe found 671 such
  false positives among 204,930 items), so the item layout must be pinned
  before the reader may interpret them; the slice documents this instead of
  shipping a guess.

Consequences and limits:

- The reader resolves the root's 22 categories, directory chains
  (`mpeg/gt4`) and file sizes (`mv0010` = 0x01200004 = 18,874,372 bytes;
  `advertise/gtloading.img` = 0x21A0 = 8,608) against the pinned volume,
  and the same shapes against a synthetic image in tests.
- The file *data* offsets stay unknown until the file item records are
  pinned; the next slice's task (with the PCDV answers that need them).

Verification:

- CTest **34/34** (the new `gt4_volume` test: the synthetic volume's
  header, names, child classification and lookups; the pinned archive's
  size/extent, root categories, `mpeg/gt4` chain and the movie's size);
  Python 73 (67 run, 6 skip).
- `gt4boot --compare-interpreter --disc <iso>` at 3,000 services:
  interpreter reference at 7,570,583 instructions, full state identical.
