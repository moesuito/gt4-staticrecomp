# Slice 100: mode-qualified live branch/trap controls

Date: 2026-10-09; baseline `63a3fc8`, branch `slice100-live-branch-controls`.
Status: bounded live audit closed; interpreter vector/return matrix remains blocked.
No production behavior change.

Goal for this bounded experiment: original synthetic branch/syscall programs,
explicitly selected EE interpreter/dynarec, record vector EPC/BD and selected
return continuation before drawing conclusions from slice-99 source differences.
Matched real update 1000-us/root interval is still unmeasured and not supplied
by a synthetic trap sample. No clock conversion or scheduler fix authorized
by these controls alone.

Owner's active config is Documents/PCSX2/inis/PCSX2.ini, not the stale private
copy (the stale copy uses renderer 3). Active config renderer 13, no cards,
EE dynarec on; no emulator process running before preparation. Plan: isolated
-datapath profiles, absolute private experiment folders and a copied BIOS set
to prevent NVM side effects on the owner's originals. Original settings hash
preserved and checked; synthetic patch/game-fix overrides disabled and recorded.
No clipboard or original savestate slot changes are planned. ELF CLI recipe
and non-HLE immediate service selection delegated read-only and independently
checked against pinned source. `-settings`/`-pause` are not supported; the
actual INI is `<datapath>/PCSX2/inis/PCSX2.ini`. No portable marker present.

## Confirmed first controls and instrumentation limitation

Original fixture `scripts/reference_branch_fixture.py`: 32 words, writable
ELF load at 0x00100000 with zero-fill stack/args extent through 0x0011FFFF;
SetupThread 0x3C outside slots, BIOS-selected stack 0x0011FD60, then positive
GetThreadId 0x2F. GetThreadId is not an HLE shortcut in pinned
`R5900OpcodeImpl.cpp`; both interpreter and dynarec final observations returned
thread id 1. These do not establish physical-console behavior.

**Confirmed source / observed limitation:** `Interpreter.cpp:164-172` guards
address breakpoint/memcheck handling behind `EXTRA_DEBUG || PCSX2_DEVBUILD`.
The pinned release ignored enabled B=0x00100060, hits=0, across reset/rerun;
`-debugger` also did not stop interpreter at ELF entry. Do not use its Step
Into as live single-word evidence or pretend to have interpreter vector data.
A developer build/instrumented interpreter would be a separately pinned input.

Dynarec did stop at entry and B. Plain syscall (no branch at B), all dynarec:

| Segment | PC | EPC | Cause | Status | v0 | cycle |
|---|---|---|---|---|---|---:|
| entry | 00100000 | 00100000 | 20 | 70030C11 | 00100000 | 44458426 |
| before | 00100060 | 0010002C | 20 | 70030C11 | -1 | 44458843 |
| vector | 80000180 | 00100064 | 20 (ExcCode8/BD0) | 70030C13 | -1 | 44458845 |
| pre-ERET | 80000328 | 00100068 | 20 | 70030C13 | 1 | 44458909 |
| first continuation | 00100068 | 00100068 | 20 | 70030C11 | 1 | 44458910 |

This positive control proves live exception entry and real ERET completion.
ERET candidates were scanned read-only in captured RAM: 0x80000328/374/488;
GUI independently disassembled the hit at 0x80000328 as ERET. Table cell
0x800155BC pointed to 0x80004540. No claim that all candidates execute.
An enabled vector breakpoint re-hit with identical PC/cycle/state; archived
`plain-vector-rehit` is a **failed-advance control**, not executed BIOS work.
Direct table checkbox click/Toggle/Space did not disable it; Edit dialog
Enable successfully disabled it. Disable current stop before Run.

Private archives: `private/pcsx2/sstates/slice100-live-branch-controls/`.
Each capture has JSON PC/CP0/GPR/cycle and SHA256, original `.p2s` ignored.
Capture helper archived under the private experiment's `helpers/`; its
timing reader refuses any build/version other than the audited v2.9.114 layout.
Interpreter pilot-latch PC00100078: v0=1/s1=11, status70020C11, cause20,
EPC00100068, cycle3640888802. Late manual pause, **not** a dispatch-cost sample.
Plain ELF SHA256 `0078338809212e74dd6006d1f4a6a5f3ce75ca3aefc1030534e03accdd5c19ff`,
4224 bytes; pinned executable SHA256
`0506a196f5bc76d47f618e68d40519c766bbf5959b8aa88d5b91cee1dcb48037`.

