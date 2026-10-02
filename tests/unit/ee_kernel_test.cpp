// Unit tests for the EE kernel model: the thread and semaphore tables and
// the deterministic cooperative scheduler of decision 0005, with no game
// data. The tests drive the services directly, exactly as the syscall
// handlers would, and check the register contexts across switches.
#include "gt4recomp/ee_device.hpp"
#include "gt4recomp/ee_kernel.hpp"
#include "gt4recomp/ee_timer.hpp"

#include <cstdint>
#include <iostream>
#include <utility>

using namespace gt4recomp::ee;

namespace {

constexpr std::uint32_t window_base = 0x00000000;
// Large enough for the whole low RAM the kernel model reaches (the game's
// compatibility constants at 0x0065829C and 0x0066829C live above 6 MiB).
constexpr std::size_t window_size = 0x00700000;

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
        check((state.read_cp0(12) & 1u) != 0,
              "the root thread runs with interrupts enabled");
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
                  && state.read_gpr32(2) == 3,
              "CreateSema returns the first id (bits 0 and 1 set)");
        check(kernel.semaphores().size() == 1
                  && kernel.semaphores()[0].count == 1
                  && kernel.semaphores()[0].max_count == 2,
              "the semaphore starts at its initial count");
        check(state.memory().read_word(sema_struct + 0x00) == 1
                  && state.memory().read_word(sema_struct + 0x0C) == 0,
              "the kernel mirrors count and wait_threads into the structure");

        state.write_gpr32(4, 3);
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

        state.write_gpr32(4, 3);
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
        kernel.create_sema(state);  // id 3, count 0

        state.set_pc(0x00100040);
        state.write_gpr32(4, 3);
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
        state.write_gpr32(4, 3);
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

        // Re-installing the number's own token is how the SDK restores the
        // kernel's handler after reading it back: the patch is dropped.
        state.write_gpr32(4, 0x5A);
        state.write_gpr32(5, Kernel::syscall_token_base + 0x5A * 4);
        kernel.set_syscall(state);
        check(kernel.patched_handler(0x5Au) == 0
                  && services.find(0x5Au) == nullptr,
              "re-installing a model token removes the patch");
    }

    // GetOsdConfigParam/SetOsdConfigParam move the ConfigParam word and
    // retain every field, including the version bits the SDK probes.
    {
        Kernel kernel;
        GuestState state = make_state();
        ServiceTable services;
        kernel.register_services(services);
        constexpr std::uint32_t word_address = 0x00100200;
        state.write_gpr32(4, word_address);
        check(kernel.get_osd_config(state) == ServiceOutcome::Handled
                  && state.read_gpr32(2) == 0
                  && state.memory().read_word(word_address) == kernel.osd_config(),
              "GetOsdConfigParam writes the ConfigParam word");
        const std::uint32_t probe = (kernel.osd_config() & 0x1fffu) | 0x2000u;
        state.memory().write_word(word_address, probe);
        check(kernel.set_osd_config(state) == ServiceOutcome::Handled
                  && kernel.osd_config() == probe,
              "SetOsdConfigParam retains every field");
        check(kernel.get_osd_config(state) == ServiceOutcome::Handled
                  && state.memory().read_word(word_address) == probe
                  && ((state.memory().read_word(word_address) >> 13) & 7u) == 1u,
              "the version field is retained for the SDK probe");
        state.write_gpr32(4, 0xfffffff0u);
        check(kernel.set_osd_config(state) == ServiceOutcome::Handled
                  && state.read_gpr32(2) == 0xFFFFFFFFu,
              "an unmapped OSD address is rejected");
    }

    // GetOsdConfigParam2/SetOsdConfigParam2 move the four-byte extended
    // block with the caller's size and offset; reads past it are zeros.
    {
        Kernel kernel;
        GuestState state = make_state();
        ServiceTable services;
        kernel.register_services(services);
        constexpr std::uint32_t buffer = 0x00100200;
        state.write_gpr32(4, buffer);
        state.write_gpr32(5, 1);   // size
        state.write_gpr32(6, 1);   // offset: the clock/date flags byte
        check(kernel.get_osd_config2(state) == ServiceOutcome::Handled
                  && state.memory().read_byte(buffer) == 0x00,
              "the extended OSD block reports no daylight savings");
        state.memory().write_byte(buffer + 0x10, 0x10);  // daylight savings
        state.write_gpr32(4, buffer + 0x10);
        state.write_gpr32(5, 1);
        state.write_gpr32(6, 1);
        check(kernel.set_osd_config2(state) == ServiceOutcome::Handled,
              "SetOsdConfigParam2 stores the caller's byte");
        state.write_gpr32(4, buffer + 0x20);
        state.write_gpr32(5, 2);
        state.write_gpr32(6, 0);
        check(kernel.get_osd_config2(state) == ServiceOutcome::Handled
                  && state.memory().read_byte(buffer + 0x20) == 0x00
                  && state.memory().read_byte(buffer + 0x21) == 0x10,
              "the stored flags byte survives a full-block read");
        state.write_gpr32(4, buffer + 0x30);
        state.write_gpr32(5, 2);
        state.write_gpr32(6, 3);
        check(kernel.get_osd_config2(state) == ServiceOutcome::Handled
                  && state.memory().read_byte(buffer + 0x30) == 0x01
                  && state.memory().read_byte(buffer + 0x31) == 0x00,
              "reads past the four-byte block are zeros");
    }

    // The GS interrupt mask round-trips; GsPutIMR returns the previous value.
    {
        Kernel kernel;
        GuestState state = make_state();
        ServiceTable services;
        kernel.register_services(services);
        state.write_gpr64(4, 0xff00ull);
        check(kernel.gs_put_imr(state) == ServiceOutcome::Handled
                  && state.read_gpr64(2) == 0,
              "GsPutIMR returns the previous mask");
        check(kernel.gs_get_imr(state) == ServiceOutcome::Handled
                  && state.read_gpr64(2) == 0xff00ull,
              "GsGetIMR returns the stored mask");
        state.write_gpr64(4, 0x1122334455667788ull);
        check(kernel.gs_put_imr(state) == ServiceOutcome::Handled
                  && state.read_gpr64(2) == 0xff00ull,
              "GsPutIMR returns the replaced value");
    }

    // SetGsCrt is accepted; the model has no display.
    {
        Kernel kernel;
        GuestState state = make_state();
        check(kernel.set_gs_crt(state) == ServiceOutcome::Handled
                  && state.read_gpr32(2) == 0,
              "SetGsCrt is accepted");
    }

    // Deci2Call is accepted: the defined calls answer 1, beyond 0x10 -1.
    {
        Kernel kernel;
        GuestState state = make_state();
        state.write_gpr32(4, 3);
        check(kernel.deci2_call(state) == ServiceOutcome::Handled
                  && state.read_gpr32(2) == 1,
              "Deci2Call accepts a defined call");
        state.write_gpr32(4, 0x20);
        check(kernel.deci2_call(state) == ServiceOutcome::Handled
                  && state.read_gpr32(2) == 0xFFFFFFFFu,
              "Deci2Call rejects a call beyond 0x10");
    }

    // The idle source advances an enabled timer by one frame of its clock
    // and raises its compare interrupt.
    {
        Kernel kernel;
        GuestState state = make_state();
        ServiceTable services;
        kernel.register_services(services);
        RegisterBank timers(0x10000000u, 0x2000u);
        timers.map_into(state.memory());
        state.memory().write_word(0x10001010u, 0x00000182u);  // CLKS=2, CUE, CMPE
        state.write_gpr32(4, 11);  // INTC_TIM2
        state.write_gpr32(5, 0x00100400u);
        state.write_gpr32(6, 0xFFFFFFFFu);
        state.write_gpr32(7, 0);
        kernel.add_intc_handler(state);
        const std::uint32_t interrupted_pc = 0x00100020u;
        state.set_pc(interrupted_pc);
        check(kernel.deliver_idle_interrupt(state),
              "the timer interrupt starts");
        check(state.pc() == 0x00100400u && state.read_gpr32(4) == 11
                  && state.memory().read_word(0x10001000u) == 9600u
                  && (state.memory().read_word(0x10001010u) & 0x400u) != 0,
              "the timer advanced one frame and set the compare flag");
    }

    // The idle interrupt source: when no thread can run, the model raises a
    // VBlank, records it in INTC_STAT, runs its handler chain and restores
    // the interrupted context; the budget bounds deliveries that change
    // nothing.
    {
        Kernel kernel;
        GuestState state = make_state();
        ServiceTable services;
        kernel.register_services(services);
        RegisterBank intc(0x1000F000u, 0x100u);
        intc.map_into(state.memory());
        state.write_gpr32(4, 2);  // the VBlank cause
        state.write_gpr32(5, 0x00100400u);
        state.write_gpr32(6, 0xFFFFFFFFu);
        state.write_gpr32(7, 0);
        kernel.add_intc_handler(state);
        const std::uint32_t interrupted_pc = 0x00100020u;
        state.set_pc(interrupted_pc);
        check(kernel.deliver_idle_interrupt(state),
              "the idle source starts the VBlank handler");
        check(state.pc() == 0x00100400u && state.read_gpr32(4) == 2
                  && (state.memory().read_word(0x1000F000u) & 4u) != 0,
              "the VBlank frame carries the cause and the status bit");
        const ServiceHandler* return_handler =
            services.find(Kernel::patch_return_service);
        check(return_handler != nullptr
                  && (*return_handler)(state) == ServiceOutcome::Jumped
                  && state.pc() == interrupted_pc,
              "the VBlank return restores the context without a thread");
        bool delivered = true;
        std::uint32_t deliveries = 1;  // the one delivered above
        while (deliveries <= Kernel::idle_interrupt_budget) {
            if (!kernel.deliver_idle_interrupt(state)) {
                delivered = false;
                break;
            }
            ++deliveries;
            // Let the handler return so the next delivery is allowed: the
            // model does not nest handler injections.
            (*return_handler)(state);
        }
        check(!delivered && deliveries == Kernel::idle_interrupt_budget,
              "the idle budget bounds deliveries that change nothing");
    }

    // Interrupt and DMA handler registrations are stored and removable;
    // enabling and disabling are accepted without delivering anything.
    {
        Kernel kernel;
        GuestState state = make_state();
        ServiceTable services;
        kernel.register_services(services);
        state.write_gpr32(4, 11);  // cause: INTC_TIM2
        state.write_gpr32(5, 0x005B1234);
        check(kernel.add_intc_handler(state) == ServiceOutcome::Handled
                  && state.read_gpr32(2) == 1
                  && kernel.interrupt_handlers().size() == 1
                  && kernel.interrupt_handlers()[0].cause == 11
                  && kernel.interrupt_handlers()[0].handler == 0x005B1234u,
              "AddIntcHandler stores the registration");
        check(kernel.enable_intc(state) == ServiceOutcome::Handled
                  && state.read_gpr32(2) == 0,
              "EnableIntc is accepted");
        state.write_gpr32(5, 1);
        check(kernel.remove_intc_handler(state) == ServiceOutcome::Handled
                  && state.read_gpr32(2) == 0
                  && kernel.interrupt_handlers().empty(),
              "RemoveIntcHandler removes the registration");
        check(kernel.remove_intc_handler(state) == ServiceOutcome::Handled
                  && state.read_gpr32(2) == 0xFFFFFFFFu,
              "removing an unknown handler fails");
        // The DMAC registrations live in their own list, keyed by channel.
        state.write_gpr32(4, 5);  // channel: SIF0
        state.write_gpr32(5, 0x005B5678);
        check(kernel.add_dmac_handler(state) == ServiceOutcome::Handled
                  && state.read_gpr32(2) == 2
                  && kernel.dmac_handlers().size() == 1
                  && kernel.dmac_handlers()[0].cause == 5
                  && kernel.interrupt_handlers().empty(),
              "AddDmacHandler stores the registration separately");
        state.write_gpr32(5, 2);
        check(kernel.remove_dmac_handler(state) == ServiceOutcome::Handled
                  && kernel.dmac_handlers().empty(),
              "RemoveDmacHandler removes the registration");
    }

    // The SIF layer: the register round trip, the model IOP's SIFCMD init
    // handshake and the injected DMA interrupt that delivers its reply.
    {
        Kernel kernel;
        GuestState state = make_state();
        ServiceTable services;
        kernel.register_services(services);
        RegisterBank sif_registers(0x1000F200u, 0x100u);
        sif_registers.map_into(state.memory());
        RegisterBank sif0(0x1000C000u, 0x100u);
        sif0.map_into(state.memory());
        RegisterBank dmac(0x1000E000u, 0x100u);
        dmac.map_into(state.memory());

        check(kernel.sif_set_d_chain(state) == ServiceOutcome::Handled
                  && sif0.register_value(0x1000C000u) == 0x184u,
              "SifSetDChain enables the SIF0 channel");
        state.write_gpr32(4, 0x80000000u);
        state.write_gpr32(5, 0x1234u);
        kernel.sif_set_reg(state);
        state.write_gpr32(4, 0x80000000u);
        kernel.sif_get_reg(state);
        check(state.read_gpr32(2) == 0x1234u,
              "software SIF registers round-trip");
        state.write_gpr32(4, 4);  // SMFLG
        kernel.sif_get_reg(state);
        check((state.read_gpr32(2) & 0x20000u) != 0,
              "the model IOP reports CMDINIT");
        check(kernel.sif_dma_stat(state) == ServiceOutcome::Handled
                  && state.read_gpr32(2) == 0xFFFFFFFFu,
              "SifDmaStat reports a completed transfer");

        // An INIT_CMD packet sent to the IOP command buffer.
        constexpr std::uint32_t packet = 0x00100200;
        constexpr std::uint32_t descriptors = 0x00100300;
        constexpr std::uint32_t iop_buffer = 0x00080000;   // the model SMCOM
        constexpr std::uint32_t ee_buffer = 0x00180000;    // the reply buffer
        state.memory().write_word(packet + 0, 20);         // psize
        state.memory().write_word(packet + 4, 0);          // dest
        state.memory().write_word(packet + 8, 0x80000002u);  // INIT_CMD
        state.memory().write_word(packet + 12, 0);         // opt
        state.memory().write_word(packet + 16, ee_buffer); // EE reply buffer
        state.memory().write_word(descriptors + 0, packet);
        state.memory().write_word(descriptors + 4, iop_buffer);
        state.memory().write_word(descriptors + 8, 20);
        state.memory().write_word(descriptors + 12, 0);
        state.write_gpr32(4, descriptors);
        state.write_gpr32(5, 1);
        check(kernel.sif_set_dma(state) == ServiceOutcome::Handled
                  && state.read_gpr32(2) == 1,
              "SifSetDma returns a transfer id");
        check(state.memory().read_word(iop_buffer + 8) == 0x80000002u,
              "the command packet reached the IOP buffer");
        check(state.memory().read_word(ee_buffer + 8) == 0x80000001u
                  && state.memory().read_word(ee_buffer + 16) == 0
                  && state.memory().read_word(ee_buffer + 20) == 1,
              "the IOP stub replied SET_SREG(RPCINIT)");
        check(kernel.pending_interrupts() == 1,
              "the reply queued the SIF0 interrupt");

        // The handlers are injected with their frames; each return runs the
        // next registered handler for the channel, and the last one restores
        // the interrupted context. The SIF0 reply is a DMAC channel 5
        // completion, so its handlers come from the DMAC list.
        state.write_gpr32(4, 5);  // DMAC channel 5
        state.write_gpr32(5, 0x00100400u);
        state.write_gpr32(6, 0xFFFFFFFFu);
        state.write_gpr32(7, 0);
        kernel.add_dmac_handler(state);
        state.write_gpr32(4, 5);
        state.write_gpr32(5, 0x00100480u);
        state.write_gpr32(6, 0xFFFFFFFFu);
        state.write_gpr32(7, 0);
        kernel.add_dmac_handler(state);
        const std::uint32_t interrupted_pc = 0x00100020u;
        state.set_pc(interrupted_pc);
        check(kernel.start_interrupt(state), "the pending interrupt starts");
        check(state.pc() == 0x00100400u && state.read_gpr32(4) == 5
                  && state.read_gpr32(31) == Kernel::patch_return_stub_physical
                  && (state.read_cp0(12) & 0x10000u) == 0
                  && (dmac.register_value(0x1000E010u) & (1u << 5)) != 0,
              "the first handler frame has the channel, the stub, EIE clear "
              "and the DMAC status bit");
        const ServiceHandler* return_handler =
            services.find(Kernel::patch_return_service);
        check(return_handler != nullptr
                  && (*return_handler)(state) == ServiceOutcome::Jumped
                  && state.pc() == 0x00100480u,
              "the first handler return chains to the second handler");
        check((*return_handler)(state) == ServiceOutcome::Jumped
                  && state.pc() == interrupted_pc,
              "the last handler return restores the interrupted context");

        // An RPC bind gets the SIFRPC end reply that unblocks the client.
        state.memory().write_word(packet + 0, 64);           // psize
        state.memory().write_word(packet + 8, 0x80000009u);  // RPC_BIND
        state.memory().write_word(packet + 16, 5);           // rec_id
        state.memory().write_word(packet + 20, 0x00100500u); // pkt_addr
        state.memory().write_word(packet + 24, 2);           // rpc_id
        state.memory().write_word(packet + 28, 0x00100600u); // cd
        state.memory().write_word(packet + 32, 0x80000001u); // sid
        state.memory().write_word(descriptors + 8, 64);      // size
        state.write_gpr32(4, descriptors);
        state.write_gpr32(5, 1);
        check(kernel.sif_set_dma(state) == ServiceOutcome::Handled
                  && state.read_gpr32(2) == 2,
              "SifSetDma returns the next transfer id");
        check(state.memory().read_word(ee_buffer + 8) == 0x80000008u
                  && state.memory().read_word(ee_buffer + 16) == 5
                  && state.memory().read_word(ee_buffer + 20) == 0x00100500u
                  && state.memory().read_word(ee_buffer + 28) == 0x00100600u
                  && state.memory().read_word(ee_buffer + 32) == 0x80000009u
                  && state.memory().read_word(ee_buffer + 36) != 0
                  && state.memory().read_word(ee_buffer + 40) != 0
                  && state.memory().read_word(ee_buffer + 44) != 0,
              "the model IOP answered the RPC bind");
        check(kernel.pending_interrupts() == 1,
              "the bind reply queued its SIF0 interrupt");

        // A reset command records the image and completes the modeled
        // reboot by announcing the fully booted flag set.
        const std::string image = "rom0:UDNL cdrom0:\\IOPRP300.IMG;1";
        state.memory().write_word(0x1000F230u,
                                  0x00020000u);  // as the game's reset leaves it
        state.memory().write_word(packet + 0, 104);          // psize
        state.memory().write_word(packet + 8, 0x80000003u);  // RESET_CMD
        state.memory().write_word(packet + 16, image.size());
        state.memory().write_word(packet + 20, 0);           // mode
        for (std::uint32_t index = 0; index < image.size(); ++index) {
            state.memory().write_byte(packet + 24 + index,
                                      static_cast<std::uint8_t>(image[index]));
        }
        state.memory().write_word(descriptors + 8, 104);
        state.write_gpr32(4, descriptors);
        state.write_gpr32(5, 1);
        check(kernel.sif_set_dma(state) == ServiceOutcome::Handled,
              "a reset command transfers");
        check(kernel.sif_iop_image() == image,
              "the model IOP records the requested image");
        state.write_gpr32(4, 4);  // SMFLAG, the register the game polls
        check(kernel.sif_get_reg(state) == ServiceOutcome::Handled
                  && (state.read_gpr32(2) & 0x70000u) == 0x70000u,
              "the reboot completes before the first register read after it");
        check(kernel.sif_get_reg(state) == ServiceOutcome::Handled
                  && (state.read_gpr32(2) & 0x70000u) == 0x70000u,
              "the announced boot state persists");

        // The version queries answer with the game's own compatibility
        // constants: the SIF manager's version word plus the flag 2, and the
        // file server's four-byte version.
        state.memory().write_word(0x0066829Cu, 0x00275520u);
        state.memory().write_word(0x0065829Cu, 0x30303033u);  // "3000"
        std::uint8_t answer[64] = {};
        check(kernel.sif_rpc_result(state, 0x80000001u, 0xFFu, answer, sizeof answer)
                      == 8
                  && answer[0] == 0x20 && answer[1] == 0x55 && answer[2] == 0x27
                  && answer[3] == 0x00 && answer[4] == 2 && answer[5] == 0,
              "the SIF manager version query answers its constant and flag");
        std::uint8_t file_answer[64] = {};
        check(kernel.sif_rpc_result(state, 0x80000006u, 0xFFu, file_answer,
                                    sizeof file_answer) == 4
                  && file_answer[0] == 0x33 && file_answer[1] == 0x30
                  && file_answer[2] == 0x30 && file_answer[3] == 0x30,
              "the file server version query answers its constant");
        check(kernel.sif_rpc_result(state, 0x80000006u, 1u, file_answer,
                                    sizeof file_answer) == 0,
              "an unknown RPC function answers an empty result");

        // A SET_SREG command is mirrored back: the acknowledgement the game's
        // command-layer init spins on (register 1 in the array at 0x008869C0
        // is only written by the incoming command's handler).
        const std::uint32_t pending_before = kernel.pending_interrupts();
        state.memory().write_word(packet + 0, 24);           // psize
        state.memory().write_word(packet + 8, 0x80000001u);  // SET_SREG
        state.memory().write_word(packet + 16, 1);           // sreg
        state.memory().write_word(packet + 20, 1);           // value
        state.memory().write_word(descriptors + 8, 24);
        state.write_gpr32(4, descriptors);
        state.write_gpr32(5, 1);
        check(kernel.sif_set_dma(state) == ServiceOutcome::Handled,
              "a SET_SREG command transfers");
        check(state.memory().read_word(ee_buffer + 0) == 24
                  && state.memory().read_word(ee_buffer + 8) == 0x80000001u
                  && state.memory().read_word(ee_buffer + 16) == 1
                  && state.memory().read_word(ee_buffer + 20) == 1,
              "the model IOP mirrored SET_SREG(1, 1) back");
        check(kernel.pending_interrupts() == pending_before + 1,
              "the SET_SREG reply queued its SIF0 interrupt");

        // The liblgdev device sync answers the completed status word
        // 0x010B2400, the value the game's check at 0x005608BC accepts.
        std::uint8_t lgdev[576] = {};
        check(kernel.sif_rpc_result(state, 0x046D046Du, 12u, lgdev, sizeof lgdev)
                      == 576
                  && lgdev[4] == 0x00 && lgdev[5] == 0x24 && lgdev[6] == 0x0B
                  && lgdev[7] == 0x01,
              "the liblgdev device sync answers the completed status");
    }

    // Handled services advance the model's clock (decision 0016): a counting
    // timer moves by its clock's share of one millisecond, its compare fires
    // when the counter crosses COMP, and a frame of slices raises VBlank.
    {
        Kernel kernel;
        GuestState state = make_state();
        TimerUnit timer;
        timer.map_into(state.memory());
        constexpr std::uint32_t timer2 =
            TimerUnit::window_base + 2 * TimerUnit::timer_stride;
        state.memory().write_word(timer2 + TimerUnit::count_offset, 0);
        state.memory().write_word(timer2 + TimerUnit::compare_offset, 576);  // one millisecond
        state.memory().write_word(timer2 + TimerUnit::mode_offset,
                                  0x00000180u | 2u);  // CUE | CMPE, CLKS = BUSCLK/256
        kernel.advance_service_time(state);
        check(state.memory().read_word(timer2 + TimerUnit::count_offset) == 576
                  && kernel.pending_interrupts() == 1,
              "one handled service advances a millisecond and fires the compare");
        kernel.advance_service_time(state);
        check(kernel.pending_interrupts() == 1,
              "a compare already behind the counter does not fire again");
        // A frame takes 2,457,600 / 147,456 = 16.67 slices: the 17th reaches
        // it, and only then does the registered VBlank handler join the queue.
        state.write_gpr32(4, 2);  // the VBlank cause
        state.write_gpr32(5, 0x005B1234);
        check(kernel.add_intc_handler(state) == ServiceOutcome::Handled,
              "the VBlank handler registers");
        for (int index = 0; index < 14; ++index) {
            kernel.advance_service_time(state);
        }
        check(kernel.pending_interrupts() == 1,
              "VBlank waits for a full frame of slices");
        kernel.advance_service_time(state);
        check(kernel.pending_interrupts() == 2,
              "the frame's VBlank joins the queue");
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

