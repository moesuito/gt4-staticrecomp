# M30, sixteenth slice — the disc image backs the file service

Date: 2026-10-02. Inputs: the pinned CORE and the pinned ISO. Follow-up to
the fifteenth slice (`m30-slice15-service-clock.md`), whose boot opened its
IOP modules off the disc and could not load them because the model answered
every open with an empty reply. This slice gives the model the disc: an
ISO9660 reader over the pinned image, and the file server's open answered
with the real file size (decision 0017). The boot now walks its module list
(SIO2MAN, MCMAN, MCSERV, SIO2D, DBCMAN, DS2U_D, LIBSD, USBD, ...) instead of
retrying one load. The differential passes at 3,000 services with the
interpreter reference at 7,570,583 instructions and the full state
identical.

## The opens the boot makes

The file server (sid 0x80000006, the IOP's loadfile module whose version the
game checks as "3000") receives 512-byte requests whose path sits at +8:

```
cdrom0:\IRX\SIO2MAN.IRX;1   -> handle=1 size=6641
cdrom0:\IRX\MCMAN.IRX;1     -> handle=2 size=96181
cdrom0:\IRX\MCSERV.IRX;1    -> handle=3 size=7385
cdrom0:\IRX\SIO2D.IRX;1     -> handle=4 size=11289
cdrom0:\IRX\DBCMAN.IRX;1    -> handle=5 size=15653
cdrom0:\IRX\DS2U_D.IRX;1    -> handle=6 size=11821
cdrom0:\IRX\LIBSD.IRX;1     -> handle=7 size=30085
cdrom0:\IRX\USBD.IRX;1      -> handle=8 size=34993
```

The client (0x005B6CA8) reads the 16-byte reply as {handle, size}: a zero
handle takes the failure path (0x005B6D6C returns 0xFFFEFFFD), any other
handle succeeds and the size is stored beside it (0x005B6D84).

## The ISO9660 reader

The pinned disc is a plain ISO9660 volume ("GRANTURISMO4", 2048-byte blocks,
1,317,056 blocks) whose root holds eight entries: SYSTEM.CNF;1,
SCUS_973.28;1, CORE.GT4;1, IOPRP300.IMG;1, IRX/, NET/, EPSON/ and
GT4.VOL;1 (2,459,502,592 bytes). `Iso9660Image` parses the primary volume
descriptor, walks directories on demand (caching what it reads) and streams
file bytes from a `DiscByteSource`; the tools open the file, the tests build
an image in memory.

The reader is verified twice: a synthetic image exercises the directory
walk, the game's path spelling (`cdrom0:\IRX\SIO2MAN.IRX;1` with and
without the version suffix and in any case) and the read tail (zero-filled
past the file's end, rejected past it); the pinned image additionally
confirms that SIO2MAN.IRX starts with the IOP ELF magic (0x7F 'E' 'L' 'F')
and that GT4.VOL's size matches the directory record.

## The engine's own disc path (the next frontier)

The game has two load paths, selected by a flag in its load task
([task+0xB0], set to `(a3 == 0)` by the task start at 0x004B1600):

- **flag 0**: the file-server path — the name is built on the stack and the
  file system's load functions run (0x004B1DA8 onward);
- **flag 1**: the game's own CD path through the PCDV server
  (0x004B1CA0 onward) — the engine starts a read (RPC 3 with the request
  {0x10, 0x800, destination}) and polls the completion (RPC 1) in a delay
  loop until the PCDV library returns a descriptor whose flag byte at
  +0x19 has bit 1 set (0x005491A0-0x005491A8). The library's reply is a
  *table of entries* with a leading type byte (0-4, 0xFF ends the table);
  the model's empty reply leaves the table empty and the engine retries
  forever (the constant request repeats thousands of times).

Which path the real console takes at this phase is evidenced by the live
boot: the PCDV client structures at 0x0087CB80/0x0087CC80 stay **zero**
while the liblgdev layer and the file server are active, and the menu dump
shows the same. The next slice should find what selects the path (the
wrapper at 0x004ACA40 always passes a3 = 0; the other wrapper at 0x004ACAB0
starts the file-server variant) and either route the model to the
file-server path or answer the PCDV file-table protocol.

## Verification

- CTest **33/33** (the new `disc_image` test; the kernel test answers the
  open with the fake disc's handle and size, handle 0 for unknown paths and
  handle 0 without a disc); Python 73 (67 run, 6 skip).
- `gt4boot --compare-interpreter --disc <iso>` at 3,000 services:
  interpreter reference at 7,570,583 instructions, full state identical.
- `--disc <iso> --services 120000`: 154,592 module calls, 4,319,091
  interpreted steps, ending at a service boundary (pc 0x005ADCC4,
  service 0x42).
