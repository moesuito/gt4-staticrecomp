#include "gt4recomp/ee_kernel.hpp"

#include "gt4recomp/disc_image.hpp"
#include "gt4recomp/ee_compare.hpp"

#include <cstdio>
#include <stdexcept>
#include <utility>

namespace gt4recomp::ee {
namespace {

constexpr std::uint32_t error_code = 0xFFFFFFFFu;

// The public ee_thread_t / ee_thread_status_t layout (ps2sdk kernel.h), in
// bytes from the structure pointer.
constexpr std::uint32_t thread_status = 0x00;
constexpr std::uint32_t thread_func = 0x04;
constexpr std::uint32_t thread_stack = 0x08;
constexpr std::uint32_t thread_stack_size = 0x0C;
constexpr std::uint32_t thread_gp = 0x10;
constexpr std::uint32_t thread_initial_priority = 0x14;
constexpr std::uint32_t thread_current_priority = 0x18;
constexpr std::uint32_t thread_attr = 0x1C;
constexpr std::uint32_t thread_option = 0x20;
constexpr std::uint32_t thread_wait_type = 0x24;
constexpr std::uint32_t thread_wait_id = 0x28;
constexpr std::uint32_t thread_wakeup_count = 0x2C;
constexpr std::uint32_t thread_status_size = 0x30;
constexpr std::uint32_t thread_create_size = 0x24;

// The public ee_sema_t layout, in bytes from the structure pointer.
constexpr std::uint32_t sema_count = 0x00;
constexpr std::uint32_t sema_max_count = 0x04;
constexpr std::uint32_t sema_init_count = 0x08;
constexpr std::uint32_t sema_wait_threads = 0x0C;
constexpr std::uint32_t sema_attr = 0x10;
constexpr std::uint32_t sema_option = 0x14;
constexpr std::uint32_t sema_size = 0x18;

} // namespace

void Kernel::write_error(GuestState& state) {
    state.write_gpr64(2, error_code);
}

KernelThread* Kernel::find_thread(std::uint32_t id) noexcept {
    for (KernelThread& thread : threads_) {
        if (thread.id == id) {
            return &thread;
        }
    }
    return nullptr;
}

KernelSemaphore* Kernel::find_semaphore(std::uint32_t id) noexcept {
    for (KernelSemaphore& semaphore : semaphores_) {
        if (semaphore.id == id) {
            return &semaphore;
        }
    }
    return nullptr;
}

KernelThread* Kernel::current_thread() noexcept {
    return find_thread(current_thread_id_);
}

KernelThread* Kernel::pick_next_ready() noexcept {
    KernelThread* best = nullptr;
    for (KernelThread& thread : threads_) {
        if ((thread.status & ThreadReady) == 0 || (thread.status & ThreadSuspend) != 0) {
            continue;
        }
        if (best == nullptr || thread.current_priority < best->current_priority) {
            best = &thread;
        }
    }
    return best;
}

bool Kernel::dispatch(GuestState& state) {
    KernelThread* next = pick_next_ready();
    if (next == nullptr) {
        // No thread can run: the machine is explicitly idle. The idle
        // sentinel is recorded here, at the single choke point every
        // block/exit path funnels through -- never as an isolated zeroing,
        // because the interrupt return and GetThreadId read the same
        // sentinel to tell idle from a blocked thread (decision 0031).
        current_thread_id_ = idle_thread_id;
        return false;
    }
    if (KernelThread* current = current_thread();
        current != nullptr && current->status == ThreadRun) {
        // Every kernel switch happens inside a syscall: the thread resumes
        // at the instruction after it, with v0 as the handler left it.
        current->context = state.save_registers();
        current->context.pc += 4;
        current->status = (current->status & ~ThreadRun) | ThreadReady;
    }
    current_thread_id_ = next->id;
    next->status = (next->status & ~ThreadReady) | ThreadRun;
    idle_interrupts_ = 0;  // a runnable thread appeared: progress
    state.restore_registers(next->context);
    return true;
}

bool Kernel::preempt_if_outranked(GuestState& state) {
    if (handler_active()) {
        // A handler is running: the kernel defers thread switches until it
        // returns (the interrupted context is restored there).
        return false;
    }
    KernelThread* current = current_thread();
    KernelThread* next = pick_next_ready();
    if (next == nullptr) {
        return false;
    }
    if (current != nullptr && current->status == ThreadRun
        && next->current_priority >= current->current_priority) {
        return false;  // only a strictly higher priority takes the CPU
    }
    return dispatch(state);
}

bool Kernel::handler_active() const noexcept {
    return !deferred_calls_.empty()
        && deferred_calls_.back().kind == DeferredCall::Kind::Interrupt;
}

bool Kernel::block_current(GuestState& state, std::uint32_t wait_type,
                           std::uint32_t wait_id, std::uint32_t return_value) {
    KernelThread* current = current_thread();
    if (current == nullptr) {
        return false;
    }
    state.write_gpr64(2, return_value);
    current->context = state.save_registers();
    current->context.pc += 4;
    current->status = (current->status & ~ThreadRun) | ThreadWait;
    current->wait_type = wait_type;
    current->wait_id = wait_id;
    return dispatch(state);
}

void Kernel::register_services(ServiceTable& services) {
    service_table_ = &services;
    const auto add = [this, &services](
                         std::uint32_t number,
                         ServiceOutcome (Kernel::*method)(GuestState&)) {
        services.add(number, [this, method](GuestState& state) {
            return (this->*method)(state);
        });
    };
    add(0x20u, &Kernel::create_thread);
    add(0x21u, &Kernel::delete_thread);
    add(0x22u, &Kernel::start_thread);
    add(0x23u, &Kernel::exit_thread);
    add(0x24u, &Kernel::exit_delete_thread);
    add(0x25u, &Kernel::terminate_thread);
    add(static_cast<std::uint32_t>(-0x26), &Kernel::terminate_thread);
    add(0x29u, &Kernel::change_thread_priority);
    add(static_cast<std::uint32_t>(-0x2A), &Kernel::change_thread_priority);
    add(0x2Bu, &Kernel::rotate_ready_queue);
    add(static_cast<std::uint32_t>(-0x2C), &Kernel::rotate_ready_queue);
    add(0x2Du, &Kernel::release_wait_thread);
    add(static_cast<std::uint32_t>(-0x2E), &Kernel::release_wait_thread);
    add(0x2Fu, &Kernel::get_thread_id);
    add(static_cast<std::uint32_t>(-0x2F), &Kernel::get_thread_id);
    add(0x30u, &Kernel::refer_thread_status);
    add(static_cast<std::uint32_t>(-0x31), &Kernel::refer_thread_status);
    add(0x32u, &Kernel::sleep_thread);
    add(0x33u, &Kernel::wakeup_thread);
    add(static_cast<std::uint32_t>(-0x34), &Kernel::wakeup_thread);
    add(0x35u, &Kernel::cancel_wakeup_thread);
    add(static_cast<std::uint32_t>(-0x36), &Kernel::cancel_wakeup_thread);
    add(0x37u, &Kernel::suspend_thread);
    add(static_cast<std::uint32_t>(-0x38), &Kernel::suspend_thread);
    add(0x39u, &Kernel::resume_thread);
    add(static_cast<std::uint32_t>(-0x3A), &Kernel::resume_thread);
    add(0x3Cu, &Kernel::setup_thread);
    add(0x40u, &Kernel::create_sema);
    add(0x41u, &Kernel::delete_sema);
    add(static_cast<std::uint32_t>(-0x49), &Kernel::delete_sema);
    add(0x42u, &Kernel::signal_sema);
    add(static_cast<std::uint32_t>(-0x43), &Kernel::signal_sema);
    add(0x44u, &Kernel::wait_sema);
    add(0x45u, &Kernel::poll_sema);
    add(static_cast<std::uint32_t>(-0x46), &Kernel::poll_sema);
    add(0x47u, &Kernel::refer_sema_status);
    add(static_cast<std::uint32_t>(-0x48), &Kernel::refer_sema_status);
    add(0x74u, &Kernel::set_syscall);
    add(0x4Au, &Kernel::set_osd_config);
    add(0x4Bu, &Kernel::get_osd_config);
    add(0x76u, &Kernel::sif_dma_stat);
    add(static_cast<std::uint32_t>(-0x76), &Kernel::sif_dma_stat);
    add(0x77u, &Kernel::sif_set_dma);
    add(static_cast<std::uint32_t>(-0x77), &Kernel::sif_set_dma);
    add(0x78u, &Kernel::sif_set_d_chain);
    add(static_cast<std::uint32_t>(-0x78), &Kernel::sif_set_d_chain);
    add(0x79u, &Kernel::sif_set_reg);
    add(0x7Au, &Kernel::sif_get_reg);
    add(0x7Cu, &Kernel::deci2_call);
    add(0x6Bu, &Kernel::sif_stop_dma);
    add(0x02u, &Kernel::set_gs_crt);
    add(0x6Eu, &Kernel::set_osd_config2);
    add(0x6Fu, &Kernel::get_osd_config2);
    add(0x70u, &Kernel::gs_get_imr);
    add(static_cast<std::uint32_t>(-0x70), &Kernel::gs_get_imr);
    add(0x71u, &Kernel::gs_put_imr);
    add(static_cast<std::uint32_t>(-0x71), &Kernel::gs_put_imr);
    add(0x10u, &Kernel::add_intc_handler);
    add(0x11u, &Kernel::remove_intc_handler);
    add(0x12u, &Kernel::add_dmac_handler);
    add(0x13u, &Kernel::remove_dmac_handler);
    add(0x14u, &Kernel::enable_intc);
    add(0x15u, &Kernel::disable_intc);
    add(0x16u, &Kernel::enable_dmac);
    add(0x17u, &Kernel::disable_dmac);
    add(static_cast<std::uint32_t>(-0x1A), &Kernel::enable_intc);
    add(static_cast<std::uint32_t>(-0x1B), &Kernel::disable_intc);
    add(static_cast<std::uint32_t>(-0x1C), &Kernel::enable_dmac);
    add(static_cast<std::uint32_t>(-0x1D), &Kernel::disable_dmac);
    add(patch_return_service, &Kernel::deferred_return);
}

std::uint32_t Kernel::current_thread_id() const noexcept {
    return current_thread_id_;
}

std::uint32_t Kernel::patched_handler(std::uint32_t number) const noexcept {
    if (number >= syscall_table_entries) {
        return 0;
    }
    return patched_handlers_[number];
}

std::uint32_t Kernel::osd_config() const noexcept {
    return osd_config_;
}

void Kernel::ensure_syscall_table(GuestState& state) {
    if (syscall_table_ready_) {
        return;
    }
    // One opaque token per number: values in the kernel segment so a guest
    // that range-checks them sees kernel addresses, distinct from the two
    // game handler addresses the boot patches in before searching.
    for (std::uint32_t index = 0; index < syscall_table_entries; ++index) {
        state.memory().write_word(syscall_table_physical + index * 4,
                                  syscall_token_base + index * 4);
    }
    syscall_table_ready_ = true;
}

const std::vector<KernelThread>& Kernel::threads() const noexcept {
    return threads_;
}

const std::vector<KernelSemaphore>& Kernel::semaphores() const noexcept {
    return semaphores_;
}

std::vector<std::uint32_t> Kernel::sif_server_sids() const {
    std::vector<std::uint32_t> sids;
    sids.reserve(sif_rpc_servers_.size());
    for (const auto& [sid, server] : sif_rpc_servers_) {
        (void)server;
        sids.push_back(sid);
    }
    return sids;
}

const std::map<std::uint64_t, RpcPairStats>& Kernel::rpc_pair_stats() const noexcept {
    return rpc_pair_stats_;
}

const std::map<std::uint32_t, RpcBindStats>& Kernel::rpc_bind_stats() const noexcept {
    return rpc_bind_stats_;
}

std::uint64_t Kernel::rpc_pair_key(std::uint32_t sid,
                                   std::uint32_t function) noexcept {
    return (static_cast<std::uint64_t>(sid) << 32) | function;
}

RpcPairClass Kernel::classify_rpc_pair(std::uint32_t sid,
                                       std::uint32_t function) noexcept {
    // Slice 73 (P07): every answered pair names its evidence; anything
    // else is Unknown, including SIDs the Opus map identifies by constant
    // only (plan section 4.4, item 6: a found SID is a candidate, not a
    // protocol). Two model replies carry disputed identities (0x80000400
    // and 0x80001300): the values are pinned by the game's own checks,
    // the server names are not, and the notes below say so.
    if (sid == 0x50434456u) {
        if (function == 2u || function == 3u || function == 4u) {
            return RpcPairClass::ImplementedVerified;
        }
        if (function == 1u) {
            return RpcPairClass::ProvisionalExplicit;
        }
        return RpcPairClass::Unknown;
    }
    if (sid == 0x53545250u) {
        if (function == 3u || function == 4u || function == 7u) {
            return RpcPairClass::ImplementedVerified;
        }
        return RpcPairClass::Unknown;
    }
    if (sid == 0x80000006u) {
        if (function == 0u) {
            return RpcPairClass::ImplementedVerified;
        }
        if (function == 0xFFu) {
            return RpcPairClass::CompatConstant;
        }
        return RpcPairClass::Unknown;
    }
    if (sid == 0x80000001u) {
        if (function == 0xFFu) {
            return RpcPairClass::CompatConstant;
        }
        return RpcPairClass::Unknown;
    }
    if (sid == 0x80000400u) {
        if (function == 0xFEu) {
            return RpcPairClass::CompatConstant;
        }
        return RpcPairClass::Unknown;
    }
    if (sid == 0x80001300u) {
        if (function == 0x80001363u) {
            return RpcPairClass::CompatConstant;
        }
        return RpcPairClass::Unknown;
    }
    if (sid == 0x046D046Du) {
        if (function == 12u || function == 4u) {
            return RpcPairClass::ProvisionalExplicit;
        }
        return RpcPairClass::Unknown;
    }
    return RpcPairClass::Unknown;
}

const char* Kernel::rpc_pair_class_name(RpcPairClass value) noexcept {
    switch (value) {
        case RpcPairClass::ImplementedVerified: return "implemented-verified";
        case RpcPairClass::CompatConstant: return "compat-constant";
        case RpcPairClass::AbsenceOrFailure: return "absence-or-failure";
        case RpcPairClass::ProvisionalExplicit: return "provisional-explicit";
        case RpcPairClass::Unknown: return "unknown";
    }
    return "unknown";
}

const char* Kernel::rpc_pair_candidate_name(std::uint32_t sid) noexcept {
    // Established means the boot shows registration, buffers, completion
    // and a consumer; candidate means the Opus map names the SID by
    // constant only. Disputed means the model comment and the Opus map
    // disagree and the boot has not settled it.
    switch (sid) {
        case 0u: return "unbound handle (no server)";
        case 0x50434456u: return "PCDV (PDICDVD primary, established)";
        case 0x50636476u: return "Pcdv secondary channel (Opus candidate)";
        case 0x53545250u: return "PRTS block cache (PDISTR, established)";
        case 0x4D504731u: return "MPG1 MPEG stream (PDISTR, Opus candidate)";
        case 0x4D504732u: return "MPG2 MPEG stream (PDISTR, Opus candidate)";
        case 0x5042474Du: return "PBGM stream (PDISTR, Opus candidate)";
        case 0x564F4943u: return "VOIC stream (PDISTR, Opus candidate)";
        case 0x53505550u: return "SPUP audio (PDISPU2, Opus candidate)";
        case 0x53505554u: return "SPUT audio transfer (PDISPU2, Opus candidate)";
        case 0x80000001u: return "SIF manager (SIFMAN, established)";
        case 0x80000006u: return "FILEIO file server (established)";
        case 0x80000592u: return "SCE-range numeric SID (unidentified)";
        // String-coded worker servers seen bound in the boot: the four
        // ASCII bytes are read mechanically (the same reading that spells
        // PCDV, MPG1 or SPUP), but no function, buffer or consumer is
        // pinned, so they stay unidentified candidates.
        case 0x424B5550u: return "BKUP worker (string-coded, unidentified)";
        case 0x45535550u: return "ESUP worker (string-coded, unidentified)";
        case 0x50555354u: return "PUST worker (string-coded, unidentified)";
        case 0x534D5550u: return "SMUP worker (string-coded, unidentified)";
        case 0x54485550u: return "THUP worker (string-coded, unidentified)";
        case 0x62737550u: return "bsuP worker (string-coded, unidentified)";
        case 0x80000400u: return "MCSERV memory card vs fileio/CDVD (identity disputed)";
        case 0x80001300u: return "DBCMAN pad bus vs disc subsystem (identity disputed)";
        case 0x8000131Cu: return "DBC socket channel A (Opus candidate)";
        case 0x8000131Eu: return "DBC socket channel B port 0 (Opus candidate)";
        case 0x8000131Fu: return "DBC socket channel B port 1 (Opus candidate)";
        case 0x046D046Du: return "LGDEV Logitech device (banner in memory)";
        default: break;
    }
    return "unidentified SID";
}

const char* Kernel::rpc_pair_note(std::uint32_t sid,
                                  std::uint32_t function) noexcept {
    if (sid == 0x50434456u) {
        if (function == 2u) {
            return "volume registration: checksum recomputed from the image, "
                   "loud on mismatch; engine keeps the size at task+0xEC";
        }
        if (function == 3u) {
            return "sector read {LBA, size, dest} from the image; library "
                   "checks CD001 at 0x00548E90 and walks the ISO";
        }
        if (function == 4u) {
            return "registered volume space size from the image; zeros "
                   "before registration read as failure and retry";
        }
        if (function == 1u) {
            return "completion-poll shape (send 64/recv 0) from slice 17; "
                   "payload unpinned, empty reply";
        }
        return "silent empty result; strict mode stops here";
    }
    if (sid == 0x53545250u) {
        if (function == 3u) {
            return "block read {LBA, size, flags} cached under a fresh "
                   "handle; the client needs non-zero to build its object";
        }
        if (function == 4u || function == 7u) {
            return "copy-out {handle, dest, size} from the handle cursor; "
                   "consumer 0x44D6BC writes stream+0x94 file object";
        }
        return "silent empty result; strict mode stops here";
    }
    if (sid == 0x80000006u) {
        if (function == 0u) {
            return "open path at +8, reply {handle, size} from the ISO; "
                   "handle 0 is valid not-found (0x005B6D6C)";
        }
        if (function == 0xFFu) {
            return "version: four bytes at 0x0065829C (3000), checked "
                   "at 0x005B6368";
        }
        return "silent empty result; strict mode stops here";
    }
    if (sid == 0x80000001u) {
        if (function == 0xFFu) {
            return "version: word at 0x0066829C plus flag 2";
        }
        return "silent empty result; strict mode stops here";
    }
    if (sid == 0x80000400u) {
        if (function == 0xFEu) {
            return "minimums 0x20A/0x20E accepted at 0x0058D674/0x0058D694; "
                   "module identity disputed (MCSERV vs fileio)";
        }
        return "silent empty result; strict mode stops here";
    }
    if (sid == 0x80001300u) {
        if (function == 0x80001363u) {
            return "first word 0x310 accepted by (word>>4)==0x31 at "
                   "0x0058F840; server identity disputed (DBCMAN vs disc)";
        }
        return "silent empty result; strict mode stops here";
    }
    if (sid == 0x046D046Du) {
        if (function == 12u) {
            return "completed status 0x010B2400 accepted at 0x005608BC; "
                   "remaining 572 bytes zero, unverified";
        }
        if (function == 4u) {
            return "media signature 0x046DC298 at +0x5C for 0x0055585C; "
                   "remaining bytes zero, unverified";
        }
        return "silent empty result; strict mode stops here";
    }
    return "silent empty result; strict mode stops here";
}

void Kernel::set_strict_rpc(bool strict) noexcept {
    strict_rpc_ = strict;
}

bool Kernel::strict_rpc() const noexcept {
    return strict_rpc_;
}

void Kernel::record_rpc_bind(GuestState& state, std::uint32_t sid) {
    RpcBindStats& stats = rpc_bind_stats_[sid];
    if (stats.count == 0) {
        stats.first_pc = state.pc();
        stats.first_thread = current_thread_id_;
    }
    ++stats.count;
    ++stats.counts_by_thread[current_thread_id_];
}

void Kernel::record_rpc_call(GuestState& state, std::uint32_t sid,
                             std::uint32_t function, std::uint32_t send_size,
                             std::uint32_t recv_buffer, std::uint32_t recv_size,
                             std::uint32_t server_buffer,
                             std::uint32_t result_size) {
    const std::uint64_t key = rpc_pair_key(sid, function);
    RpcPairStats& stats = rpc_pair_stats_[key];
    const std::uint32_t pc = state.pc();
    if (stats.calls == 0) {
        stats.sid = sid;
        stats.function = function;
        stats.first_pc = pc;
        stats.first_send_size = send_size;
        stats.first_recv_size = recv_size;
        stats.first_result_size = result_size;
        stats.first_recv_buffer = recv_buffer;
        stats.max_send_size = send_size;
        stats.max_recv_size = recv_size;
        stats.max_result_size = result_size;
        if (server_buffer != 0 && state.memory().contains(server_buffer, 16)) {
            for (std::uint32_t index = 0; index < 4; ++index) {
                stats.first_request_words[index] =
                    state.memory().read_word(server_buffer + index * 4);
            }
        }
    }
    ++stats.calls;
    ++stats.calls_by_thread[current_thread_id_];
    stats.last_pc = pc;
    if (send_size > stats.max_send_size) {
        stats.max_send_size = send_size;
    }
    if (recv_size > stats.max_recv_size) {
        stats.max_recv_size = recv_size;
    }
    if (result_size > stats.max_result_size) {
        stats.max_result_size = result_size;
    }
    bool seen = false;
    for (const std::uint32_t sample : stats.sample_pcs) {
        if (sample == pc) {
            seen = true;
            break;
        }
    }
    if (!seen) {
        if (stats.sample_pcs.size() < 4) {
            stats.sample_pcs.push_back(pc);
        } else {
            ++stats.extra_pcs;
        }
    }
}

const std::vector<KernelInterruptHandler>& Kernel::interrupt_handlers() const noexcept {
    return interrupt_handlers_;
}

const std::vector<KernelInterruptHandler>& Kernel::dmac_handlers() const noexcept {
    return dmac_handlers_;
}

ServiceOutcome Kernel::setup_thread(GuestState& state) {
    // SetupThread(gp, stack, stack_size, args, root): the crt0 calls this
    // before any thread exists to register the current execution as the root
    // thread and get its stack pointer, which it stores into sp. The model
    // returns the top of the caller-provided region aligned to 16 bytes
    // (evidence in docs/reverse-engineering/m30-bios-services-and-bridge.md).
    if (!threads_.empty()) {
        write_error(state);
        return ServiceOutcome::Handled;
    }
    const std::uint32_t stack = state.read_gpr32(5);
    const std::uint32_t stack_size = state.read_gpr32(6);
    if (stack_size == 0) {
        throw std::runtime_error("SetupThread with a zero stack size");
    }
    const std::uint64_t region_end =
        static_cast<std::uint64_t>(stack) + stack_size;
    if (region_end > 0x100000000ull) {
        throw std::runtime_error(
            "SetupThread stack region leaves the 32-bit address space");
    }
    if (!state.memory().contains(stack, stack_size)) {
        throw std::runtime_error(
            "SetupThread stack region is outside the mapped guest memory");
    }
    KernelThread root;
    root.id = next_thread_id_++;
    root.status = ThreadRun;
    root.stack = stack;
    root.stack_size = stack_size;
    // The ABI passes no priority; the model starts the root at 0 and relies
    // on the SDK's own ChangeThreadPriority ordering afterwards.
    root.initial_priority = 0;
    root.current_priority = 0;
    root.context = state.save_registers();
    threads_.push_back(root);
    current_thread_id_ = root.id;
    state.write_gpr64(2, static_cast<std::uint32_t>(region_end & ~0xfull));
    return ServiceOutcome::Handled;
}

ServiceOutcome Kernel::create_thread(GuestState& state) {
    const std::uint32_t pointer = state.read_gpr32(4);
    if (!state.memory().contains(pointer, thread_create_size)) {
        write_error(state);
        return ServiceOutcome::Handled;
    }
    KernelThread thread;
    thread.id = next_thread_id_++;
    thread.status = ThreadDormant;
    thread.function = state.memory().read_word(pointer + thread_func);
    thread.stack = state.memory().read_word(pointer + thread_stack);
    thread.stack_size = state.memory().read_word(pointer + thread_stack_size);
    thread.gp = state.memory().read_word(pointer + thread_gp);
    thread.initial_priority = static_cast<std::int32_t>(
        state.memory().read_word(pointer + thread_initial_priority));
    thread.attr = state.memory().read_word(pointer + thread_attr);
    thread.option = state.memory().read_word(pointer + thread_option);
    thread.current_priority = thread.initial_priority;
    if (thread.function == 0 || thread.stack_size == 0
        || thread.initial_priority < 0 || thread.initial_priority >= 128
        || !state.memory().contains(thread.stack, thread.stack_size)) {
        --next_thread_id_;  // a failed creation does not consume an id
        write_error(state);
        return ServiceOutcome::Handled;
    }
    // The kernel maintains the thread's current priority in the caller's
    // structure as well as internally.
    state.memory().write_word(pointer + thread_current_priority,
                             static_cast<std::uint32_t>(thread.current_priority));
    threads_.push_back(thread);
    state.write_gpr64(2, thread.id);
    return ServiceOutcome::Handled;
}

ServiceOutcome Kernel::delete_thread(GuestState& state) {
    const std::uint32_t id = state.read_gpr32(4);
    KernelThread* thread = find_thread(id);
    if (thread == nullptr || thread->status != ThreadDormant) {
        write_error(state);
        return ServiceOutcome::Handled;
    }
    threads_.erase(threads_.begin() + (thread - threads_.data()));
    state.write_gpr64(2, 0);
    return ServiceOutcome::Handled;
}

ServiceOutcome Kernel::start_thread(GuestState& state) {
    const std::uint32_t id = state.read_gpr32(4);
    const std::uint32_t argument = state.read_gpr32(5);
    KernelThread* thread = find_thread(id);
    if (thread == nullptr || thread->status != ThreadDormant) {
        write_error(state);
        return ServiceOutcome::Handled;
    }
    // A started thread begins at its entry with the argument in a0, its gp,
    // and the top of its stack region in sp. CP0 carries over from the
    // running thread so interrupts stay enabled; the FPU and VU0 files start
    // cleared.
    RegisterContext context;
    context.cp0 = state.save_registers().cp0;
    context.pc = thread->function;
    context.gpr[4] = argument;
    context.gpr[28] = thread->gp;
    context.gpr[29] = static_cast<std::uint32_t>(
        (static_cast<std::uint64_t>(thread->stack) + thread->stack_size) & ~0xfull);
    thread->context = context;
    thread->status = ThreadReady;
    thread->wait_type = ThreadWaitNone;
    thread->wait_id = 0;
    state.write_gpr64(2, 0);
    if (preempt_if_outranked(state)) {
        return ServiceOutcome::Switched;
    }
    return ServiceOutcome::Handled;
}

ServiceOutcome Kernel::exit_thread(GuestState& state) {
    KernelThread* current = current_thread();
    if (current == nullptr) {
        return ServiceOutcome::NoRunnableThread;
    }
    current->status = ThreadDormant;
    current->wait_type = ThreadWaitNone;
    current->wait_id = 0;
    if (dispatch(state)) {
        return ServiceOutcome::Switched;
    }
    return ServiceOutcome::NoRunnableThread;
}

ServiceOutcome Kernel::exit_delete_thread(GuestState& state) {
    KernelThread* current = current_thread();
    if (current == nullptr) {
        return ServiceOutcome::NoRunnableThread;
    }
    threads_.erase(threads_.begin() + (current - threads_.data()));
    if (dispatch(state)) {
        return ServiceOutcome::Switched;
    }
    return ServiceOutcome::NoRunnableThread;
}

ServiceOutcome Kernel::terminate_thread(GuestState& state) {
    const std::uint32_t id = state.read_gpr32(4);
    KernelThread* thread = find_thread(id);
    if (thread == nullptr || thread->status == ThreadDormant) {
        write_error(state);
        return ServiceOutcome::Handled;
    }
    const bool is_current = id == current_thread_id_;
    if ((thread->status & ThreadWait) != 0 && thread->wait_type == ThreadWaitSema) {
        if (KernelSemaphore* semaphore = find_semaphore(thread->wait_id);
            semaphore != nullptr) {
            --semaphore->wait_threads;
        }
    }
    thread->status = ThreadDormant;
    thread->wait_type = ThreadWaitNone;
    thread->wait_id = 0;
    state.write_gpr64(2, 0);
    if (is_current) {
        if (dispatch(state)) {
            return ServiceOutcome::Switched;
        }
        return ServiceOutcome::NoRunnableThread;
    }
    return ServiceOutcome::Handled;
}

ServiceOutcome Kernel::change_thread_priority(GuestState& state) {
    const std::uint32_t id = state.read_gpr32(4);
    const std::uint32_t priority = state.read_gpr32(5);
    KernelThread* thread = find_thread(id);
    if (thread == nullptr || priority >= 128) {
        write_error(state);
        return ServiceOutcome::Handled;
    }
    thread->current_priority = static_cast<std::int32_t>(priority);
    state.write_gpr64(2, 0);
    if (preempt_if_outranked(state)) {
        return ServiceOutcome::Switched;
    }
    return ServiceOutcome::Handled;
}

ServiceOutcome Kernel::rotate_ready_queue(GuestState& state) {
    // The kernel rotates equal-priority ready threads. The model dispatches
    // in creation order, which is already deterministic, so the call is
    // accepted with no effect (recorded in decision 0005).
    state.write_gpr64(2, 0);
    return ServiceOutcome::Handled;
}

ServiceOutcome Kernel::release_wait_thread(GuestState& state) {
    const std::uint32_t id = state.read_gpr32(4);
    KernelThread* thread = find_thread(id);
    if (thread == nullptr || (thread->status & ThreadWait) == 0) {
        write_error(state);
        return ServiceOutcome::Handled;
    }
    if (thread->wait_type == ThreadWaitSema) {
        if (KernelSemaphore* semaphore = find_semaphore(thread->wait_id);
            semaphore != nullptr) {
            --semaphore->wait_threads;
        }
    }
    thread->status = (thread->status & ~ThreadWait) | ThreadReady;
    thread->wait_type = ThreadWaitNone;
    thread->wait_id = 0;
    state.write_gpr64(2, 0);
    if (preempt_if_outranked(state)) {
        return ServiceOutcome::Switched;
    }
    return ServiceOutcome::Handled;
}

ServiceOutcome Kernel::get_thread_id(GuestState& state) {
    // Inside a handler over a running thread this names the interrupted
    // thread, which is still current. Over idle no thread runs, so it
    // names the idle thread: thread 0 is always the kernel's idle thread
    // (ps2sdk ee/kernel/include/kernel.h, MAX_THREADS comment), and the
    // game's own ids start at 1. The game's safe-wakeup wrapper compares
    // iGetThreadId against its live target, so idle must never look like
    // a blocked thread (decision 0031).
    const KernelThread* current = current_thread();
    const std::uint32_t id = (current != nullptr && current->status == ThreadRun)
        ? current->id : idle_thread_id;
    state.write_gpr64(2, id);
    return ServiceOutcome::Handled;
}

ServiceOutcome Kernel::refer_thread_status(GuestState& state) {
    const std::uint32_t id = state.read_gpr32(4);
    const std::uint32_t info = state.read_gpr32(5);
    KernelThread* thread = find_thread(id);
    if (thread == nullptr || !state.memory().contains(info, thread_status_size)) {
        write_error(state);
        return ServiceOutcome::Handled;
    }
    state.memory().write_word(info + thread_status, thread->status);
    state.memory().write_word(info + thread_func, thread->function);
    state.memory().write_word(info + thread_stack, thread->stack);
    state.memory().write_word(info + thread_stack_size, thread->stack_size);
    state.memory().write_word(info + thread_gp, thread->gp);
    state.memory().write_word(info + thread_initial_priority,
                              static_cast<std::uint32_t>(thread->initial_priority));
    state.memory().write_word(info + thread_current_priority,
                              static_cast<std::uint32_t>(thread->current_priority));
    state.memory().write_word(info + thread_attr, thread->attr);
    state.memory().write_word(info + thread_option, thread->option);
    state.memory().write_word(info + thread_wait_type, thread->wait_type);
    state.memory().write_word(info + thread_wait_id, thread->wait_id);
    state.memory().write_word(info + thread_wakeup_count, thread->wakeup_count);
    state.write_gpr64(2, 0);
    return ServiceOutcome::Handled;
}

ServiceOutcome Kernel::sleep_thread(GuestState& state) {
    KernelThread* current = current_thread();
    if (current == nullptr) {
        write_error(state);
        return ServiceOutcome::Handled;
    }
    if (current->wakeup_count > 0) {
        --current->wakeup_count;
        state.write_gpr64(2, 0);
        return ServiceOutcome::Handled;
    }
    if (!block_current(state, ThreadWaitSleep, 0, 0)) {
        return ServiceOutcome::NoRunnableThread;
    }
    return ServiceOutcome::Switched;
}

ServiceOutcome Kernel::wakeup_thread(GuestState& state) {
    const std::uint32_t id = state.read_gpr32(4);
    KernelThread* thread = find_thread(id);
    if (thread == nullptr) {
        write_error(state);
        return ServiceOutcome::Handled;
    }
    if (thread->id == current_thread_id_ && thread->status == ThreadRun) {
        // The kernel's documented iWakeupThread defect: a running thread
        // cannot wake itself; the SDK's patched wrapper routes those cases
        // through the KernelTopThread.
        write_error(state);
        return ServiceOutcome::Handled;
    }
    if ((thread->status & ThreadWait) != 0
        && thread->wait_type == ThreadWaitSleep) {
        thread->status = (thread->status & ~ThreadWait) | ThreadReady;
        thread->wait_type = ThreadWaitNone;
        state.write_gpr64(2, 0);
        if (preempt_if_outranked(state)) {
            return ServiceOutcome::Switched;
        }
        return ServiceOutcome::Handled;
    }
    ++thread->wakeup_count;
    state.write_gpr64(2, 0);
    return ServiceOutcome::Handled;
}

ServiceOutcome Kernel::cancel_wakeup_thread(GuestState& state) {
    const std::uint32_t id = state.read_gpr32(4);
    KernelThread* thread = find_thread(id);
    if (thread == nullptr) {
        write_error(state);
        return ServiceOutcome::Handled;
    }
    const std::uint32_t previous = thread->wakeup_count;
    thread->wakeup_count = 0;
    state.write_gpr64(2, previous);
    return ServiceOutcome::Handled;
}

ServiceOutcome Kernel::suspend_thread(GuestState& state) {
    const std::uint32_t id = state.read_gpr32(4);
    KernelThread* thread = find_thread(id);
    if (thread == nullptr || (thread->status & ThreadDormant) != 0) {
        write_error(state);
        return ServiceOutcome::Handled;
    }
    thread->status |= ThreadSuspend;
    state.write_gpr64(2, 0);
    if (thread->id == current_thread_id_ && (thread->status & ThreadRun) != 0) {
        thread->context = state.save_registers();
        thread->context.pc += 4;
        thread->status = (thread->status & ~ThreadRun) | ThreadReady;
        if (dispatch(state)) {
            return ServiceOutcome::Switched;
        }
        return ServiceOutcome::NoRunnableThread;
    }
    return ServiceOutcome::Handled;
}

ServiceOutcome Kernel::resume_thread(GuestState& state) {
    const std::uint32_t id = state.read_gpr32(4);
    KernelThread* thread = find_thread(id);
    if (thread == nullptr || (thread->status & ThreadSuspend) == 0) {
        write_error(state);
        return ServiceOutcome::Handled;
    }
    thread->status &= ~ThreadSuspend;
    state.write_gpr64(2, 0);
    if (preempt_if_outranked(state)) {
        return ServiceOutcome::Switched;
    }
    return ServiceOutcome::Handled;
}

ServiceOutcome Kernel::create_sema(GuestState& state) {
    const std::uint32_t pointer = state.read_gpr32(4);
    if (!state.memory().contains(pointer, sema_size)) {
        write_error(state);
        return ServiceOutcome::Handled;
    }
    KernelSemaphore semaphore;
    semaphore.count = static_cast<std::int32_t>(
        state.memory().read_word(pointer + sema_count));
    semaphore.max_count = static_cast<std::int32_t>(
        state.memory().read_word(pointer + sema_max_count));
    semaphore.init_count = static_cast<std::int32_t>(
        state.memory().read_word(pointer + sema_init_count));
    semaphore.wait_threads = static_cast<std::int32_t>(
        state.memory().read_word(pointer + sema_wait_threads));
    semaphore.attr = state.memory().read_word(pointer + sema_attr);
    semaphore.option = state.memory().read_word(pointer + sema_option);
    if (semaphore.max_count <= 0 || semaphore.init_count < 0
        || semaphore.init_count > semaphore.max_count) {
        write_error(state);
        return ServiceOutcome::Handled;
    }
    semaphore.id = next_semaphore_id_;
    next_semaphore_id_ += 4;
    semaphore.count = semaphore.init_count;
    semaphore.wait_threads = 0;
    // The kernel is the writer of count and wait_threads; mirror them into
    // the caller's structure.
    state.memory().write_word(pointer + sema_count,
                              static_cast<std::uint32_t>(semaphore.count));
    state.memory().write_word(pointer + sema_wait_threads, 0);
    semaphores_.push_back(semaphore);
    state.write_gpr64(2, semaphore.id);
    return ServiceOutcome::Handled;
}

ServiceOutcome Kernel::delete_sema(GuestState& state) {
    const std::uint32_t id = state.read_gpr32(4);
    KernelSemaphore* semaphore = find_semaphore(id);
    if (semaphore == nullptr || semaphore->wait_threads != 0) {
        write_error(state);
        return ServiceOutcome::Handled;
    }
    semaphores_.erase(semaphores_.begin() + (semaphore - semaphores_.data()));
    state.write_gpr64(2, 0);
    return ServiceOutcome::Handled;
}

ServiceOutcome Kernel::signal_sema(GuestState& state) {
    const std::uint32_t id = state.read_gpr32(4);
    KernelSemaphore* semaphore = find_semaphore(id);
    if (semaphore == nullptr) {
        write_error(state);
        return ServiceOutcome::Handled;
    }
    if (semaphore->wait_threads > 0) {
        // Hand the semaphore to the first waiter in creation order; a
        // suspended waiter stays ready-but-suspended.
        for (KernelThread& thread : threads_) {
            if ((thread.status & ThreadWait) != 0
                && thread.wait_type == ThreadWaitSema && thread.wait_id == id) {
                thread.status = (thread.status & ~ThreadWait) | ThreadReady;
                thread.wait_type = ThreadWaitNone;
                thread.wait_id = 0;
                --semaphore->wait_threads;
                break;
            }
        }
    } else if (semaphore->count < semaphore->max_count) {
        ++semaphore->count;
    }
    state.write_gpr64(2, 0);
    if (preempt_if_outranked(state)) {
        return ServiceOutcome::Switched;
    }
    return ServiceOutcome::Handled;
}

ServiceOutcome Kernel::wait_sema(GuestState& state) {
    const std::uint32_t id = state.read_gpr32(4);
    KernelSemaphore* semaphore = find_semaphore(id);
    if (semaphore == nullptr) {
        write_error(state);
        return ServiceOutcome::Handled;
    }
    if (semaphore->count > 0) {
        --semaphore->count;
        state.write_gpr64(2, 0);
        return ServiceOutcome::Handled;
    }
    ++semaphore->wait_threads;
    if (!block_current(state, ThreadWaitSema, id, 0)) {
        return ServiceOutcome::NoRunnableThread;
    }
    return ServiceOutcome::Switched;
}

ServiceOutcome Kernel::poll_sema(GuestState& state) {
    const std::uint32_t id = state.read_gpr32(4);
    KernelSemaphore* semaphore = find_semaphore(id);
    if (semaphore == nullptr) {
        write_error(state);
        return ServiceOutcome::Handled;
    }
    if (semaphore->count > 0) {
        --semaphore->count;
        state.write_gpr64(2, 0);
    } else {
        write_error(state);
    }
    return ServiceOutcome::Handled;
}

ServiceOutcome Kernel::refer_sema_status(GuestState& state) {
    const std::uint32_t id = state.read_gpr32(4);
    const std::uint32_t info = state.read_gpr32(5);
    KernelSemaphore* semaphore = find_semaphore(id);
    if (semaphore == nullptr || !state.memory().contains(info, sema_size)) {
        write_error(state);
        return ServiceOutcome::Handled;
    }
    state.memory().write_word(info + sema_count,
                              static_cast<std::uint32_t>(semaphore->count));
    state.memory().write_word(info + sema_max_count,
                              static_cast<std::uint32_t>(semaphore->max_count));
    state.memory().write_word(info + sema_init_count,
                              static_cast<std::uint32_t>(semaphore->init_count));
    state.memory().write_word(info + sema_wait_threads,
                              static_cast<std::uint32_t>(semaphore->wait_threads));
    state.memory().write_word(info + sema_attr, semaphore->attr);
    state.memory().write_word(info + sema_option, semaphore->option);
    state.write_gpr64(2, 0);
    return ServiceOutcome::Handled;
}

ServiceOutcome Kernel::set_syscall(GuestState& state) {
    const std::uint32_t number = state.read_gpr32(4);
    const std::uint32_t handler = state.read_gpr32(5);
    if (service_table_ == nullptr) {
        throw std::logic_error("SetSyscall without a registered service table");
    }
    if (number >= syscall_table_entries || handler == 0) {
        write_error(state);
        return ServiceOutcome::Handled;
    }
    ensure_syscall_table(state);
    if (handler == syscall_token_base + number * 4) {
        // The SDK's GetEntryAddress returned this number's own model entry
        // and the caller re-installed it: drop any patch, keep the token.
        patched_handlers_[number] = 0;
        state.memory().write_word(syscall_table_physical + number * 4, handler);
        service_table_->remove(number);
        state.write_gpr64(2, 0);
        return ServiceOutcome::Handled;
    }
    // The synthetic table is the guest-visible view of the patch: the SDK
    // locates it by searching for the handler values it just installed, then
    // reads the other entries through it.
    state.memory().write_word(syscall_table_physical + number * 4, handler);
    patched_handlers_[number] = handler;
    // A patched syscall transfers control to the guest handler. The kernel
    // dispatcher on real hardware saves the user context and returns through
    // EPC; the model saves the caller's ra and the resume address, sends the
    // handler back through the stub, and restores both there. Caller-saved
    // registers may change, exactly as the o32 ABI allows.
    service_table_->add(number, [this, handler](GuestState& state) {
        const std::uint32_t syscall_pc = state.pc();
        state.memory().write_word(patch_return_stub_physical, 0x24030100u);
        state.memory().write_word(patch_return_stub_physical + 4, 0x0000000Cu);
        DeferredCall call;
        call.kind = DeferredCall::Kind::Patch;
        call.resume_pc = syscall_pc + 4;
        call.caller_ra = static_cast<std::uint32_t>(state.read_gpr64(31));
        deferred_calls_.push_back(call);
        state.write_gpr64(31, patch_return_stub_physical);
        state.set_pc(handler);
        return ServiceOutcome::Jumped;
    });
    state.write_gpr64(2, 0);
    return ServiceOutcome::Handled;
}

ServiceOutcome Kernel::deferred_return(GuestState& state) {
    // The stub's private service: restore what the deferred call saved. A
    // patched syscall gets its caller's ra and resume address back; an
    // injected interrupt handler gets the whole interrupted context back
    // once its chain of handlers has run.
    if (deferred_calls_.empty()) {
        throw std::logic_error("The patch return stub fired with no call in flight");
    }
    DeferredCall& pending = deferred_calls_.back();
    if (pending.kind == DeferredCall::Kind::Patch) {
        const DeferredCall call = pending;
        deferred_calls_.pop_back();
        state.write_gpr64(31, call.caller_ra);
        state.set_pc(call.resume_pc);
        return ServiceOutcome::Jumped;
    }
    // Interrupt: every handler registered for the cause runs in turn,
    // each framed with its own argument (the pair travels in handlers).
    if (pending.next_handler + 1 < pending.handlers.size()) {
        ++pending.next_handler;
        install_handler_frame(state, pending.cause,
                              pending.handlers[pending.next_handler],
                              pending.context);
        return ServiceOutcome::Jumped;
    }
    const DeferredCall call = pending;
    deferred_calls_.pop_back();
    if (call.thread_id == idle_thread_id) {
        // Interrupted-idle: no thread ran underneath. The handlers may
        // have woken threads, so the scheduler chooses among the ready;
        // when they woke nothing the machine stays idle with the
        // interrupted frame preserved (decision 0031). This branch is
        // only reachable because injection records idle explicitly:
        // zeroing the current id alone would still land in the
        // find_thread failure below by accident, with no dispatch.
        if (dispatch(state)) {
            return ServiceOutcome::Jumped;
        }
        state.restore_registers(call.context);
        return ServiceOutcome::NoRunnableThread;
    }
    KernelThread* interrupted = find_thread(call.thread_id);
    if (interrupted == nullptr) {
        // Its thread was deleted while the handler ran, so there is no
        // thread context to account for: restore the interrupted context.
        state.restore_registers(call.context);
        return ServiceOutcome::Jumped;
    }
    if (interrupted->status != ThreadRun) {
        // The interrupted thread stopped running while the handler ran
        // (it blocked or left): the handlers may have woken a thread, so
        // the scheduler picks the best ready one instead of restoring a
        // WAIT thread as RUN. When they woke nothing, the machine stays
        // idle with the interrupted frame preserved.
        if (dispatch(state)) {
            return ServiceOutcome::Jumped;
        }
        state.restore_registers(call.context);
        return ServiceOutcome::NoRunnableThread;
    }
    state.restore_registers(call.context);
    return ServiceOutcome::Jumped;
}

ServiceOutcome Kernel::get_osd_config(GuestState& state) {
    // GetOsdConfigParam(addr): the kernel writes its ConfigParam word to the
    // guest address.
    const std::uint32_t address = state.read_gpr32(4);
    if (!state.memory().contains(address, 4)) {
        write_error(state);
        return ServiceOutcome::Handled;
    }
    state.memory().write_word(address, osd_config_);
    state.write_gpr64(2, 0);
    return ServiceOutcome::Handled;
}

ServiceOutcome Kernel::set_osd_config(GuestState& state) {
    // SetOsdConfigParam(addr): every field is retained, including version;
    // the SDK probes exactly that to tell a late kernel from an early
    // Japanese one (decision 0008).
    const std::uint32_t address = state.read_gpr32(4);
    if (!state.memory().contains(address, 4)) {
        write_error(state);
        return ServiceOutcome::Handled;
    }
    osd_config_ = state.memory().read_word(address);
    state.write_gpr64(2, 0);
    return ServiceOutcome::Handled;
}

ServiceOutcome Kernel::get_osd_config2(GuestState& state) {
    // GetOsdConfigParam2(buffer, size, offset). The game reads one byte at
    // offset 1 (the daylight-savings, clock and date-format bits).
    const std::uint32_t address = state.read_gpr32(4);
    const std::uint32_t size = state.read_gpr32(5);
    const std::uint32_t offset = state.read_gpr32(6);
    if (!state.memory().contains(address, size)) {
        write_error(state);
        return ServiceOutcome::Handled;
    }
    for (std::uint32_t index = 0; index < size; ++index) {
        const std::uint32_t position = offset + index;
        const std::uint8_t byte = position < osd_config2_.size()
            ? osd_config2_[position] : 0u;
        state.memory().write_byte(address + index, byte);
    }
    state.write_gpr64(2, 0);
    return ServiceOutcome::Handled;
}

ServiceOutcome Kernel::set_gs_crt(GuestState& state) {
    // SetGsCrt(interlace, video, field): no display is modeled, so the call
    // is accepted and reports success.
    state.write_gpr64(2, 0);
    return ServiceOutcome::Handled;
}

ServiceOutcome Kernel::deci2_call(GuestState& state) {
    // Deci2Call(call, address): the DECI2 debug-host interface. No debug
    // host is attached, so the calls are accepted with the reference
    // emulator's returns (1 for the defined calls, -1 beyond 0x10); the
    // game's debug output has no destination, as on a console without a
    // host.
    const std::uint32_t call = state.read_gpr32(4);
    state.write_gpr64(2, call > 0x10u ? 0xFFFFFFFFu : 1u);
    return ServiceOutcome::Handled;
}

ServiceOutcome Kernel::gs_get_imr(GuestState& state) {
    // GsGetIMR: the stored 64-bit interrupt mask, in one GPR.
    state.write_gpr64(2, gs_imr_);
    return ServiceOutcome::Handled;
}

ServiceOutcome Kernel::gs_put_imr(GuestState& state) {
    // GsPutIMR: store the mask and return the previous value.
    const std::uint64_t value = state.read_gpr64(4);
    const std::uint64_t previous = gs_imr_;
    gs_imr_ = value;
    state.write_gpr64(2, previous);
    return ServiceOutcome::Handled;
}

ServiceOutcome Kernel::set_osd_config2(GuestState& state) {
    // SetOsdConfigParam2(buffer, size, offset): retain what the caller
    // writes inside the block; bytes past it are dropped like the
    // reference emulator drops them.
    const std::uint32_t address = state.read_gpr32(4);
    const std::uint32_t size = state.read_gpr32(5);
    const std::uint32_t offset = state.read_gpr32(6);
    if (!state.memory().contains(address, size)) {
        write_error(state);
        return ServiceOutcome::Handled;
    }
    for (std::uint32_t index = 0; index < size; ++index) {
        const std::uint32_t position = offset + index;
        if (position < osd_config2_.size()) {
            osd_config2_[position] = state.memory().read_byte(address + index);
        }
    }
    state.write_gpr64(2, 0);
    return ServiceOutcome::Handled;
}

ServiceOutcome Kernel::add_intc_handler(GuestState& state) {
    const std::uint32_t cause = state.read_gpr32(4);
    const std::uint32_t handler = state.read_gpr32(5);
    // Both the plain and the 2-suffixed forms share this syscall number
    // (ps2sdk syscallnr.h: __NR_AddIntcHandler2 is __NR_AddIntcHandler),
    // so the kernel keeps the fourth register either way, exactly like
    // the hardware register the dispatcher later reads for a1: the
    // three-argument form simply stores whatever the caller left in a3,
    // and only handlers prototyped with (cause, arg, addr) read it back
    // (ps2sdk kernel.h, decision 0031).
    const std::uint32_t argument = state.read_gpr32(7);
    if (handler == 0) {
        write_error(state);
        return ServiceOutcome::Handled;
    }
    KernelInterruptHandler registration;
    registration.id = next_handler_id_++;
    registration.cause = cause;
    registration.handler = handler;
    registration.argument = argument;
    interrupt_handlers_.push_back(registration);
    state.write_gpr64(2, registration.id);
    return ServiceOutcome::Handled;
}

ServiceOutcome Kernel::add_dmac_handler(GuestState& state) {
    // AddDmacHandler(channel, handler, next): the channel's completions are
    // dispatched from the DMAC's own list, separate from the INTC causes.
    // The fourth register is kept like the INTC path's: both the plain
    // and the 2-suffixed forms share the syscall number (ps2sdk
    // syscallnr.h), and only (channel, arg, addr) handlers read it back.
    const std::uint32_t channel = state.read_gpr32(4);
    const std::uint32_t handler = state.read_gpr32(5);
    const std::uint32_t argument = state.read_gpr32(7);
    if (handler == 0 || channel >= 32) {
        write_error(state);
        return ServiceOutcome::Handled;
    }
    KernelInterruptHandler registration;
    registration.id = next_handler_id_++;
    registration.cause = channel;
    registration.handler = handler;
    registration.argument = argument;
    dmac_handlers_.push_back(registration);
    state.write_gpr64(2, registration.id);
    return ServiceOutcome::Handled;
}

ServiceOutcome Kernel::remove_intc_handler(GuestState& state) {
    const std::uint32_t cause = state.read_gpr32(4);
    const std::uint32_t id = state.read_gpr32(5);
    for (auto entry = interrupt_handlers_.begin();
         entry != interrupt_handlers_.end(); ++entry) {
        if (entry->id == id && entry->cause == cause) {
            interrupt_handlers_.erase(entry);
            state.write_gpr64(2, 0);
            return ServiceOutcome::Handled;
        }
    }
    write_error(state);
    return ServiceOutcome::Handled;
}

ServiceOutcome Kernel::remove_dmac_handler(GuestState& state) {
    const std::uint32_t channel = state.read_gpr32(4);
    const std::uint32_t id = state.read_gpr32(5);
    for (auto entry = dmac_handlers_.begin(); entry != dmac_handlers_.end();
         ++entry) {
        if (entry->id == id && entry->cause == channel) {
            dmac_handlers_.erase(entry);
            state.write_gpr64(2, 0);
            return ServiceOutcome::Handled;
        }
    }
    write_error(state);
    return ServiceOutcome::Handled;
}

ServiceOutcome Kernel::enable_intc(GuestState& state) {
    // EnableIntc(cause): the privileged path to the cause's INTC mask bit.
    // The guest can also toggle the same bit by writing INTC_MASK; both
    // writers share the unit's mask, each with its own contract.
    const std::uint32_t cause = state.read_gpr32(4);
    if (cause >= 32) {
        write_error(state);
        return ServiceOutcome::Handled;
    }
    if (intc_unit_ != nullptr) {
        intc_unit_->enable_internal(cause);
    }
    state.write_gpr64(2, 0);
    return ServiceOutcome::Handled;
}

ServiceOutcome Kernel::disable_intc(GuestState& state) {
    const std::uint32_t cause = state.read_gpr32(4);
    if (cause >= 32) {
        write_error(state);
        return ServiceOutcome::Handled;
    }
    if (intc_unit_ != nullptr) {
        intc_unit_->disable_internal(cause);
    }
    state.write_gpr64(2, 0);
    return ServiceOutcome::Handled;
}

ServiceOutcome Kernel::enable_dmac(GuestState& state) {
    // EnableDmac(channel): the privileged path to the channel's CIM bit.
    const std::uint32_t channel = state.read_gpr32(4);
    if (channel >= 16) {
        write_error(state);
        return ServiceOutcome::Handled;
    }
    if (dmac_unit_ != nullptr) {
        dmac_unit_->enable_internal(channel);
    }
    state.write_gpr64(2, 0);
    return ServiceOutcome::Handled;
}

ServiceOutcome Kernel::disable_dmac(GuestState& state) {
    const std::uint32_t channel = state.read_gpr32(4);
    if (channel >= 16) {
        write_error(state);
        return ServiceOutcome::Handled;
    }
    if (dmac_unit_ != nullptr) {
        dmac_unit_->disable_internal(channel);
    }
    state.write_gpr64(2, 0);
    return ServiceOutcome::Handled;
}

void Kernel::set_timer_unit(TimerUnit* timer) noexcept {
    timer_unit_ = timer;
}

void Kernel::set_intc_unit(IntcUnit* intc) noexcept {
    intc_unit_ = intc;
}

void Kernel::set_dmac_unit(DmacStatusUnit* dmac) noexcept {
    dmac_unit_ = dmac;
}

bool Kernel::cpu_gate_allows(const GuestState& state,
                             bool is_dmac) noexcept {
    const std::uint32_t status = state.read_cp0(12);
    if ((status & cp0_status_ie) == 0 || (status & cp0_status_eie) == 0
        || (status & cp0_status_exl) != 0 || (status & cp0_status_erl) != 0) {
        return false;
    }
    return (status & (is_dmac ? cp0_status_int1 : cp0_status_int0)) != 0;
}

bool Kernel::intc_mask_allows(std::uint32_t cause) const noexcept {
    if (intc_unit_ == nullptr) {
        return true;
    }
    return intc_unit_->mask_allows(cause);
}

bool Kernel::dmac_mask_allows(std::uint32_t channel) const noexcept {
    if (dmac_unit_ == nullptr) {
        return true;
    }
    return dmac_unit_->mask_allows(channel);
}

std::uint32_t Kernel::pending_interrupts() const noexcept {
    return static_cast<std::uint32_t>(interrupt_queue_.size());
}

std::size_t Kernel::deferred_call_count() const noexcept {
    return deferred_calls_.size();
}

std::uint32_t Kernel::sif_register_index_address(std::uint32_t index) const noexcept {
    // The public indices 1-4 name MSCOM, SMCOM, MSFLG and SMFLG.
    if (index >= 1 && index <= 4) {
        return sif_register_physical + (index - 1) * 0x10;
    }
    return 0;
}

const std::string& Kernel::sif_iop_image() const noexcept {
    return sif_iop_image_;
}

void Kernel::ensure_sif_ready(GuestState& state) {
    if (sif_ready_) {
        return;
    }
    // The model's IOP has completed its SIF and SIFCMD initialization: SMFLG
    // reports SIFINIT|CMDINIT|BOOTEND and SMCOM holds the IOP's command
    // buffer address. The addresses live in the SIF register bank the host
    // maps; an unmapped bank fails loudly instead of pretending.
    state.memory().write_word(sif_register_index_address(4), sif_mesg_init);
    state.memory().write_word(sif_register_index_address(2), sif_iop_command_buffer);
    sif_ready_ = true;
}

void Kernel::queue_interrupt(std::uint32_t cause) {
    // Coalescing in place (decisions 0025/0029): a cause that is already
    // pending keeps its queue position, like the hardware status bit the
    // handler reads and clears. Queue entries carry no payload (handlers
    // read live state), so a second instance adds nothing but backlog:
    // without this, one VBlank per idle call grows the queue without bound
    // (463k observed) and buries every other cause behind stale frames.
    // Keeping the position (instead of erase-and-push) preserves the
    // relative order of distinct causes.
    for (const InterruptRequest& entry : interrupt_queue_) {
        if (entry.kind == InterruptRequest::Kind::Intc
            && entry.number == cause) {
            if (intc_unit_ != nullptr) {
                intc_unit_->set_pending_internal(cause);
            }
            return;
        }
    }
    if (intc_unit_ != nullptr) {
        intc_unit_->set_pending_internal(cause);
    }
    InterruptRequest request;
    request.kind = InterruptRequest::Kind::Intc;
    request.number = cause;
    interrupt_queue_.push_back(request);
}

void Kernel::queue_dmac_completion(std::uint32_t channel) {
    // One status bit per channel (D_STAT CIS): a completion that is
    // already pending stays a single entry in place, like the INTC path.
    for (const InterruptRequest& entry : interrupt_queue_) {
        if (entry.kind == InterruptRequest::Kind::Dmac
            && entry.number == channel) {
            if (dmac_unit_ != nullptr) {
                dmac_unit_->set_completion_internal(channel);
            }
            return;
        }
    }
    if (dmac_unit_ != nullptr) {
        dmac_unit_->set_completion_internal(channel);
    }
    InterruptRequest request;
    request.kind = InterruptRequest::Kind::Dmac;
    request.number = channel;
    interrupt_queue_.push_back(request);
}

void Kernel::maybe_send_originating_packet(GuestState& state) {
    // One shot per boot; a resumed boot that already delivered keeps its
    // flag through the snapshot below.
    if (originating_packet_sent_) {
        return;
    }
    // The game must have registered the pump (its DMAC channel-5
    // handler): without a consumer the bytes would sit unread.
    bool pump_registered = false;
    for (const KernelInterruptHandler& registration : dmac_handlers_) {
        if (registration.cause == sif_channel_dmac) {
            pump_registered = true;
        }
    }
    if (!pump_registered) {
        return;
    }
    // Additive only: never overwrite live traffic the game queued, and
    // never deliver into an unpopulated dispatch table (the pump would
    // drain the packet and skip the empty entry).
    if (!state.memory().contains(originating_queue_pointer, 4)) {
        return;
    }
    const std::uint32_t queue =
        state.memory().read_word(originating_queue_pointer) & 0x1fffffffu;
    constexpr std::size_t packet_span = 24;
    if (!state.memory().contains(queue, packet_span)) {
        return;
    }
    if (state.memory().read_byte(queue) != 0) {
        return;
    }
    if (!state.memory().contains(originating_dispatch_table, 4)) {
        return;
    }
    const std::uint32_t table =
        state.memory().read_word(originating_dispatch_table);
    if (!state.memory().contains(table + originating_dispatch_entry, 4)
        || state.memory().read_word(table + originating_dispatch_entry) == 0) {
        return;
    }
    // Count byte plus words {0, 1, 0, register, value}: the pump's
    // table dispatches word 2 (masked) to the SET_SREG register writer,
    // which stores the value at software register `register`.
    state.memory().write_byte(queue, originating_packet_count);
    state.memory().write_word(queue + 4, 0);
    state.memory().write_word(queue + 8, sif_command_cid_set_sreg & 0x7fffffffu);
    state.memory().write_word(queue + 12, 0);
    state.memory().write_word(queue + 16, originating_packet_register);
    state.memory().write_word(queue + 20, originating_packet_value);
    queue_dmac_completion(sif_channel_dmac);
    originating_packet_sent_ = true;
}

void Kernel::raise_interrupt(std::uint32_t cause) {
    queue_interrupt(cause);
}

void Kernel::raise_dmac_completion(std::uint32_t channel) {
    queue_dmac_completion(channel);
}

bool Kernel::start_interrupt(GuestState& state) {
    if (handler_active()) {
        // A handler is still running. The model does not nest injections:
        // the handler frame clears EIE and the kernel's handlers run with
        // nesting disabled, so a queued cause waits for the return.
        return false;
    }
    if (interrupt_queue_.empty()) {
        return false;
    }
    // Peek, never pop, until the cause proves eligible. An occurrence
    // without a registered handler, with a closed mask, or behind a closed
    // CP0 gate stays queued with its status bit set: pending exists before
    // delivery, and enabling later delivers what is still pending.
    const InterruptRequest request = interrupt_queue_.front();
    const bool is_dmac = request.kind == InterruptRequest::Kind::Dmac;
    // Handler and argument travel as one unit from here on: two
    // registrations for one cause keep their own arguments through the
    // chain (decision 0031).
    std::vector<InterruptHandlerCall> calls;
    if (is_dmac) {
        for (const KernelInterruptHandler& registration : dmac_handlers_) {
            if (registration.cause == request.number) {
                InterruptHandlerCall call;
                call.handler = registration.handler;
                call.argument = registration.argument;
                calls.push_back(call);
            }
        }
    } else {
        for (const KernelInterruptHandler& registration : interrupt_handlers_) {
            if (registration.cause == request.number) {
                InterruptHandlerCall call;
                call.handler = registration.handler;
                call.argument = registration.argument;
                calls.push_back(call);
            }
        }
    }
    if (calls.empty()) {
        // No handler registered for the cause: the occurrence stays
        // represented in the status bit and the queue; nothing is called.
        return false;
    }
    if (is_dmac ? !dmac_mask_allows(request.number)
                : !intc_mask_allows(request.number)) {
        // Masked out: pending is conserved, dispatch is refused.
        return false;
    }
    if (!cpu_gate_allows(state, is_dmac)) {
        // The CPU cannot take interrupts now: pending is conserved.
        return false;
    }
    // Eligible: the status bits were set when the occurrence was queued
    // (never here through a guest path), so pop and inject.
    interrupt_queue_.erase(interrupt_queue_.begin());
    return inject_interrupt(state, request.number, std::move(calls));
}

bool Kernel::deliver_idle_interrupt(GuestState& state) {
    if (idle_interrupts_ >= idle_interrupt_budget) {
        // The budget bounds consecutive idle interrupts that changed
        // nothing; the driver reports the no-runnable-thread boundary.
        return false;
    }
    maybe_send_originating_packet(state);
    // The timers and the frame's VBlank advance together through the
    // unified machine: one frame-sized delta completes exactly one frame
    // on the shared accumulator (it always rests below one frame), so the
    // registered VBlank handler gets exactly the one opportunity per idle
    // call the old explicit source offered.
    advance_timers(state);
    if (interrupt_queue_.empty()) {
        return false;
    }
    if (!start_interrupt(state)) {
        // Nothing was delivered (for example a handler is still running);
        // the cause stays queued for the next boundary.
        return false;
    }
    ++idle_interrupts_;
    return true;
}

void Kernel::advance_timers(GuestState& state) {
    // One frame of the unified time base. The quantum stays the P03-owned
    // approximation (one frame per idle call); what changed is the
    // mechanism: the counting now runs through the same machine as the
    // service clock, sharing the per-timer remainders and the VBlank
    // accumulator (decision 0030).
    advance_busclk(state, busclk_per_frame);
}

void Kernel::advance_busclk(GuestState& state, std::uint32_t busclk_ticks) {
    // The single advance machine behind both quanta. Each enabled timer
    // takes its clock's share of the BUSCLK delta: BUSCLK, BUSCLK/16,
    // BUSCLK/256 and the horizontal-blank rate (NTSC) for CLKS values
    // 0-3. The HBLANK selector divides by 9372, the integer nearest the
    // true BUSCLK/HBLANK ratio (147456000/15734 = 9371.6...); the shared
    // remainder absorbs the drift, and the long-run rate is a
    // deterministic model policy, not a hardware claim. The remainder
    // keeps the division exact across any split of the delta, whichever
    // path contributed each piece. The TimerUnit moves the 16-bit counter
    // with wrap and sets EQUF only on a real crossing of COMP (a compare
    // behind the counter waits for the next wrap) and OVFF only on a real
    // wrap; both edges in one advance are reported together and queue the
    // timer's single INTC cause, coalesced like any other occurrence. A
    // handler that reprograms COMP or acknowledges a flag between two
    // advances steers the next one, because every advance reads the live
    // registers.
    (void)state;
    if (timer_unit_ != nullptr) {
        static constexpr std::uint32_t clock_divisors[4] = {1u, 16u, 256u, 9372u};
        for (std::uint32_t index = 0; index < 4; ++index) {
            const std::uint32_t base = timer_window_physical + index * timer_stride;
            const std::uint32_t mode =
                timer_unit_->register_value(base + timer_mode_offset);
            if ((mode & timer_count_enable) == 0) {
                continue;  // not counting
            }
            const std::uint32_t divisor = clock_divisors[mode & 3u];
            timer_remainders_[index] += busclk_ticks;
            const std::uint32_t step = timer_remainders_[index] / divisor;
            timer_remainders_[index] %= divisor;
            if (step == 0) {
                continue;
            }
            const TimerUnit::TimerAdvance advanced =
                timer_unit_->add_ticks(index, step);
            if (advanced.compare_edge || advanced.overflow_edge) {
                queue_interrupt(9u + index);
            }
        }
    }
    // One VBlank per frame of accumulated time, with the same registration
    // rule as the old idle source: the accumulator is shared, so service
    // slices and idle frames complete each other's frames.
    busclk_accumulator_ += busclk_ticks;
    while (busclk_accumulator_ >= busclk_per_frame) {
        busclk_accumulator_ -= busclk_per_frame;
        for (const KernelInterruptHandler& registration : interrupt_handlers_) {
            if (registration.cause == vblank_cause) {
                queue_interrupt(vblank_cause);
                break;
            }
        }
    }
}

void Kernel::advance_service_time(GuestState& state) {
    // One handled service advances the model's time base by one millisecond
    // of BUSCLK ticks — the unit the game's delay library schedules in (its
    // timer nodes' base values are BUSCLK ticks of elapsed time). The
    // advance is tied to services because both engines handle the same
    // service sequence in the same order, which keeps the differential
    // exact; a cycle-accurate clock is out of scope (decision 0016). The
    // quantum is unchanged by P03: only the mechanism is now shared with
    // the idle path (decision 0030).
    advance_busclk(state, service_time_slice);
}

bool Kernel::inject_interrupt(GuestState& state, std::uint32_t cause,
                              std::vector<InterruptHandlerCall> calls) {
    if (calls.empty()) {
        return false;
    }
    const RegisterContext interrupted = state.save_registers();
    // An injection over anything but a running thread is an
    // interrupted-idle context: record the idle thread, never the id of
    // whichever thread blocked last (decision 0031). The same predicate
    // answers GetThreadId, so the frame and the query agree by
    // construction.
    const KernelThread* current = current_thread();
    DeferredCall call;
    call.kind = DeferredCall::Kind::Interrupt;
    call.thread_id = (current != nullptr && current->status == ThreadRun)
        ? current->id : idle_thread_id;
    call.cause = cause;
    call.handlers = std::move(calls);
    call.next_handler = 0;
    call.context = interrupted;
    deferred_calls_.push_back(call);
    install_handler_frame(state, cause, call.handlers[0], interrupted);
    return true;
}

void Kernel::install_handler_frame(GuestState& state, std::uint32_t cause,
                                   const InterruptHandlerCall& call,
                                   const RegisterContext& interrupted) {
    // The handler returns through the model's stub, like a patched syscall.
    state.memory().write_word(patch_return_stub_physical, 0x24030100u);
    state.memory().write_word(patch_return_stub_physical + 4, 0x0000000Cu);
    // The BIOS handler frame (a0, a1, a2) = (cause, argument, pc):
    //   a0 is the INTC cause or DMAC channel;
    //   a1 is the registration's fourth word, kept as a unit with the
    //     handler: the 2-suffixed forms prototype their handlers as
    //     (cause, arg, addr) with a void *arg parameter (ps2sdk
    //     kernel.h), and both forms share the syscall number (ps2sdk
    //     syscallnr.h), so every registration stores a3 and the
    //     dispatcher always passes it on;
    //   a2 is the interrupted pc: ps2sdk's own TIM2 handler forwards its
    //     third argument into the alarm callback slot the timer header
    //     names pc_value (timer.c TimerHandler_callback into timer.h
    //     timer_alarm_handler_t), which only fits a pc.
    RegisterContext frame;
    frame.pc = call.handler;
    frame.gpr[4] = cause;                          // a0 = cause
    frame.gpr[5] = call.argument;                  // a1 = registration argument
    frame.gpr[6] = interrupted.pc;                 // a2 = interrupted pc
    frame.gpr[28] = interrupted.gpr[28];           // gp as interrupted
    frame.gpr[29] = interrupted.gpr[29];           // sp as interrupted
    frame.gpr[31] = patch_return_stub_physical;
    frame.cp0 = interrupted.cp0;
    frame.cp0[12] &= ~0x00010000u;                 // EIE clear while handling
    state.restore_registers(frame);
}

void Kernel::copy_guest_bytes(GuestState& state, std::uint32_t source,
                              std::uint32_t destination, std::uint32_t size) {
    if (!state.memory().contains(source, size)
        || !state.memory().contains(destination, size)) {
        throw std::runtime_error(
            "A SifSetDma transfer leaves the mapped guest memory");
    }
    for (std::uint32_t offset = 0; offset < size; ++offset) {
        state.memory().write_byte(destination + offset,
                                  state.memory().read_byte(source + offset));
    }
}

void Kernel::run_iop_stub(GuestState& state, std::uint32_t command_buffer,
                          std::uint32_t size) {
    // The outgoing SIFCMD header: psize/dsize, dest, cid, opt. The model IOP
    // recognizes the SIFCMD init handshake the boot performs: the reply is a
    // SET_SREG packet that flips the game's RPCINIT software register, sent
    // back through the EE buffer named in the request and announced by the
    // SIF0 DMA interrupt. Other commands are transferred without a reply,
    // exactly as an IOP that does not implement them.
    if (size < 20) {
        return;
    }
    const std::uint32_t cid = state.memory().read_word(command_buffer + 8);
    if (cid != sif_command_cid_init_cmd) {
        if (cid == sif_command_cid_rpc_bind) {
            answer_sif_rpc_bind(state, command_buffer, size);
        } else if (cid == sif_command_cid_rpc_call) {
            answer_sif_rpc_call(state, command_buffer, size);
        } else if (cid == sif_command_cid_reset_cmd) {
            answer_sif_reset(state, command_buffer, size);
        } else if (cid == sif_command_cid_set_sreg) {
            answer_sif_set_sreg(state, command_buffer, size);
        }
        return;
    }
    const std::uint32_t reply_buffer = state.memory().read_word(command_buffer + 16);
    if (!state.memory().contains(reply_buffer, 24)) {
        throw std::runtime_error(
            "The SIFCMD init reply buffer is outside the mapped guest memory");
    }
    // The EE's receive buffer for every IOP command reply.
    ee_command_buffer_ = reply_buffer;
    state.memory().write_word(reply_buffer + 0, 24);  // psize (dsize remains 0)
    state.memory().write_word(reply_buffer + 4, 0);   // dest
    state.memory().write_word(reply_buffer + 8, sif_command_cid_set_sreg);
    state.memory().write_word(reply_buffer + 12, 0);  // opt
    state.memory().write_word(reply_buffer + 16, sif_sreg_rpcinit);
    state.memory().write_word(reply_buffer + 20, 1);
    queue_dmac_completion(sif_channel_dmac);
}

void Kernel::answer_sif_set_sreg(GuestState& state, std::uint32_t command_buffer,
                                 std::uint32_t size) {
    // The IOP mirrors a software register back to the EE. The game's command
    // layer init (0x00590978) sends SET_SREG{sreg 1, value 1} and then spins
    // until its own register 1 is non-zero (0x00590A18); only an incoming
    // SET_SREG can write it — the library's system handler at 0x005B0850
    // stores the packet's words into the register array at 0x008869C0. The
    // reply is the same 24-byte packet through the EE command buffer the
    // INIT_CMD handshake announced. See decision 0015 and the slice 14
    // evidence document.
    if (size < 24 || ee_command_buffer_ == 0) {
        return;
    }
    if (!state.memory().contains(ee_command_buffer_, 24)) {
        throw std::runtime_error(
            "The SIFCMD command reply buffer is outside the mapped guest memory");
    }
    state.memory().write_word(ee_command_buffer_ + 0, 24);  // psize
    state.memory().write_word(ee_command_buffer_ + 4, 0);   // dest
    state.memory().write_word(ee_command_buffer_ + 8, sif_command_cid_set_sreg);
    state.memory().write_word(ee_command_buffer_ + 12, 0);  // opt
    state.memory().write_word(ee_command_buffer_ + 16,
                              state.memory().read_word(command_buffer + 16));
    state.memory().write_word(ee_command_buffer_ + 20,
                              state.memory().read_word(command_buffer + 20));
    queue_dmac_completion(sif_channel_dmac);
}

void Kernel::set_disc_files(const DiscFiles* files) noexcept {
    disc_files_ = files;
    disc_files_by_handle_.clear();
    next_disc_handle_ = 1;
}

const DiscFiles* Kernel::disc_files() const noexcept {
    return disc_files_;
}

void Kernel::set_disc_sectors(const DiscByteSource* sectors) noexcept {
    disc_sectors_ = sectors;
    disc_volume_lba_ = 0;
}

const DiscByteSource* Kernel::disc_sectors() const noexcept {
    return disc_sectors_;
}

std::uint32_t Kernel::answer_disc_read(GuestState& state,
                                       std::uint32_t request) {
    // The game's own CD driver (the PCDV server, sid 0x50434456) reads the
    // disc itself: the request is {LBA, byte count, EE destination} — the
    // boot's first read is LBA 0x10 (the ISO9660 primary volume descriptor,
    // whose "CD001" signature the library checks at 0x00548E90) for 0x800
    // bytes into 0x0084E080. The model copies the sectors from the image
    // exactly as the drive would; without a disc the read answers zeros.
    constexpr std::uint32_t sector_size = 2048;
    const std::uint32_t lba = state.memory().read_word(request + 0);
    const std::uint32_t read_size = state.memory().read_word(request + 4);
    const std::uint32_t destination = state.memory().read_word(request + 8);
    if (read_size == 0 || read_size > 0x100000
        || !state.memory().contains(destination, read_size)) {
        return 0;
    }
    std::vector<std::uint8_t> data(read_size, 0);
    if (disc_sectors_ != nullptr) {
        const std::uint64_t offset =
            static_cast<std::uint64_t>(lba) * sector_size;
        if (offset + read_size > disc_sectors_->size()) {
            throw std::runtime_error(
                "The game's disc read leaves the disc image");
        }
        disc_sectors_->read(offset, data);
    }
    for (std::uint32_t index = 0; index < read_size; ++index) {
        state.memory().write_byte(destination + index, data[index]);
    }
    return read_size;
}

std::uint32_t Kernel::answer_prts_read(GuestState& state,
                                       std::uint32_t request) {
    // The block read: {LBA, byte count, flags}. The cache keeps the sectors
    // and answers the handle the copy-out calls use; the client checks the
    // reply is non-zero before it builds its file object, so a missing
    // block is answered with zero exactly like a failed transfer.
    constexpr std::uint32_t sector_size = 2048;
    const std::uint32_t lba = state.memory().read_word(request + 0);
    const std::uint32_t read_size = state.memory().read_word(request + 4);
    if (read_size == 0 || read_size > 0x100000 || disc_sectors_ == nullptr) {
        return 0;
    }
    const std::uint64_t offset = static_cast<std::uint64_t>(lba) * sector_size;
    if (offset + read_size > disc_sectors_->size()) {
        throw std::runtime_error(
            "The game's block cache read leaves the disc image");
    }
    PrtsBlock block;
    block.lba = lba;
    block.data.resize(read_size);
    disc_sectors_->read(offset, block.data);
    const std::uint32_t handle = next_prts_handle_++;
    if (prts_blocks_.size() >= 8) {
        prts_blocks_.erase(prts_blocks_.begin());
    }
    prts_blocks_[handle] = std::move(block);
    return handle;
}

std::uint32_t Kernel::answer_prts_copy(GuestState& state,
                                       std::uint32_t request) {
    // The copy-out: {handle, EE destination, byte count}. The block is
    // consumed sequentially from the handle's cursor, which advances past
    // the copied bytes.
    const std::uint32_t handle = state.memory().read_word(request + 0);
    const std::uint32_t destination = state.memory().read_word(request + 4);
    const std::uint32_t size = state.memory().read_word(request + 8);
    const auto entry = prts_blocks_.find(handle);
    if (entry == prts_blocks_.end()) {
        return 0;
    }
    const std::uint32_t available =
        static_cast<std::uint32_t>(entry->second.data.size());
    if (entry->second.cursor >= available) {
        return 0;
    }
    const std::uint32_t remaining = available - entry->second.cursor;
    const std::uint32_t copy_size = size < remaining ? size : remaining;
    if (copy_size == 0 || !state.memory().contains(destination, copy_size)) {
        return 0;
    }
    for (std::uint32_t index = 0; index < copy_size; ++index) {
        state.memory().write_byte(
            destination + index,
            entry->second.data[entry->second.cursor + index]);
    }
    entry->second.cursor += copy_size;
    return copy_size;
}

namespace {

// The snapshot codec twins the one in checkpoint.cpp (same put_u32 and
// Reader shape): both must keep the RegisterContext field order the
// ee_checkpoint header documents, which the unit test below pins through
// a context round-trip on each side.
void put_u32(std::vector<std::uint8_t>& out, std::uint32_t value) {
    out.push_back(static_cast<std::uint8_t>(value & 0xFFu));
    out.push_back(static_cast<std::uint8_t>((value >> 8) & 0xFFu));
    out.push_back(static_cast<std::uint8_t>((value >> 16) & 0xFFu));
    out.push_back(static_cast<std::uint8_t>((value >> 24) & 0xFFu));
}

void put_u64(std::vector<std::uint8_t>& out, std::uint64_t value) {
    put_u32(out, static_cast<std::uint32_t>(value & 0xFFFFFFFFull));
    put_u32(out, static_cast<std::uint32_t>((value >> 32) & 0xFFFFFFFFull));
}

void put_context(std::vector<std::uint8_t>& out, const RegisterContext& context) {
    for (const std::uint64_t value : context.gpr) {
        put_u64(out, value);
    }
    for (const std::uint64_t value : context.gpr_high) {
        put_u64(out, value);
    }
    for (const std::uint32_t value : context.fpr) {
        put_u32(out, value);
    }
    put_u64(out, context.hi);
    put_u64(out, context.lo);
    put_u64(out, context.hi1);
    put_u64(out, context.lo1);
    put_u32(out, context.fpu_accumulator);
    put_u32(out, context.fpu_control);
    put_u32(out, context.shift_amount_cache);
    for (const std::uint32_t value : context.cp0) {
        put_u32(out, value);
    }
    for (const auto& lanes : context.vu0_vf) {
        for (const std::uint32_t lane : lanes) {
            put_u32(out, lane);
        }
    }
    for (const std::uint32_t value : context.vu0_vi) {
        put_u32(out, value);
    }
    put_u32(out, context.vu0_clip_flag);
    for (const std::uint32_t lane : context.vu0_acc) {
        put_u32(out, lane);
    }
    put_u32(out, context.vu0_mac_flag);
    put_u32(out, context.vu0_status_flag);
    put_u32(out, context.pc);
}

struct Reader {
    std::span<const std::uint8_t> bytes;
    std::size_t offset = 0;

