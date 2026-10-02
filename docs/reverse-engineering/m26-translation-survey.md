# M26 — the whole-text translation survey

Date: 2026-10-02. Inputs: the pinned CORE. Tool: `gt4translate CORE.GT4
--survey` (new mode; the walk logic was factored into `collect_units` so the
same validation runs for one function and for the survey).

## What the survey does

Every direct-call target inside the text becomes a candidate function entry
(15,067 unique `jal` targets over 97,269 call sites). Each entry is walked
with the standard call-tree translation — the same validation the normal mode
uses, bounded by 20,000 instructions and the 256-function limit — and the
outcome is recorded: translated, or the rejection reason grouped. The report
also counts the union of instructions the successful trees reach.

## Results (pinned CORE, 2026-10-02)

```
survey: entries=15067 translated=9345 functions_in_trees=58397
covered instructions: 399046 of 1334917 words in the file-backed text
reason: 4576 x Indirect calls are not supported
reason: 1055 x Not supported by this translator
reason: ~100 x no reachable instructions (jal targets in data or misaligned spots)
reason: 5 x start validation edge cases
```

- **62% of the direct-call targets translate today** as standalone trees
  (9,345 of 15,067), covering **399,046 instructions — about 30% of the real
  code region** (1,334,218 words).
- The dominant blocker is **indirect control flow**: 4,576 trees fail on a
  `jalr` call and 1,055 on a computed `jr` (jump tables and function-pointer
  dispatch). Together they account for 93% of the rejections.
- The remaining rejections are `jal` targets that are not code (data words
  hit by stray call sites) or edge cases in the start validation.

## What this means

The translator's instruction-level coverage is effectively complete for the
code region (four unmodeled words total, all in the exception handler and the
DMA-dependent BC0F); what limits translation now is **control flow structure**,
not instruction semantics. The next milestone is indirect control flow:
register a table of statically known function entries (the survey already
knows them) and translate `jalr`/computed `jr` as a dispatch through it that
stops with context on unknown targets.

## Evidence

- The survey output above is reproducible: `gt4translate
  private/fingerprint-check/CORE.GT4 --survey` (runs in ~15 seconds).
- A Python CLI smoke test exercises the mode and asserts the report shape and
  the dominant blocker (`test_translate_cli`).
- CTest 24/24; Python 72 collected (66 run, 6 skip).

## Limits recorded

- The survey counts an entry as translated only when its whole direct call
  tree validates; a function reachable inside another tree still counts as a
  failure of its own entry. The 62% is therefore a lower bound on function
  coverage, and the 30% instruction coverage counts only successful trees.
- `jal` targets inside the trailing data table (700 words) are filtered by
  the text range but stray call words inside code can still name non-code
  addresses; those appear as "no reachable instructions" and are harmless.
