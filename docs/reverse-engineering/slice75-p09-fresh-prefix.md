# Slice 75: P09 fresh prefix and independent observation (decision: none — observation only)

Date: 2026-10-04. Baseline: main at 7bb4f14 (P08 widened comparator).
Plan item: P09 (PLAN.md section 6). Scope kept: fresh boot from the
entry, new checkpoints under the current semantics, small comparable
anchors, extended differential legs. No reply byte changed; DMA, JR,
the clock, interrupts, RPC answers untouched; no fabricated traffic;
no new instrumentation (existing `--threads` / `--dump` / checkpoint
flags only; all scratch logs under ignored `build/`).

## Run identity (PLAN 9.4 fields)

- run_id: slice75-20261004 (legs A-J below, one fresh process each).
- Model compatibility: time=3 interrupt=3 rpc=1 translation=2
  (`include/gt4recomp/ee_checkpoint.hpp:90-94`), format GT4CPT2.
- Inputs (verified, `scripts/gt4disc.py verify`: PASS):
  ISO 5,314,478,080 bytes sha256
  `67b6c0075837f3ae1132d608acf2858bf13b2dd62d6eae83dff76df02e4e824f`;
  CORE.GT4 2,020,861 bytes sha256
  `85d26aa8430154967b2633eede929286694ac39e99762527edcec365fd642ff9`
  (matches `docs/inputs/usa-v2.00.json`); entry 0x00100008.
- Config: MSVC 19.44.35228 x64 Debug, Ninja, CMake 4.3.3; service
  clock 1 ms/service, idle 1 frame/interrupt, budgets 2M idle /
  200M steps (default). Provenance baked into the binary names
  commit 837dc30: the binary was built from the pre-commit tree and
  `ninja: no work to do` after 7bb4f14 proves source identity; the
  one-commit lag is provenance staleness, not a semantic difference.
- No pre-v3 checkpoint used anywhere: every leg is a fresh boot
  (`leg == cumulative` on all stats lines); `build/ckpt-*.bin`
  (2026-10-03) stayed untouched and count as forensic per gate 0028.

## Anchor legs (all fresh, `--disc` pinned ISO, exit 0)

| Leg | Services | Boundary | Module calls | Bridge steps | Threads | RPC pairs / unknown |
|---|---|---|---|---|---|---|
| A 400 | 400 | syscall 0x005adb94 / 0x2F | 983 | 114,384 interp steps | 2 | 1 / 0 |
| B 3000 | 3000 | syscall 0x00001604 / 0x100 | 11,160 | 332,459 | 13 | 22 / 32 |
| C 20000 | 20000 | syscall 0x00001604 / 0x100 | 50,985 | 1,254,673 | 13 | 22 / 32 |
| E 200000 | 200000 | syscall 0x00001604 / 0x100 | 472,800 | 11,021,957 | 13 | 22 / 32 |
| F 300000 | 300000 | syscall 0x00001604 / 0x100 | 707,110 | 16,447,060 | 13 | 22 / 32 |
| H 1000000 | 1000000 | syscall 0x00001604 / 0x100 | 2,347,431 | 54,427,332 | 13 | 22 / 32 |

Module-call counts at 400/3000/20000/90000 reproduce the
slice-71/73 census legs exactly (983 / 11160 / 50985 / 215013):
same execution under the current semantics.

Anchors observed (engine = driver unless noted):

- Entry: every leg boots from 0x00100008 fresh; leg A stops in
  early init (thread 1 RUN at 0x005adb38, thread 2 WAIT 2/11 at
  0x005adce8).
- Handler init: leg A registers INTC cause 11 -> 0x005b8158
  (TIM2) and DMAC ch5 -> 0x005b0e30 (SIF). Leg B adds INTC
  2 -> 0x004ab430, 5 -> 0x004ab548, 0 -> 0x004ab668,
  2 -> 0x00551728 and DMAC ch0/1/2 -> 0x004ab6d8. Tables
  frozen from 3000 through 1M.
- Binds: leg A binds SIFMAN (0x80000001) only, 1 pair
  (fn 0xFF compat-constant, send/recv/result 8/8/8). Leg B binds
  21 SIF servers (string workers BKUP/ESUP/PUST/THUP/bsuP,
  PCDV + secondary channel, MPG1/MPG2, PBGM, PRTS, SPUP, SPUT,
  SMUP, VOIC, FILEIO, SIFMAN, 0x80000400, 0x80000592,
  0x80001300, 0x8000131c); bound-but-never-called SIDs keep
  lifecycle-without-traffic (slice-73 note stands).
- Verified file open: leg B FILEIO fn 0 x22, class
  implemented-verified (`open path at +8, reply {handle, size}
  from the ISO`), plus fn 0xFF version "3000" compat-constant.
- Time: TIM2 programmed MODE 0x0382 (CUE|CMPE|OVFE, CLKS/256),
  COMP 0, count advancing (0x3040 at 3000); T0/T1/T3 zero/mode 0
  through 1M. Virtual service time at 800k = 800 s model policy.
