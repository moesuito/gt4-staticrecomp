#pragma once

// The EE kernel model: threads and semaphores with the deterministic
// cooperative scheduler of docs/decisions/0005-thread-scheduler.md. The
// kernel owns the thread and semaphore tables; the register context of the
// running thread lives in GuestState, and a switch saves it into the
// thread's slot and restores the next thread's. There is no timer
// preemption: a thread runs until it blocks, or until a service makes a
// strictly higher-priority thread ready.

#include "gt4recomp/ee_services.hpp"
#include "gt4recomp/ee_state.hpp"

#include <array>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

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
// registration so removal and re-registration behave; no interrupt is ever
// delivered (documented in docs/decisions/0007-timer-registers.md).
struct KernelInterruptHandler {
    std::uint32_t id = 0;
    std::uint32_t cause = 0;
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
    // SetGsCrt(interlace, video, field): the model has no display, so the
    // call is accepted with no state; the game only needs it to return.
    ServiceOutcome set_gs_crt(GuestState& state);          // 0x02

    // Interrupt injection: the driver calls this at unit boundaries. It
    // saves the running context, sets up the registered handler's call
    // frame (a0 = cause, returning through the model's stub), and returns
    // true while the handler context is live. False when none is pending.
    [[nodiscard]] bool start_interrupt(GuestState& state);

    // The idle-time interrupt source: when no thread can run, the model
    // delivers one VBlank interrupt (the periodic source of a real console)
    // if the game registered a handler for it. The handler may wake
    // threads; its return re-dispatches. A bounded budget of consecutive
    // idle interrupts without a runnable thread stops the model instead of
    // spinning forever.
    [[nodiscard]] bool deliver_idle_interrupt(GuestState& state);

    // Model introspection for tests and tools.
    [[nodiscard]] std::uint32_t pending_interrupts() const noexcept;
    [[nodiscard]] std::uint32_t sif_register_index_address(std::uint32_t index) const noexcept;
    // The IOP image path named by the last reset command, empty when none.
    [[nodiscard]] const std::string& sif_iop_image() const noexcept;
    // Interrupt and DMA handler registrations (0x10-0x17 and the negative
    // i* aliases). The model stores them; enable/disable are accepted with
    // no effect because no interrupt is delivered.
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
    [[nodiscard]] const std::vector<KernelInterruptHandler>& interrupt_handlers() const noexcept;
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
    // VBlank delivery: the cause the idle source raises (INTC status bit 2,
    // the public INTC_VBLANK_S), its status register, and the budget of
    // consecutive idle interrupts without a runnable thread.
    static constexpr std::uint32_t vblank_cause = 2;
    static constexpr std::uint32_t intc_stat_physical = 0x1000F000;
    static constexpr std::uint32_t idle_interrupt_budget = 60;
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
    static constexpr std::uint32_t sif_sreg_rpcinit = 0;
    // The model IOP's scratch region for RPC server state. These are model
    // addresses below the game's image (which starts at 0x00100000), inside
    // the kernel's zero-filled low RAM; the EE never allocates there.
    static constexpr std::uint32_t sif_iop_server_handles = 0x000A0000;
    static constexpr std::uint32_t sif_iop_server_buffers = 0x000B0000;
    static constexpr std::uint32_t sif_iop_server_connections = 0x000C0000;
    static constexpr std::uint32_t sif_iop_server_stride = 0x1000;
    static constexpr std::uint32_t sif_iop_server_capacity = 16;

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
    // pc + 4.
    bool dispatch(GuestState& state);
    // Dispatches when a ready thread strictly outranks the running one.
    bool preempt_if_outranked(GuestState& state);
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
    // Appends a pending SIF interrupt; the driver delivers it through
    // start_interrupt at the next unit boundary.
    void queue_interrupt(std::uint32_t cause);
    // Saves the live context on the deferred stack and installs the first
    // handler's frame; shared by the queued and idle interrupt sources. The
    // handler chain runs each registered handler for the cause in turn.
    bool inject_interrupt(GuestState& state, std::uint32_t cause,
                          std::vector<std::uint32_t> handlers);
    // Installs one handler call frame over the saved interrupted context.
    void install_handler_frame(GuestState& state, std::uint32_t cause,
                               std::uint32_t handler,
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
    // The model IOP's function behavior. Known pairs get their documented
    // answer bytes; anything else answers an error-shaped empty result.
    [[nodiscard]] std::uint32_t sif_rpc_result(std::uint32_t sid,
                                               std::uint32_t rpc_number,
                                               std::uint8_t* result,
                                               std::uint32_t capacity);

    std::vector<KernelThread> threads_;
    std::vector<KernelSemaphore> semaphores_;
    std::uint32_t next_thread_id_ = 1;
    std::uint32_t next_semaphore_id_ = 1;
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
        std::uint32_t thread_id = 0;  // the interrupted thread (interrupts)
        std::uint32_t cause = 0;      // the interrupt cause (interrupts)
        // The registered handlers for the cause, in registration order; the
        // kernel calls them all, one after the next.
        std::vector<std::uint32_t> handlers;
        std::size_t next_handler = 0;
        RegisterContext context;
    };
    std::vector<DeferredCall> deferred_calls_;
    std::vector<KernelInterruptHandler> interrupt_handlers_;
    std::uint32_t next_handler_id_ = 1;
    // The SIF layer: software system registers, the model IOP's pending
    // interrupts and the transfer id handed out by SifSetDma.
    std::map<std::uint32_t, std::uint32_t> sif_software_registers_;
    std::vector<std::uint32_t> interrupt_queue_;
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
    // Consecutive idle interrupts without a runnable thread (progress
    // resets the count).
    std::uint32_t idle_interrupts_ = 0;
};

} // namespace gt4recomp::ee
