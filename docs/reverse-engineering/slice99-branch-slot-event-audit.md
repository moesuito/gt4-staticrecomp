# Slice 99: branch/slot event and trap-resume audit

Date: 2026-10-09; baseline `89cabf6`, branch `slice99-branch-slot-audit`.
Status: source and model-control audit verified. Live reference matrix and
matched update/root interval still pending; no production semantics changed.

## Questions / boundaries

Before dynamic completed-work observation can become time, audit eligibility
between a branch and its executed slot (taken AND not taken), native event
unwinding through callees, and trap/service resumability. Independent pinned
PCSX2 source review delegated read-only. It cannot by itself establish BIOS
resume policy or a matched update/root interval. No reference run launched.

## Confirmed model source and executed controls

Interpreter::step sets transfer_pending_ for taken branches, but a normal
not-taken branch only sets pc+4. Driver::run and boot run_reference permit
interrupt injection, native-entry selection and syscall acceptance when
pending_transfer is false. Thus "pending transfer" tracks the nonsequential
destination, not all instructions executed after branch encodings. Independent
reference source below shows BEQ-not-taken distinctions too; no correction
may be inferred from the name of that flag alone.

New existing-driver fixtures cover BEQ/BGEZ/BEQL taken and not taken, recording
exact callback PCs, then synthetic delivery after one completed instruction.
The callback redirects to BREAK, not a real kernel/CP0/BIOS IRQ. Same-driver
one-attempt budget segmentation is also checked. This is characterization of
current behavior, NOT an architectural acceptance criterion for a new clock.
An ordinary syscall slot tests taken-refused versus not-taken-accepted service.
No existing equality test or production failure phrase weakened.

Executed current observations with base B, target B+12:

| Shape | Event probe PCs (no delivery) | First event due after branch | Work at delivery |
| --- | --- | --- | ---: |
| BEQ taken | B, B+12 | B+12 after slot | 2 |
| BEQ / BGEZ not taken | B, B+4, B+8, B+12 | B+4 before executed slot | 1 |
| BGEZ taken | B, B+12 | B+12 after slot | 2 |
| BEQL taken | B, B+12 | B+12 after slot | 2 |
| BEQL not taken | B, B+8, B+12 | B+8, slot nullified | 1 |

The ordinary not-taken row is the specific audit question. No claim of a new
real-game timing failure from these synthetic event thresholds is made.
Service-slot baseline: taken 1 completed / 0 accepted, stopped at B+4 with
pending transfer; not taken 3 / 1, stopped at B+12. BIOS EPC/BD is not applied
by either fixture: services here are model shortcuts, not CPU exceptions.

Slice-98 likely-slot discrepancy remains separate: translated native accepts
the taken-slot syscall and runs slot+4 rather than retaining a pending target;
interpreter refuses it. The translator only permits exception slots on likely
branches (flow.cpp); ordinary exception slots reject during translation.
Simply making both shortcuts "accept" would not prove reference correctness.

### Stopped slots and native unwind

Three same-state repeated-stop cases in the real generated module and a
bridge-only Driver: BEQL syscall at 0x00100064, JR unsupported slot at
0x00100128, JR ADD-overflow slot at 0x00100100. Both engines report the same
individual kind and PC, the same GPR/CP0/mapped-window effects and completed
work **1**. But native Driver.pending_transfer is **false**, bridge is
**true**. Repeating with a false event callback probes the native slot PC
once while the bridge forbids that probe; neither count increases.

**Confirmed synthetic limitation:** equal architectural snapshots and work
totals do not establish equal event eligibility/resumability. The hidden
interpreter pending target does not travel through a native BoundaryKind.
Boot checkpoint predicates use that flag for clean syscall stops, so a
native syscall-slot stop could pass the clean predicate without an explicit
slot context. This is a source-derived exposure, not a captured bad game
checkpoint. No real boot is shown to hit this case.

New caller 0x0010015C JALs to store callee 0x0010016C; link 0x00100164.
SW completes at callee+4 (0x00100170). Existing DMA-poll hook asserts the
stored word is 0x100 and work total **3** (call + slot + store), saves the
register context and redirects to a synthetic return-service stub. Callee's
Returned propagates through its native caller without obsolete continuation.
A synthetic accepted service restores the saved registers; bridge executes
callee JR/slot, then caller addiu/break. Work **7**, accepted subset **1**,
v0 **9**, same as independently sequenced interpreter control, store once.
This verifies one caller/callee edge, not arbitrary nesting, a timer clock,
CPU exception entry, real handler or BIOS dispatch. Module comparison retains
the existing GPR/CP0/window scope, plus explicit CHCR synthetic word checks.

