# Slice 94: artificial clock overhead is contributory, not sufficient

Date: 2026-10-09. Baseline `992e255`. Diagnostic-only experiments; no
production timing policy adopted. Objective: distinguish ordinary thread
services from handlers and synthetic returns without reducing the ordinary
1 ms quantum used by decision 0016.

## Method / input identity

Fresh runs from ELF entry, no checkpoint reuse. Reverified CORE and ISO
size/SHA256 against `docs/inputs/usa-v2.00.json`:

- CORE: `85d26aa8430154967b2633eede929286694ac39e99762527edcec365fd642ff9`.
- ISO: `67b6c0075837f3ae1132d608acf2858bf13b2dd62d6eae83dff76df02e4e824f`,
  5,314,478,080 bytes.

Temporary `Kernel::audit_before_service` classifies **before** the service
executes: 0 = ordinary thread service; 1 = syscall anywhere within an
Interrupt deferred stack (including beneath a nested Patch); 2 = private
Patch return; 3 = private Interrupt return. Private service 0x100 is
classified from the top frame's kind. All these are mutually exclusive;
ordinary calls to a patched service remain class 0 unless under Interrupt.
Class 4 marks the existing one-frame idle advance, not a syscall.

The driver calls classification in `on_service`; the independent
interpreter loop calls it before invoking the service handler. Temporary
nonserialized fields store the current class/service index for logging and
the diagnostic skip policy. Timer edges and frame crossings are logged at
the common `advance_busclk`, with the class of the advance crossing the
boundary. This last classification is an endpoint label, **not proof that
the whole accumulated frame came from that class**.

Modes controlled only in the dirty diagnostic binary:

- 0: original policy, charge every service.
- 1: do not charge synthetic returns (classes 2/3).
- 2: do not charge handlers or synthetic returns (classes 1/2/3).

Ordinary services still cost 1 ms in all modes; idle remains one frame.
Priorities, dispatch rules, replies and translated guest code are unchanged.
No diagnostic checkpoint was saved; the banner retains the production
model identity/time description and must not be mistaken for an adopted
compatible model. CMake was reconfigured to baseline `992e255` before build.

## Confirmed results at 10,000 services

All runs exit 0. Baseline mode 0 exactly reproduces 26,988 module calls and
702,724 interpreted bridge steps from slice 93.

| Mode | Ordinary | Handler | Patch return | Interrupt return | Module calls / bridge steps |
|---|---:|---:|---:|---:|---:|
| 0 | 7,674 | 744 | 17 | 1,565 | 26,988 / 702,724 |
| 1 | 7,869 | 688 | 17 | 1,426 | 27,486 / 707,991 |
| 2 | 7,946 | 669 | 17 | 1,368 | 27,669 / 709,541 |

The counts in each row sum to 10,000. In mode 0 artificial/handler charges
make up 2,326 ms of the 10,000 ms service-time advance (23.26%); excluding
them changes interleaving, so the other rows are not the same trace with
some time subtracted afterward.

Steady window **5,000 <= service index < 10,000**:

| Mode | Ordinary / Handler / Patch / Interrupt-return calls | Device signals / waits | Zero device waits | Workers dispatched |
|---|---|---|---:|---|
| 0 | 3,688 / 392 / 0 / 920 | 300 / 92 | 0 | only 9/13, before this window |
| 1 | 3,828 / 347 / 0 / 825 | 251 / 96 | 0 | same |
| 2 | 3,880 / 330 / 0 / 790 | 233 / 97 | 0 | same |

The device semaphore still receives more than twice as many signals as
consumed waits in mode 2. The ten previously unexecuted workers stay READY
at their entry, and main stays asleep. All modes retain 25 RPC pairs / 33
unknown calls and zero GIF payload. Some early device waits reach zero in
modes 1/2, but **none do in the steady window**; this does not cure exclusion.

Mode 0's ordinary steady calls split: thread 3 = 2,397, thread 4 = 1,291.
GetThreadId dominates (1,292 calls by thread 3, 1,014 by thread 4). The
clock shortcut therefore charges milliseconds even to the frequent identity
queries, not just to operations that plausibly take a millisecond. This
observation is not an independently measured syscall-duration table.

Frame-crossing endpoint labels for that window:

