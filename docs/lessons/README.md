# Lessons index — worked explanations for the GT4Recomp arcs

Twenty-two lessons cover the project's milestone arcs from setup
(M0) through the asset recon (M33), plus the checkpoint
system that underpins the long-boot work, the autosave tooling,
and the numbers-refresh method. Each lesson is a worked
EXPLANATION in a shared voice, not a summary: objective and
motivation, worked steps with real addresses and run outputs,
honest dead ends, an implementation/representation table, and
checkpoint questions. Every load-bearing statement traces to the
evidence documents each lesson cites; later reframings are noted
explicitly as pointers, never smoothed over.

## Convention (what every lesson does)

- **Objective and motivation** — the failure or wall that makes
  the arc necessary, in one or two paragraphs.
- **Worked steps** — the investigation replayed with real
  addresses, encodings, counts, and quoted run outputs.
- **Honest dead ends** — tried-and-rejected paths, kept so the
  next reader does not retry them.
- **Implementation table** — each piece mapped to its mechanism
  and its source (file paths only where the sources pin them).
- **Understanding checkpoint** — questions a reader should be
  able to answer, including at least one about a later
  reframing.

## The twenty-two lessons (curriculum order)

1. [M0–M1 scaffolding](m0-m1-scaffolding.md) — the build file,
   the identity and smoke tests, the two suites and their
   wirings, and the conventions (ignore rules, input pins,
   live-state docs) every later slice presupposes.
2. [M2–M5 foundation notes](m2-m5-foundation.md) — pinned disc
   manifest, two ELFs with identical scope, decoder plus Ghidra
   verification: the four facts everything after M6 stands on.
3. [M7–M12 tooling](m7-m12-tooling.md) — flow classification,
   function map, guest state, one-step interpreter, and
   generated suites: the reading tools before any translation.
4. [M13–M16 first execution](m13-m16-first-execution.md) — one
   real function compiled natively, then branches, calls,
   integers, COP1/MMI, startup, and unaligned/multiply: first
   contact with game code.
5. [M14 live observation](m14-live-observation.md) — the game
   itself as oracle via PCSX2/PINE: byte-identical text, the
   menu savestate anchor, and the adopt/refuse rule for live
   values.
6. [M17–M21 advanced control](m17-m21-advanced-control.md) —
   dual-identity delay slots, COP0/traps/shifts, and the
   57-function verified module: gap-driven translation, one
   rejection class at a time.
7. [M22–M24 VU0 macro](m22-m24-vu0-macro.md) — the vector
   register file, the full macro table, trapping arithmetic,
   and the four words left in the real code region.
8. [M25–M29 scale-up](m25-m29-scale-up.md) — shared-executor
   fallback, whole-text survey, indirect-flow boundaries,
   module dispatch, and the whole game as one module.
9. [Numbers refresh](numbers-refresh.md) — re-running the
   big counts against rot: identical lifted survey, evolved
   default survey, identical decode total with a corrected
   split, and the method rules the next refresh inherits.
10. [Checkpoint system](checkpoint-system.md) — snapshots at
   service boundaries (C1–C3), resume proven equal by
   differential, chained legs, and the idle budget: frontier
   work without full replays.
11. [Autosave tooling](autosave-tooling.md) — spec-first
    rotating photos with quiet mode, leg-sliced implementation,
    and the upsert defect the re-run suite caught: unattended
    bounded checkpoints.
12. [M30 BIOS services + bridge/driver](m30-bios-driver.md) —
   executing a translated module as a program, classifying its
   stops, and continuing through services or the interpreter.
13. [M30 scheduler + kernel patches](m30-scheduler-patches.md) —
    cooperative threads the game can block on, and the guest
    syscall patches its own helpers search.
14. [M30 timer/OSD](m30-timer-osd.md) — hardware as storage
    first (timer registers, OSD word, handler bookkeeping),
    thread creation end to end, and the translator bug the
    wider run caught.
15. [M30 delay library](m30-delay-library.md) — traced wait
    machinery, semaphore handle bits, and handler execution
    without nesting or mid-handler preemption: the first
    million-service run.
16. [M30 SIF/RPC](m30-sif-rpc.md) — register/DMA layer, seeded
    model IOP, the first real injected interrupt, and the idle
    VBlank heartbeat: the boot comes alive.
17. [M30 handshakes + service clock](m30-handshakes-clock.md) —
    version/negotiation answers from the game's own constants,
    the SIF register mirror, and time as a function of the
    service sequence.
18. [M30 disc/file](m30-disc-file.md) — the ISO behind file
    opens, two file layers, and the lazy validated GT4.VOL
    reader with its explicit record limit.
19. [M30 long boot](m30-long-boot.md) — raw sectors, two
    volumes, the per-handle cursor fix, and the 243M-service
    idle proven event-starved, not deadlocked.
20. [M32 march + tripwire](m32-march-tripwire.md) — honest delay
    maturation leg by leg, sterile firings, and the specified
    first originating event with its tripwire.
21. [M32 tripwire event](m32-tripwire-event.md) — one synthesized
    SIF pump packet with a four-item bar, and the load-bearing
    negative: the census unchanged.
22. [M33 recon + assets](m33-recon-assets.md) — proving absence
    (dry pipe, unbuilt job system, silent watch) alongside the
    graded asset trailheads (cipher, pages, .gpb, binder).

## Earlier notes (outside the backlog)

Three single-milestone notes predate the backlog convention and
are kept as-is: [M6](m6.md), [M7](m7.md), and [M8](m8.md). They
are not audited below and are not part of the twenty-one.

## Closure audit (slice 57, 2026-10-04)

- **Files:** all 19 lessons present, plus the 3 earlier notes
  (22 files total in `docs/lessons/`).
- **Links:** all 69 distinct relative targets cited across the
  19 lessons (`../reverse-engineering/*.md`,
  `../decisions/*.md`, `../inputs/*.json`, `../plans/*.md`)
  resolve to real files — zero broken links.
- **BUILD/VERIFY counts (spot-check):** the five lessons that
  restate counts in their headers (M14, M17–M21, M22–M24,
  M25–M29, checkpoint-system) match their named sources
  verbatim; the remaining lessons defer counts to their cited
  evidence docs without restating them, so there is nothing to
  drift. No mismatches found.
- **Rule for this index:** it summarizes; it adds no evidence.
  If a lesson changes, its one line here moves with it. The
  audit above was a read-only pass: glob the directory, extract
  every `](../…)` target from the 19 lessons, check each file
  exists, and compare each header's restated counts against the
  cited sources.
- **Addendum (slice 60, 2026-10-04, owner-designated):** index
  extended 19→20 with the numbers-refresh lesson (placed after
  scale-up, before checkpoint-system, the era it re-verifies);
  entries renumbered; all 72 distinct relative targets across
  all 21 lesson files re-checked, zero broken.
- **Addendum 2 (slice 60 review, 2026-10-04, owner):** the
  autosave lesson predates numbers-refresh but was never indexed
  — added as entry 10 (after checkpoint-system, which it
  extends), entries renumbered to 21 total; targets re-checked
  by the owner, zero broken.
- **Addendum 3 (slice 61, 2026-10-04):** M0–M1 scaffolding
  prepended as entry 1 (it precedes everything), entries
  renumbered to 22 total; link set re-checked by script
  across all lesson files (70 distinct `../` targets, the new
  lesson adding none — it cites repo paths as text), zero
  broken. Count truth: the absolute is 70 today; earlier
  69/72/67 readings were extraction-method variance, all
  agreeing on zero broken.
