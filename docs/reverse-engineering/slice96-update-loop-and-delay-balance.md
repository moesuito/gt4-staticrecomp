# Slice 96: the update loop's conditional one-shot delay

Date: 2026-10-09. Baseline `804b0a3`, interrupt_model 4. Evidence-only slice:
temporary logging/overhead exclusion removed, original policy rebuilt.
No scheduling, clock, checkpoint identity or acceptance gate changed.

## Confirmed measurements

- The model's thread 3 is a generic-wrapper invocation of the game's
  update/frame-sync loop `0x001016E8`, not a dedicated delay-library thread.
  Its object is `0x006DE790`, published at `[0x00617D80]`, with routine at
  object+0x38. `0x005786F0` calls that routine through `0x00578714 jalr`.
- 270 requests in a fresh original-policy 10k run are **1000 microseconds**,
  caller `0x00578B20`, outer return `0x00101760`. This confirms the static
  delay at `0x00101758` via `0x00578B10` -> `0x005AED18`.
- Every matching delay WaitSema (`ra=0x005AEDC0`) sees count **0**. It
  blocks, selects root, then TIM2's callback signals it and final interrupt
  return reselects thread 3. **Rejected:** signal accumulated before these
  waits. A sampled RUN thread context did not reveal this scheduling history.
- Example: request at service 1327, WaitSema/block/dispatch 3 -> 1 at 1328,
  iSignalSema (`ra=0x005AEF68`) at 1329, final return/dispatch 1 -> 3 at
  1330. No ordinary root syscall appears between those events.
- Diagnostic exclusion of handler/return charges, preserving ordinary
  1 ms/service and priorities, gives 273 requests and the same block-then-
  immediate-signal pattern. Root still READY/prio64, only threads 1–3,
  2 RPC pairs, zero GIF payload. Differential at 10k is green over
  8,956,856 interpreter instructions. This exclusion is not adopted.

## Cause: WaitSema's charge consumes the entire requested delay

**Confirmed in the model:** all 270 original-policy waits have CLKS=2,
OVFF clear, valid armed timer flags=3, callback 0x005AEF58, and
`deadline - current = 147456` BUSCLK ticks (exactly 1 ms). No delay has
expired at the service-entry sample. The timer base was taken after
CreateSema's clock charge, not before it.

`Driver::handle_syscall` executes the blocking service, restores root,
then calls advance_time, charging 147456 BUSCLK ticks. The run loop's
next operation is start_interrupt, before a module call or bridge step.
The new TIM2 cause 11 fires at root's exact restored PC **0x005ADBC8**.
Across the 270 delay waits, the very next logged event after restoration
is injection at that same PC and service index. The callback wakes the
higher-priority update loop; corrected final return immediately reselects
it. **Root executed zero guest instructions during these opportunities.**
The other three root restores belong to boot setup/other waits, not these
270 one-shot delay cycles.

First full sample (decimal time values are BUSCLK ticks):

| Event | Service index | Thread / PC | Timer observation |
|---|---:|---|---|
| request 1000 us | 1327 | 3; outer 0x00101760 | frame 5, synced 4 |
| WaitSema id 103, count 0 | 1328 | 3; 0x005ADCE4 | base/current 192675840, deadline 192823296 |
| root restored | 1328 | 1; 0x005ADBC8 | global diagnostic ticks 198131712 |
| TIM2 injected after charge | 1328 | 1; **0x005ADBC8** | global ticks 198279168 |
| iSignalSema callback | 1329 | interrupt on 1; ra 0x005AEF68 | one waiter |
| final return selects update | 1330 | 3; 0x005ADCE8 | exact waiting continuation |

At WaitSema: COUNT=31744, MODE=0x382, library overflow=11. Hand-computed
`((11 << 16) | 31744) << 8 = 192675840`, and
`192675840 + 147456 = 192823296`. This agrees with the live timer fields;
there is no epoch subtraction or guessed conversion factor in this test.

The handler/return-exclusion control also has 273/273 waits with a full
147456 ticks left and immediate same-PC cause-11 injection. It cannot cure
this mechanism: **the charge that expires the delay is an ordinary
WaitSema**, not handler or private-return overhead.

At services 5000..9999 the original run has 4037 ordinary, 155 handler and
808 interrupt-return services; the exclusion control has 4091/157/752.
These rates concern model 4 only; do not substitute slice-94 model-3 rates.
Original 10k run: 31595 module calls, 797092 bridge steps, 8946332
interpreter instructions. Exclusion: 31861/796619/8956856. Both compare
identically within their own policies, retain threads 1–3, two RPC pairs
and zero GIF payload. DMA chain starts (270/274) are not rendered frames.

