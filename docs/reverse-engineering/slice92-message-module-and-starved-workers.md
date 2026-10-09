# Slice 92: the message module mapped — the producer and the starved workers

Date: 2026-10-09. Baseline: `main` = `0dbbac6` (slice 91). No model code
changed (exploration only). Task: find the message-post function and its
expected trigger; compare the job object with the reference; inspect the
stream chain.

## The message module (0x00576xxx), mapped

Function starts found by prologue scan; direct `jal` callers counted over
the reconstructed ELF:

| Function | Role (evidence) | Direct callers |
|---|---|---|
| 0x00576418 | queue/lock core (waiter list walk; calls SleepThread when contended) | 0x00576790, 0x005768A0, 0x005769F8 (internal) |
| 0x00576550 | **lock with owner** (owner = current thread id at +0x18, count at +0x20, priority boost +0x1C/+0x24) | **0x0022FC10 only** |
| 0x00576640 | try-dequeue; its exit path walks the waiter list (obj+0x0) calling `WakeupThread` per waiter | 0x005767C8, 0x00576884, 0x00576A30 |
| 0x00576788 | "proceed" wrapper → 0x00576418 | ~300 sites, mostly early code (0x0010xxxx, 0x00109xxx) |
| 0x005767A8 | lock wrapper → 0x00576550 | **0x0022FC10** |
| 0x005767C0 | try wrapper → 0x00576640 | same early-code family |
| 0x005767E0 | **blocking receive** (sleeps until a message) | 0x109810, 0x10AE1C, 0x154C44, 0x1551A4, 0x4AE040, **0x4AF3C4** (the main thread), 0x574E58 |
| 0x00576860 | second receive variant (sleep-once + proceed) | 0x109984, 0x154BF8, 0x154EF0, 0x156138/318/4B0/5D0, 0x4AE094 |
| 0x005768D8 / 0x00576968 | receive/lock variants used by the job framework | 0x4AE094, 0x4AF4D4 (and early code) |
| 0x005769F0 / 0x00576A28 | entry points for the 0x20xxxx–0x23xxxx subsystem | 0x206xxx–0x235xxx family |

The lock's only caller (0x0022FC10, function 0x0022FBE0) is in the
0x20xxxx–0x23xxxx subsystem: it locks an object at +0x48, then walks a queue
at +0x34 — a worker/queue processor, not the main thread's producer.

## The main thread's wait, re-read

- The job object (thread 1's stack, 0x01FFFE00) holds: state word +0x80 = 1
  (the wait wrapper 0x004AF3A0 skips the blocking receive when +0x80 == 3);
  lock fields +0x18/+0x1C/+0x24 = 0xFFFFFFFF (owner -1, unclaimed); the
  handler table +0xA8 = 0x00688F28 (entry 4 = the wait wrapper); callbacks
  into the 0x004Axxxx stream region (+0x30 0x004AC640, +0x64/+0x6C
  0x004AD048/0x004AD110, +0xC8 0x004AFA88) and the job framework
  (+0x120/+0x158 0x004AE39C/0x004AE254).
- So the job is **idle/unclaimed** and the main thread blocks in the
  receive; a producer must claim the object (lock) and deliver the message.

## The starved workers (new, Confirmed)

At 10k and at 18.5M services the 12 workers (threads 5–16, priority 13/14)
are **READY but have never run**: their pc equals their entry (0x005786F0)
and ra = 0. The CPU is held by two game loops:

- **thread 3** (priority 0): the delay library (ra 0x005AEDC0 — the
  WaitSema/DeleteSema delay cycle);
- **thread 4** (priority 1): the device polling loop (0x005515xx): raises
  its priority, walks device-table entries at 0x0086FC80 (stride 0x6C,
  calling 0x0058F0B0), restores priority, tries a message on the global
  object 0x0086F940 (non-blocking), then processes callbacks at 0x0064C878
  (0x00551D08 → 0x00552290 per node). The device interrupt handler
  (0x00551728, cause 2) signals sema [0x0064C718] on device events.

Both loops look legitimate; the question is whether thread 4's loop should
block somewhere (letting the workers run) and why it never does in the
model.

## Interpretation (labeled)

- **Confirmed**: the main thread's job is unclaimed and waits for a message;
  the message module's wake path exists but never runs for this object; the
  job workers have never executed.
- **Hypothesis**: either the workers are not needed for this job (the
  completion should arrive via the device/interrupt callback path) or the
  device loop starves them (its intended blocking point never triggers in
  the model). The next experiment decides: instrument the model's scheduler
  for a small window and log every thread switch with its reason, then read
  the reference's equivalent behavior.

## Next

1. Temporary scheduler instrumentation (window ~9,990–10,000 services): who
   runs, who is starved, why no switch to the workers.
2. Find the device loop's intended blocking point (its caller/loop wrapper;
   the waiters of the device sema 0x0064C718; the callback list 0x0064C878
   contents at 10k).
3. Re-check the reference's t=14 s state for the same structures by meaning
   (device loop state, callback list).
