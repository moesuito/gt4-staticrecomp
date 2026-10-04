#pragma once

// The EE kernel model: threads and semaphores with the deterministic
// cooperative scheduler of docs/decisions/0005-thread-scheduler.md. The
// kernel owns the thread and semaphore tables; the register context of the
// running thread lives in GuestState, and a switch saves it into the
// thread's slot and restores the next thread's. There is no timer
// preemption: a thread runs until it blocks, or until a service makes a
// strictly higher-priority thread ready.

#include "gt4recomp/ee_device.hpp"
#include "gt4recomp/ee_services.hpp"
#include "gt4recomp/ee_state.hpp"
#include "gt4recomp/ee_timer.hpp"

#include <array>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace gt4recomp {
// The disc image interfaces the file and CD services read from
// (disc_image.hpp).
class DiscFiles;
class DiscByteSource;
} // namespace gt4recomp

namespace gt4recomp::ee {

// Thread status bits, from the public THS_* definitions.
enum ThreadStatus : std::uint32_t {
    ThreadRun = 0x01,
    ThreadReady = 0x02,
    ThreadWait = 0x04,
    ThreadSuspend = 0x08,
    ThreadDormant = 0x10
};

// Why a thread is in WAIT.
enum ThreadWaitType : std::uint32_t {
    ThreadWaitNone = 0,
    ThreadWaitSleep = 1,
    ThreadWaitSema = 2
};

// One registered interrupt or DMA handler. The model records the
// registration so removal and re-registration behave; delivery follows the
// pending/mask/CP0-gate contract of the dispatch path.
struct KernelInterruptHandler {
    std::uint32_t id = 0;
    std::uint32_t cause = 0;
    std::uint32_t handler = 0;
    std::uint32_t argument = 0;
};

// One handler call in flight: the entry point plus the argument its
// registration carried. The pair travels as a unit from registration
// through injection to the call frame, so two handlers for one cause
// keep their own arguments (decision 0031).
struct InterruptHandlerCall {
    std::uint32_t handler = 0;
    std::uint32_t argument = 0;
};

// One semaphore. The kernel is the only writer of count and wait_threads,
// matching the public ee_sema_t contract.
struct KernelSemaphore {
    std::uint32_t id = 0;
    std::int32_t count = 0;
    std::int32_t max_count = 0;
    std::int32_t init_count = 0;
    std::int32_t wait_threads = 0;
    std::uint32_t attr = 0;
    std::uint32_t option = 0;
};

// One thread plus its saved register context. The context's pc is where the
// thread resumes; a running thread's live registers are in GuestState.
struct KernelThread {
    std::uint32_t id = 0;
    std::uint32_t status = ThreadDormant;
    std::uint32_t function = 0;
    std::uint32_t stack = 0;
    std::uint32_t stack_size = 0;
    std::uint32_t gp = 0;
    std::int32_t initial_priority = 0;
    std::int32_t current_priority = 0;
    std::uint32_t attr = 0;
    std::uint32_t option = 0;
    std::uint32_t wait_type = ThreadWaitNone;
    std::uint32_t wait_id = 0;
    std::uint32_t wakeup_count = 0;
    RegisterContext context;
};

// The scheduler and the service implementations. One kernel belongs to one
// execution; the driver and the differential reference each build their own.
class Kernel {
public:
    // Adds every thread and semaphore service to the table. The handlers
    // capture this kernel, so the table must not outlive it.
    void register_services(ServiceTable& services);

