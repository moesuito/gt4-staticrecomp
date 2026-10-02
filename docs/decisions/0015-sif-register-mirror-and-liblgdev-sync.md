# 0015 — The SIF register mirror and the liblgdev device sync

Status: implemented 2026-10-02 for the M30 fourteenth slice
(`docs/reverse-engineering/m30-slice14-sif-register-mirror.md`).

Context: after the service handshakes of decision 0014 the boot reached the
game's SIF command layer, which then spun forever inside its own
initialization (pc 0x00590A18) and consumed the whole step budget. Two
service behaviors were missing: the acknowledgement of the command layer's
software-register write, and the status answer of the game's disc device
library (liblgdev).

Decision:

- **The model IOP mirrors `SIF_CMD_SET_SREG` back to the EE.** The game's
  command-layer init (0x00590978) sends SET_SREG{sreg 1, value 1} and then
  spins until its own register 1 is non-zero; only an incoming SET_SREG can
  write it — the library's system handler at 0x005B0850 stores the packet's
  words into the register array at 0x008869C0. The model answers with the
  same 24-byte packet through the EE command buffer the INIT_CMD handshake
  announced, and queues the SIF0 completion. The live memory shows the real
  IOP left registers 0 and 1 both set (0x008869C0, 0x008869C4).
- **The liblgdev device sync answers the completed status.** The device
  library's request function (0x00560778) binds the server 0x046D046D, sends
  RPC 12 with 576 bytes in and 576 out, and accepts only a reply whose status
  word at +4 is 0x010B2400 (completed; 0x005608BC) or at least 0x010Bxxxx
  (the partial path; 0x005608CC) — anything else runs into the deliberate
  trap at 0x005608DC-0x005608F8. The model answers 0x010B2400 with the rest
  of the 576-byte reply zero; the real structure comes from the game's IOP
  module, which the model does not execute. The module identifies itself in
  its banner string "liblgdev version 1.11.036" (live memory 0x006C8D40).

Alternatives considered:

- **Setting the register from the model's side without an incoming command**
  (for example by writing the array directly): the EE's handler path is the
  evidence-backed mechanism, and the same path also carries the game's other
  register traffic, so the mirror is the smaller and more faithful change.
- **Answering the liblgdev sync with the partial 0x010Bxxxx code**: the
  completed code is the value the game's own check treats as success, and the
  partial path only runs a no-op before continuing; the completed answer
  keeps the game on its normal path.

Consequences and limits:

- The boot leaves the command-layer spin, binds the liblgdev server and
  passes its sync; the game then runs its device polling round (the liblgdev
  RPCs 6, 13 and 15 plus the "Pusb"/"PUPS"/"MGBP"/"PCDV" servers) and reaches
  the service limit: 1,000,000 services with 1,710,779 module calls and
  46,608,011 interpreted steps, no step-limit stop.
- Every other SET_SREG and every liblgdev RPC except the sync function still
  answers an empty result; the next slices answer the polling round's calls
  from the live oracle.
- The mirror writes a fixed 24-byte packet; a SET_SREG with a larger payload
  would be truncated (none has been observed).

Verification:

- CTest **32/32** (the mirror and the sync status in `ee_kernel`); Python 73
  (67 run, 6 skip).
- `gt4boot --compare-interpreter` at 3,000 services: interpreter reference
  at 7,573,241 instructions, full state identical.
