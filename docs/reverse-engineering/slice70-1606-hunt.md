# Slice 70: the service-1606 divergence hunt (2026-10-04)

Status: diagnosis complete, no code fix (cause is contract-scale).
The incident stays open: both `gt4boot_services` WILL_FAIL markers stay
until the delivery-granularity contract (candidate decision 0034) lands.
No post-1606 measurement can be trusted first (P07 telemetry and P09
comparisons still wait for a green differential).

## Claim

The first divergence between the driver and the interpreter in the
service-1606 window is an interrupt-DELIVERY timing difference, not a
translation bug:

- Guest code on both engines identically programs a VIF1 DMA chain
  (`sw` at 0x004abae0 sets STR, chcr 0x100001c5) and the P06 device model
  completes it synchronously and raises DMAC cause 1 on both sides.
- The interpreter delivers the queued cause before the next instruction
  (interrupted pc 0x004abae4, sp stays 0x6de6b0).
- The driver cannot deliver inside a translated call: the raise queues
  mid-module, `function_004aba50` runs to its epilogue
  (guest 0x004abafc `addiu sp,sp,+0x20`, the FIRST differing write:
  driver sp 0x6de6b0 -> 0x6de6d0, which the interpreter never performs),
  returns `Returned` with pc 0x004a1274, and only then injects.
- Stop state: boundary matches (syscall 0x00001604 service 0x100, the
  patch/interrupt return stub); exactly one register differs (r29:
  driver 0x6de6d0 vs interpreter 0x6de6b0, pcs both 0x1604); exactly
  three RAM words differ, all stack slots placed by the handler frame
  at the two different sps. All other registers, HI/LO, FPU, VU0, CP0
  and RAM are identical.

Confidence: Confirmed (bounded two-engine event trace, pairwise
value match pre-raise, 19/19 driver loop-top pcs subsequence of the
165 reference pre-raise steps, full-register rescan).

## Reproduction (inherited tree 45d2b71, honest rebuild, MSVC 19.44 x64)

- `--services 1605 --compare-interpreter` with disc: exit 0, boundary
  syscall 0x005adce4 service 0x44, 8197 module calls, 258622 interpreted
  steps, interpreter 7520925 instructions, state identical.
- `--services 1606 --compare-interpreter` with disc: exit 1, boundary
  syscall 0x00001604 service 0x100, 8200 module calls, 258680
  interpreted steps, `state differs at register 29`
  (driver gpr=0x6de6d0, interp gpr=0x6de6b0).
- Note: on this tree the signature is register 29 (the incident's
  "P06 tree reports the memory window first" line was a Hypothesis;
  the register check runs before the memory check, so a register
  difference always reports first when present).

## Method (transient, bounded, reverted)

- Checkpointed the driver at 1605 (`--checkpoint-at 1605`, 33,589,961
  bytes) and proved resume==direct for the window (`--verify-resume`
  + 1 service: `resume states identical (1606 services cumulative,
  digest 0xd9b8c60b72fd7e13)`). Hunt legs resume from the checkpoint,
  so each traced leg holds exactly the 1-service window
  (3 module calls, 59 bridge steps on the driver side).
- Temporary hooks (all reverted after, logs under ignored `build/hunt/`,
  deleted after extraction): sp-write logging in `write_gpr64` /
  `write_gpr_low32` / `restore_registers` (log-on-change, 50k cap);
  driver loop-top notes (call/exit/bridge/service outcome); kernel
  notes (wait-sema outcome, dmac raise, injection); DMA completion
  note; interpreter step/service notes gated to services >= 1605;
  stop-time report-all (every differing register plus first differing
  words). No RPC/clock/interrupt/mask behavior touched.
- The resumed-leg reference runs 1606 services from boot (same stop
  and state as the direct leg, per verify-resume); its trace opens at
  1605 handled, so its first ~20 lines are post-1605 tail (the WaitSema
  wrapper at 0x004a0f08 calling into the 0x44 stub = service 1606),
  not a divergence.

## Timelines (abridged, one line per engine event)

