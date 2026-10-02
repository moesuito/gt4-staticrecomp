# M30, twenty-first slice — the driver's disc walk and the volume's version family

Date: 2026-10-02. Inputs: the pinned CORE and ISO; the GT4FS reference
(github.com/Razer2015/GT4FS, C#) as the format authority. Follow-up to the
twentieth slice (`m30-slice20-pcdv-disc-reads.md`). This slice traces what
the game's own CD driver does with the disc-read service and settles the
pinned volume's place in the format family. **No model behavior changed**;
the suite stays green.

## What the driver does with the reads

With the sector service in place, a 120,000-service run issues exactly two
reads:

```
[read] lba=0x10  size=2048   the ISO9660 primary volume descriptor
[read] lba=0x105 size=2048   the ISO's root directory (extent 261 = 0x105)
```

and then stops reading: the driver parses the volume descriptor, follows
its root-directory extent and reads the root, but does not proceed to any
file's extent (GT4.VOL's extent 105879 would be the next natural read). The
engine's lookup loop keeps polling, so the driver's parse of the root
directory's records is where the next slice must look. The run's work at
120,000 services is 135,924 module calls and 4,345,094 interpreted steps,
ending at the same service boundary as before.

## The pinned volume's version family

The GT4FS packer (`RoFSBuilder.cs`) writes the **same 2.2 version the
pinned volume carries** (`WriteInt16(2); WriteInt16(2)`) before its main
header write uses 3.1, and its offset encryption is
`offset ^ index * 0x14AC327A + 0x14AC327A`; its pages are always
deflate-compressed and XORed with 0x55 when encrypting. Testing the pinned
volume against that model:

- the TOC page table is the `count - 1` offsets after the header's seven
  words (at +0x20), and the pages are the intervals between them
  (24, 16, 20, 24, 92, 268, ... bytes);
- **none of those pages inflates** (raw or after XOR 0xFF), so the pinned
  volume's pages are *uncompressed* — Sony's tools wrote the 2.2 variant
  without deflate, which is why the empirical reader (names as text XOR
  0xFF, plain structures) reads it directly;
- the name table at 0xBA60 (XOR 0xFF, NUL separated, sorted) and the entry
  tree the eighteenth slice verified remain the pinned volume's structure.

The reference therefore supplies the *semantics* (entry fields: parent
node, name, type byte, page offset/date/size; file offset =
DataOffset + pageOffset * PageLength; B-tree pages) while the *byte layout*
of the pinned 2.2 TOC stays as this project measured it.

## What was tried and rejected

- Serving the PCDV reads from GT4.VOL at the requested offset (slice 20):
  the game's behaviour did not change; the positions are disc LBAs, as the
  "CD001" check showed.
- Treating the pinned pages as deflate streams: no page inflates; the
  single "success" at 0x78 is 39 bytes of junk (a raw-deflate false
  positive).
- The writer of the five-byte string the library compares at 0x00548E90
  (0x006D83D8) was not found by address construction scans; the address is
  likely built through a pointer. The next slice should read the *library's*
  parse of the root-directory block (0x00548E20 onward) rather than chase
  the string.

## Verification

- No code changed; CTest **34/34**, Python 73 (67 run, 6 skip), and
  `gt4boot --compare-interpreter --disc <iso>` at 3,000 services with the
  interpreter reference at 7,570,583 instructions and the full state
  identical.
