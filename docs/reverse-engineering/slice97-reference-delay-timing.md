# Slice 97: reference-relative delay timing

Date: 2026-10-09, baseline `25b0269`. Observation/tooling slice; no production
clock, scheduler, checkpoint or translation semantics changed.

## Live reference setup / first positive control

Reverified ISO and CORE against usa-v2.00.json before running. Stock
PCSX2 executable SHA256
`0506a196f5bc76d47f618e68d40519c766bbf5959b8aa88d5b91cee1dcb48037`,
reports v2.9.114; GitHub v2.9.114 tag resolves to
`aa7ab4306e269075784c7ac3eb4b45e6e6c53445`. Use that source version for
layout evidence rather than assuming old 81526d4 and live build are equal.
Active config unchanged: renderer 13 (software), no cards, PINE 28011,
EECycleRate=0/EECycleSkip=0; no controller input. Fresh boot with -debugger
stopped at entry 0x01000008 (original loader), then execution breakpoint at
0x005AED18 produced a real positive hit. First request is **2000 us**,
ra=0x00577FB8, root retry wrapper; do not call it the update-loop 1000-us
request. Captures under ignored private/pcsx2/sstates/slice97-delay-timing/.
Original host slot 9 and .backup preserved there with hashes before saving.

PCSX2 aa7ab430 R5900.h: cpuRegs is frozen raw by SaveState.cpp. Its u64
cycle field offset is 1088: GPR/HI/LO/CP0=672, sa/IsDelaySlot/pc/code=16,
PERF=16, eCycle[32]=128, sCycle[32]=256. Live step of WaitSema syscall
0x005ADCE4 reaches BIOS vector 0x80000180 and advances this field by **1**,
from 1646380287 to 1646380288. This calibrates extraction, not a universal
instruction cost. It is emulator time, not proof of physical-console cycles.

First retry request: entry cycle 1646379414 -> WaitSema cycle 1646380287
(873 EE cycles across setup); continuation at 0x005AEDC0 cycle 1646974149
(593862 cycles after WaitSema). Timer active head 0x00889F40 at wait,
empty at continuation. Remaining time was measured in the replay below;
a matched update-loop **1000-us delay** was not captured.

GUI calibration notes: native tabs/buttons and set_value work; popup menu
actions may update asynchronously. Repeated CSV import produced duplicate
rows temporarily; refresh on actual pause collapsed to the three real
breakpoints. Verify the resulting list/hit and captured PC, never assume
an Invoke return proves application. Quoted CSV uses eight columns and
type 8 for execution (BreakpointModel.cpp / Breakpoints.h at aa7ab430).
PINE cannot pause/step: GUI debugger controls execution, PINE only observes
and saves the paused state. Temporary script slice97_capture.py saves/copies
with explicit expected-PC and preservation checks.

## Read-only scout: feasibility map (reviewed against current source)

No work->time conversion supplied. Dynamic accounting is feasible, but
module_calls and static instruction_count are not executed guest work.
Loop length, internal callees, likely-nullified slots, stops and native
poll exits all defeat fixed cost per module call.

Integration points identified: tools/gt4translate/main.cpp emit_unit_body
(common instructions, branch/slot, call/return, standalone slots, eret and
stops); src/ee/interpreter.cpp Interpreter::step (not shared effect executor,
which would double-count); GuestState for observation hook, but machine
accounting/residue must not restore through RegisterContext thread switches.
Driver syscall handling needs an explicit accepted-service rule; interpreter
stops before applying syscall. Merely observing a stop must not charge again
on resume. BasicBlock::instruction_count includes static stops/slots and
cannot be used as a dynamic count.

Event boundaries require more than a counter: timer MMIO must observe work
before reads/writes, pending CP0 eligibility must be respected, and timer
delivery needs precise native stack unwinding like existing DMA poll but
also at non-DMA code. Native callee exits must propagate without executing
obsolete continuations. Branch/slot pairs must defer interrupt delivery.
Scout found ordinary not-taken interpreter branches do not mark transfer
pending; frequent work-clock events may expose a slot-delivery gap. Existing
syscall-in-likely-slot stop tests do not prove treated-service resume parity.
These are audit/test targets, not demonstrated new production bugs.

Compatibility: new clock requires time identity review, new delivery points
translation/interrupt review. Serialize work residue/unconsumed work/cursor,
compare all added state; current GT4KERN2 has no work->BUSCLK residue.
Generated static metadata must not be mistaken for dynamic instrumentation.

