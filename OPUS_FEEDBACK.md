# External Technical Review & Strategic Roadmap (`OPUS_FEEDBACK.md`)

**Date:** 2026-10-04  
**Reviewed commit:** `75ea3a1` (*Tripwire re-check 01 SAME, work stopped for external review (50/50 CTest)*)  
**Scope:** Read-only audit of the codebase, git history through slice 62, reverse-engineering corpus (`docs/reverse-engineering/`, `docs/decisions/`, `docs/lessons/`), reconstructed EE ELF (`private/reconstructed/SCUS_973.28.elf`), disc image (`Gran Turismo 4 (USA) (v2.00).iso`, `/IRX/*.IRX`, `IOPRP300.IMG`), and checkpoint snapshots (`build/ckpt-*.bin`). No source files, tests, or git history were modified.

---

## Executive Summary

1. **What is exceptional:**  
   The R5900 static recompiler pipeline (M0–M29) and its differential verification harness are world-class. Decoding 349 operations across 1,334,917 words (with only 30 real-code unsupported words), translating 15,068 functions (924,991 instructions) into a single warning-free C++20 module, and proving bit-for-bit state equivalence against a reference interpreter across millions of instructions is a rare engineering achievement. The checkpoint/resume infrastructure (`GT4CPT1`) is clean, fast, and deterministic.

2. **Why forward progress stalled after M30 Slice 46/47:**  
   Between M30 Slice 47 and Slice 62 (including M32 Slices 1–34, M33 Slices 23–28, M35 Slice 35, and 22 lesson slices), the project entered an observational loop ("watch-standing with tripwires armed"). The operating hypothesis became that the parked machine at 243.7M services is waiting for an unknown "originating async IOP event" that cannot be deduced without fabrication.
   **That hypothesis is falsified by static and checkpoint evidence already on disk.** The stall is caused by **specific, verifiable bugs in the host model** (DMAC interrupt routing, `current_thread_id_` across idle blocks, `AddIntcHandler2`/`AddDmacHandler2` argument passing, silent zero-filled RPC replies, and misidentified IOP servers).

3. **Five Concrete Breakthroughs (All Confirmed with Addresses & Byte Offsets Below):**
   - **Breakthrough #1 — VIF0/VIF1/GIF DMA completions are routed to INTC instead of DMAC (`tools/gt4boot/main.cpp:60-64, 843-846`):** `BootDevices` wires `vif0_dma`, `vif1_dma`, and `gif_dma` to `kernel.raise_interrupt(4 / 5 / 9)` (`InterruptRequest::Kind::Intc`) instead of `kernel.queue_dmac_completion(0 / 1 / 2)` (`InterruptRequest::Kind::Dmac`). As a result, the game's registered DMAC channel 0/1/2 completion handler at **`0x004ab6d8` has never executed once in 243.7M services**. In `ckpt-1980k.bin`, the scratchpad busy flags for VIF1 (`0x70002079`) and GIF (`0x70002085`) are stuck at `0x01`, and Thread 3's stack frame (`0x006de6a0`, `{next=0, thid=3}`) is permanently parked on the VIF1 DMA wait list at `[0x7000207c]` inside `0x004a0f68` (`pc 0x004a1098`).
   - **Breakthrough #2 — The "Proven-Unknown" Ring Producer of Thread 2 (`0x00885ee8`) is `0x005aeb68` (`iWakeupThread` wrapper), 16 bytes below `0x005aea78`:** M32 Slices 2, 3, 33, and 34 hunted the producer of the 181 `{0, 3}` jobs in Thread 2's ring buffer (`0x00885ee8`) and concluded it was "proven-unknown / a fossil". Disassembling `0x005aeb68..0x005aec70` (immediately following the creator `0x005aea78`) shows that **`0x005aeb68` is Sony's standard `libkernl` safe `iWakeupThread` wrapper**. It calls `syscall -0x2f` (`iGetThreadId`) and compares `current_thread_id` (`s0`) with `target_thid` (`a0`). When `s0 != a0`, it calls `0x005adbe0` (`syscall -0x34`, raw `iWakeupThread`) directly; **only when `s0 == a0`** does it write `{0, target_thid}` to `0x00885ee8` and signal `sema 11` (`0x005adcd0`).
   - **Breakthrough #3 — `Kernel::block_current` leaves `current_thread_id_` stale when entering idle (`src/ee/kernel.cpp:123-136`):** When the last runnable thread blocks (`SleepThread` / `WaitSema`), `dispatch()` returns `false` without clearing `current_thread_id_`. Consequently, during any idle interrupt (or synchronous DMA completion injected before `SleepThread`), `Kernel::get_thread_id()` (`-0x2f`) returns the sleeping thread's ID as if it were `ThreadRun`, tricking `0x005aeb68` into routing wakeups through Thread 2's ring buffer.
   - **Breakthrough #4 — All 24 Bound SIF Server SIDs Mapped to Exact Disc `.IRX` Modules (Correcting M30 Slices 13/14 and M35 Slice 35):**
     - **M35 Slice 35 waited for `PADMAN` (`0x80000100`), which GT4 never uses:** GT4 uses Sony's **`libpad2` + `sio2d.irx` + `dbcman.irx` + `ds2u_d.irx`** stack. Its SIF RPC servers are **`0x80001300`, `0x8000131c`, `0x8000131e`, `0x8000131f`** (registered in `DBCMAN.IRX` `@0x1e8c`, `@0x1f24`, `@0x1fc8`), which **are already bound**! M30 Slice 13 misidentified `0x80001300` as "the disc subsystem's status query" when answering `0x310` (which is `DBCMAN.IRX`'s module version `0x310` = `PsIIdbcman 3020`).
     - **`0x80000400` (`RPC 0xFE`) is `MCSERV.IRX` (`libmc` Memory Card server, `@0x384`), not "fileio/CDVD":** Subsequent memory card calls (`mcGetInfo`, `mcOpen`, `mcGetDir`) hit the silent zero-reply fallback in `Kernel::sif_rpc_result`.
     - **`0x046d046d` (`LGDEV.IRX` `@0x2248`) is the Logitech USB Force Feedback Wheel driver (`0x046D` = Logitech VID), not a "disc device library":** Answering `0x046DC298` on RPC 4 tells GT4 a **Logitech Driving Force Pro (`046d:c298`)** wheel is plugged in!
     - **`PDISTR.IRX` (`PDI_Streaming_service`) owns `PBGM`, `MPG1`, `MPG2`, `STRP`/`PRTS`, and `VOIC`:** While `PRTS` (`0x53545250`) was modeled in Slice 42, `MPG1` (`0x4d504731`), `MPG2` (`0x4d504732`), `PBGM` (`0x5042474d`), and `VOIC` (`0x564f4943`) still receive silent zero replies.
   - **Breakthrough #5 — Replacing 20-Slice Blind Inference with a PCSX2 SIF/DMA Differential Oracle:**  
     The current `--compare-interpreter` gate proves *translator equivalence* (`translated C++ == interpreter`), because both engines share the same `Kernel` model. It cannot detect *model divergence from PS2 hardware*. Instrumenting PCSX2 (already in `private/pcsx2/`) to log SIF RPCs and DMA completions gives an exact ground-truth oracle.

