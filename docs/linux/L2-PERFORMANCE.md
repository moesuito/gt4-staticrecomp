# Linux performance: the release VU path (TASK-074, plan step L2)

Status: **the Linux build runs the release profile's VU path** (2026-11-20): VU1 and VU0 microprograms compiled ahead of
time, the VU1 worker thread (MTVU) and the TASK-134 fast headers, all produced on Linux by the project's own tools and
identical to the 1.3.0-rc14 pins. Owner report before this step, from a demanding scene: mean VBlank 17.75 ms,
2,201 late frames of 2,267, 853 audio underruns.

## Inputs, all reproduced on Linux and compared with the Windows pins

| Input | Command | Result | Label |
|---|---|---|---|
| Runtime capsule | `prepare_perf_runtime.py <out> --mtvu` | the 7 derived sources (system, vif, vu_micro, gif, memory, hw, dmac) **byte-identical** to `slice172`/`slice178-main-runtime.json`; only the receipt differs (paths) | Confirmed |
| VU1 image | `vu1_image.py build --elf <elf> --pinned-tail --out <dir>` (new option, below) | equals the `slice155-vu1-image.json` pin (`9e5bc860...`) | Confirmed |
| VU1 entries | `vu1_image.py entries --policy all` | 2,048 entries, `284330f8...` as pinned | Confirmed |
| VU1 table | `gt4_vu_recomp --programs-per-file 16 --entry-policy all --image <image> --entries <list> <out>` | 2,048 programs, 0 refused; `vu1_generated.cpp` **equals the slice408 (rc14) pin** `3eed39d6...`; the manifest equals the slice395-408 pins | Confirmed |
| VU0 image and entries | `vu0_image.py build`, `entries` | image `2d7b2653...` as pinned; 14 VCALLMS entries | Confirmed |
| VU0 table | `gt4_vu_recomp --unit 0 --namespace gt4::vugen::vu0 --entry-policy vcallms --image <image> --entries <list> <out>` | 14 programs; `vu0_generated.cpp` and its manifest **equal the slice408 pins** (`951de107...`) | Confirmed |
| Fast headers | `prepare_fast_headers.py` (from CMake, `GT4_FAST_HEADERS=ON`) | derived from the CRLF donor checkout without change | Confirmed |

The older `slice1210-pcsx2.json` still names a slice160 VU0 table; the Linux output matches the later rc pins instead.

## Results

| Check | Result | Label |
|---|---|---|
| CTest, Linux tree with the AOT runtime | **48/48**: adds `vu_recomp_core`, `vu0_emitter`, `native_vu_fast_ops`, `vu_recomp_differential` (907 checks, 0 failures), `vu1_aot_registry`, `vu0_aot_registry` (87 checks), `vu0_aot_differential` (28,000 seeded states, 28,911 checks, 0 failures) | Confirmed |
| Game output, AOT build against the interpreter build (headless Vulkan, 1,200 frames) | **12 of 12 frames byte-identical**, the same 5,898,359,180 guest cycles; the VU1 worker ran 40,447 jobs | Confirmed |
| Intro movie in a window with sound, 2,500 frames from a save state, interpreter build | 2,478 of 2,499 intervals in 16-17 ms, 10 stalls of 25-35+ ms, 21 late frames, 18 underruns | Confirmed |
| The same, AOT build | **all 2,499 intervals in 16-17 ms, 0 late frames, 0 underruns**, audio queue median 2,879 for a 2,880 target | Confirmed |
| A tester playing the AOT build in a window | plays smoothly; the flicker seen with the interpreter build is gone (no F8 report taken) | Confirmed (tester report) |

## How to build it

After the [L1](L1-VULKAN.md#how-to-reproduce) and [L2 window](L2-WINDOW.md#how-to-run-it) steps:

```sh
P=private/tooling-venv/bin/python; E=private/<producer>/SCUS_973.28.elf
$P scripts/prepare_perf_runtime.py private/<capsule> --mtvu
$P scripts/vu1_image.py build --elf $E --pinned-tail --out private/<vu1-image>
$P scripts/vu1_image.py entries --image private/<vu1-image>/vu1_image.bin --policy all --out private/<vu1-image>/entries-all.txt
$P scripts/vu0_image.py build --elf $E --out private/<vu0-image>
$P scripts/vu0_image.py entries --image private/<vu0-image>/vu0_image.bin --out private/<vu0-image>/entries.txt
cmake --build <build dir> --target gt4_vu_recomp      # the recompiler builds whenever GT4_DONOR is set
<build dir>/gt4_vu_recomp --programs-per-file 16 --entry-policy all --image private/<vu1-image>/vu1_image.bin \
  --entries private/<vu1-image>/entries-all.txt private/<vu1-gen>
<build dir>/gt4_vu_recomp --unit 0 --namespace gt4::vugen::vu0 --entry-policy vcallms \
  --image private/<vu0-image>/vu0_image.bin --entries private/<vu0-image>/entries.txt private/<vu0-gen>
cmake -S . -B <build dir> ... -DGT4_LINUX_RUNTIME_CAPSULE=$PWD/private/<capsule> \
  -DGT4_LINUX_VU1_GENERATED_DIR=$PWD/private/<vu1-gen> -DGT4_LINUX_VU0_GENERATED_DIR=$PWD/private/<vu0-gen> \
  -DGT4_LINUX_VU0_IMAGE=$PWD/private/<vu0-image>/vu0_image.bin
```

CMake refuses a capsule whose sources differ from `slice178-main-runtime.json` and tables that differ from
`slice408-pcsx2.json`. `GT4_MTVU=0` turns the worker off at run time (as on Windows).

## Changes

- `scripts/vu1_image.py build --pinned-tail`: builds the image without the recorded snapshots; the 16 tail bytes come
  from `slice155-vu1-image.json` (which 86 snapshots controlled), and the decrypted blob and the whole image must equal
  their pins, else nothing is written. `--corpus` is unchanged; one of the two is required. Test in
  `test_vu1_image.py`.
- `cmake/LinuxHost.cmake`: `gt4_vu_recomp` and its controls whenever `GT4_DONOR` is set; the optional AOT runtime
  (capsule sources replacing the donor's, the generated tables, `mtvu_bridge.cpp`, `gt4_mtvu_runtime`, the perf
  accounting defines as in `native/CMakeLists.txt`) and its four differential tests; fast headers on by default.
- **A portability fix applied at build time, not in the pinned capsule.** The capsule `vu_micro.cpp` (from
  `scripts/vu1_aot_overlay.inc` and `vu0_aot_overlay.inc`) has two lambdas returning both `std::strtoull` (`unsigned
  long long`) and `std::uint64_t`. With MSVC these are one type; on Linux `std::uint64_t` is `unsigned long`, so clang
  and GCC refuse the lambda. CMake checks the pinned file, then compiles a copy in which exactly those two returns are
  cast to `std::uint64_t` (refusing unless it finds exactly two). The lasting fix is the same cast in the two overlays,
  which changes the capsule text and so needs new Windows pins: an owner decision.

## Not yet on Linux

- The rest of the release runtime profile: the frame-cost capsule (frame skip, TASK-082/084), the IPU fast decoder
  (`slice365-ipu-fast-runtime`), the SPU2, USB, memory-card and license-guard capsules.
- The EE game library with fast headers and PGO/ThinLTO, as the release builds it; the Linux library is plain `-O3`.
- CoreGS 2.0 (upscaling and anti-aliasing; the CoreGS 1.1 backend renders at 1x with nearest scaling). The flicker
  went away with the AOT build, which points at late frames rather than the backend (hypothesis).
