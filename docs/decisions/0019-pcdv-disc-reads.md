# 0019 — The game's own CD driver reads the disc

Status: implemented 2026-10-02 for the M30 twentieth slice
(`docs/reverse-engineering/m30-slice20-pcdv-disc-reads.md`).

Context: with the archive reader in place the boot still spun in the game's
own CD path (the PCDV server, sid 0x50434456). Tracing its request showed
what the library actually asks for: `{0x10, 0x800, 0x0084E080}` — an LBA, a
byte count and an EE destination — and the code around the reply checks the
block's bytes at +1 against a five-byte value (`0x00548E90`), which is the
ISO9660 primary volume descriptor's "CD001" signature at LBA 16. The game's
own driver walks the disc's file system itself; it is not asking the model
for archive metadata at all.

Decision:

- **The PCDV read (RPC 3) answers from the disc image's raw sectors.** The
  kernel gains `set_disc_sectors(const DiscByteSource*)`; the request's LBA
  selects `lba * 2048`, the byte count is copied into the requested EE
  destination, and the reply's status word stays zero (the library's status
  check at 0x00548718 reads zero as success). The tool opens a second
  handle on the pinned ISO for this service.
- **A machine without a disc answers zeros**, like a drive with no medium,
  and a read that leaves the image stops loudly instead of inventing data.
- **The GT4FS reference (github.com/Razer2015/GT4FS) corroborates the
  format family**: its `TocHeader` (magic "RoFS", version 3.1), its
  `RecordEntry` layout (parent node, name, type byte, then a file's page
  offset/date/size or a directory's node id) and its
  `file offset = DataOffset + pageOffset * PageLength` match the semantics
  this project read empirically from the pinned volume (which is the older,
  uncompressed 2.2 variant with names XORed by 0xFF; GT4FS handles the
  compressed 3.1 variant with different keys).

Alternatives considered:

- **Answering the reads from GT4.VOL** (the archive at the requested
  offset): tested first; the game's behaviour did not change, and the
  "CD001" check explains why — the positions are disc LBAs, not archive
  offsets.
- **Synthesizing the file-system structures**: unnecessary; the ISO image
  already holds them.

Consequences and limits:

- The boot's own driver now reads the disc: the first reads observed are
  **LBA 0x10 (the primary volume descriptor)** and **LBA 0x105 (the root
  directory, matching the ISO's root extent 261)**, and the run's work
  counts change (71,124 module calls at 60,000 services versus 80,089
  before), so the driver is walking the disc instead of retrying blind.
- The library caches 2048-byte blocks, so a handful of reads covers the
  directory walk; the archive's own file records (the next slice) still
  need the record grammar pinned, for which GT4FS's entry layout is the
  reference.

Verification:

- CTest **34/34** (the kernel test answers a read with the fake disc's
  "CD001" sector, rejects a read outside the image and answers zeros
  without a disc); Python 73 (67 run, 6 skip).
- `gt4boot --compare-interpreter --disc <iso>` at 3,000 services:
  interpreter reference at 7,570,583 instructions, full state identical.
