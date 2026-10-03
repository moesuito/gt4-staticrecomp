# M32, twenty-first slice — the announcement audit: who waits for whom

Date: 2026-10-03. Inputs: the pinned CORE (disassembly), the translated
header (caller inventory), and a full waiter census parsed from
`ckpt-1980k.bin` (temporary parser, since deleted). No probes, no runs,
no model change. Every parked thread classified with its announcer —
or marked Unknown with the lead recorded.

## Census (Confirmed — 17 threads, 45 semaphores)

| thread | wait | class | announcer |
|---|---|---|---|
| 2 | sema 11 (counting) | job hunger | job-ring producer (silent) |
| 4, 6, 8, 11, 18 | binary delay semas | delay-timeout | delay dispatcher (lottery) |
| 1, 5, 7, 9, 10, 12, 13, 14, 16, 17 | sleep (condvar) | event-wait | engine-family completions (below) |
| 3 | sleep (new site) | event-wait | Unknown (frames recorded) |

(Semaphore count fell 46 → 45: thread 3's one-shot sema was deleted in
its D9 lifecycle, as predicted.)

## Sleepers wait on engine condvars (Confirmed — code reading)

The shared spine (`0x00574e60`/`0x00574ecc` in 8 of 11 sleepers) bottoms
out in `0x005767c0`-family code: get id, enqueue self (`sp`!) on the
subsystem queue (`[queue+0x20] = 1`, `[+0x24] = -1`), check via
`0x00576640`, then `SleepThread` (`0x005adbc0`; parked pc `0x0057689c`
is just past it). WakeupThread callers (direct `jal 0x005adbd0` sites
in translated code): `0x0054e3a8`, `0x00554948`, `0x00554a24`,
`0x00576728` (the condvar's own wake-next), `0x00587b4c`,
`0x00587b68` — plus thread 2's dispatcher loop (untranslated, known
from disassembly). So sleepers wake on engine-family completions and
thread-2 jobs; every one of those sources is currently silent (no jobs
posted, no completions flowing, no new SIF traffic in any leg).

## Unknowns, with leads (honest)

- Thread 3's new sleep (`pc 0x004a1098`, frames `0x004a2400`,
  `0x00107850`/`0x00101770`): left its delay lifecycle into SDK-flavored
  code — needs its own disasm pass.
- Thread 9 (`pc 0x005b20d8`, frames `0x00553bxx`): sibling of thread
  11's `0x555xxx` family by address — Hypothesis only.
- Main thread 1 (sleep, SDK `0x0010xxxx` + `0x005767d0` frames): the
  menu-side wait itself — identifying it IS the M31 question; slice 22
  starts here.

## Verdict (high confidence)

No parked thread waits on anything the model answers but fails to
announce — the feared sync-without-async gap does not exist in this
census: delay waits announce via dispatch (proven), condvar waits via
engine completions and thread-2 jobs (all silent for lack of input,
not for lack of plumbing), sleeps via WakeupThread (same silence).
The machine is starved of *originating* work from every direction at
once (IOP bytes, pad, jobs). The cheapest originating event to model
is slice 22's call — candidates: main thread 1's wait, the `0x587b`
double-wake, or a scripted pad press if a consumer appears.

## Verification

- Census parsed from the checkpoint file (registers, semas, stacks);
  all code claims quote bytes; the one failed search pattern (flag
  writers by immediate, slice 18) stays failed.
- Temporary parser deleted; no product-code change (docs only); full
  gates run on the final tree before commit.