## Six live syscall-slot paths (dynarec only)

Fresh processes, preloaded B/vector/continuation/latch breakpoints. Vector
condition `v1 == 0x2f` excludes ordinary SetupThread. Run to B, archive paused
state, Run again; no Step. All continuation captures precede their marker word:

| Branch | t0 | First stopped PC | EPC | Status | cycle before -> after |
|---|---|---|---|---|---|
| BEQ taken | 0 | 00100074 | 00100060 | 70030C13 | 44458923 -> 44458925 |
| BEQ false | 1 | 00100068 | 00100060 | 70030C13 | 44458923 -> 44458925 |
| BGEZ taken | 0 | 00100074 | 00100060 | 70030C13 | 44458943 -> 44458945 |
| BGEZ false | -1 | 00100068 | 00100060 | 70030C13 | 44458943 -> 44458945 |
| BEQL taken | 0 | 00100074 | 00100060 | 70030C13 | 44458943 -> 44458945 |
| BEQL false | 1 | 00100068 | 0010002C | 70030C11 | 44458943 -> 44458944 |

All have Cause20/BD0, v0=-1, v1=2F, s0=s1=0, delay flag0. Five cases establish
EXL, but **do not execute a BIOS-selected return**. BEQL false leaves CP0 at
its prior SetupThread state and annuls the slot. Conditional vector hits were
zero in the observed debugger table. This is not a slot vector-entry capture.
BEQ taken reproduced exactly with preloaded and mid-run-loaded stops (same
selected PC/cycle/registers); the latter is retained as a perturbation control.
Neither excludes all debugger effects.

### Source composition, independently inspected after delegated research

Pinned `iR5900Branch.cpp:117-177` emits the BEQ slot then `SetBranchImm`.
`iR5900.cpp:780-795` recSYSCALL calls interpreted SYSCALL and sets compile-time
g_branch=2; the later SetBranchImm (:903-912) changes g_branch=1, emits PC of
the branch continuation and dispatches. The slot compiler does not preincrement
its PC as an ordinary instruction does (:1706-1731,1858-1865). Thus the flush
PC is S=B+4, and `R5900OpcodeImpl.cpp:1206-1207` subtracts four before calling
cpuException(20,cpuRegs.branch). `R5900.cpp:139-152` uses that runtime flag for
BD/EPC; compile-time slot flags are not runtime cpuRegs.branch. The dormant
BD-flush block in iR5900.cpp:1272-1279 is `#if 0`.

**Confirmed source composition / high-confidence explanation:** with initial
EXL0/runtime branch0, the exception update selects the vector and EPC=B/BD0;
subsequent emitted branch code overwrites PC before dispatch. Cause/EXL prove
the synchronous exception update, not handler execution; vector zero hits and
unchanged v0 agree. Do not describe this as "SYSCALL never executed" or infer
architectural BD from EPC=B alone. No physical-console result is supplied.
Delta +2 is a pinned emulator block sample, not instruction-to-cycle policy.

## Effect, segmentation and engine controls

Six dynarec effect-slot fixtures, fresh launch with **only latch breakpoint**
0x00100078 (not B/vector/continuation), Run from ELF entry, capture first latch:

| Branch | s0 (slot effect) | s1 (continuation) | EE cycle |
|---|---:|---|---:|
| BEQ taken | 1 | 22 | 44458907 |
| BEQ false | 1 | 11 | 44458909 |
| BGEZ taken | 1 | 22 | 44458927 |
| BGEZ false | 1 | 11 | 44458929 |
| BEQL taken | 1 | 22 | 44458927 |
| BEQL false | 0 | 11 | 44458928 |

All stop at PC00100078, EPC0010002C, Cause20, Status70030C11, v0=-1/v1=2F,
sp0011FD60. These independently distinguish ordinary slot execution from
likely-slot annulment; they do not test exception handling.

