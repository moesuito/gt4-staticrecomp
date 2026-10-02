// Unit tests for the EE kernel model: the thread and semaphore tables and
// the deterministic cooperative scheduler of decision 0005, with no game
// data. The tests drive the services directly, exactly as the syscall
// handlers would, and check the register contexts across switches.
#include "gt4recomp/ee_kernel.hpp"

#include <cstdint>
#include <iostream>
#include <utility>

using namespace gt4recomp::ee;

namespace {

constexpr std::uint32_t window_base = 0x00000000;
constexpr std::size_t window_size = 0x00200000;

constexpr std::uint32_t root_stack = 0x00100800;
constexpr std::uint32_t root_stack_size = 0x800;
constexpr std::uint32_t thread_stack = 0x00100900;
constexpr std::uint32_t thread_stack_size = 0x100;
constexpr std::uint32_t thread_function = 0x00100400;
constexpr std::uint32_t thread_gp = 0x00100500;
constexpr std::uint32_t thread_struct = 0x00100100;
constexpr std::uint32_t sema_struct = 0x00100140;
constexpr std::uint32_t status_struct = 0x00100180;

GuestState make_state() {
    GuestMemory memory(window_base, window_size);
    return GuestState(std::move(memory));
}

void write_thread_struct(GuestState& state, std::uint32_t priority) {
    state.memory().write_word(thread_struct + 0x00, 0);
    state.memory().write_word(thread_struct + 0x04, thread_function);
    state.memory().write_word(thread_struct + 0x08, thread_stack);
    state.memory().write_word(thread_struct + 0x0C, thread_stack_size);
    state.memory().write_word(thread_struct + 0x10, thread_gp);
    state.memory().write_word(thread_struct + 0x14, priority);
    state.memory().write_word(thread_struct + 0x18, 0);
    state.memory().write_word(thread_struct + 0x1C, 0);
    state.memory().write_word(thread_struct + 0x20, 0);
}

void write_sema_struct(GuestState& state, std::uint32_t max_count,
                       std::uint32_t init_count) {
    state.memory().write_word(sema_struct + 0x00, 0);
    state.memory().write_word(sema_struct + 0x04, max_count);
    state.memory().write_word(sema_struct + 0x08, init_count);
    state.memory().write_word(sema_struct + 0x0C, 0);
    state.memory().write_word(sema_struct + 0x10, 0);
    state.memory().write_word(sema_struct + 0x14, 0);
}

// Registers the current execution as the root thread (id 1).
bool setup_root(Kernel& kernel, GuestState& state) {
    state.write_gpr32(5, root_stack);
    state.write_gpr32(6, root_stack_size);
    return kernel.setup_thread(state) == ServiceOutcome::Handled
        && kernel.current_thread_id() == 1;
}

// Creates and starts a second thread (id 2) through the services.
bool start_second_thread(Kernel& kernel, GuestState& state, std::uint32_t priority,
                         std::uint32_t argument) {
    write_thread_struct(state, priority);
    state.write_gpr32(4, thread_struct);
    if (kernel.create_thread(state) != ServiceOutcome::Handled
        || state.read_gpr32(2) != 2) {
        return false;
    }
    state.write_gpr32(4, 2);
    state.write_gpr32(5, argument);
    return kernel.start_thread(state) != ServiceOutcome::Unhandled;
}

} // namespace

