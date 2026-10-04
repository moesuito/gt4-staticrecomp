# M30 lesson — the disc and the archive: real bytes for real opens

Prepared 2026-10-04. BUILD/VERIFY: passed for slices 16–18; see the
[M30 slice-16 evidence](../reverse-engineering/m30-slice16-disc-image.md),
[slice-17 evidence](../reverse-engineering/m30-slice17-archive-path.md),
[slice-18 evidence](../reverse-engineering/m30-slice18-gt4-volume-reader.md),
and [decision 0017](../decisions/0017-disc-image-file-service.md) /
[decision 0018](../decisions/0018-gt4-volume-reader.md). EXPLAIN:
this is the worked explanation; tutoring review pending.

## Objective and motivation

The clock arc ends with the game polling its devices forever,
asking for files by name and getting empty replies. This arc
teaches the project's file discipline in both directions: serve
what the disc really holds — sizes, spellings, bytes — and keep
every payload byte out of the repository while doing it. It ends
with the boot walking its IOP module list by name and the movie
path resolving to names and sizes inside the game's own 2.4 GB
archive.

The motivating shape is a retry loop that cannot succeed: the
file open fails before sending anything, because the model's
empty reply reads as "no disc". Loading cannot advance without
the files, and invented sizes would corrupt the game's
allocation and read bounds. The evidence must come from the disc.

## Step 1 — answer opens from the pinned image

The file server (sid `0x80000006`) receives 512-byte requests
with the path at +8. The client (`0x005B6CA8`) reads the
16-byte reply as `{handle, size}`: handle 0 takes the failure
path (`0x005B6D6C` returns `0xFFFEFFFD`); any other handle
succeeds and the size is stored beside it. The boot's asks:

```text
cdrom0:\IRX\SIO2MAN.IRX;1   -> handle=1 size=6641
cdrom0:\IRX\MCMAN.IRX;1     -> handle=2 size=96181
cdrom0:\IRX\MCSERV.IRX;1    -> handle=3 size=7385
cdrom0:\IRX\SIO2D.IRX;1     -> handle=4 size=11289
cdrom0:\IRX\DBCMAN.IRX;1    -> handle=5 size=15653
cdrom0:\IRX\DS2U_D.IRX;1    -> handle=6 size=11821
cdrom0:\IRX\LIBSD.IRX;1     -> handle=7 size=30085
cdrom0:\IRX\USBD.IRX;1      -> handle=8 size=34993
```

