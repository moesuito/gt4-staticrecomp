# Slice 58 — pinned whole-text numbers refreshed (no rot, one split corrected)

Date: 2026-10-04. Method: read-only runs against the hash-verified
input (`private/fingerprint-check/CORE.GT4`: 2,020,861 bytes,
sha256 `85d26aa8…42ff9`, exactly the `docs/inputs/usa-v2.00.json`
pin). No code changes, no builds of new code, no instruments.
Scratch (disassembly listings, a 146.6 MB whole-program header)
lived in the approved temp dir and was deleted after extraction.

## Pinned vs observed

| Figure (pinned source) | Pinned | Observed now | Verdict |
| --- | --- | --- | --- |
| Survey entries translating, default policy (M27: 14,938/15,067) | 14,938, 858,621 covered, 119 + 5 + 5 edge cases | **14,943**, 316,108 fns, **864,814** covered, 119 + 5, edge row gone | **Moved by code evolution** (see cause) |
| Survey entries translating, lifted policy (M29: 14,991/15,067 = 99.5%) | 14,991, 331,035 fns, 871,317 covered (65.3%), 76 budget rejections | **identical, all four numbers** | **Same** |
| Decode scan total (M24: 497/1,334,917) | 497 (chunks 0, 0, 26, 471) | **497** (chunks 0, 0, 26, 471; every chunk exit 0) | **Same** |
| Decode split (M24: 493 table + 4 real) | 493 / 4 | **467 table + 30 real** by address (see finding) | **Pinned split wrong** (total intact) |
| Whole-program functions (M29: 15,068) | 15,068 | **15,068 distinct** (`function_*` defs + dispatch cases agree) | **Same** |
| Whole-program size (M29: 146.4 MB) | 146.4 MB | **146.6 MB** (153,678,731 bytes) | **Same** (rounding) |
| Whole-program instructions (M29: 924,991) | 924,991 | proxy 925,478 emitting `// ADDR` lines (Δ487; definition not recovered) | **Holds within proxy** (not byte-exact) |

## Cause of the default-survey move (High confidence, no doc edit by this slice)

The input is unchanged (hash above), and `git log` shows no
decoder/disassembler change since the M24 commit — so the move is
translator acceptance, and exactly one acceptance change postdates
the M28 survey: M29's out-of-text calls stopping as boundaries
instead of aborting the walk ("five in the pinned CORE, pointing
into the data segment"). The evidence fits three ways: the vanished
row counts exactly 5 (5 == 5), the mechanism converts rejections
into translations, and the only other post-M28 translator changes
(the M29 COP1 emitter, which per M29 touched no surveyed tree, and
the M30 `jr ra` emission fix, which changes no acceptance) cannot
move entry counts. The +16 tree functions and +6,193 covered words
are the five newly admitted trees. The lifted survey absorbs the
same five either way, which is why its figures never moved.

## The split finding (address-level audit of the 497)

Chunk 3 (0x501640..0x617A14) holds 471 unsupported words: 467 at
or after the documented table start 0x616F28, and 4 before it —
exactly the pinned four (`0x4100fffa` twice at 0x00562a0c and
0x00568490; `0x00001028` twice at 0x005b9dac and 0x005b9fec, the
exception-handler dump). The remaining 26 sit in chunk 2 at
0x0049999c..0x004ac1d8 — real code, all macro function 0x38 words
(`0x4a0033f8` and kin; the VU-memory forms M23 left Unsupported),
including 0x004A53FC, the word M27's spot check already knew
stops a module. M24's split (497 − 4 = 493 "in the table") was
subtraction, not addresses: the true split is **467 table + 30
real**. The total every downstream claim uses (497) is unaffected;
only the split sentence (repeated in AGENTS.md/STATUS.md) needs
the owner's one-line correction. This slice changes no doc beyond
this record, per the no-silent-update rule.

## Method notes (for the next refresh)

- Chunking must end exactly at 0x617A14 (1,334,917 words from
  0x100000); a first attempt with hand-computed hex strides
  overshot the end and failed loudly (exit 1) instead of
  reporting — the M20 exit-status rule earning its keep again.
  All addresses above were computed, not hand-hexed.
- `gt4disasm` writes UTF-16 listings; parse accordingly.
- The per-family stderr summary (`unsupported opcode=… count=…`)
  gives totals; only the stdout listing gives addresses, and only
  addresses settle split claims.
- The `--all` regeneration took ~3 minutes wall time (under the
  10-minute bar); no MSVC rebuild was attempted.