    // The services, exposed directly for unit tests. Every one reads its
    // arguments from the registers like the syscall would and returns what
    // the driver should do next. Public ps2sdk prototypes name the arguments
    // in each implementation's comment; the numbers are the EE syscall
    // numbers from syscallnr.h.
    ServiceOutcome setup_thread(GuestState& state);        // 0x3C
    ServiceOutcome create_thread(GuestState& state);       // 0x20
    ServiceOutcome delete_thread(GuestState& state);       // 0x21
    ServiceOutcome start_thread(GuestState& state);        // 0x22
    ServiceOutcome exit_thread(GuestState& state);         // 0x23
    ServiceOutcome exit_delete_thread(GuestState& state);  // 0x24
    ServiceOutcome terminate_thread(GuestState& state);    // 0x25
    ServiceOutcome change_thread_priority(GuestState& state);  // 0x29
    ServiceOutcome rotate_ready_queue(GuestState& state);  // 0x2B
    ServiceOutcome release_wait_thread(GuestState& state); // 0x2D
    ServiceOutcome get_thread_id(GuestState& state);       // 0x2F
    ServiceOutcome refer_thread_status(GuestState& state); // 0x30
    ServiceOutcome sleep_thread(GuestState& state);        // 0x32
    ServiceOutcome wakeup_thread(GuestState& state);       // 0x33
    ServiceOutcome cancel_wakeup_thread(GuestState& state);  // 0x35
    ServiceOutcome suspend_thread(GuestState& state);      // 0x37
    ServiceOutcome resume_thread(GuestState& state);       // 0x39
    ServiceOutcome create_sema(GuestState& state);         // 0x40
    ServiceOutcome delete_sema(GuestState& state);         // 0x41
    ServiceOutcome signal_sema(GuestState& state);         // 0x42
    ServiceOutcome wait_sema(GuestState& state);           // 0x44
    ServiceOutcome poll_sema(GuestState& state);           // 0x45
    ServiceOutcome refer_sema_status(GuestState& state);   // 0x47
    ServiceOutcome set_syscall(GuestState& state);         // 0x74
    // The OSD configuration word (ConfigParam): Get writes it to the guest
    // address, Set stores what the guest wrote. The kernel of a late console
    // retains every field, including the version bits the SDK probes for an
    // early Japanese kernel; the initial value is a documented USA default
    // (decision 0008).
    ServiceOutcome get_osd_config(GuestState& state);      // 0x4B
    ServiceOutcome set_osd_config(GuestState& state);      // 0x4A
    // The extended OSD configuration block (ConfigParam2, four bytes): the
    // services copy between it and the guest buffer with the size/offset the
    // caller asks for. Reads past the block are zeros; writes past it are
    // ignored, exactly the reference emulator's behavior.
    ServiceOutcome set_osd_config2(GuestState& state);     // 0x6E
    ServiceOutcome get_osd_config2(GuestState& state);     // 0x6F
    // The SIF (IOP interface) services of decision 0009. The register
    // services map the public register indices onto the SIF register block;
    // SetDma performs the transfer synchronously and runs the model's IOP
    // stub for the SIFCMD commands it recognizes.
    ServiceOutcome sif_set_reg(GuestState& state);         // 0x79
    ServiceOutcome sif_get_reg(GuestState& state);         // 0x7A
    ServiceOutcome sif_set_d_chain(GuestState& state);     // 0x78
    ServiceOutcome sif_stop_dma(GuestState& state);        // 0x6B
    ServiceOutcome sif_set_dma(GuestState& state);         // 0x77
    ServiceOutcome sif_dma_stat(GuestState& state);        // 0x76
    // The GS interrupt mask register: stored 64-bit, GsPutIMR returns the
    // previous value (both travel in one GPR under the EE's 64-bit ABI).
    ServiceOutcome gs_get_imr(GuestState& state);          // 0x70
    ServiceOutcome gs_put_imr(GuestState& state);          // 0x71
    // Deci2Call(call, address): the DECI2 debug-host interface. No debug
    // host is attached; the defined calls are accepted with the reference
    // emulator's returns.
    ServiceOutcome deci2_call(GuestState& state);          // 0x7C
    // SetGsCrt(interlace, video, field): the model has no display, so the
    // call is accepted with no state; the game only needs it to return.
    ServiceOutcome set_gs_crt(GuestState& state);          // 0x02

    // Interrupt injection: the driver calls this at unit boundaries. It
    // saves the running context, sets up the registered handler's call
    // frame (a0 = cause, a1 = the registration's argument, a2 = the
    // interrupted pc, returning through the model's stub), and returns
    // true while the handler context is live. False when none is pending.
    // Over idle the saved context belongs to the idle thread below.
    [[nodiscard]] bool start_interrupt(GuestState& state);

    // A device reports an occurrence: an INTC cause joins the pending state
    // and the dispatch queue. Pending exists whether or not a handler is
    // registered and whether or not the mask allows delivery; the dispatch
    // step decides about handlers later.
    void raise_interrupt(std::uint32_t cause);

    // A DMA channel reports a completion: the channel's CIS bit is set and
    // the completion joins the dispatch queue. Like raise_interrupt, this
    // records the occurrence even with no handler registered.
    void raise_dmac_completion(std::uint32_t channel);

    // The device units behind the MMIO windows. The boot wires them after
    // construction; until then the kernel keeps the P00-era degraded
    // behavior (queue without status bits, dispatch without mask checks),
    // which unit tests must not rely on for contract coverage.
    void set_timer_unit(TimerUnit* timer) noexcept;
    void set_intc_unit(IntcUnit* intc) noexcept;
    void set_dmac_unit(DmacStatusUnit* dmac) noexcept;

