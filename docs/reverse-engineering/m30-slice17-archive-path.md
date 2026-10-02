# M30, seventeenth slice — the archive path: PCDV and GT4.VOL

Date: 2026-10-02. Inputs: the pinned CORE, the pinned ISO and the live menu
dump. Follow-up to the sixteenth slice (`m30-slice16-disc-image.md`), whose
boot loaded its IOP modules off the disc but then spun in the game's own CD
path. This slice establishes what that path is for, what it expects, and
what the game's data volume looks like — the groundwork for serving it.
**No model behavior changed**; the acceptance evidence is the unchanged
green suite.

## The engine has two file layers

The engine carries two complete implementations of its file layer, selected
by a flag in each load task ([task+0xB0], set to `(a3 == 0)` by the task
start at 0x004B1600):

- **flag 0**: the file-server path — the name is built on the stack and the
  file system's load functions run (0x004B1DA8);
- **flag 1**: the game's own CD path through the PCDV server (0x004B1CA0).

Two API tables hold the pairs (0x00617420 and 0x00617950, 0xC entries
apart by 0x530); neither table is referenced from the image or the live
memory — the objects carry their vtable at +0xA8 (the file-layer object
table is at 0x0063A078, 127 entries of 0xF0 bytes) and the mode is chosen
per object.

## What the model is loading

Instrumenting the PCDV calls in the model showed the structures the SDK
passes to its loader: `0x00617AA8` holds the **file name** `/mpeg` (the
field followed to the string at 0x0068BB90) beside a buffer pointer
(`0x0084B480`) and a pointer to the request struct at 0x00617AB0. In other
words, with the disc in place the boot has reached its **movie phase**: it
wants to stream the intro/attract MPEG, and it does so through the game's
own CD driver, not the loadfile-based file server.

The calls the engine makes on the PCDV server (sid 0x50434456) are:

```
rpc 3: send 64, recv 64, recvbuf 0x0086CC40   (start a read; the reply is
                                               the library's entry buffer)
rpc 1: send 64, recv 0                        (poll; no payload: the
                                               completion lives in the
                                               library's own state)
```

Both carry the same 64-byte request `{0x10, 0x800, 0x0084E080, 0, ...}` —
`0x800` = one 2048-byte sector, `0x0084E080` = the EE destination. The
position is not in the request: the library tracks the stream and resolves
the name against its 0x800-byte entry cache at 0x0087D580, which the model
has never filled (the scan at 0x00548E98 accepts entries whose first byte
is 1 and advances by the byte at +0x21; a zero byte skips, 0xFF ends), so
the engine's `while (0x549130(...) == 0) delay(2000);` loop
(0x004B1C70-0x004B1CD8) retries forever. The completion flag the engine
tests is bit 1 of the descriptor's byte +0x19 (0x005491A0).

## The data volume (GT4.VOL)

The game's data lives in `GT4.VOL;1` (2,459,502,592 bytes, extent 105879).
Its header is evidence-backed now:

```
+000: magic 0xACB990AD, version 0x00020002
+008: 59809, 47712, 12097        (three sizes)
+014: 0x0100BA61                 (the name table's offset)
+018: 23                         (the root's child count)
+01C: 23 child offsets: 0x78, 0x90, 0xA0, 0xB4, 0xCC, 0x128, ...
```

Each directory entry is `{name_offset, count, 0x14}` followed by `count`
child offsets, and the **names are the text XOR 0xFF** with NUL
terminators, sorted in a table at the header's name offset:

```
0x00BA61 ^ 0xFF: "advertise\0bgm\0car\0character\0coff..."
```

The root's 23 children decode to the game's top-level categories
("advertise", "bgm", "car", "character", ...), and the symbol strings seen
around archive offset 0x2463834 ("MpegRoot", "StartProject", "design_work",
"main::menu::...") show the archive stores the game's own modules with
their symbol tables — which is why the game streams it through its own
driver instead of the IOP loadfile module.

## The next slice

1. Implement the GT4.VOL reader (`Iso9660Image::read_file` already streams
   the volume's bytes) with the header, the directory tree and the
   XOR-0xFF name table; verify it against the pinned archive (the root's 23
   names, `mpeg`/`MpegRoot`) and with a synthetic archive in tests.
2. Use it to answer the PCDV protocol: fill the library's entry cache (the
   reply's entry layout: type byte 1 at +0, the length at +0x21, the
   position fields copied from +0x50 and +0x9C) and serve the sector reads
   (the 2048-byte requests into the requested EE address).
3. Keep the differential exact: both engines share the kernel and the disc,
   so the archive answers must hang off the same service path.

## Verification

- No model behavior changed; the suite stays green: CTest **33/33**,
  Python 73 (67 run, 6 skip) and `gt4boot --compare-interpreter --disc
  <iso>` at 3,000 services with the interpreter reference at 7,570,583
  instructions and the full state identical.
