# Slice 98: optional completed guest-work observation

Date: 2026-10-09; baseline `80bc985`, branch `slice98-completed-work`.

Objective: independently hand-count dynamic paths in native/interpreter
execution, without changing time, timers, scheduling or compatibility.
No instruction-to-cycle conversion is inferred from the slice-97 samples.

Implementation contract: optional external GuestWorkCounter attached to
GuestState, excluded from RegisterContext and checkpoints. Successful effects
and actually executed slots count once; accepted syscall words count once
at the service owner. Unsupported/trapping/unhandled/repeated stops and
nullified slots do not count. Unknown native targets defer completion to
the bridge. Overflow is an explicit fatal observation error, never wrapping;
the guest effect has already happened, so this is not a resumable boundary.
Counter lifetime is the attaching caller's responsibility. Segmented runs
using the same live state/counter keep their total, including applying a
snapshot onto that object. A new boot invocation loading a checkpoint begins
a new observation interval, not historical replayed work.

Status: implemented, independently reviewed and verified. Full-state and
dynamic-work comparison agree at 10k/90k services; 26 hand-counted paths and
isolation tests pass. Production clock remains 1 ms/service; no menu progress
claimed. Decision 0040 accepted. Known likely-slot service gap retained
explicitly, not confused with the tested ordinary/boot path parity.

## Implemented observation points

- GuestState attaches a host-owned GuestWorkCounter by pointer, null by
  default. completed_instructions includes accepted syscall words;
  accepted_services counts that subset. Recording is after completion;
  no callbacks/event injection are permitted through this observer.
- Interpreter::step records at its seven successful-return sites, never in
  the shared plain-effect executor. Native inline and checked-fallback
  effects record after success; fallback exceptions return before recording.
- Emitter records each branch/call/jump/return separately from its slot.
  A later unsupported/trapping slot or callee leaves the completed transfer
  counted. Known indirect target capture/link and slot are counted once;
  an unapplied unknown-target stop counts zero until the bridge executes it.
  Label-targeted slots are recorded through the copy that actually runs.
- ERET records after PC/CP0 apply, not at a stopping boundary. Native store
  records before DMA poll, so native unwinding cannot erase/repeat it.
- Driver::handle_syscall records after non-Unhandled acceptance and before
  the unchanged advance_time call. run_reference separately records its
  Handled, Switched/Jumped and NoRunnableThread outcomes. Service 0x100 is
  accepted work like other service words, not idle or ERET. Restoring a
  context or scheduling the next handler is not another guest instruction.
- `gt4boot --count-work` prints the current observation interval and, with
  --compare-interpreter, independently requires both totals to match in
  addition to existing semantic-state equality. Flag off preserves output.
  Resume without compare observes the new leg; same live-state autosave
  segmentation keeps its counter. Refuse work-enabled verify-resume or
  resume+compare: restored and fresh totals cover different intervals, and
  diagnostic work is deliberately absent from checkpoints.

No ModelCompatibility or serialized-layout changes: instrumentation is
host-side opt-in observation, not translated guest semantics or time. Future
use as a clock cannot reuse this justification to skip identity/residue work.

## Reviewed read-only audit and known semantic gap

Scout mapped all emitter/step/acceptance paths and independently calculated
the existing eleven counts. Parent checked the seven successful step sites,
post-store/pre-poll placement, checked-fallback early return and service
acceptance before restoring-context counters. No counting in shared effects.

**Confirmed in source and executed synthetic fixture:** a taken likely
branch with a handled syscall in its slot is asymmetric **before slice 98**.
The native module stops at the slot without an interpreter pending transfer,
so Driver accepts the service and resumes at slot+4. Interpreter/reference
retain pending_transfer and do not accept a slot syscall. Original tests
covered only the unhandled slot. Observation makes the gap explicit rather
than silently asserting universal parity: native total 5/service subset 1
to landing; interpreter total 1/subset 0 stopped at the syscall. No fix or
clock policy is bundled here. A reference-backed branch/slot delivery and
trap-resume contract is the next audit, not guessed behavior.

Scout also confirms ordinary not-taken branches leave pending_transfer
clear; current service-clock trace did not prove a new runtime failure.
Any future instruction-work clock must audit both taken and not-taken slots.

## Independent hand counts / existing fixture extension

