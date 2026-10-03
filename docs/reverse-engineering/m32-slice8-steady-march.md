# M32, eighth slice — the steady march: three legs, firing in sight

Date: 2026-10-03. Inputs: the pinned CORE and ISO; three chained 100k
legs from `ckpt-180k.bin` (all limit-hit, all saved cleanly). No probes,
no model change — the chain doing its job. Frontier: `ckpt-480k.bin`.

## The march (Confirmed)

| frontier | TIM2 COUNT | combined* | threads | COMP |
|---|---|---|---|---|
| ckpt-180k | 0xab87f540 | 0xABDFF540 ≈ 2.88e9 | all waiting | 0xfffffe40 |
| ckpt-280k | 0xbe35f0c0 | 0xBE5FF0C0 ≈ 3.19e9 | all waiting | 0xfffffe40 |
| ckpt-380k | 0xd0e411c0 | 0xD0FFE1C0 ≈ 3.50e9 | all waiting | 0xfffffe40 |
| ckpt-480k | 0xe3920d40 | 0xE3DF8D40 ≈ 3.82e9 | all waiting | 0xfffffe40 |

\* combined = `(95 << 16) | COUNT` with the overflow OR, the handler's
`current >> 8` numerator. Rate: ~313M COUNT and ~310M combined per
100k leg, metronomic across all three (deltas 0x12AE3B80, 0x12AE2100,
0x12ADFB80).

The slice-5 poke fired at combined ≈ 0xFDDF63C0 (≈ 4.26e9). Remaining:
≈ 0.44e9 ≈ **1.4 legs** — the firing should land during the fifth leg
from here (leg D5), possibly late in D4. Slice 9 runs D4+D5 watching
for slice 7's signature (a node's flags `3 → 0`, its waiter readied,
COMP reprogrammed).

## Chain fidelity notes (Confirmed)

- Module calls identical across all three legs (55,987); interpreted
  steps agree to ±80 in 3.14M (seam noise, bounded, non-compounding —
  D3 reproduced D1's exact step count).
- Every leg ends at the service limit with a Syscall boundary (never
  the idle wall — the raised budget holds) and saves cleanly.
- Superseded chain files deleted (`ckpt-c1/c2`, `ckpt-60k/120k`);
  frontier chain `180k → 280k → 380k → 480k` plus `243m` kept in
  `build/` (ignored).

## Verification

- No product-code change this slice (the binary is slice 6's); gates
  re-run on the final tree before commit per discipline (they cover the
  tree, which moved only in docs).
