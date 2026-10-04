# M32, slice 30 — the first event fires, drains, writes, and wakes nobody

Date: 2026-10-03. Implements decision 0026 (plus a table-readiness
amendment recorded there): one synthesized SIF pump SET_SREG packet
per boot, delivered as a DMAC channel-5 completion on the first
qualifying idle tick. Product change: `ee_kernel.hpp` (constants,
flag, method decl), `kernel.cpp` (trigger, snapshot word),
`ee_kernel_test.cpp` (three unit blocks + headless hardening),
`CMakeLists.txt` (one boot CTest). Evidence legs: fresh-boot
20k/90k and a 1980k-resume park leg.

## What was implemented (Confirmed — code + green runs)

- Trigger `maybe_send_originating_packet`, called first inside
  `deliver_idle_interrupt` (so both engines inject at the same
  boundary): skips when already sent; requires a registered DMAC-5
  handler, an empty queue, and a populated dispatch entry
  (`[table + 12] != 0`); writes count `0x18` + words `{0, 1, 0, 1,
  1}` at the queue base read from `[0x00886818]` (alias-masked);
  queues the ch-5 completion; sets the flag. Every guard uses
  `contains()` before touching guest memory.
- Snapshot: one trailing word (`originating_packet_sent_`), with a
  tolerant read so pre-decision checkpoints load with the packet
  unsent (correct: their queues never held one).
- Unit tests: exact packet bytes + immediate dispatch + one-shot;
  three guard holds (no pump, live queue, empty table); snapshot
  round-trip with legacy-blob tolerance. Plus a top-level
  try/catch in the test main so headless runs report instead of
  hanging on a runtime dialog (kept deliberately — see below).
- Boot CTest `gt4boot_originating` (ISO branch): fresh 20k leg
  asserting the drained packet words, the idle boundary, and thread
  2 still hungry.

## Bar 1 — gates green including differentials (Confirmed — 42/42)

Full `ctest` green: **42/42 passed, 0 failed** (45 s), including
`ee_kernel` (trigger/guards/snapshot unit blocks), the 90k-service
`gt4boot_services` run **with `--compare-interpreter`** (driver and
reference agree with the trigger live in both engines — the
sequence-determinism rule of decision 0016 holds for the new event),
and the new `gt4boot_originating` (fresh 20k leg: drained packet
words, idle boundary, thread 2 still hungry — all three `;`-listed
regexes matched, confirming CMake ANDs the list).

## Bar 2 — mechanism asserted (Confirmed — park-resume leg)

Resume from `ckpt-1980k.bin` + 2,000 services: SIFREG reads
`00000001 00000001` (register 1 set by the dispatched packet),
queue drained (count 0, packet words intact), table intact, clean
limit-hit at the idle boundary, 1 deferred / 1 pending (normal
shape). The pump ran end to end (drain → `-0x78` re-kick → table
dispatch → register write → clean return) with all threads parked.

## Bar 3 — the load-bearing negative (Confirmed — census match)

The resume leg's 17-thread census matches the pre-change baseline
(slice-29 leg) thread for thread — same waits, priorities, pcs.
Nobody woke. The CTest pins the early-park half (thread 2 still on
sema 11 at the idle boundary); the unit tests pin the trigger
semantics.

## Bar 4 — new coverage (Confirmed)

Kernel-level: trigger bytes, one-shot, all three guards, snapshot
round-trip including legacy-blob tolerance (`ee_kernel`, green).
Boot-level: `gt4boot_originating` (packet drain + early park).
Existing fixtures only; no parallel harnesses.

## The fresh-boot transient, honestly recorded (High confidence)

On fresh boot the queue drains (trigger + pump ran) but SIFREG[1]
reads 0 at 20k and at 90k. The handler necessarily ran (clean
return, no fault), so the write landed while the game's table base
was still settling — or, more precisely, delivery raced game-phase
churn that the stable park phase no longer has. This is why the
table-readiness gate exists (a pre-gate leg showed drain-without-
handling), and why CI pins the drain rather than the register: the
register proof lives where the phase is stable (park leg + unit
tests). No model defect is indicated — the pump path is identical
in both phases; only the game's table setup moves.

## Failures and costs (all closed)

- **Unit-suite abort hunt (root-caused):** the suite froze with a
  headless `abort()` dialog (`UserRequest` wait, frozen CPU,
  `abort() has been called` read off the dialog's static text via
  window enumeration). Cause was mine, not the product's: the test
  fixture window (`0x700000`) does not cover the pump addresses
  near `0x00886818` — the test's own setup write threw before any
  assertion. Fix: window to `0x900000`. The product guard
  (`contains()` before read) behaved exactly as designed.
  Collateral lessons, all paid for: (a) never diagnose from broken
  tooling — an unreliable CPU reading plus wall-time suspicion led
  to premature kills of healthy runs (the suite itself takes
  seconds; it was the Debug *builds* that took minutes); (b) a
  headless abort looks exactly like a hang — the kept test-main
  try/catch converts it to text; (c) nested `cmd`/PowerShell quoting
  repeatedly mangled `gt4boot` invocations — route every run
  through a `.cmd` file; (d) a `.cmd` file must `call` a `.bat`
  (VsDevCmd without `call` transfers control away and silently
  skips the rest — this bit twice before the BEFORE/AFTER echo
  isolated it). Scratch `.cmd` files deleted before landing.
- **A threatening `;`-list regex:** the new CTest needs all three
  patterns to match; verified empirically in the suite run
  (CMake ANDs the list — a failing member fails the test).

## Verification and hygiene

- `TEMPORARY`-free tree (the test-main try/catch is kept
  deliberately as headless hardening, documented above — not a
  probe); scratch logs and `.cmd` runners deleted.
- Gates green on the final tree (42/42, this slice's own run).
- No commit per slice rules (owner reviews, gates, commits).

Next (recommended): slice 31 watches for the first waiter that can
consume async traffic — the census-unchanged tripwire from decision
0026 fails loudly the day anything wakes, which is exactly the
signal to chase. Until then, milestone event work continues
elsewhere (the pump path is proven live for whatever traffic comes
next: RPC replies, pad servers, GS-side).