    // The idle-time interrupt source: when no thread can run, the model
    // advances time by one frame through the unified machine below
    // (queueing the timers' compare interrupts and one VBlank when their
    // handlers are registered). The handlers may wake threads; their
    // return re-dispatches. A bounded budget of consecutive idle
    // interrupts without a runnable thread stops the model instead of
    // spinning forever. The interrupted context of an idle injection is
    // the idle thread (idle_thread_id below), never the id of whichever
    // thread blocked last.
    [[nodiscard]] bool deliver_idle_interrupt(GuestState& state);
    // The unified advance machine both quanta share (decision 0030): the
    // caller names a BUSCLK-tick delta, every counting timer takes its
    // clock's share of it (the CLKS division with one fractional
    // remainder per timer, shared by both paths), and VBlank accumulates
    // toward one frame on the same shared accumulator. Splitting one
    // delta into smaller advances with no guest writes between them
    // reaches the identical state (decomposition, pinned by unit tests);
    // guest writes between the pieces - a handler reprogramming COMP or
    // acknowledging a flag - intentionally steer the next piece, which is
    // the reprogramming opportunity the scheduler offers between events.
    // Both engines call the two quanta below in the same order, so the
    // differential stays exact.
    void advance_busclk(GuestState& state, std::uint32_t busclk_ticks);
    // Advances the model's time base by one handled service: one millisecond
    // of BUSCLK ticks. Both engines call this exactly once per handled
    // service, so the time base stays a function of the guest's service
    // sequence and the differential stays exact; a cycle-accurate clock is
    // out of scope (decision 0016). The quantum is unchanged by P03: only
    // the mechanism is now shared with the idle path (decision 0030).
    void advance_service_time(GuestState& state);

    // The disc image the file services read from. A null image means no
    // disc: file opens answer "not found", exactly like a console without
    // one (decision 0017). The image must outlive the kernel.
    void set_disc_files(const DiscFiles* files) noexcept;
    [[nodiscard]] const DiscFiles* disc_files() const noexcept;

    // The disc's raw bytes for the game's own CD driver (the PCDV read
    // service): its requests carry an LBA, a byte count and an EE
    // destination, and the model copies the sectors straight from the
    // image, exactly as the drive would (decision 0019). Null means no
    // disc: reads answer nothing. The source must outlive the kernel. A
    // DiscSectors source presents the logical blocks of a dual-layer disc;
    // the requests name logical blocks and the source maps them.
    void set_disc_sectors(const DiscByteSource* sectors) noexcept;
    [[nodiscard]] const DiscByteSource* disc_sectors() const noexcept;

    // Model introspection for tests and tools.
    [[nodiscard]] std::uint32_t pending_interrupts() const noexcept;
    // The deferred-call stack depth (patched syscalls and active handlers).
    [[nodiscard]] std::size_t deferred_call_count() const noexcept;
    // The model IOP's function behavior: each known (server, function) pair
    // answers the bytes the game's own check consumes (the status queries
    // and version negotiations of decision 0014); anything else answers an
    // empty result. Exposed for unit tests.
    [[nodiscard]] std::uint32_t sif_rpc_result(GuestState& state,
                                               std::uint32_t sid,
                                               std::uint32_t rpc_number,
                                               std::uint8_t* result,
                                               std::uint32_t capacity);
    // Answers the file server's open (sid 0x80000006, RPC 0) from the disc
    // image: the request's path at +8, the reply {handle, size} (decision
    // 0017). Returns the reply's length in bytes. Exposed for unit tests.
    [[nodiscard]] std::uint32_t answer_file_open(GuestState& state,
                                                 std::uint32_t request,
                                                 std::uint32_t request_size,
                                                 std::uint8_t* result,
                                                 std::uint32_t capacity);
    // Answers the game's own CD read (sid 0x50434456, RPC 3): the request is
    // {LBA, byte count, EE destination}; the sectors come from the disc
    // image (decision 0019). Returns the byte count transferred. Exposed for
    // unit tests.
    [[nodiscard]] std::uint32_t answer_disc_read(GuestState& state,
                                                 std::uint32_t request);
    // Answers the game's own block cache (sid 0x53545250, RPC 3): the
    // request is {LBA, byte count, flags}. The cache reads the sectors from
    // the disc image and keeps them under a fresh handle, which is the
    // reply — the client copies the block out with RPC 4/7 and never sees
    // the bytes here. Returns the handle (zero when the request is invalid
    // or no disc is set). Exposed for unit tests.
    [[nodiscard]] std::uint32_t answer_prts_read(GuestState& state,
                                                 std::uint32_t request);
    // Answers the block cache's copy-out (sid 0x53545250, RPC 4 and 7): the
    // request is {handle, EE destination, byte count}. The cached block's
    // bytes are copied out sequentially from the handle's cursor (clamped
    // to what remains); the reply carries no data. Returns the bytes
    // copied. Exposed for unit tests.
    [[nodiscard]] std::uint32_t answer_prts_copy(GuestState& state,
                                                 std::uint32_t request);
    // Snapshots the whole kernel state (threads with their contexts,
    // semaphores, syscall patches, OSD, deferred calls, SIF/RPC tables,
    // clocks, disc handles, PRTS blocks with their copy-out cursors, and
    // every counter) into a versioned blob; load restores it. Anything
    // malformed throws. Pointers are policy, not state: the disc sources
    // and the service table are relinked identically on both sides and
    // never serialized. Exposed for unit tests and the checkpoint tooling.
    [[nodiscard]] std::vector<std::uint8_t> save_kernel_state() const;
    void load_kernel_state(std::span<const std::uint8_t> bytes);
    // Answers the game's CD driver volume registration (sid 0x50434456,
    // RPC 2): the request is {block, checksum}, where the checksum is the
    // library's index-weighted byte sum (0x00548D20) over the 0x800-byte
    // block its scan accepted as the volume descriptor. The model recomputes
    // it from the same image the reads come from and stops loudly on a
    // mismatch, then records the block. Returns the bytes consumed. Exposed
    // for unit tests.
    [[nodiscard]] std::uint32_t answer_disc_volume(GuestState& state,
                                                   std::uint32_t request);
    // Answers the game's CD driver volume query (sid 0x50434456, RPC 4):
    // the reply is {status, value}, where the value is the registered
    // volume's "volume space size" read from the image (the engine stores
    // it at [task+0xEC] and uses it as the logical block where the next
    // volume begins, so the pinned disc's second layer is found through it).
    // Without a disc or a registration the reply is zeros, which the library
    // reports as a failure and the engine retries. Returns the reply's
    // length in bytes. Exposed for unit tests.
    [[nodiscard]] std::uint32_t answer_disc_volume_size(std::uint8_t* result,
                                                        std::uint32_t capacity) const;
    [[nodiscard]] std::uint32_t sif_register_index_address(std::uint32_t index) const noexcept;
    // The IOP image path named by the last reset command, empty when none.
    [[nodiscard]] const std::string& sif_iop_image() const noexcept;
    // Interrupt and DMA handler registrations (0x10-0x17 and the negative
    // i* aliases). Add/Remove store the registration. Enable/Disable set or
    // clear the cause's mask bit in the wired unit (the privileged path to
    // the same mask the guest toggles by writing INTC_MASK/D_STAT); without
    // a wired unit they are accepted with no effect.
    ServiceOutcome add_intc_handler(GuestState& state);     // 0x10
    ServiceOutcome remove_intc_handler(GuestState& state);  // 0x11
    ServiceOutcome add_dmac_handler(GuestState& state);     // 0x12
    ServiceOutcome remove_dmac_handler(GuestState& state);  // 0x13
    ServiceOutcome enable_intc(GuestState& state);          // 0x14
    ServiceOutcome disable_intc(GuestState& state);         // 0x15
    ServiceOutcome enable_dmac(GuestState& state);          // 0x16
    ServiceOutcome disable_dmac(GuestState& state);         // 0x17

