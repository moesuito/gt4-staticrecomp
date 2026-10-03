# M32, thirteenth slice — the walk designates thread 6's node next

Date: 2026-10-03. Inputs: the pinned CORE and ISO; the post-fire
checkpoint read directly (temporary parser, since deleted), three
disassembly reads, and leg D10 (`ckpt-1280k.bin`). No probes that touch
execution, no model change.

## The walk tests one node: now 0x00889f80 (Confirmed)

Post-fire `[0x6592F0+0x18]` reads `0x00889f80` — the new tail after
`0x0088a000`'s consumption — not null and not the sorted head. So the
slot names the walk's designated node (which the unlink advances), and
a first-not-due exits the frame: exactly one node is tested per timer
run. Correction to slice 11's mental model (sorted-list walk): the
order barely matters; the designation does. Next firing, whenever
current matures, is node `0x00889f80` → thread 6 (sema 4245855) — a
standing prediction.

## Closed corners (Confirmed — static)

- Thread 2's else-branch (`0x005b07d0`) is an assert/log path (formats
  through `0x005af850`, faults otherwise): the job protocol is exactly
  three ops (wake / rotate / suspend), no hidden fourth.
- The SIF pump's post-dispatch is a table-driven indirect call to
  per-cid SIFCMD handlers (`jalr` with switched gp). Correction to
  slice 50's label: `0x005ae090` is the `SifSetDChain` stub the pump
  funnels through, not a dispatcher in itself.
- The producer hunt ends structurally for now: the pump→handler→job
  chain is fully mapped and fully dormant — it needs inbound IOP bytes
  in `[0x886818]` that a synchronous model never receives. No static
  trail remains (no immediates, one runtime-distributed handle).

## Leg D10 (Confirmed)

COUNT `0x382c9540 → 0x74d3ed40` (+~1.02e9, steady), designation stable
(`0x00889f80`), nodes/COMP/threads unchanged, lockstep holds (100,000
calls). Overflow 99 → 99 this leg (the earlier 95 → 99 climb is
sporadic, source Unknown — one line, no chase). Combined ≈ 1.97e9 vs.
≈ 4.26e9 needed: ~2.3 legs to thread 6's firing.

Next: slice 14 keeps marching (D11+) toward thread 6's firing, while
the originating-bytes question (the true M32 work: what the model IOP
should one day send unasked) waits behind the retiring timeouts.

## Verification

- Temporary parser deleted; product code untouched (docs only).
- Saves need clean Syscall stops and got one; deterministic prefixes
  reproduce. Full gates run on the final tree before commit.
