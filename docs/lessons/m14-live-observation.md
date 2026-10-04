# M14 lesson — live observation: the game itself as the oracle

Prepared 2026-10-04. BUILD/VERIFY: as recorded in the sources —
M14: Python 64 collected (58 run, 6 skip); M14 slice 2: Python 70
collected (64 run, 6 skip). See the
[M14 evidence](../reverse-engineering/m14-live-observation.md) and
the [live-RAM manifest](../inputs/usa-v2.00-live-ram.json).
EXPLAIN: this is the worked explanation; tutoring review pending.

Every load-bearing statement below traces to the M14 evidence
document, the live-RAM manifest, or — for the consumption history
the task requires — the explicitly cited later records
(M19 adoption, M30 correction, decision 0007/0008, the timer/OSD
lesson). Corroboration is labeled as such; nothing is invented.

## Objective and motivation

Through M13 every check ran against artifacts the project itself
produced: reconstructed ELFs, hand-written fixtures, a second
emulator's source code read as documentation. M14 opens a second
channel — the game actually executing, observed live — and teaches
the project's oracle discipline: a live-observed value beats an
invented constant, but only a value the game itself checks earns a
place in the model. Everything else stays labeled. The slice ends
with the strongest validation of the M4 reconstruction to date
(the loaded text image matching live RAM byte-for-byte) and with a
reusable anchor (a restorable menu savestate) that later
milestones use for differential work.

Two failures motivate the method rules: a placeholder Status word
that the game's own setup code rejects (corrected two milestones
later), and an OSD-mining attempt that finds 282 candidates and is
rejected as evidence. Both look, from the outside, like "using live
data".

## Step 1 — open the channel without touching the game (setup + identity)

The setup, as cited: PCSX2 nightly `2.9.93` at `F:\Games\PS2` with
BIOS dumps (v7/v12/v15/v18; v18 `SCPH-90001` configured). PINE is
enabled by flipping `EnablePINE` to true in
`Documents/PCSX2/inis/PCSX2.ini` (original kept as
`PCSX2.ini.bak-gt4recomp`; PCSX2 persists the setting on clean
exit). The new tool `scripts/pcsx2_pine.py` implements the protocol
from the PCSX2 sources: batched requests (up to 32768 `Read64` per
round trip), status, version, title, serial, savestate save/load,
and `verify-elf`, which compares a local ELF segment against live
EE memory. It never writes memory; only the explicit
`save-state`/`load-state` commands change emulator state.

Two details worth keeping. First, the protocol work is covered
offline: the Python suite includes a fake PINE server exercising
socket, batching and unaligned read slicing, plus encoders, reply
parsing and ELF extraction — and one parser bug (wrong
`e_phoff`-relative offsets) was caught by the synthetic ELF
fixture before any live use. The channel is tested before it is
trusted. Second, the identity check: the pinned USA v2.00 ISO
boots with `-batch -slowboot`, and PINE reports title
`Gran Turismo 4` and serial `SCUS-97328` — the exact pinned
revision. An oracle for the wrong build is worse than no oracle.

## Step 2 — the comparison that matters (text identical, data explained)

Live EE main RAM at the main menu, compared against the
reconstructed analysis ELF (byte-identical to the pinned M4 hash):

| Record | Range | Size | Result |
| --- | --- | --- | --- |
| text | `0x00100000` | 5,339,668 | **5,339,668 matching, 0 differing** |
| reginfo | `0x006179fc` | 24 | 24 matching, 0 differing |
| data | `0x00617a80` | 779,132 | 736,471 matching, 42,661 differing |

The text dump and the ELF segment hash equal exactly
(`5a9a9107…bdd2d34` in full in the manifest) — quoted here as
metadata, never as payload. The source states the meaning
plainly: the strongest validation of the M4 reconstruction to
date, not against another tool but against the game executing
under an independent emulator.

The data record needs the honest treatment, because 42,661
differences could be read as a reconstruction error. A cold-boot
time series of the data record explains them:

```text
t0 (serial visible)   351,057 differing   (still loading)
t0 + 15 s             351,057 differing
t0 + 45 s              42,606 differing   (steady state)
menu (separate boot)   42,661 differing
```

