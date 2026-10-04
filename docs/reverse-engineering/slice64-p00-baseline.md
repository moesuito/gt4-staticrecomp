# Slice 64 (P00) — checkpoint baseline and compatibility policy

Date: 2026-10-04. Status: implemented, 50/50 CTest + Python 73 (6 skips).
Decision: `docs/decisions/0028-p00-checkpoint-baseline-and-compatibility.md`.

## Baseline pinned here

- Commit: `58de2cf` (main, clean tree except this slice's files).
- Binary: `gt4boot` Debug, MSVC 1951 (BuildTools 18), Ninja.
- Inputs: CORE.GT4 `85d26aa8...642ff9` (verified at load by
  `read_verified_core`); ISO `67b6c007...e824f` per
  `docs/inputs/usa-v2.00.json` (hash checked in the fingerprint step, not
  at `--disc` open — open gap, recorded below).
- Config: service-clock 1 ms/service, idle 1 frame/interrupt, idle budget
  2,000,000; whole-program AOT module (15,068 functions).
- Model semantics baseline: time=1, interrupt=1, rpc=1, translation=1
  (all domains start at 1; see the decision for the bump rule).

## Restore-path audit (item 5)

| Path | Finding |
|---|---|
| `DmaChannel::restore_registers` | Already bypassed (writes `bank_` storage directly). No `raise_` call on the restore path — Confirmed by code + new test. |
| `RegisterBank::restore_registers` | **Shared the guest write entry point** (`write_register` per entry). Behaviorally identical today (the write path is plain storage), but any future W1C/mask/completion effect added to `write_register` would have leaked into restores. Fixed: writes `registers_` storage directly (device contract untouched). |
| `TimerUnit::restore_registers` | Delegates to the bank restore — fixed transitively. |
| `Kernel::load_kernel_state` | Parses into temporaries, assigns wholesale; restores the queued-interrupt list without delivering anything — no device interaction. No change needed. |
| `restore_banks` (gt4boot) | Only calls the per-device restores above — side-effect free after the fix. |

New regression (in the existing `ee_device` fixture, no parallel
harness): a live `STR|TIE` write through mapped memory fires exactly once
and clears STR; restoring a snapshot of that completion fires nothing;
restoring crafted entries with `STR|TIE` set keeps the bits verbatim
with no fire — the live path would have fired and cleared. This is the
discriminating case the old test (TIE alone, no STR) could not catch.

## Format / provenance / compatibility separation (items 1–3)

- Format: file magic `GT4CPT1` → `GT4CPT2` (section magics
  `GT4CKPT1`/`GT4KERN1`/`GT4BANK1`/`GT4PROV1` unchanged). Loader detects a
  `GT4CPT1` prefix and refuses with a forensic message before parsing —
  proven live against `build/ckpt-180k.bin` (pre-P00, untouched).
- Compatibility: four u32 domain versions (time, interrupt, RPC,
  translation) right after the magic; `require_compatible` refuses before
  any restore with per-domain detail. Each domain refused independently
  in unit tests; editorial-only differences (commit/binary strings) load.
- Provenance: `GT4PROV1` section (commit from a CMake-generated header,
  binary+build-type+compiler, pinned CORE hash, disc attachment note,
  time-policy string). Deterministic by construction — no timestamps, no
  host paths — pinned by the `gt4boot_autosave_identical` byte-compare
  CTest still passing.
- Kernel codec untouched (no semantic change in P00).

## Log scopes (item 4)

- `run provenance:` header on every run (commit, binary, CORE, model
  versions, time policy).
- Resume/verify lines print the checkpoint's cumulative count, its model
  versions and its writer provenance.
- `stats:` line appends `(fresh run: leg == cumulative)` or
  `(leg-relative; cumulative T since boot)`.
- `--checkpoint-at` keeps the `checkpoint saved at N services` text and
  appends `[cumulative]` or `[leg-relative; cumulative T since boot]`;
  autosave photos append `(cumulative since boot)`.
- New lines carry no guest addresses, so `--quiet` semantics hold
  (quiet CTest green).

## Evidence

- `ee_checkpoint` unit fixture: extended with provenance codec
  round-trip + 3 malformed cases, GT4CPT2 magic pin, identity round-trip,
  editorial acceptance, 4 single-domain refusals naming the domain, and
  pre-P00 forensic refusal.
- `ee_device` unit fixture: 3 new restore-bypass checks (live fires +
  clears; snapshot restore silent; armed-bits verbatim).
- CTest 50/50 (205 s), Python 73 collected / 67 run / 6 skipped, OK.
- Live probes: pre-P00 resume refused as forensic; fresh
  save → resume → 400 leg / 800 cumulative correctly reported.
- Forensic inventory (`build/ckpt-*.bin`, 20 historical files):
  byte-identical before/after the suite (only the suite's own
  `ckpt-test.bin`/`ckpt-chain-test.bin`/autosave dirs regenerated).

## Known gaps (not P00)

- Disc hash unverified at `--disc` open (GPT feedback §12): provenance
  records `attached:<name> (hash unverified at open)` honestly instead of
  claiming the pinned hash. Verification stays a pre-run step.
- Git SHA is configure-time (stale if committed without re-configure);
  documented as best-effort provenance, never a gate.
- Time-policy string is provenance, not semantics: a future quantum
  change must decide whether to bump `time_model` (P03 territory).
- No timer/DMA/JR semantic change in this slice — P01/P02 own those.
