# M32, seventh slice — who fired: node 0x0088a000 woke thread 3

Date: 2026-10-03. Inputs: the pinned CORE and ISO; the checkpoint read
directly plus one poked leg (temporary `--poke-count`, since removed).
Attribution complete: the full node → descriptor → worker chain, the
one-shot consumption, and the base time source. One honest
non-reconciliation is recorded with all numbers.

## The chain, all three links observed (Confirmed)

Descriptor words are `{next, id, callback, sema}` — callback
`0x005AEF58` (the `iSignalSema` endpoint) for all six, and the sema
words name the workers:

| node | descriptor | sema | worker |
|---|---|---|---|
| 0x00889f40 | 0x0088bf40 | 0x61c6a7 (6407847) | thread 11 |
| 0x00889f80 | 0x0088bf50 | 0x40c95f (4245855) | thread 6 |
| 0x00889fc0 | 0x0088bf60 | 0xab718f (11235727) | thread 18 |
| 0x0088a000 | 0x0088bf70 | 0xaf3543 (11482435) | thread 3 |
| 0x0088a040 | 0x0088bf80 | 0xab71bf (11235775) | thread 8 |
| 0x0088a080 | 0x0088bf90 | 0x9e8403 (10388483) | thread 4 |

The poked leg's diff on node `0x0088a000` (thread 3's): `prev`
`0x00889f80 → 0x0088a0c0`, idbits `0x8f → 0`, flags `3 → 0` — consumed
one-shot and unlinked (`0x00889f80` is the new tail), while its
descriptor's link word cleared. Together with slice 5's log (exactly
one `iSignalSema(11482435)`, thread 3 readied, COMP reprogrammed to
`0x240`): node → dispatcher → sema → worker, all three links seen.
This also corrects slice 5's guess: the reschedule path did not run;
the one-shot path did.

## The base time source (Confirmed)

`0x005B8728` stamps `base` from `0x005B8400`, which computes exactly the
handler's formula — `(overflow << 16) | COUNT`, shifted by `(MODE&3)*4`
(the shift resolves to 8 for the live mode `0x782`, both sides). Base
and current share units by construction; there is no 256x scale break.

## Leading hypothesis: scheduled pre-wrap, fuse missed (Hypothesis)

Bases sit at combined ≈ 4.29e9 — just below the 32-bit ceiling — while
the run's counter never passed ~2.9e9: the delays were stamped with a
~65k-tick fuse just before a COUNT wrap, the fuse burned through the
wrap (combined collapsed), and they now wait nearly a full epoch. The
fuse is ~7 frames wide; missing it needs the handler silent across
those frames, which the backlog-stall era supplies. If true, the chain
matures them without any model change — poking only time-traveled past
the wait.

## Honest non-reconciliation (Unknown)

The two slice-5 pokes were `+0x73800000` (COUNT `0xFD1E63C0`, nothing)
and `+0x73E70000` (COUNT `0xFDCF63C0`, fired) — only 6.7M ticks apart,
far less than a leg's own advance — so the sharp difference must come
from early-leg COUNT dynamics (game rewrites are observed: poked legs
end with game-written COUNT/COMP), not from the static gap. My earlier
gap arithmetic used two mis-added sums and is withdrawn; the numbers
above are the checked ones. The decisive follow-up, if it matters: dump
COUNT a few hundred services into a poked leg and watch the first
handler runs. It may not matter — the chain fires the nodes anyway.

Next: slice 8 keeps chaining from `ckpt-180k.bin` (COUNT
`0xab87f540`) toward the firing neighborhood, now able to verify the
predicted signature (a node's flags `3 → 0`, its waiter readied,
COMP reprogrammed) instead of watching blindly.

## Verification

- Node/descriptor/worker/register quadruples agree 6/6; the consumption
  diff is word-exact against the checkpoint parse.
- Poke instrument removed (grep-clean); checkpoint file untouched.
- Full gates run on the final tree before commit.
