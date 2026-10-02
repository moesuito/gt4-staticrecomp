# 0017 — The disc image backs the file service

Status: implemented 2026-10-02 for the M30 sixteenth slice
(`docs/reverse-engineering/m30-slice16-disc-image.md`).

Context: the boot's file opens are the game loading its IOP modules by name
from the disc ("cdrom0:\IRX\SIO2MAN.IRX;1", MCMAN, MCSERV, SIO2D, DBCMAN,
DS2U_D, LIBSD, USBD, ...). With the model's empty reply the open client read
handle 0, returned 0xFFFEFFFD (0x005B6D6C) and the game retried the same
loads forever: the loading path could not advance without the files.

Decision:

- **The model serves files from the pinned disc image.** A host-side
  ISO9660 reader (`Iso9660Image` over a `DiscByteSource`) parses the volume
  descriptor and walks the directory tree on demand; reads stream from the
  byte source, so the 2.4 GB GT4.VOL costs only the parts that are read.
  Paths accept the game's spelling (`cdrom0:\IRX\NAME.EXT;1`), are matched
  case-insensitively and the `;version` suffix is optional.
- **The file server's open answers from the disc.** The 512-byte request
  carries the path at +8; the model answers the 16-byte reply
  {handle, size} with a non-zero handle and the file's real size, and
  records the handle -> path mapping for the reads that follow.
- **A path the disc does not have — or a machine with no image configured —
  answers handle 0**, exactly like a console without a disc. The tool names
  the image with `--disc`; unit tests never need it (a synthetic ISO plus
  the pinned image when present cover the reader).
- **Both engines get the same image**, so the differential stays exact.

Alternatives considered:

- **Answering invented sizes** (any non-zero value): the game uses the size
  to allocate and to bound its reads, so a guess would corrupt its loading;
  the real size comes from the disc's directory record.
- **Committing any file payload**: never; the image is a local input and the
  repository keeps only hash-pinned metadata.
- **Making the unit tests require the ISO**: the reader is exercised with a
  synthetic image built in memory; the pinned-ISO checks run only when the
  caller passes the image path (CTest passes it when the file is present).

Consequences and limits:

- The opens now succeed with the disc's sizes (SIO2MAN.IRX 6,641 bytes,
  MCMAN.IRX 96,181, MCSERV.IRX 7,385, SIO2D.IRX 11,289, DBCMAN.IRX 15,653,
  DS2U_D.IRX 11,821, LIBSD.IRX 30,085, USBD.IRX 34,993) and the game walks
  through its module list instead of retrying one load.
- Only the open is answered so far; the model also exposes
  `read_file` on the image for the read services the next slices will need.
- The engine's own disc path (the PCDV file-table protocol) is still
  unanswered; see the slice's evidence document for what it expects.

Verification:

- CTest **33/33** (the new `disc_image` test: the synthetic image, the path
  spellings, the read tail and the pinned ISO's ELF magic); Python 73
  (67 run, 6 skip).
- `gt4boot --compare-interpreter --disc <iso>` at 3,000 services:
  interpreter reference at 7,570,583 instructions, full state identical.
