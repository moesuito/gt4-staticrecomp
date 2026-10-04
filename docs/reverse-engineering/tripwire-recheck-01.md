# Tripwire re-check 01 — frontier census live (SAME)

Date: 2026-10-04. Leg: `gt4boot private/fingerprint-check/CORE.GT4
--disc "Gran Turismo 4 (USA) (v2.00).iso" --resume
build/ckpt-1980k.bin --services 2000 --threads --dump 0x00886740 32
--dump 0x008869C0 8`. Read-only: no instruments, no model change.
Baseline: the slice-29 leg census (17 threads, thread 2 on sema
11, delay semas, 11 sleepers) as reframed by the post-0026 legs
(same census + drained queue + SIFREG[1]=1), pinned in the
`gt4boot_originating` CTest (`thread 2: status 0x4, wait 2/11`;
`boundary: syscall 0x00001604 service 0x100`).

## Verdict: SAME — nothing woke

Thread-for-thread against the baseline: 17 threads, all status
0x4; thread 2 `wait 2/11` at entry 0x005ae9a0; threads 4, 6, 8,
11, 18 in `WaitSema` on the delay semaphores (10388483,
4245855, 11235775, 6407847, 11235727); the other eleven asleep
(`wait 1/0`: main, eight engine workers, threads 3, 5, 7, 9,
10, 12, 13, 14, 16, 17 — eleven sleepers by count: 1, 3, 5, 7,
9, 10, 12, 13, 14, 16, 17). Stop:
`boundary: syscall 0x00001604 service 0x100` (the pinned
post-0026 shape, not the pre-0026 NoRunnableThread kind at the
same pc); 1,999 module calls, 183,875 interpreted steps
(off-by-one/one-thousandth vs the pre-0026 leg's 2,000/184,001
— the injected packet's own service and drain, expected);
1 deferred call, 1 pending interrupt, same as baseline.

## The packet fired exactly once, to no effect

- Pump queue `[0x00886740]`: first byte 0 — drained; packet
  residue plus the stale template behind it, the specified
  shape.
- Software registers `[0x008869C0]`: `00000001 00000001` —
  regs 0 and 1 both set, the live-console state, not an
  invention.
- Census unchanged thread-for-thread (above): the
  load-bearing negative holds — mechanism, not unblock.
- Timer 2 healthy (count 0x1e9dbbc0, mode 0x782, comp 0x240);
  SIF0 CHCR shows the re-kick.

## Gates (same session)

- Build: `ninja: no work to do` (nothing to rebuild).
- CTest: 50/50 green (all eight autosave tests incl.).
- Python: 73 tests, OK, 6 skipped (two pre-existing
  ResourceWarnings from the fake-PINE-server socket test —
  noise, not failure).

## Next

No tripwire tripped. The watch continues: same leg shape on
the next re-check, compared against this record. A census
that ever shows a non-waiting thread (or a missing waiter)
is the trip event — stop there with dumps, do not continue
past it.