BEQ/BEQL taken syscall **latch-only** repeats retain EPC=B, BD0, EXL1, v0=-1,
s1=22; cycles44458927/44458947. Removing intermediate breakpoints does not
recover the handler. A further BEQL taken dynarec run loaded an empty breakpoint
list, resumed from the debugger's ELF-entry stop and was paused globally later:
PC00100078, EPC00100060/Cause20/Status70030C13, v0=-1/v1=2F/s1=22,
cycle3148876800. This is **no-range-breakpoint final state**, not a first-return
or trap-cost measurement, and not a run with the debugger entirely absent.

Same original BEQL-taken syscall ELF, fresh interpreter-selected process,
empty breakpoint list, automatic execution past entry, later global pause:
PC80012BC0, EPC800148E0/Cause8/Status70020C02, v0=v1=0,
spFFFFFFFF800236C0, cycle7409700723. Its log records ELF activation then
`[2.1002] branch delay!!`; the fixture words in saved RAM at B are still the
exact generated BEQL/syscall words. It did **not** reach the fixture latch.
Archive name `beql-taken-early-bios-pause` describes the paused location, not
proof it was still booting: activation preceded this sample.

**Confirmed correction:** warning text comes from pinned `R5900.cpp:142-146`
inside cpuException's bd path, **not** from the BIOS. It is evidence of a
BD-qualified exception update somewhere in this run, not a captured service2F
vector entry. The later Cause8/EPC is not the target syscall's initial context.
**Unknown:** exact failure chain and BIOS-selected syscall-slot return;
neither infer a normal return nor label the later BIOS location a captured
fatal handler. The warning was not found as bytes in captured RAM/ROM0 because
it is emulator console output. Earlier speculative BIOS-message attribution
was corrected before publication.

## Reproduction and qualification

Generator (no BIOS or game payload, refusal to overwrite):

```powershell
private/tooling-venv/Scripts/python.exe scripts/reference_branch_fixture.py private/pcsx2/sstates/slice100-live-branch-controls/new.elf --branch beq --taken yes --slot syscall --service 0x2f
```

Launch template, absolute paths required:

```text
pcsx2-qt.exe -debugger -bios -elf <absolute-original-fixture.elf> -datapath <absolute-mode-directory>
```

Effective profile: `<mode>/PCSX2/inis/PCSX2.ini`, not the initial `<mode>/PCSX2.ini`.
Copies `<mode>/PCSX2-effective-launch.ini` preserve launch settings. StartPaused
true and -debugger did not establish interpreter entry pause; trust observed PC.
Both profiles select software renderer13, no cards, no injected controller input,
no patches/game fixes, PINE28011, normal EE cycle rate/skip. Only EE EnableEE
differs; IOP/VU options retained (including MTVU, which warns on savestate save),
and inherited nondefault FPU clamps are not silently normalized.
These are qualifications, not a claim of default-hardware configuration.

Private helper `slice100_breakpoints.py` writes exported JSON schema to the
isolated synthetic game's debugger settings only; filename CRC is XOR of all
little-endian ELF words (`Elfheader.cpp:248-256`). Load from Settings replaces
the list; preloading on fresh launch avoids mid-run engine reset perturbation.
Current-stop Run may re-hit at equal cycle: verify saved PC/cycle, do not count
button presses. Breakpoint Edit/Enable, rather than the table checkbox, was
needed for verified plain vector/ERET advance.

`helpers/slice100_capture.py <mode> <label> <expected-PC-hex[,alternate-PC]>`
requires PINE pause, saves only isolated slot9, waits for a changed complete ZIP,
refuses archive overwrite, verifies actual PC and pinned timing layout, then
archives CPU metadata and SHA256. Final latch samples may allow 78/7C, but
vector/return samples require the exact stop. Reader offsets come from slice97,
not from guessing a new layout. `slice100_close.ps1` validates a single isolated
process command line, posts normal Close to its windows and Return to its exact
Confirm Shutdown modal; no force kill or resume-save. Native messages were
needed because the modal lacked accessible children and GUI Return targeted
the main window. Four helpers archived privately (not project tooling/inputs).

### Independently refetched source hashes

