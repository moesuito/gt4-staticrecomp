# M33 lesson — proving absence: the dry pipe, the unbuilt dispatcher, and the asset trailheads

Prepared 2026-10-04. BUILD/VERIFY: M33 slices 23–28 (recon dry
pipe, heartbeat, wrappers, switch, watch, asset dumps); see the
[M33 recon evidence](../reverse-engineering/m33-recon-dry-pipe.md),
[heartbeat](../reverse-engineering/m33-slice24-heartbeat-trace.md),
[wrappers](../reverse-engineering/m33-slice25-lazy-wrappers.md),
[switch](../reverse-engineering/m33-slice26-switch-cases.md),
[watch](../reverse-engineering/m33-slice27-write-watch.md),
[asset dumps](../reverse-engineering/m33-slice28-asset-dumps.md),
and the asset-track docs (`asset-page-cipher-static`,
`asset-texture-gpb-recon`, `asset-gs-chute-indirect`,
`asset-gif-emitter-hunt`, `asset-tex0-data-origins`,
`docs/plans/asset-viewer-roadmap.md`). Scout claims keep their
[V]erified / [S]cout-reported grades wherever reused. EXPLAIN:
this is the worked explanation; tutoring review pending.

## Objective and motivation

M32 ends parked and asks whether the engine is secretly alive —
a headless-but-running game would still submit graphics packets
nobody observes. M33 is chartered to answer binary: packets flow
(the boot is alive at the graphics layer) or the pipe is dry
(confirmed deep park). This lesson teaches the project's
hardest evidence discipline: proving *absence* — of traffic, of
a caller, of a writer, of a whole subsystem — with probes that
can only exonerate, never convict, while a parallel scout maps
the asset formats the engine would feed if it ever ran. It ends
with the absence proven six ways and the asset trailheads queued
for a boot that reaches them.

The motivating discipline: an empty observation ("nothing
happened") is worthless from an unverified probe. Every slice
here pairs its negative with a control proving the probe was
live.

## Step 1 — the pipe is dry, measured twice

Every GIF/VIF/SIF/SPR transfer the game starts must write its
CHCR with STR set — so a temporary log in the DMA channel write
path over a 12,000-service leg, plus all six channel controls
read idle at every stop, answers binary: **zero starts**. A
400-service fresh-boot control rules out a broken probe only
insofar as the CHCR-stop reads already stand on their own; the
SIF caveat (syscall-issued transfers bypass a register log) is
checked and moot, because final legs issue no SIF syscalls at
all. The sharpened table names the single upstream cause of
parked workers *and* dry pipe: the missing heartbeat — what
invokes the frame dispatcher (`0x00587b30` region) per frame.

## Step 2 — the dispatcher nobody invokes, built by nobody

The region disassembles cleanly (validate/wake-by-table/join/
signal at `0x005878f8`; a sleep-loop at `0x00587bb0`; a 10-way
switch at `0x00587c08` through the table at `0x006CE970`), but:
zero direct `jal` sites in all 3.5M translated lines (one
internal); no thread parked anywhere in `0x00587xxx` (register
ra census plus stack scans over all 17 threads, two legs); the
eight engine sleepers sit one frame *below*, in worker mains
through the `0x00574e30` spine. The worker loop's entry
materializes exactly once in the image — the initializer
`0x00586da0`'s `CreateThread` — whose chain climbs sole-caller
by sole-caller (`0x00583718` ← one-shot `0x00101c50`, flag
`[0x00617d88]` ← `0x001c9468` ← lazy wrapper `0x0019a698`)
until the flag reads 0 and the job global reads all zeros at
stop time: the whole job subsystem was never built. Its four
feeders resolve, by whole-text byte scan (each exactly one
direct site) plus stop-time strings, to USB-printer channel
initializers (`MPhotoRendererFace`, `printout`, `cleaning`,
`nozzleCheck`) — peripheral work, not the boot's path. The
switch's three case bodies prove domain-neutral (thread sync,
priority, cache flush, break-traps; no print verbs, no frame
verbs), and a PASS-verified write-watch over the flag word and
the loop slot records zero stores: total silence with a proven
probe.

## Step 3 — the parallel asset track, and its stop rule

While the main line proves absence, the read-only scout maps
what present engine code would consume — every claim below
keeps its grade:

- **Page cipher** ([V] reader shape, [S] sweeps): the game
  validates `RoFS`/0xACB990AD magics (`0x4B38A0`), reads tables
  with key `0x14AC327A` (`0x4B3938`), runs xor55 (`0x4B36E0`)
  and page fetch (`0x4B39B0`); v3.1 pages (`page[i] = enc[i] ^
  ((i+1) * 0x14AC327A)`, 179 pages) resisted exhaustive
  xor/inflate/skip grids — payload Unknown, reader mapped.