The count falls as loading completes and settles at the
runtime-modified steady state. The first differences are exactly
what running code writes into writable data: pointers into the
image (`b0 7a 61 ...`), `0xff` sentinels. Conclusion, as cited:
after loading, the data record equals the reconstructed bytes
except where the game itself has written; there is no evidence of
load-time transformation of the static image. A falling time
series distinguishes "the game wrote here" from "we decoded
wrong" — one snapshot could not.

## Step 3 — freeze the moment so it can be replayed (savestate anchor + CPU state)

PINE savestate slot 9 (`SCUS-97328 (77E61C8A).09.p2s`,
13,453,263 bytes) holds the main menu and is restorable with
`load-state 9`; the owner independently saved slot 1 of the same
menu. Future experiments start from an exact, repeated state
instead of re-navigating the game. That is the snapshot
discipline: two independent saves of the same state, one of them
restorable by command.

Slice 2 turns the savestate into a register channel. A savestate
is a ZIP container: a version entry, memory blobs (`eeMemory.bin`
is the full 32 MiB EE RAM) and the `PCSX2 Internal
Structures.dat` raw freeze stream. `Freeze()` copies values
verbatim, so the stream contains host structs: the `cpuRegs` block
follows a 32-byte zero-padded tag and holds `struct cpuRegisters`
— GPR[32] in 16-byte slots (first 8 bytes are the 64-bit value),
HI, LO, CP0 (32 words), then `pc` at offset 680. The new tool
`scripts/pcsx2_savestate.py` reads it (`info`, `registers`,
`extract`); Python 3.14's zipfile reads the Zstandard entries
natively.

Real evidence from the menu savestate (slot 9):

- `pc = 0x00568b94`, word at pc `0x1520004a` (a `bne t1, zero,
  +0x4a` with t1 = 0), `ra = 0x00568acc` — both inside the loaded
  text;
- `sp = 0x0113fa90` inside RAM, `gp/fp/s0/s1` inside the data
  window (0x65xxxx–0x6ddxxx), `a1 = 0x70000000` (the hardware
  scratchpad);
- `cp0.status = 0x70030c11` (kernel mode, interrupts enabled).

Every value is consistent with a running game — the register
channel works. And the savestate's own `eeMemory.bin` re-verified
the text image offline: 5,339,668 bytes, 0 differing — a second,
independent confirmation of Step 2 through a different path.

The fixture incident is recorded because it is the method working:
the synthetic savestate fixtures initially used a 31-byte tag
(7 + 24 instead of 7 + 25 zeros), shifting every decoded field by
one byte; the synthetic tests caught it, the parser was correct
(real tags are 32 bytes), and the fixtures now mirror the real
layout exactly (biosdesc, tag offsets 0 and 322). The second
channel is tested before it is trusted, same as the first.

Limits, as recorded: this validates the loaded image and the
register channel — not execution semantics; savestate parsing is
offline and precise, but live single-stepping remains unsolved;
the interpreter still stops at COP1/MMI words, so differential
execution over real code needs broader decoding first; and from
slice 2 — one snapshot only, no N-instruction stepping, the freeze
layout is coupled to the emulator build and must be re-verified
per PCSX2 update, TLBs and the rest of CP0 not decoded yet.
Artifacts stay local (`private/pcsx2/` dumps); only distributable
metadata enters `docs/inputs/usa-v2.00-live-ram.json` — hashes and
counts, never payload bytes.

## Step 4 — what later slices took, what they refused, and what they corrected

This step correlates the M14 captures with their later
consumption, per the cited later records:

- **Adopted, then corrected: the CP0 Status baseline.** M19 set
  the CP0 default to `Status = 0x40000000` (CU2 usable), citing the
  M14 observation (see `m19-cop0-and-shifts.md`; M18 already
  previews it as "a realistic default"). M30 slice 5 corrected the
  baseline to the M14 live capture **`0x70030c11`** (IE/EIE set),
  calling the earlier value a placeholder — because the SDK's own
  thread setup checks `Status.IE` and `EIE` before the crt0 reaches
  its `ei`, and a placeholder that fails the game's own checks is
  not neutral (see `m30-timer-and-interrupts.md`, decision 0007;
  the interpreter's COP0 fixture now hand-computes against the
  capture). The timer/OSD lesson states the generalized rule:
  live-observed state beats invented constants, and "the game
  checks it" is the test for which constants matter.
