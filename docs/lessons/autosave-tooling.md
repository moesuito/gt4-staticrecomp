# Autosave lesson — spec first, legs second, and the bug the re-run caught

Prepared 2026-10-04. BUILD/VERIFY: CTest 50/50 (42 prior + 8
new), Python 73 (67 run, 6 skip). See the
[decision 0027](../decisions/0027-autosave-checkpoints.md) and the
[slice-56 evidence](../reverse-engineering/autosave-slice56-implemented.md).
EXPLAIN: this is the worked explanation; tutoring review pending.

Every load-bearing statement below traces to one of those two
sources. Helper and test names were re-verified against
`tools/gt4boot/main.cpp` by grep before citing.

## Objective and motivation

Manual checkpoints work and chains preserve progress, but every
photo is hand-placed: a 243M-service march needs an operator
watching the stops. This arc teaches the project's spec-first
motion — write the exact bar before the product change, like
decision 0026 did — then implement inside the bound the spec
draws. It ends with unattended bounded photos (eight new tests,
50/50 green) and one genuine defect found not by review but by
re-running the suite into populated directories.

Two constraints shape everything: a photo is only ever taken at
a clean stop the existing predicate already defines, and the
implementation touches the tool plus the checkpoint header and
tests — never the model, the formats, or the decoder.

## Step 1 — the spec (slice 55, decision 0027)

The plan's §5 specified rotating photos plus quiet mode and was
never implemented — a read-only grep confirmed zero hits for
`checkpoint-every` while the three manual flags existed. The
decision fixes the semantics before any code:

- **CLI surface.** `--checkpoint-every K DIR --keep M
  [--max-bytes B]` plus `--quiet`. Photos are named
  `ckpt-<S>.bin` with S cumulative since boot, so names sort
  and compare by service count. Zero counts and dangling
  `--keep`/`--max-bytes` are usage errors, not silent defaults.
- **Clean moments only, never forced.** Photo moments are the
  multiples of K at or below the final count, each gated by the
  0022 clean-stop predicate (syscall boundary, no transfer in
  flight). A dirty moment is skipped quietly — counted and
  reported once at the end (`checkpoint skipped at <S>
  (<reason>)`) — never thrown. Unlike `--checkpoint-at`'s loud
  refusal, autosave must survive its dirty stretches: a 10-hour
  push dies on nothing. Failure is never photographed; resume
  always starts from the last healthy photo.
- **Rotation and eviction, oldest first.** After each photo:
  delete beyond the newest M by service number, then delete
  oldest-first past the byte cap — except the just-written
  photo, which its own run never evicts (a lone over-cap photo
  stays, reports, and becomes evictable later). Only
  `ckpt-<n>.bin` names are managed, including pre-existing
  ones (rotation continues across chained resume legs);
  foreign files are never touched, and a manual
  `--checkpoint-at` PATH inside the DIR is refused loudly.
