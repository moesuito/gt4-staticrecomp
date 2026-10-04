# Slice 72: option-A prototype — emitter poll points at DMA STR writes (2026-10-04)

Status: implemented, all three acceptances green, decision 0034 accepted,
WILL_FAIL markers removed. The transient recensus instrumentation was
reverted (`git checkout` of `src/ee/kernel.cpp`, `src/ee/driver.cpp`,
`include/gt4recomp/ee_kernel.hpp`; manual hunk revert in
`tools/gt4boot/main.cpp`); its TSV logs and pairing script lived under
ignored `build/` and were deleted after extraction. What survives is the
permanent change set below, this document, the accepted decision 0034
and the journal entry.

## Claim

Translated code now checks the pending queue after a store that starts
a DMA transfer, so a synchronous completion delivers inside the module
at the same guest pc the interpreter uses. All three closing gates for
incident 1606 are green:

- **1606 differential green** (r29 identical, full state identical).
- **90k differential green** (census leg volume, both engines, full
  state identical).
- **Fresh raise census: zero in-module raise / divergent-delivery
  pairs** (12,227 raises with identical sequences, 12,224 deliveries
  with zero field mismatches).

Confidence: Confirmed (dual-engine differentials + line-by-line
delivery pairing over 90,000 services; census leg reproduces slice 71's
class counts exactly).

## Permanent change set

Poll point design (option A of decision 0034, scoped to DMA-window STR
writes):

- `tools/gt4translate/main.cpp` (~line 718): after every
  falling-through `sw` the emitter appends
  `if (state.poll_dma_start(effective_address, value, address + 4))
  { return ee::BoundaryKind::Returned; }`.
  The early `Returned` unwinds through the existing caller propagation
  (the `state.pc() != address + 8` check), so the driver loop-top
  delivers the pending completion at the next guest instruction. No
  driver change was needed.
- `include/gt4recomp/ee_state.hpp` (lines 171-189, 278) and
  `src/ee/state.cpp` (lines 597-627): `GuestState::poll_dma_start`
  with a `DmaStartPoll` hook. Null hook (unit tests, and every state
  that never wires one) answers false with the pc untouched, so
  behavior there is bit-identical. Otherwise it folds the address to
  physical only when the segment alias is on (the same condition
  `GuestMemory::physical_address` uses), requires an exact CHCR match
  (VIF0 `0x10008000`, VIF1 `0x10009000`, GIF `0x1000A000`) with the STR
  bit (`0x100`) set, then sets the pc to the next guest instruction
  and invokes the hook.
- `tools/gt4boot/main.cpp` (line 1034): the driver wires
  `Kernel::start_interrupt` as the hook. A masked, gated or
  handler-less completion refuses there exactly as before, and the
  translated code simply continues; only an eligible completion
  unwinds. The reference engine never runs translated code, so it
  needs no hook. The interpreter, the devices, the RPC layer, the
  clocks and the masks are untouched.
- `include/gt4recomp/ee_checkpoint.hpp`: `translation_model` 1 -> 2
  (emitter semantics changed; old photos refuse loudly per the
  compatibility contract).
- `tests/unit/ee_state_test.cpp` (lines 151-199): poll unit coverage
  (no hook, VIF0/VIF1/GIF CHCR with STR, CHCR without STR, QWC offset,
  plain RAM, refusing hook, KSEG0 mirror with the alias on).
- `CMakeLists.txt`: both `gt4boot_services` WILL_FAIL markers removed
  (decision 0034 allowed removal only by the green runs below).

Deliberate exclusions (no evidence of need, census keeps them
checked): `swl`/`swr` (no observed STR-by-merge in 90k services),
stores in delay slots (the census raise pc `0x004abae0` falls
through), and the `execute_plain_effect` fallback (no memory-store
form can reach a device window: `sq`/wide writes throw on MMIO).
Coverage obligation: any future guest DMA programmer outside plain
falling-through `sw` reopens this scope; the census method below is
the check.

Generated-code footprint: 46,663 poll sites in the whole-program
header, including the incident store's own poll
(`effective_address(state, 18, 0)`, value `read_gpr32(2)`,
next `0x004abae4`).

## Acceptance legs (inherited tree e819576, honest rebuild, MSVC 19.44 x64)

All legs with disc, `--quiet`:

- **1605** (sanity): exit 0, boundary syscall `0x005adce4` service
  `0x44`, 8197 module calls, 258692 bridge steps, interpreter
  7520925 instructions, state identical.
- **1606** (accept 1): exit 0, boundary syscall `0x00001604` service
  `0x100`, 8200 module calls, 258750 bridge steps (70 more than the
  pre-fix 258680: poll early-returns run the remainder through the
  bridge), interpreter 7521133 instructions, state identical
  (registers, HI/LO, FPU, VU0, CP0, pc, memory digest) — the old
  `state differs at register 29` is gone.
- **90000** (accept 2): exit 0, boundary `0x00001604`/`0x100`,
  215013 module calls (**exactly** slice 71 leg C's count),
  5052977 bridge steps (18676 more than leg C's 5034301, the same
  bridge-remainder effect), interpreter 26812702 instructions, state
  identical in full.

## Recensus (accept 3, transient, bounded, reverted)

Env-gated TSV logging (`GT4_CENSUS72`, unset means a null check per
hook and unchanged behavior): `queue_interrupt` /
`queue_dmac_completion` log engine, kind, number, sequence,
in-translated / in-bridge flags and the enclosing module entry;
`inject_interrupt` logs engine, kind, number, interrupted pc,
handler, sp, handler count and thread. Driver context flags are set
around `module_.call_entry` and the bridge step; the reference engine
only interprets, so its flags stay clear. Unbuffered writes. The leg
itself stayed green (state identical), proving the hooks neutral.

90k-service compare leg, paired line by line with a transient script:

- Raises: 12,227 on each engine, sequences identical on
  (kind, number, seq, coalesced) — the exact slice-71 total.
- Driver classes: D ch0 x1 + ch1 x1334 + ch2 x1333 in-module, all with
  enclosing entry `004aba50`; D ch5 x89, INTC 2 x5327, INTC 11 x4143
  loop-top. Zero bridge-step raises.
- Deliveries: 12,224 on each engine, paired in order per
  (kind, number): **zero field mismatches** (interrupted pc, handler,
  sp, handler count, thread).
- The in-module class delivers at `0x004abae4` with handler
  `0x004ab6d8` on **both** engines (ch0: 1, ch1: 1334 over the same 2
  matched sps, ch2: 1333). Zero in-module raise /
  divergent-delivery pairs.

## What was NOT done (scope guard)

No device/RPC/clock/mask semantic change; no reference weakening (the
interpreter still delivers per instruction); no poll points outside
falling-through `sw`; no fabricated traffic; no generic success.
P07 telemetry and P09 comparisons resume behind the now-green
differential. The coverage obligation above stays open by design.
