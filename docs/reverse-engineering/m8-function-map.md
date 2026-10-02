# M8 function map evidence — evidence-backed entries and bounded reachable sets

2026-10-01: BUILD/VERIFY passed for the first slice. EXPLAIN lesson pending.

## Scope

This slice builds the function map M7-M8 asks for, in its honest form: function
entries are only created from recorded evidence, and each function's extent is
a bounded *reachable set*, never a claimed boundary.

## Entry evidence

| Evidence | Meaning |
| --- | --- |
| `elf-entry` | the image's declared entry address |
| `seed` | a caller-provided starting point (the CLI start address) |
| `direct-call` | a direct JAL target observed inside an analyzed block |

Register calls (JALR), returns, exceptions and unsupported words never create
entries. A jump target is not entry evidence: the M7 evidence showed the seeded
function jumping across regions into startup code, which is a flow edge, not
necessarily a new function.

## Discovery rules

- Traverse each entry with the M7 CFG walker under a per-function block cap.
- Every direct call target found inside becomes a new candidate
  (deduplicated, in discovery order).
- Traversal continues until no candidates remain or the function cap is
  reached; a capped run records the remaining candidates as `pending`.
- All seeds are validated before any traversal, so an invalid start is
  reported even when a cap would otherwise stop before reaching it. (A bug
  this slice's tests caught: an outside-text seed had silently gone unreached
  under `max_functions=1`; a regression fixture now covers it.)

## Real evidence

```powershell
.\build\gt4funcs.exe private/fingerprint-check/CORE.GT4 0x5a3140 50 500
```

```text
function=0x00100008 evidence=elf-entry blocks=1 instructions=1 open_ends=1 limited=0 call_targets=none
function=0x005a3140 evidence=seed blocks=15 instructions=71 open_ends=1 limited=0 call_targets=0x005b78a0
function=0x005b78a0 evidence=direct-call blocks=8 instructions=20 open_ends=2 limited=0 call_targets=0x005ae100,0x005ae110,0x005b9af0
function=0x005ae100 evidence=direct-call blocks=1 instructions=2 open_ends=1 limited=0 call_targets=none
function=0x005ae110 evidence=direct-call blocks=1 instructions=2 open_ends=1 limited=0 call_targets=none
function=0x005b9af0 evidence=direct-call blocks=2 instructions=21 open_ends=1 limited=0 call_targets=0x005b07d0
function=0x005b07d0 evidence=direct-call blocks=6 instructions=36 open_ends=2 limited=0 call_targets=0x005b72a8,0x005af850,0x005b72f8
function=0x005b72a8 evidence=direct-call blocks=1 instructions=1 open_ends=1 limited=0 call_targets=none
function=0x005af850 evidence=direct-call blocks=1 instructions=18 open_ends=1 limited=0 call_targets=none
function=0x005b72f8 evidence=direct-call blocks=1 instructions=1 open_ends=1 limited=0 call_targets=none

funcmap functions=10 blocks_sum=37 instructions_sum=173 discovered_calls=8 pending=0 limited=0
```

Observations, kept as evidence rather than conclusions:

- The transitive direct-call closure from the seed completed (`pending=0`,
  `limited=0`): 10 functions, 8 discovered direct calls, 37 reachable blocks
  summed.
- `0x00100008` (the real entry) is a single unsupported block: the startup
  window needs COP1/MMI decoding before the map can bootstrap from the entry
  itself.
- Two functions are exactly 2 instructions (`0x5ae100`, `0x5ae110`) with one
  open end each — thunk-shaped evidence that the map represents tiny
  trampolines distinctly.
- `0x5af850` is 18 instructions in a single block with one open end: a long
  straight-line body ending in a dynamic transfer.
- Reachable sets can overlap (the seed's 15 blocks include the jump into
  startup code), so block sums are not a partition of the text.

## Verification

- Unit fixtures: call discovery and evidence classification, per-function and
  global caps (including the pending queue), seed deduplication, zero-cap
  rejection and the late-invalid-seed regression.
- Optional Python CLI checks assert the real first two lines, the discovered
  `0x005b78a0` record, the capped pending output and malformed arguments.
- No decoder output changed, so the M6 Ghidra comparison remains valid
  (`matched=417 non_nop=352 unsupported=71 mismatched=0`).

## Limits

Entries are evidence-backed; extents are not boundaries. Static reachability
does not prove execution, and unsupported words can hide both code and calls.
Later slices can seed from external knowledge (M2/M3 manifests, observed
starts) and eventually from execution traces — the same map mechanism with new
evidence kinds.

Next: the M7/M8 lessons, then M9 (explicit guest state and memory model).
