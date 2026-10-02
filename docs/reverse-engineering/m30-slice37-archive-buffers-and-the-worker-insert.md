# M30, thirty-seventh slice — the archive buffers are identical; the worker's insert

Date: 2026-10-02. Inputs: the pinned CORE and ISO and the live PCSX2 dump.
Follow-up to the thirty-sixth slice (the handlers' state matches).
**No model behavior changed**: this slice rules out the archive content and
maps the worker's step the open uses.

## The archive buffers match the console byte for byte

The live dump's handler buffers hold the same archives the model serves:

| handler buffer | compared with | result |
| --- | --- | --- |
| 0x0090EA80 (0x59440 bytes, layer-0) | the image's logical block 0x1BEF0 | **identical** |
| 0x00905B00 (0xF00 bytes, layer-1) | the image's logical block 0x143B24 (file 0x143B14, the -16 layer shift) | **identical** |

Both start with the archive header (`ad90b9ac 01000300` — magic 0xACB990AD,
version 3.1). So the data the handlers look up in is **byte-identical
between the model and the console** — the difference is not the archive
content.

The sound names (`gt4sys`, `roadnoiz`, `gt4se`) appear plainly XOR-0xFF in
the **outer** archives' name tables (GT4.VOL and GT4L1.VOL at the same
offsets 0xC01B/0xC025/0xC059). The inner archives' names are not
raw-searchable (their pages are compressed), so that search is inconclusive
for them.

## The worker's step

The worker step **0x004AD4A0** walks a sorted list at the handler's +0x58
comparing the stream's key pair (+0xA0/+0xA4). With an **empty list** — the
state in both the model and the console (slice 36) — it takes the insert
path **0x004AD53C**: it locks, sets the **stream's state (+0x80) to 2** and
inserts the stream into the handler's list (0x0057CB28 with the stream's
condition node at +0x3C). So the open's queued stream becomes a node of the
handler's list, and the state advances to 2.

## What the next slice must find

The formatter reads the **stream's +0x94** (the result) and it stays 0 in
the model. With the content, the registry and the handler state all
matching, the difference is in the stream's dynamic fields. The next
instrument should watch the **formatter's context** directly: the driver
already can expose the guest **sp**, so the write watch can follow a narrow
stack window around the current sp and report the context's field writes
(+0x94, +0x80, +0x3C) during the open.

## Verification

- No model behavior changed; the tree is clean.
- CTest 34/34; Python 73 (67 run, 6 skip); the differential passes at
  3,000 services with the interpreter reference at 7,570,583 instructions
  and the full state identical.
