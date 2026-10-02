# M30, eighteenth slice — the GT4.VOL reader

Date: 2026-10-02. Inputs: the pinned CORE and the pinned ISO. Follow-up to
the seventeenth slice (`m30-slice17-archive-path.md`), which decoded the
archive's format. This slice implements the reader in the library
(decision 0018) and verifies it against the pinned volume: the boot's movie
path (`/mpeg`) can now be resolved to names and sizes.

## What the reader does

`Gt4Volume` (include/gt4recomp/gt4_volume.hpp,
src/executable/gt4_volume.cpp) parses:

- the header: magic 0xACB990AD, version 0x00020002, three values, the
  name-table pointer, the child count and `count - 1` child offsets at
  +0x20 (the header counts itself, exactly like an entry);
- entries `{name pointer, count, value}` followed by `count - 1` item
  words; directories list children by entry offset; a file's `value` is its
  byte size;
- names as the archive's text XOR 0xFF (NUL terminated, three pointer
  tables tagged 0x00/0x01/0x02).

Parsing is lazy (children on demand, with entry/children caches and a
rejected-items set) and validated: a child candidate must carry a known
name tag and decode to printable text, or it is skipped. The metadata
window (64 MB) is read once; `DiscFileSliceSource` presents the ISO's
`GT4.VOL;1` as the byte source.

## Verified against the pinned archive

```
header: version 0x00020002, values 0xE9A1/0xBA60/0x2F41, count 23
root (22 entries): advertise, bgm, car, character, config, crs, database,
                   fep, font, icon, menu, mpeg, music, narration, projects,
                   race, rtext, script, sound, specdb, text, tire
mpeg        -> one child: gt4
mpeg/gt4    -> mv0010 among 33 children
mv0010      -> value 0x01200004 (18,874,372 bytes; count 204,931)
advertise/gtloading.img -> value 0x21A0 (8,608 bytes)
```

The counts and values match the earlier probes exactly (the root's count
field is 23 = the 22 children plus the header itself; `mv0010`'s count
204,931 is its chunk count and its value the movie's byte size).

## The limit this slice records

A file entry's items are *data records*, not child offsets, and the two
kinds are not yet distinguishable by inspection: among `mv0010`'s 204,930
items, 671 values coincide with real entries (e.g. 0x15C0 = "arcade",
0xA1B4 = "CarSelectionRoot.gpb") while the rest are names and offsets of
the movie's chunks. The reader therefore exposes `children()` for
directories and reports file names/sizes, and *does not* interpret a file's
records. Pinning those records (the {name, offset, packed} shapes seen at
0x4FAC+) is the next slice's first task, together with the PCDV answers
that need the data offsets.

## Verification

- CTest **34/34** (the new `gt4_volume` test: the synthetic volume's
  header, names, child classification and lookups; the pinned archive's
  size/extent, the 22 root categories, the `mpeg/gt4` chain and the movie's
  size); Python 73 (67 run, 6 skip).
- `gt4boot --compare-interpreter --disc <iso>` at 3,000 services:
  interpreter reference at 7,570,583 instructions, full state identical.