    // Model introspection for tests and tools.
    [[nodiscard]] std::uint32_t current_thread_id() const noexcept;
    [[nodiscard]] const std::vector<KernelThread>& threads() const noexcept;
    [[nodiscard]] const std::vector<KernelSemaphore>& semaphores() const noexcept;
    // The SIF RPC servers the game has bound, sorted by sid.
    [[nodiscard]] std::vector<std::uint32_t> sif_server_sids() const;
    [[nodiscard]] const std::vector<KernelInterruptHandler>& interrupt_handlers() const noexcept;
    [[nodiscard]] const std::vector<KernelInterruptHandler>& dmac_handlers() const noexcept;
    // The guest handler a SetSyscall installed for the number, or zero.
    [[nodiscard]] std::uint32_t patched_handler(std::uint32_t number) const noexcept;
    // The OSD configuration word the services read and write.
    [[nodiscard]] std::uint32_t osd_config() const noexcept;

    // The model's synthetic syscall table lives at this physical address, in
    // the zero-filled low RAM the SDK's kernel search scans. The game only
    // derives the address by searching, so its exact location is free; the
    // entries are opaque kernel-range tokens the search never matches, except
    // the two the boot patches before searching.
    static constexpr std::uint32_t syscall_table_physical = 0x1000;
    static constexpr std::uint32_t syscall_table_entries = 256;
    // Every entry starts as one opaque token in the kernel segment. The
    // boot's GetEntryAddress re-installs what it read; a handler equal to
    // the number's own token means "the model's own entry" and drops any
    // patch instead of jumping to a token address.
    static constexpr std::uint32_t syscall_token_base = 0x80010000;
    // A patched handler returns through this stub: it issues the model's
    // private return service, which restores the caller's ra and the
    // instruction after the syscall. On real hardware the kernel dispatcher
    // returns through EPC; the stub is this model's equivalent.
    static constexpr std::uint32_t patch_return_stub_physical = 0x1600;
    static constexpr std::uint32_t patch_return_service = 0x100;
    // The thread id observed when no thread runs. The EE kernel's thread
    // 0 is always its idle thread (ps2sdk ee/kernel/include/kernel.h,
    // MAX_THREADS comment), and the game's own ids start at 1, so 0 is
    // unambiguous. The kernel reports it from GetThreadId while an
    // interrupt runs over idle and records it on the in-flight call; the
    // game's safe-wakeup wrapper compares iGetThreadId against its live
    // target, so idle must never look like a blocked thread (decision
    // 0031).
    static constexpr std::uint32_t idle_thread_id = 0;
    // VBlank delivery: the cause the idle source raises (INTC status bit 2,
    // the public INTC_VBLANK_S), its status register, and the budget of
    // consecutive idle interrupts without a runnable thread.
    static constexpr std::uint32_t vblank_cause = 2;
    static constexpr std::uint32_t intc_stat_physical = 0x1000F000;
    // The idle budget is this model's own stuck-machine guard (hardware
    // keeps delivering frames); decision 0024 sizes it 10x past the
    // measured delay maturation (~202k deliveries), because the slice-5
    // poke showed the boot's delays maturing just past the old 200,000.
    static constexpr std::uint32_t idle_interrupt_budget = 2000000;
    // The EE timers: four register blocks at 0x10000000 + index * 0x800,
    // COUNT at +0x00, MODE at +0x10, COMP at +0x20. COUNT and COMP are
    // 16-bit logical counters (see TimerUnit); the MODE bits below name the
    // enables and the edge-triggered flags. Both quanta advance through
    // the one advance_busclk machine: each handled service contributes
    // one millisecond of BUSCLK ticks (decision 0016) and each idle call
    // one frame, sharing remainders and the VBlank accumulator
    // (decision 0030). A flag edge reports the timer's INTC cause
    // (9/10/11/12 for T0/T1/T2/T3).
    static constexpr std::uint32_t timer_window_physical = 0x10000000;
    static constexpr std::uint32_t timer_stride = 0x800;
    static constexpr std::uint32_t timer_count_offset = 0x00;
    static constexpr std::uint32_t timer_mode_offset = 0x10;
    static constexpr std::uint32_t timer_compare_offset = 0x20;
    static constexpr std::uint32_t timer_count_enable = 0x00000080;  // CUE
    static constexpr std::uint32_t timer_compare_enable = 0x00000100;  // CMPE
    static constexpr std::uint32_t timer_overflow_enable = 0x00000200;  // OVFE
    static constexpr std::uint32_t timer_compare_flag = 0x00000400;  // EQUF
    static constexpr std::uint32_t timer_overflow_flag = 0x00000800;  // OVFF
    // The CP0 Status bits gating interrupt delivery (PS2tek EE COP0
    // Exception Handling): IE, EIE, EXL, ERL for the master gate, INT0 for
    // the INTC path and INT1 for the DMAC path.
    static constexpr std::uint32_t cp0_status_ie = 0x00000001;
    static constexpr std::uint32_t cp0_status_exl = 0x00000002;
    static constexpr std::uint32_t cp0_status_erl = 0x00000004;
    static constexpr std::uint32_t cp0_status_int0 = 0x00000400;
    static constexpr std::uint32_t cp0_status_int1 = 0x00000800;
    static constexpr std::uint32_t cp0_status_eie = 0x00010000;
    // BUSCLK ticks per frame and per service slice: the slice is one
    // millisecond, the unit the game's delay library schedules in (its timer
    // nodes' base values are BUSCLK ticks of elapsed time; slice 14's
    // evidence). One VBlank per frame of accumulated slices.
    static constexpr std::uint32_t busclk_per_frame = 2457600;
    static constexpr std::uint32_t service_time_slice = 147456;
    // The DMAC's status register: one bit per channel; a completion sets the
    // channel's bit and the handler clears it by writing back.
    static constexpr std::uint32_t dmac_stat_physical = 0x1000E010;
    // The SIF register block: hardware indices 1-4 map to
    // SIF_MSCOM/SMCOM/MSFLG/SMFLG at 0x1000F200 + (index-1)*0x10; the
    // software system registers (0x80000000+) live in the kernel. The
    // model's IOP is initialized: SMFLG reports SIFINIT|CMDINIT|BOOTEND and
    // SMCOM holds a plausible shared command-buffer address.
    static constexpr std::uint32_t sif_register_physical = 0x1000F200;
    static constexpr std::uint32_t sif_chcr_physical = 0x1000C000;
    static constexpr std::uint32_t sif_channel_dmac = 5;
    static constexpr std::uint32_t sif_mesg_init = 0x70000;  // SIFINIT|CMDINIT|BOOTEND
    static constexpr std::uint32_t sif_iop_command_buffer = 0x00080000;
    static constexpr std::uint32_t sif_command_cid_set_sreg = 0x80000001;
    static constexpr std::uint32_t sif_command_cid_init_cmd = 0x80000002;
    static constexpr std::uint32_t sif_command_cid_reset_cmd = 0x80000003;
    static constexpr std::uint32_t sif_command_cid_rpc_end = 0x80000008;
    static constexpr std::uint32_t sif_command_cid_rpc_bind = 0x80000009;
    static constexpr std::uint32_t sif_command_cid_rpc_call = 0x8000000A;
    // The first originating event (decision 0026): one synthesized SIF
    // pump packet per boot. The pump reads its queue pointer from the
    // game's word below; the packet matches what the pump's table
    // dispatches to the SET_SREG register writer (count byte, then
    // words {0, 1, 0, register, value}). Delivery also waits for the
    // game's dispatch entry, so the first packet is never sent into an
    // unpopulated table.
    static constexpr std::uint32_t originating_queue_pointer = 0x00886818;
    static constexpr std::uint32_t originating_dispatch_table = 0x00886824;
    static constexpr std::uint32_t originating_dispatch_entry = 12;
    static constexpr std::uint32_t originating_packet_count = 0x18;
    static constexpr std::uint32_t originating_packet_register = 1;
    static constexpr std::uint32_t originating_packet_value = 1;
    static constexpr std::uint32_t sif_sreg_rpcinit = 0;
    // The model IOP's scratch region for RPC server state. These are model
    // addresses below the game's image (which starts at 0x00100000), inside
    // the kernel's zero-filled low RAM; the EE never allocates there. The
    // 0x00080000 command-buffer address sits between the buffers and the
    // connections regions.
    static constexpr std::uint32_t sif_iop_server_handles = 0x00020000;
    static constexpr std::uint32_t sif_iop_server_buffers = 0x00030000;
    static constexpr std::uint32_t sif_iop_server_connections = 0x00090000;
    static constexpr std::uint32_t sif_iop_server_stride = 0x1000;
    static constexpr std::uint32_t sif_iop_server_capacity = 80;

private:
    [[nodiscard]] KernelThread* find_thread(std::uint32_t id) noexcept;
    [[nodiscard]] KernelSemaphore* find_semaphore(std::uint32_t id) noexcept;
    [[nodiscard]] KernelThread* current_thread() noexcept;
    [[nodiscard]] KernelThread* pick_next_ready() noexcept;
    // Saves the running thread as it resumes after its syscall (pc + 4, with
    // v0 as the handler set it), marks it waiting, then dispatches. False
    // when no thread can run.
    bool block_current(GuestState& state, std::uint32_t wait_type,
                       std::uint32_t wait_id, std::uint32_t return_value);
    // Switches to the best ready thread; false when none. Every kernel
    // switch happens inside a syscall, so the running thread is saved with
    // pc + 4. A failed switch names the idle thread explicitly, so the
    // machine never keeps a blocked thread's id as current (decision
    // 0031); the return path and GetThreadId read the same sentinel.
    bool dispatch(GuestState& state);
    // Dispatches when a ready thread strictly outranks the running one.
    bool preempt_if_outranked(GuestState& state);
    // True while an injected handler has not returned through the stub; the
    // kernel does not nest injections and defers switches until it returns.
    [[nodiscard]] bool handler_active() const noexcept;
    // The model's private return service: a patched handler or an injected
    // interrupt handler returns through the stub, which issues this number;
    // here the saved state is restored.
    ServiceOutcome deferred_return(GuestState& state);
    // Errors the kernel reports as -1 in v0, like the public ABI's negative
    // error codes.
    static void write_error(GuestState& state);
    // Fills the synthetic syscall table with one token per number the first
    // time it is needed.
    void ensure_syscall_table(GuestState& state);
    // Seeds the model's initialized-IOP register values the first time a SIF
    // service runs.
    void ensure_sif_ready(GuestState& state);
    // Appends a pending interrupt cause; the driver delivers it through
    // start_interrupt at the next unit boundary. Repeats coalesce in place
    // (a queued cause stays where it is, like the hardware status bit the
    // handler reads and clears), so distinct causes keep their relative
    // order. The occurrence also sets the cause's status bit in the wired
    // INTC unit, whether or not a handler is registered.
    void queue_interrupt(std::uint32_t cause);
    // Appends a pending DMAC channel completion; the channel's registered
    // handlers run when the completion is dispatched. Repeats coalesce in
    // place like INTC causes (one status bit per channel), and the
    // occurrence sets the channel's CIS bit in the wired DMAC unit whether
    // or not a handler is registered.
    void queue_dmac_completion(std::uint32_t channel);
    // Saves the live context on the deferred stack and installs the first
    // handler's frame; shared by the queued and idle interrupt sources. The
    // handler chain runs each registered handler for the cause in turn,
    // each with the argument its own registration carried.
    bool inject_interrupt(GuestState& state, std::uint32_t cause,
                          std::vector<InterruptHandlerCall> calls);
    // Advances the enabled EE timers by one idle frame through the unified
    // machine above (one frame of BUSCLK ticks).
    void advance_timers(GuestState& state);
    // Writes one SET_SREG pump packet and queues its DMAC completion the
    // first time an idle tick finds the pump handler registered, the
    // queue empty, and the game's dispatch entry populated (decision
    // 0026). Pure function of kernel and guest state, so both engines
    // inject at the same boundary.
    void maybe_send_originating_packet(GuestState& state);
    // Installs one handler call frame over the saved interrupted context:
    // (a0, a1, a2) = (cause, registration argument, interrupted pc).
    void install_handler_frame(GuestState& state, std::uint32_t cause,
                               const InterruptHandlerCall& call,
                               const RegisterContext& interrupted);
    // Byte copy inside the guest memory, used by the synchronous SIF DMA.
    void copy_guest_bytes(GuestState& state, std::uint32_t source,
                          std::uint32_t destination, std::uint32_t size);
    // The model IOP stub: inspects one transferred SIFCMD packet and answers
    // the commands it recognizes (currently the SIFCMD init handshake).
    void run_iop_stub(GuestState& state, std::uint32_t command_buffer,
                      std::uint32_t size);
    // The model IOP's RPC server table: one slot per server the game binds.
    struct SifRpcServer {
        std::uint32_t sid = 0;
        std::uint32_t handle = 0;
        std::uint32_t buffer = 0;
        std::uint32_t connection_buffer = 0;
    };
    [[nodiscard]] SifRpcServer& find_or_create_sif_server(std::uint32_t sid);
    [[nodiscard]] const SifRpcServer* find_sif_server_by_handle(
        std::uint32_t handle) const noexcept;
    // Answers one `SIF_CMD_RPC_BIND` request with the SIFRPC end packet the
    // game's client waits for.
    void answer_sif_rpc_bind(GuestState& state, std::uint32_t command_buffer,
                             std::uint32_t size);
    // Answers one `SIF_CMD_RPC_CALL` request: runs the model's function for
    // the (server, rpc number) pair, transfers the result into the caller's
    // receive buffer and sends the SIFRPC end packet.
    void answer_sif_rpc_call(GuestState& state, std::uint32_t command_buffer,
                             std::uint32_t size);
    // Handles `SIF_CMD_RESET_CMD`: records the requested IOP image and
    // completes the modeled reboot by announcing SIFINIT|CMDINIT|BOOTEND in
    // SMFLAG, the bits the game's boot-completion wait polls. The game
    // overwrites SMFLAG right after sending the reset (its own protocol
    // writes), so the completion is one-shot applied before the first
    // register read that follows.
    void answer_sif_reset(GuestState& state, std::uint32_t command_buffer,
                          std::uint32_t size);
    // Handles `SIF_CMD_SET_SREG`: mirrors the register back to the EE, the
    // acknowledgement the game's command-layer init spins on (decision 0015).
    void answer_sif_set_sreg(GuestState& state, std::uint32_t command_buffer,
                             std::uint32_t size);

