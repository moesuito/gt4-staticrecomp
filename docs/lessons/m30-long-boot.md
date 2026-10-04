# M30 lesson — the long boot: real sectors, two volumes, cursors, and rest

Prepared 2026-10-04. BUILD/VERIFY: passed for slices 43–48 (via
decisions 0019–0021 and the runs between); see the [slice-43
evidence](../reverse-engineering/m30-slice43-beyond-the-step-limit.md),
[slice-44 evidence](../reverse-engineering/m30-slice44-why-the-pointer-is-odd.md),
[slice-46 evidence](../reverse-engineering/m30-slice46-the-copy-out-cursor.md),
[slice-47 evidence](../reverse-engineering/m30-slice47-no-wall-to-243m.md),
[slice-48 evidence](../reverse-engineering/m30-slice48-wait-graph.md),
and [decision 0019](../decisions/0019-pcdv-disc-reads.md) /
[decision 0020](../decisions/0020-dual-layer-disc-and-pcdv-volume-ops.md) /
[decision 0021](../decisions/0021-prts-block-cache.md). EXPLAIN:
this is the worked explanation; tutoring review pending.

## Objective and motivation

The disc/file arc ends with the game reading its own disc through
its own driver — and immediately misreading it, faulting,
stalling, and finally outlasting every budget. This arc teaches
the project's long-boot discipline: serve real sectors, derive
(not hardcode) the disc's geometry, keep per-handle state where
the protocol is stateless, prove starvation is not deadlock with
a wait-for graph, and let checkpoints make hundred-million-step
runs cheap. It ends with the machine fully idle at 243,711,723
services — a boot that rests.

The motivating shape is a driver that checks its own work: the
PCDV read reply is tested against the ISO9660 "CD001" signature
before the game trusts a single byte. Serve archive offsets and
the check fails forever; serve disc sectors and the driver walks.

## Step 1 — answer reads with sectors, derive the volumes

The PCDV read (RPC 3) carries `{LBA, byte count, EE
destination}` — first observed as `{0x10, 0x800, 0x0084E080}` —
and the reply check at `0x00548E90` tests bytes at +1 against
"CD001": positions are disc LBAs, which falsified the first
attempt (serving GT4.VOL offsets changed nothing). The kernel
gains a raw-sector source: LBA × 2048 into the EE destination,
zeros without a disc, loud stop outside the image. The boot's
driver walks: LBA `0x10` (primary descriptor), LBA `0x105` (root
directory, extent 261).

Then the disc turns out to hold two ISO9660 volumes: the live
cache's blocks prove a second volume whose logical block 0 is
`0x1418C0`, stored sixteen blocks early (its system area left
out). `DiscSectors` derives both volumes from the image itself
— first descriptor at block 16, second searched in `[size, size
+ 16]`, the two required to tile the image exactly, second
volume mapped back by the shift — rejecting non-ISO images and
unexplained tails loudly, with single-volume images passing
through. The driver's volume protocol: RPC 2 registers the
descriptor with an index-weighted checksum (recomputed from the
served image; mismatch stops loudly), RPC 4 answers the
registered volume's declared size, which the engine uses as the
next volume's start. The boot mounts both layers and reads the
inner archives (version 3.1) — stopping at an unaligned guest
access while parsing them.

## Step 2 — the cache server and its cursor

The fault (unaligned word access while the engine parses archive
data) is diagnosed the honest way first: the reference
interpreter faults at the same address and width, so the cause
is guest data the model provides, not translation. Tracing the
data back: the engine's sound library builds 13-byte stream
records whose odd position leaves a static object's pointer odd;
the records' source is the null pointer — the resolver returned
0 on a failed parse, so copies read low memory. The parse fails
inside the layer-0 archive handler's open, whose worker leaves
the result at 0 — because the game's own block-cache server
(sid `0x53545250` "PRTS", bound at `0x00550D00`) was unmodeled
and its zero reply left the file object unbuilt.

The PRTS protocol, read off traced calls: RPC 3 block read
`{LBA, size, flags}` → fresh handle; RPC 4/7 copy-out `{handle,
destination, size}` straight into client buffers; at most eight
cached blocks. Then the font load (`/fonts/system.fnt`, three
late PRTS reads, the last 186 KB) exposes the cursor bug: the
copy-out carries no offset, and re-serving chunk zero for every
chunked copy-out builds the object from one chunk repeated
seven times (seven `0x4000`-byte copies alternating two
buffers) — hence the patterned buffer and the odd "pointer"
that faulted relocation at 15,010,045 services. The fix is
server-side position: each handle carries a cursor, copies
advance it, exhaustion copies nothing. Verified past the wall
(41,919,339 services, clean stop) and past the sound phase
before it (3,648,011 services where the old run faulted at
83,783).

