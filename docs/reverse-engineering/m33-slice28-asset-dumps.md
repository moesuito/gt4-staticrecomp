# M33, slice 28 — asset-dump legs: the pipeline never runs, hooks verified quiet

Date: 2026-10-03. Inputs: the pinned CORE and ISO; one validation
resume leg (2,000 services, all hooks zero, stats identical to
uninstrumented legs) and one fresh-boot `--disc` leg (95,000
services, full boot-to-park); whole-text byte scans; `gt4disasm`
reads. Temporary hooks in `ee_driver.hpp`/`driver.cpp`/`gt4boot`
(all reverted). No model change.

Charter: Hook A (xor55 buffers at `0x4B36E0`), Hook B (128 B context
items at `0x004995EC`, completed-item substitution for the spec's
`0x004995D4`), Hook C (80 B upload packets at `0x004A4DD0`, last
store), Hook D (`[obj+0x1C]` writer) if time permits.

## Verdict: zero executions on every hook (Confirmed, probe audited)

- Fresh leg: `hookA entries 0 exits 0`, `hookB entries 0 items 0`,
  `hookC entries 0 packets 0`, all dropped counters 0. Limit-hit at
  the usual idle signature (`0x00001604` / `0x100`); service mix
  shows a real boot (1,232 non-idle: GetThreadId 1012, CreateSema 33,
  SetSyscall/SIF/OSD… then 93,768 idle returns) — the leg genuinely
  traversed boot-to-park, past the scout's ~83k archive-read window.
- Resume validation leg: all zeros with run shape bit-identical to
  uninstrumented legs (2000/184001/2000) — zero perturbation.
- The zeros are observed absences, not blind hooks:
  - xor55's sole caller (`0x004B3B2C`, inside untranslated page
    fetch `0x004B39B0` — no translated function, no entry) can only
    run via the interpreter bridge, where the exact-pc hook fires;
    the xori body exists solely in `function_004B36E0`, so no
    translated path can inline past the hook. All three forced
    entries verified present in the entry table.
  - Hook B/C pcs are mid-function non-delay-slot steps reached only
    through forced entries; entries never arrived either.
- Therefore: no v3.1 page fetch, no context item, no upload packet is
  built during fresh boot-to-park. The ~83k archive reads go through
  raw sector/block paths (PCDV/PRTS), never the page-fetch reader.
  The whole asset pipeline (page fetch → materials → GIF packets) is
  downstream of the park — consistent with the 41M/243M long-run
  record (slices 46/47), in which texture code likewise never runs.

## Per-hook results

- Hook A (Confirmed absent): no xor55 call in 95k fresh services, so
  which buffers get xor55 stays Unknown from live behavior (as the
  static doc already records). Hook design stands ready for a leg
  that reaches the loading/movie phase.
- Hook B (Confirmed unreachable): `0x00499508` never entered, fresh
  or parked — bind unreachable from any boot the model produces.
- Hook C (Confirmed unreachable): `0x004A4CF0` never entered —
  same verdict. (Packet layout confirmed statically en passant:
  tag + 4 A+D pairs at `[t3, t3+0x50)` = 80 B; the `jr ra` at
  `0x004A4DD4` also serves the t2==0 skip path, which is why the
  hook sits on the final store `0x004A4DD0` instead.)
- Hook D (deferred to slice 29, conditional): needs the material
  object, which only Hook B could have supplied (a0 at `0x00499508`
  / `0x00105F3C` — the latter confirmed as a direct `jal 0x00499508`
  caller by byte scan, matching the trailhead). No object was ever
  observed, so D has no operand this slice. It additionally requires
  a boot that reaches the material path — a milestone-level
  precondition, not a longer leg (41M/243M runs already prove the
  parked boot never gets there).

## Hook design (reverted; reuse notes)

- Force-interpret at three entries (`0x004B36E0`, `0x00499508`,
  `0x004A4CF0`): same differential-verified semantics, bridge
  granularity, so exact-pc hooks see live regs. This forces
  *interpretation of executing code*, never execution itself.
- Hook A exit needs a pending slot (entry stashes a0/a1/pre-bytes;
  the leaf clobbers a0 via `sltu`); safe because the leaf contains
  no calls. Hook B logs at item-completion `0x004995EC`
  (`[s0-0x80, s0)`) rather than the spec's tag-store pc, whose
  trailing stores are still pending there. All reads `contains()`-
  guarded with empty-vector partial markers; all vectors capped
  (16/8/8) with dropped counters.
- No raw payload bytes enter this doc or the repo: nothing was ever
  captured (all vectors stayed empty).

## Grades and limits

- Confirmed: all zero-counts above, with the audited non-blindness
  argument; fresh-leg boot genuineness (service mix); validation-leg
  non-perturbation; caller/entry inventories (whole-text byte scans).
- High confidence: the asset pipeline is downstream of the park; the
  viewer track cannot harvest live packets until milestone event work
  moves the boot (decision 0023).
- Unknown: xor55's buffers; `[obj+0x1C]`'s writer; the break-trap
  codes (unchanged).

## Verification and hygiene

- Temporary code (header struct + decls, driver force/step/results,
  gt4boot report) fully reverted via checkout; `TEMPORARY`
  grep-clean in source (0 hits); `build/gt4boot.exe` relinked and
  `TEMPORARY hook`-clean; working tree holds only this doc plus the
  journal appendix. Two scratch logs deleted.
- Gates left for review (no commit per slice rules).

Next (recommended): slice 29 executes Hook D **only** against a boot
that reaches the material path — none exists yet, so park D behind
the milestone precondition and spend the slice instead on the one
remaining cheap stimulus from slice 22's table, if any is still
untried; otherwise close the asset-dump line as answered-negative
and return fully to decision 0023 event work.
