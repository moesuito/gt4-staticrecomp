#include "gt4recomp/ee_kernel.hpp"

#include <stdexcept>

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
    state.restore_registers(next->context);
    return true;
}

bool Kernel::preempt_if_outranked(GuestState& state) {
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
    add(0x6Bu, &Kernel::sif_stop_dma);
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

const std::vector<KernelInterruptHandler>& Kernel::interrupt_handlers() const noexcept {
    return interrupt_handlers_;
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
    state.write_gpr64(2, current_thread_id_);
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
    semaphore.id = next_semaphore_id_++;
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
    // injected interrupt handler gets the whole interrupted context back.
    if (deferred_calls_.empty()) {
        throw std::logic_error("The patch return stub fired with no call in flight");
    }
    const DeferredCall call = deferred_calls_.back();
    deferred_calls_.pop_back();
    if (call.kind == DeferredCall::Kind::Interrupt) {
        state.restore_registers(call.context);
        return ServiceOutcome::Jumped;
    }
    state.write_gpr64(31, call.caller_ra);
    state.set_pc(call.resume_pc);
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

ServiceOutcome Kernel::add_intc_handler(GuestState& state) {
    const std::uint32_t cause = state.read_gpr32(4);
    const std::uint32_t handler = state.read_gpr32(5);
    // AddIntcHandler2 passes a fourth argument; the three-argument form
    // leaves a3 as the caller had it, which the registration copies anyway
    // because nothing consults it without an interrupt.
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
    // AddDmacHandler has the same shape as AddIntcHandler; the model keeps
    // both registrations in one list because neither can fire.
    return add_intc_handler(state);
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
    return remove_intc_handler(state);
}

ServiceOutcome Kernel::enable_intc(GuestState& state) {
    // The registration is the observable part; no interrupt is delivered,
    // so enabling and disabling are accepted with no effect.
    state.write_gpr64(2, 0);
    return ServiceOutcome::Handled;
}

ServiceOutcome Kernel::disable_intc(GuestState& state) {
    state.write_gpr64(2, 0);
    return ServiceOutcome::Handled;
}

ServiceOutcome Kernel::enable_dmac(GuestState& state) {
    state.write_gpr64(2, 0);
    return ServiceOutcome::Handled;
}

ServiceOutcome Kernel::disable_dmac(GuestState& state) {
    state.write_gpr64(2, 0);
    return ServiceOutcome::Handled;
}

std::uint32_t Kernel::pending_interrupts() const noexcept {
    return static_cast<std::uint32_t>(interrupt_queue_.size());
}

std::uint32_t Kernel::sif_register_index_address(std::uint32_t index) const noexcept {
    // The public indices 1-4 name MSCOM, SMCOM, MSFLG and SMFLG.
    if (index >= 1 && index <= 4) {
        return sif_register_physical + (index - 1) * 0x10;
    }
    return 0;
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
    interrupt_queue_.push_back(cause);
}

bool Kernel::start_interrupt(GuestState& state) {
    if (interrupt_queue_.empty()) {
        return false;
    }
    const std::uint32_t cause = interrupt_queue_.front();
    interrupt_queue_.erase(interrupt_queue_.begin());
    std::uint32_t handler = 0;
    for (const KernelInterruptHandler& registration : interrupt_handlers_) {
        if (registration.cause == cause) {
            handler = registration.handler;
            break;
        }
    }
    if (handler == 0) {
        // No handler registered for the cause: like the hardware, nothing
        // happens; the interrupt is dropped and the model records nothing.
        return false;
    }
    // The handler returns through the model's stub, like a patched syscall.
    state.memory().write_word(patch_return_stub_physical, 0x24030100u);
    state.memory().write_word(patch_return_stub_physical + 4, 0x0000000Cu);
    const RegisterContext interrupted = state.save_registers();
    DeferredCall call;
    call.kind = DeferredCall::Kind::Interrupt;
    call.context = interrupted;
    deferred_calls_.push_back(call);

    RegisterContext frame;
    frame.pc = handler;
    frame.gpr[4] = cause;                          // a0 = channel
    frame.gpr[28] = interrupted.gpr[28];           // gp as interrupted
    frame.gpr[29] = interrupted.gpr[29];           // sp as interrupted
    frame.gpr[31] = patch_return_stub_physical;
    frame.cp0 = interrupted.cp0;
    frame.cp0[12] &= ~0x00010000u;                 // EIE clear while handling
    state.restore_registers(frame);
    return true;
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
        return;
    }
    const std::uint32_t reply_buffer = state.memory().read_word(command_buffer + 16);
    if (!state.memory().contains(reply_buffer, 24)) {
        throw std::runtime_error(
            "The SIFCMD init reply buffer is outside the mapped guest memory");
    }
    state.memory().write_word(reply_buffer + 0, 24);  // psize (dsize remains 0)
    state.memory().write_word(reply_buffer + 4, 0);   // dest
    state.memory().write_word(reply_buffer + 8, sif_command_cid_set_sreg);
    state.memory().write_word(reply_buffer + 12, 0);  // opt
    state.memory().write_word(reply_buffer + 16, sif_sreg_rpcinit);
    state.memory().write_word(reply_buffer + 20, 1);
    queue_interrupt(sif_channel_dmac);
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