Driver (resumed leg, window only):
```
bridge 0x005adce4
wait-sema id 131 -> handled ; svc 0x44 -> 0 (Handled)
bridge 0x005adce8 ; bridge 0x005adcec      (jr ra; nop)
bridge 0x004a123c ; bridge 0x004a1240
call 0x004a0f68 ; (sp 6de6d0<->6de660 pair) ; exit 5 pc 0x004a1244
bridge 0x004a1244 ; bridge 0x004a1248
call 0x004a0f68 ; (second 0x70 pair) ; exit 5 pc 0x004a124c
bridge 0x004a124c ... 0x004a1270
call 0x004aba50
sp 6de6d0 -> 6de6b0                        (0x004aba50 prologue -0x20)
sp 6de6b0 -> 6de640 ; sp 6de640 -> 6de6b0  (nested 0x004a0f68 pair)
dma-complete cause 1 chcr 0x100001c5 ; raise-dmac ch 1
sp 6de6b0 -> 6de6d0                        (FIRST DIVERGENCE, epilogue)
exit 5 pc 0x004a1274                       (5 = Returned)
inject cause 1 interrupted 0x004a1274 handlers 1
restore pc=0x004a1274->0x004ab6d8 (sp unchanged 6de6d0)
bridge 0x004ab6d8 ... (handler, frame 6de6d0->6de6c0->6de6d0) ...
bridge 0x00001600 ; bridge 0x00001604      (stub; over-limit stop)
```

Interpreter (same window): identical pcs in order (the 19 driver
loop-tops are a subsequence of its 165 pre-raise steps, verified by
script), identical sp values pairwise, identical raise lines at the
same store, then:
```
ref-step 0x004abae0                        (the STR sw)
dma-complete cause 1 chcr 0x100001c5 ; raise-dmac ch 1
inject cause 1 interrupted 0x004abae4 handlers 1
restore pc=0x004abae4->0x004ab6d8 (sp unchanged 6de6b0)
(handler frame 6de6b0->6de6a0->6de6b0, same shape, lower base)
ref-step 0x00001600 ; ref-step 0x00001604
```

Stop diff (report-all rescan: no other register differs):
```
driver gpr=0x6de6d0 / interp gpr=0x6de6b0 (r29 only)
word 0x6de6a8: driver 0x4aba80 interp 0x1600
word 0x6de6c0: driver 0x0 interp 0x3
word 0x6de6c8: driver 0x1600 interp 0x4a1274
```

## Attribution and exonerations

- Translator: EXONERATED. Pre-raise guest pcs and sp values match
  pairwise (stale-pc attribution inside translated calls verified
  against `function_004aba50` emission: the callee's `jr ra` return
  sets pc to 0x004aba80 mid-call, so post-call writes log that pc).
  Exit reasons are `Returned` with the exact link pcs (0x004a1244,
  0x004a124c, 0x004a1274).
- Kernel services: EXONERATED. WaitSema id 131 takes the count>0
  `Handled` path on both sides (no block, no switch, no restore);
  `svc 0x44 -> 0` on both; injection logic identical given the
  pending queue (same cause, same handler 0x004ab6d8, 1 handler).
- Device content: EXONERATED. Same transfer, same completion, same
  raise on both sides (vif1 6th start, +424 bytes; thread/DMA census
  identical; dmac stat unchanged 0x00270027).
- Cause class: delivery granularity (driver unit = one module call
  or bridge step; interpreter unit = one instruction) crossed with
  synchronous DMA completion (decisions 0011/0029/0033, P06 keeps
  the at-once conclusion). The queued cause is consumed at different
  guest pcs. Kernel-internal state (sema counts, queues, banks) is
  NOT part of `states_match`; guest state matched at 1605 while the
  delivery point still diverged at 1606. Pre-existing, not P06
  (same shape on the clean tree per the incident record).

## Why no fix in this slice

Deferring the queue push, deferring completion, or polling harder
cannot align delivery: the two engines consume pending causes at
different guest pcs by construction (decision 0013 units). Aligning
them needs a delivery-granularity contract (emitter poll points, or
interpreter deferral to translated-region exits, or deterministic
async DMA delivery points) with re-verification of every downstream
claim. That is contract-scale (candidate decision 0034), explicitly
out of scope for the hunt. No masking, no invented traffic, no RPC /
clock / mask changes were made.

## Next experiment (slice 71)

Raise census over a long run: log every raise with inside-module vs
loop-top provenance and the resulting delivery pc on both sides, to
size the contract work (how many raises land mid-module, which
devices, whether handler effects always stay sp-local like here).
Then draft decision 0034 with measured options. Telemetry (P07) and
fresh comparisons (P09) stay gated behind the green differential.

## Build hygiene note

The correct toolchain for this tree is VS2022 BuildTools MSVC 19.44
x64 (`VsDevCmd.bat -arch=x64`): plain VsDevCmd defaults to x86
(link LNK4272 + missing CRT/startup symbols), and the VS18
VsDevCmd mixes a 19.44 compiler with 19.51 headers (STL1001
static_assert). The committed provenance string still names the
configure-time commit (abf7126); code freshness was verified by
ninja (`no work to do`) plus identical leg stats, not by that string.