## Independent pinned PCSX2 source audit (Confirmed in source)

Commit `aa7ab4306e269075784c7ac3eb4b45e6e6c53445`, corresponding to slice-97
v2.9.114 build label. Scout read source only, no payloads/GUI/builds. Parent
independently fetched raw files with Python urllib.request, reverified the
ten hashes below and reviewed critical ranges. URLs:
`https://raw.githubusercontent.com/PCSX2/pcsx2/<commit>/<path>`.
Line numbers refer to this exact commit, not HEAD. No source bytes vendored.

| pcsx2-relative path | Bytes | SHA256 of raw HTTP bytes |
| --- | ---: | --- |
| Interpreter.cpp | 15107 | `ec426922f281645c2aee44f211de937d72c4af782f9d2cb990a7b67d25fda909` |
| R5900.cpp | 25943 | `23974a38273c329054063a324fbe04b13719f484c18ca557de3655d257a8cb3d` |
| R5900OpcodeImpl.cpp | 41783 | `32f5ecad0f5bc4661ac37d1ca9b9ee5557954835c0d5474f4b53ff37a4b468cf` |
| COP0.cpp | 18825 | `5c15c34884f06c95074d725415df0d9751092f9bb9115f3324351ec4aba17686` |
| x86/ix86-32/iR5900.cpp | 74142 | `62c9c84058b9c8bb017688a97cedfd4b39efacd4e1dd41c44f91da4c914ae91c` |
| x86/ix86-32/iR5900Branch.cpp | 16152 | `d8a73475a46b691bfbe4f878f939c15edc7ced9595bd99b12e30b7388df2fe2c` |
| x86/ix86-32/iR5900Jump.cpp | 4263 | `951e248bf5ca08aa2c994ea07b6f1d1da2e8b3511313f5b162c6490b6da8719c` |
| x86/iCore.h | 11459 | `f9edffdf072f1c2d4e4c4cd3700b3d33b874bb549ace0e41793b04a5820c64ea` |
| x86/iCOP0.cpp | 9691 | `4267fdb5b0180263a371d9cf185c4de7018a43a6f00e6858a8da3a86bec61dcc` |
| Config.h | 42928 | `50ad65a5091571ca1ff5474f11129836e449bb525f8d36bfd88557781336e498` |

### Interpreter: distinct branch families, not a universal slot rule

Interpreter.cpp:174–219 preincrements PC, accumulates cpuBlockCycles and calls
the opcode implementation. _doBranch_shared (222–268) sets cpuRegs.branch=1,
executes the slot recursively, then applies the destination and clears branch
only if no exception cleared it. doBranch (270–275) then updates cycles and
tests events. A common slot in a **taken** ordinary/likely branch therefore
executes before that normal event test. Instructions with their own event
tests require separate analysis; this is not an unconditional IRQ-controller
rule. WaitLoop speedhack also exists inside this path (234–260).

BEQ/BNE **not taken** (334–348) directly call intEventTest at **B+4**, without
setting branch or calling intUpdateCPUCycles there. Thus the source allows
testing before the next executed word, matching the location of the model's
BEQ-false callback, not proving equivalent time/eligibility. BGEZ, BGTZ, BLEZ,
BLTZ and linking variants (355–403) false paths do not call doBranch **or
directly test events**, leaving the slot for a later execI. Our bridge callback
policy is uniform, unlike those source call locations. Likely false paths
(411–514) increment PC again to **B+8**, skip the slot and test events.
Integer branches do not establish coprocessor timing semantics.

R5900.cpp:352–374 gates CPU IRQ using domain mask, IE/EIE and EXL/ERL. It does
not have a general "if branch, defer" guard; it passes cpuRegs.branch into
cpuException. The event test evaluates CPU INTC/DMAC exceptions **before**
IOP synchronization/counter updates/internal DMA events (367–428). Signals
raised during this test may be delivered later. Signal, event test and CPU
exception delivery must be recorded separately, not conflated into one event.