int main() {
    int failures = 0;
    const auto check = [&](bool passed, const char* label) {
        if (!passed) { std::cerr << label << '\n'; ++failures; }
    };

    // SetupThread registers the root, returns the region top and starts it
    // running.
    {
        Kernel kernel;
        GuestState state = make_state();
        check(setup_root(kernel, state), "SetupThread created the root thread");
        check(state.read_gpr32(2) == 0x00101000u, "SetupThread returned the stack top");
        check(kernel.threads().size() == 1
                  && kernel.threads()[0].status == ThreadRun
                  && kernel.threads()[0].initial_priority == 0,
              "the root is the single running thread at priority 0");
        check(kernel.setup_thread(state) == ServiceOutcome::Handled
                  && state.read_gpr32(2) == 0xFFFFFFFFu,
              "a second SetupThread is rejected");
    }

    // CreateSema, PollSema, SignalSema, ReferSemaStatus and DeleteSema
    // follow the public ee_sema_t contract.
    {
        Kernel kernel;
        GuestState state = make_state();
        write_sema_struct(state, 2, 1);
        state.write_gpr32(4, sema_struct);
        check(kernel.create_sema(state) == ServiceOutcome::Handled
                  && state.read_gpr32(2) == 1,
              "CreateSema returns the first id");
        check(kernel.semaphores().size() == 1
                  && kernel.semaphores()[0].count == 1
                  && kernel.semaphores()[0].max_count == 2,
              "the semaphore starts at its initial count");
        check(state.memory().read_word(sema_struct + 0x00) == 1
                  && state.memory().read_word(sema_struct + 0x0C) == 0,
              "the kernel mirrors count and wait_threads into the structure");

        state.write_gpr32(4, 1);
        check(kernel.poll_sema(state) == ServiceOutcome::Handled
                  && state.read_gpr32(2) == 0,
              "PollSema takes the available count");
        check(kernel.poll_sema(state) == ServiceOutcome::Handled
                  && state.read_gpr32(2) == 0xFFFFFFFFu,
              "PollSema fails when the count is zero");

        check(kernel.signal_sema(state) == ServiceOutcome::Handled
                  && kernel.semaphores()[0].count == 1,
              "SignalSema increments the count");
        kernel.signal_sema(state);
        kernel.signal_sema(state);
        check(kernel.semaphores()[0].count == 2,
              "SignalSema stops at max_count");

        state.write_gpr32(4, 1);
        state.write_gpr32(5, sema_struct);
        check(kernel.refer_sema_status(state) == ServiceOutcome::Handled
                  && state.memory().read_word(sema_struct + 0x00) == 2
                  && state.memory().read_word(sema_struct + 0x04) == 2
                  && state.memory().read_word(sema_struct + 0x08) == 1,
              "ReferSemaStatus reports count, max and init");
        check(kernel.delete_sema(state) == ServiceOutcome::Handled
                  && state.read_gpr32(2) == 0
                  && kernel.semaphores().empty(),
              "DeleteSema removes an idle semaphore");
        check(kernel.delete_sema(state) == ServiceOutcome::Handled
                  && state.read_gpr32(2) == 0xFFFFFFFFu,
              "DeleteSema fails for a missing id");
    }

    // A started thread has its entry, argument, gp and stack; lowering the
    // root's priority hands the CPU to it, saving the root at pc + 4.
    {
        Kernel kernel;
        GuestState state = make_state();
        check(setup_root(kernel, state), "root ready");
        check(start_second_thread(kernel, state, 0, 0xCAFEu),
              "second thread created and started");
        check(kernel.current_thread_id() == 1
                  && kernel.threads()[1].status == ThreadReady,
              "an equal-priority ready thread does not preempt the root");

        state.set_pc(0x00100020);
        state.write_gpr32(4, 1);
        state.write_gpr32(5, 1);
        check(kernel.change_thread_priority(state) == ServiceOutcome::Switched,
              "lowering the root's priority switches to the ready thread");
        check(kernel.current_thread_id() == 2, "the new thread runs");
        check(state.pc() == thread_function, "the new thread starts at its entry");
        check(state.read_gpr32(4) == 0xCAFEu, "the argument arrives in a0");
        check(state.read_gpr32(28) == thread_gp, "the thread gp is set");
        check(state.read_gpr32(29) == thread_stack + thread_stack_size,
              "the thread sp is its region top");
        check(kernel.threads()[0].status == ThreadReady
                  && kernel.threads()[0].context.pc == 0x00100024u
                  && kernel.threads()[0].context.gpr[2] == 0,
              "the root is saved after its syscall with v0 = 0");
    }

    // WaitSema blocks the root, SignalSema releases it, and SleepThread
    // hands the CPU back to the released thread.
    {
        Kernel kernel;
        GuestState state = make_state();
        check(setup_root(kernel, state), "root ready");
        check(start_second_thread(kernel, state, 0, 0), "second thread ready");
        write_sema_struct(state, 1, 0);
        state.write_gpr32(4, sema_struct);
        kernel.create_sema(state);  // id 1, count 0

        state.set_pc(0x00100040);
        state.write_gpr32(4, 1);
        check(kernel.wait_sema(state) == ServiceOutcome::Switched,
              "Waiting on an empty semaphore blocks");
        check(kernel.current_thread_id() == 2, "the other thread runs");
        check(kernel.semaphores()[0].wait_threads == 1,
              "the semaphore counts its waiter");
        check(kernel.threads()[0].status == ThreadWait
                  && kernel.threads()[0].wait_type == ThreadWaitSema
                  && kernel.threads()[0].context.pc == 0x00100044u,
              "the blocked root resumes after its syscall");

        state.set_pc(0x00100050);
        state.write_gpr32(4, 1);
        check(kernel.signal_sema(state) == ServiceOutcome::Handled,
              "signaling from an equal-priority thread does not preempt");
        check(kernel.semaphores()[0].wait_threads == 0
                  && kernel.threads()[0].status == ThreadReady,
              "the waiter is ready and the semaphore handed over");

        check(kernel.sleep_thread(state) == ServiceOutcome::Switched,
              "SleepThread hands the CPU to the ready thread");
        check(kernel.current_thread_id() == 1 && state.pc() == 0x00100044u
                  && state.read_gpr32(2) == 0,
              "the released WaitSema resumes with v0 = 0");
    }

    // ReferThreadStatus reports the public status layout; SuspendThread and
    // ResumeThread gate readiness without losing the context.
    {
        Kernel kernel;
        GuestState state = make_state();
        check(setup_root(kernel, state), "root ready");
        check(start_second_thread(kernel, state, 3, 0x55u), "second thread ready");

        state.write_gpr32(4, 2);
        state.write_gpr32(5, status_struct);
        check(kernel.refer_thread_status(state) == ServiceOutcome::Handled
                  && state.memory().read_word(status_struct + 0x00) == ThreadReady
                  && state.memory().read_word(status_struct + 0x04) == thread_function
                  && state.memory().read_word(status_struct + 0x08) == thread_stack
                  && state.memory().read_word(status_struct + 0x0C) == thread_stack_size
                  && state.memory().read_word(status_struct + 0x10) == thread_gp
                  && state.memory().read_word(status_struct + 0x14) == 3
                  && state.memory().read_word(status_struct + 0x18) == 3,
              "ReferThreadStatus reports the thread's fields");

        state.write_gpr32(4, 2);
        check(kernel.suspend_thread(state) == ServiceOutcome::Handled
                  && kernel.threads()[1].status == (ThreadReady | ThreadSuspend),
              "SuspendThread marks a ready thread");
        check(kernel.resume_thread(state) == ServiceOutcome::Handled
                  && kernel.threads()[1].status == ThreadReady,
              "ResumeThread clears the suspend bit");
    }

    // SetSyscall records the patch, mirrors it into the synthetic table the
    // SDK searches, and dispatches the patched number to guest code.
    {
        Kernel kernel;
        GuestState state = make_state();
        ServiceTable services;
        kernel.register_services(services);

        state.write_gpr32(4, 0x83);
        state.write_gpr32(5, 0x5B73C8);
        check(kernel.set_syscall(state) == ServiceOutcome::Handled
                  && state.read_gpr32(2) == 0,
              "SetSyscall accepts a patch");
        check(kernel.patched_handler(0x83) == 0x5B73C8u,
              "the patch is recorded");
        check(state.memory().read_word(Kernel::syscall_table_physical + 0x83 * 4)
                  == 0x5B73C8u,
              "the guest-visible table holds the patched handler");
        check(state.memory().read_word(Kernel::syscall_table_physical + 0x5A * 4)
                  != 0,
              "the synthetic table has an entry for every number");

        const ServiceHandler* handler = services.find(0x83u);
        check(handler != nullptr, "the patched number gains a dispatch handler");
        state.set_pc(0x00100060);
        state.write_gpr64(31, 0x00100040u);
        const ServiceOutcome outcome = (*handler)(state);
        check(outcome == ServiceOutcome::Jumped && state.pc() == 0x5B73C8u
                  && state.read_gpr32(31) == Kernel::patch_return_stub_physical,
              "a patched syscall jumps to the guest handler through the stub");
        const ServiceHandler* patch_return =
            services.find(Kernel::patch_return_service);
        check(patch_return != nullptr, "the private return service is registered");
        const ServiceOutcome returned = (*patch_return)(state);
        check(returned == ServiceOutcome::Jumped && state.pc() == 0x00100064u
                  && state.read_gpr32(31) == 0x00100040u,
              "the stub return restores the caller's ra and resume address");

        state.write_gpr32(4, 0x5A);
        state.write_gpr32(5, 0x5B7390);
        kernel.set_syscall(state);
        check(kernel.patched_handler(0x5Au) == 0x5B7390u
                  && state.memory().read_word(Kernel::syscall_table_physical + 0x5A * 4)
                      == 0x5B7390u,
              "a second patch lands in the table");
    }

    // A run with no runnable thread is the NoRunnableThread outcome.
    {
        Kernel kernel;
        GuestState state = make_state();
        check(setup_root(kernel, state), "root ready");
        check(kernel.exit_thread(state) == ServiceOutcome::NoRunnableThread,
              "exiting the last thread leaves nothing to run");
        check(kernel.threads()[0].status == ThreadDormant,
              "the exited thread is dormant");
    }

    if (failures != 0) {
        return 1;
    }
    std::cout << "kernel threads, semaphores and the cooperative scheduler behave as specified\n";
    return 0;
}