`Iso9660Image` parses the primary volume descriptor
("GRANTURISMO4", 2048-byte blocks, 1,317,056 blocks, eight root
entries), walks directories on demand with caching, and streams
file bytes from a `DiscByteSource` — so the 2.4 GB volume costs
only the parts actually read. Paths accept the game's spelling
(the `cdrom0:\` prefix, any case, optional `;version`). A path
the disc lacks — or no image at all — answers handle 0,
exactly like a console without a disc. Both engines get the
same image, so the differential stays exact; unit tests never
need the ISO (a synthetic image in memory, plus pinned checks
the CTest runs only when the file is present — including
SIO2MAN's ELF magic and GT4.VOL's size against its record).

## Step 2 — notice the engine has two file layers

With modules loading, the boot spins somewhere else: the game's
*own* CD path. Each load task carries a flag at `[task+0xB0]`
(set to `(a3 == 0)` at task start `0x004B1600`): flag 0 builds
the name on the stack and runs the file-system loaders
(`0x004B1DA8`); flag 1 runs the engine's CD driver through the
PCDV server (`0x004B1CA0`), starting reads (RPC 3 with
`{0x10, 0x800, destination}`) and polling completion (RPC 1) in
a delay loop until the descriptor's flag byte at `+0x19` shows
bit 1. Two API tables hold the pairs (`0x00617420` and
`0x00617950`); neither is referenced from image or live memory
— objects carry their vtable at `+0xA8` (object table at
`0x0063A078`, 127 entries of `0xF0`), mode chosen per object.

Instrumenting the PCDV calls showed what the boot wants: the
structure at `0x00617AA8` names the file `/mpeg` beside a
buffer and a request struct. The boot has reached its **movie
phase** — streaming the attract MPEG through its own driver,
not the IOP loadfile module. The driver's entry cache
(`0x0087D580`, 0x800 bytes) is empty in the model (the scan at
`0x00548E98` takes type byte 1, advances by the byte at
`+0x21`, ends on `0xFF`), so `while (0x549130(...) == 0)
delay(2000)` retries forever. The PCDV client structures at
`0x00887CB80/0x00887CC80` stay zero while the file-server path
is active — and the menu dump agrees. Which path the console
takes, and what selects it (the wrapper at `0x004ACA40` always
passes `a3 = 0`; `0x004ACAB0` starts the file-server variant),
is the documented open question, not a guess.

## Step 3 — read the archive lazily, and stop at the records

The data lives in `GT4.VOL;1` (2,459,502,592 bytes, extent
105879). Its decoded header:

```text
+000: magic 0xACB990AD, version 0x00020002
+008: 59809, 47712, 12097          (three sizes)
+014: 0x0100BA61                   (name-table offset)
+018: 23                           (root child count)
+01C: 23 child offsets
```

Entries are `{name pointer, count, value}` with `count - 1`
item words — the header counts itself, exactly like an entry.
Directories list child offsets; a file's `value` is its byte
size. Names are the archive text XOR `0xFF`, NUL-terminated,
through three tagged pointer tables; the root's 23 children
decode to the categories (`advertise`, `bgm`, `car`, …) and the
archive stores the game's own modules with their symbol tables
(`MpegRoot`, `main::menu::…`) — which is *why* the game streams
it through its own driver.

`Gt4Volume` parses lazily (header and root now, children on
demand with entry/children caches and a rejected-items set; the
64 MB metadata window read once; `DiscFileSliceSource` presents
the ISO's `GT4.VOL;1`), and validates instead of guessing: a
child candidate must carry a known name tag and decode to
printable text, or it is skipped. Verified against the pinned
volume: the 22 root categories, the `mpeg/gt4` chain,
`mv0010`'s `0x01200004` (18,874,372 bytes over 204,931 chunks),
`gtloading.img` at `0x21A0` (8,608).

The limit is stated where the evidence stops: a file entry's
items are *data records*, not child offsets, and the two kinds
are not distinguishable by inspection — 671 of `mv0010`'s
204,930 items coincide with real entries (`0x15C0` is both a
chunk field and "arcade"). The reader exposes `children()` for
directories and names/sizes for files, and does not interpret
records. Pinning the `{name, offset, packed}` shapes is
explicitly the next slice's task, together with the PCDV
answers that need the data offsets.

## What later evidence reframed (not smoothed over)

- The PCDV retry loop was genuinely answered afterward from raw
  disc sectors (the `{LBA, size, destination}` reads), then the
  disc grew a second volume with its own protocol, then the
  block-cache server filled the archive open the reader alone
  could not serve. Each step is its own evidenced slice; this
  lesson stops where its three slices stop.
- The file-item records were probed (candidate grammars tested
  and rejected) and later pinned from the game's own consumer
  side — the honest order (refuse the guess, then let the
  consumer define the grammar) is the method, stated in advance
  by the limit above.
- `Gt4Volume` outlived its slice: the asset track reuses it as
  the index over the same volume (page tables, name lookups),
  which is what lazy validated parsing buys — a reader strict
  enough to trust as infrastructure.

## Connection to our implementation

| Piece | File | Job |
| --- | --- | --- |
| ISO9660 walk + streaming | `src/executable/disc_image.cpp` (`Iso9660Image`, `DiscByteSource`) | descriptors, on-demand walk, game spellings |
| File open/size/handles | `src/ee/kernel.cpp` (file-server open, handle→path map) | real sizes, handle 0 as not-found |
| Archive parse | `src/executable/gt4_volume.cpp` (`Gt4Volume`, lazy caches, validation) | header/tree/XOR names, no record guessing |
| Image source | `DiscFileSliceSource` + `--disc` tool flag | pinned bytes in, payload bytes never in git |
| Reader fixtures | `tests/unit/disc_image_test.cpp`, `gt4_volume_test.cpp` | synthetic images; pinned checks only with image |
| Whole-run agreement | `tools/gt4boot/main.cpp` (`--compare-interpreter --disc`) | identical state with real sizes served |

## Understanding checkpoint

1. The open client treats handle 0 as failure and any other
   handle as success. Why must the model answer unknown paths
   (and no-disc machines) with handle 0 rather than an error
   code, and what in the client's code proves it?
2. Streaming the 2.4 GB volume costs "only the parts actually
   read". Which two design choices make that true, and what
   would an eager full-tree parse have cost instead?
3. The engine has two file layers selected per task. What
   evidence shows the boot is in its movie phase, and through
   which layer does it ask?
4. A child candidate must carry a known tag *and* decode to
   printable text. Why both conditions — what does each one
   reject, and what real data motivated the second?
5. The header "counts itself, exactly like an entry". Explain
   the `count - 1` rule in both places and what breaks if a
   reader treats the count as the child total.
6. The reader reports file names/sizes but will not interpret
   file records. Why is refusing the more useful behavior for
   the PCDV work that follows, and who eventually defines the
   records' grammar?