### Dynarec normal paths: paired slots before event test

Config.h:1543/1550 enables native branch/jump recompilation in pinned source.
iR5900Branch.cpp:117–164 emits the ordinary BEQ slot in constant and dynamic
taken/non-taken paths before SetBranchImm. Likely paths (243–270) execute
the slot only when taken; false continuation skips it. iR5900.cpp:903–912
writes continuation then calls iBranchTest; 1384–1414 accumulates block cycles
and tests nextEventCycle before dispatch. Normal paths do not test EE events
between branch and slot like interpreter BEQ-false. TrySwapDelaySlot can
reorder safe host instructions; host order is not a guest time measurement.

**Confirmed:** the emulator engines cannot be treated as one identical
branch-event oracle. Do not call the model's ordinary-not-taken flag behavior
an independently proved hardware bug from a general paired-slot assumption.
Decision 0034 covers observed plain fallthrough SW delivery, not an all-branch
clock contract. The model native module also lacks general per-word event
polls: source similarity on normal slots is not runtime timing equivalence.

### Synchronous trap entry versus service acceptance

R5900OpcodeImpl.cpp:1206–1212 subtracts four from preincremented PC and passes
cpuRegs.branch to cpuException for SYSCALL/BREAK. R5900.cpp:95–165 clears
branch, selects vector and, with initial EXL=0, sets EXL and EPC/BD:

- Taken-slot SYSCALL at S=B+4: source composition gives EPC=B, BD=1,
  ExcCode=8; exception prevents applying the original branch target.
- Ordinary not-taken slot has branch clear: source gives EPC=S, BD=0.
- ERL=EXL=0, BEV=0: general syscall vector 0x80000180; BEV=1: 0xBFC00380.
  Initial EXL=1 is not the same EPC/BD rule.

These are source-derived predictions, **not live captured trap states or
physical-console measurements**. GetMemorySize with extra memory can bypass
the trap (1193–1197); avoid HLE controls in an experiment. GT4Recomp services
do not apply this CPU exception context.

Dynarec recSYSCALL (iR5900.cpp:780–795) calls interpreted SYSCALL and sets
g_branch=2; constant FlushCache/iFlushCache can be elided with 5650-cycle
charge. recCall (375–378) flushes interpreter context. BD flush code in
iFlushCall (1272–1279) and FLUSH_CAUSE (iCore.h:320) are disabled;
g_recompilingDelaySlot is compile-side context, not demonstrated runtime
cpuRegs.branch transfer. Branch emission after the slot may write continuation
again. **Hypothesis only:** this could mishandle a synchronous slot trap's
context/vector. No runtime dynarec trap result asserted, no emulator bug report.

COP0.cpp:651–663 ERET selects ErrorEPC/ERL or EPC/EXL, then updates mode and
event scheduling. It does not inspect BD and compute slot+4 or branch target.
BIOS must have selected whatever EPC will be used. **Unknown:** BIOS slot
syscall return policy, especially a service that blocks/switches. Slice-97
normal syscall captures do not answer it.

### Debugger qualification (Confirmed in source, modes of old captures Unknown)

Follow-up source scout and independent parent raw re-fetch/review:

| Repository-relative path | Bytes | SHA256 raw |
| --- | ---: | --- |
| pcsx2-qt/Debugger/DebuggerWindow.cpp | 15908 | `8baa206abd4c751b429e3791cc708cbdd7254e2573070cb6deee510a9de6a7cf` |
| pcsx2/DebugTools/DebugInterface.cpp | 28573 | `5fffe9dc907382a5889e0de6f780d92352d5a190a577de1715dd83a0c72b976e` |
| pcsx2/VMManager.cpp | 118475 | `57532cc8ee102995e3ce8138bb45cfa83854f67affe9a4b86f9f81cbc3043e95` |

DebuggerWindow.cpp:441–488 Run/Pause toggles pause; Step Into computes a
temporary execution breakpoint: default PC+4, taken branch destination,
conditional-false PC+8, syscall target from opcode analysis. It sets skip-first
on the old PC, adds that breakpoint and calls resumeCpu. **It does not force
the interpreter, nor mean exactly one guest word completed**. DebugInterface
61–64 calls VMManager::SetPaused(false); VMManager 2740–2799 selects Cpu from
CHECK_EEREC on x86 and executes Cpu->Execute. Pending configuration changes
can switch/reset it; SetPaused itself only changes VM state (2811–2817), and
config changes set the implementation-change flag (3007–3014).