- First DMAs: leg B VIF0 1x [ref..end] 3 tags 2,712 B
  (madr 0x006de5d0, sink == bytes, tap hash 0xe44b9f8d);
  VIF1 26x ([cnt..end] x25 + [ref..end] x1) 87 tags 27,512 B;
  GIF 25x [next..end] 50 tags 0 B keepalive; SIF0 chcr 0x184
  service path; DMAC stat 0x00270027. VIF0 shape frozen after
  (still exactly 1 start at 1M); VIF1/GIF grow at ~1 frame per
  66 services (15,019/15,018 starts at 1M; 6,384,544 B VIF1
  total, sink == bytes every leg).

## New checkpoint + verify-resume: FIRST DIVERGENCE (checkpoint fidelity)

- New checkpoint: `--services 3000 --checkpoint-at 3000
  build/slice75-ckpt-3000.bin` (33,606,280 bytes, GT4CPT2,
  model 3/3/1/2, ignored dir, exit 0).
- `--verify-resume` (+400 services) exits 1:
  `state differs at kernel IOP image: left "" , right
  "rom0:UDNL cdrom0:\IOPRP300.IMG;1"`.
  Call order (`tools/gt4boot/main.cpp:1026`) is
  (resumed, direct), so left = resumed (image lost), right =
  fresh (image recorded by `answer_sif_reset`,
  `src/ee/kernel.cpp:3199-3224`).
- Mechanism (code-level, exact): the blob codec saves the image
  (`kernel.cpp:2198-2202`) and parses it into local
  `sif_iop_image` (`kernel.cpp:2382-2384`), but the restore
  block writes `sif_iop_image_ = std::move(sif_iop_image_)`
  (`kernel.cpp:2460`) — a self-move of the member instead of
  the parsed local, which empties it (observed ""). Every other
  line of that block moves a named local (deferred_calls,
  sif_software_registers, interrupt_queue, ...); line 2460 is
  the lone member-both-sides line.
- Discriminant: the CTest 400+400 verify stays green because at
  400 services the SIF reset has not run yet (image "" on both
  sides, self-move invisible); at 3000 the reset has recorded
  the path and the drop becomes visible. Same procedure, two
  anchors, green-then-red at exactly one named field.
- Status: observation only, no fix shipped (P09 observes; the
  fix belongs to a P00-follow-up slice with its own
  regression). No causal claim beyond the codec: the game's
  behavior is unaffected (fresh-boot legs never read the field
  back from a checkpoint).

## Extended differential (model vs internal reference, P08 lens)

| Leg | Services | Driver | Interpreter | Verdict |
|---|---|---|---|---|
| D 90k compare | 90,000 | 215,013 calls / 5,052,977 steps | 26,812,702 instr | identical (4/4 domains), exit 0 |
| G 300k compare | 300,000 | 707,110 calls / 16,447,060 steps | 72,779,809 instr | identical (4/4 domains), exit 0, 34 s |
| J 800k compare | 800,000 | 1,878,764 calls / 43,575,756 steps | 182,221,543 instr | identical (4/4 domains), exit 0, 86 s |
| I 1M compare | 1,000,000 | 2,347,431 calls / 54,427,332 steps | >200M (step-limit at 0x0057f23c) | budget stop, exit 1 |

- No semantic divergence through 800,000 services (8.9x the
  pinned 90k): boundary, all registers, all RAM regions
  (incl. scratchpad), full kernel tables and all 19 device
  banks identical on both engines.
- The 1M stop is a harness budget ceiling, not a divergence:
  the reference hardcodes `default_step_limit` (200M,
  `main.cpp:1435-1437`, `--steps` does not reach it); per-leg
  density (~243 instr/service at 300k, ~228 at 800k)
  projects ~228M at 1M > 200M. Reaching 1M needs a slice that
  wires the flag through (or raises the default) — recorded as
  next-harness work, not done here.

## Stationary-phase facts (no new guest request through 1M)

- RPC pair set frozen at 22 pairs / 32 unknown calls from
  3,000 through 1,000,000 services. The slice-14 polling
  traffic (PCDV 1, PBGM 8, SPUP 4, LGDEV 6, ...) never starts
  under the current semantics: no naturally woken thread (all
  waits identical: 1: 2/63, 2: 2/11, 9/13: 1/0; no 0x4 -> live
  transition), `deferred calls: 1, pending interrupts: 0` on
  every leg, DMAC stat pinned at 0x00270027.
- Strict-mode first-unknown still (0x80000592, fn 0) per
  slice 73; default legs answer it silently and continue.
- Consequence for P10: the predicated producer the main waits
  on has not been identified yet — the boot has not asked for
  new data by 1M services, so there is no causal chain to
  implement against. The next observation leg needs either a
  bigger budget (1M+ compare after the harness fix) or a
  longer driver-only march to find the polling onset.

## Gates

- Build: `ninja: no work to do` (VsDevCmd `-arch=x64`,
  MSVC 19.44.35228).
- CTest 53/53 (incl. pinned 90k disc differential,
  checkpoint/resume/autosave chains).
- Python: 73 run, OK, 6 skips (2 known socket
  ResourceWarnings).
- Not committed, not pushed, no branches (per orders).
  Scratch logs + one checkpoint under ignored `build/`
  (`slice75-*.log`, `slice75-ckpt-3000.bin`); no payload in git.
