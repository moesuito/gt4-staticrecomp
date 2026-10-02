# M30, thirty-sixth slice — the handlers' state matches; the failure is in the stream's processing

Date: 2026-10-02. Inputs: the pinned CORE and ISO and the live PCSX2 dump.
Follow-up to the thirty-fifth slice (the stream pool and the queued open).
**No model behavior changed**: this slice compares the handlers' internal
state and narrows the failure to the dynamic processing of the stream.

## The handlers' state at the fault

Every field compared matches the live dump exactly:

| handler | pending (+0x40) | state (+0x50) | done (+0xAC) | current (+0x60) |
| --- | --- | --- | --- | --- |
| 0x00617BB0 | 0 | 0 | 0x006B00D0 | 0x00688C40 |
| 0x00617AB0 | 0 | 0 | 0x006B00D0 | 0x00688C40 |
| 0x0084B480 | 0 | 0 | 0x006317C4 | 0x00688C40 |

The **pending lists are empty** in both — the queued open *was processed*,
not stuck — and the completed list, the state and the current object match
the console's. Together with the previous slices (the handler registry, the
prefix fields, the archive bindings, the stream pool), **the entire
handler-side state matches the console**.

## The completion path

The pop-and-complete step is **0x004AED80** (in the same library as the
formatter): it locks, checks the handler's +0x50, pops the completed list
(0x0057CB80 on the handler's +0xAC), and when that list empties, reads the
handler's current object (+0x60) and calls its vtable+0x48 method — the
completion dispatch. The handler's +0xAC currently holds 0x006B00D0 (the
"/" string) in both model and console, so the completion bookkeeping is in
the same state.

## Where the difference must be

With the handler state matching, the difference is in the **stream's
dynamic processing** — the stream is the **formatter's context** (on its
stack), and the result the formatter reads (+0x94) stays 0 in the model
while the console's open succeeds. The next slice must watch the *context*
itself: the writes to its +0x94 (the result) and +0x80 (the state) during
the open. Since the context lives on the sound thread's stack
(0x1FFxxxxx), the watch should cover that stack window and filter on the
field offsets, or key on the formatter's context construction (0x004AEFF0's
argument).

## Verification

- No model behavior changed; the temporary dump is removed and the tree is
  clean.
- CTest 34/34; Python 73 (67 run, 6 skip); the differential passes at
  3,000 services with the interpreter reference at 7,570,583 instructions
  and the full state identical.