Original synthetic words only: `tests/data/synth-module-exits.txt` and its
matching array in `tests/unit/ee_module_exit_test.cpp`, verified identical
by a separate Python word-table extraction. No game payload in git. Count
paths with exact words/addresses, not generated static instruction metadata.

| Dynamic path | Native direct | Full native/bridge and interpreter | Decomposition |
| --- | ---: | ---: | --- |
| JR with slot rewriting RA | 2 | 3 | JR + slot + bridge tail |
| ERET EXL / ERL | 1 | 2 | ERET + bridge tail |
| BREAK / unhandled syscall | 0 | 0 | observation of stop only |
| accepted plain syscall | 0 before acceptance | 2 | accepted syscall + tail |
| known computed JR | 5 | 5 | JR + slot + leaf effect + JR + slot |
| unknown computed JR | 0 | 3 | bridged transfer + slot + target effect |
| likely taken syscall slot, unhandled | 1 | 1 | branch only |
| likely nullified syscall slot | 4 | 4 | branch + fallthrough + JR + slot |
| internal dispatch trap | 2 | 2 | transfer + slot; no BREAK |
| 3-pass loop 0x00100080 | 12 | 12 | init + 3*(decrement/branch/slot) + JR/slot |
| direct JAL / known JALR | 6 | 6 | call/slot + leaf effect/JR/slot + continuation |
| direct callee trap | 2 | 2 | call + slot only |
| unknown JALR | 0 | 3 | bridged call/slot + target effect |
| J + slot + target effect | 3 | 3 | BREAK excluded |
| trapping ADD | 0 | 0 | failed checked effect |
| successful checked ADD + return | 3 | 3 | fallback + JR + slot |
| JR with trapping ADD slot | 1 | 1 | completed transfer retained |
| branch to later standalone JR slot | 3 | 3 | branch + its slot + standalone slot |
| same graph via J/JR inline slots | 6 | 6 | branch/slot + J/slot + JR/slot |
| JR with unsupported slot | 1 | 1 | completed transfer retained |
| ordinary likely slot, taken/not taken | 4 | 4 | actual slot or fallthrough only |
| JALR rd==rs captured before link | 5 | 5 | call/slot + leaf effect/JR/slot |

26 concrete counted paths (eret and likely cases each have two legs), each
also compared with disabled observation for unchanged effects/boundary,
and repeated native/interpreter stops for zero extra work. Accepted plain
syscall has an independently written interpreter service owner, unlike the
old effect-only fixture that covered only Driver's handled path.

Additional cases: all four accepted outcomes versus Unhandled; zero service
budget then same-state resumed acceptance; post-store DMA poll reads counted
store before unwinding, bridge continuation contributes once; full u64 limit
and explicit overflow with no partially advanced diagnostic counters;
register restore and snapshot apply cannot rewind/detach observation;
snapshot bytes are identical regardless of diagnostic count, fresh counter
after load starts at zero. No existing state assertion was weakened.

Parent gate audit: CTest's existing PASS_REGULAR_EXPRESSION can override a
plain nonzero exit, so work mismatch must also join the existing differential
FAIL_REGULAR_EXPRESSION tripwires. Final gate preserves all current phrases
and the 90k acceptance expression and adds `guest work differs`.

Read-only implementation review approved all 18 textual native emission
sites, seven interpreter success sites and service-owner boundaries; reviewed
87 matching synthetic words (extent 0x0010015C) and the 26 hand counts.
Nonblocking corrections: distinguish fresh checkpoint invocation from live
snapshot apply (documented above); counter should be declared before its
attached boot state so it literally outlives attachment (reorder completed).
Production inputs: CORE reverified SHA256
`85d26aa8430154967b2633eede929286694ac39e99762527edcec365fd642ff9`,
2,020,861 bytes, against usa-v2.00.json. ISO reverified by streaming SHA256
`67b6c0075837f3ae1132d608acf2858bf13b2dd62d6eae83dff76df02e4e824f`,
5,314,478,080 bytes, same manifest. Verification never rewrites the manifest.

## Confirmed verification results

