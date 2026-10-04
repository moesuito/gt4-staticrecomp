# Slice 74: P08 widened comparator (decision 0036)

Date: 2026-10-04. Baseline: main at 837dc30 (P07 RPC telemetry).
Scope kept: comparison only. No reply byte changed; DMA, JR, the
service clock and interrupt delivery untouched; no fabricated
traffic; no MMIO guest reads used for snapshots.

## What was blind

`states_match` in `tools/gt4boot/main.cpp` compared the live EE
registers (GPR/GPR-high/FPR/CP0, VU0 files, HI/LO, FPU, shift cache,
clip/mac/status, pc) plus one FNV-1a digest looped over `[0,
0x2000000)`. Outside that digest: the scratchpad region
(0x70000000), the GS block region (0x12000000), every kernel table
and every device bank. The kernel blob and the bank sections existed
only for checkpoints, never for the differential.

## The widened design

New module `include/gt4recomp/ee_compare.hpp` + `src/ee/compare.cpp`
in `gt4recomp_decode`, wired into `gt4boot`'s two comparing legs
(`--compare-interpreter` and `--verify-resume`):

- `compare_contexts`: every RegisterContext field with both values
  (`registers gpr[29]: left ..., right ...`).
- `compare_memory_regions`: regions matched by base, then size, then
  first divergent byte address
  (`memory region base 0x70000000 byte at 0x70000040: ...`).
- `Kernel::describe_kernel_difference` (`src/ee/kernel.cpp`): threads
  by id (all fields plus the saved context), semaphores by id,
  id counters, the running thread, syscall patches, OSD/GS IMR, the
  deferred stack in order, handler tables in order, the pending queue
  in order, SIF software registers/servers, IOP image, reboot and
  idle flags, the service-clock accumulator and remainders, disc
  handles/paths, block-cache entries with cursors, the originating
  flag. Host pointers, RPC telemetry and the strict flag excluded
  with a comment at the declaration.
- `compare_bank_sections`: banks matched by name (labels live in
  `gt4boot`'s `bank_names`, 19 entries in `snapshot_banks` order),
  entries matched by address
  (`device bank "timer" register 0x10001020: ...`).
- `compare_full_states`: guests, then kernel, then banks. First
  difference wins, printed as `state differs at ...`.

`memory_digest` stays only as the informational resume-log hash;
the gate now rests on the field comparison. Old wrapper lines kept,
so the CTest `FAIL_REGULAR_EXPRESSION
"states differ;but the interpreter stopped at"` still bites.

## Fixtures (new `ee_compare` CTest, `tests/unit/ee_compare_test.cpp`)

Control legs (identical machines equal at guest, kernel and full
level, including after identical SetupThread+CreateSema), then:

- scratchpad-only byte flip: old main-RAM digest equal (blindness
  proved), new diagnosis names `0x70000000`/`0x70000040`;
- main-RAM flip names `0x00000100`; missing region names the count;
- GPR flip names `gpr[29]`; direct `compare_contexts` legs for pc,
  `cp0[12]`, `vu0_vf[3][1]`;
- semaphore signal on one side (states re-synced): names
  `semaphore 3 count`, at kernel and full level;
- wakeup on one side: names `thread 2 wakeup count`;
- different handler argument: names `argument` with equal states;
- one-sided `raise_interrupt`: names `interrupt queue`;
- one-sided `advance_service_time`: names `service clock accumulator`;
- different PRTS copy-out sizes: names `cursor`;
- timer COMP flip: names `timer`/`0x10001020` at section and full
  level with equal states;
- reversed bank insertion order: equal (incidental order excluded).

## Evidence

- Build: full rebuild warning-free, MSVC 19.44.35228.0 x64 Debug,
  Ninja, VsDevCmd `-arch=x64` chained.
- CTest: 53/53 (52 + new `ee_compare`), including
  `gt4boot_services` 90k with disc + compare-interpreter.
- Live 90k leg: boundary syscall 0x00001604 service 0x100;
  215,013 module calls (identical to the pre-P08 census count);
  5,052,977 bridge steps; 90,000 services; interpreter 26,812,702
  instructions; `state identical (registers, RAM regions, kernel,
  device banks)`.
- Python: 73 tests, 6 skips, OK (the 2 known socket
  ResourceWarnings).
- Added-lines ASCII scan: 0 non-ASCII.
- Not committed, not pushed, no branches (per orders).

## Limits carried forward

- `describe_kernel_difference` must be extended with every new
  kernel member, next to the blob codec; the two lists can drift
  silently (unit legs cover the categories, not each field).
- SIF-server divergence has no live fixture yet (needs RPC
  traffic); the map-by-sid compare is covered by construction.
- `memory_digest` still hashes main RAM only for the resume log
  line; it is informational, not a gate.