Thus neither Step Into nor Run labels alone attest the engine. Historical
slice-97 mode **per segment** remains unqualified; don't infer interpreter
from UI step or dynarec from Run. The captured +1 cycle at ordinary WaitSema
is still that measured delta/extraction control, not universal opcode cost,
one-word stepping rule or proof of a selected engine. It is not invalidated
by this source review; its interpretation is explicitly limited.

## Verification / reviewed limits

Read-only fixture reviewer approved word tables, PCs and hand counts. Parent
strengthened the suggested controls: absolute 0x100 store value on both
paths, exact delivered BREAK address, individual stop kinds/PCs and repeated
state/flag/count checks. Fixture is **94 identical words**, base 0x00100000,
376 bytes, exclusive extent 0x00100178. The original 348-byte slice-98 prefix
hash is unchanged (`1022b533806ade30c15c221c286a17a96c6e09a5f1f6d0f7321a365be426ae59`).
LF-normalized spec SHA256
`5228c70081dff4f5dafd750c9b3c57ea53f2fd8b5e4d6ee604b7338eb3245dba`;
little-endian synthetic image SHA256
`d5c60430e66f9e25a3a7d9a857afcd3ca814db4e20a28a20b977cb8c328fe8b9`.
Independent Python extraction verifies both table equality and old prefix.
No payload/generated code tracked, no shared reference source implementation.

Serialized VS 2022 17.14.34 x64 build warning-free, explicit gt4boot no work;
**53/53 CTest (56.34 s)** and **84 Python tests / 6 skips (78 run, 70.937 s)**,
existing socket ResourceWarnings only. Existing 90k differential remains
**22,566,319 completed / 90,000 accepted** identically; full state identical.
No acceptance/failure regex, model identity or serializer changed. Production
gt4boot.exe is byte-identical to slice 98, SHA256
`4c1dd903ee1bdc82d516e714fc5107ef448b34269ffbf83de61fa573d17230ba`.
CORE and streamed 5,314,478,080-byte ISO reverified against unchanged manifests:
CORE `85d26aa8430154967b2633eede929286694ac39e99762527edcec365fd642ff9`,
ISO `67b6c0075837f3ae1132d608acf2858bf13b2dd62d6eae83dff76df02e4e824f`.

24 added logical controls in existing suites: six callback-location shapes,
twelve due-event direct/segmented legs, two ordinary syscall-slot legs,
three native/bridge repeated-slot stops, one nested callee poll/restore edge.
These assert **current behavior**, not hardware acceptance. Later proven
semantic changes must revise characterization expectations rather than keep
a known error merely to retain green tests. Internal interpreter agreement
is not an independent console oracle, even when hand counts also agree.

Reproduce existing gates in VS Developer PowerShell:

```powershell
cmake --build build
cmake --build build --target gt4boot
ctest --test-dir build --output-on-failure
private/tooling-venv/Scripts/python.exe -m unittest discover -s tests/python
```

No live emulator, savestate changes, controller input or reference experiment
this slice. Source-derived syscall entry predictions are not executed BIOS
evidence. State-work equality does not settle precise slot return policy.

## Decision / remaining experiment

Decision 0041 qualifies reference motor, branch family, exception entry and
BIOS-selected return separately. No uniform slot exclusion/service acceptance
fix, time conversion or compatibility identity change in this audit.

Next slice 100 is a runtime reference experiment, not a clock implementation.
Matched reference **update-loop 1000-us delay/root-work interval remains
Unknown and uncaptured**. Source review and synthetic controls do not meet
that requirement. Next runtime experiment must distinguish interpreter/dynarec
with original synthetic branch/trap words, record mode per segment (including
debugger stepping), vector EPC/BD, slot effect and post-ERET continuation.
Use a non-HLE immediate service and separate blocking service; no game/BIOS
bytes in tracked fixtures. Then capture matched real update 1000-us request,
dispatch, runnable root interval and continuation. Keep software rendering,
no cards/input, pin inputs/binary/config/captures, restore reference slots and
clipboard. The menu and production root exclusion remain unchanged.
