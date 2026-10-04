# M32, thirty-third slice — hunting the ring producer: proven unknown

Date: 2026-10-03. Inputs: the pinned CORE (whole-text byte scans,
disassembly), twelve checkpoint files read directly (no runs
needed), one resumed `--dump` leg. Read-only throughout: no product
change, no instruments. Charter: name thread 2's producer within a
two-caller-level stop rule, or report a justified negative.

## Verdict: producer unidentified (proven-unknown, sweep recorded)

## What thread 2's loop does (Confirmed — disassembly of 0x005ae9a0)

Entry saves s1 = arg block; each iteration: `WaitSema([0x00885EE0])`
(service `0x44`), counter `[s1] = ([s1] & 0x1FF) + 1`, then two byte
reads at `(s1+8) + counter*2` and `(s1+9) + counter*2`, dispatched
as: 0 → `WakeupThread` (`0x005adbd0`, target id from the tables),
1 → `0x005adb50` (rotate), 2 → `0x005adc10`, else `0x005aea68`.
Live s1 (`0x00885EE8`) makes the layout: `[0]` consumer,
`[+4]` producer, 512 `{op,arg}` byte-pairs from `+8` — every slot
reads `{0, 3}` (bytes `00 03` repeated; the `0x03000300` dump words
are little-endian pairs, not u16 768s).

## The sweep (all Confirmed absences)

- **Writer immediates**: zero references to ring/control words
  (`0x885EE0`-family) anywhere in 5.3M text bytes — bases are
  runtime (stack-adjacent layout), so no static writer exists to
  find. (Method trap avoided: an early grep for `0x869C0`-style
  substrings returned only false positives — `0x2869C0`-type data
  addresses and one code address.)
- **Sema-11 signalers**: the only `0x88`-based `0x5EE0` access in
  the image is the creator's own store; the `0x65`-based hits are a
  different (engine) semaphore home.
- **Caller following, two levels**: creator `0x005aea78` ← sole
  `jal` at `0x005B7590` (init chain: creator, `0x005B7688`,
  `0x005B9928`, jump away — boot-time only, zero runtime posts);
  op handlers resolve to SDK wrappers and the loop body
  (consumption side). Level 2 would climb the init chain away
  from any runtime poster: stopped per rule.
- **Delay dispatcher** (`0x005b8ed8`, 52 words read): descriptor
  `jalr` + free-list at `[0x89c340]` + direct `iSignalSema` —
  no ring touch, disproving the most plausible mechanical
  producer.
- **Op mix**: all 512 slots op 0; ops 1/2 (rotate/suspend) were
  never posted by anyone, ever.

## Era bound from checkpoints read directly (Confirmed)

Twelve checkpoint files parsed (`GT4CPT1` framing → context section
→ region containing `0x00885EE8`): at 400/800 services the ring is
`0/0` (nothing posted — thread 2 may not exist yet); at 60,000
services and at every checkpoint through 243.7M it reads
consumer = producer = `0xB5` (181) with uniform `{0,3}` slots.
The posting era is exactly bounded: **(800, 60000] services** —
SIF-handshake/RPC/thread-creation/disc-mount window — then
permanent silence. The creator itself posts nothing (tail
disassembled: zeroes the indices, calls the delay helper,
`GetThreadId`, `ChangeThreadPriority`, returns).

## Reading of the evidence (High confidence)

Every job ever posted wakes thread 3, 181 times (or 693 with one
undetectable wrap — indistinguishable since posted values equal
any fill), then nothing for ~243M services. That is a
single-purpose mechanical waker whose era ended in early boot, not
an ongoing producer the boot still needs. Even a replay would
likely be sterile: waking suspends nobody — thread 3 sleeps in
SDK code, and slice 3 already proved a ring wake flickers out
when the woken thread finds nothing.

## Grades

- Confirmed: loop decode, both sweeps' zeros, creator behavior,
  era bound (12 files), uniformity, silence since.
- High confidence: no future slice should manufacture ring posts
  (sterile by precedent + fabrication against evidence discipline).
- Unknown: the poster's identity (needs a live catch, below).

## Verification and hygiene

- No code touched, no instruments written, no rebuild needed.
- Scratch deleted: one run log.
- Gates unaffected; review + commit per rules.

Next (recommended): NO slice-34 event — there is no consumer-side
or producer-side opening. If a future boot phase ever replays
early-boot conditions, the trailhead is exact: re-apply the
slice-27 write-watch to the ring slots over a fresh (800, 60000]
leg and the poster names itself by writer pc. Until such a phase
exists, the tripwire stands watch per slice 31.