---

## Part 1 — Confirmed Bugs in the Current Host Model

### 1.1 Bug #1: VIF0, VIF1, and GIF DMA Completions Route to `INTC` Instead of `DMAC` (Confirmed)

**Location:** [`tools/gt4boot/main.cpp:60-64`](file:///c:/Antigravity/gt4-staticrecomp/tools/gt4boot/main.cpp#L60-L64) and [`tools/gt4boot/main.cpp:843-846`](file:///c:/Antigravity/gt4-staticrecomp/tools/gt4boot/main.cpp#L843-L846) (also lines 867, 945, 976).

Currently in `tools/gt4boot/main.cpp`:
```cpp
explicit BootDevices(std::function<void(std::uint32_t)> raise)
    : vif0_dma(0x10008000u, 0x1000u, 4, raise),
      vif1_dma(0x10009000u, 0x1000u, 5, raise),
      gif_dma(0x1000A000u, 0x1000u, 9, raise) {
}
```
And every instantiation of `BootDevices` passes:
```cpp
BootDevices devices(
    [&kernel](std::uint32_t cause) {
        kernel.raise_interrupt(cause);
    });
```
In [`src/ee/kernel.cpp:1156-1158`](file:///c:/Antigravity/gt4-staticrecomp/src/ee/kernel.cpp#L1156-L1158), `Kernel::raise_interrupt(cause)` calls `queue_interrupt(cause)`, which pushes an `InterruptRequest::Kind::Intc` (setting `INTC_STAT` at `0x1000F000` and dispatching `interrupt_handlers_`).

**Why this is wrong on PS2 hardware:**
1. On the Emotion Engine, DMA transfer completions (`Dn_CHCR.TIE = 1`) are **DMAC interrupts** (`AddDmacHandler` / `EnableDmac`, `D_STAT` at `0x1000E010`, dispatched from `dmac_handlers_` via `Kernel::queue_dmac_completion(channel)`), **not** INTC interrupts!
2. On the EE DMAC, the channel numbers are:
   - Channel **0** (`0x10008000`): **VIF0**
   - Channel **1** (`0x10009000`): **VIF1**
   - Channel **2** (`0x1000A000`): **GIF**
   - Channel **3** (`0x1000B000`): **fromIPU**
   - Channel **4** (`0x1000B400`): **toIPU**
   - Channel **5** (`0x1000C000`): **SIF0**
   - Channel **6** (`0x1000C400`): **SIF1**
   - Channel **7** (`0x1000C800`): **SIF2**
   - Channel **8** (`0x1000D000`): **fromSPR**
   - Channel **9** (`0x1000D400`): **toSPR**
   (In M30 Slice 9 / Decision 0011, DMAC channels `0, 1, 2` were confused with INTC causes `4 = VIF0`, `5 = VIF1`, and `9 = TIM0`; note that GIF has no INTC cause at all, so every GIF DMA completion was actually firing a bogus Timer 0 INTC interrupt!)

**Direct guest-code proof (`0x004ab6d8` and `0x004a0f68`):**
Disassembling the game's graphics/DMA manager (`gt4disasm private/fingerprint-check/CORE.GT4 0x004ab6d8 150` and `0x004a0f68 85`):
- The game registers **`0x004ab6d8`** via `AddDmacHandler` for DMAC channels `0` (VIF0), `1` (VIF1), and `2` (GIF):
  ```
  004ab6d8: 0080382d  daddu a3, a0, zero         # a3 = DMAC channel number (0, 1, or 2)
  004ab6dc: 3c047000  lui a0, 0x7000
  004ab6e0: 34842000  ori a0, a0, 0x2000         # a0 = 0x70002000 (scratchpad state block)
  004ab6e8: 00071080  sll v0, a3, 0x2
  004ab6ec: 3c030063  lui v1, 0x63
  004ab6f0: 00621821  addu v1, v1, v0
  004ab6f4: 8c639dc8  lw v1, -0x6238(v1)         # v1 = Dn_CHCR address from 0x00639dc8[a3]
  004ab700: 24860050  addiu a2, a0, 0x50         # a2 = 0x70002050
  004ab708: 8c680000  lw t0, 0x0(v1)             # t0 = Dn_CHCR
  ```
- When a DMA transfer finishes, `0x004ab8dc..0x004ab910` clears the per-channel busy flag at `0x70002050 + a3*12 + 0x1d` (`sb zero, 0xd(a0)` at `0x004ab8e8`) and walks the per-channel wait list at `0x70002050 + a3*12 + 0x20` (`0x70002070` for ch 0, `0x7000207c` for ch 1, `0x70002088` for ch 2), waking every sleeping waiter via `0x004ab900: jal 0x005aeb68` (`iWakeupThread_safe`).
- Meanwhile, **`0x004a0f68`** (where **Thread 3** is parked at `pc 0x004a1098`, called from `0x004a23f0` with `a0 = 1` then `a0 = 2`) checks that exact scratchpad busy byte (`lbu v1, 0(s1)` where `s1 = 0x70002050 + a0*12 + 0x1d`). Because `0x004ab6d8` never ran, the busy bytes at `0x70002079` (ch 1) and `0x70002085` (ch 2) never cleared.
- **Direct verification in `build/ckpt-1980k.bin`:**  
  Dumping scratchpad offset `0x2070..0x208f` in `build/ckpt-1980k.bin` shows:
  ```
  0x70002070: 00000000 0000007b 00000100 006de6a0
  0x70002080: 0000007f 00000100 00000000 00000083
  ```
  - At `0x70002079` (VIF1 busy byte): **`0x01`** (stuck busy).
  - At `0x70002085` (GIF busy byte): **`0x01`** (stuck busy).
  - At `0x7000207c` (VIF1 wait-list head): **`0x006de6a0`** (Thread 3's stack pointer `sp`!).
  - Dumping guest RAM at `0x006de6a0` in `build/ckpt-1980k.bin` shows `00000000 00000003` (`next = 0`, `thread_id = 3`).

**Additional hardware detail in `0x004ab6d8` (`CHCR[30:28]` Tag ID check):**  
Look at `0x004ab70c..0x004ab748`:
```
004ab70c: 00081082  srl v0, t0, 0x2
004ab710: 30420003  andi v0, v0, 0x3             # v0 = CHCR.MOD ((t0 >> 2) & 3)
004ab714: 14450058  bne v0, a1, 0x004ab878       # if MOD != 1 (Chain Mode), go to Normal/Interleave
004ab718: 00081702  srl v0, t0, 0x1c
004ab71c: 24030006  addiu v1, zero, 0x6
004ab720: 30440007  andi a0, v0, 0x7             # a0 = (CHCR >> 28) & 7 = DMA Tag ID!
004ab724: 10830006  beq a0, v1, 0x004ab740       # if ID == 6 (ret), check ASP == 0
004ab728: 24020007  addiu v0, zero, 0x7
004ab72c: 5082006a  beql a0, v0, 0x004ab8d8      # if ID == 7 (end), normal completion!
```
When a Chain Mode (`MOD == 1`) DMA transfer completes on real PS2 hardware, `Dn_CHCR[31:16]` holds bits `[31:16]` of the last DMAtag read, so for an `end` tag (`ID = 7`), `CHCR[30:28]` is `7` (`0x70000000`) and bit 31 (`IRQ`) is `0` (unless the `end` tag also had `IRQ=1`, which still matches `ID == 7` at `0x004ab72c` first!). If `DmaChannel::write_register` completes a Chain Mode (`((value >> 2) & 3) == 1`) transfer synchronously without walking the chain, setting the terminal tag ID to `7` (`(value & ~start_bit & 0x0fffffffu) | 0x70000000u`) makes `0x004ab72c` take the clean `ID == 7` completion branch (`0x004ab8d8`) instead of the stalled-tag/error branch (`0x004ab750..0x004ab770`, which sets error byte `0xc(v0) = 1` or waits at `0x004ab780` for a paired GS interrupt).

---

### 1.2 Bug #2 & Mystery Solved: The "Proven-Unknown" Producer of Thread 2's Ring Buffer (`0x00885ee8`) is `0x005aeb68`, Triggered by Stale `current_thread_id_` (Confirmed)

Across M32 Slices 2, 3, 21, 33, and 34 (`m32-slice2-job-queue.md`, `m32-slice33-ring-producer.md`, `m32-slice34-poster-caught.md`), the project investigated Thread 2 (`0x005ae9a0`), its creator (`0x005aea78`), its semaphore (`sema 11` at `0x00885ee0`), and its 512-slot ring buffer at `0x00885ee8` (which held 181 `{0, 3}` jobs in `ckpt-243m.bin`, but 0 jobs in a fresh `0 -> 200k` service watch). Slice 33 concluded the producer was "proven-unknown", and Slice 34 concluded the 181 jobs were a "fossil".

**Static disassembly of `0x005aeb68..0x005aec70` (immediately following `0x005aea78`) solves the entire mechanism:**
```
005aeb68: 27bdffe0  addiu sp, sp, -0x20          # _iWakeupThread_safe(int thid)
005aeb6c: ffbf0010  sd ra, 0x10(sp)
005aeb70: ffb00000  sd s0, 0x0(sp)
005aeb74: 2403ffd1  addiu v1, zero, -0x2f        # syscall -0x2f = iGetThreadId()
005aeb78: 0000000c  syscall
005aeb7c: 0040802d  daddu s0, v0, zero           # s0 = current_thread_id
005aeb80: 12040005  beq s0, a0, 0x005aeb98       # IF current_thread_id == target_thid (a0), defer to ring!
005aeb84: 2e020100  sltiu v0, s0, 0x100
005aeb88: 0c16b6f8  jal 0x005adbe0               # ELSE call raw iWakeupThread (syscall -0x34) directly!
005aeb8c: 00000000  sll zero, zero, 0x0
005aeb90: 10000017  beq zero, zero, 0x005aebf0
005aeb94: dfbf0010  ld ra, 0x10(sp)
005aeb98: 10400004  beq v0, zero, 0x005aebac
005aeb9c: 3c020066  lui v0, 0x66
005aeba0: 8c4382a8  lw v1, -0x7d58(v0)           # check Thread 2 initialized ([0x006582a8] != 0)
005aeba4: 14600003  bne v1, zero, 0x005aebb4
005aeba8: 3c030088  lui v1, 0x88
...
005aebb4: 3c050088  lui a1, 0x88
005aebb8: 24635ee8  addiu v1, v1, 0x5ee8         # v1 = 0x00885ee8 (ring control block!)
005aebbc: 8ca45ee0  lw a0, 0x5ee0(a1)            # a0 = [0x00885ee0] (sema 11!)
005aebc0: 8c620004  lw v0, 0x4(v1)               # v0 = producer index [0x00885eec]
005aebc4: 304201ff  andi v0, v0, 0x1ff
005aebc8: 00023040  sll a2, v0, 0x1
005aebcc: 24420001  addiu v0, v0, 0x1
005aebd0: 00662821  addu a1, v1, a2
005aebd4: ac620004  sw v0, 0x4(v1)               # [0x00885eec] = producer + 1
005aebd8: 00a0182d  daddu v1, a1, zero
005aebdc: a0a00008  sb zero, 0x8(a1)             # slot.op = 0 (WakeupThread)
005aebe0: 0c16b734  jal 0x005adcd0               # iSignalSema(sema 11) (syscall -0x43)
005aebe4: a0700009  sb s0, 0x9(v1)               # slot.arg = target_thid (in delay slot)
```
And right below `0x005aeb68`:
- **`0x005aec00`** is `_iRotateThreadReadyQueue_safe` (writes `op = 1`, `arg = priority` into `0x00885ee8` and calls `iSignalSema(11)`).
- **`0x005aec78`** is `_iSuspendThread_safe` (writes `op = 2`, `arg = thid` into `0x00885ee8` and calls `iSignalSema(11)`).

**Who calls `0x005aeb68` in the game binary?**  
A full `.text` scan of `SCUS_973.28.elf` finds 10 direct call/jump sites to `0x005aeb68`:
1. **`0x005b199c` (`j 0x005aeb68`)**: Inside `libsifrpc`'s `_request_end` (`RPC_END` completion callback at `0x005b1940`), waking the thread that called `sceSifBindRpc` or `sceSifCallRpc`!
2. **`0x004ab4d0`, `0x004ab500`**: Inside the VBlank handler (`0x004ab430`), waking threads on the GS/DMA wait lists at `0x70002090` and `0x70002070..0x70002088`!
3. **`0x004ab900`**: Inside the DMAC channel 0/1/2 handler (`0x004ab6d8`), waking threads when a VIF0/VIF1/GIF DMA completes!
4. **`0x0054d050`, `0x0054d094`, `0x0054d0d0`, `0x0054d978`, `0x0054d9b8`**: Inside PDI's CD/DVD / streaming completion callbacks!
5. **`0x00576a90`**: Inside the engine's condition-variable / event wakeup helper!

**Why `0x005aeb68` wrote `{0, 3}` 181 times (and why there is a kernel bug in `block_current`):**
1. Look at why Sony's `libkernl` has `0x005aeb68`: on the PS2 BIOS, `iWakeupThread(thid)` fails if `thid` is the *currently running (`ThreadRun`)* thread that was interrupted before it reached `SleepThread()` (see `Kernel::wakeup_thread` at `src/ee/kernel.cpp:546-552`). So `0x005aeb68` checks `if (iGetThreadId() == target_thid)` and, if equal, defers the wakeup to Thread 2 (`KernelTopThread`, priority 1) via the ring buffer at `0x00885ee8`.
2. In `gt4boot`, two things trigger `iGetThreadId() == target_thid`:
   - **Synchronous SIF RPC DMA completions:** When Thread 3 calls `sceSifCallRpc` / `sceSifBindRpc`, `SifSetDma` immediately calls `queue_dmac_completion(5)`. The DMAC channel-5 interrupt is injected at the very next boundary **while Thread 3 is still running (`current_thread_id_ == 3`), before it reaches `SleepThread()` in `sceSifCallRpc`**. Inside that interrupt, `0x005b199c` calls `0x005aeb68(3)`, which sees `iGetThreadId() == 3` and posts `{0, 3}` to `0x00885ee8`!
   - **Stale `current_thread_id_` at idle (`src/ee/kernel.cpp:123-136`):** When the last runnable thread calls `SleepThread()` or `WaitSema()`, `Kernel::block_current` sets `current->status = ThreadWait` and calls `dispatch(state)`. When no thread is ready, `dispatch(state)` returns `false` **without setting `current_thread_id_ = 0` (idle)**. Thus, during idle interrupts, `Kernel::get_thread_id()` (`syscall -0x2f`) continues to return the ID of whichever thread blocked last!
3. And why did Slice 34's `0 -> 200k` fresh-boot watch see 0 posts to `0x00885ee8` while `ckpt-180k..ckpt-243m` had 181 posts?
   - Because `ckpt-60k`, `ckpt-120k`, `ckpt-180k` (created in M32 Slice 6) were **resumed from `ckpt-243m.bin`** (`243.7M + 60k` services, etc.), **not** `60k` services from boot! The 181 posts occurred during the main boot between 200k and 243.7M services whenever Thread 3 made synchronous RPC calls or was woken from `0x004ab500` while `current_thread_id_ == 3`.

---

### 1.3 Bug #3: `Kernel::install_handler_frame` Drops `registration.argument` (`a1` / `gpr[5]`) (Confirmed)

**Location:** [`src/ee/kernel.cpp:947-986`](file:///c:/Antigravity/gt4-staticrecomp/src/ee/kernel.cpp#L947-L986) and [`src/ee/kernel.cpp:1361-1376`](file:///c:/Antigravity/gt4-staticrecomp/src/ee/kernel.cpp#L1361-L1376).

In `Kernel::add_intc_handler` and `Kernel::add_dmac_handler`, the model records `registration.argument = state.read_gpr32(7)` (`a3`, the `void *arg` parameter of `AddIntcHandler2` / `AddDmacHandler2`).  
However, when `Kernel::start_interrupt` and `Kernel::inject_interrupt` build the handler frame in `Kernel::install_handler_frame`:
```cpp
RegisterContext frame;
frame.pc = handler;
frame.gpr[4] = cause;                          // a0 = cause
frame.gpr[28] = interrupted.gpr[28];           // gp as interrupted
frame.gpr[29] = interrupted.gpr[29];           // sp as interrupted
frame.gpr[31] = patch_return_stub_physical;
```
**`frame.gpr[5]` (`a1`) is left as `0` instead of `registration.argument`!**
On the PS2 EE BIOS, every INTC and DMAC handler is called with `(int cause, void *arg, void *addr)` in `(a0, a1, a2)`. For example, the SIF0 DMAC channel-5 handler (`sceSifInitCmd`'s `_sceSifCmdIntrHdlr` at `0x005b0e30`) or timer/engine handlers registered with an argument structure expect `a1 = registration.argument` (note how `0x005b0e30` works around `a1 == 0` only when reading a global fallback or when its argument happens to live in a global, whereas handlers that dereference `a1` directly see null/low memory!). Passing `registration.argument` in `frame.gpr[5]` and `interrupted.pc` in `frame.gpr[6]` matches the hardware ABI.

---

## Part 2 — Complete Ground-Truth Map of All 24 Bound SIF Servers (and Corrected Identities)

In M32 Slice 1 (`docs/reverse-engineering/m32-slice1-server-inventory.md`), `--threads` listed 24 bound SIF server SIDs (16 custom + 8 system), of which only 3 were modeled (`PCDV`, `PRTS`, `FILEIO`) and the remaining 21 received hardcoded version constants or silent all-zero replies in [`Kernel::sif_rpc_result`](file:///c:/Antigravity/gt4-staticrecomp/src/ee/kernel.cpp#L2372-L2462).

By scanning every `.IRX` module in `/IRX/` and `/IOPRP300.IMG;1` on `Gran Turismo 4 (USA) (v2.00).iso` for MIPS `lui + ori/addiu` pairs and 32-bit words passed to `sceSifRegisterRpc`, **100% of the 24 bound SIDs are now mapped to their exact IOP modules and file offsets:**

| Bound SID | Big-Endian ASCII | Little-Endian ASCII | Exact IOP `.IRX` Module on Disc (Version) | Registration Offset in `.IRX` | True Subsystem Role & Current Model Status |
|---|---|---|---|---|---|
| `0x046d046d` | `.m.m` | `m.m.` | `LGDEV.IRX;1` (`LgDev_tb_rb_Driver`, `v1.11` / `0x10b`) | `lui+ori @ 0x2248` | **Logitech USB Wheel Driver (`0x046D` = Logitech USB VID).** **MISIDENTIFIED** in M30 Slice 14 / `kernel.cpp:2418-2440` as "disc device library / loading framework"! Answering `0x046DC298` on RPC 4 tells GT4 a **Logitech Driving Force Pro (`046d:c298`)** is plugged in. |
| `0x50434456` | `PCDV` | `VDCP` | `PDICDVD.IRX;1` (`PDI_CDVD_Manager`, `v1.17` / `0x111`) | `lui+ori @ 0x135c` | **Polyphony Digital CD/DVD Manager (primary).** RPCs 2, 3, 4 modeled in `kernel.cpp`. |
| `0x50636476` | `Pcdv` | `vdcP` | `PDICDVD.IRX;1` (`PDI_CDVD_Manager`, `v1.17` / `0x111`) | `lui+ori @ 0x1520` | **Polyphony Digital CD/DVD Manager (secondary channel `Pcdv`).** Not a "bogus string sid" (M30 Slice 13); registered by `PDICDVD.IRX` at `0x1520`! Currently receives silent zeros. |
| `0x5042474d` | `PBGM` | `MGBP` | `PDISTR.IRX;1` (`PDI_Streaming_service`, `v1.07` / `0x107`) | `lui+ori @ 0x10cc` | **PDI Background Music (BGM) Stream Server.** Currently receives silent zeros. |
| `0x4d504731` | `MPG1` | `1GPM` | `PDISTR.IRX;1` (`PDI_Streaming_service`, `v1.07` / `0x107`) | `lui+ori @ 0x1828` | **PDI MPEG Stream Server #1 (`/mpeg` PSS movies!).** Currently receives silent zeros. |
| `0x4d504732` | `MPG2` | `2GPM` | `PDISTR.IRX;1` (`PDI_Streaming_service`, `v1.07` / `0x107`) | `lui+ori @ 0x1828` | **PDI MPEG Stream Server #2.** Currently receives silent zeros. |
| `0x53545250` | `STRP` | `PRTS` | `PDISTR.IRX;1` (`PDI_Streaming_service`, `v1.07` / `0x107`) | `lui+ori @ 0x2d10` | **PDI Block/Stream Cache Server (`STRP` in BE).** RPCs 3, 4, 7 modeled in Slice 42 & 46. |
| `0x564f4943` | `VOIC` | `CIOV` | `PDISTR.IRX;1` (`PDI_Streaming_service`, `v1.07` / `0x107`) | `lui+ori @ 0x317c` | **PDI Voice / Narration Stream Server.** Currently receives silent zeros. |
| `0x53505550` | `SPUP` | `PUPS` | `PDISPU2.IRX;1` (`PDI_SPU2_Manager`, `v1.18` / `0x112`) | `word @ 0x36b4` | **PDI SPU2 Audio Server (primary).** Currently receives silent zeros. |
| `0x53505554` | `SPUT` | `TUPS` | `PDISPU2.IRX;1` (`PDI_SPU2_Manager`, `v1.18` / `0x112`) | `word @ 0x36b8` | **PDI SPU2 Audio Server (secondary/transfer).** Currently receives silent zeros. |
| `0x62737550` | `bsuP` | `Pusb` | `PDIUSB.IRX;1` (`PDIUSB`, `v1.01` / `0x101`) | `lui+ori @ 0x01d8` | **PDI USB Core / Printer Server.** Currently receives silent zeros. |
| `0x424b5550` | `BKUP` | `PUKB` | `PDIUSB.IRX;1` (`PDIUSB`, `v1.01` / `0x101`) | `lui+ori @ 0x2b40` | **PDI USB Backup Server.** Currently receives silent zeros. |
| `0x534d5550` | `SMUP` | `PUMS` | `PDIUSB.IRX;1` (`PDIUSB`, `v1.01` / `0x101`) | `lui+ori @ 0x3308` | **PDI USB SmartMedia / Printer Server.** Currently receives silent zeros. |
| `0x54485550` | `THUP` | `PUHT` | `PDIUSB.IRX;1` (`PDIUSB`, `v1.01` / `0x101`) | `lui+ori @ 0x38e4` | **PDI USB Thumbnail / Printer Server.** Currently receives silent zeros. |
| `0x45535550` | `ESUP` | `PUSE` | `PDIUSB.IRX;1` (`PDIUSB`, `v1.01` / `0x101`) | `lui+ori @ 0x3f70` | **PDI USB Epson Printer Server (`cdrom0:\EPSON\`).** Currently receives silent zeros. |
| `0x50555354` | `PUST` | `TSUP` | `USTORAGE.IRX;1` (`PDI_USBSTORAGE`, `v1.01` / `0x101`) | `lui+ori @ 0x171c` | **PDI USB Mass Storage Server (Photo Mode flash drive).** Currently receives silent zeros. |
| `0x80000001` | — | — | `IOPRP300.IMG;1` / `MSIFRPC.IRX;1` | `lui+ori @ 0x2278` | **SCE SIF Manager (`SIFMAN` / `MSIFRPC`).** RPC `0xFF` version modeled. |
| `0x80000006` | — | — | `IOPRP300.IMG;1` (`FILEIO`) | `lui+ori @ 0x3b6dc` | **SCE `FILEIO` Server.** RPC `0xFF` version + RPC `0` (`open`) modeled; `read`/`lseek`/`close` unmodeled. |
| `0x80000400` | — | — | `MCSERV.IRX;1` (`mcserv`, `v2.16` / `0x210`, paired with `MCMAN.IRX` `v2.48` / `0x230`) | `lui+ori @ 0x0384` | **SCE Memory Card Server (`libmc` / `XMCSERV`).** **MISIDENTIFIED** in M30 Slice 13 / `kernel.cpp:2409` as "fileio/CDVD version negotiation"! Only RPC `0xFE` (`MC_RPCCMD_INIT`) is answered; `mcGetInfo` (`0x01`), `mcOpen` (`0x02`), `mcGetDir` (`0x0D`), etc. receive silent zeros! |
| `0x80000592` | — | — | `IOPRP300.IMG;1` (`CDVDFSV`) | `lui+ori @ 0x376b0` | **SCE CD/DVD File/Search Server (`sceCdSearchFile`, etc.).** Currently receives silent zeros. |
| `0x80001300` | — | — | `DBCMAN.IRX;1` (`Dbc_Manager`, `v3.16` / `0x310`, `PsIIdbcman 3020`) | `lui+ori @ 0x1e8c` | **SCE `libpad2` Device Bus Controller Manager (Master).** **MISIDENTIFIED** in M30 Slice 13 / `kernel.cpp:2402` as "disc subsystem status query" and missed by M35 Slice 35! |
| `0x8000131c` | — | — | `DBCMAN.IRX;1` (`Dbc_Manager`, `v3.16` / `0x310`) | `lui+ori @ 0x1f24` | **SCE `libpad2` DBC Socket/Port Channel A.** Paired with `DS2U_D.IRX;1` (`PsIIpdman_pl3020`). Currently receives silent zeros. |
| `0x8000131e` | — | — | `DBCMAN.IRX;1` (`Dbc_Manager`, `v3.16` / `0x310`) | `lui+ori @ 0x1fc8` | **SCE `libpad2` DBC Socket/Port Channel B (port 0).** Currently receives silent zeros. |
| `0x8000131f` | — | — | `DBCMAN.IRX;1` (`Dbc_Manager`, `v3.16` / `0x310`) | `0x8000131e + 1` | **SCE `libpad2` DBC Socket/Port Channel B (port 1).** Currently receives silent zeros. |

*Bonus embedded IRX discovery:* At file offset `0x54be80` (guest address `0x0064ae80` in `.data`, length `0x5b5` = 1,461 bytes), `SCUS_973.28.elf` contains an embedded R3000A IOP ELF module (`e_type = 0xff80`, `e_machine = 8`) named **`rt_ac`** (`version 0x105`, imports `sysmem`, `modload`, `thbase`), loaded from EE memory during boot.

---

### 2.1 Critical Implications of the 24-Server Map

1. **M35 (Controllers) is NOT waiting on `PADMAN` (`0x80000100`); `libpad2` (`0x80001300`..`0x8000131f`) is ALREADY bound!**
   - M35 Slice 35 (`docs/reverse-engineering/m35-slice35-pad-groundwork.md`) deferred controller specification because `0x80000100`/`0x80000101` (`PADMAN`) was not in the bound server list.
   - Even though `PADMAN.IRX` exists on the disc in `/IRX/`, GT4's module loader (`0x0068BC80`) loads **`sio2man.irx` -> `sio2d.irx` -> `dbcman.irx` -> `pad2/ds2u_d.irx`**, which is Sony's newer `libpad2` stack (`PsIIlibpad2 3020` / `PsIIdbcman 3020` / `PsIIpdman_pl3020`).
   - `DBCMAN.IRX` registers `0x80001300`, `0x8000131c`, `0x8000131e`, and `0x8000131f` — **all four of which are already bound in the checkpoint!**
   - How `libpad2` + `dbcman` + `ds2u_d` works (confirmed by `DBCMAN.IRX` strings `'dbcman: Connect DepNo.=%d'`, `'dbcman: dbcSignalData() - bad data size align'`, `'dbcman: sceSifSetDma faild'` and `DS2U_D.IRX` imports `vblank`, `sio2man`, `dbcman`):
     - The EE calls `scePad2CreateSocket` (via `0x80001300` / `0x8000131c` / `0x8000131e` / `0x8000131f`), passing the EE ring/socket buffer address (64-byte aligned, cf. string `"buffer addr is not 64 byte align"` at `0x006CF398`).
     - Every VBlank on the IOP, `ds2u_d.irx` polls the DualShock 2 via `sio2d`/`sio2man` and calls `dbcSignalData()` in `dbcman.irx`, which DMAs the controller state descriptor (`sceSifSetDma`) directly into the EE's registered socket buffer!

2. **Why `0x046d046d` (`LGDEV.IRX`) Should NOT Report a Connected Driving Force Pro (`0x046DC298`) Unless Model-Complete:**
   - In [`src/ee/kernel.cpp:2418-2440`](file:///c:/Antigravity/gt4-staticrecomp/src/ee/kernel.cpp#L2418-L2440), the model answers `0x046d046d` RPC 4 with `0x046DC298` at `+0x5C`.
   - `LGDEV.IRX` constants (`@0x2694: 0x046dc298`, `@0x26ac: 0x046dc293`, `@0x26c0: 0x046dca03`, `@0x3e28: 0x046dc294`) prove these are Logitech USB Vendor/Product IDs (`046d:c298` = Driving Force Pro).
   - In M30 Slice 14 (`docs/journal/2026-10-02.md:958`), `0x046D046D` RPC 6, RPC `0x0F`, and RPC `0x0D` were among the hottest RPC calls in the entire boot (thousands of calls), because `0x046DC298` convinced the game that a USB steering wheel is attached and ready for calibration/polling, while RPCs 6, `0x0D`, `0x0F` return all zeros!

3. **Why `0x80000400` (`MCSERV.IRX`) Needs Real `libmc` Replies:**
   - Verified against `ps2sdk/ee/rpc/memorycard/src/libmc.c` (lines 61–103): `0x80000400` is `libmc`'s RPC server (`XMCSERV`), and RPC `0xFE` is `MC_RPCCMD_INIT` (`mcRpcCmd[MC_TYPE_XMC][MC_RPCCMD_INIT] = 0xFE`).
   - In `docs/journal/2026-10-02.md:958`, the game already issued **537 calls to `0x80000400` RPC `0x01` (`MC_RPCCMD_GET_INFO` = `mcGetInfo`) and 479 calls to RPC `0x15`**, all of which received silent all-zero replies!
   - Note how `libmc` works (`ps2sdk/ee/rpc/memorycard/src/libmc.c:219-240`): `mcGetInfo` (`RPC 0x01`) is called with `SIF_RPC_M_NOWAIT` and an `end_function` callback that reads `m_rpc_rdata` and writes `*m_p_type`, `*m_p_free`, `*m_p_format` via `m_extra_send_recv_param` (or `m_result`). Answering `0` for `m_result` tells the game **"status 0 = card present and formatted, same card as last check"** with `type = 0 (unformatted/none)`, `free = 0`, `format = 0` — an inconsistent combination that can trap the game's autosave/boot memory-card state machine! Returning `-1` (`0xFFFFFFFF`, no card inserted) or modeling a clean formatted card (`SCE_MC_TYPE_PS2 = 2`, `free = 8192`, `format = 1`, `result = -2` on first detection then `0`, with `MC_RPCCMD_GET_DIR = 0x0D` returning `-4` `McResNoEntry`) is trivial and deterministic.

4. **Why `PDISTR.IRX` (`MPG1` `0x4d504731`, `PBGM` `0x5042474d`) Matters for the `/mpeg` Boot Phase:**
   - M30 Slice 17 established that the boot is in its `/mpeg` opening-movie / attract phase (`mv0010` / `gtloading.img`).
   - `PDISTR.IRX` (`PDI_Streaming_service`) registers `STRP`/`PRTS` (`0x53545250`, modeled in Slice 42), **`MPG1` (`0x4d504731`)**, **`MPG2` (`0x4d504732`)**, **`PBGM` (`0x5042474d`)**, and **`VOIC` (`0x564f4943`)**.
   - While `PRTS` serves the raw archive blocks, `MPG1`/`MPG2` and `PBGM`/`SPUP` control stream playback and completion status. Because `MPG1`, `PBGM`, and `SPUP` receive all-zero replies in `Kernel::sif_rpc_result`, any worker polling stream state or waiting for a stream-end notification sees permanent zeroes.

---

## Part 3 — Strategic & Architectural Recommendations Going Forward

### Priority 1 (Immediate, High ROI): Fix the 3 Confirmed Kernel/Device Bugs & Test Against the Existing Checkpoints

Before writing any new subsystem, fix the three concrete hardware-model bugs uncovered in Part 1:
1. **Fix `BootDevices` DMA completion routing (`tools/gt4boot/main.cpp` & `src/ee/device.cpp`):**
   - Change `vif0_dma`, `vif1_dma`, `gif_dma` channel numbers in `BootDevices` from `(4, 5, 9)` to **`(0, 1, 2)`**.
   - Route `BootDevices`'s `DmaChannel` completion callback to **`kernel.queue_dmac_completion(channel)`** (`InterruptRequest::Kind::Dmac`) instead of `kernel.raise_interrupt(cause)` (`InterruptRequest::Kind::Intc`).
   - In `DmaChannel::write_register`, when completing a Chain Mode (`((value >> 2) & 3) == 1`) transfer synchronously, set the terminal DMAtag ID field `CHCR[30:28] = 7` (`end` tag, `0x70000000u`) while clearing `STR` (`bit 8`) and `IRQ` (`bit 31`) so `0x004ab6d8` (`0x004ab72c`) takes the normal end-of-chain completion path (`0x004ab8d8`), clears the scratchpad busy byte (`0x70002050 + ch*12 + 0x1d`), and wakes waiting threads (`0x004ab900: jal 0x005aeb68`).
2. **Pass `registration.argument` (`a1`) and `interrupted.pc` (`a2`) in `Kernel::install_handler_frame` (`src/ee/kernel.cpp:1361-1376`):**
   - Carry `{handler, argument}` in `DeferredCall` and set `frame.gpr[5] = argument` and `frame.gpr[6] = interrupted.pc`.
3. **Clear `current_thread_id_` (or check `current->status == ThreadRun`) when all threads block (`src/ee/kernel.cpp:80-98, 123-136, 491-494`):**
   - When `pick_next_ready()` returns `nullptr` in `dispatch()`, set `current_thread_id_ = 0` (no running thread), or in `get_thread_id()` return `0` / `-1` when the interrupted context was idle, so `0x005aeb68` (`syscall -0x2f`) does not mistake a sleeping thread (`ThreadWait`) for a running thread (`ThreadRun`).

**Expected immediate payoff:**  
On a fresh boot (and on `ckpt-1980k.bin` if a VIF1/GIF DMA is triggered), `0x004ab6d8` will execute for the first time, clearing `0x70002079` / `0x70002085` and unblocking Thread 3 at `0x004a1098` without needing the 120-VBlank timeout fallback.

---

### Priority 2 (Highest Leverage Tooling): Build a Dynamic PCSX2 SIF/DMA Differential Oracle

**Why the project got stuck in 10–20 slice detective arcs:**  
- Slices 23–42 took **20 slices** of reverse-engineering downstream memory corruption (odd pointer `0x008475E7` in sound init) to discover that `PRTS` (`0x53545250`) RPC 3/4/7 was returning all-zeros.
- Slices 43–46 took **4 slices** of reverse-engineering a font relocation crash (`0x009cf08f`) to discover that `PRTS` copy-out needed a per-handle cursor.
- Slices 47–62 spent **40+ slices** probing the 243.7M stall from the inside.

Every single one of those investigations was caused by [`Kernel::sif_rpc_result`](file:///c:/Antigravity/gt4-staticrecomp/src/ee/kernel.cpp#L2390) silently returning zeros (`// Anything else answers an empty result`), which violates `AGENTS.md`:
> *"Never guess in code: unsupported instructions and services stop with useful context; returning success cannot establish correctness. No silent fallbacks."*

**How to build the Oracle in 1–2 slices using existing local assets:**
1. **Option A — PCSX2 Savestate / PINE Breakpoint Trace (Zero C++ compilation of PCSX2):**  
   `private/pcsx2/` already contains PCSX2 v2.9.94 configured with PINE (`port 28011`) and menu savestates (`private/pcsx2/sstates/`). Even simpler: PCSX2's debugger / PINE protocol or a small PCSX2 patch logs every call to `sceSifSendCmd` (`SifSetDma` syscall `0x77`/`-0x77`) on the EE side (`cid = 0x80000009` `RPC_BIND`, `cid = 0x8000000a` `RPC_CALL`, `cid = 0x80000008` `RPC_END`), capturing:
   - `{seq, pc, thread_id, cid, sid, rpc_number, send_size, send_bytes_hash, recv_size, recv_bytes_first64}`.
2. **Option B — Eliminate Silent RPC Fallbacks in `gt4boot` (`--strict-rpc` / RPC coverage table):**  
   In `Kernel::answer_sif_rpc_call`, log a per-`(sid, rpc_number)` call histogram in `gt4boot --threads` (showing call count, send/recv sizes, and whether the pair is `Modeled`, `CompatibilityConstant`, or `UnmodeledZeroFallback`), and add a `--strict-rpc` flag that stops with full context (`pc`, `thread_id`, `sid`, `rpc_number`, request bytes) on the first unmodeled `(sid, rpc_number)` call.
3. **Diffing `gt4boot`'s RPC trace against PCSX2's boot-to-menu RPC trace** immediately pinpoints the **first divergent RPC reply** in seconds instead of 20 slices.

---

### Priority 3: Disassemble the `.IRX` Modules on Disc & Decide IOP HLE vs. Hybrid R3000A LLE (Decision 0028)

A major blind spot in the repository up to Slice 62 is that **none of the `.IRX` files in `/IRX/` were disassembled**, even though they are plain, unencrypted MIPS I (R3000A) ELF binaries sitting on the pinned ISO!
- `PDICDVD.IRX` is only **9,597 bytes** (~1,500 instructions).
- `PDISTR.IRX` is only **20,773 bytes** (~3,500 instructions).
- `PDISPU2.IRX` is only **19,261 bytes** (~3,200 instructions).
- `DBCMAN.IRX` is only **15,653 bytes** (~2,500 instructions).
- `DS2U_D.IRX` is only **11,821 bytes** (~1,900 instructions).
- `MCSERV.IRX` is only **7,385 bytes** (~1,100 instructions).

Even Ghidra (`private/tooling/ghidra_12.1.3_PUBLIC`, already in the repo!) or `gt4disasm` (which already decodes base MIPS I instructions!) can disassemble the `.text` section of every `.IRX` file in `/IRX/` in seconds!

**Two architectural paths for M32 (record as Decision 0028):**
1. **Path A — IRX-Guided HLE (Short-Term Fastest Unblock):**  
   Disassemble `MCSERV.IRX`, `DBCMAN.IRX`/`DS2U_D.IRX`, `PDICDVD.IRX`, `PDISTR.IRX`, `PDISPU2.IRX`, and `LGDEV.IRX` with Ghidra/Python to read their exact RPC dispatch tables (`switch (rpc_number)`), request/reply struct layouts, and async SIF DMA callbacks (`sceSifSetDma`). Then implement exact, evidence-backed handlers in `Kernel`:
   - `0x046d046d` (`LGDEV.IRX`): Return "no USB wheel connected" (0 devices) instead of faking a Driving Force Pro (`0x046DC298`), shutting down the bogus wheel polling loop.
   - `0x80000400` (`MCSERV.IRX`): Implement `mcGetInfo` (`0x01`), `mcOpen` (`0x02`), `mcClose` (`0x03`), `mcRead` (`0x05`), `mcWrite` (`0x06`), `mcGetDir` (`0x0D`) backed by a host directory or virtual 8 MB card (or return `-1` = no card inserted to pass boot).
   - `0x80001300`..`0x8000131f` (`DBCMAN.IRX` + `DS2U_D.IRX`): Implement the `libpad2` socket registration and per-VBlank 64-byte DualShock 2 state DMA write into the EE socket buffer.
   - `0x53545250` / `0x4d504731` / `0x5042474d` (`PDISTR.IRX`) & `0x53505550` (`PDISPU2.IRX`): Read the RPC dispatch tables in `PDISTR.IRX` and `PDISPU2.IRX` to answer stream init/status/stop calls accurately.
2. **Path B — Hybrid IOP R3000A Interpreter (`ps2xIOP` pattern, Medium-Term):**  
   As demonstrated by `ran-j/PS2Recomp` (`ps2xIOP`), the IOP is a simple 32-bit MIPS I (R3000A) CPU with no FPU, no MMI, and a 2 MB RAM space (`0x00000000..0x001fffff`), where `.IRX` modules link against standard IOP kernel exports (`thbase`, `thsemap`, `sifman`, `sifcmd`, `intrman`, `cdvdman`, `sio2man`, `libsd`). Running the game's custom PDI `.IRX` modules (`PDICDVD.IRX`, `PDISTR.IRX`, `PDISPU2.IRX`, `LIBPDI.IRX`) in a small (~800-line) R3000A interpreter while HLE-stubbing their IOP kernel imports eliminates protocol-guessing forever.

---

### Priority 4: Replace the 1-ms-per-Service Clock with an Instruction-Count / Cycle-Derived Clock

**Problem with Decision 0016 (`src/ee/kernel.cpp:1279`):**  
Advancing `BUSCLK` by 1 ms (`147,456` ticks) per handled syscall/service (`patch_return_service = 0x100` included) means:
- 243,711,723 services = **243,711 seconds ≈ 67.7 hours** of virtual PS2 uptime!
- Worse, during busy interrupt bursts (where each handler return `0x100` counts as 1 service = 1 ms), the timer advances 576 `TIM2` ticks per handler return even though only ~50 instructions executed! This is the root cause of the **M32 Slices 14–20 "firing lottery"** (*"firing sliver narrower than one frame's advance"*), where `TIM2` jumps over compare windows or wraps its 32-bit epoch counter before worker threads can run.
- Tying `BUSCLK` advancement to **executed guest instructions** (e.g., `1 instruction ≈ 1 CPU cycle = 0.5 BUSCLK ticks`, accrued deterministically at every module call and interpreter step, with idle advancing to the next timer compare or VBlank boundary) keeps the translated-vs-interpreter differential 100% deterministic (or keeps a service-proportional mode for `--compare-interpreter` while using instruction/idle-skip timing for `gt4boot`), and eliminates the 40-minute epoch wrap artifact.

---

### Priority 5: Parallel De-Risking for M33/M34 (GS + VU1 + IPU + RoFS v3.1)

1. **Settle the RoFS v3.1 Page Cipher Dynamically in 1 Run (`docs/reverse-engineering/asset-page-cipher-static.md`):**  
   The static grid in `asset-page-cipher-static.md` already designed **Hook A** (`0x4B36E0` XOR-`0x55` buffer logger) and **Hook B** (`0x4B39B0` page-fetch in/out buffer logger), and noted they only need a short `--disc` boot (~83k services, ~20 seconds) because the boot mounts and reads the inner v3.1 archives before service 83,783! Running Hook A + Hook B on an 85k-service run immediately unlocks offline extraction of every v3.1 `.gpb` texture and menu asset on the disc.
2. **Start M33/M34 (GS + VIF1 + VU1 Micro-Mode) Against PCSX2 GS Dumps / Savestates:**  
   GT4's rendering pipeline relies heavily on **VIF1 unpack + VU1 microprograms (`MPG` / `MSCAL` / `MSCNT`) + GIF PATH1/PATH2/PATH3**. Currently, `VCALLMS`/`VCALLMSR` and VU1 micro-execution are out of scope. Rather than waiting for `gt4boot` to reach the menu to begin M34, capture a single-frame GS/VIF1 dump from the existing PCSX2 menu savestate (`private/pcsx2/sstates/`) and build/verify the VIF1/VU1/GIF/Vulkan replay harness against real GT4 menu packets in parallel.
3. **Reduce Interpreter Bridge Overhead (`M30` open item):**  
   In the 243.7M run, `gt4boot` executed **7.1 billion interpreted steps** vs. **241.8 million native module calls** (~30 interpreted instructions per boundary), because after a `syscall`, `jalr`, or `jr ra`, the driver steps the interpreter until the next function entry in `translated::has_entry`. Emitting **post-syscall and post-`jal` return-site resume labels** into `translated::call_entry` (or calling the `ServiceTable` inline from generated C++ when `pc` does not switch threads) will speed up `gt4boot` by **5×–10×**, turning 20-minute runs into 2-minute runs.

---

### Priority 6: Repository & Process Hygiene for the Operating Agent

1. **Pause Lesson Writing Until M31 (Title Menu Idle) Is Reached:**  
   Slices 36–43 and 44–61 produced 22 comprehensive lessons (`docs/lessons/`), and the lessons backlog is now 100% closed. All engineering bandwidth should return to unblocking M31/M32.
2. **Compact `docs/STATUS.md` (Currently 1,693 Lines / 114 KB):**  
   Reading 114 KB of chronological slice summaries at the start of every session consumes context window and leaves stale sections behind (e.g., `## Next actions` at line 1575 still lists *"1. M30 slice 22: the library's parse of the root directory block"* from 40 slices ago!). Move the M0–M30 slice-by-slice narrative to `docs/STATUS_ARCHIVE.md` and keep `docs/STATUS.md` under 200 lines: current milestone frontier, test counts, active hypotheses, and the exact next slice.

---

## Concrete Next-Slice Action Plan for the Agent

| Order | Proposed Slice | Concrete Action & Acceptance Criteria |
|---|---|---|
| **1** | **M32 Slice 36 — Fix DMAC Channel 0/1/2 Routing & Handler Arguments** | 1. In [`tools/gt4boot/main.cpp`](file:///c:/Antigravity/gt4-staticrecomp/tools/gt4boot/main.cpp#L60-L64), change `vif0_dma`, `vif1_dma`, `gif_dma` causes from `(4, 5, 9)` to channels `(0, 1, 2)` and wire their callback to `kernel.queue_dmac_completion(channel)`.<br>2. In [`src/ee/device.cpp`](file:///c:/Antigravity/gt4-staticrecomp/src/ee/device.cpp#L129-L143), set `CHCR[30:28] = 7` (`end` tag ID) when completing a Chain Mode (`MOD == 1`) transfer synchronously.<br>3. In [`src/ee/kernel.cpp`](file:///c:/Antigravity/gt4-staticrecomp/src/ee/kernel.cpp#L1361-L1376), pass `registration.argument` in `a1` (`gpr[5]`) and `interrupted.pc` in `a2` (`gpr[6]`) when injecting INTC/DMAC handlers, and clear `current_thread_id_ = 0` when `dispatch()` finds no ready thread.<br>4. Add unit tests in `ee_kernel_tests` + `ee_device_tests` and verify on a boot run that `0x004ab6d8` runs and clears `0x70002079`/`0x70002085`. |
| **2** | **M32 Slice 37 — RPC Call Histogram & No-Silent-Fallback Telemetry** | Add a per-`(sid, rpc_number)` call counter table to `Kernel` (printed by `gt4boot --threads`), recording call count, request/reply sizes, and handler status (`Modeled` vs `ZeroFallback`). Identify every unmodeled `(sid, rpc_number)` called during a `0 -> 200k` and `243.7M` boot run. |
| **3** | **M32 Slice 38 — Disassemble Disc `.IRX` RPC Dispatchers (`MCSERV`, `LGDEV`, `DBCMAN`, `PDICDVD`, `PDISTR`, `PDISPU2`)** | Write a small Python/Ghidra inspection script over `/IRX/*.IRX` from the ISO to document the exact RPC command numbers and reply structs of `MCSERV.IRX` (`0x80000400`), `LGDEV.IRX` (`0x046d046d`), `DBCMAN.IRX` (`0x80001300`..`131f`), `PDICDVD.IRX` (`0x50636476`), `PDISTR.IRX` (`MPG1`, `MPG2`, `PBGM`, `VOIC`), and `PDISPU2.IRX` (`SPUP`, `SPUT`). |
| **4** | **M32 Slice 39 — Fix `LGDEV` (No Wheel) & `MCSERV` (`libmc` Memory Card) Replies** | 1. Change `0x046d046d` (`LGDEV.IRX`) to report no USB wheel attached (or verify against PCSX2 RAM/RPC trace).<br>2. Model `0x80000400` (`MCSERV.IRX`) RPC `0x01` (`mcGetInfo`), `0x0D` (`mcGetDir`), etc. per `ps2sdk/ee/rpc/memorycard/src/libmc.c`.<br>3. Verify how far the boot advances past the old 243.7M park. |
| **5** | **M32 Slice 40 — `PDISTR` (`MPG1`/`PBGM`), `PDISPU2` (`SPUP`), and `DBCMAN` (`libpad2`) Replies** | Implement the remaining active RPC servers from their `.IRX` disassembly and/or PCSX2 SIF trace until `gt4boot` reaches the M31 title-menu loop and emits its first VIF1/GIF display-list packets (re-opening M33). |
| **6** | **Asset Track — Run Hook A & Hook B on an 85k-Service Boot** | Execute the already-specified Hook A (`0x4B36E0`) and Hook B (`0x4B39B0`) from `docs/reverse-engineering/asset-page-cipher-static.md` over an 85,000-service `--disc` run to capture the exact in/out page transform for RoFS v3.1 and unlock `.gpb` texture extraction. |
