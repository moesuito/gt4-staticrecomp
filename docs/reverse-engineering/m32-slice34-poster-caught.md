# M32, thirty-fourth slice — the poster that never posted: a load-bearing surprise

Date: 2026-10-03. Inputs: the pinned CORE and ISO; two fresh-boot
legs with a temporary ring-slot write watch (0→60k and 0→200k
services, since reverted); twelve checkpoint files read directly;
whole-text scans; disassembly. The watch was purely observational
(slice-27 pattern: five RAM store paths, physical-overlap test, pc
attribution via driver globals, guarded reads, cap + dropped
counter); product code untouched.

Charter: replay the posting era with the watch live and let the
poster name itself — or, if silent against the era bound, report
the falsification without explaining it away.

## Verdict: silence falsifies the era reading (Confirmed surprise)

- Fresh 0→60k services: **131 watch hits, all accounted for** —
  130× 8-wide zero stores sweeping `[0x00885EE8, 0x008862F8)` at
  pc `0x00100008` (ELF-entry startup clearing `.bss`, module-entry
  attribution as documented) plus 1× word store of 0 to the
  producer word at exact pc `0x005AEB30` (the creator's delay-slot
  init, bridge-exact). Dropped 0. Clean limit-hit at idle.
- Fresh 0→200k services: **the same 131 hits, zero more.**
- Yet every checkpoint from `ckpt-180k` through `ckpt-243m` reads
  consumer = producer = `0xB5` with uniform `{0,3}` slots, while
  `ckpt-test`/`ckpt-chain-test` (400/800 services) read `0/0`.
- The era bound of slice 33 — posts within (800, 60000] — is
  therefore **falsified**: a verified-live watch observed zero
  posts across the entire alleged era, extended 3× past it.

## Why the silence is trustworthy (Confirmed — non-blindness audit)

- All five RAM store paths hooked (byte/half/word/doubleword/
  bulk); the translator emits only these calls (verified in
  `gt4translate/main.cpp` — no raw-pointer stores, including for
  `sq`/unaligned forms); kernel/device/service code funnels
  through them (slice-30 grep).
- Liveness proven in-leg: the 130 zeroing hits plus the
  bridge-exact creator hit (pc `0x005AEB30`, a delay slot no
  module entry could attribute) show exact-pc capture works.
- A poster writing slots/producer by any width, any alias, lands
  in the watched span `[0x00885EEC, +0x408)` — consumer word
  deliberately excluded (thread 2's own write).

## Ranked resolutions (no explaining away)

1. **Late era, past TRUE-200k (live hypothesis).** The checkpoint
   filenames' units were never pinned (leg-relative service counts
   vs cumulative calls/steps both fit the record); if the first
   181-bearing checkpoint sits past 200k true services, there is
   no contradiction — only a longer era. Deciding it needs a
   heroic leg (rejected per rules) or a true-total anchor the
   files do not carry.
2. **Fossil from past conditions (Hypothesis).** 181 identical
   `{0,3}` wakes, fully consumed, frozen ever since, is exactly
   what a dead early-boot waker leaves behind; model timer-rate
   changes mid-march supply a behavior-shift mechanism. Requires
   a formation bound it lacks.
3. **Watch blindness (rebutted above).** Kept on the table with
   near-zero weight: every bypass examined is closed.
4. **Fresh-vs-chain non-determinism (rebutted, catastrophic
   branch).** `verify-resume` bit-proofs (800, 22000) and the
   243.7M reproduced ending forbid it; its exact falsifier would
   be a resume-from-800 long leg diverging from direct — not run.

## Grades

- Confirmed: the two silent legs (verified-live watch, clean
  stops); the checkpoint series (12 files, parsed directly);
  non-blindness (emitter grep + in-leg liveness).
- High confidence: slice 33's era bound is wrong (falsified by
  the 60k leg alone; the 200k leg triples it).
- Hypothesis: late era vs fossil (undecided; see recommended
  experiment).
- Unknown: the poster's identity (no closer than before — further,
  since the era moved).

## Verification and hygiene

- All 108 temporary insertions reverted via checkout (4 files);
  source grep-clean (0 `TEMPORARY`); `build/gt4boot.exe` relinked
  and binary-clean; all scratch (2 logs, 2 `.cmd` files) deleted.
- Gates unaffected (nothing to gate); review + commit per rules.

Next (recommended, only if the owner wants it): ONE decisive leg
— resume-from-800 (guaranteed pre-era only under the small-era
reading) or fresh, to ~2M services with the watch — settles late
era vs fossil vs nondeterminism in a single run. It is frankly
heroic (~10× the longest leg this slice); the cheaper standing
position is the tripwire: any future post trips nothing today,
and the day the census moves, the parked waiters' announcers
resolve one by one. Do not manufacture wakes in the meantime
(slice 3 proved them sterile, slice 32 proved fabrication
out of scope).
