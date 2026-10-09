# Slice 93: scheduler audit and the saturated device semaphore

Date: 2026-10-09. Baseline: `main` = `3ff513f`. Exploration and diagnostic
A/B complete; **no production model change**. Temporary instrumentation and
timing edits were removed, rebuilt and verified before committing.

## Confirmed baseline observations

- The device loop waits before every polling round: `0x005517B8` calls
  `0x00578500` with `[0x0064C718]`, then `0x005517C0` calls `0x00551580`.
  The branch at `0x005517CC` repeats while `[0x0064C714]` is nonzero.
- At 10k, that handle is `0x18F` (raw semaphore id 143). The checkpoint
  records count 254, maximum 255, no waiters.
- A fresh 10,200-service audit logs dispatch, blocking, and semaphore-143
  signals/waits without changing behavior. Between services 5,000 and
  9,999: 300 signals, 92 waits. The wait count grows from 55 to 255; no
  device-loop wait blocks. After initialization each round costs roughly
  54 modeled services, while the service clock generates a VBlank every
  16 2/3 services. This is a rate mismatch, not a missing blocking call.
- Dispatch targets over the entire audit: thread 3 x165, 4 x120, 1 x43,
  2 x1, idle x1, **9 x1, 13 x1**. Threads 9/13 did run and then sleep.
  The other ten workers were never dispatched in this bounded run.
  Slice 92's blanket statement that all twelve workers never ran is
  superseded; a sampled saved entry PC alone was insufficient evidence.
- `0x100` at `0x00001604` is the model's shared deferred-return service
  (Patch or Interrupt), **not idle**. The earlier trace-bucket experiment
  counted deferred returns, not idle events. There is no separate `0x101`
  return in this baseline.

## Evidence and tools

- CORE SHA256 verified against `docs/inputs/usa-v2.00.json`:
  `85d26aa8430154967b2633eede929286694ac39e99762527edcec365fd642ff9`.
- Decompressed ELF SHA256 verified against `usa-v2.00-native.json`:
  `10f82e2231a51404b95682ed3ea81171100a1af2fefdeed3391943016c7c935c`.
- ISO size 5,314,478,080 bytes and SHA256 verified against the disc manifest:
  `67b6c0075837f3ae1132d608acf2858bf13b2dd62d6eae83dff76df02e4e824f`.
- `gt4disasm` establishes the wait/caller addresses. A temporary Python
  GT4CPT3/GT4KERN2 reader obtains semaphore and saved-register fields.
- MSVC 19.44 toolchain, CMake/Ninja, temporarily instrumented `gt4boot`:
  `CORE.GT4 --services 10200 --disc <pinned ISO> --quiet --threads`.
  A Python subprocess captures native stdout/stderr separately and verifies
  process exit 0. Direct PowerShell redirection returned 1 because stderr
  logging was wrapped as ErrorRecords; that was not a guest failure.
- Ignored capture directory:
  `C:/Users/Alano/AppData/Local/Temp/opencode/gt4-live-session/`.
  Audit `slice93-scheduler.log` SHA256:
  `937727c5681471834e25da8e14dd746883bada7e307ed6f5770402027df2bd11`.
  `slice93_analyze.py` counts events and prints the 9,990–10,000 window.
- That window: service 9,991 waits on id 143 with count 255; service 9,994
  changes priority and dispatches thread 4 -> 3. No worker dispatch.

## Independent source audit (subagent, reviewed)

The read-only exploration agent inspected the service clock, contracts and
tests. Confirmed against source: every recognized service advances 1 ms,
including interrupt-handler calls and synthetic returns. `driver.cpp`
invokes `advance_time` after handling; `gt4boot` wires it to
`advance_service_time`. The interpreter uses the same policy. The time
source can therefore feed its own handler work back into time generation.
This mechanism is confirmed; its contribution to this particular failure
needs an A/B experiment. Decisions 0016/0030 explicitly retain the quantum;
engine agreement does not establish hardware timing fidelity.

