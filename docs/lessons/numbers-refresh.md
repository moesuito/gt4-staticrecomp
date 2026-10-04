# Numbers-refresh lesson — re-running the big counts, and the split that was subtraction

Prepared 2026-10-04. BUILD/VERIFY: no test gates moved — this
slice re-ran read-only tools, not the suite. See the sole source,
the [slice-58 evidence](../reverse-engineering/numbers-refresh-slice58.md).
EXPLAIN: this is the worked explanation; tutoring review pending.

Every load-bearing statement below traces to that document. It
records re-observed numbers and two corrections; it adds no
semantics, and neither does this lesson.

## Objective and motivation

Pinned figures rot two ways: code evolution moves them honestly,
and old splits turn out to have been computed, not measured. This
arc teaches the project's re-verification motion: hash the input
first, re-run the exact commands, check every exit status, and —
when a number moved — decide whether the code, the input, or the
old arithmetic changed before touching any pinned sentence. It
ends with three verdicts of three flavors: identical, evolved,
and wrong-from-the-start.

## Step 1 — the input before the tools (hash first)

The CORE file hashes exactly to its manifest pin (2,020,861
bytes, sha256 `85d26aa8…42ff9` against
`docs/inputs/usa-v2.00.json`) before any tool runs. That single
check divides the whole investigation space: with the input
identical, any moved number is either code evolution or old
arithmetic — never a changed game.

## Step 2 — the surveys (lifted identical, default evolved)

The lifted survey (`--functions 20000 --survey`) returns all
four M29 figures exactly: 14,991/15,067 entries, 331,035 tree
functions, 871,317 covered instructions, 76 budget rejections.
The headline 99.5% holds.

The default-policy survey moves: 14,938 → **14,943**
translating, 858,621 → **864,814** covered, and the five
start-validation edge cases vanish. Cause, at high confidence:
M29 converted exactly five out-of-text aborts into boundary
stops — count equality (5 == 5) plus mechanism match, with no
other translator acceptance change since the M28 survey (the
COP1 emitter touched no surveyed tree per M29 itself, the `jr
ra` fix changes no acceptance) and no decoder change since M24
per the log. The +16 tree functions and +6,193 covered words
are the five newly admitted trees. The lifted figures absorb
the same five either way, which is why they never moved — and
why the M27 record stays as its dated self rather than being
rewritten.

## Step 3 — the decode scan (total identical, split corrected)

Four `gt4disasm` chunks spanning exactly 0x100000..0x617A14
(1,334,917 words), every chunk exit-checked per the M20 rule,
report 0, 0, 26, 471 — total **497**, exactly the pin. But the
address-level audit falsifies the pinned split: chunk 3 holds
467 words at or after the table start 0x616F28 plus the known
four before it (BC0F `0x4100fffa` at 0x00562a0c and 0x00568490;
`0x00001028` at 0x005b9dac and 0x005b9fec), while the remaining
26 sit in chunk 2 at 0x0049999c..0x004ac1d8 — real code, all
macro function-0x38 words (`0x4a0033f8` and kin, the VU-memory
forms), including 0x004A53FC, the word the M27 spot check
already knew stops a module. M24's "493 in the table" was
497 − 4: subtraction, not addresses. The true split is **467
table + 30 real**; every downstream total stands, only the
split sentence needed the owner's line.

Two dead ends belong here. First, hand-hexed chunk strides
overshot the text end and failed loudly (exit 1) instead of
reporting — recomputed programmatically thereafter. Second, the
whole-program instruction figure (924,991) has no recoverable
counting definition: distinct comment addresses give 882,458
and emitting comment lines give 925,478 (Δ487), so it is
reported as holding-within-proxy, not re-verified. Functions
(15,068 distinct, definitions plus dispatch cases agreeing)
and size (146.6 vs 146.4 MB, rounding) re-verified exactly
from a fresh `--all` regeneration inside the time bar.

## Method rules (what the next refresh inherits)

- Hash the input against the manifest before running anything.
- Chunk boundaries computed, ending exactly at the text end;
  every chunk's exit status checked, nonzero never read as zero.
- `gt4disasm` writes UTF-16; totals come from the stderr
  family summary, splits only from stdout addresses.
- Moved numbers are findings first: code, input, or old
  arithmetic — pinned docs change by owner decision, never by
  silent update.

## Connection to our implementation

Here "implementation" is the survey/scan tooling itself:

| Piece | Mechanism (as cited) | Source |
| --- | --- | --- |
| Survey | `gt4translate CORE --survey` (default) and `--functions 20000 --survey` (lifted); grouped reason rows | slice 58 |
| Decode scan | `gt4disasm CORE start count` × 4 chunks; stderr family counts, stdout addresses | slice 58 |
| Whole program | `gt4translate CORE --functions 20000 --all 2000000 <temp>`; function defs + dispatch cases + file size | slice 58 |
| Input pin | size + sha256 vs `docs/inputs/usa-v2.00.json` | slice 58 |
| Split audit | per-word addresses vs 0x616F28; the four known words by address | slice 58 |

## Understanding checkpoint

1. The lifted survey is identical while the default survey
   moved. Explain why one figure can absorb five newly
   admitted trees without changing — and what that implies
   about which figure to pin.
2. The input hash check comes before any tool runs. What
   investigation branch does it close, and what would a
   mismatch have required before any number could be read?
3. A chunk exits 1. State the exact error this prevents if
   obeyed — and name the slice whose lesson it repeats.
4. The stderr summary gives 497; only stdout settles the
   split. Why can no family-count table, however detailed,
   replace the address list?
5. M24's split was 497 − 4 = 493. Reconstruct the most likely
   procedure that produced it, and state the single step
   that would have caught it at the time.
6. The instruction figure is reported as holding-within-proxy
   rather than re-verified. What would be needed to promote
   it — and why is the current verdict still useful?