**High confidence:** service-sized time leaps are the immediate cause of
this measured root exclusion, with the correct priority rule exposing it.
**Unknown:** what independent reference-backed replacement clock will
reproduce timing, boot phases and eventual menu rendering. Agreement of
our two engines does not answer that question. No timing fix adopted.

## Static audit and independent reference (subagent, reviewed)

Verified CORE and ELF hashes against manifests, and disassembled CORE with
`gt4disasm`, independently of generated C++. The update loop begins with
s0=0; it delays only when prior frame sync returned nonzero. Sync
`0x00107878` -> `0x004A22C0` compares scratchpad counters 0x70002050/58.
If already late it returns 1 without sleeping; otherwise `0x004A21F0`
enqueues a stack waiter at 0x70002090 and calls SleepThread at 0x004A2240.
The cause-2 handler `0x004AB430` increments the counter and wakes that list.
The repeated delay is one-shot work from the loop, **not a periodic callback
that rearms itself**.

The 1000-us argument converts through `0x005B8D88`: BUSCLK 147,456,000
divided by 1,000,000, yielding interval 147,456 (0x24000) BUSCLK ticks.
Timer base comes from `0x005B8400` at `0x005B8788`, stored at timer+0x10
by `0x005B8798`; the due deadline is base + interval - accumulated.

At CLKS=2 the getter's current time is
`((effective_overflow << 16) | COUNT) << 8`, where effective_overflow adds
one if MODE.OVFF (0x800) is pending. The getter rereads COUNT and
increments overflow only locally. Handler `0x005B8158` instead updates
the library overflow field and acknowledges OVFF before its due comparison
at `0x005B822C`; do not compensate its old MODE sample twice. Pending
overflow must be handled explicitly when comparing a WaitSema sample.

**Important epoch limit:** diagnostic BUSCLK accumulation starts at ELF
entry, before TIM2 is enabled. Its fixed offset relative to the guest's
timer base is not proof of an expired deadline. Compare the timer's own
COUNT/MODE/overflow epoch, not unrelated absolute counters.

Reference `t0014-post-gate.p2s` (PCSX2 v2.9.114, software rendering, no
card/input), SHA256 `381bebe4e9f5e20c19925d836b19539005d7671f82f98555bd51be574240ed29`:
the same object/routine is present; scratchpad current/synced counters both
0x172; VBlank waiter list points to 0x006DE670 with BIOS thread id 5, and
stack returns 0x004A2390, 0x00107928, 0x00101818, 0x0057871C corroborate
its blocking frame-sync path. Active delay list `[0x00659308]` is empty.
This is semantic object/function/stack matching, **not numeric thread-ID
matching or host-time phase alignment**. A snapshot does not measure syscall
durations or the proportion of conditional delays across a reference run.

The parent rechecked the snapshot hash, RAM/scratchpad values and getter
disassembly after reviewing the read-only subagent's findings. Reference
association is independently corroborated, not inferred from model output.

### Additional static findings preserved from the read-only scout

The thread creation at 0x00101858 stores routine 0x001016E8 at 0x00101878;
stack region 0x006D67E0, requested length 0x8000. Wrapper return is
0x0057871C. Sync condition comes from 0x00107878 -> 0x004A22C0 with
negative argument -1 or -[0x006186E4], selected by [0x006186DC]. The
nonzero result selects the next iteration's conditional delay.

Eight direct calls to 0x005AED18 were found by scanning the verified
decompressed ELF and checked in CORE disassembly:

| Call address | Argument (microseconds) |
|---|---|
| 0x0053AE40 | forwarded a0 |
| 0x00577FB0 | 2000, retry wrapper; not this update delay |
| 0x00578B18 | forwarded a0; 1000 in measured update loop |
| 0x005809F8 / 0x00580A90 | 4000 each |
| 0x00582F3C / 0x00583470 | 600 each |
| 0x005B22EC | s1 * 1000 |

Library control 0x006592F0 holds u64 overflow at +0, timer free list at
+0x14, active list at +0x18. Descriptor free-list head is 0x0088C340.
Timer size 0x40: next/prev +0/+4, generation +8, flags +0xC, u64
base/accumulated/interval +0x10/+0x18/+0x20, dispatcher/gp/descriptor
+0x28/+0x2C/+0x30. Descriptor +4 is timer id, +8 callback 0x005AEF58,
+0xC semaphore. Observed descriptor/timer: 0x0088BF40/0x00889F40.

**Correction to early slice-10 interpretation:** the `ori ... 2` changes
timer+0xC flags, not descriptor+0xC's semaphore. Callback returns zero;
dispatcher 0x005B8ED8 returns -1, freeing descriptor and timer. New loop
calls produce new one-shots. Historical model-3 checkpoint's RUN-thread
saved registers were older than its RAM stack; they cannot identify a
live delay caller. Pre-service live observations above resolve that gap.

