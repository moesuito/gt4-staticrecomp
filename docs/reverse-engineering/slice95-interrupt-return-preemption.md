# Slice 95: complete deferred preemption on final interrupt return

Date: 2026-10-09. Baseline `29d3585`, branch `slice95-interrupt-return`.
Implements the existing decision-0013 contract, separately from the open
clock-policy problem of slices 93/94. No timing quantum or RPC reply change.

## Defect and regression proof

`preempt_if_outranked` deliberately refuses switching during handlers.
WakeupThread/SignalSema make the waiter READY; final `deferred_return`
previously only restored an interrupted thread that remained RUN. It never
looked for a higher-priority READY rival. Decision 0013 explicitly promises
that delayed switch after the handler finishes; static subagent findings
were reviewed against the source in slice 94.

Added six fixtures inside the existing `ee_kernel` CTest:

- Higher-priority semaphore wake, two-handler INTC chain.
- Higher-priority WakeupThread wake, same chain.
- Higher-priority semaphore wake, two-handler DMAC chain.
- Equal-priority, lower-priority and suspended-higher-priority controls.

Root blocks at PC 0x00100040 (continuation 0x00100044), allowing priority-10
worker to run. Worker is interrupted at 0x00100080 with distinctive integer
low/high lanes, FPU, HI/LO, CP0 and VU state. First handler wakes root;
neither that action nor the intermediate return switches. Final return
must dispatch an eligible higher-priority root, saving worker's **exact**
full context. Root sleeps afterward to verify the worker really resumes
at 0x00100080, not 0x00100084 or the trampoline. `compare_contexts` checks
all fields, not just the illustrative registers.

**Confirmed red/green:** the new fixture compiled on the old code and
failed nine assertions (three per higher-priority case), process exit 1.
The three controls passed. The minimal correction makes the entire kernel
unit executable pass, process exit 0.

## Correction and compatibility

After the full handler chain finishes, `deferred_return` asks the existing
ready selector for the best eligible thread. Only a strictly higher
priority preempts. Before dispatch, store `call.context` directly on the
interrupted thread and mark it READY. This avoids ordinary dispatch's
RUN-thread syscall save path (`context.pc += 4`). The return remains
`Jumped`, so the driver must not advance the restored PC either. Idle,
non-RUN, deleted-thread and Patch return branches are unchanged.

Semantic identity changes **interrupt_model 3 -> 4**. Time remains 3,
kernel 2, RPC 1, translation 2. No serialization layout changed:
GT4CPT3 and GT4KERN2 remain the formats. An explicit checkpoint unit case
refuses the old interrupt identity; previously saved slice-93/94 checkpoint
files remain forensic evidence, not resume sources for this binary.

Independent limit: this is a correction against an accepted project
contract with a direct automated reproducer. No newly decoded BIOS
scheduler or measured console timing is claimed. It does not by itself
prove a correction for lower-priority worker starvation.

## Fresh-run effect: earlier frontier, not menu progress

Verified inputs are unchanged from slice 94's recheck. Fresh pinned-disc
runs with production 1 ms/service and the preemption correction:

| Horizon | Module calls | Bridge steps | Threads | RPC pairs / unknown calls |
|---|---:|---:|---|---|
| 10k | 31,595 | 797,092 | 1 READY/prio64; 2 waits sema11; 3 RUN/prio0 | 2 / 2 |
| 100k | 279,827 | 6,522,231 | same three-thread census | 2 / 2 |

Both exit 0 at the shared private return (`0x1604`, service 0x100). The
10k driver/interpreter differential is identical over all compared state,
**8,946,332 interpreter instructions**. No GIF payload at either stop.

This is **less boot progression**, not a menu milestone: the model now
honors the higher-priority delay thread promptly, but main (READY at
priority 64) gets no opportunity to create the later workers in these
sampled horizons. The old uncorrected model's later phase depended on
delayed preemption. A green differential cannot establish which shared
timing policy is console-faithful. Clock fidelity remains open; do not
weaken a proven priority rule to recover a more attractive boot counter.

An actual old checkpoint (`ckpt-10k.bin`, interrupt_model 3) is refused
before resume with exit 1 and the explicit diagnostic:
`interrupt (file 3, this binary 4) ... forensic evidence ... run a fresh prefix`.
The old checkpoint was not modified.

Captures in the ignored `gt4-live-session` directory:

| File | SHA256 |
|---|---|
| `slice95-10k-compare.stdout` | `857136263b06197f9b9a7b8e0c1e85fb4777b10f183bd3f0c8d4b973e4ab190e` |
| `slice95-100k.stdout` | `100ea5b8c055b2f7b38b4195068df1ffaa8abd372cce6fa24694f800a3daaf79` |
| `slice95-old-checkpoint-refused.log` | `58d7b21bcbd791bb05d2a37b32af361412abdbf74811f698eb57ba75d84dd55a` |

Commands: `gt4boot CORE.GT4 --services 10000 --disc <ISO> --quiet --threads
--compare-interpreter`, same fresh command at 100000 without compare, and
`--services 1 --resume <old checkpoint>` for refusal. Dirty candidate
banner names baseline `29d3585` and accurately reports interrupt_model 4;
that commit alone does not contain the uncommitted correction.

## Gates and adoption

MSVC 19.44 x64 / CMake / Ninja; kernel red/green and checkpoint unit pass.
Explicit `gt4boot` and full build warning-free. CTest **53/53** passed
(52.35 s), including the existing 90k driver/interpreter gate. Python
**73 tests / 6 skips, OK** (64.234 s; existing socket ResourceWarnings).
No existing acceptance expression changed to conceal the earlier frontier.

Adopt this verified correction to the existing scheduling contract; do not
claim that passing lifecycle/differential gates demonstrates boot phase or
visible-output progress. All temporary clock instrumentation/exclusion
modes remain removed, the quantum remains 1 ms, and no experiment is
running. Next slice 96 must measure the delay thread's time/self-wakeup
balance under **interrupt_model 4**, then establish independent timing
evidence. Slice-94 rates describe interrupt_model 3 and must not be blindly
reused as the current trace.
