# M32, eighteenth slice — what each worker asked for (and how long)

Date: 2026-10-03. Inputs: the pinned CORE (disassembly) plus the
translated header (one stub identity). No probes, no runs, no model
change. This slice reads the shared worker dispatch chain to name the
request behind each wait — and corrects the delay scale from
"40-minute" to microscopic.

## Workers are one-shot job processors (Confirmed)

Entry `0x005786f0` takes a work item in `a0`, dispatches through the
item's table (`[item+0x38]` indirect call), sets priority
(`0x005adb30` = service `0x29`, ChangeThreadPriority), then
`ExitDeleteThread` (`0x005adae0` = `0x24`). The missing thread 15 and
the uniform entries all fit: one item, one run, self-deletion. The six
waiters sit inside their item handlers (the family-distinct return
addresses), all funneled into the same one-shot wait wrapper.

## Worked example: thread 4's engine (Confirmed shape, open setter)

`0x00551580` (T4's family) checks `[0x65c714]` — zero at every stop,
so the real work is skipped (the effect-free path slice 4 saw) — else
calls into the `0x58xxxx` engine (`0x0058f0b0`/`0x0058efe0`/`0x0058f5b8`)
after `GetThreadId`. The flag's writer is not statically reachable (the
address is runtime-computed; no immediates anywhere) — it joins the
slice-19 watchlist (dump `[0x65c710]` per leg and see if live delivery
ever sets it).

## Delay-scale correction (Confirmed arithmetic)

`sched` values (`0x48000`/`0x24000` current-units) are microscopic —
~0.1 services at live rates, not 40 minutes. The epoch scale lives
entirely in `base` (stamped near a COUNT ceiling via `0x005b8400`);
`target = sched + base - acc` inherits it. So these were ~instant
"yield and re-check" waits whose fuse burned across a wrap during the
delivery-stall era — dissolving slice 9's paradox (no absurd boot
waits on any hardware; just missed micro-fuses). Targets cluster near
one ceiling (`≈ 0x100000xxxxx`), so all six stranded the same way in
the same era.

## Verdict (high confidence)

Each worker = one subsystem job ending in a micro-delay wait (same
wrapper, same stall). Completions now come only via natural maturation
(lottery per designated node) or via the subsystems' real events —
with VBlank handlers live, the gated engine paths (`[0x65c714]` and
kin) may open on their own, which is now observable per leg. Next
(slice 19): march D14+ toward `0x00889f80`'s firing while watching the
gate words for opening.

## Verification

- Every claim quotes disassembled bytes, service-table rows, or dumps;
  no run was needed. The one static search that failed (flag writers
  by immediate) is recorded as failed, not fudged.
- No product-code change (docs only); full gates run on the final tree
  before commit.