Commit `aa7ab4306e269075784c7ac3eb4b45e6e6c53445`, raw GitHub HTTP bytes;
delegated read-only findings reviewed, six hashes independently refetched and
checked by the primary agent using Python hashlib/urllib. Key emitter/SYSCALL/
breakpoint excerpts inspected independently. No upstream mutation.

| PCSX2 path | SHA256 |
|---|---|
| pcsx2/x86/ix86-32/iR5900.cpp | `62c9c84058b9c8bb017688a97cedfd4b39efacd4e1dd41c44f91da4c914ae91c` |
| pcsx2/x86/ix86-32/iR5900Branch.cpp | `d8a73475a46b691bfbe4f878f939c15edc7ced9595bd99b12e30b7388df2fe2c` |
| pcsx2/R5900OpcodeImpl.cpp | `32f5ecad0f5bc4661ac37d1ca9b9ee5557954835c0d5474f4b53ff37a4b468cf` |
| pcsx2/R5900.cpp | `23974a38273c329054063a324fbe04b13719f484c18ca557de3655d257a8cb3d` |
| pcsx2/Interpreter.cpp | `ec426922f281645c2aee44f211de937d72c4af782f9d2cb990a7b67d25fda909` |
| pcsx2/COP0.cpp | `5c15c34884f06c95074d725415df0d9751092f9bb9115f3324351ec4aba17686` |

The interpreter's **per-word probe**, not the whole intBreakpoint helper, is
compiled under EXTRA_DEBUG/PCSX2_DEVBUILD. Source also warns (:161-162) about
debugger cycle-count usability in the interpreter. Observed ignored stops agree
with a release build without that probe; build label alone does not independently
prove all compile definitions. A Devel build must be pinned and qualified anew.

## Closure and next experiment

Confirmed: build no work/warnings, CTest **53/53**, 52.44s; Python **91** collected,
85 run/6 skip, 72.281s (7 new generator controls). Existing Python socket fixture
paths emitted two ResourceWarnings; suite returned success, not warning-free
Python. No acceptance expressions weakened. Production SHA256 still
`4c1dd903ee1bdc82d516e714fc5107ef448b34269ffbf83de61fa573d17230ba`.
All **31** archived .p2s hashes and metadata PC/CP0/7GPR/cycle/delay flag were
re-extracted and matched; this checks archives against metadata, not hardware.

All experiment processes closed normally, no reference experiment running.
Owner INI hash still `5dce374066ec9210241fed4a19b9816a9510f253433c61cf82a9631aeae19979`;
six original BIOS files verified against the initial preservation manifest,
including ROM0 `f609ed1ca62437519828cdd824b5ea79417fd756e71a4178443483e3781fedd2`.
Owner Documents/PCSX2/sstates slot9 and backup still match slice97's restored
`2311ea75a0d5dbdf929ed64f1fbf3d93c388f4262dfb4bba887a0978dd24429e` /
`ac43c9fc427551969c2a23c649ee1861ec7cb23bef78d0dd9029c33891de10db`.
The private stale slot has a different hash and was not treated as owner input.
No clipboard operation or input injection performed. Failed shell quoting of
the build command was replaced by a Temp .cmd using call VsDevCmd x64; both
full suites ran serialized after reference shutdown.

Next slice101: instrument/pin a separately qualified interpreter observation
path to capture the **first** slot exception and BIOS-selected return, including
the likely-slot model gap; inspect the interpreter failure chain without copying
dynarec's lost-vector behavior. Then match real update1000-us/root interval.
No guessed clock conversion, timer policy or scheduler exception; main READY
starvation and original-menu/rendering goal remain unresolved. Decision0041
still applies. Do not mistake these synthetic observations for boot advancement.

## Hash ledger (no payload bytes)

Original ELFs, all4224 bytes, generated by the tracked fixture script:

