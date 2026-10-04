# Slice 85: staged-anchor or live-watchpoint attempt (decision: none — observation only)

Date: 2026-10-04. Baseline: main at ff35eda (slice 84, clean tree, no code
touched this slice). Task (slice 85): attempt the exact experiment named in
slice 84 — temporal evidence for H1 (old waiter parked, rescued by delete with
the ROM-confirmed wake-all mechanism) vs H2′ (gate ran, pulse-first
interleave, never parked) — in order: (1) OFFLINE staged anchor first
(cheapest): any existing savestate/capture in `private/` showing an
intermediate subsystem stage; (2) if (1) exhausts, ONE minimal live PCSX2
session only if the four slice-79 blockers resolve in minutes, with a
mandatory positive control before any causal read; (3) if both exhaust,
verdict + next best step toward P10. PROHIBITED and kept: no model behavior
change, no conclusion without a discriminant, no game/BIOS bytes in git/docs,
no marathon live attempts without a positive control. No commit, no push, no
branches.

Method: host-Python reads of the pinned input are NOT needed (no new static
claim); re-reads of existing reference states with `scripts/pcsx2_savestate.py`
(`info` / `registers` / `extract`) plus stock-Python `struct` over the
extracted EE RAM. Helper script lives only in the approved host temp dir, not
in git. Confidence labels per project rule.

## (1) Offline staged-anchor search — EXHAUSTED, no stage found (Confirmed)

### Sweep: every candidate state file in `private/`

A full file sweep of `private/` (excluding `tooling-venv`, the bundled
`pcsx2-v2.9.94` tree and the JDK/Ghidra tooling) lists exactly three guest
state captures, all already known:

- `private/pcsx2/sstates/SCUS-97328 (77E61C8A).09.p2s` (13,453,263 bytes,
  pc 0x00568b94, menu phase) — slice-79/84 endpoint anchor;
- `private/pcsx2/sstates/SCUS-97328 (77E61C8A).01.p2s` (13,692,267 bytes,
  pc 0x00081fc0, zeroed GPRs — slice 79 proved its EE RAM is the SAME
  late-phase image, not an early anchor);
- `private/pcsx2/sstates/SCUS-97328 (77E61C8A).01.p2s.backup`
  (11,671,935 bytes) — the ONE never-compared candidate (about 2 MB smaller
  than slot 1; size alone could mean different content or just different
  compression).
- Plus the derived `menu-eeMemory.bin` / `menu-registers.txt` /
  `text-ram.bin` captures (all menu-phase, slice 79).

No other savestate, memory dump, screenshot with state, log with guest words,
or staged capture exists anywhere in `private/`.

### The backup file: same menu endpoint, not a stage (Confirmed)

`info`: backup is savestate v2.9.93, BIOS USA v02.30 (same line as slots 1/9),
pc **0x00081fc0** — the same post-boot CPU snapshot shape as slot 1
(`registers`: zeroed GPRs, HI/LO 0, CP0 status 0x70030c11 / cause 0x20 /
epc 0x00081fc0). EE RAM extracted to the approved host temp dir (never in
git) and compared word-for-word against slot 1 and the menu capture at every
subsystem address slices 79–84 use:

| Location | backup | slot 1 | menu-eeMemory.bin |
|---|---|---|---|
| slot A `0x0064C3C8` | 0x11E (id 30, gen 1) | identical | identical |
| job B `+0x00/+0x34/+0x38/+0x3C/+0x40` | 0x10D / 1 / 0x6897f8 / 0 / 0x10E | identical | identical |
| job B `+0x80/+0x84/+0x88/+0x8c` | 0x1bef0 / 0x59440 / 0x90ea80 / 1 (four live) | identical | identical |
| sibling C `+0x00/+0x34/+0x38/+0x40` | 0x10F / 0 / 0x6897d8 / 0x86cd00 | identical | identical |
| sibling C `+0x80` | 0x32523e58 (volatile heap data) | 0x32523e9a | 0x32523e70 |
| gen-counter table `0x00874550` | 22 nonzero, ALL = 1 (ids 6–16, 26, 29–37, 42) | identical | identical |

The full-32 MiB hashes differ (compression-level noise lives mostly in the
low-RAM window: first word-diffs at 0x10c0–0x1210, outside the subsystem),
but every discriminant-relevant word is byte-identical across all three
images. The ~2 MB size gap is compression ratio, not content: the backup is
the same menu endpoint, a third copy of the same stage — NOT the staged
anchor (no ids in transition, no partial gate, no half-submitted job).

