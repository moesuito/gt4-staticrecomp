# 0020 — The disc's two volumes and the CD driver's volume protocol

Date: 2026-10-02. Status: accepted (M30 slice 22).

## Context

The game's own CD driver (the PCDV server, sid 0x50434456) mounts the
pinned disc's two ISO9660 volumes. The live PCSX2 memory dump shows its
block cache holding the descriptor and root of both volumes (keys 0x10,
0x105, 0x1418D0, 0x1419C5), and each block is byte-identical to the image's
sector at 0x10, 0x105, 0x1418C0 and 0x1419B5. The image stores the second
volume sixteen blocks early: its system area (the first sixteen blocks) is
left out, so its logical blocks map back by that shift. Disassembly of the
library settled the protocol: RPC 2 registers the descriptor block with an
index-weighted byte checksum; RPC 4 answers the registered volume's "volume
space size", which the engine uses as the logical block where the next
volume begins (0x004ACB58–0x004ACB80). Slice 21's reads stopped at the
layer-0 root because the model answered RPC 4 with zeros and the engine
retried forever.

## Decision

1. **The sector source derives the volumes from the image itself.** A
   `DiscSectors` byte source parses the first descriptor at block 16
   (rejecting a non-ISO image and a non-both-endian volume size), searches
   the file's blocks `[size, size + 16]` for the second descriptor (the
   only place the file can hold it), requires the two volumes to tile the
   logical image exactly (`descriptor block + its size == the file's blocks
   + the shift`), and maps every logical block of the second volume back by
   the shift. A single-volume image maps straight through; an image with an
   unexplained tail stops loudly. Nothing is hardcoded to the pinned disc.
2. **RPC 2 validates and records.** The kernel recomputes the library's
   checksum (Σ byte[i]·(i+1) over the 0x800-byte block) from the same image
   it serves; a mismatch stops loudly (the registration has no reply to
   report it in), and the accepted block is recorded.
3. **RPC 4 answers the registered volume's declared size** read from the
   image, as `{1, size}`. Without a disc or a registration the reply is
   zeros — the library reports failure and the engine retries, exactly like
   a console without a disc.

## Alternatives considered

- **Hardcoding the layer break (0x1418C0).** Rejected: the derivation is
  short, validates the input (a changed image stops loudly) and works for
  single-volume images.
- **Serving the second volume at its logical blocks without the shift.**
  Rejected by the live evidence: the game receives the image's sectors at
  (logical − 16) for the second volume.
- **Answering RPC 4 with a fixed value.** Rejected: the registered
  volume's own declared size is the general rule the engine depends on,
  and it keeps the model honest about which volume is mounted.

## Consequences

- The boot leaves the movie-mount loop: it mounts both volumes, finds the
  layer-1 archive and reads the inner archives (version 3.1), stopping at a
  new frontier (an unaligned guest access while parsing archive data).
- The model supports exactly **two** volumes. The pinned disc has two and
  the engine only ever asks for the volume after the registered one; a
  third volume would stop loudly rather than be mis-served.
- The checksum check makes the model's disc an input the game validates
  against its own reads: a differing image cannot pass silently.