| Fixture stem | SHA256 |
|---|---|
| plain-syscall | `0078338809212e74dd6006d1f4a6a5f3ce75ca3aefc1030534e03accdd5c19ff` |
| beq-taken-syscall | `07b7ed08d138fd67a7ed46556f919947cac054cfb27118e6736df7b7f8847e7f` |
| beq-false-syscall | `70f105dd12985c50c2e01cf4d7e855dc551dee79323fea7b45d2fe865f3a6a59` |
| bgez-taken-syscall | `06eb2030005b2ece29cd673d197f8a090593ac38d82fd0d6adebd9eac617fd59` |
| bgez-false-syscall | `055a58fa762e45429b8b8263c7f110f39bb1450089b7273801eddb28a3937ee8` |
| beql-taken-syscall | `084c302a58e4148d2c5eb8c830ac2824b5006161cdf3d7735ac85c531a53ecf7` |
| beql-false-syscall | `f751b6c4d5fcd4be1cae260b30ff0488c3baf932d3f8fac633db679d894abc16` |
| beq-taken-effect | `30f7ce33259c5655a23e4beffdfd65106a96cddde7fed15559bd5273b16f2af0` |
| beq-false-effect | `9110fc5e5c88be9a378628d2cb6e6987a6765e425e1750c953cf55be6427a1cd` |
| bgez-taken-effect | `ed475fd4c379910979e85a55b3996077e1e8ac4884bad92f080b7688f67a68f0` |
| bgez-false-effect | `2b2f25f0920e8d3991d17489da453cd7574f637edec22a1eedc10c976f7aa8a5` |
| beql-taken-effect | `06a2212540479dfdcc8cc136518164565893e99ac9d35a478995c35723bc2e01` |
| beql-false-effect | `5242a36100dcdd53b4118479f0ce57ada327be63b8a7ebcc66a00ce3a8e3690f` |

Archives relative to ignored experiment root; each stem has .p2s and JSON:

| Capture stem | .p2s SHA256 |
|---|---|
| dynarec/plain-entry | `397129b3c3ad61a8e81603f900ee3386a23f195045ba60b90a99c5c5eda280f4` |
| dynarec/plain-before | `e9e73e28ab35aed3a575c15e1d0d205d26ca7d8805b4768bc09db8da4cb55047` |
| dynarec/plain-vector | `a114e4beda9ccf268e4e81317f82dd5e200bc0e839c0eedc4af3bb3a2719cf5d` |
| dynarec/plain-vector-rehit | `51ced399a4c54cfe1b42f0c279751e78bd81c8ea7a96a30192a0156b8bf0f19b` |
| dynarec/plain-pre-eret | `55813946e5e9c87659131bcc990a8a7ad4bc1c2c73b56123cbc98f252d5842e2` |
| dynarec/plain-continuation | `74d52a86cb89c0b4c6e1be85983f3fdc73971f331e17461a00ade79cb0aec09a` |
| dynarec/beq-taken-before | `d7d1380a88ce875e86d4a5659f3a7af9d127d690a9a87081d4f31bc8b79ab704` |
| dynarec/beq-taken-target | `2e5e4baada7ed403e1aee72db6ca4c4cd3487094606a397db61e36736e59bc51` |
| dynarec/beq-taken-preloaded-before | `9af1bebd6619997d2649bacc86a71741880b9985499a24a408f2bd5c0ea5fce3` |
| dynarec/beq-taken-preloaded-target | `84e738d5589972b7b3a13476e76814f341af572fbb72fe6dc1cdf9be7fa5bb93` |
| dynarec/beq-false-before | `0d29ea08e503160ccdc8edaa406de65b6d7404845c5c47e8a911c9abda4d2c89` |
| dynarec/beq-false-fallthrough | `38818335c0a426d01c12430e499b3157e6dcf756bb1e0072f5cbc1676edec437` |
| dynarec/bgez-taken-before | `f00730402b3d491dc56c46cb62847ca9288319878e2f7668aee50cc415601f23` |
| dynarec/bgez-taken-target | `0e267c5f073e7451abffd08d434e4f9104c18139110186f4f5cbafb2a66745bb` |
| dynarec/bgez-false-before | `97c54faa1d7708a023a25767c5d6744ab44c591cfa044683e0256d8d2660fe9e` |
| dynarec/bgez-false-fallthrough | `182ae6e9235a9b3bea9684c56961c665c26f7fe3767388420b9e47c2fc9db46e` |
| dynarec/beql-taken-before | `8492ac25edf75159606b1f4466748e68f01f3210a4b247e59f3edfac544dde64` |
| dynarec/beql-taken-target | `402722c79c37b0d162d56cab63fcd5bba6bc9235de50c160a7dcc14f0dcc5c3d` |
| dynarec/beql-false-before | `8b0cd2135df36ab45f1c23ba41d21f4da7e63fed2be8b22b7ba823e973c7cc04` |
| dynarec/beql-false-fallthrough | `ee7db2db552295706e3050f9cb92c618ac386f40aec38e9345569cf26ef6a8f5` |
| dynarec/beq-taken-effect-latch | `245f3a487bf283bd2e04371dcfd6c2fdef7564d78018062c2866be7f7ae12564` |
| dynarec/beq-false-effect-latch | `4f15e42810013df0155588a253056a81f3eb92e49eae300c99cc9891fbb7eba7` |
| dynarec/bgez-taken-effect-latch | `f58d922d45257e25f2508ecec6fc5631aebfd8979d4f6af2935b24142026646d` |
| dynarec/bgez-false-effect-latch | `51a80f52f98c185a4f0ed704625a8e6127fa9bcb7d03e923601ad5cb94172c26` |
| dynarec/beql-taken-effect-latch | `01d4db40967ad6ad089b5cc9b4ae8838513a7228bcb21ecf5760ec62e194f7d4` |
| dynarec/beql-false-effect-latch | `b4e0cff555e00f11cde084c05fe337be9e8be3b9e8eb4a91788629ef1cdc4392` |
| dynarec/beq-taken-syscall-latch-only | `59becda8efc67461d1fd5b0a9ec7d8e94d3c5a50801ec04f6d408f8e4d0dbe69` |
| dynarec/beql-taken-syscall-latch-only | `94599fbf3bab4f712c09430e419beadb8ad9b85ad620d995e06394a71e369390` |
| dynarec/beql-taken-no-range-breakpoints | `05ac8148dff3a27d96175e6cd068272d0558eba85e6b61578d8928ed7cdfc466` |
| interpreter/pilot-latch | `e233faa1ae2ac8f0f29cfa83285355957a0cfa832a19d0a2ddc3848b77c1f150` |
| interpreter/beql-taken-early-bios-pause | `7bea2f63e5f6815618906d8341b1c4753d48e4a0c1cf6ce77a1fce7cdd4040f2` |