    std::vector<KernelThread> threads_;
    std::vector<KernelSemaphore> semaphores_;
    std::uint32_t next_thread_id_ = 1;
    // Semaphore ids carry bits 0 and 1 set. Evidence: the game's timer
    // library ORs 0x2 into the "common" value it was handed (a semaphore id)
    // and tests bit 0 of the same value to decide whether to activate the
    // node (0x005B8B68). Both operations are only harmless when the kernel's
    // handle already has those bits, so the model hands out ids that do:
    // 3, 7, 11, ... The exact real-kernel handle format is not documented in
    // the pinned sources; this is the shape the game's own code requires.
    std::uint32_t next_semaphore_id_ = 3;
    std::uint32_t current_thread_id_ = 0;  // 0 = no thread has run yet
    ServiceTable* service_table_ = nullptr;  // set by register_services
    bool syscall_table_ready_ = false;
    std::array<std::uint32_t, syscall_table_entries> patched_handlers_{};
    // spdifMode=1 (disabled), screenType=0 (4:3), videoOutput=0 (RGB),
    // japLanguage=1 (non-Japanese), ps1drvConfig=0, version=1 (OSD2),
    // language=1 (English), timezoneOffset=0. See decision 0008.
    std::uint32_t osd_config_ = 0x00012011u;
    // ConfigParam2 (four bytes): format 0, daylightSavings 0 (winter),
    // timeFormat 0 (24-hour), dateFormat 0, version 2 (OSD2 with extended
    // languages), language 1 (English, matching osd_config_). The game's
    // boot reads the daylight-savings bit for its timezone; the reference
    // emulator fills this block from the console's language parameters.
    std::array<std::uint8_t, 4> osd_config2_{0x00u, 0x00u, 0x02u, 0x01u};
    // The GS interrupt mask the syscalls keep; zero until the game sets it.
    std::uint64_t gs_imr_ = 0;
    // One entry per deferred guest call in flight (a patched syscall or an
    // injected interrupt handler); nested calls are a stack, exactly like
    // the handlers' returns. A patch saves the caller's ra and the resume
    // address; an interrupt saves the whole interrupted context.
    struct DeferredCall {
        enum class Kind { Patch, Interrupt };
        Kind kind = Kind::Patch;
        std::uint32_t resume_pc = 0;
        std::uint32_t caller_ra = 0;
        // The interrupted thread (interrupts), or idle_thread_id when the
        // injection found no running thread. The return path dispatches
        // from the idle sentinel instead of restoring a waiter as RUN.
        std::uint32_t thread_id = 0;
        std::uint32_t cause = 0;      // the interrupt cause (interrupts)
        // The registered handler calls for the cause, in registration
        // order; the kernel calls them all, one after the next, each
        // framed with its own argument.
        std::vector<InterruptHandlerCall> handlers;
        std::size_t next_handler = 0;
        RegisterContext context;
    };
    std::vector<DeferredCall> deferred_calls_;
    std::uint32_t next_handler_id_ = 1;
    // The SIF layer: software system registers, the model IOP's pending
    // interrupts and the transfer id handed out by SifSetDma.
    std::map<std::uint32_t, std::uint32_t> sif_software_registers_;
    // One pending interrupt: an INTC cause or a DMAC channel completion.
    struct InterruptRequest {
        enum class Kind { Intc, Dmac };
        Kind kind = Kind::Intc;
        std::uint32_t number = 0;
    };
    std::vector<InterruptRequest> interrupt_queue_;
    std::vector<KernelInterruptHandler> interrupt_handlers_;  // INTC causes
    std::vector<KernelInterruptHandler> dmac_handlers_;       // DMA channels
    // The device units behind the MMIO windows, wired by the boot after
    // construction (null until then). The timer owns counting and flags;
    // the INTC unit owns STAT/MASK; the DMAC unit owns CIS/CIM. The kernel
    // never writes their guest paths to change pending state: occurrences
    // use the internal setters, acknowledges belong to the guest.
    TimerUnit* timer_unit_ = nullptr;
    IntcUnit* intc_unit_ = nullptr;
    DmacStatusUnit* dmac_unit_ = nullptr;
    // True when the CP0 master gate and the path's interrupt line allow
    // the CPU to take an interrupt now (PS2tek Status contract).
    [[nodiscard]] static bool cpu_gate_allows(const GuestState& state,
                                              bool is_dmac) noexcept;
    // True when the cause's mask bit allows dispatch. Without a wired
    // unit the check passes (the P00-era degraded behavior).
    [[nodiscard]] bool intc_mask_allows(std::uint32_t cause) const noexcept;
    [[nodiscard]] bool dmac_mask_allows(std::uint32_t channel) const noexcept;
    std::uint32_t next_dma_id_ = 1;
    bool sif_ready_ = false;
    // The EE's SIFCMD receive buffer, learned from the init handshake; the
    // model IOP writes its command replies there.
    std::uint32_t ee_command_buffer_ = 0;
    std::map<std::uint32_t, SifRpcServer> sif_rpc_servers_;
    std::string sif_iop_image_;
    // True between a reset command and the first following register read:
    // the model IOP's reboot completes there (see answer_sif_reset).
    bool sif_reboot_pending_ = false;
    // Whether the one-shot originating packet was delivered (decision
    // 0026). Snapshotted below so a resumed boot never replays it.
    bool originating_packet_sent_ = false;
    // Consecutive idle interrupts without a runnable thread (progress
    // resets the count). The budget only bounds a truly stuck machine; idle
    // interrupts are cheap, so it allows long waits (about an hour of
    // virtual frames) before the driver reports the boundary.
    std::uint32_t idle_interrupts_ = 0;
    // The unified time base both quanta share (decision 0030): BUSCLK
    // ticks accumulated toward the next VBlank, and the per-timer
    // fractional remainders of the CLKS division. The snapshot keeps the
    // model-2 wire order (one accumulator word, then four remainder
    // words); only the meaning widened from service-only to both paths,
    // which is why time_model is 3.
    std::uint32_t busclk_accumulator_ = 0;
    std::uint32_t timer_remainders_[4] = {};
    // The disc image (null without one) and the files the file server has
    // handed out: handle -> path. Handle 0 is the "not found" answer.
    const DiscFiles* disc_files_ = nullptr;
    // The disc's raw byte source for the PCDV read service (decision 0019).
    const DiscByteSource* disc_sectors_ = nullptr;
    // The volume descriptor block the game's CD library registered (RPC 2);
    // zero when none has been registered.
    std::uint32_t disc_volume_lba_ = 0;
    std::map<std::uint32_t, std::string> disc_files_by_handle_;
    std::uint32_t next_disc_handle_ = 1;
    // The game's own block cache (the PRTS server, sid 0x53545250): the
    // client asks for a disc block with one call and copies it out with the
    // next, so the model keeps the blocks it read under the handle it
    // answered. The client copies each block out right after reading it, so
    // a handful of entries covers the streaming reads.
    struct PrtsBlock {
        std::uint32_t lba = 0;
        std::vector<std::uint8_t> data;
        // The copy-out cursor: the client reads each block sequentially in
        // fixed-size chunks (the font load streams 0x4000-byte chunks from
        // one block into alternating buffers), and the request carries no
        // offset — so the server tracks how far each handle was consumed.
        std::uint32_t cursor = 0;
    };
    std::map<std::uint32_t, PrtsBlock> prts_blocks_;
    std::uint32_t next_prts_handle_ = 1;
};

} // namespace gt4recomp::ee