- **Refused: the OSD word.** The attempt to mine the M14 dump for
  the OSD config block found 282 plausible words with no way to
  identify the block — rejected as evidence in decision 0008, and
  the initial word (`0x00012011`) labeled a model value instead.
  The timer/OSD lesson keeps this as the counter-example: labeling
  the model value instead of laundering a guess. Adoption and
  refusal are the same rule applied twice: the Status word was
  adopted because game code checks those bits; the OSD word was
  refused because no evidence identified it.
- **Context, not adoption: the stack and scratchpad.** The `sp`
  and `a1 = 0x70000000` observations stand as consistency evidence
  for a running game; no later record cited here promotes them
  into model constants. They are kept as what they are: proof the
  channel sees a live machine, available for the day some game
  check needs them.

## Connection to our implementation

The sources name tools, artifacts, and the manifest — so the table
maps each piece to its location and its source:

| Piece | Location / content (as cited) | Source |
| --- | --- | --- |
| PINE protocol tool | `scripts/pcsx2_pine.py` (batched `Read64`, status/version/title/serial, save/load, `verify-elf`; never writes memory) | M14 |
| Offline protocol cover | fake PINE server + encoders/reply/ELF fixtures; `e_phoff` bug caught pre-live | M14 |
| Savestate tool | `scripts/pcsx2_savestate.py` (`info`, `registers`, `extract`); 32-byte tag, `pc` at offset 680 | M14 slice 2 |
| Fixture incident | 31-byte tag shifted fields; tests caught it; fixtures mirror real layout (biosdesc, offsets 0/322) | M14 slice 2 |
| Menu anchor | PINE slot 9 (`SCUS-97328 (77E61C8A).09.p2s`, 13,453,263 bytes) + owner slot 1, same menu | M14 / manifest |
| Text identity | 5,339,668 bytes, 0 differing; SHA-256 equality (manifest) | M14 / manifest |
| Data steady state | 736,471 / 42,661; time series 351,057 → 42,606 → 42,661 | M14 / manifest |
| Menu CPU state | pc `0x00568b94`, word `0x1520004a`, ra `0x00568acc`, sp `0x0113fa90`, data-window regs, `a1` scratchpad, Status `0x70030c11` | M14 slice 2 / manifest |
| Status adoption | `0x40000000` default (M19) → `0x70030c11` baseline (M30 slice 5, decision 0007) | corroborated |
| OSD refusal | 282 candidates, rejected; `0x00012011` labeled model value | decision 0008 |
| Payload hygiene | dumps under `private/pcsx2/`; manifest/metadata only in `docs/inputs/` | M14 / manifest |

## Understanding checkpoint

1. The data record differs in 42,661 bytes, yet the slice claims
   "no evidence of load-time transformation". What distinguishes
   runtime writes from a reconstruction error — and why would a
   single snapshot be insufficient?
2. The PINE tool "never writes memory", and only explicit
   save/load commands change emulator state. Why does an
   observation channel need a no-write rule stated and tested
   before first live use?
3. Two independent saves (PINE slot 9, owner slot 1) anchor the
   same menu. What failure mode does the second save protect
   against that a single restorable slot does not?
4. The synthetic fixtures used a 31-byte tag while real tags are
   32 bytes — and the tests caught it. Explain why testing the
   parser against synthetic data *before* live use is what makes
   the live decode trustworthy rather than circular.
5. `0x40000000` was adopted as the Status default citing M14,
   then replaced by the M14 capture `0x70030c11`. Reconstruct the
   failure that forced the correction — whose check failed, and
   why does that check define which constants the model must get
   right?
6. The OSD mining found 282 plausible words and was rejected,
   while the Status capture was adopted. State the single rule
   that produces both verdicts — and what a future slice must
   show before any OSD field stops being a model value.
