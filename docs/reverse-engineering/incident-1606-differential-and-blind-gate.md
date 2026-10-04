# Incident: differential red at service 1606 behind a blind gate (2026-10-04)

Status: open. Owner-verified end to end during the slice-69 review.
Next: slice 70 hunts the first divergence; closing it removes the
WILL_FAIL markers in CMakeLists.txt (a fixed differential XPASSes red
until they are gone).

## What the worker reported

While verifying P06, the slice-69 worker found (a) the `gt4boot_services`
CTest passes while `gt4boot` exits 1, and (b) a driver-vs-interpreter
divergence in the RAM window first appearing between services 1600 and
1700, claimed pre-existing (stash + rebuild on the intact main) and
therefore not a P06 regression.

## What the owner verified independently

- `tools/gt4boot/main.cpp:1357-1360`: on a differential mismatch the
  process prints "the driver and the interpreter states differ" to
  stderr and returns 1 — but only AFTER the "services handled 90000"
  line reached stdout. CTest with only PASS_REGULAR_EXPRESSION reports
  Passed on that output despite exit 1. Mechanism Confirmed.
- Direct runs (P06 tree, with disc): 90,000 exits 1 ("state differs in
  the guest memory window"); 2,000 exits 1; bisection 1500 = 0,
  1600 = 0, 1604 = 0, 1605 = 0, 1606 = 1, 1609/1618/1637/1675/1750 = 1.
  First divergence at service 1606. Confirmed.
- Attribution (`git stash`, rebuild, same 1606 leg on the clean
  post-P05 tree): still exits 1. The first divergence pre-exists P06.
  Confirmed. (Signature note: clean tree reports "register 29" first,
  P06 tree reports the memory window first; the P06 rebind fix is the
  suspected cause of the signature change — Hypothesis, for the hunt.)
- No-disc leg (`--services 3000 --compare-interpreter`): exits 1 with
  the same memory-window message. Both `gt4boot_services` variants
  were blind and red. Confirmed.
- `gt4run --compare-interpreter` (startup leg): exits 0, genuinely
  green. Confirmed — the blindness is specific to legs whose pinned
  line prints before the compare.

## Response (in the slice-69 commit)

- `FAIL_REGULAR_EXPRESSION "states differ;but the interpreter stopped
  at"` on all three compare-mode tests (`gt4run_startup`, both
  `gt4boot_services`). Neither phrase appears in any passing log.
- `WILL_FAIL TRUE` on both `gt4boot_services` variants, with the
  removal condition documented inline: the suite stays green while
  honestly flagging the open incident.
- Reviewer-added bound: the P06 payload tap retains at most 1 MiB per
  channel (counting and hashing still stream every byte), so long
  marches cannot grow host memory without bound.

## Consequences

- Every translator-vs-interpreter claim after service ~1606 is
  suspect until the hunt closes this: RPC telemetry (P07), fresh
  prefix comparisons (P09), and the causal hunt (P10) all stand
  downstream of a green differential.
- Out of scope for the hunt: the 1606 divergence is a fact about the
  two engines disagreeing, not yet attributed to module, bridge,
  device, or kernel.
