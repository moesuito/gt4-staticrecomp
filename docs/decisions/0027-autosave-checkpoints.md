# 0027 — Autosave checkpoints: rotating photos plus quiet mode

Date: 2026-10-04. Status: accepted (specification only; implementation
is slice 56). Plan: `docs/plans/checkpoint-resume-mapping.md` §5.
Predecessors: 0022 (checkpoint/resume semantics and proof), 0024
(chained checkpoints, idle budget).

## Context

Manual checkpoints work (`--checkpoint-at`, decision 0022) and chains
preserve progress across legs (decision 0024), but every photo is
hand-placed: a 243M-service march needs an operator watching the
stops. The checkpoint plan's §5 specifies rotating autosave photos
plus a quiet mode for long pushes; it was never implemented — a
read-only grep confirms `tools/gt4boot/main.cpp` has zero hits for
`checkpoint-every` while `--checkpoint-at` / `--resume` /
`--verify-resume` are all present. Like decision 0026, this document
specifies first so slice 56 implements against a fixed bar instead
of discovering the semantics mid-code.

## Decision

### 1. CLI surface

```text
gt4boot CORE.GT4 --services N --checkpoint-every K DIR --keep M [--max-bytes B] [--quiet]
```

- `--checkpoint-every K DIR`: take a photo every K handled services
  into directory DIR (created when missing). `K == 0` is a usage
  error. Filenames are fixed: `ckpt-<S>.bin` where S is cumulative
  services handled since boot (resume base plus leg count, decimal,
  no padding), so names sort and compare by service count.
- `--keep M`: retain the newest M photos; `M == 0` is a usage error.
- `--max-bytes B` (optional): total-bytes cap over the retained
  photos; absent means count-only rotation. `B == 0` when given is
  a usage error.
- `--quiet`: suppress the per-service `service 0x…` line (the
  `options.on_service` print). Everything else keeps printing:
  boundary, stats, `checkpoint saved …` / `skipped …` notes,
  resume/verify lines, dumps and thread tables when requested,
  errors. Exit codes unchanged.

### 2. When a photo is taken (clean only, never forced)

Photo moments are the service counts S in {K, 2K, …} at or below
the run's final handled count. At each moment the tool applies the
existing clean-stop predicate (decision 0022, the save block's
check): the stop is a syscall boundary with no transfer in flight.
A photo is written iff the predicate holds at that count;
otherwise the moment is **skipped quietly** — counted and
reported once at the end (`checkpoint skipped at <S> (<reason
class: fault / step-limit / idle / transfer-in-flight>)`), never
thrown. Consequences:

- A fault, a step limit, an idle stop, or a transfer in flight
  never produces a photo: **failure is never photographed**.
  Resume always starts from the last healthy photo, shortening
  replay without eliminating it.
- Unlike `--checkpoint-at` (loud refusal), autosave never aborts
  a run over a missed moment — a 10-hour push must survive its
  dirty stretches, not die on the first.

### 3. Rotation and eviction (newest wins, foreign files untouched)

After each written photo, in order:

1. Delete managed photos beyond the newest M by service number.
2. While `--max-bytes` is set and the managed total exceeds it,
   delete the oldest — except the photo just written: the newest
   photo is never evicted by the run that wrote it (if one photo
   alone exceeds the cap it stays, is reported, and becomes
   evictable by the next photo).

Managed means names matching `ckpt-<digits>.bin` in DIR,
including pre-existing ones inventoried at startup (rotation
continues across chained resume legs). Anything else in DIR is
never touched. Combining `--checkpoint-at PATH` with autosave is
allowed, but a manual PATH inside the autosave DIR is refused
loudly at startup — rotation owns every `ckpt-<n>.bin` there and
must not eat a hand-placed file.

### 4. Determinism and interchange (the load-bearing rules)

- Photo timing is a pure function of the handled-service count
  (the guest sequence), never wall-clock — the same determinism
  rule as the 0026 trigger and the 0016 clock. No host timing may
  enter any save/skip/evict decision.
- An auto photo at S is built by the same code path as a manual
  `--checkpoint-at S` photo and must be usable interchangeably:
  loadable by `--resume`, verifiable by `--verify-resume`
  against the direct total. Resume bit-identity (decision 0022's
  proof) is preserved, not redefined.
- Autosave combines with `--resume` (chained autosave; photo
  counts stay cumulative from the file's base, per decision
  0024's leg-relative recount), `--services`, `--disc`,
  `--steps`, `--threads`, `--dump`, `--quiet`. It stands alone
  from `--verify-resume` and `--compare-interpreter`, under the
  same rules as `--checkpoint-at`.

### 5. What slice 56 touches (bounded implementation)

- `tools/gt4boot/main.cpp` only: usage text, argument parsing and
  combination rules (beside the existing checkpoint block);
  factoring the save block into a `save_stop_checkpoint`-style
  helper reused by `--checkpoint-at` and the every-K hook;
  driving the hook from the service loop (the `on_service`
  neighborhood); a pure inventory/eviction helper
  (`select-evictions`-style: inventory in, names-to-delete out —
  unit-testable without the filesystem); DIR inventory at
  startup; the `--quiet` gate on the per-service print.
- No model, decoder, interpreter, kernel, or device changes. No
  format changes (`GT4CPT1`/`GT4KERN1`/`GT4BANK1` untouched).
- Tests reuse existing fixtures only: unit cases for the
  eviction helper (synthetic inventories: keep-N, cap overflow,
  single-photo-over-cap, foreign files ignored) inside the
  existing checkpoint test file(s); one or two CTests in the
  existing `gt4boot_checkpoint` pattern — a short
  `--checkpoint-every K DIR --keep N` leg asserting photos land
  on clean multiples, the count never exceeds N, the oldest is
  deleted, one auto photo passes `--verify-resume` against the
  direct total, and `--quiet` drops the `service 0x` lines while
  keeping boundary/stats. No parallel harnesses.

## Verification bar (slice 56 must meet all)

1. **Gates green**, especially every existing checkpoint CTest
   and differential (the refactor of the save block must not move
   `--checkpoint-at` behavior by a byte).
2. **Rotation asserted**: post-leg DIR holds at most M managed
   photos, they are the newest service counts, evicted names are
   gone, foreign files intact, over-cap totals evicted
   oldest-first.
3. **Interchange proven**: an auto photo through `--verify-resume`
   equals the direct total (same stop, same states, same digest)
   — decision 0022's proof, reused binary.
4. **Determinism held**: photo moments and skip notes are
   functions of the service sequence; two identical runs name
   identical photos.
5. **Quiet measured**: no per-service lines on stdout under
   `--quiet`; boundary, stats, photo/skip notes, and errors
   still present.

## Consequences

- Long pushes become unattended: photos accumulate at every K
  clean services, bounded by M files and B bytes, chatter-free.
- A dirty stretch costs photos, never the run; a crash costs at
  most the services since the last healthy photo.
- Out of scope: resuming *with* autosave re-armed past a crash
  loop (operator restarts the command), photo compression,
  remote/off-machine photo stores, and any use of photos beyond
  debugging (decision 0022's scope stands).