## Inputs / tools / next experiment

Reverified local CORE and ISO against `docs/inputs/usa-v2.00.json` before
runtime work. CORE SHA256
`85d26aa8430154967b2633eede929286694ac39e99762527edcec365fd642ff9`;
ISO SHA256 `67b6c0075837f3ae1132d608acf2858bf13b2dd62d6eae83dff76df02e4e824f`.
Scout also checked reconstructed ELF against its native manifest:
`10f82e2231a51404b95682ed3ea81171100a1af2fefdeed3391943016c7c935c`.
MSVC 19.44, CMake/Ninja,
Python subprocess captures and temporary kernel pre-service logging.
Script/captures live in ignored `C:/Users/Alano/AppData/Local/Temp/opencode/gt4-live-session/`:
`slice96_probe.py`, `slice96-mode0-10k.*`, `slice96-mode1-10k-compare.*`.
All requests use fresh boot, not checkpoint resume; diagnostic banners
retain production identity and are not resume-compatible policy changes.

Final captures, same temporary directory:

| Artifact | SHA256 |
|---|---|
| slice96-clock-mode0-10k-compare.stdout | 4d5104b630a1b7b6477cc9e36beb099c820421858dd00f8bdbe84f72dc5aec3e |
| slice96-clock-mode0-10k-compare.stderr | 1c9238bd8a81e1e5312a7c6a5d7f8654f70ef1eec9f47cccda9ff4572fcc8d89 |
| slice96-clock-mode1-10k-compare.stdout | 2beca34908c42ed790cd0c96c5218857c17ceac1c7c74245cf18563dc2e25b02 |
| slice96-clock-mode1-10k-compare.stderr | c3e611a2b939d1b1005ad88a44da645634f1ec3686cefe1d039f45bd0201abc1 |
| slice96-diagnostic.patch (local only, removed from production) | 3f5f8a468562d3a489c879469a858f99f7422cdc644dd114dc122e8bdf149fb2 |

Reproduction with the archived diagnostic patch on baseline (do not use
for production/checkpoint acceptance): build gt4boot, then run the ignored
`slice96_probe.py 0 --compare` / `slice96_probe.py 1 --compare`. Equivalent
boot command, with environment GT4_SLICE96_SKIP_OVERHEAD=0 or 1:

```powershell
build/gt4boot.exe private/fingerprint-check/CORE.GT4 --services 10000 `
  --disc 'Gran Turismo 4 (USA) (v2.00).iso' --quiet --threads --compare-interpreter
build/gt4disasm.exe private/fingerprint-check/CORE.GT4 0x5b8400 20
```

Observation recipe if local artifacts are lost: increment service index
before executing either engine's service, classify nested Interrupt frames
and private returns before they are popped; log live s1 and stack+0x40/50
at CreateSema ra=0x005AED68. At WaitSema ra=0x005AEDC0 decode descriptor
`(s1 & 0xFFFFFF00) >> 4` and timer `(descriptor.timer_id >> 10) << 6`,
validate callback/flags, read fields and coherent COUNT/MODE/overflow.
Log restored dispatch PC and injected-interrupt PC/service index. Increment
diagnostic BUSCLK total inside shared advance_busclk, retaining its
separate epoch. The exclusion control skips only classes handler and
Patch/Interrupt return; ordinary WaitSema still costs the original slice.

## Restoration / next experiment

All temporary fields, hooks, stderr logging and environment-controlled
exclusion removed; production code diff is empty. Full warning-free build
and explicit gt4boot rebuild; unchanged CTest 53/53 passed (55.85 s),
including existing 90k differential. Python 73 tests / 6 skips passes
(65.846 s; existing socket ResourceWarnings). No experiment remains running.

Restored fresh 10k differential repeats 31595 module calls, 797092 bridge
steps and 8946332 interpreter instructions; stdout is byte-identical to
the original-policy diagnostic run (SHA256
`4d5104b630a1b7b6477cc9e36beb099c820421858dd00f8bdbe84f72dc5aec3e`).
`slice96-restored-10k.stderr` is empty (SHA256
`e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855`).
No AUDIT output or exclusion remains in the binary. This confirms removal
without depending on a visual source inspection alone.

Slice 97: obtain independent **relative elapsed time/work** around delay
arming -> blocking -> root execution in the no-card/software reference;
match routine/stack, not thread IDs or host timestamps. Study a shared
work-accounted clock contract that both engines can observe identically,
including non-service guest computation, precise event boundaries and
serialization of accounting residue. This is a candidate, not an adopted
instruction-to-cycle conversion. Do not guess a new per-service quantum,
special-case thread 3/WaitSema, force a root turn, or undo preemption.
Reference timing, implementation cost and compatibility/test requirements
must justify any replacement for decision 0016.
