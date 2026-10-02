# M30, twentieth slice — the game's own CD driver reads the disc

Date: 2026-10-02. Inputs: the pinned CORE and ISO; the external GT4FS
reference (github.com/Razer2015/GT4FS) as a cross-check. Follow-up to the
nineteenth slice (`m30-slice19-file-item-records.md`). This slice answers
the game's own CD read service and observes the boot's driver walking the
disc's file system for the first time.

## What the read service is

The PCDV server (sid 0x50434456) read request is
`{position, byte count, EE destination}`; the boot's first request is
`{0x10, 0x800, 0x0084E080}` and the library checks the returned block's
bytes at +1 against a five-byte value at 0x00548E90 — the ISO9660 primary
volume descriptor's "CD001" signature at LBA 16. So the positions are
**disc LBAs** and the game's driver walks the ISO's file system itself.

Implementation (decision 0019): `Kernel::set_disc_sectors` takes the disc's
raw byte source; the read copies `lba * 2048` for the requested length into
the requested destination; without a disc the bytes are zero; a read
outside the image stops loudly. The tool opens a second handle on the
pinned ISO for this path.

## Observed

With the service in place the reads are:

```
[read] lba=0x10  size=2048   the primary volume descriptor ("CD001")
[read] lba=0x105 size=2048   the root directory (ISO root extent 261 = 0x105)
```

and the run's work changes from 80,089 module calls at 60,000 services
(before) to 71,124 — the driver is reading structures instead of retrying
the same blind read. The library caches 2048-byte blocks, so a handful of
reads covers the directory walk.

## The GT4FS cross-check

The owner pointed at github.com/Razer2015/GT4FS (C#, by Razer2015). Its
sources corroborate this project's empirical format reading and supply the
semantics that were missing:

- `TocHeader`: magic "RoFS" (encrypted constant), version 3.1, compressed
  TOC length, page count, page length and entry count; the data begins at
  `pageCount * pageLength`. The pinned volume is the older 2.2 variant:
  uncompressed TOC, names XORed by 0xFF (GT4FS handles 3.1: deflate
  compressed pages XORed by 0x55 after inflate).
- `RecordEntry`: `{parent node (big endian), name (ASCII), type byte, then
  a file's {page offset, date, size} or a directory's node id}`; the file
  offset is `DataOffset + pageOffset * PageLength`.
- `BTree`/`DebugReader`: the TOC is a B-tree of pages — index pages hold
  sorted `{parent node, name}` keys pointing at child pages, record pages
  hold the entries — which explains the "counts" and "references" this
  project read in the pinned TOC: they are node ids and page navigation,
  not plain offsets.

The reference cannot read the pinned volume (it requires version 3.1), so
the empirical reader stays; but the field meanings above are exactly what
the file records need, and the next slice can now pin them with the
reference's layout as the guide instead of guessing.

The owner also pointed at github.com/Nenkai/GTAdhocToolchain (the GT4 Adhoc
script toolchain); it belongs to the later scripting milestone and is
recorded in `docs/STATUS.md` for it.

## Verification

- CTest **34/34** (the kernel test covers the read service: the fake disc's
  "CD001" sector lands in the guest, a read outside the image stops loudly,
  and a machine without a disc answers zeros); Python 73 (67 run, 6 skip).
- `gt4boot --compare-interpreter --disc <iso>` at 3,000 services:
  interpreter reference at 7,570,583 instructions, full state identical.
