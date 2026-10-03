# M30, forty-sixth slice — the font file's load path: the copy-out cursor

Date: 2026-10-03. Inputs: the pinned CORE and ISO. Follow-up to the
forty-fifth slice (the fatal query asked for `/fonts/system.fnt`; the
loaded object's offset tables lead the relocate into a data buffer).
This slice finds the load path, pins the divergence in the model's
copy-out, fixes it, and verifies the boot past the 15M-service fault.

## The load path (temporary RPC/file traces, since removed)

A full run with every RPC and every file open traced shows the whole
boot's disc traffic is **12 block reads**:

- 8 PCDV reads in the first 4,320 services (descriptors, both volumes,
  the archive block `0x1BEF0`).
- 1 early PRTS read (`0x1BFC6`, sound era, 14,146 services).
- 3 late PRTS reads right before the fault: `{0x1BFBF, 0x323C}` at
  14,961,958 services, `{0x1BFA3, 0x672}` at 14,975,067, and
  `{0x1C878, 0x2D66E}` (186 KB) at 14,991,629 — 18k services before the
  fault at 15,010,045.

Only `cdrom0:\IRX\*.IRX` opens ever occur (22, all IOP modules): the
font travels purely through PRTS, never fileio or PCDV. The disc bytes
are packed (no `FT01` magic on disc), so a transform stands between the
sectors and the live object.

## The divergence: the copy-out had no cursor

The tail also captured every PRTS copy-out. Handles increment per read
(1–4, matching the model's counter), and the font block is consumed as
**seven 0x4000-byte copy-outs alternating two buffers**
(`0x88FFC0`/`0x893FC0`) — sequential chunk streaming. But the op-4/7
request `{handle, destination, size}` carries **no offset**, and the
model always copied from the block start: chunks 2–7 re-served chunk
zero. The loaded object was built from one chunk repeated seven times —
hence the patterned data buffer and the odd interior "pointer" that
faulted the relocate. The sound phase survived only because its single
small copy-out starts at zero anyway.

A server without per-handle position cannot serve this protocol, so the
cursor is required, not guessed: the client never rewinds in the traced
runs (each block is read once and streamed straight through).

## The fix (extends decision 0021)

`PrtsBlock` gains a `cursor`; `answer_prts_copy` serves from
`data + cursor`, clamps to what remains, validates the destination and
advances past the copied bytes (an exhausted or unknown handle copies
nothing). Unit tests cover sequential chunks, exhaustion, an unknown
handle, an outside-image read and a disc-less machine.

## Verification

- A 10x verification run with the fix sails past the old fault to its
  step limit: **41,919,339 services handled** (65,423,272 module calls,
  1,934,576,728 interpreted steps), stopping cleanly
  (`step-limit 0x005552b0`, exit 0, healthy thread table) — 2.8x past
  the 15,010,045-service wall.
- CTest 36/36 and Python 73 (67 run, 6 skip), green on the final tree.
- All temporary instruments removed; the tree holds the fix, its tests
  and docs.