Verdict (1): the offline staged-anchor path is exhausted. The only three
guest states in existence are all menu-phase; the H1/H2′ difference (waiter
parked vs not at delete time) remains purely temporal with no offline
residue, exactly as slice 84 §endpoint-equivalence states.

## (2) Minimal live session — BLOCKED at the GUI gate, stopped (Confirmed)

Single bounded attempt, cheapest gate first (does the bundled emulator even
offer a scriptable path from this shell session):

- `pcsx2-qt.exe --help` from a shell: NO output, process does NOT exit
  (still alive after a 30 s `Wait-Process`; had to be `Stop-Process`d to
  leave no side effects — verified absent afterwards).
- Exact blocker: the bundled `pcsx2-qt` is a GUI-only Qt application with no
  working CLI smoke path from here — no `--help` text, no version line, no
  exit. Every one of the four slice-79 blockers (interactive boot of the
  5.3 GB ISO + minutes to menu; BIOS-ini repoint from the absent
  `F:\Games\PS2\BIOS` path; v2.9.93-state on v2.9.94-emu skew test;
  debugger/logpoint session with a positive control FIRST per PLAN §9.2–9.3)
  depends on driving that GUI interactively or on a CLI automation that does
  not exist and was never validated.

Per the slice-85 stop rule (no permanent installs without documented need,
no marathon live attempts without a positive control), the live path stops
HERE: nothing was repointed, nothing installed, no boot attempted, no
causal read taken. The emulator process was killed; `git status` clean.

## (3) Verdict and next best step toward P10

### H1/H2′ verdict: UNCHANGED — still tied (honest negative result)

- H1 (parked + delete-rescued): mechanism banked (slice-84 D4: the local
  ROM's 41h handler wakes ALL waiters; counter/allocator order evidence
  favors build-before-teardown at Hypothesis grade). This slice adds
  nothing for or against.
- H2′ (pulse-first, never parked): survives untouched (slice-84 falsified
  only the bypass variant, not the interleave variant).
- No discriminant was available offline (three menu-phase anchors, zero
  staged anchors) and none was obtainable live (GUI gate). Closing the tie
  still needs the exact experiment slice 84 named: staged savestates at the
  slice-83 break pcs (0x00101938 init, 0x005BC4C8 BUILD pass, 0x00548660
  gate park, 0x00568B94 menu) comparing A/B fields + generation table, or a
  live delete-watchpoint with a positive control — both still future work
  requiring an interactive PCSX2 session.

### Next best step: P10 traffic with F's mechanism banked

Candidate F (TEARDOWN-first) now has its revival condition SATISFIED as a
mechanism (ROM delete wakes waiters with the -1 release the game's wait
wrapper 0x00578500 is written to expect, retry re-resolves the fresh
handle) but still has NO invoker (teardown wrapper 0x00548A00: zero `jal`,
no genuine table entry, no pointer — slices 80–83) and the model still
refuses delete-with-waiters (`src/ee/kernel.cpp:943`, slice-83 D4 gap (i)).
So the concrete next slice is NOT another hunt for temporal evidence — it is
P10 design work with F's mechanism banked:

1. Bank the mechanism on paper first: write the ROM-41h wake-all + wait-(-1)
   + retry-re-resolve sequence into draft 0037 as F's accepted first-event
   chain (no implementation), with the falsifiable acceptance already
   designed (waiter's WaitSema returns the error, loop terminates on the
   fresh handle).
2. Keep shopping the invoker statically (cheap remainder): wider
   `lui`-materialization windows for 0x005485E0/0x00548A00 (slice 83 capped
   at 8 words), a `jalr`-site census around the dispatcher/boot-step
   regions, the late cluster (0x0060FF18/0x0060FFD0/0x00610000/0x00610050)
   consumer hunt — the same unknown-caller question D6 left open.
3. Only then consider adopting the two-line model change (delete releases
   waiters with -1) behind the decision record — it is now evidence-backed
   by the ROM, but still gated on an invoker, so it stays unshipped.

Tripwires re-armed (unchanged): 0x0086CBBC, B+0x00, the generation table
(any 0x23x stamp or +0x3C != 0 falsifies today's endpoint).

## Gates and hygiene

- Tree holds only this doc + the journal: `git status` clean (verified
  before and after; emulator smoke process killed, no ini touched).
- No payload bytes in docs/git: addresses, handle values, counts, relations.
- Full gates on the unmodified tree (VsDevCmd `-arch=amd64` chained):
  configure+build green; **CTest 53/53**; **Python 73 collected — OK
  (skipped=6)**.
- Hunt artifacts (extracted 32 MiB EE-RAM copies, compare script, pcsx2-help
  redirect stubs) live only in the approved host temp dir, never in the repo.
- No commit, no push, no branches.