Final serialized VS 2022 x64 build (Developer PowerShell 17.14.34), explicit
gt4boot build, CTest 4.3.3, then Python 3.14: warning-free build, **53/53
CTest (54.29 s)** and **84 Python tests, 6 skips (78 run, 68.851 s)**,
existing socket ResourceWarnings only. Initial full run before declaration
reorder/tripwire reinforcement was also green (53/53, 56.53 s; Python 81,
63.998 s); final result, not that preliminary run, is the acceptance evidence.
Three added Python CLI tests verify flag-on/off output isolation and both
incomparable-interval rejection paths with exact diagnostic/exit code 2.

Real pinned disc boot at unchanged **90,000 services**, now with --count-work
in the existing gate:

```text
stats: module calls 252249, interpreted steps 5886216, services handled 90000
guest work: 22566319 completed instructions, 90000 accepted services
guest work identical: 22566319 completed instructions, 90000 accepted services
interpreter: 22566320 instructions, state identical (registers, RAM regions, kernel, device banks)
```

The old interpreter number counts step **attempts**, including the final
unaccepted stop; completed work excludes that last attempt. This explains
the one-count difference here, not a universal identity for every trap/run.
The native module calls/bridge attempts remain unchanged statistics, not
completed work. The gate still requires its original 90k success phrase
and all existing failure phrases; work failure now explicitly trips it too.
Inspected generated `build/CTestTestfile.cmake` for the active phrase.

Fresh **10,000-service** counted and plain runs each used --quiet --threads
--compare-interpreter and the pinned disc. Both: 31,595 module calls,
797,092 bridge attempts, 8,946,332 interpreter attempts. Counted work:
**8,946,331 completed / 10,000 accepted services**, identical across engines.
After removing exactly the two `guest work` lines, **entire stdout matches**
the unobserved run; both stderr empty, exit 0. Root READY/prio64 with saved
PC 0x005ADBC8, three threads, two RPC pairs, GIF payload bytes/sink 0 remain
the slice-96 frontier. No clock advance, root turn or menu progress gained.
Logs in approved temporary directory gt4-live-session:
`slice98-10k-counted.log`, `slice98-10k-plain.log`,
`slice98-final-python.log`; full gate output in build/Testing/Temporary.

Negative control: temporary `slice98-tripwire/CTestTestfile.cmake` runs a
successful echo containing **both** `services handled 90000` and
`guest work differs`. Same production PASS/FAIL properties cause CTest 4.3.3
to fail with exit **8**, naming the new failure regex. Thus a printed old
success phrase cannot mask a work mismatch. This is an isolated diagnostic
test configuration outside the repository, not a replacement test harness
or changed guest program. Official property reference:
`https://cmake.org/cmake/help/latest/prop_test/PASS_REGULAR_EXPRESSION.html`.

Synthetic fixture provenance: 87 words, base 0x00100000, exclusive extent
0x0010015C. LF-normalized spec SHA256
`9fb593d0002bb2749d48773b0e13534ba4b67274d5392a1ddcd92df0687116b4`;
348-byte little-endian synthetic image SHA256
`1022b533806ade30c15c221c286a17a96c6e09a5f1f6d0f7321a365be426ae59`.
Spec/table exact match reverified independently of generated C++. Whole
generated header still reports 15,068 functions / **924,991 static
instructions**, now 215,945,387 bytes (~216 MB) with readable observation
calls; static count is not runtime work. Derived payload remains ignored.

Representative reproduction (VS Developer PowerShell for build/CTest):

```powershell
cmake --build build
cmake --build build --target gt4boot
ctest --test-dir build --output-on-failure
private/tooling-venv/Scripts/python.exe -m unittest discover -s tests/python
build/gt4boot.exe private/fingerprint-check/CORE.GT4 --disc "Gran Turismo 4 (USA) (v2.00).iso" --services 10000 --quiet --threads --compare-interpreter --count-work
```

## Closure / next experiment

Decision 0040 adopted for **observation only**. Compatibility still time=3,
interrupt=4, kernel=2, RPC=1, translation=2; GT4CPT3/GT4KERN2 unchanged.
No diagnostic quantum/exclusion/forced scheduling exists; no reference or
temporary experiment is running. Counter is off unless attached/requested.

Next slice 99: reference-backed branch/slot event eligibility and syscall
trap-resume audit, including the executed likely-slot discrepancy, plus a
matched reference update-delay/root-work interval. Do not convert the new
counter into a clock before independently justified costs, precise native
event exits, serialized residues and explicit compatibility review exist.
