# M2–M5 foundation notes — inputs, hashes, decoder (retroactive)

Prepared 2026-10-04. Sources: `docs/inputs/usa-v2.00*.json`
(manifests), `docs/reverse-engineering/m6-disassembly.md` (method
and region manifest), and the STATUS M2–M6 bullets. No M2–M5-era
evidence documents exist — these notes are retroactive by
construction, and every section below says exactly what backs
it. Where the record is thin, the gap is labeled instead of
backfilled. EXPLAIN: worked explanation; tutoring review pending.

## Objective and motivation

Everything after M6 stands on four facts established before any
game code ran: the disc input is pinned, a reference ELF exists,
our own image matches it byte for byte, and the decoder names
nearly every word. Later slices cite these the way mathematics
cites axioms — input hashes rejected on mismatch, unsupported
words counted over the whole text, Ghidra as the independent
reader. This note writes down what those foundations actually
establish, from the manifests that pin them, because a lesson
written three weeks later must not upgrade "accepted then" into
"proved now".

## Step 1 — M2: the pinned input (manifest, not bytes)

`docs/inputs/usa-v2.00.json` pins the disc without containing a
single payload byte:

```text
disc sha256 67b6c007…2e824, 5314478080 bytes
CORE.GT4;1  sha256 85d26aa8…fd642ff9, 2020861 bytes
SCUS_973.28;1 sha256 f8f10823…794dd8019fa, 273020 bytes
SYSTEM.CNF;1 sha256 7bb2979e…2e5a9272, 57 bytes
serial SCUS-97328, version 2.00, NTSC, boot /SCUS_973.28;1
```

Verification rejects changed inputs; it never updates a
manifest. That one-way rule is the whole lesson: the manifest
is older than every conclusion drawn from the input, so no
conclusion can quietly move it. (Provenance gap, stated: no
M2-era run log survives in the tree — the manifest's schema and
the verifier that enforces it are the evidence, not a dated
experiment write-up.)

## Step 2 — M3/M4: two ELFs and what "identical" covers

The reference run (upstream) reconstructs the executable image
from the CORE: entry `0x00100008` (1048584), three payload
records inflated and matched per-record hashes (24 bytes to
`0x006179FC`, 5,339,668 text bytes to `0x00100000`, data after),
reference size 6,127,896 bytes at sha256
`94aada89…c1943016c`. Our native image repeats the
reconstruction under the reference analysis policy (8 MiB
zero-fill, reginfo relocation — both labeled policy, neither
claimed as the real runtime loader) and lands byte-identical
where the policy defines bytes: native ELF 6,123,004 bytes at
`10f82e22…935c`, all three payload records matched, loaded
layout matching, alignment valid. "Byte-identical to the pinned
hash here" means exactly this much and no more — the zero-fill
ranges carry the explicit caveat "real runtime extent
unverified".

## Step 3 — M5/M6: the decoder and its independent reader

The decoder names 349 operations (line-filtered count; older
documents' 175 counted comment fragments — a counting
correction, not a coverage change). Whole-text accounting,
corrected after M24: 497 unsupported of 1,334,917 words, of
which 493 sit inside the 700-word **data table** at the text
section's tail (`0x616F28..0x617A14`) — data, not code —
leaving **4 real code words**: two BC0F whose condition needs
a DMA model, two unassigned function-`0x28` words inside the
exception handler. The first 350,000 words decode cleanly.

The independence claim rests on Ghidra 12.1.3
(`MIPS:LE:64:64-32addr`, pseudo-disassembler, one word at a
time, mnemonic+operands with alias normalization): the M6
ten-region run (entry/startup windows, constructed addresses,
JAL/J destinations — inspection windows, not claimed
functions) matched 417 with 0 mismatches (352 non-NOP, 71
unsupported); the M16 listing matched 594 with 0 mismatches
plus 34 R5900-only rows verified against the reference tables
instead. The Loop is closed by tooling, not trust:
`gt4disasm` reads the original text record directly (no
synthetic BSS, no intermediate ELF), shares the hash-checked
core reader, and exits nonzero on input/range/output failure —
*check the tool's exit status, not only its output*, after the
M20 false-positive lesson (chunk ranges past the text end read
as clean until the ERET sighting exposed it).

Reproduce it (Developer PowerShell, paths from the M6 doc):

```powershell
.\build\gt4disasm.exe private/fingerprint-check/CORE.GT4 0x10011c 31
python scripts/sample_disassembly.py
```

Count means instructions; regions come from
`docs/inputs/usa-v2.00-disassembly-regions.json`; game words
stay in ignored local output.

## What this foundation does not cover (thin record, labeled)

- No M2–M5 slice docs, journals, or run logs survive: the
  *decisions* behind early shapes (19-operation decoder,
  region choices, the reference policy's 8 MiB fill) are
  reconstructed from manifests and code, not minutes. Treat
  motive claims about that era as Hypothesis.
- The decoder's 349 count is line-filtered tooling output, not
  an audited census; the load-bearing numbers are the
  unsupported-word counts and the zero-mismatch comparisons.
- "Upstream run" for M3 means exactly the reference
  reconstruction artifacts pinned above — not an endorsement
  of any upstream branch state.

## Connection to our implementation

| Piece | File | Job |
| --- | --- | --- |
| Pinned metadata | `docs/inputs/usa-v2.00*.json` | hashes/sizes, never payload |
| Hash-checked core read | `tools/common/verified_core.cpp` | size+SHA-256 gate for every frontend |
| Image reconstruction | `src/executable/` + analysis ELF policy | per-record matches, layout checks |
| Decoder | decoder tables + whole-text scan | 349 ops, 4 real unsupported words |
| Independent reader | `tools/gt4disasm` + region manifest | Ghidra-compared listings |
| Learned commands | `scripts/sample_disassembly.py`, README build lines | reproducible windows |

## Understanding checkpoint

1. Verification "never updates a manifest." Construct the
   failure mode that rule prevents, using a hypothetical
   changed-ISO run whose author is tempted to "fix" the hash.
2. The native ELF is byte-identical "where the policy defines
   bytes". Which ranges does the policy *not* define, and what
   caveat travels with them?
3. 493 of 497 unsupported words sit in a data table. Why does
   that change the meaning of "fully decoding text" — and what
   are the 4 words that still count as real gaps?
4. Ghidra matched 417 with 0 mismatches while 71 words went
   unsupported. Explain why "unsupported" and "mismatched" are
   different verdicts, and which one would falsify the decoder.
5. The M20 false positive came from chunk ranges past the text
   end. Reconstruct the error mechanically, and state the
   standing rule it produced.
6. No M2–M5 slice docs exist. Which claims in this note rest
   on manifests alone, and which single check would most
   strengthen the thinnest one?