Private helper SHA256:

| Name | SHA256 |
|---|---|
| slice100_capture.py | `8c57fd3a80a4d0a957212efdcde80d1127a3ec36879eb45cb1330a524fdfb6d0` |
| slice100_breakpoints.py | `9d5d7fe270b4ae366d525d34c14fa9976d2c27f6ef3c3ab8aadeae78a96168c0` |
| slice100_close.ps1 | `d15609dea12a5c9f719a6a6c0caa5034d85a77f005a41fa96c1b305c25787dce` |
| slice100_next.ps1 | `9b90661d900fc51147c2ac2b3e4bf4eccaaa302023d401961cafbe84106a54e6` |

Publication: verified slice100 commit `2e0de29` pushed on its branch, fast-forward
merged into main and pushed to origin. Artifact ledger independently matched
48 private files (31 captures/13 ELFs/4 helpers) against the document. Launch
INI EE modes/software/card disables and normal cycle rate/skip independently
re-read from preserved effective-launch copies. No reference process remained.

Owner-requested Desktop source archive refreshed without overwriting old ZIP:
`C:/Users/Alano/Desktop/gt4recomp-fonte-2e0de29.zip`, source commit
`2e0de29f5ac8fec78e3c16db640fe2271b6ddbd7`, 381 members including provenance
text and current generated `whole_program_tu.cpp`/`whole-program.hpp` only.
Excludes ISO, BIOS, private tools/captures, build, .obj and other binaries.
157,244,275 uncompressed bytes /16,331,733 ZIP bytes; SHA256
`583ea1896e7616f2ed1afaa3009d470f30667e65e1ed08e691de238d2a8df789`.
Member list checked exactly, all source members independently streamed and
hashed against working files after creation. These are the current generated
files, not a promise that every private counted specimen is included. This
publication paragraph is added after the archive's pinned source commit.
Post-merge recheck: gt4boot build + unchanged 90k service/work/full-state gate
**2/2 (13.58s)**, generator **7/7 (0.310s)**. Closing docs add no semantics.