- Mode 0: ordinary 271, handler 6, Interrupt return 23 (300 total).
- Mode 1: ordinary 245, handler 6 (251 total).
- Mode 2: ordinary 233 (233 total).

## Interpretation

- **Confirmed:** handler/return charges contribute time and removing them
  changes event delivery, but neither exclusion mode removes the worker
  starvation in this bounded experiment.
- **Rejected as sufficient correction:** "just stop charging model return
  trampolines/handlers". Do not ship this as the solution to the old wait.
- **High confidence:** the ordinary 1 ms-per-service assumption also needs
  examination. **Unknown:** a reference-backed replacement quantum or
  deterministic work-accounting policy. No duration is guessed into code.
- The engine differential for mode 2 at 10k is green: **9,489,860 interpreter
  instructions**, identical registers/RAM regions/kernel/device banks.
  It verifies shared experimental behavior, not physical clock fidelity.

## Separate interrupt-return audit (read-only subagent, reviewed)

The exploration agent traced the accepted contract in decision 0013 and
the RUN branch of `Kernel::deferred_return`. Static source confirms:
handler wake/signal defers preemption, but the final return simply restores
a still-RUN interrupted thread without checking a newly READY higher
priority. Existing fixtures cover RUN restoration without a rival and
idle wakeup, not this rival case. This is a defect against the **already
adopted internal contract**, not evidence for a new BIOS timing contract.

A safe correction must save the exact interrupted context before making
it READY and dispatching; using the ordinary syscall switch helper after
restoration would add 4 to its PC and skip an instruction. Test the full
handler chain, exact saved/resumed PC, and equal/lower/suspended controls.
No local pinned ps2sdk/ps2autotests source was located for independent BIOS
verification in this audit; no external review was used. This issue is
separate from ten **lower-priority** workers excluded by a busy thread.

## Repetition / captures

MSVC 19.44 x64 / CMake / Ninja; Python 3.14 venv drives three fresh processes
with independent environment/captures. Do not relink their executables while
they run. Ignored script:
`C:/Users/Alano/AppData/Local/Temp/opencode/gt4-live-session/slice94_probe.py`.
Its arguments are `<mode> 10000 [--compare]`; it sets `GT4_SLICE94_MODE`,
runs `gt4boot CORE.GT4 --services 10000 --disc <ISO> --quiet --threads`,
captures native stdout/stderr separately, then groups service/frame/device
events by class and service-index window. `--compare` additionally passes
`--compare-interpreter` and labels both engines' audit events separately.

Capture hashes (same ignored directory):

| File | SHA256 |
|---|---|
| `slice94-mode0-10000.stdout` | `f3966430e5c3eed15c85f8a61de8e1f24849a2e821002fd1f5da35ec9db3773f` |
| `slice94-mode0-10000.stderr` | `a83ac3333d18da6e232f68b2735d95863229feeb6c7597ad04c3ff36a5ef39d2` |
| `slice94-mode1-10000.stdout` | `0b148676d082786dbed6553f3a9013df6c613f8cde5625037342584f2ba41b46` |
| `slice94-mode1-10000.stderr` | `a03979ac83b123064a198e8a46b5ae17dca55a12dda438d7f3d08f33c281d192` |
| `slice94-mode2-10000.stdout` | `d6c8da519625ea7601484bfeb1dabe6aa928a2f0ef2694ff2d8f6edf3cf10ccc` |
| `slice94-mode2-10000.stderr` | `88acdced94e5c959cfae042a3d16829dfe3d99dc89f0b851ffab8f36bcbb699d` |
| `slice94-mode2-10000-compare.stdout` | `9688c22a9d29ab53a4082c73770363cd9abd259fbabaaed03a6ae0a5618f5893` |
| `slice94-mode2-10000-compare.stderr` | `2520ffb7f2fe0e24eb736e5783ba31ce3a73ae68a9771336cde8ef545a305726` |

Next: remove all temporary attribution/skip fields and hooks; verify the
original model, then correct and regression-test the independent
interrupt-return contract defect in a separate verified slice. The clock
replacement remains open and requires reference evidence.

Restoration verified before commit: all temporary code diffs empty; explicit
gt4boot and full build warning-free; CTest **53/53** (66.00 s), Python
**73 tests, 6 skips, OK** (67.214 s; existing socket ResourceWarnings).
No experiment remains running and no skip mode exists in production.