- **Textures** ([V] strings/shapes): `.gpb` indexed containers
  (`%s/%s/%s.gpb`, `doesn't exist in GPB`), a runtime binder
  table keyed by (tag, blurLevel[, texnum]) with named fallback
  (`no texture…`), PNG/TGA as photo-path only, `clut` on the
  font path first.
- **GS chute** (correction recorded): `0x004568F0` programs no
  GS register (16-slot object-pointer table); the Tex1 bind,
  field-setter family, and DMA/GIF pump poll are indirect end
  to end; **no PSM/TEX0/TEX1/TEXA/MIPTBP/CLAMP immediate in
  any stretch** — the pixel format travels *as data*, so the
  static road runs through the `.gpb` record grammar, not more
  disassembly.
- **Emitters + record flow** ([S] shapes, [V]-adjacent): upload
  packet (`0x004A4CA8`, BITBLTBUF/TRXDIR/TRXREG/TRXPOS) and
  NLOOP=6 context packet (`0x00499508`), fed from upload
  records walked `{position, name, count}` off the entry object
  — with `[obj+0x1C]` (the list writer, i.e. the material
  loader) honestly Unknown.

The track parks itself at an explicit stop rule: no static
road remains; the decisive experiments are main-line dumps
(Hook A buffers, Hook B in/out pairs, 128 B context, 80 B
upload, the `[obj+0x1C]` watch) — queued, not taken, because
the recon above shows the boot never reaches the code that
would execute them.

## Step 4 — verified-quiet dump legs close the line

The main line spends its last two slices cashing the trailhead
checks: force-interpreted hooks at the xor entry/exit, the
context-item completion, and the upload-packet final store fire
**zero times** across a fresh 95k-service boot-to-park
(non-blindness audited: the xor caller lives in untranslated
code that can only run through the bridge, where the hook
sits). The asset pipeline never runs — page fetch, materials,
packets — all downstream of the park. No payload bytes were
ever captured or stored; the dumps the viewer needs are
specified (addresses, lengths, trigger pcs), not taken.

## What this arc does not claim (kept explicit throughout)

- A dormant subsystem is not a dead one: the printer init, the
  packet emitters, and the volume reader all exist as code and
  data; what was proven is that *this boot* never reaches
  them — a statement about the trajectory, falsifiable by any
  future leg that does.
- The print-pool reframing of the dispatcher stays Hypothesis:
  contents-neutral handlers plus printer-owned creation is
  suggestive, not dispositive, and the lesson keeps it labeled.
- Scout [S] claims stay provisional by construction; each
  carries its method (counts, shapes, sweeps) so a failure
  localizes instead of collapsing the track.
- This arc directly parents decision 0026: the first *modeled*
  traffic arrives only after absence is proven six ways — a
  synthesized SIF packet through the mapped pump path, with a
  census tripwire that fails loudly if anything ever wakes.

## Connection to our implementation

| Piece | File | Job |
| --- | --- | --- |
| DMA/GIF traffic truth | `src/ee/device.cpp` (channel writes) + `--threads` CHCR reads | zero starts, idle controls |
| Dispatcher/switch code | translated whole-program module (verified, unreached) | disassembly source, never executed live |
| Archive index reuse | `src/executable/gt4_volume.cpp` (`Gt4Volume`) | names/sizes for the viewer track |
| Packet-handler facts | pump/dispatch code + SIF register array | what future traffic will traverse |
| Probe pattern | temporary write-watch (slice-27 design, reverted) | verified-quiet legs, cap + dropped counters |
| Viewer backlog | `docs/plans/asset-viewer-roadmap.md` | cipher, fonts, textures, skip-list |

## Understanding checkpoint

1. Zero DMA starts in 12,000 services is an observation; the
   "confirmed deep park" verdict needs two more premises.
   Name them, and the control that secures each.
2. The dispatcher has exactly one direct `jal` site, and it is
   internal. Why does that fact alone not prove dormancy — and
   which two further evidences close it?
3. The one-shot flag reads 0 and the job global reads zeros.
   Reconstruct the full creation chain that makes those two
   words prove "never built" rather than merely "idle".
4. No PSM/TEX0 immediate appears in any emitter stretch. Why
   does that push the static road toward the `.gpb` grammar
   instead of toward more disassembly — and what would falsify
   the "travels as data" verdict?
5. A write-watch leg records zero hits with a PASS selftest.
   Explain why the selftest is load-bearing rather than
   decorative, using the alignment-crash precedent.
6. Decision 0026 arrives only after this arc. Why must modeled
   traffic wait for proven absence — what goes wrong (with a
   concrete precedent from these slices) if synthesis comes
   first?