- **Determinism and interchange (load-bearing).** Photo timing
  is a pure function of the handled-service count — the same
  rule as the 0026 trigger and the service clock — and an auto
  photo at S must load via `--resume` and verify via
  `--verify-resume` interchangeably with a manual one.
  Autosave combines with `--resume` (chained autosave, counts
  cumulative from the file's base) and stands alone from
  `--verify-resume` and `--compare-interpreter`.
- **Bounded implementation.** `tools/gt4boot/main.cpp` only
  (parse, a factored save helper, the leg hook, pure
  inventory/eviction helpers, the quiet gate) plus tests in
  existing fixtures: synthetic unit cases for eviction, one or
  two CTests in the `gt4boot_checkpoint` pattern plus a verify
  on an auto photo.
- **The five-item bar.** Gates green with `--checkpoint-at`
  byte-behavior untouched; rotation asserted; interchange
  proven; determinism across identical runs; quiet measured.

## Step 2 — the implementation (slice 56)

The save block becomes `write_stop_checkpoint` — the same
calls in the same order, now shared — so both paths write
byte-identical files for the same stop state. `run_with_autosave`
runs the boot leg by leg: each leg handles up to the next photo
multiple (`next_autosave_photo`, first multiple strictly above
the count, max-sentinel when unreachable), applies the clean
predicate, writes through the shared path, and rotates
(`inventory_autosave_dir` plus `apply_autosave_rotation`, which
reports every deletion). A short leg ends the run — the machine
stopped on its own — with the reason classified by
`skip_reason_text` (step limit / idle / transfer in flight /
fault with kind). Step and service budgets span legs, so
`--steps`/`--services` mean the same totals with or without
autosave, and the returned stats accumulate command-relative to
honor the resume-recount rule. `--quiet` gates only the
per-service `on_service` print; boundary, stats, photo, skip,
eviction, dumps, and errors still print.

The pure helpers live header-inline in `ee_checkpoint.hpp`
(`format_autosave_name`, `parse_autosave_name` refusing
everything not exactly `ckpt-<digits>.bin` including overflows,
`select_autosave_evictions` with oldest-first ordering) — no
format touched. Eight CTests reuse the fixture pattern:
`gt4boot_autosave` (photos at 400 + 800), foreign setup plus
rotation plus intact-check (`cmake -E copy` drops the file,
`md5sum` proves it survived), verify on the auto ckpt-400
(`resume states identical (800 …)`), quiet (boundary and photo
present, ` at 0x` absent), and repeat plus `compare_files` for
determinism.

## Step 3 — the bug the re-run caught (upsert)

The first full suite passed 48/48 into fresh directories. The
first filtered re-run failed `gt4boot_autosave_verify` with
`Cannot open …/ckpt-400.bin`: the startup inventory had kept
the stale ckpt-400 entry, the rewrite pushed a duplicate, and
rotation (4 entries > keep 3) deleted the file just written.
The fix is an upsert on write — erase same-count entries
before pushing — and the re-run that caught it is now the
regression proof, because every later suite re-runs into
populated directories. A second, cheaper failure belongs here
too: multi-line `PASS_REGULAR_EXPRESSION` lists were rejected
at configure time, fixed with single-line `;`-joined patterns
on the `gt4boot_originating` precedent.

## What this arc does not claim (notes for the reviewer)

- The decision refuses `--compare-interpreter` with autosave
  while citing "the same rules as `--checkpoint-at`" — but
  `--checkpoint-at` allows compare (only verify is refused
  there). Implemented literally; one line to relax.
- Manual files store the command-relative count (chain
  convention); auto photos store the cumulative count. Both
  load and verify through the same code — interchangeable as
  proofs, not byte-identical under resume.
- Rotation is strictly by service number (unit-pinned): a stale
  higher-numbered file can evict a fresh photo. Deliberate per
  the text; a DIR is one run's scratch.

## Connection to our implementation

| Piece | Mechanism (as cited / grep-verified) | Source |
| --- | --- | --- |
| Shared save | `write_stop_checkpoint` — same calls, same order; byte-identical files | slice 56 |
| Leg driver | `run_with_autosave` — legs to the next multiple, clean predicate, skip-and-stop, cumulative stats | slice 56 |
| Moments | `next_autosave_photo` — pure function of the count, max-sentinel | slice 56 |
| Reasons | `skip_reason_text` — step limit / idle / transfer / fault(kind) | slice 56 |
| Rotation | `inventory_autosave_dir` + `apply_autosave_rotation` over `select_autosave_evictions`; upsert on write | slice 56 |
| Names | `format_autosave_name` / `parse_autosave_name` (`ckpt-<digits>.bin`, overflow refused) | slice 56 |
| Quiet | `--quiet` gates only the `on_service` trace line | slice 56 |
| CLI + rules | K/M/B validation, combination refusals, manual-in-DIR refusal | 0027 |
| Unit cover | name round-trips + ten refusal shapes; keep/cap/fresh/stale-higher eviction cases | slice 56 |
| Boot cover | 8 CTests: photos, rotation, foreign intact, auto-verify, quiet, repeat, identical | slice 56 |

## Understanding checkpoint

1. `--checkpoint-at` refuses loudly on a dirty stop while
   autosave skips quietly and continues. Why is each rule
   correct for its use — and what breaks if you swap them?
2. Photo moments are a pure function of the handled-service
   count. Name two things that would silently break if wall
   time entered any save/skip/evict decision.
3. The save helper was factored instead of duplicated. What
   exact property does sharing give the interchange proof
   that two parallel implementations could not?
4. Budgets span legs rather than resetting per leg. Construct
   the observable difference for `--steps N` under each
   choice, and explain which one preserves the flag's meaning.
5. The upsert fix erases same-count entries before pushing.
   Walk the failure without it: which entry does rotation
   pick, and why is the victim always the file just written?
6. Rotation is strictly by service number, so a stale higher
   file can evict a fresh photo. Argue for and against
   protecting freshness — and state which side the adopted
   text takes.
