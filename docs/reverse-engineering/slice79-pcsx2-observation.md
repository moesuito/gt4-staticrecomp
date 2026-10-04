# Slice 79: PCSX2 independent observation — setup + first calibrated reference captures (decision: none — observation only)

Date: 2026-10-04. Baseline: main at 9f7de5c (clean tree, no code touched this
slice). Task (slice 79): inventory `private/pcsx2`, capture the first useful
calibrated reference for the sema-63 hunt (slice 78 question: sema 63's
producer / originating traffic / later phase), compare against the model only
where the reference is trustworthy, and record the first model-vs-reference
divergence OR the first new fact about the producer. No model change ships
from this slice. No commit, no push, no branches.

## Run identity

- Model compatibility: unchanged from slice 78 (time=3 interrupt=3 rpc=1
  translation=2; quoted, not re-derived — no legs ran here).
- Inputs: not re-verified with `gt4disc.py` this session (unchanged bytes
  since slice 78's PASS on SCUS-97328 / VER 2.00). Instead, the reference
  itself re-verifies the payload: the slot-9 savestate's EE-RAM text window
  hashes to the pinned payload (section 3, Confirmed).
- Reference identity: PCSX2 USA BIOS **v02.30 (20/02/2008)** (reported inside
  both savestates) = the bundled SCPH-90001 v18 set; savestates are
  **v2.9.93** format; bundled emulator is **v2.9.94**.
- Tools used (read-only): `scripts/pcsx2_savestate.py` (`info` / `registers` /
  `extract`); stock Python `struct`/`hashlib` over the extracted EE RAM.
  Extraction targets went to the approved host temp dir, never into git.
  No writes to the emulator, no PINE writes (PINE was never even connected).

## (1) Environment inventory — what exists, what is missing

| Item | State (Confirmed by direct read) |
|---|---|
| Emulator | `private/pcsx2/pcsx2-v2.9.94/pcsx2-qt.exe` + Qt/SDL3 DLLs present (bundled set; not launched this session) |
| PINE config | `private/pcsx2/pcsx2-config/PCSX2.ini`: `EnablePINE = true`, `PINESlot = 28011`; `[Filenames] BIOS = SCPH-90001_BIOS_V18_USA_230.ROM0` |
| BIOS set | `private/pcsx2/bios/`: 4 USA revisions (39001-v7, 70012-v12, 77001-v15, 90001-v18 with ROM0/ROM1/MEC/NVM; plus DIFF/INF sidecars). Reference states used the v02.30/90001 line |
| Savestates | slot 9 `SCUS-97328 (77E61C8A).09.p2s` (13,453,263 bytes, pc 0x00568b94 = menu phase); slot 1 `.01.p2s` (13,692,267 bytes + backup, pc 0x00081fc0 with zeroed GPRs — but its EE RAM is the SAME late-phase image, see section 4, so it is NOT an early anchor) |
| Prior captures | `menu-eeMemory.bin` (33,554,432 bytes), `menu-registers.txt` (live PINE register capture), `text-ram.bin` (5,339,668 bytes) + `ram-text-comparison.txt` (`matching=5339668 differing=0`) |
| Live process | none: no `pcsx2-qt` process, PINE 127.0.0.1:28011 connection refused |

What is missing for live observation (exact blockers, nothing installed):

1. No running PCSX2 this session — a live capture needs an interactive run
   (open ISO `Gran Turismo 4 (USA) (v2.00).iso` at the repo root, 5,314,478,080
   bytes; boot; wait minutes to menu) or a validated CLI automation (untested).
2. The bundled ini points `Bios = F:\Games\PS2\BIOS` (original machine's path,
   absent here) — must be repointed to `private/pcsx2/bios` before any boot.
3. Savestate/emu version skew: states are v2.9.93, bundled emu is v2.9.94 —
   load compatibility untested.
4. No debugger/logpoint session was opened; per PLAN §9.2–9.3 any future
   watchpoint needs a positive control first (this slice's offline analogue is
   in section 2).

## (2) Calibration — positive control FIRST (Confirmed)

- Slot-9 offline `registers` decode is **identical** to the live PINE capture
  `menu-registers.txt`: pc 0x00568b94, all 32 GPRs, HI/LO, CP0
  status 0x70030c11 / cause 0x20 / epc 0x005adcc8. The only diff line is the
  `word at pc:` trailer, which the savestate decoder never prints (tool-output
  difference, not a state difference).
- Slot-9 EE-RAM text window `[0x00100000, +5,339,668)` hashes to
  `5a9a9107…2d34` = the pinned payload hash in `docs/inputs/usa-v2.00*.json`
  (byte-identical, second independent confirmation after M14's live check).
- Verdict: the offline savestate path is calibrated — word reads from these
  states are trustworthy references. Absence-of-signal arguments would still
  need per-path calibration (PLAN §9.3); none are made here.

## (3) Reference captures — sema-63 object set at menu phase (Confirmed)

Guest-virtual == physical offsets here (all < 32 MiB). Handles decode as
`id | (generation<<8)` per the slice-78 wrapper survey (game code
0x005782e8/0x00578290, same binary in the reference).

Slot 9 (menu phase, pc 0x00568b94):

| Location | Reference value |
|---|---|
| slot A `0x0064C3C8` | **0x11E** = sema id 30, generation 1 |
| job B `0x0086CB80+0x00` | **0x10D** = sema id 13, generation 1 |
| job B `+0x34` (submitted) | **1** |
| job B `+0x38` | 0x6897f8 (same allocator address as the model's 5M stop) |
| job B `+0x3C` (gate) | 0 |
| job B `+0x40` | 0x10E (sema id 14, gen 1) |
| job B `+0x80/+0x84/+0x88/+0x8c` (published args) | 0x1bef0 / 0x59440 / 0x90ea80 / 1 (ALL LIVE) |
| sibling C `0x0086CC80+0x00` | 0x10F (sema id 15, gen 1) |
| sibling C `+0x34` | 0 |
| sibling C `+0x38` | 0x6897d8 |
| sibling C `+0x40` | 0x86cd00 (pointer-shaped, not a stamped handle) |
| sibling C `+0x80` | 0x32523e70 (pointer-shaped; slot 1 has 0x32523e9a — volatile heap data, one alloc apart) |
| gen-counter table `0x00874550` | 22 nonzero of 256: ids 6–16, 26, 29–36 (+rest) all = 1 — including **13, 14, 15, 30** (match the live handles); ids **63/71/75 read 0** |

## (4) Model-vs-reference comparison (endpoint only — read carefully)

Model stop state (slice 78, 5M services, early init): A=0x13F (sema 63),
B+0x00=0x13F, B+0x34=0, B+0x3C=0, B+0x80s=0, sibling+0x00=0x147 (sema 71).

| Fact | Status |
|---|---|
| Handle slots A and B no longer name sema 63 in the reference: A→id 30, B→id 13 (both gen 1, counters confirm) | Confirmed (slot-9 RAM) |
| A job passed the 0x00548660 gate on (an incarnation of) object B: +0x34=1 with four live args | Confirmed (slot-9 RAM) |
| Same heap slot: B+0x38 = 0x6897f8 in BOTH model stop and reference | High confidence it is the same object slot |
| The knot was broken by **re-stamping with freshly created semas**, not by re-supplying sema 63 (ids 63/71/75 counters are 0; live ids are 13/14/15/30) | Confirmed as end-state; mechanism timing Unknown |
| Slot-1 RAM at all six probed addresses is identical to slot 9 (only volatile sibC+0x80 differs) — slot 1 is the same late phase, NOT an early anchor, despite its pc=0x00081fc0/zeroed-regs CPU snapshot | Confirmed |
| WHEN the re-creation happened between the 5M stop and the menu | Unknown — no intermediate anchors exist (this is an endpoint comparison, NOT a localized first divergence) |
| HOW the early W_B waiter on sema 63 unparked (re-stamping a slot does not by itself release a wait already issued on the old sema) | Unknown — no causal claim made |
| Raw sema-id ordinals (63 vs 13/30) | NOT comparable across kernels (real BIOS vs HLE allocator order differ by construction); only handle structure + object fields are compared by meaning |

First new fact for the hunt (not a divergence localization): in the reference,
the early A=B=0x13F incarnation is dead history by menu time — the subsystem
re-created its semaphores (fresh low ids, generation 1) and re-stamped both
slots, and a job flowed through object B to submission. This corroborates slice
78's ranked "later boot phase" outlet with a concrete mechanism shape
(re-create + re-stamp + gate passed), and rules out the shape "sema 63 itself
got re-supplied and everything continued on id 63".

## (5) Discriminants armed for the next slice

- D1: who writes `0x0064C3C8` / `0x0086CB80+0x00` after the early phase (creator
  0x00548500 re-invoked later, or slot recycled by another path)? Needs
  intermediate anchors or a live watchpoint with positive control.
- D2: no savestate exists between the model's 5M stop and the menu — any live
  session should save staged states (post-5M-equivalent, pre-menu) to bracket
  the re-creation.
- D3: live-boot recipe is now fully inventoried (§1 blockers 1–4); the
  reference BIOS is identified (90001-v18 line), so a fresh boot can reuse it.

## Gates and hygiene

- Tree untouched except this doc + journal: `git status` holds only
  `docs/reverse-engineering/slice79-pcsx2-observation.md` + the journal edit.
  No source, config, or test change (a behavior change was prohibited).
- No payload bytes in docs/git: only hashes, addresses, sizes, values of
  runtime fields, and relations (per project rule).
- Full gates on the unmodified tree (VsDevCmd `-arch=amd64` chained, x86-path
  install at `C:\Program Files (x86)\...`, NOT `C:\Program Files\...`):
  configure+build green; **CTest 53/53** (206.37 s); **Python 73 collected —
  OK (skipped=6)**.
- Hunt artifacts (extracted 32 MiB EE-RAM copies, regs, logs) live only in the
  approved host temp dir, never in the repo.
- No commit, no push, no branches.