Recommended smallest next implementation: observation-only completed-work
count, no timer/scheduler changes; extend existing synth-module-exits and
ee_module_exit fixtures with independently hand-counted paths (loops,
likely taken/not taken, standalone slot, rewritten RA, known/unknown
indirect, callee stop, eret, repeated stop and overflow). Separately test
event thresholds across branch/slot. Cost conversion remains a later choice.

Parent review: `Interpreter::step` at 2524..2620 reports Executed only after
applying an instruction; traps/unsupported words return before completion.
Ordinary not-taken branches at 2576..2578 leave transfer_pending_ clear.
`Driver::run` at 175..184 uses that flag to prevent IRQ/module entry during
pending transfers. Thus the flag gap is confirmed in source, but no new
runtime failure is claimed here: current service-clock event points differ
from a prospective per-instruction clock. `handle_syscall` at 135..159
accepts a service before advancing time, including Switched/Jumped/idle
outcomes; it must not count an unhandled or budget-limited stop as work.
`emit_unit_body` at 661..899 has separate paths for ordinary effects,
branch/slot, calls, unknown targets and captured returns; fallthrough DMA
poll happens after the store. Its halt/eret path precedes those cases.
`RegisterContext` is the per-thread register save, not machine accounting.
These checks confirm the integration map, not an implemented clock.

## Confirmed reference measurements and their limits

### Retry delay: syscall dispatch leaves nearly all 2 ms available

Reloaded `first-delay-wait.p2s`, then stopped before BIOS WaitSema dispatch
ERET `0x80003490`. Arrival cycle 1646380287, pre-ERET cycle 1646381398:
**1111 EE cycles**. At the nominal 294,912,000 EE cycles/s this is about
**3.77 us**, not a measured physical-console syscall cost. It includes
reference BIOS dispatch, but not ERET execution or subsequent guest work.
Selected EPC **0x00081FC0**, not root. The captured RAM at 0x81FC0..0x81FDC
is six NOPs followed by `0x1000FFF9` (branch to 0x81FC0) and a NOP slot;
the reference's idle context is also visible in the raw TCB words.

At wait: TIM2 stored COUNT=1517, startCycle=1646380032, cycle=1646380287,
rate=512, MODE=0x382 (enabled, CLKS=2, no gate, OVFF clear). Effective
COUNT stays 1517. At pre-ERET, effective COUNT=1519. Library overflow at
0x006592F0 is 1 in both, so time is respectively 17165568 and 17166080
BUSCLK ticks. Active one-shot head 0x00889F40 has base=17165568,
interval=294912, flags=3, accumulated=0. Remaining: **294912 -> 294400**
BUSCLK ticks (about **1996.53 us** at pre-ERET). This directly contradicts
using a whole millisecond as a realistic cost for this dispatch window.
It does **not** supply a replacement constant or prove root phase alignment.

Lazy-count decoding independently checked against PCSX2 aa7ab430:

- `pcsx2/R5900.h`, cpuRegisters: cycle u64 after PERF/eCycle/sCycle;
  computed offset 1088, after the 32-byte freeze tag.
- `pcsx2/Counters.h`, Counter: six u32 fields and u64 startCycle, 32 bytes.
- `pcsx2/SaveState.cpp`, FreezeRegisters/EE-Subsystems, and
  `Counters.cpp::SaveStateBase::rcntFreeze`: raw cpuRegs and counters first.
- `Counters.cpp::rcntSyncCounter`, `rcntCanCount`, `rcntRcount`: with the
  observed enabled/ungated CLKS=2 mode, effective COUNT is
  `(stored_count + (cycle - startCycle) // rate) & 0xFFFF`; mode source 2
  sets rate=512 (twice BUSCLK/256). No pending overflow in these samples;
  do not generalize this expression to gated/HBlank/unsynchronized-overflow
  states. Game-side time formula comes from the slice-96 getter audit.

Sources are pinned URLs under
`https://raw.githubusercontent.com/PCSX2/pcsx2/aa7ab4306e269075784c7ac3eb4b45e6e6c53445/`.
The one-syscall step (+1 cycle) is a separate positive control of extraction,
not calibration of all instruction classes. A subsequent Step Into while
ERET's breakpoint remained enabled re-hit the same PC/cycle: the capture
`replay-first-delay-eret-step` proves **no advancement**, not executed ERET.

### Actual frame waiter versus GPU semaphore waits

First update branch `0x00101750`: cycle 1810607861, s0=0,
sp=0x006DE710, scratchpad frame/synced=3. It skips the conditional delay.
Two later WaitSema captures on the same update stack are **not VBlank
SleepThread calls**, despite their provisional filenames:

| Capture | EE cycle | a0 | ra | frame/synced |
| --- | ---: | --- | --- | --- |
| replay-first-update-vblank-wait | 1815528722 | 0x17 | 0x004A1234 | 4/4 |
| replay-update-second-vblank-wait | 1815529123 | 0x18 | 0x004A123C | 4/4 |

CORE disassembly `gt4disasm ... 0x004A11F8 30` shows consecutive
`0x004A0F08` calls at 0x004A122C/34 with arguments 1/2, followed by
0x004A0F68 calls: GPU synchronization path. **Unknown:** which individual
semaphore waits blocked. No corresponding blocking WaitSema ERET was hit;
do not infer duration or immediate success from the absence of that hit.

The actual frame-sync waiter is independently identified by
`0x004A21F0` disassembly: queue stack node at 0x70002090, snapshot frame
counter, call SleepThread at 0x004A2240, link 0x004A2248. The syscall
wrapper is `0x005ADBC0`, syscall **0x005ADBC4**, service 0x32. Captured:

| Capture | PC | EE cycle | frame/synced | sp / ra |
| --- | --- | ---: | --- | --- |
| replay-update-vblank-sleep | 0x005ADBC4 | 1820459139 | 5/5 | 0x006DE670 / 0x004A2248 |
| replay-update-second-vblank-sleep | 0x005ADBC4 | 1825381503 | 6/6 | 0x006DE670 / 0x004A2248 |
| replay-update-vblank-dispatch-eret | 0x80003338 | 1825383133 | 6/6 | restored idle registers |

BIOS syscall table located by the independently known WaitSema pointer
0x80003440 at physical 0x15610 gives table base 0x15500. Entry 0x32
contains 0x800032C0. Its raw words distinguish **0x80003300 fast-return**
from **0x80003338 blocking dispatch**. The initial breakpoint at 3300
did not capture blocking dispatch; corrected after inspecting BIOS words.
Matched second SleepThread -> blocking pre-ERET delta: **1630 EE cycles**
(about 5.53 us nominal reference time). Selected EPC again **0x00081FC0**.
No active one-shot delay in these three captures (head=0, overflow=6).
This confirms the frame wait/dispatch path, **not root execution**.

**Unknown:** update-loop 1000-us delay duration in the reference, root's
corresponding restored PC/work interval, and phase-relative model alignment.
No observed 2000-us or SleepThread sample substitutes for that experiment.
No clock policy is adopted; production root exclusion remains slice 96.

## Reproduction and capture provenance

Pinned ISO SHA256 `67b6c0075837f3ae1132d608acf2858bf13b2dd62d6eae83dff76df02e4e824f`;
CORE SHA256 `85d26aa8430154967b2633eede929286694ac39e99762527edcec365fd642ff9`
(verified against `docs/inputs/usa-v2.00.json`, not a rewritten manifest).
Disassembly used baseline `build/gt4disasm.exe`, independent of generated
C++. Saved evidence only in ignored directories; no RAM/BIOS/game payload
bytes are tracked. Python 3.14 from `private/tooling-venv/Scripts/python.exe`.

The supported offline reader now exposes:

```powershell
private/tooling-venv/Scripts/python.exe scripts/pcsx2_savestate.py timing private/pcsx2/sstates/slice97-delay-timing/replay-first-delay-dispatch-eret.p2s
private/tooling-venv/Scripts/python.exe scripts/pcsx2_savestate.py registers private/pcsx2/sstates/slice97-delay-timing/replay-update-vblank-dispatch-eret.p2s
build/gt4disasm.exe private/fingerprint-check/CORE.GT4 0x004A21F0 25
build/gt4disasm.exe private/fingerprint-check/CORE.GT4 0x005ADBC0 4
```

`timing` reports raw fields only; it refuses unaudited save version **or
embedded build label**. Save number alone is insufficient: v2.9.93 states
also report 0x9A590000. This corrects the slice-88 journal's claim that the
version-number match alone resolved raw-layout skew. The label is not binary
attestation; input binary hash/source pin remain required evidence.
Temporary `slice97_analyze.py` TCB filtering at +0 is provisional/wrong for
status (observed status at +8); its `tcbs` output is not an adopted decoder.

SHA256 values of `.p2s` files in the ignored slice-97 directory:

| Capture stem | SHA256 |
| --- | --- |
| first-delay-entry | 92e8dc4e2a6add7624c506781aff7171cad79bdf63f0c3bded06f851f64bd2a6 |
| first-delay-wait | ffc56f61260cc7fb8a25527b3bde44d538c8235989a7f1ec15fd58d3ad838603 |
| first-delay-syscall-vector | f80551726e868ec348063c576f7148537b87c63e4abb71923e171a18997f2490 |
| first-delay-return | 9923b008920dc2520f3de9a127315899d18f9ebc31521be092832cd7ba2254c9 |
| replay-first-delay-dispatch-eret | ed1f520fa99c724e80c8c55f246ebab77a44eb87b264cd33d30156a6ad5c232b |
| replay-first-delay-eret-step | 4cf5119410dc3bff5bb54ecf626c4ab10a6dc378aa04a30412ec2fdf92c59361 |
| replay-first-update-branch | 62a248d67bc54cc90ef0bd09c655d6b4d689121f1c6fde4e5d05adc215d05750 |
| replay-first-update-vblank-wait | 69f95dacd32064514e27687634c2f74a3b0fc0c052caf75cc3618139241845f9 |
| replay-update-second-vblank-wait | 34d6ed7871c6020990f710d253046891ecdc3e2a480b141b819a9e6e2a9b06eb |
| replay-update-vblank-sleep | fb9fa63ba2316354828aa5ddc842f18952118ecc34fe3e77678214391ee06e95 |
| replay-update-second-vblank-sleep | ee22b92ac0a7c2fbbb71ee430618590b344907323064c02e9d5371832e3661e8 |
| replay-update-vblank-dispatch-eret | d8ddc136e7d21e39c5fe551d7b8b5786531d5ee1e230504118a807dfbc6c26a6 |

## Experiment closure / next slice

Removed all eight diagnostic breakpoints, verified the empty table. Popup
Delete Invoke initially failed; keyboard menu selection with four Down keys
and Return worked, followed by checking each row really vanished. Shutdown
through native CloseMainWindow and confirmed Yes, **Save State For Resume
unchecked**. Experiment PID 10760 exited; an earlier --help-only PCSX2
window was also closed. No reference experiment remains running.
After shutdown, `slice97_capture.py restore` restored slot 9 and `.backup`
and verified SHA256 respectively
`2311ea75a0d5dbdf929ed64f1fbf3d93c388f4262dfb4bba887a0978dd24429e` /
`ac43c9fc427551969c2a23c649ee1861ec7cb23bef78d0dd9029c33891de10db`.
Preserved clipboard text restored only after checking current text was our
diagnostic breakpoint address. No cards, controller input or timing settings
were enabled. Active timing configuration stays unchanged.

Next slice 98: observation-only **completed guest work** in both engines,
tested with hand-counted dynamic paths and no timer/scheduler coupling.
Ordinary successful instructions and actually executed slots count once;
nullified slots, unsupported words, trapping/unhandled/repeated stops and
native->bridge boundaries count zero until completion. Accepted syscalls
count once at the service owner, not at the stopping emitter/interpreter.
Use a machine-wide monotonic diagnostic total that cannot rewind on thread
restore; handle overflow explicitly. Keep diagnostic observation separate
from future serialized time/residue, and prove segmented-run behavior.
This is an engineering proposal, not an instruction-to-cycle conversion.
Then audit branch/slot event delivery and collect a matched reference root
interval before adopting any work-to-BUSCLK policy or compatibility change.

## Verification and independent tooling review

All 12 captures' SHA256, PC and extracted u64 cycle rechecked against their
JSON metadata. Supported reader reproduced raw fields and the manually
derived 294912/294400 BUSCLK remaining values; independent pinned PCSX2
source supplies the offsets, not the temporary analyzer. Read-only second
review approved the timing reader/fixtures and independently reproduced
cycle=1646381398 and timer2 count/mode/rate/start. Limits retained: the
tag search is not a full freeze-stream validator, build label is not binary
attestation, and a <4-byte version entry can still raise the preexisting
uncaught struct.error in savestate_version. No new unsupported layout is
silently accepted by the timing command.

Serialized VS 2022 x64 build -> CTest -> Python, no concurrent linker users:
build `ninja: no work to do`, no warnings; **53/53 CTest** (54.90 s,
unchanged existing 90k differential included); **81 Python tests**, 6 skips
(75 run, 69.193 s, existing socket ResourceWarnings). Eight added offline
timing tests cover raw 64-bit fields, rejected save/build, missing tags,
truncated CPU/counters, CLI output and CLI rejection. No gate weakened.