Separate open audit item: the final Interrupt return restores a still-RUN
thread without a generic ready-priority check. This is not automatically
the explanation for starvation of lower-priority workers; do not conflate
the two issues.

## A/B: smaller quantum, unchanged scheduler

The only behavior change was temporarily replacing
`advance_busclk(state, service_time_slice)` with
`advance_busclk(state, service_time_slice / 10)` in `advance_service_time`.
Idle still advances one frame; priorities, queues, RPC replies, interrupt
delivery and generated guest code are unchanged. Integer quantum 14,745
BUSCLK ticks is approximately 100 us (truncation of 14,745.6), **not a
measured hardware duration**. Run from ELF entry, not from a checkpoint
created under another timing policy.

| Observation | A: original 1 ms | B: diagnostic ~100 us |
|---|---|---|
| Horizon | 10,200 services | 100,000 services |
| Dispatch of workers 5–16 | only 9 and 13 | all twelve by service 2,478 |
| Device thread's wait | always positive in sampled loop | count 0 at service 2,358; blocks; dispatches 4 -> 5 |
| Main thread | asleep in old job receive | RUN at final boundary; old wait no longer persistent |
| GIF payload | 0 bytes at 10,200 | 1,255,616 bytes / 13 DMA starts |
| RPC inventory | 25 pairs, 33 unknown calls | 29 pairs, 42 unknown calls |

First B dispatches: 9/13 at 2,349/2,351; 4 at 2,355; 5 at 2,358;
6/7/11/14/15/16 at 2,383/2,386/2,390/2,408/2,412/2,416;
8/10/12 at 2,471/2,474/2,478. Thread 15 is later deleted; thread 17 is
created but has not been dispatched at the stop. These IDs refer to the
model, not the BIOS reference.

B is **not a completed fix**: thread 4 subsequently stays READY behind
higher-priority activity; its semaphore can accumulate again. Main is
RUN at priority 0; the horizon ends in GetThreadId (`0x005ADB94`), not at
a verified movie/menu milestone. We have not demonstrated rendered frames
or identified all newly reached RPC behavior. More GIF bytes alone do not
establish visible output. Smaller time changes boot interleaving as well
as the post-gate balance, so this is a fresh-run sensitivity experiment,
not an isolated change to the ten-service window.

Diagnostic differential at 6,000 services: exit 0; translated and
interpreted state identical (registers, RAM regions, kernel, device banks),
29,616,464 interpreter instructions. Both use the diagnostic quantum;
this rules out an engine disagreement for that prefix, not a shared model
error. **Confirmed:** changing time policy alone changes worker access to
CPU. **High confidence:** the original service quantum is responsible for
this baseline's saturated-loop exclusion. **Unknown:** the correct
replacement time policy and how much of the effect comes from handler/
synthetic-return charging versus ordinary guest syscalls.

Captures in the same ignored directory:

| Capture | SHA256 |
|---|---|
| `slice93-quantum100us-device.log` | `9ee61370b9cda1df3c58c8f604ea34f1c1701b4c30106b841f3d71748c67dd5b` |
| `slice93-quantum100us-device-summary.log` | `52004e376bd66e9e55580608da12a97fcd7610e63eb1375f07ad309c5ddd2fbf` |
| `slice93-quantum100us-compare-summary.log` | `a640b216a63ce1fc881f8f40d4d61400a85b8975d17fdae042b3955cccc0d9e7` |

Commands: `gt4boot CORE.GT4 --services 100000 --disc <ISO> --quiet
--threads --dump 0x0064C714 0x8`, and separately `--services 6000 --quiet
--compare-interpreter --disc <ISO>`. The preliminary B run used
`--dump 0x01FFFE00 256`: dump lengths are parsed **hexadecimal**, so it
requested 0x256 bytes and crossed RAM's end. Its guest execution reached
100,000 normally but reporting failed at 0x02000000. The corrected repeat
without that dump exits 0 with identical execution statistics. Do not
record that reporting error as a guest fault.