    std::uint8_t take_byte() {
        if (offset >= bytes.size()) {
            throw std::runtime_error("The kernel snapshot ends mid-value");
        }
        return bytes[offset++];
    }

    std::uint32_t take_u32() {
        const std::uint32_t low = take_byte();
        const std::uint32_t mid_low = take_byte();
        const std::uint32_t mid_high = take_byte();
        const std::uint32_t high = take_byte();
        return low | (mid_low << 8) | (mid_high << 16) | (high << 24);
    }

    std::uint64_t take_u64() {
        const std::uint64_t low = take_u32();
        const std::uint64_t high = take_u32();
        return low | (high << 32);
    }

    RegisterContext take_context() {
        RegisterContext context;
        for (std::uint64_t& value : context.gpr) {
            value = take_u64();
        }
        for (std::uint64_t& value : context.gpr_high) {
            value = take_u64();
        }
        for (std::uint32_t& value : context.fpr) {
            value = take_u32();
        }
        context.hi = take_u64();
        context.lo = take_u64();
        context.hi1 = take_u64();
        context.lo1 = take_u64();
        context.fpu_accumulator = take_u32();
        context.fpu_control = take_u32();
        context.shift_amount_cache = take_u32();
        for (std::uint32_t& value : context.cp0) {
            value = take_u32();
        }
        for (auto& lanes : context.vu0_vf) {
            for (std::uint32_t& lane : lanes) {
                lane = take_u32();
            }
        }
        for (std::uint32_t& value : context.vu0_vi) {
            value = take_u32();
        }
        context.vu0_clip_flag = take_u32();
        for (std::uint32_t& lane : context.vu0_acc) {
            lane = take_u32();
        }
        context.vu0_mac_flag = take_u32();
        context.vu0_status_flag = take_u32();
        context.pc = take_u32();
        return context;
    }
};

} // namespace

std::vector<std::uint8_t> Kernel::save_kernel_state() const {
    std::vector<std::uint8_t> out;
    for (const char letter : {'G', 'T', '4', 'K', 'E', 'R', 'N', '1'}) {
        out.push_back(static_cast<std::uint8_t>(letter));
    }
    const auto put_size = [&out](std::size_t count) {
        if (count > 0xFFFFFFFFu) {
            throw std::runtime_error("The kernel snapshot holds too many entries");
        }
        put_u32(out, static_cast<std::uint32_t>(count));
    };
    put_size(threads_.size());
    for (const KernelThread& thread : threads_) {
        put_u32(out, thread.id);
        put_u32(out, thread.status);
        put_u32(out, thread.function);
        put_u32(out, thread.stack);
        put_u32(out, thread.stack_size);
        put_u32(out, thread.gp);
        put_u32(out, static_cast<std::uint32_t>(thread.initial_priority));
        put_u32(out, static_cast<std::uint32_t>(thread.current_priority));
        put_u32(out, thread.attr);
        put_u32(out, thread.option);
        put_u32(out, thread.wait_type);
        put_u32(out, thread.wait_id);
        put_u32(out, thread.wakeup_count);
        put_context(out, thread.context);
    }
    put_size(semaphores_.size());
    for (const KernelSemaphore& semaphore : semaphores_) {
        put_u32(out, semaphore.id);
        put_u32(out, static_cast<std::uint32_t>(semaphore.count));
        put_u32(out, static_cast<std::uint32_t>(semaphore.max_count));
        put_u32(out, static_cast<std::uint32_t>(semaphore.init_count));
        put_u32(out, static_cast<std::uint32_t>(semaphore.wait_threads));
        put_u32(out, semaphore.attr);
        put_u32(out, semaphore.option);
    }
    put_u32(out, next_thread_id_);
    put_u32(out, next_semaphore_id_);
    put_u32(out, current_thread_id_);
    put_u32(out, syscall_table_ready_ ? 1u : 0u);
    for (const std::uint32_t entry : patched_handlers_) {
        put_u32(out, entry);
    }
    put_u32(out, osd_config_);
    for (const std::uint8_t byte : osd_config2_) {
        out.push_back(byte);
    }
    put_u64(out, gs_imr_);
    put_size(deferred_calls_.size());
    for (const DeferredCall& call : deferred_calls_) {
        put_u32(out, call.kind == DeferredCall::Kind::Interrupt ? 1u : 0u);
        put_u32(out, call.resume_pc);
        put_u32(out, call.caller_ra);
        put_u32(out, call.thread_id);
        put_u32(out, call.cause);
        // Handler and argument ride as pairs (decision 0031); the
        // interrupt_model version gates pre-change blobs before parsing.
        put_size(call.handlers.size());
        for (const InterruptHandlerCall& handler_call : call.handlers) {
            put_u32(out, handler_call.handler);
            put_u32(out, handler_call.argument);
        }
        put_u64(out, static_cast<std::uint64_t>(call.next_handler));
        put_context(out, call.context);
    }
    put_u32(out, next_handler_id_);
    put_size(sif_software_registers_.size());
    for (const auto& [index, value] : sif_software_registers_) {
        put_u32(out, index);
        put_u32(out, value);
    }
    put_size(interrupt_queue_.size());
    for (const InterruptRequest& request : interrupt_queue_) {
        put_u32(out, request.kind == InterruptRequest::Kind::Dmac ? 1u : 0u);
        put_u32(out, request.number);
    }
    const auto put_handlers = [&out, &put_size](
                                  const std::vector<KernelInterruptHandler>& list) {
        put_size(list.size());
        for (const KernelInterruptHandler& registration : list) {
            put_u32(out, registration.id);
            put_u32(out, registration.cause);
            put_u32(out, registration.handler);
            put_u32(out, registration.argument);
        }
    };
    put_handlers(interrupt_handlers_);
    put_handlers(dmac_handlers_);
    put_u32(out, next_dma_id_);
    put_u32(out, sif_ready_ ? 1u : 0u);
    put_u32(out, ee_command_buffer_);
    put_size(sif_rpc_servers_.size());
    for (const auto& [sid, server] : sif_rpc_servers_) {
        put_u32(out, sid);
        put_u32(out, server.handle);
        put_u32(out, server.buffer);
        put_u32(out, server.connection_buffer);
    }
    if (sif_iop_image_.size() > 0xFFFFFFFFu) {
        throw std::runtime_error("The kernel snapshot holds a wild image name");
    }
    put_u32(out, static_cast<std::uint32_t>(sif_iop_image_.size()));
    out.insert(out.end(), sif_iop_image_.begin(), sif_iop_image_.end());
    put_u32(out, sif_reboot_pending_ ? 1u : 0u);
    put_u32(out, idle_interrupts_);
    // Wire order frozen since model 2: the accumulator word, then the four
    // per-timer remainders. P03 only widened their meaning from
    // service-only to both paths (decision 0030).
    put_u32(out, busclk_accumulator_);
    for (const std::uint32_t remainder : timer_remainders_) {
        put_u32(out, remainder);
    }
    put_u32(out, disc_volume_lba_);
    put_size(disc_files_by_handle_.size());
    for (const auto& [handle, path] : disc_files_by_handle_) {
        if (path.size() > 0xFFFFFFFFu) {
            throw std::runtime_error("The kernel snapshot holds a wild path");
        }
        put_u32(out, handle);
        put_u32(out, static_cast<std::uint32_t>(path.size()));
        out.insert(out.end(), path.begin(), path.end());
    }
    put_u32(out, next_disc_handle_);
    put_size(prts_blocks_.size());
    for (const auto& [handle, block] : prts_blocks_) {
        if (block.data.size() > 0xFFFFFFFFu) {
            throw std::runtime_error("The kernel snapshot holds a wild block");
        }
        put_u32(out, handle);
        put_u32(out, block.lba);
        put_u32(out, static_cast<std::uint32_t>(block.data.size()));
        out.insert(out.end(), block.data.begin(), block.data.end());
        put_u32(out, block.cursor);
    }
    put_u32(out, next_prts_handle_);
    put_u32(out, originating_packet_sent_ ? 1u : 0u);
    return out;
}

void Kernel::load_kernel_state(std::span<const std::uint8_t> bytes) {
    Reader reader{bytes, 0};
    constexpr char magic[8] = {'G', 'T', '4', 'K', 'E', 'R', 'N', '1'};
    for (const char letter : magic) {
        if (reader.take_byte() != static_cast<std::uint8_t>(letter)) {
            throw std::runtime_error("The kernel snapshot has a bad magic");
        }
    }
    const auto take_bool = [&reader](const char* what) {
        const std::uint32_t flag = reader.take_u32();
        if (flag > 1u) {
            throw std::runtime_error(what);
        }
        return flag == 1u;
    };
    // Parse into temporaries first so a malformed blob leaves the live
    // kernel untouched.
    std::vector<KernelThread> threads;
    const std::uint32_t thread_count = reader.take_u32();
    for (std::uint32_t index = 0; index < thread_count; ++index) {
        KernelThread thread;
        thread.id = reader.take_u32();
        thread.status = reader.take_u32();
        thread.function = reader.take_u32();
        thread.stack = reader.take_u32();
        thread.stack_size = reader.take_u32();
        thread.gp = reader.take_u32();
        thread.initial_priority = static_cast<std::int32_t>(reader.take_u32());
        thread.current_priority = static_cast<std::int32_t>(reader.take_u32());
        thread.attr = reader.take_u32();
        thread.option = reader.take_u32();
        thread.wait_type = reader.take_u32();
        thread.wait_id = reader.take_u32();
        thread.wakeup_count = reader.take_u32();
        thread.context = reader.take_context();
        threads.push_back(std::move(thread));
    }
    std::vector<KernelSemaphore> semaphores;
    const std::uint32_t semaphore_count = reader.take_u32();
    for (std::uint32_t index = 0; index < semaphore_count; ++index) {
        KernelSemaphore semaphore;
        semaphore.id = reader.take_u32();
        semaphore.count = static_cast<std::int32_t>(reader.take_u32());
        semaphore.max_count = static_cast<std::int32_t>(reader.take_u32());
        semaphore.init_count = static_cast<std::int32_t>(reader.take_u32());
        semaphore.wait_threads = static_cast<std::int32_t>(reader.take_u32());
        semaphore.attr = reader.take_u32();
        semaphore.option = reader.take_u32();
        semaphores.push_back(semaphore);
    }
    const std::uint32_t next_thread_id = reader.take_u32();
    const std::uint32_t next_semaphore_id = reader.take_u32();
    const std::uint32_t current_thread_id = reader.take_u32();
    const bool syscall_table_ready =
        take_bool("The kernel snapshot has a bad table flag");
    std::array<std::uint32_t, syscall_table_entries> patched_handlers{};
    for (std::uint32_t& entry : patched_handlers) {
        entry = reader.take_u32();
    }
    const std::uint32_t osd_config = reader.take_u32();
    std::array<std::uint8_t, 4> osd_config2{};
    for (std::uint8_t& byte : osd_config2) {
        byte = reader.take_byte();
    }
    const std::uint64_t gs_imr = reader.take_u64();
    std::vector<DeferredCall> deferred_calls;
    const std::uint32_t deferred_count = reader.take_u32();
    for (std::uint32_t index = 0; index < deferred_count; ++index) {
        DeferredCall call;
        const std::uint32_t kind = reader.take_u32();
        if (kind > 1u) {
            throw std::runtime_error("The kernel snapshot has a bad call kind");
        }
        call.kind = kind == 1u ? DeferredCall::Kind::Interrupt
                              : DeferredCall::Kind::Patch;
        call.resume_pc = reader.take_u32();
        call.caller_ra = reader.take_u32();
        call.thread_id = reader.take_u32();
        call.cause = reader.take_u32();
        const std::uint32_t handler_count = reader.take_u32();
        for (std::uint32_t handler = 0; handler < handler_count; ++handler) {
            InterruptHandlerCall handler_call;
            handler_call.handler = reader.take_u32();
            handler_call.argument = reader.take_u32();
            call.handlers.push_back(handler_call);
        }
        call.next_handler = static_cast<std::size_t>(reader.take_u64());
        call.context = reader.take_context();
        deferred_calls.push_back(std::move(call));
    }
    const std::uint32_t next_handler_id = reader.take_u32();
    std::map<std::uint32_t, std::uint32_t> sif_software_registers;
    const std::uint32_t register_count = reader.take_u32();
    for (std::uint32_t index = 0; index < register_count; ++index) {
        const std::uint32_t key = reader.take_u32();
        sif_software_registers[key] = reader.take_u32();
    }
    std::vector<InterruptRequest> interrupt_queue;
    const std::uint32_t queued_count = reader.take_u32();
    for (std::uint32_t index = 0; index < queued_count; ++index) {
        InterruptRequest request;
        const std::uint32_t kind = reader.take_u32();
        if (kind > 1u) {
            throw std::runtime_error("The kernel snapshot has a bad queue kind");
        }
        request.kind = kind == 1u ? InterruptRequest::Kind::Dmac
                                  : InterruptRequest::Kind::Intc;
        request.number = reader.take_u32();
        interrupt_queue.push_back(request);
    }
    const auto take_handlers = [&reader]() {
        std::vector<KernelInterruptHandler> list;
        const std::uint32_t count = reader.take_u32();
        for (std::uint32_t index = 0; index < count; ++index) {
            KernelInterruptHandler registration;
            registration.id = reader.take_u32();
            registration.cause = reader.take_u32();
            registration.handler = reader.take_u32();
            registration.argument = reader.take_u32();
            list.push_back(registration);
        }
        return list;
    };
    std::vector<KernelInterruptHandler> interrupt_handlers = take_handlers();
    std::vector<KernelInterruptHandler> dmac_handlers = take_handlers();
    const std::uint32_t next_dma_id = reader.take_u32();
    const bool sif_ready = take_bool("The kernel snapshot has a bad SIF flag");
    const std::uint32_t ee_command_buffer = reader.take_u32();
    std::map<std::uint32_t, SifRpcServer> sif_rpc_servers;
    const std::uint32_t server_count = reader.take_u32();
    for (std::uint32_t index = 0; index < server_count; ++index) {
        SifRpcServer server;
        const std::uint32_t sid = reader.take_u32();
        server.handle = reader.take_u32();
        server.buffer = reader.take_u32();
        server.connection_buffer = reader.take_u32();
        server.sid = sid;
        sif_rpc_servers[sid] = server;
    }
    const std::uint32_t image_size = reader.take_u32();
    if (image_size > reader.bytes.size() - reader.offset) {
        throw std::runtime_error("The kernel snapshot image overruns the blob");
    }
    const std::string sif_iop_image(
        reader.bytes.begin() + reader.offset,
        reader.bytes.begin() + reader.offset + image_size);
    reader.offset += image_size;
    const bool sif_reboot_pending =
        take_bool("The kernel snapshot has a bad reboot flag");
    const std::uint32_t idle_interrupts = reader.take_u32();
    const std::uint32_t busclk_accumulator = reader.take_u32();
    std::uint32_t timer_remainders[4] = {};
    for (std::uint32_t& remainder : timer_remainders) {
        remainder = reader.take_u32();
    }
    const std::uint32_t disc_volume_lba = reader.take_u32();
    std::map<std::uint32_t, std::string> disc_files_by_handle;
    const std::uint32_t file_count = reader.take_u32();
    for (std::uint32_t index = 0; index < file_count; ++index) {
        const std::uint32_t handle = reader.take_u32();
        const std::uint32_t path_size = reader.take_u32();
        if (path_size > reader.bytes.size() - reader.offset) {
            throw std::runtime_error("The kernel snapshot path overruns the blob");
        }
        disc_files_by_handle[handle] = std::string(
            reader.bytes.begin() + reader.offset,
            reader.bytes.begin() + reader.offset + path_size);
        reader.offset += path_size;
    }
    const std::uint32_t next_disc_handle = reader.take_u32();
    std::map<std::uint32_t, PrtsBlock> prts_blocks;
    const std::uint32_t block_count = reader.take_u32();
    for (std::uint32_t index = 0; index < block_count; ++index) {
        PrtsBlock block;
        const std::uint32_t handle = reader.take_u32();
        block.lba = reader.take_u32();
        const std::uint32_t block_size = reader.take_u32();
        if (block_size > reader.bytes.size() - reader.offset) {
            throw std::runtime_error("The kernel snapshot block overruns the blob");
        }
        block.data.assign(reader.bytes.begin() + reader.offset,
                          reader.bytes.begin() + reader.offset + block_size);
        reader.offset += block_size;
        block.cursor = reader.take_u32();
        if (block.cursor > block.data.size()) {
            throw std::runtime_error("The kernel snapshot cursor leaves the block");
        }
        prts_blocks[handle] = std::move(block);
    }
    const std::uint32_t next_prts_handle = reader.take_u32();
    // Checkpoints from before decision 0026 end here: they resume with
    // the packet unsent, which is correct because their queues never
    // held one. Anything longer must parse exactly.
    bool originating_packet_sent = false;
    if (reader.offset != reader.bytes.size()) {
        originating_packet_sent =
            take_bool("The kernel snapshot has a bad originating flag");
    }
    if (reader.offset != reader.bytes.size()) {
        throw std::runtime_error("The kernel snapshot has trailing bytes");
    }
    threads_ = std::move(threads);
    semaphores_ = std::move(semaphores);
    next_thread_id_ = next_thread_id;
    next_semaphore_id_ = next_semaphore_id;
    current_thread_id_ = current_thread_id;
    syscall_table_ready_ = syscall_table_ready;
    patched_handlers_ = patched_handlers;
    osd_config_ = osd_config;
    osd_config2_ = osd_config2;
    gs_imr_ = gs_imr;
    deferred_calls_ = std::move(deferred_calls);
    next_handler_id_ = next_handler_id;
    sif_software_registers_ = std::move(sif_software_registers);
    interrupt_queue_ = std::move(interrupt_queue);
    interrupt_handlers_ = std::move(interrupt_handlers);
    dmac_handlers_ = std::move(dmac_handlers);
    next_dma_id_ = next_dma_id;
    sif_ready_ = sif_ready;
    ee_command_buffer_ = ee_command_buffer;
    sif_rpc_servers_ = std::move(sif_rpc_servers);
    sif_iop_image_ = std::move(sif_iop_image_);
    sif_reboot_pending_ = sif_reboot_pending;
    idle_interrupts_ = idle_interrupts;
    busclk_accumulator_ = busclk_accumulator;
    for (std::size_t index = 0; index < 4; ++index) {
        timer_remainders_[index] = timer_remainders[index];
    }
    disc_volume_lba_ = disc_volume_lba;
    disc_files_by_handle_ = std::move(disc_files_by_handle);
    next_disc_handle_ = next_disc_handle;
    prts_blocks_ = std::move(prts_blocks);
    next_prts_handle_ = next_prts_handle;
    originating_packet_sent_ = originating_packet_sent;
}

std::optional<std::string> Kernel::describe_kernel_difference(
    const Kernel& other) const {
    // One helper per value shape; the first mismatch wins, so every
    // check below runs only while no difference is recorded yet.
    std::string difference;
    const auto fail32 = [&](const std::string& field, std::uint32_t left,
                            std::uint32_t right) {
        if (left != right && difference.empty()) {
            difference = "kernel " + field + ": left " + hex_text(left)
                + ", right " + hex_text(right);
        }
    };
    const auto fail_signed = [&](const std::string& field, std::int32_t left,
                                 std::int32_t right) {
        if (left != right && difference.empty()) {
            difference = "kernel " + field + ": left "
                + std::to_string(left) + ", right " + std::to_string(right);
        }
    };
    const auto find_thread = [](const std::vector<KernelThread>& threads,
                                std::uint32_t id) {
        for (const KernelThread& thread : threads) {
            if (thread.id == id) {
                return &thread;
            }
        }
        return static_cast<const KernelThread*>(nullptr);
    };
    if (threads_.size() != other.threads_.size()) {
        return "kernel thread count: left "
            + std::to_string(threads_.size()) + ", right "
            + std::to_string(other.threads_.size());
    }
    for (const KernelThread& thread : threads_) {
        const KernelThread* match = find_thread(other.threads_, thread.id);
        if (match == nullptr) {
            return "kernel thread " + std::to_string(thread.id)
                + " missing on the right";
        }
        const std::string owner = "thread " + std::to_string(thread.id);
        fail32(owner + " status", thread.status, match->status);
        fail32(owner + " function", thread.function, match->function);
        fail32(owner + " stack", thread.stack, match->stack);
        fail32(owner + " stack size", thread.stack_size, match->stack_size);
        fail32(owner + " gp", thread.gp, match->gp);
        fail_signed(owner + " initial priority", thread.initial_priority,
                    match->initial_priority);
        fail_signed(owner + " current priority", thread.current_priority,
                    match->current_priority);
        fail32(owner + " attr", thread.attr, match->attr);
        fail32(owner + " option", thread.option, match->option);
        fail32(owner + " wait type", thread.wait_type, match->wait_type);
        fail32(owner + " wait id", thread.wait_id, match->wait_id);
        fail32(owner + " wakeup count", thread.wakeup_count,
               match->wakeup_count);
        if (difference.empty()) {
            if (const std::optional<std::string> context =
                    compare_contexts(thread.context, match->context,
                                     ("kernel " + owner + " saved context")
                                         .c_str())) {
                return context;
            }
        }
        if (!difference.empty()) {
            return difference;
        }
    }
    for (const KernelThread& thread : other.threads_) {
        if (find_thread(threads_, thread.id) == nullptr) {
            return "kernel thread " + std::to_string(thread.id)
                + " missing on the left";
        }
    }
    const auto find_semaphore =
        [](const std::vector<KernelSemaphore>& semaphores, std::uint32_t id) {
            for (const KernelSemaphore& semaphore : semaphores) {
                if (semaphore.id == id) {
                    return &semaphore;
                }
            }
            return static_cast<const KernelSemaphore*>(nullptr);
        };
    if (semaphores_.size() != other.semaphores_.size()) {
        return "kernel semaphore count: left "
            + std::to_string(semaphores_.size()) + ", right "
            + std::to_string(other.semaphores_.size());
    }
    for (const KernelSemaphore& semaphore : semaphores_) {
        const KernelSemaphore* match =
            find_semaphore(other.semaphores_, semaphore.id);
        if (match == nullptr) {
            return "kernel semaphore " + std::to_string(semaphore.id)
                + " missing on the right";
        }
        const std::string owner =
            "semaphore " + std::to_string(semaphore.id);
        fail_signed(owner + " count", semaphore.count, match->count);
        fail_signed(owner + " max count", semaphore.max_count,
                    match->max_count);
        fail_signed(owner + " init count", semaphore.init_count,
                    match->init_count);
        fail_signed(owner + " wait threads", semaphore.wait_threads,
                    match->wait_threads);
        fail32(owner + " attr", semaphore.attr, match->attr);
        fail32(owner + " option", semaphore.option, match->option);
        if (!difference.empty()) {
            return difference;
        }
    }
    for (const KernelSemaphore& semaphore : other.semaphores_) {
        if (find_semaphore(semaphores_, semaphore.id) == nullptr) {
            return "kernel semaphore " + std::to_string(semaphore.id)
                + " missing on the left";
        }
    }
    fail32("next thread id", next_thread_id_, other.next_thread_id_);
    fail32("next semaphore id", next_semaphore_id_,
           other.next_semaphore_id_);
    fail32("current thread id", current_thread_id_,
           other.current_thread_id_);
    if (syscall_table_ready_ != other.syscall_table_ready_) {
        return std::string("kernel syscall table ready: left ")
            + (syscall_table_ready_ ? "set" : "clear") + ", right "
            + (other.syscall_table_ready_ ? "set" : "clear");
    }
    for (std::uint32_t index = 0; index < syscall_table_entries; ++index) {
        if (patched_handlers_[index] != other.patched_handlers_[index]) {
            return "kernel syscall patch " + std::to_string(index)
                + ": left " + hex_text(patched_handlers_[index])
                + ", right " + hex_text(other.patched_handlers_[index]);
        }
    }
    fail32("osd config", osd_config_, other.osd_config_);
    for (std::size_t index = 0; index < osd_config2_.size(); ++index) {
        if (osd_config2_[index] != other.osd_config2_[index]) {
            return "kernel osd config2 byte " + std::to_string(index)
                + ": left " + hex_byte(osd_config2_[index]) + ", right "
                + hex_byte(other.osd_config2_[index]);
        }
    }
    if (gs_imr_ != other.gs_imr_ && difference.empty()) {
        difference = "kernel gs imr: left " + hex_text64(gs_imr_)
            + ", right " + hex_text64(other.gs_imr_);
    }
    if (deferred_calls_.size() != other.deferred_calls_.size()) {
        return "kernel deferred call count: left "
            + std::to_string(deferred_calls_.size()) + ", right "
            + std::to_string(other.deferred_calls_.size());
    }
    for (std::size_t index = 0; index < deferred_calls_.size(); ++index) {
        const DeferredCall& call = deferred_calls_[index];
        const DeferredCall& match = other.deferred_calls_[index];
        const std::string owner =
            "deferred call " + std::to_string(index);
        const auto kind_name = [](DeferredCall::Kind kind) {
            return kind == DeferredCall::Kind::Interrupt ? "interrupt"
                                                        : "patch";
        };
        if (call.kind != match.kind && difference.empty()) {
            difference = "kernel " + owner + " kind: left "
                + kind_name(call.kind) + ", right "
                + kind_name(match.kind);
        }
        fail32(owner + " resume pc", call.resume_pc, match.resume_pc);
        fail32(owner + " caller ra", call.caller_ra, match.caller_ra);
        fail32(owner + " thread id", call.thread_id, match.thread_id);
        fail32(owner + " cause", call.cause, match.cause);
        if (call.handlers.size() != match.handlers.size()
            && difference.empty()) {
            difference = "kernel " + owner + " handler count: left "
                + std::to_string(call.handlers.size()) + ", right "
                + std::to_string(match.handlers.size());
        } else {
            for (std::size_t handler = 0;
                 handler < call.handlers.size() && difference.empty();
                 ++handler) {
                fail32(owner + " handler " + std::to_string(handler),
                       call.handlers[handler].handler,
                       match.handlers[handler].handler);
                fail32(owner + " handler " + std::to_string(handler)
                           + " argument",
                       call.handlers[handler].argument,
                       match.handlers[handler].argument);
            }
        }
        if (call.next_handler != match.next_handler && difference.empty()) {
            difference = "kernel " + owner + " next handler: left "
                + std::to_string(call.next_handler) + ", right "
                + std::to_string(match.next_handler);
        }
        if (difference.empty()) {
            if (const std::optional<std::string> context = compare_contexts(
                    call.context, match.context,
                    ("kernel " + owner + " saved context").c_str())) {
                return context;
            }
        }
        if (!difference.empty()) {
            return difference;
        }
    }
    fail32("next handler id", next_handler_id_, other.next_handler_id_);
    if (sif_software_registers_.size()
            != other.sif_software_registers_.size()) {
        return "kernel SIF software register count: left "
            + std::to_string(sif_software_registers_.size()) + ", right "
            + std::to_string(other.sif_software_registers_.size());
    }
    for (const auto& [index, value] : sif_software_registers_) {
        const auto found = other.sif_software_registers_.find(index);
        if (found == other.sif_software_registers_.end()) {
            return "kernel SIF software register " + hex_text(index)
                + " missing on the right";
        }
        fail32("SIF software register " + hex_text(index), value,
               found->second);
        if (!difference.empty()) {
            return difference;
        }
    }
    if (interrupt_queue_.size() != other.interrupt_queue_.size()) {
        return "kernel interrupt queue length: left "
            + std::to_string(interrupt_queue_.size()) + ", right "
            + std::to_string(other.interrupt_queue_.size());
    }
    for (std::size_t index = 0; index < interrupt_queue_.size(); ++index) {
        const InterruptRequest& request = interrupt_queue_[index];
        const InterruptRequest& match = other.interrupt_queue_[index];
        const std::string owner =
            "interrupt queue entry " + std::to_string(index);
        const auto queue_kind_name = [](InterruptRequest::Kind kind) {
            return kind == InterruptRequest::Kind::Dmac ? "dmac" : "intc";
        };
        if (request.kind != match.kind && difference.empty()) {
            difference = "kernel " + owner + " kind: left "
                + queue_kind_name(request.kind) + ", right "
                + queue_kind_name(match.kind);
        }
        fail32(owner + " number", request.number, match.number);
        if (!difference.empty()) {
            return difference;
        }
    }
    const auto compare_handlers = [&](const char* list,
                                      const std::vector<KernelInterruptHandler>&
                                          left,
                                      const std::vector<KernelInterruptHandler>&
                                          right)
        -> std::optional<std::string> {
        if (left.size() != right.size()) {
            return "kernel " + std::string(list) + " count: left "
                + std::to_string(left.size()) + ", right "
                + std::to_string(right.size());
        }
        for (std::size_t index = 0; index < left.size(); ++index) {
            const std::string owner =
                std::string(list) + " " + std::to_string(left[index].id);
            if (left[index].id != right[index].id
                || left[index].cause != right[index].cause
                || left[index].handler != right[index].handler
                || left[index].argument != right[index].argument) {
                if (left[index].cause != right[index].cause) {
                    return "kernel " + owner + " cause: left "
                        + hex_text(left[index].cause) + ", right "
                        + hex_text(right[index].cause);
                }
                if (left[index].handler != right[index].handler) {
                    return "kernel " + owner + " handler: left "
                        + hex_text(left[index].handler) + ", right "
                        + hex_text(right[index].handler);
                }
                if (left[index].argument != right[index].argument) {
                    return "kernel " + owner + " argument: left "
                        + hex_text(left[index].argument) + ", right "
                        + hex_text(right[index].argument);
                }
                return "kernel " + owner + " registration order differs";
            }
        }
        return std::nullopt;
    };
    if (const std::optional<std::string> handlers = compare_handlers(
            "interrupt handler", interrupt_handlers_,
            other.interrupt_handlers_)) {
        return handlers;
    }
    if (const std::optional<std::string> handlers = compare_handlers(
            "DMAC handler", dmac_handlers_, other.dmac_handlers_)) {
        return handlers;
    }
    fail32("next DMA id", next_dma_id_, other.next_dma_id_);
    if (sif_ready_ != other.sif_ready_) {
        return std::string("kernel SIF ready: left ")
            + (sif_ready_ ? "set" : "clear") + ", right "
            + (other.sif_ready_ ? "set" : "clear");
    }
    fail32("EE command buffer", ee_command_buffer_,
           other.ee_command_buffer_);
    if (sif_rpc_servers_.size() != other.sif_rpc_servers_.size()) {
        return "kernel SIF server count: left "
            + std::to_string(sif_rpc_servers_.size()) + ", right "
            + std::to_string(other.sif_rpc_servers_.size());
    }
    for (const auto& [sid, server] : sif_rpc_servers_) {
        const auto found = other.sif_rpc_servers_.find(sid);
        if (found == other.sif_rpc_servers_.end()) {
            return "kernel SIF server " + hex_text(sid)
                + " missing on the right";
        }
        const std::string owner = "SIF server " + hex_text(sid);
        fail32(owner + " handle", server.handle, found->second.handle);
        fail32(owner + " buffer", server.buffer, found->second.buffer);
        fail32(owner + " connection buffer", server.connection_buffer,
               found->second.connection_buffer);
        if (!difference.empty()) {
            return difference;
        }
    }
    if (sif_iop_image_ != other.sif_iop_image_) {
        return "kernel IOP image: left \"" + sif_iop_image_
            + "\", right \"" + other.sif_iop_image_ + "\"";
    }
    if (sif_reboot_pending_ != other.sif_reboot_pending_) {
        return std::string("kernel SIF reboot pending: left ")
            + (sif_reboot_pending_ ? "set" : "clear") + ", right "
            + (other.sif_reboot_pending_ ? "set" : "clear");
    }
    fail32("idle interrupt count", idle_interrupts_,
           other.idle_interrupts_);
    fail32("service clock accumulator", busclk_accumulator_,
           other.busclk_accumulator_);
    for (std::uint32_t index = 0; index < 4; ++index) {
        fail32("service clock remainder " + std::to_string(index),
               timer_remainders_[index], other.timer_remainders_[index]);
    }
    fail32("disc volume LBA", disc_volume_lba_, other.disc_volume_lba_);
    if (disc_files_by_handle_.size() != other.disc_files_by_handle_.size()) {
        return "kernel disc file count: left "
            + std::to_string(disc_files_by_handle_.size()) + ", right "
            + std::to_string(other.disc_files_by_handle_.size());
    }
    for (const auto& [handle, path] : disc_files_by_handle_) {
        const auto found = other.disc_files_by_handle_.find(handle);
        if (found == other.disc_files_by_handle_.end()) {
            return "kernel disc file handle " + std::to_string(handle)
                + " missing on the right";
        }
        if (path != found->second) {
            return "kernel disc file handle " + std::to_string(handle)
                + ": left \"" + path + "\", right \"" + found->second
                + "\"";
        }
    }
    fail32("next disc handle", next_disc_handle_,
           other.next_disc_handle_);
    if (prts_blocks_.size() != other.prts_blocks_.size()) {
        return "kernel block cache entry count: left "
            + std::to_string(prts_blocks_.size()) + ", right "
            + std::to_string(other.prts_blocks_.size());
    }
    for (const auto& [handle, block] : prts_blocks_) {
        const auto found = other.prts_blocks_.find(handle);
        if (found == other.prts_blocks_.end()) {
            return "kernel block cache handle " + std::to_string(handle)
                + " missing on the right";
        }
        const std::string owner =
            "block cache handle " + std::to_string(handle);
        fail32(owner + " LBA", block.lba, found->second.lba);
        if (block.data.size() != found->second.data.size()
            && difference.empty()) {
            difference = "kernel " + owner + " size: left "
                + std::to_string(block.data.size()) + ", right "
                + std::to_string(found->second.data.size());
        } else {
            for (std::size_t offset = 0;
                 offset < block.data.size() && difference.empty();
                 ++offset) {
                if (block.data[offset] != found->second.data[offset]) {
                    difference = "kernel " + owner + " byte "
                        + std::to_string(offset) + ": left "
                        + hex_byte(block.data[offset]) + ", right "
                        + hex_byte(found->second.data[offset]);
                }
            }
        }
        fail32(owner + " cursor", block.cursor, found->second.cursor);
        if (!difference.empty()) {
            return difference;
        }
    }
    fail32("next block cache handle", next_prts_handle_,
           other.next_prts_handle_);
    if (originating_packet_sent_ != other.originating_packet_sent_) {
        return std::string("kernel originating packet sent: left ")
            + (originating_packet_sent_ ? "set" : "clear") + ", right "
            + (other.originating_packet_sent_ ? "set" : "clear");
    }
    if (!difference.empty()) {
        return difference;
    }
    return std::nullopt;
}

std::uint32_t Kernel::answer_disc_volume(GuestState& state,
                                         std::uint32_t request) {
    // The game's CD library registers the volume descriptor its scan
    // accepted: 0x00548E20's success path calls 0x005485D8 with the block
    // and the checksum 0x00548D20 computed over it. The model recomputes
    // that checksum from the same image the reads come from; a mismatch
    // means the model's disc differs from the one the game read, and the
    // registration carries no reply to report it in, so it stops loudly.
    constexpr std::uint32_t sector_size = 2048;
    const std::uint32_t lba = state.memory().read_word(request + 0);
    const std::uint32_t checksum = state.memory().read_word(request + 4);
    if (disc_sectors_ == nullptr) {
        return 0;
    }
    const std::uint64_t offset = static_cast<std::uint64_t>(lba) * sector_size;
    if (offset + sector_size > disc_sectors_->size()) {
        throw std::runtime_error(
            "The game registered a volume outside the disc image");
    }
    std::vector<std::uint8_t> block(sector_size, 0);
    disc_sectors_->read(offset, block);
    // The library's sum: each byte times its one-based position, wrapping in
    // 32 bits exactly like the guest's accumulator.
    std::uint32_t computed = 0;
    for (std::uint32_t index = 0; index < sector_size; ++index) {
        computed += static_cast<std::uint32_t>(block[index]) * (index + 1);
    }
    if (computed != checksum) {
        throw std::runtime_error(
            "The game's volume checksum differs from the disc image's block");
    }
    disc_volume_lba_ = lba;
    return sector_size;
}

std::uint32_t Kernel::answer_disc_volume_size(std::uint8_t* result,
                                              std::uint32_t capacity) const {
    // The library's query wrapper (0x005487C0) reads the reply as {status,
    // value} and reports the value only when the status word is non-zero.
    constexpr std::uint32_t reply_size = 8;
    constexpr std::uint32_t sector_size = 2048;
    if (capacity < reply_size) {
        return 0;
    }
    for (std::uint32_t offset = 0; offset < reply_size; ++offset) {
        result[offset] = 0;
    }
    if (disc_sectors_ == nullptr || disc_volume_lba_ == 0) {
        return reply_size;
    }
    const std::uint64_t offset =
        static_cast<std::uint64_t>(disc_volume_lba_) * sector_size;
    if (offset + sector_size > disc_sectors_->size()) {
        throw std::runtime_error(
            "The registered volume lies outside the disc image");
    }
    std::vector<std::uint8_t> block(sector_size, 0);
    disc_sectors_->read(offset, block);
    const std::uint32_t volume_size =
        static_cast<std::uint32_t>(block[0x50])
        | (static_cast<std::uint32_t>(block[0x51]) << 8)
        | (static_cast<std::uint32_t>(block[0x52]) << 16)
        | (static_cast<std::uint32_t>(block[0x53]) << 24);
    const auto put_word = [result](std::uint32_t at, std::uint32_t value) {
        result[at + 0] = static_cast<std::uint8_t>(value);
        result[at + 1] = static_cast<std::uint8_t>(value >> 8);
        result[at + 2] = static_cast<std::uint8_t>(value >> 16);
        result[at + 3] = static_cast<std::uint8_t>(value >> 24);
    };
    put_word(0, 1);
    put_word(4, volume_size);
    return reply_size;
}

std::uint32_t Kernel::answer_file_open(GuestState& state, std::uint32_t request,
                                       std::uint32_t request_size,
                                       std::uint8_t* result,
                                       std::uint32_t capacity) {
    // The file server's open (sid 0x80000006, RPC 0). The 512-byte request
    // carries the path at +8 — the boot's first three were
    // "cdrom0:\IRX\SIO2MAN.IRX;1", "MCMAN.IRX" and "MCSERV.IRX" — and the
    // client reads the 16-byte reply as {handle, size}: a zero handle means
    // "not found" (the check at 0x005B6D6C returns 0xFFFEFFFD for it), any
    // other handle succeeds and the size is stored beside it. The bytes come
    // from the pinned disc image when the tool supplied one; without it the
    // open fails like a console without a disc (decision 0017).
    constexpr std::uint32_t reply_size = 16;
    if (capacity < reply_size || request_size < 16) {
        return 0;
    }
    std::string path;
    for (std::uint32_t offset = 8; offset < request_size && path.size() < 0x100;
         ++offset) {
        const char character =
            static_cast<char>(state.memory().read_byte(request + offset));
        if (character == '\0') {
            break;
        }
        path.push_back(character);
    }
    std::uint32_t handle = 0;
    std::uint32_t file_size = 0;
    if (disc_files_ != nullptr && !path.empty()) {
        const std::uint64_t found = disc_files_->file_size(path);
        if (found > 0 && found <= 0xFFFFFFFFull) {
            handle = next_disc_handle_++;
            file_size = static_cast<std::uint32_t>(found);
            disc_files_by_handle_[handle] = path;
        }
    }
    for (std::uint32_t offset = 0; offset < reply_size; ++offset) {
        result[offset] = 0;
    }
    const auto put_word = [result](std::uint32_t offset, std::uint32_t value) {
        result[offset + 0] = static_cast<std::uint8_t>(value);
        result[offset + 1] = static_cast<std::uint8_t>(value >> 8);
        result[offset + 2] = static_cast<std::uint8_t>(value >> 16);
        result[offset + 3] = static_cast<std::uint8_t>(value >> 24);
    };
    put_word(0, handle);
    put_word(4, file_size);
    return reply_size;
}

Kernel::SifRpcServer& Kernel::find_or_create_sif_server(std::uint32_t sid) {
    const auto found = sif_rpc_servers_.find(sid);
    if (found != sif_rpc_servers_.end()) {
        return found->second;
    }
    const std::uint32_t slot = static_cast<std::uint32_t>(sif_rpc_servers_.size());
    if (slot >= sif_iop_server_capacity) {
        throw std::runtime_error("The model IOP ran out of RPC server slots");
    }
    SifRpcServer server;
    server.sid = sid;
    server.handle = sif_iop_server_handles + slot * 0x40;
    server.buffer = sif_iop_server_buffers + slot * sif_iop_server_stride;
    server.connection_buffer = sif_iop_server_connections + slot * sif_iop_server_stride;
    const auto inserted = sif_rpc_servers_.emplace(sid, server);
    return inserted.first->second;
}

const Kernel::SifRpcServer* Kernel::find_sif_server_by_handle(
    std::uint32_t handle) const noexcept {
    for (const auto& entry : sif_rpc_servers_) {
        if (entry.second.handle == handle) {
            return &entry.second;
        }
    }
    return nullptr;
}

void Kernel::answer_sif_rpc_bind(GuestState& state, std::uint32_t command_buffer,
                                 std::uint32_t size) {
    // The request is a SifRpcBindPkt_t; the layout matched the public header
    // in the live packet dump (sid at +32, cd at +28, pkt_addr at +20).
    if (size < 36) {
        return;
    }
    if (ee_command_buffer_ == 0) {
        throw std::runtime_error(
            "The game sent an RPC bind before the SIFCMD init handshake");
    }
    const std::uint32_t rec_id = state.memory().read_word(command_buffer + 16);
    const std::uint32_t pkt_addr = state.memory().read_word(command_buffer + 20);
    const std::uint32_t rpc_id = state.memory().read_word(command_buffer + 24);
    const std::uint32_t cd = state.memory().read_word(command_buffer + 28);
    const std::uint32_t sid = state.memory().read_word(command_buffer + 32);
    const SifRpcServer& server = find_or_create_sif_server(sid);
    if (!state.memory().contains(ee_command_buffer_, 64)) {
        throw std::runtime_error(
            "The EE command buffer is outside the mapped guest memory");
    }
    record_rpc_bind(state, sid);
    // Answer with the SIFRPC end packet (SifRpcRendPkt_t, 64 bytes) the
    // client's bind wait is waiting for: its own handles echoed back plus a
    // non-null server handle and the model server's buffers.
    state.memory().write_word(ee_command_buffer_ + 0, 64);   // psize
    state.memory().write_word(ee_command_buffer_ + 4, 0);    // dest
    state.memory().write_word(ee_command_buffer_ + 8, sif_command_cid_rpc_end);
    state.memory().write_word(ee_command_buffer_ + 12, 0);   // opt
    state.memory().write_word(ee_command_buffer_ + 16, rec_id);
    state.memory().write_word(ee_command_buffer_ + 20, pkt_addr);
    state.memory().write_word(ee_command_buffer_ + 24, rpc_id);
    state.memory().write_word(ee_command_buffer_ + 28, cd);
    state.memory().write_word(ee_command_buffer_ + 32, sif_command_cid_rpc_bind);
    state.memory().write_word(ee_command_buffer_ + 36, server.handle);
    state.memory().write_word(ee_command_buffer_ + 40, server.buffer);
    state.memory().write_word(ee_command_buffer_ + 44, server.connection_buffer);
    for (std::uint32_t offset = 48; offset < 64; offset += 4) {
        state.memory().write_word(ee_command_buffer_ + offset, 0);
    }
    queue_dmac_completion(sif_channel_dmac);
}

void Kernel::answer_sif_rpc_call(GuestState& state, std::uint32_t command_buffer,
                                 std::uint32_t size) {
    // The request is a SifRpcCallPkt_t (layout confirmed in the live packet
    // dump: rpc_number at +32, recvbuf at +40, recv_size at +44, sd at +52).
    if (size < 56) {
        return;
    }
    if (ee_command_buffer_ == 0) {
        throw std::runtime_error(
            "The game sent an RPC call before the SIFCMD init handshake");
    }
    const std::uint32_t rec_id = state.memory().read_word(command_buffer + 16);
    const std::uint32_t pkt_addr = state.memory().read_word(command_buffer + 20);
    const std::uint32_t rpc_id = state.memory().read_word(command_buffer + 24);
    const std::uint32_t cd = state.memory().read_word(command_buffer + 28);
    const std::uint32_t rpc_number = state.memory().read_word(command_buffer + 32);
    const std::uint32_t recvbuf = state.memory().read_word(command_buffer + 40);
    const std::uint32_t recv_size = state.memory().read_word(command_buffer + 44);
    const std::uint32_t sd = state.memory().read_word(command_buffer + 52);
    const SifRpcServer* server = find_sif_server_by_handle(sd);
    const std::uint32_t sid = server == nullptr ? 0 : server->sid;
    const std::uint32_t send_size =
        state.memory().read_word(command_buffer + 36);
    const std::uint32_t server_buffer =
        server == nullptr ? 0 : server->buffer;
    if (strict_rpc_
        && classify_rpc_pair(sid, rpc_number) == RpcPairClass::Unknown) {
        // Strict RPC mode (slice 73, P07): the first unknown pair stops
        // with the full call context instead of answering the silent
        // empty result. The default path below is untouched, so the boot
        // keeps its current behavior unless --strict-rpc is set.
        std::uint32_t request_words[4] = {};
        if (server_buffer != 0
            && state.memory().contains(server_buffer, 16)) {
            for (std::uint32_t index = 0; index < 4; ++index) {
                request_words[index] =
                    state.memory().read_word(server_buffer + index * 4);
            }
        }
        char context[320];
        std::snprintf(context, sizeof context,
                      "Strict RPC stop: unknown pair sid 0x%08X fn 0x%08X "
                      "at pc 0x%08X thread %u (send %u recv %u recvbuf "
                      "0x%08X server-buf 0x%08X sd 0x%08X request %08X "
                      "%08X %08X %08X)",
                      sid, rpc_number, state.pc(), current_thread_id_,
                      send_size, recv_size, recvbuf, server_buffer, sd,
                      request_words[0], request_words[1], request_words[2],
                      request_words[3]);
        throw std::runtime_error(context);
    }
    if (sid == 0x50434456u && server != nullptr) {
        if (rpc_number == 2u) {
            // The volume registration: {descriptor block, checksum}.
            (void)answer_disc_volume(state, server->buffer);
        } else if (rpc_number == 3u) {
            // The transfer's byte count is not part of the reply the library
            // reads (its status word stays zero, which it takes as success).
            (void)answer_disc_read(state, server->buffer);
        }
    }
    std::uint8_t result[576] = {};
    std::uint32_t result_size = 0;
    if (sid == 0x80000006u && rpc_number == 0u && server != nullptr) {
        result_size = answer_file_open(state, server->buffer,
                                       state.memory().read_word(command_buffer + 36),
                                       result, sizeof result);
    } else if (sid == 0x50434456u && rpc_number == 4u && server != nullptr) {
        // The volume query: the registered volume's "volume space size".
        result_size = answer_disc_volume_size(result, sizeof result);
    } else if (sid == 0x53545250u && rpc_number == 3u && server != nullptr) {
        // The game's block cache read: the reply is the block's handle,
        // which the copy-out calls pass back.
        const std::uint32_t handle = answer_prts_read(state, server->buffer);
        result[0] = static_cast<std::uint8_t>(handle);
        result[1] = static_cast<std::uint8_t>(handle >> 8);
        result[2] = static_cast<std::uint8_t>(handle >> 16);
        result[3] = static_cast<std::uint8_t>(handle >> 24);
        result_size = 4;
    } else if (sid == 0x53545250u && (rpc_number == 4u || rpc_number == 7u)
               && server != nullptr) {
        // The copy-out: the cached block goes straight into the client's
        // buffer, so the reply carries no data.
        (void)answer_prts_copy(state, server->buffer);
        result_size = 0;
    } else {
        result_size = sif_rpc_result(state, sid, rpc_number, result,
                                     sizeof result);
    }
    // Telemetry only: the reply bytes above are untouched, so the default
    // behavior is identical with or without an observer.
    record_rpc_call(state, sid, rpc_number, send_size, recvbuf, recv_size,
                    server_buffer, result_size);
    if (recv_size > 0) {
        if (!state.memory().contains(recvbuf, recv_size)) {
            throw std::runtime_error(
                "An RPC call receive buffer is outside the mapped guest memory");
        }
        for (std::uint32_t offset = 0; offset < recv_size; ++offset) {
            const std::uint8_t byte = offset < result_size ? result[offset] : 0;
            state.memory().write_byte(recvbuf + offset, byte);
        }
    }
    // The SIFRPC end packet, exactly like a bind reply but with the call's
    // cid, plus the result bytes transferred above.
    state.memory().write_word(ee_command_buffer_ + 0, 64);
    state.memory().write_word(ee_command_buffer_ + 4, 0);
    state.memory().write_word(ee_command_buffer_ + 8, sif_command_cid_rpc_end);
    state.memory().write_word(ee_command_buffer_ + 12, 0);
    state.memory().write_word(ee_command_buffer_ + 16, rec_id);
    state.memory().write_word(ee_command_buffer_ + 20, pkt_addr);
    state.memory().write_word(ee_command_buffer_ + 24, rpc_id);
    state.memory().write_word(ee_command_buffer_ + 28, cd);
    state.memory().write_word(ee_command_buffer_ + 32, sif_command_cid_rpc_call);
    state.memory().write_word(ee_command_buffer_ + 36, server == nullptr ? 0 : sd);
    state.memory().write_word(ee_command_buffer_ + 40,
                              server == nullptr ? 0 : server->buffer);
    state.memory().write_word(ee_command_buffer_ + 44,
                              server == nullptr ? 0 : server->connection_buffer);
    for (std::uint32_t offset = 48; offset < 64; offset += 4) {
        state.memory().write_word(ee_command_buffer_ + offset, 0);
    }
    queue_dmac_completion(sif_channel_dmac);
}

void Kernel::answer_sif_reset(GuestState& state, std::uint32_t command_buffer,
                              std::uint32_t size) {
    // The reset command carries the image to boot (arg length at +16, mode
    // at +20, the string at +24); the game's own call names
    // "rom0:UDNL cdrom0:\IOPRP300.IMG;1". The model does not execute IOP
    // code: it records the path and completes the reboot by announcing the
    // fully booted flag set, which is the observable handshake the game's
    // `SifIopSync` poll waits for (SMFLAG & BOOTEND).
    sif_iop_image_.clear();
    if (size >= 24) {
        const std::uint32_t argument_length =
            state.memory().read_word(command_buffer + 16);
        for (std::uint32_t index = 0; index < argument_length; ++index) {
            if (24 + index >= size) {
                break;
            }
            const char character = static_cast<char>(
                state.memory().read_byte(command_buffer + 24 + index));
            if (character == '\0') {
                break;
            }
            sif_iop_image_.push_back(character);
        }
    }
    sif_reboot_pending_ = true;
}

std::uint32_t Kernel::sif_rpc_result(GuestState& state, std::uint32_t sid,
                                     std::uint32_t rpc_number,
                                     std::uint8_t* result,
                                     std::uint32_t capacity) {
    // The model IOP's function table. Each entry is shaped by the check in
    // the game that consumes it (the slice documents hold the trails):
    //  - the disc subsystem's status query (sid 0x80001300, RPC 0x80001363)
    //    answers a 144-byte payload whose first word is 0x310, the lowest
    //    value the game's `(word >> 4) == 0x31` check accepts (0x0058F840);
    //  - the fileio/CDVD version negotiation (sid 0x80000400, RPC 0xFE)
    //    answers 12 bytes with the minimum versions its checks accept, the
    //    second word 0x20A and the third 0x20E (0x0058D674 and 0x0058D694);
    //  - the version queries (RPC number 0xFF) answer the compatibility
    //    constant the game's own check expects, read from its data: the SIF
    //    manager (sid 0x80000001) reports the word at 0x0066829C plus the
    //    flag 2 its client checks, and the file server (sid 0x80000006)
    //    reports the four bytes at 0x0065829C ("3000"), which the file
    //    open's version check compares (0x005B6368).
    // Anything else answers an empty result.
    const auto put_word = [result](std::uint32_t offset, std::uint32_t value) {
        result[offset + 0] = static_cast<std::uint8_t>(value);
        result[offset + 1] = static_cast<std::uint8_t>(value >> 8);
        result[offset + 2] = static_cast<std::uint8_t>(value >> 16);
        result[offset + 3] = static_cast<std::uint8_t>(value >> 24);
    };
    const auto clear_result = [result, capacity]() {
        for (std::uint32_t offset = 0; offset < capacity; ++offset) {
            result[offset] = 0;
        }
    };
    if (sid == 0x80001300u && rpc_number == 0x80001363u && capacity >= 144) {
        // The real status structure comes from the game's IOP server, which
        // the model does not execute; the rest of the payload stays zero.
        clear_result();
        put_word(0, 0x310u);
        return 144;
    }
    if (sid == 0x80000400u && rpc_number == 0xFEu && capacity >= 12) {
        // Answering the minimums makes the game choose the oldest protocol
        // variant it supports; the real module versions come from the IOP
        // modules the model does not execute.
        clear_result();
        put_word(4, 0x20Au);
        put_word(8, 0x20Eu);
        return 12;
    }
    if (sid == 0x046D046Du && rpc_number == 12u && capacity >= 576) {
        // The liblgdev device sync (the call at 0x0056087C): the game's check
        // at 0x005608BC accepts the status word 0x010B2400 as completed and
        // any 0x010Bxxxx as a partial one; anything else hangs. The model
        // answers the completed status with the rest of the 576-byte reply
        // zero; the real structure comes from the game's IOP module, which
        // the model does not execute. The module identifies itself in its
        // banner string "liblgdev version 1.11.036" (live memory 0x006C8D40).
        clear_result();
        put_word(4, 0x010B2400u);
        return 576;
    }
    if (sid == 0x046D046Du && rpc_number == 4u && capacity >= 576) {
        // The device library's command exchange. The loading framework calls
        // it with command 0x10005 and requires the media signature
        // 0x046DC298 at the reply's word +0x5C (the comparison at 0x0055585C)
        // before it arms the read slots through RPC 5; with any other value
        // the slots stay disarmed and the framework only polls. The model
        // answers the signature; the rest of the 576-byte reply stays zero.
        clear_result();
        put_word(0x5C, 0x046DC298u);
        return 576;
    }
    if (rpc_number != 0xFFu) {
        return 0;
    }
    if (sid == 0x80000001u && capacity >= 8) {
        if (!state.memory().contains(0x0066829Cu, 4)) {
            throw std::runtime_error(
                "The SIF manager version constant is outside the mapped guest memory");
        }
        put_word(0, state.memory().read_word(0x0066829Cu));
        put_word(4, 2);
        return 8;
    }
    if (sid == 0x80000006u && capacity >= 4) {
        if (!state.memory().contains(0x0065829Cu, 4)) {
            throw std::runtime_error(
                "The file server version constant is outside the mapped guest memory");
        }
        put_word(0, state.memory().read_word(0x0065829Cu));
        return 4;
    }
    return 0;
}

ServiceOutcome Kernel::sif_set_reg(GuestState& state) {
    ensure_sif_ready(state);
    const std::uint32_t index = state.read_gpr32(4);
    const std::uint32_t value = state.read_gpr32(5);
    if ((index & 0x80000000u) != 0) {
        sif_software_registers_[index] = value;
        state.write_gpr64(2, value);  // the verified callers ignore the result
        return ServiceOutcome::Handled;
    }
    const std::uint32_t address = sif_register_index_address(index);
    if (address == 0) {
        write_error(state);
        return ServiceOutcome::Handled;
    }
    state.memory().write_word(address, value);
    state.write_gpr64(2, value);
    return ServiceOutcome::Handled;
}

ServiceOutcome Kernel::sif_get_reg(GuestState& state) {
    ensure_sif_ready(state);
    if (sif_reboot_pending_) {
        // The modeled IOP finished rebooting: announce the fully booted
        // state before answering the read the game's wait polls.
        sif_reboot_pending_ = false;
        state.memory().write_word(sif_register_index_address(4), sif_mesg_init);
    }
    const std::uint32_t index = state.read_gpr32(4);
    if ((index & 0x80000000u) != 0) {
        const auto found = sif_software_registers_.find(index);
        state.write_gpr64(2, found == sif_software_registers_.end() ? 0 : found->second);
        return ServiceOutcome::Handled;
    }
    const std::uint32_t address = sif_register_index_address(index);
    if (address == 0) {
        write_error(state);
        return ServiceOutcome::Handled;
    }
    state.write_gpr64(2, state.memory().read_word(address));
    return ServiceOutcome::Handled;
}

ServiceOutcome Kernel::sif_set_d_chain(GuestState& state) {
    ensure_sif_ready(state);
    // Enable the SIF0 channel (its CHCR lives at 0x1000C000): STR, TIE and
    // the chain mode, 0x184, exactly the value the public header documents.
    state.memory().write_word(sif_chcr_physical, 0x00000184u);
    state.write_gpr64(2, 0);
    return ServiceOutcome::Handled;
}

ServiceOutcome Kernel::sif_stop_dma(GuestState& state) {
    ensure_sif_ready(state);
    state.memory().write_word(sif_chcr_physical, 0);
    state.write_gpr64(2, 0);
    return ServiceOutcome::Handled;
}

ServiceOutcome Kernel::sif_dma_stat(GuestState& state) {
    // Every model transfer completes synchronously, so the status is always
    // "done": -1, the value the SDK's polling loops expect.
    state.write_gpr64(2, 0xFFFFFFFFu);
    return ServiceOutcome::Handled;
}

ServiceOutcome Kernel::sif_set_dma(GuestState& state) {
    ensure_sif_ready(state);
    const std::uint32_t descriptors = state.read_gpr32(4);
    const std::uint32_t count = state.read_gpr32(5);
    if (count == 0 || count > 16) {
        write_error(state);
        return ServiceOutcome::Handled;
    }
    const std::uint32_t command_buffer =
        state.memory().read_word(sif_register_index_address(2));
    for (std::uint32_t index = 0; index < count; ++index) {
        const std::uint32_t entry = descriptors + index * 16;  // SifDmaTransfer_t
        if (!state.memory().contains(entry, 16)) {
            write_error(state);
            return ServiceOutcome::Handled;
        }
        const std::uint32_t source = state.memory().read_word(entry);
        const std::uint32_t destination = state.memory().read_word(entry + 4);
        const std::uint32_t size = state.memory().read_word(entry + 8);
        if (size == 0) {
            continue;
        }
        copy_guest_bytes(state, source, destination, size);
        if (destination == command_buffer) {
            run_iop_stub(state, command_buffer, size);
        }
    }
    state.write_gpr64(2, next_dma_id_++);
    return ServiceOutcome::Handled;
}

} // namespace gt4recomp::ee