## Step 3 — prove the idle is starvation, not deadlock

With the cursor fix the boot sails: 243,711,723 services
(241,845,012 module calls, 7,119,118,045 steps), exit 0, ending
early at `no-runnable-thread 0x00001604` with 245,036
interrupts pending. A resumed final leg with counters shows, over
3,123 idle ticks: 3,123 injections, **zero** WAIT-to-Ready
unblocks, **zero** signal/wakeup/release calls in the whole
leg — VBlank and timer-2 chains deliver and run effect-free
(the VBlank callback slot `*(0x70002060)` is empty), and no
thread runs guest code even once. Ten threads sleep in
`SleepThread`, seven wait on semaphores (11 plus six delay
semas, each count 0 with one waiter). No cycle exists: every
edge points outward to a signaler that does not run. On
hardware the same handlers would run with the same (non-)effect,
so the difference must be an event source the model never
generates — ranked: async IOP completions (the model answers
synchronously, so an arrival-awaiting dispatcher sleeps
forever), then input (the 640-wide font layout suggests
menu/UI), then GS-side. The stuck SIF0 CHCR (`0x184`) was
checked and cleared: all-zero addresses, vestigial, not the
stall. The same resumed leg reproduces the ending bit-for-bit
(thread table to semaphore ids, identical pending count) — the
large-N checkpoint proof in passing, and the method that makes
hundred-million-step exploration affordable at all.

## What later evidence reframed (not smoothed over)

- The step budget became a flag (`--steps N`, CTest
  `gt4boot_steps`) in this arc's course — tooling co-evolving
  with the questions, as throughout the project.
- The starvation verdict directly parents decision 0026: the
  first *modeled* traffic (one synthesized SIF pump packet)
  arrived only after this arc proved nothing deliverable could
  wake anyone. The candidates ranked here (async IOP, input,
  GS-side) are the same ranking 0026 adopted.
- The 200M-step stop analysis (mid-copy in the per-packet
  routine `0x0055e6d0`, stationary 42-number service mix,
  healthy thread table) is the template for reading a
  budget-stop as work-in-progress rather than a wall.
- Scratch discipline became infrastructure here too: the 10B
  run's 14 GB service log was deleted post-evidence with a
  standing recommendation (`--quiet` mode) — long runs must not
  cost gigabytes per exploration.

## Connection to our implementation

| Piece | File | Job |
| --- | --- | --- |
| Raw-sector reads | `src/ee/kernel.cpp` (`set_disc_sectors`, PCDV RPC 3) | LBA×2048 into EE, zeros w/o disc |
| Volume derivation | `DiscSectors` over the image | two volumes, shift, tiling validation |
| Volume protocol | `src/ee/kernel.cpp` (RPC 2 checksum, RPC 4 size) | registered size drives next volume |
| Block cache + cursors | `src/ee/kernel.cpp` (`answer_prts_read/copy`, ≤8 blocks) | handles, sequential copy-outs |
| Idle proof tooling | `--threads`, `--dump`, checkpoint save/resume/verify | census, memory, bit-identical replays |
| Step budget flag | `tools/gt4boot` (`--steps`) + `gt4boot_steps` | bounded exploration past walls |
| Read-path fixtures | `tests/unit/ee_kernel_test.cpp` | sectors, volumes, handles, cursors, no-disc |

## Understanding checkpoint

1. Serving GT4.VOL offsets changed nothing; serving disc
   sectors walked the driver. What check distinguishes the two
   hypotheses, and where does it live?
2. The second volume is "stored sixteen blocks early". Derive
   the shift from the evidence (which blocks prove it), and
   explain why hardcoding `0x1418C0` was rejected.
3. The copy-out request carries no offset. Why is a server-side
   cursor *required* rather than merely convenient — and which
   observed streaming pattern proves the client never rewinds?
4. 3,123 injections, zero unblocks, zero signal calls. Why does
   that triple prove starvation rather than deadlock, and what
   single alternative observation would have flipped the
   verdict?
5. The same resumed leg reproduces the ending bit-for-bit.
   Beyond confidence, what engineering capability does that
   proof unlock for all later long-run exploration?
6. Decision 0026's synthesized packet reuses this arc's pump
   machinery. Which piece mapped here does it depend on most
   directly, and what did this arc prove about what such
   traffic can and cannot wake?