Important provenance limit: the initial experiments' baked banner still
named commit `180e1b3`, and the B banner still described 1 ms. These were
**dirty diagnostic binaries**, not compatible production models; the
temporary quantum was not assigned a model identity and no B checkpoint
was saved. This document supplies the actual baseline/delta. Reconfigure
CMake before relying on baked commit provenance; restored run now names
`3ff513f` and the actual 1 ms policy.

## Reference comparison: initialized structures, not phase alignment

PCSX2 v2.9.114 no-card/software-rendered reference:
`private/pcsx2/sstates/slice88-live-no-card/t0014-post-gate.p2s`, SHA256
`381bebe4e9f5e20c19925d836b19539005d7671f82f98555bd51be574240ed29`.
Read-only Python `zipfile`/`struct` reads `eeMemory.bin`; compared with
the restored production model at 10k using `--dump`.

| Guest location | Model | Reference |
|---|---|---|
| Active flag 0x0064C714 | 1 | 1 |
| Semaphore handle 0x0064C718 | 0x18F | 0x11A |
| Callback head slot 0x0064C878 | 0x0086FD80 | 0x0086FD80 |
| First 16 words at that head | `0064C72C 0064C7D4 00689890`, then 13 zeros | identical |

**Confirmed:** this head/flag structure is initialized in both. Different
raw semaphore IDs are expected under different allocators; the BIOS's
semaphore counter was **not decoded**. A matching list head does not prove
all callbacks, linked nodes, device state or scheduling are equivalent.
The host t=14 s label is not an instruction or phase alignment proof.
A PCSX2 savestate ZIP has no standalone HLE-thread entry, and this does
not imply its BIOS thread state is absent from EE RAM.

## Repeating the audit

Temporary logging sites (removed from production):

1. `gt4boot`'s `options.on_service`: emit a monotonic service number,
   syscall number and PC to stderr **before** execution.
2. `Kernel::dispatch`: emit current ID, selected ID (0 when none), PC.
3. `Kernel::block_current`: emit thread ID, wait type/id and PC.
4. `signal_sema`/`wait_sema`: after valid semaphore lookup, emit count
   before action and handler-active state. Original A selects raw id 143;
   B repeats using `[0x0064C718] & 0xFF` to check the semantic device slot.
   Account for earlier uses/reuse before the slot is initialized.
5. Capture stdout and stderr separately with Python `subprocess.run`;
   count signals/waits by service-index windows, not host time. For
   complete clock attribution, classify the deferred stack **before** the
   service changes it; returns pop their frame and patches can nest.

## Restoration and verification

- `git diff -- src/ee/kernel.cpp tools/gt4boot/main.cpp`: empty after
  removing instrumentation and the quantum change. Reconfigured CMake;
  explicitly rebuilt `gt4boot` and all normal targets, warning-free.
- Restored 10k run: exit 0; 26,988 module calls, 702,724 interpreted steps,
  10,000 services, same baseline boundary (`0x1604`, private return 0x100).
- CTest **53/53** passed (53.91 s); Python **73 tests, 6 skips, OK**
  (66.228 s). Python emits existing socket ResourceWarnings.
- Failed first gate attempt: running Python CLI tests while relinking
  their executables caused Windows LNK1104/LNK1168 locks and seven
  missing-executable test errors. Rebuild and tests were rerun serially
  and passed. Builds and tests consuming those binaries are not independent
  tasks; do not parallelize them.

## Next: slice 94

1. Attribute time advances to ordinary thread calls, interrupt handlers,
   Patch returns and Interrupt returns; diagnose selective exclusion of
   artificial overhead without silently changing decision 0016.
2. Establish a replacement clock contract from independent reference
   evidence, not an arbitrary factor that happens to advance boot. Any
   adopted policy needs identity/compatibility updates and regression tests.
3. Separately verify priority preemption after interrupt return and the
   diagnostic main thread's new running loop. Neither is proved to be the
   correct explanation for every remaining wait.
