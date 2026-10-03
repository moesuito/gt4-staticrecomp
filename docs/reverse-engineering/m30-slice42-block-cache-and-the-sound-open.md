# M30, forty-second slice — the block cache (PRTS) and the sound open

Date: 2026-10-02. Inputs: the pinned CORE and ISO. Follow-up to the
forty-first slice (the worker runs; the result field is the wall). **This
slice fixes the model**: it finds why the open's result stayed zero — the
game's own block cache server was not modeled — and answers it, so the boot
runs past the sound phase.

## The instrument: a write watch in the memory API

Every guest store (translated or interpreted) passes through
`GuestMemory`'s write functions, so a temporary watch there logged each new
(pc, address) pair inside a small window with the guest pc the generated
code publishes per basic block. Watching the formatter's context
(0x01FFFDB0) and its descriptor (0x01FFFE60) produced the open's exact
sequence:

```
0x4B0EC8/0x4B0ECC/0x4B0ED0/0x4B0ED4/0x4B0ED8   the request copy into the stream (+0x84..+0x97)
0x4B0F34 [descriptor+0] = 9     the block read returned zero
0x44D6C4 [descriptor+0] = 2     the header magic check failed
0x4ADDC0 [descriptor+8] = 0     the release
0x4AF7BC [stream+0x84] = 2      the descriptor's status copied into the stream
0x4AD61C [stream+0x80] = 0      the drain's pop
0x4ADC10 [stream+0x98] = 0      the gate reset
0x4AF4D8 [stream+0x80] = 3      the stream's completion
```

## The writer of the result (confirmed)

The descriptor callback **0x44D540** — invoked by the second drain through
0x4ADC40 → 0x4AF780 (descriptor, stream) — runs **0x44D6BC:
`stream+0x94 = s3`**, the file object allocated with 0x4AC430, after the
header read (0x4AFA20 → the handler's vtable+0xB8 = 0x4B11A8) and the
magic check ("INST" / 0x4E474E45). On the failure paths it stores the
error instead: 0x44D5B0 (not an open), 0x44D6C0 (the magic mismatch),
0x44D6E8 (the header read failed).

## The root cause: the game's block cache server

The open's read chain — 0x4B0E28 (the handler's vtable+0x90) → 0x4B1950
(vtable+0xD0) → **0x550D28** — belongs to the game's cache client at
0x0055xxxx, and its bind (**0x550D00**) names a server the model never
answered: **sid 0x53545250 ("PRTS")**. A temporary trace of every SIF RPC
showed the client's protocol around the failure:

```
sif rpc sid 0x53545250 n 3: req 0x0001C2C0 0x0001A830 0x00008000   the sound file's block
sif rpc sid 0x53545250 n 4: req 0x00000000 0x0096DD80 0x00000020   the header copy-out
```

The client checks the read's reply is non-zero before it builds its file
object (0x4B0E28 stores it at the descriptor's +0xC); the model's generic
answer is all zeros, so the object was never built, the header read fell
back to address zero's fields, the magic check failed and the stream's
result stayed zero — which the assign chain then copied from address 0
(the unaligned fault at 0x008475EB).

## The fix

`Kernel::answer_prts_read` (RPC 3) reads the block from the disc image
({LBA, byte count}, bounded like the PCDV read), keeps it under a fresh
handle (a small cache of at most eight blocks — the client copies each
block out right after reading it) and answers the handle.
`Kernel::answer_prts_copy` (RPC 4 and 7) copies the cached block to the
client's destination ({handle, destination, byte count}, clamped to the
block). The dispatch answers both; unit tests in `ee_kernel_test.cpp` cover
the handle, the copy-out, an unknown handle, a read outside the image and a
machine without a disc.

## Verification

- The boot now runs **past the sound phase**: from the ELF entry to the
  step limit (200M steps) with **3,648,011 services handled** — where the
  old run faulted at 83,783.
- The differential passes at **100,000 services** (past the old fault):
  43,082,342 interpreter instructions, the full state identical.
- CTest 35/35 — `gt4boot_services` now pins **90,000 services with the
  disc** (about 22 s; without a disc image the old 3,000-service frontier
  stays) — and Python 73 (67 run, 6 skip).
- The temporary instruments (the memory watch, the RPC trace) are removed;
  the tree is clean.
