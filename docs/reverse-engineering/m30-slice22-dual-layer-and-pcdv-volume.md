# M30, twenty-second slice — the disc's two volumes and the CD driver's volume protocol

Date: 2026-10-02. Inputs: the pinned CORE and ISO, the live PCSX2 memory
dump (`private/pcsx2/menu-eeMemory.bin`) and the GT4FS reference as the
format guide. Follow-up to the twenty-first slice
(`m30-slice21-driver-disc-walk.md`). This slice changes model behavior: the
sector service now presents the disc's **logical blocks** (both layers) and
the CD driver's volume protocol is answered, so the boot leaves the
movie-mount loop and reaches the archive parsing.

## What the live game showed

The live dump's library cache table (0x0064C3D0: capacity 6, count 4,
counter 6, entries at 0x0084E000) holds four blocks:

| key (logical block) | block |
| --- | --- |
| 0x10 | 0x0084E080 |
| 0x105 | 0x0084E880 |
| 0x1418D0 | 0x0084F080 |
| 0x1419C5 | 0x0084F880 |

Each block is byte-identical to the image's sector at 0x10, 0x105, 0x1418C0
and 0x1419B5. So the live game read the descriptor and root of a **second
ISO9660 volume** whose logical block 0 is 0x1418C0, and the image file
stores that volume **sixteen blocks early** — its system area (the first
sixteen blocks) is left out. The arithmetic closes exactly: the layer-0
descriptor's volume space size is 0x1418C0 (where the second volume's
logical block 0 lies), the second descriptor's size is 0x137FE0, and
0x1418C0 + 0x137FE0 = the file's block count plus 16. The image scan finds
"CD001" at blocks 0x10, 0x11 (layer 0) and 0x1418C0, 0x1418C1 (layer 1).

## The driver's volume protocol

The game's own CD library (the "PCDV" server, sid 0x50434456) has four
operations, settled by disassembly:

- **RPC 1** polls the driver's status.
- **RPC 2** registers the volume descriptor the library's scan accepted
  (0x00548E20's success path calls 0x005485D8): the request is
  `{block, checksum}`, where the checksum is the library's index-weighted
  byte sum (0x00548D20: `Σ byte[i] * (i + 1)` over the 0x800-byte block).
  The registration happens **only for the first volume** — the scan skips
  it when its offset argument is non-zero (0x00548F5C).
- **RPC 3** reads `{block, byte count, EE destination}`.
- **RPC 4** answers `{status, value}` (the wrapper at 0x005487C0 reads the
  value only when the status word is non-zero). The engine
  (0x004ACB58–0x004ACB80) stores the value at `[task+0xEC]` and passes it
  as the offset of its next lookup, so the **second volume is found through
  the first volume's declared size**.

## What the model does now

- `DiscSectors` (`disc_image.hpp`/`disc_image.cpp`) presents the image by
  the console's logical blocks: it parses the first volume's descriptor
  (both-endian volume-size check), searches the file's blocks
  `[size, size + 16]` for the second volume's descriptor — the only place
  the file can hold it, since omitting the system area shifts it down by at
  most sixteen blocks — verifies that the volumes tile the logical image
  exactly, and maps every logical block of the second volume back by the
  shift. A single-volume image maps straight through; an unexplained tail
  stops loudly instead of guessing.
- The kernel answers **RPC 2** by recomputing the library's checksum from
  the same image it serves (a mismatch stops loudly) and records the block;
  **RPC 4** reads the registered volume's "volume space size" from the
  image and answers `{1, size}`; without a disc or a registration it
  answers zeros, which the library reports as failure and the engine
  retries — the correct behavior for a console without a disc.
- `gt4boot --disc` prints the derived volumes.

## Observed result

A traced run (temporary instrument, removed) shows the boot's read
sequence: layer-0 descriptor (0x10) → registration (op 2, checksum
0xD3DB18 accepted) → layer-0 root (0x105) → op 4 answers 0x1418C0 →
**layer-1 descriptor (logical 0x1418D0)** and **layer-1 root (logical
0x1419C5)** — the exact blocks the live cache holds — → a layer-1 archive
header probe and whole-file read (0x143B24, 0xF00 bytes) and an inner
layer-0 archive (0x1BEF0, 0x59440 bytes), both carrying the archive magic
0xACB990AD and version 3.1 (the family the GT4FS reference documents).

The run then stops at a **new frontier**: a guest fault at pc 0x00462670
(an unaligned word access at 0x008475EB) after 83,783 services while the
engine parses that archive data. The function is the game's streaming pool
class (0x462670–0x462730, static counters at 0x00623A38); the bad pointer
arrives through 0x0044D740. The next slice investigates what the engine's
parser expected there.

## Verification

- CTest **34/34** (the `disc_image` test now covers the logical mapping:
  a synthetic dual-layer image, the single-volume pass-through, the
  unexplained-tail rejection and the pinned ISO's derived volumes and
  content; the `ee_kernel` test covers the registration, the query reply
  and both loud failures).
- Python 73 (67 run, 6 skip).
- The differential passes at 3,000 services with the interpreter reference
  at 7,570,583 instructions and the full state identical.
