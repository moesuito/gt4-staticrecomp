# Slice 56 — autosave implemented (decision 0027)

Date: 2026-10-04. Inputs: the pinned CORE (existing fixtures only:
400/800-service legs, no disc). Status: BUILD/VERIFY green —
CTest 50/50 (42 prior + 8 new), Python 73 (67 run, 6 skip).

## What changed (product code)

Bounded to the tool plus the checkpoint header/tests, per the
decision. No model, decoder, interpreter, kernel, device, or
format changes (`GT4CPT1`/`GT4KERN1`/`GT4BANK1` untouched).

- `tools/gt4boot/main.cpp`:
  - Usage text gains `--checkpoint-every K DIR`, `--keep M`,
    `--max-bytes B`, `--quiet`.
  - Argument parsing for the four flags; usage errors on `K == 0`,
    missing/non-positive `--keep`, `--keep`/`--max-bytes` without
    `--checkpoint-every`, and zero `--max-bytes` when given.
  - Combination rules: autosave refuses `--verify-resume` and
    `--compare-interpreter`; a manual `--checkpoint-at` PATH
    inside the autosave DIR is refused loudly (rotation owns
    every `ckpt-<n>.bin` there).
  - `write_stop_checkpoint` factors the save block byte-for-byte
    (same calls, same order) and is now shared by
    `--checkpoint-at` and autosave photos.
  - `run_with_autosave` runs the boot leg by leg: each leg handles
    up to the next photo multiple (a pure function of the service
    count), applies the 0022 clean-stop predicate, writes
    `ckpt-<cumulative>.bin` through the shared path on clean legs,
    and records a skip note plus stops on dirty legs. Step and
    service budgets span legs, so `--steps`/`--services` mean the
    same totals with or without autosave. Returned stats
    accumulate across legs (services command-relative, honoring
    the resume-recount rule), so the report and the manual block
    read totals.
  - `next_autosave_photo` (first multiple strictly above a count,
    max-sentinel when unreachable), `skip_reason_text` (step
    limit / idle / transfer-in-flight / fault with kind),
    `inventory_autosave_dir` (managed names plus sizes; foreign
    files ignored, unreadable sizes skipped), and
    `apply_autosave_rotation` (deletes plus reports each pick,
    keeps the inventory in step).
  - `--quiet` gates only the per-service `options.on_service`
    print; boundary, stats, photo/skip/eviction notes, dumps,
    and errors still print.
- `include/gt4recomp/ee_checkpoint.hpp`: three pure helpers with
  no filesystem or model dependency — `format_autosave_name`,
  `parse_autosave_name` (refuses everything not exactly
  `ckpt-<digits>.bin`, overflow included), and
  `select_autosave_evictions` (beyond-newest-M by service
  number, then oldest-first past the byte cap sparing the
  just-written photo). Header-inline; no format touched.
- `tests/unit/ee_checkpoint_test.cpp`: name round-trips plus ten
  refusal shapes; rotation (beyond-keep, fitting keep, empty
  dir), byte-cap oldest-first, lone over-cap fresh photo stays,
  and the documented strict-by-number case.
- `CMakeLists.txt`: eight new CTests in the existing fixture
  pattern — `gt4boot_autosave` (photos at 400 + 800),
  `gt4boot_autosave_foreign_setup` (drops a foreign file via
  `cmake -E copy`), `gt4boot_autosave_rotation` (keep 2 over
  200/400/600/800, eviction messages pinned),
  `gt4boot_autosave_foreign_intact` (`md5sum` proves survival),
  `gt4boot_autosave_verify` (`--verify-resume` on the auto
  ckpt-400 against the direct 800),
  `gt4boot_autosave_quiet` (boundary + photo present, ` at 0x`
  absent), `gt4boot_autosave_repeat` (identical run elsewhere),
  `gt4boot_autosave_identical` (`cmake -E compare_files` on the
  two ckpt-800 files).

## The five bar items, with exact evidence

1. **Gates incl. untouched `--checkpoint-at` byte-behavior.**
   CTest 50/50; `gt4boot_checkpoint`, `gt4boot_resume_verify`,
   and `gt4boot_checkpoint_chain` pass unmodified (same
   commands, same `PASS_REGULAR_EXPRESSION`s). The save refactor
   is call-identical, and the chain/verify legs load its files.
2. **Rotation asserted incl. foreign files intact.**
   `gt4boot_autosave_rotation` pins both eviction messages
   (`ckpt-200.bin`, `ckpt-400.bin` beyond keep 2) and the final
   photo; `gt4boot_autosave_foreign_intact` proves the foreign
   file survived the same run; the unit tests prove foreign
   names never enter the inventory (ten refusal shapes).
3. **Interchange via `--verify-resume` on an auto photo.**
   `gt4boot_autosave_verify` prints `resume states identical
   (800 services …)`: auto ckpt-400 + 400 equals direct 800.
4. **Determinism across identical runs.**
   `gt4boot_autosave_identical` byte-compares the two runs'
   ckpt-800 files (exit 0 = identical).
5. **Quiet measured.** `gt4boot_autosave_quiet` requires
   `boundary:` and the photo line present while ` at 0x` (the
   per-service trace shape `service 0x… at 0x…`) matches
   nowhere.

## Two failures caught by the slice's own tests

1. **CMake multi-line property lists rejected at configure
   time.** `set_tests_properties` with `PASS_REGULAR_EXPRESSION`
   values on following lines failed configuration twice
   (`incorrect number of arguments`). Fix: single-line `;`-joined
   patterns, matching the in-repo `gt4boot_originating`
   precedent. Lesson: this CMake setup wants one line per
   property value list.
2. **Re-runs into a populated DIR evicted the fresh photo.**
   The first filtered re-run failed `gt4boot_autosave_verify`
   (`Cannot open …/ckpt-400.bin`): the startup inventory kept
   the stale ckpt-400 entry, the rewrite pushed a duplicate, and
   rotation (size 4 > keep 3) deleted the file just written.
   Fix: upsert on write (erase same-count entries before
   pushing). The re-run that caught it is now the regression
   proof — every later suite re-runs into populated dirs.

## Notes for the reviewer (no action hidden)

- The decision says autosave "stands alone from
  `--verify-resume` and `--compare-interpreter`, under the same
  rules as `--checkpoint-at`". Implemented literally: both
  refused. Flagging the tension: `--checkpoint-at` itself
  *allows* `--compare-interpreter` (only verify is refused
  there), so the analogy sentence points the other way for
  compare. One-line change to allow it if preferred; the
  reference-total math already covers legs
  (`resume_services + service_limit` equals the cumulative).
- Manual `--checkpoint-at` files store the command-relative
  count (existing chain convention); auto photos store the
  cumulative count (decision 0027). Both load and verify
  through the same code; they are interchangeable as proofs,
  not byte-identical under resume.
- Rotation is strictly by service number (unit-pinned
  `stale_higher` case): a stale higher-numbered file can evict
  a fresh photo. Deliberate per the decided text; a DIR is one
  run's scratch, not shared storage.
- No TEMPORARY instruments were added; no scratch logs were
  created (all evidence is CTest output). Autosave
  directories live under `build/` (ignored).
