# 0021 — The game's own block cache (the PRTS server)

Date: 2026-10-02. Status: accepted (M30 slice 42).

## Context

The sound library's first file open (through the formatter at 0x0044D740)
failed in the model with the stream's result left at zero, and the assign
chain then copied from address zero (the unaligned fault at 0x008475EB).
A write watch on the formatter's context and its descriptor, plus a trace
of every SIF RPC, showed the open's read chain — 0x004B0E28 → 0x004B1950 →
**0x00550D28** — belongs to the game's own cache client at 0x0055xxxx,
whose bind (0x00550D00) names the server **sid 0x53545250 ("PRTS")**. The
model answered only the PCDV server (sid 0x50434456, decisions 0019/0020),
so the cache's calls fell through to the generic zero reply: the client
checks the read's reply is non-zero before it builds its file object
(0x004B0E28 stores the reply at the descriptor's +0xC), the object was
never built, the header read fell back to address zero's fields and the
magic check failed.

The client's protocol, read from the traced calls and the disassembly:

- **RPC 3 — the block read**: `{LBA, byte count, flags}` (the client's
  fields +0x40/+0x44/+0x48; the path string follows at +0x54). The reply's
  first word is the handle the copy-out calls pass back.
- **RPC 4 and 7 — the copy-out**: `{handle, EE destination, byte count}`.
  The bytes land straight in the client's buffer; the reply carries no
  data the client uses.
- Other numbers (2, 5, 8, 9) are the cache's management calls and were not
  needed for the boot's reads.

## Decision

1. **The model answers the PRTS server from the same disc image as the
   PCDV reads.** `Kernel::answer_prts_read` (RPC 3) validates the request
   like the PCDV read (nonzero size, at most 0x100000 bytes, inside the
   image — a request outside stops loudly), reads the block, keeps it under
   a fresh handle and answers that handle.
2. **The cache keeps at most eight blocks.** The client copies each block
   out right after reading it (the traced reads and copies interleave), so
   a small bound covers the streaming reads without unbounded growth.
3. **The copy-out consumes the block sequentially from a per-handle
   cursor.** `Kernel::answer_prts_copy` (RPC 4 and 7) copies from the
   handle's cursor (clamped to what remains), validates the destination
   and advances the cursor past the copied bytes; an exhausted or unknown
   handle copies nothing. The request carries no offset — the client's
   font load streams one block in fixed-size chunks into alternating
   buffers (seven 0x4000-byte copy-outs of one handle), so only a
   server-side cursor serves successive chunks (M30 slice 46); serving
   chunk zero repeatedly corrupted the loaded object and faulted the
   boot's asset relocation at 15,010,045 services.
4. **The handle is a model-chosen counter.** The guest only passes the
   handle back to the copy-out calls; it never interprets its value beyond
   "non-zero means the read succeeded", so a monotonic counter is faithful
   where it matters and cannot collide with a real block's identity.
5. **The temporary instruments (a memory write watch and RPC/file traces)
   stay out of the tree.** The unit tests in `ee_kernel_test.cpp` pin the
   new behavior: the handle, the copy-out, sequential copy-outs advancing
   through a block (plus exhaustion), an unknown handle, a read outside
   the image and a machine without a disc.

## Consequences

- The boot runs past the sound phase: from the ELF entry to the step limit
  (200M steps, 3,648,011 services handled) where the old run faulted at
  83,783 services.
- With the copy-out cursor (M30 slice 46), the boot also runs past the
  font-load fault at 15,010,045 services: a 10x run reaches its step limit
  with 41,919,339 services handled (65,423,272 module calls,
  1,934,576,728 interpreted steps), stopping cleanly at 0x005552b0.
- `gt4boot_services` pins 90,000 services with the disc (about 22 s);
  without a disc image the old 3,000-service frontier stays.
- The differential passes at 100,000 services with the full state
  identical (43,082,342 interpreter instructions).
- The next wall, if any, lies beyond the step limit; advancing it is a
  later slice's call.
