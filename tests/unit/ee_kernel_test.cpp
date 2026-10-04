// Unit tests for the EE kernel model: the thread and semaphore tables and
// the deterministic cooperative scheduler of decision 0005, with no game
// data. The tests drive the services directly, exactly as the syscall
// handlers would, and check the register contexts across switches.
#include "gt4recomp/disc_image.hpp"
#include "gt4recomp/ee_device.hpp"
#include "gt4recomp/ee_kernel.hpp"
#include "gt4recomp/ee_timer.hpp"

#include <cstdint>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string_view>
#include <utility>

using namespace gt4recomp::ee;

namespace {

constexpr std::uint32_t window_base = 0x00000000;
// Large enough for the whole low RAM the kernel model reaches (the game's
// compatibility constants at 0x0065829C and 0x0066829C live above 6 MiB;
// the SIF pump queue and its pointer near 0x00886818 need almost 9 MiB).
constexpr std::size_t window_size = 0x00900000;

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
    try {

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
    // and raises its compare interrupt through the INTC contract: the
    // occurrence sets INTC_STAT even before dispatch, and the mask (set by
    // EnableIntc, the privileged path to the same bit the guest toggles)
    // allows the delivery.
    {
        Kernel kernel;
        GuestState state = make_state();
        ServiceTable services;
        kernel.register_services(services);
        TimerUnit timer;
        timer.map_into(state.memory());
        kernel.set_timer_unit(&timer);
        IntcUnit intc;
        intc.map_into(state.memory());
        kernel.set_intc_unit(&intc);
        state.memory().write_word(0x10001000u, 0);       // COUNT
        state.memory().write_word(0x10001020u, 0x2000u);  // COMP inside one frame
        state.memory().write_word(0x10001010u, 0x00000182u);  // CLKS=2, CUE, CMPE
        state.write_gpr32(4, 11);  // INTC_TIM2
        state.write_gpr32(5, 0x00100400u);
        state.write_gpr32(6, 0xFFFFFFFFu);
        state.write_gpr32(7, 0);
        kernel.add_intc_handler(state);
        state.write_gpr32(4, 11);
        check(kernel.enable_intc(state) == ServiceOutcome::Handled
                  && intc.mask_allows(11),
              "EnableIntc sets the cause's mask bit");
        const std::uint32_t interrupted_pc = 0x00100020u;
        state.set_pc(interrupted_pc);
        check(kernel.deliver_idle_interrupt(state),
              "the timer interrupt starts");
        check(state.pc() == 0x00100400u && state.read_gpr32(4) == 11
                  && state.read_gpr32(5) == 0
                  && state.read_gpr32(6) == interrupted_pc
                  && state.memory().read_word(0x10001000u) == 0x2580u
                  && (state.memory().read_word(0x10001010u) & 0x400u) != 0
                  && (intc.register_value(0x1000F000u) & (1u << 11)) != 0,
              "the timer advanced one frame with EQUF and INTC_STAT set");
    }

    // A masked cause stays pending without dispatch: enabling later
    // delivers what is still pending.
    {
        Kernel kernel;
        GuestState state = make_state();
        ServiceTable services;
        kernel.register_services(services);
        IntcUnit intc;
        intc.map_into(state.memory());
        kernel.set_intc_unit(&intc);
        kernel.raise_interrupt(7);
        check(kernel.pending_interrupts() == 1 && intc.is_pending(7),
              "an occurrence without a handler stays in the status");
        check(!kernel.start_interrupt(state)
                  && kernel.pending_interrupts() == 1 && intc.is_pending(7),
              "no handler means no dispatch, and the pending survives");
        state.write_gpr32(4, 7);
        state.write_gpr32(5, 0x00100400u);
        state.write_gpr32(7, 0);
        kernel.add_intc_handler(state);
        check(!kernel.start_interrupt(state)
                  && kernel.pending_interrupts() == 1,
              "a registered handler behind a closed mask still waits");
        state.write_gpr32(4, 7);
        kernel.enable_intc(state);
        const std::uint32_t interrupted_pc = 0x00100020u;
        state.set_pc(interrupted_pc);
        check(kernel.start_interrupt(state) && state.pc() == 0x00100400u
                  && state.read_gpr32(4) == 7,
              "enabling later delivers the still-pending occurrence");
    }

    // The CP0 gate: with EIE cleared the CPU cannot take the interrupt, so
    // the eligible occurrence waits; reopening the gate delivers it.
    {
        Kernel kernel;
        GuestState state = make_state();
        ServiceTable services;
        kernel.register_services(services);
        IntcUnit intc;
        intc.map_into(state.memory());
        kernel.set_intc_unit(&intc);
        state.write_gpr32(4, 7);
        state.write_gpr32(5, 0x00100400u);
        state.write_gpr32(7, 0);
        kernel.add_intc_handler(state);
        state.write_gpr32(4, 7);
        kernel.enable_intc(state);
        kernel.raise_interrupt(7);
        state.set_pc(0x00100020u);
        state.write_cp0(12, state.read_cp0(12) & ~Kernel::cp0_status_eie);
        check(!kernel.start_interrupt(state)
                  && kernel.pending_interrupts() == 1,
              "a closed CP0 gate holds the pending occurrence");
        state.write_cp0(12, state.read_cp0(12) | Kernel::cp0_status_eie);
        check(kernel.start_interrupt(state) && state.pc() == 0x00100400u,
              "reopening the gate delivers the held occurrence");
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
        IntcUnit intc;
        intc.map_into(state.memory());
        kernel.set_intc_unit(&intc);
        state.write_gpr32(4, 2);  // the VBlank cause
        state.write_gpr32(5, 0x00100400u);
        state.write_gpr32(6, 0xFFFFFFFFu);
        state.write_gpr32(7, 0);
        kernel.add_intc_handler(state);
        state.write_gpr32(4, 2);
        check(kernel.enable_intc(state) == ServiceOutcome::Handled,
              "the VBlank mask opens through EnableIntc");
        const std::uint32_t interrupted_pc = 0x00100020u;
        state.set_pc(interrupted_pc);
        check(kernel.deliver_idle_interrupt(state),
              "the idle source starts the VBlank handler");
        check(state.pc() == 0x00100400u && state.read_gpr32(4) == 2
                  && state.read_gpr32(5) == 0
                  && state.read_gpr32(6) == interrupted_pc
                  && (intc.register_value(0x1000F000u) & 4u) != 0,
              "the VBlank frame carries the cause and the status bit");
        const ServiceHandler* return_handler =
            services.find(Kernel::patch_return_service);
        // Interrupted-idle (decision 0031): with no thread underneath,
        // the return stays idle instead of jumping back into an idle pc
        // the driver would re-execute; the context is still restored.
        check(return_handler != nullptr
                  && (*return_handler)(state) == ServiceOutcome::NoRunnableThread
                  && state.pc() == interrupted_pc
                  && kernel.current_thread_id() == 0,
              "the VBlank return stays idle with the context restored");
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

    // The first originating event (decision 0026): the first idle tick
    // with the pump handler registered and an empty queue writes one
    // SET_SREG packet and queues its DMAC completion, once per boot.
    {
        Kernel kernel;
        GuestState state = make_state();
        check(setup_root(kernel, state), "originating root ready");
        state.write_gpr32(4, 5);  // channel: SIF0
        state.write_gpr32(5, 0x005B0E30);
        check(kernel.add_dmac_handler(state) == ServiceOutcome::Handled,
              "originating pump registered");
        // The queue pointer through the mirror alias, proving the
        // physical resolution the pump relies on, plus a populated
        // dispatch entry so the packet has a consumer.
        state.memory().write_word(0x00886818u, 0x20886740u);
        state.memory().write_word(0x00886824u, 0x00886000u);
        state.memory().write_word(0x0088600Cu, 0x005B0850u);
        check(kernel.pending_interrupts() == 0, "originating queue starts empty");
        check(kernel.deliver_idle_interrupt(state),
              "the idle tick delivers the packet completion");
        check(state.memory().read_byte(0x00886740u) == 0x18
                  && state.memory().read_word(0x00886744u) == 0
                  && state.memory().read_word(0x00886748u) == 1
                  && state.memory().read_word(0x0088674Cu) == 0
                  && state.memory().read_word(0x00886750u) == 1
                  && state.memory().read_word(0x00886754u) == 1,
              "the packet bytes match the decision: count plus {0,1,0,reg,value}");
        check(kernel.pending_interrupts() == 0,
              "the completion dispatched immediately to the pump");
        check(!kernel.deliver_idle_interrupt(state)
                  && state.memory().read_byte(0x00886740u) == 0x18
                  && kernel.pending_interrupts() == 0,
              "the packet is one-shot: no replay, no duplicate");
    }

    // The trigger's guards: no pump registered, or live traffic in the
    // queue, means nothing is written and nothing is queued.
    {
        Kernel kernel;
        GuestState state = make_state();
        check(setup_root(kernel, state), "guarded root ready");
        state.memory().write_word(0x00886818u, 0x00886740u);
        check(!kernel.deliver_idle_interrupt(state)
                  && kernel.pending_interrupts() == 0
                  && state.memory().read_byte(0x00886740u) == 0,
              "without a registered pump nothing fires");
        state.write_gpr32(4, 5);
        state.write_gpr32(5, 0x005B0E30);
        check(kernel.add_dmac_handler(state) == ServiceOutcome::Handled,
              "guarded pump registered");
        state.memory().write_byte(0x00886740u, 0x07);
        check(!kernel.deliver_idle_interrupt(state)
                  && kernel.pending_interrupts() == 0
                  && state.memory().read_byte(0x00886740u) == 0x07,
              "a non-empty queue is never overwritten");
        state.memory().write_word(0x00886818u, 0x00886740u);
        state.memory().write_byte(0x00886740u, 0);
        check(!kernel.deliver_idle_interrupt(state)
                  && kernel.pending_interrupts() == 0
                  && state.memory().read_byte(0x00886740u) == 0,
              "an unpopulated dispatch table holds the packet");
    }

    // The one-shot flag rides the snapshot: a restored kernel never
    // replays, and a pre-decision blob (without the trailing word)
    // loads with the packet unsent.
    {
        Kernel kernel;
        GuestState state = make_state();
        check(setup_root(kernel, state), "snapshot root ready");
        state.write_gpr32(4, 5);
        state.write_gpr32(5, 0x005B0E30);
        check(kernel.add_dmac_handler(state) == ServiceOutcome::Handled,
              "snapshot pump registered");
        state.memory().write_word(0x00886818u, 0x00886740u);
        state.memory().write_word(0x00886824u, 0x00886000u);
        state.memory().write_word(0x0088600Cu, 0x005B0850u);
        check(kernel.deliver_idle_interrupt(state), "snapshot packet sent");
        const std::vector<std::uint8_t> blob = kernel.save_kernel_state();
        Kernel restored;
        restored.load_kernel_state(blob);
        check(!restored.deliver_idle_interrupt(state)
                  && state.memory().read_byte(0x00886740u) == 0x18
                  && restored.save_kernel_state() == blob,
              "the restored kernel keeps the one-shot and re-saves identically");
        std::vector<std::uint8_t> legacy(blob.begin(), blob.end() - 4);
        Kernel legacy_kernel;
        legacy_kernel.load_kernel_state(legacy);
        GuestState legacy_state = make_state();
        check(setup_root(legacy_kernel, legacy_state), "legacy root ready");
        legacy_state.write_gpr32(4, 5);
        legacy_state.write_gpr32(5, 0x005B0E30);
        check(legacy_kernel.add_dmac_handler(legacy_state)
                  == ServiceOutcome::Handled,
              "legacy pump registered");
        legacy_state.memory().write_word(0x00886818u, 0x00886740u);
        legacy_state.memory().write_word(0x00886824u, 0x00886000u);
        legacy_state.memory().write_word(0x0088600Cu, 0x005B0850u);
        // The blob carries the first delivery's live handler frame, exactly
        // as a snapshot taken mid-dispatch would: return it first, the way
        // the driver does after running the handler, so the next delivery
        // is allowed.
        ServiceTable legacy_services;
        legacy_kernel.register_services(legacy_services);
        const ServiceHandler* legacy_return =
            legacy_services.find(Kernel::patch_return_service);
        check(legacy_return != nullptr
                  && (*legacy_return)(legacy_state) != ServiceOutcome::Unhandled
                  && legacy_kernel.deferred_call_count() == 0,
              "legacy handler frame returned");
        check(legacy_kernel.deliver_idle_interrupt(legacy_state)
                  && legacy_state.memory().read_byte(0x00886740u) == 0x18,
              "a pre-decision snapshot loads and still sends once");
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

    // Enable/disable are the privileged setters of the same mask bits the
    // guest toggles by writing INTC_MASK/D_STAT: enabling sets, disabling
    // clears, and out-of-range causes fail instead of touching state.
    {
        Kernel kernel;
        GuestState state = make_state();
        IntcUnit intc;
        intc.map_into(state.memory());
        kernel.set_intc_unit(&intc);
        DmacStatusUnit dmac;
        dmac.map_into(state.memory());
        kernel.set_dmac_unit(&dmac);
        state.write_gpr32(4, 11);
        check(kernel.enable_intc(state) == ServiceOutcome::Handled
                  && intc.mask_allows(11),
              "EnableIntc sets the cause's mask bit");
        // The guest toggle contract is separate: toggling the enabled bit
        // clears it again without involving the syscall.
        state.memory().write_word(0x1000F010u, 1u << 11);
        check(!intc.mask_allows(11),
              "a guest mask write toggles the enabled bit off");
        state.write_gpr32(4, 11);
        check(kernel.disable_intc(state) == ServiceOutcome::Handled
                  && !intc.mask_allows(11),
              "DisableIntc clears the cause's mask bit");
        state.write_gpr32(4, 40);
        check(kernel.enable_intc(state) == ServiceOutcome::Handled
                  && state.read_gpr32(2) == 0xFFFFFFFFu,
              "enabling an out-of-range cause fails");
        state.write_gpr32(4, 5);
        check(kernel.enable_dmac(state) == ServiceOutcome::Handled
                  && dmac.mask_allows(5),
              "EnableDmac sets the channel's mask bit");
        state.memory().write_word(0x1000E010u, 0x20u << 16);
        check(!dmac.mask_allows(5),
              "a guest D_STAT write toggles the channel mask off");
        state.write_gpr32(4, 5);
        check(kernel.disable_dmac(state) == ServiceOutcome::Handled
                  && !dmac.mask_allows(5),
              "DisableDmac clears the channel's mask bit");
        state.write_gpr32(4, 40);
        check(kernel.enable_dmac(state) == ServiceOutcome::Handled
                  && state.read_gpr32(2) == 0xFFFFFFFFu,
              "enabling an out-of-range channel fails");
    }

    // A DMAC completion lives in its own domain: channel 2 sets CIS bit 2
    // and dispatches the channel's handlers, while INTC cause 9 (Timer0)
    // stays untouched.
    {
        Kernel kernel;
        GuestState state = make_state();
        ServiceTable services;
        kernel.register_services(services);
        IntcUnit intc;
        intc.map_into(state.memory());
        kernel.set_intc_unit(&intc);
        DmacStatusUnit dmac;
        dmac.map_into(state.memory());
        kernel.set_dmac_unit(&dmac);
        state.write_gpr32(4, 2);  // DMAC channel 2 (GIF)
        state.write_gpr32(5, 0x00100400u);
        state.write_gpr32(7, 0);
        kernel.add_dmac_handler(state);
        state.write_gpr32(4, 2);
        kernel.enable_dmac(state);
        kernel.raise_dmac_completion(2);
        check(kernel.pending_interrupts() == 1
                  && dmac.completion_pending(2)
                  && !intc.is_pending(9),
              "a GIF completion pends CIS 2 without touching Timer0");
        kernel.raise_dmac_completion(2);
        check(kernel.pending_interrupts() == 1,
              "a repeated completion coalesces onto the pending one");
        const std::uint32_t interrupted_pc = 0x00100020u;
        state.set_pc(interrupted_pc);
        check(kernel.start_interrupt(state) && state.pc() == 0x00100400u
                  && state.read_gpr32(4) == 2
                  && dmac.completion_pending(2),
              "the channel completion dispatches its handlers");
    }

    // Dispatch order: repeats coalesce in place, so distinct causes keep
    // their relative order instead of the repeat moving to the back.
    {
        Kernel kernel;
        GuestState state = make_state();
        ServiceTable services;
        kernel.register_services(services);
        IntcUnit intc;
        intc.map_into(state.memory());
        kernel.set_intc_unit(&intc);
        for (const std::uint32_t cause : {2u, 4u}) {
            state.write_gpr32(4, cause);
            state.write_gpr32(5, 0x00100400u + cause * 0x10u);
            state.write_gpr32(7, 0);
            kernel.add_intc_handler(state);
            state.write_gpr32(4, cause);
            kernel.enable_intc(state);
        }
        kernel.raise_interrupt(2);
        kernel.raise_interrupt(4);
        kernel.raise_interrupt(2);  // repeat: must not move behind 4
        state.set_pc(0x00100020u);
        check(kernel.start_interrupt(state) && state.pc() == 0x00100420u,
              "a repeated cause keeps its queue position");
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
        DmacStatusUnit dmac;
        dmac.map_into(state.memory());
        kernel.set_dmac_unit(&dmac);

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
        // The SIF0 completion needs its mask open before it can dispatch.
        state.write_gpr32(4, 5);
        check(kernel.enable_dmac(state) == ServiceOutcome::Handled
                  && dmac.mask_allows(5),
              "the SIF0 mask opens through EnableDmac");
        const std::uint32_t interrupted_pc = 0x00100020u;
        state.set_pc(interrupted_pc);
        check(kernel.start_interrupt(state), "the pending interrupt starts");
        check(state.pc() == 0x00100400u && state.read_gpr32(4) == 5
                  && state.read_gpr32(5) == 0
                  && state.read_gpr32(6) == interrupted_pc
                  && state.read_gpr32(31) == Kernel::patch_return_stub_physical
                  && (state.read_cp0(12) & 0x10000u) == 0
                  && dmac.completion_pending(5),
              "the first handler frame has the channel, the stub, EIE clear "
              "and the DMAC status bit");
        const ServiceHandler* return_handler =
            services.find(Kernel::patch_return_service);
        check(return_handler != nullptr
                  && (*return_handler)(state) == ServiceOutcome::Jumped
                  && state.pc() == 0x00100480u,
              "the first handler return chains to the second handler");
        check((*return_handler)(state) == ServiceOutcome::NoRunnableThread
                  && state.pc() == interrupted_pc,
              "the last handler return stays idle with the context restored");

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
        check(kernel.sif_server_sids().size() == 1
                  && kernel.sif_server_sids()[0] == 0x80000001u,
              "the bound server is listed in the server inventory");

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
        check(kernel.pending_interrupts() == pending_before,
              "a repeat SIF0 completion coalesces onto the pending one");

        // The liblgdev device sync answers the completed status word
        // 0x010B2400, the value the game's check at 0x005608BC accepts.
        std::uint8_t lgdev[576] = {};
        check(kernel.sif_rpc_result(state, 0x046D046Du, 12u, lgdev, sizeof lgdev)
                      == 576
                  && lgdev[4] == 0x00 && lgdev[5] == 0x24 && lgdev[6] == 0x0B
                  && lgdev[7] == 0x01,
              "the liblgdev device sync answers the completed status");
    }

    // Slice-73 RPC telemetry (P07): per-pair classification, call and
    // bind recording from real packets, and the strict stop. Recording
    // is observation only: no reply byte changes on either path.
    {
        check(Kernel::classify_rpc_pair(0x50434456u, 3u)
                      == RpcPairClass::ImplementedVerified
                  && Kernel::classify_rpc_pair(0x53545250u, 7u)
                         == RpcPairClass::ImplementedVerified
                  && Kernel::classify_rpc_pair(0x80000006u, 0u)
                         == RpcPairClass::ImplementedVerified,
              "disc, cache and file-open pairs are implemented-verified");
        check(Kernel::classify_rpc_pair(0x80000001u, 0xFFu)
                      == RpcPairClass::CompatConstant
                  && Kernel::classify_rpc_pair(0x80000006u, 0xFFu)
                         == RpcPairClass::CompatConstant
                  && Kernel::classify_rpc_pair(0x80000400u, 0xFEu)
                         == RpcPairClass::CompatConstant
                  && Kernel::classify_rpc_pair(0x80001300u, 0x80001363u)
                         == RpcPairClass::CompatConstant,
              "version and status answers are compat constants");
        check(Kernel::classify_rpc_pair(0x046D046Du, 12u)
                      == RpcPairClass::ProvisionalExplicit
                  && Kernel::classify_rpc_pair(0x50434456u, 1u)
                         == RpcPairClass::ProvisionalExplicit,
              "partial replies are provisional-explicit");
        check(Kernel::classify_rpc_pair(0x80000400u, 1u)
                      == RpcPairClass::Unknown
                  && Kernel::classify_rpc_pair(0x5042474Du, 8u)
                         == RpcPairClass::Unknown
                  && Kernel::classify_rpc_pair(0u, 1u)
                         == RpcPairClass::Unknown,
              "unlisted pairs, even on named SIDs, are unknown");
        check(Kernel::rpc_pair_key(0x80000001u, 0xFFu)
                      > Kernel::rpc_pair_key(0x80000001u, 0u)
                  && Kernel::rpc_pair_class_name(RpcPairClass::Unknown) != nullptr
                  && Kernel::rpc_pair_candidate_name(0x80000400u) != nullptr
                  && Kernel::rpc_pair_note(0x50434456u, 3u) != nullptr,
              "pair keys order and every name/note is present");
    }

    // Calls and binds are recorded with caller, sizes and buffers; the
    // strict stop names the first unknown pair with its full context.
    {
        Kernel kernel;
        GuestState state = make_state();
        ServiceTable services;
        kernel.register_services(services);
        check(setup_root(kernel, state), "telemetry root ready");
        check(!kernel.strict_rpc(), "strict RPC mode is off by default");
        RegisterBank sif_registers(0x1000F200u, 0x100u);
        sif_registers.map_into(state.memory());
        RegisterBank sif0(0x1000C000u, 0x100u);
        sif0.map_into(state.memory());
        DmacStatusUnit dmac;
        dmac.map_into(state.memory());
        kernel.set_dmac_unit(&dmac);
        constexpr std::uint32_t packet = 0x00100200;
        constexpr std::uint32_t descriptors = 0x00100300;
        constexpr std::uint32_t iop_buffer = 0x00080000;
        constexpr std::uint32_t ee_buffer = 0x00180000;
        constexpr std::uint32_t recv_buffer = 0x00181000;
        state.memory().write_word(packet + 0, 20);
        state.memory().write_word(packet + 8, 0x80000002u);  // INIT_CMD
        state.memory().write_word(packet + 16, ee_buffer);
        state.memory().write_word(descriptors + 0, packet);
        state.memory().write_word(descriptors + 4, iop_buffer);
        state.memory().write_word(descriptors + 8, 20);
        state.memory().write_word(descriptors + 12, 0);
        state.write_gpr32(4, descriptors);
        state.write_gpr32(5, 1);
        check(kernel.sif_set_dma(state) == ServiceOutcome::Handled,
              "telemetry handshake transfers");
        state.set_pc(0x00100020u);
        state.memory().write_word(packet + 0, 64);
        state.memory().write_word(packet + 8, 0x80000009u);  // RPC_BIND
        state.memory().write_word(packet + 16, 5);
        state.memory().write_word(packet + 20, 0x00100500u);
        state.memory().write_word(packet + 24, 2);
        state.memory().write_word(packet + 28, 0x00100600u);
        state.memory().write_word(packet + 32, 0x80000001u);
        state.memory().write_word(descriptors + 8, 64);
        state.write_gpr32(4, descriptors);
        state.write_gpr32(5, 1);
        check(kernel.sif_set_dma(state) == ServiceOutcome::Handled,
              "telemetry bind transfers");
        const std::uint32_t server_handle =
            state.memory().read_word(ee_buffer + 36);
        const std::uint32_t server_buffer =
            state.memory().read_word(ee_buffer + 40);
        check(server_handle != 0 && server_buffer != 0,
              "the bind hands out a server");
        check(kernel.rpc_bind_stats().count(0x80000001u) == 1
                  && kernel.rpc_bind_stats().at(0x80000001u).count == 1
                  && kernel.rpc_bind_stats().at(0x80000001u).first_pc
                         == 0x00100020u
                  && kernel.rpc_bind_stats().at(0x80000001u).first_thread == 1,
              "the bind is recorded with caller pc and thread");
        state.memory().write_word(0x0066829Cu, 0x00275520u);
        state.memory().write_word(server_buffer + 0, 0xAAAAAAAAu);
        state.memory().write_word(server_buffer + 4, 0xBBBBBBBBu);
        const auto send_call = [&](std::uint32_t function,
                                   std::uint32_t recv_size, std::uint32_t pc,
                                   std::uint32_t handle) {
            state.set_pc(pc);
            state.memory().write_word(packet + 0, 64);
            state.memory().write_word(packet + 8, 0x8000000Au);  // RPC_CALL
            state.memory().write_word(packet + 16, 7);
            state.memory().write_word(packet + 20, 0x00100500u);
            state.memory().write_word(packet + 24, 3);
            state.memory().write_word(packet + 28, 0x00100600u);
            state.memory().write_word(packet + 32, function);
            state.memory().write_word(packet + 36, 8);
            state.memory().write_word(packet + 40, recv_buffer);
            state.memory().write_word(packet + 44, recv_size);
            state.memory().write_word(packet + 52, handle);
            state.write_gpr32(4, descriptors);
            state.write_gpr32(5, 1);
            return kernel.sif_set_dma(state);
        };
        check(send_call(0xFFu, 8u, 0x00100040u, server_handle)
                      == ServiceOutcome::Handled
                  && state.memory().read_word(recv_buffer) == 0x00275520u,
              "a known version call answers its constant");
        const std::uint64_t version_key =
            Kernel::rpc_pair_key(0x80000001u, 0xFFu);
        check(kernel.rpc_pair_stats().count(version_key) == 1,
              "the known call is recorded");
        const RpcPairStats& version_stats =
            kernel.rpc_pair_stats().at(version_key);
        check(version_stats.calls == 1
                  && version_stats.calls_by_thread.count(1) == 1
                  && version_stats.first_pc == 0x00100040u
                  && version_stats.last_pc == 0x00100040u
                  && version_stats.first_send_size == 8
                  && version_stats.first_recv_size == 8
                  && version_stats.first_result_size == 8
                  && version_stats.first_recv_buffer == recv_buffer
                  && version_stats.first_request_words[0] == 0xAAAAAAAAu
                  && version_stats.first_request_words[1] == 0xBBBBBBBBu,
              "the known call carries caller, sizes, buffer and request");
        check(send_call(0xFFu, 8u, 0x00100060u, server_handle)
                      == ServiceOutcome::Handled
                  && kernel.rpc_pair_stats().at(version_key).calls == 2
                  && kernel.rpc_pair_stats().at(version_key).last_pc
                         == 0x00100060u,
              "a repeat call counts and moves the last pc");
        check(send_call(0x1234u, 0u, 0x00100080u, server_handle)
                      == ServiceOutcome::Handled,
              "an unknown call answers empty by default");
        const std::uint64_t unknown_key =
            Kernel::rpc_pair_key(0x80000001u, 0x1234u);
        check(kernel.rpc_pair_stats().count(unknown_key) == 1
                  && kernel.rpc_pair_stats().at(unknown_key).calls == 1
                  && kernel.rpc_pair_stats().at(unknown_key).first_result_size
                         == 0,
              "the unknown call is recorded as an empty result");
        kernel.set_strict_rpc(true);
        check(kernel.strict_rpc(), "strict RPC mode turns on");
        check(send_call(0xFFu, 8u, 0x001000A0u, server_handle)
                      == ServiceOutcome::Handled,
              "a known call still answers under strict mode");
        bool strict_fired = false;
        try {
            send_call(0x1234u, 0u, 0x001000C0u, server_handle);
        } catch (const std::runtime_error& error) {
            const std::string_view message(error.what());
            strict_fired = message.find("Strict RPC stop") != std::string_view::npos
                && message.find("0x80000001") != std::string_view::npos
                && message.find("0x001000C0") != std::string_view::npos;
        }
        check(strict_fired,
              "strict mode stops the unknown pair with sid and pc");
        kernel.set_strict_rpc(false);
        check(send_call(0x1234u, 0u, 0x001000E0u, server_handle)
                      == ServiceOutcome::Handled,
              "default mode answers empty again after strict turns off");
    }

    // Handled services advance the model's clock (decision 0016): a counting
    // timer moves by its clock's share of one millisecond, its compare fires
    // when the counter crosses COMP, and a frame of slices raises VBlank.
    {
        Kernel kernel;
        GuestState state = make_state();
        TimerUnit timer;
        timer.map_into(state.memory());
        kernel.set_timer_unit(&timer);
        IntcUnit intc;
        intc.map_into(state.memory());
        kernel.set_intc_unit(&intc);
        constexpr std::uint32_t timer2 =
            TimerUnit::window_base + 2 * TimerUnit::timer_stride;
        state.memory().write_word(timer2 + TimerUnit::count_offset, 0);
        state.memory().write_word(timer2 + TimerUnit::compare_offset, 576);  // one millisecond
        state.memory().write_word(timer2 + TimerUnit::mode_offset,
                                  0x00000180u | 2u);  // CUE | CMPE, CLKS = BUSCLK/256
        state.write_gpr32(4, 11);
        check(kernel.enable_intc(state) == ServiceOutcome::Handled,
              "the timer mask opens through EnableIntc");
        kernel.advance_service_time(state);
        check(state.memory().read_word(timer2 + TimerUnit::count_offset) == 576
                  && kernel.pending_interrupts() == 1
                  && intc.is_pending(11),
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
        state.write_gpr32(4, 2);
        check(kernel.enable_intc(state) == ServiceOutcome::Handled,
              "the VBlank mask opens through EnableIntc");
        for (int index = 0; index < 14; ++index) {
            kernel.advance_service_time(state);
        }
        check(kernel.pending_interrupts() == 1,
              "VBlank waits for a full frame of slices");
        kernel.advance_service_time(state);
        check(kernel.pending_interrupts() == 2,
              "the frame's VBlank joins the queue");
    }

    // The 16-bit wrap through the service clock: 0xFFF0 plus one
    // millisecond at CLKS = BUSCLK/256 (576 ticks) lands on 0x0230 with the
    // overflow edge, while a compare behind the start never fires.
    {
        Kernel kernel;
        GuestState state = make_state();
        TimerUnit timer;
        timer.map_into(state.memory());
        kernel.set_timer_unit(&timer);
        IntcUnit intc;
        intc.map_into(state.memory());
        kernel.set_intc_unit(&intc);
        constexpr std::uint32_t timer2 =
            TimerUnit::window_base + 2 * TimerUnit::timer_stride;
        state.memory().write_word(timer2 + TimerUnit::count_offset, 0xFFF0u);
        state.memory().write_word(timer2 + TimerUnit::compare_offset, 0x5000u);
        state.memory().write_word(timer2 + TimerUnit::mode_offset,
                                  0x00000382u);  // CUE | CMPE | OVFE, CLKS = 2
        state.write_gpr32(4, 11);
        check(kernel.enable_intc(state) == ServiceOutcome::Handled,
              "the wrap test mask opens through EnableIntc");
        kernel.advance_service_time(state);
        check(state.memory().read_word(timer2 + TimerUnit::count_offset)
                      == 0x0230u
                  && (state.memory().read_word(timer2 + TimerUnit::mode_offset)
                          & 0x800u)
                         != 0
                  && (state.memory().read_word(timer2 + TimerUnit::mode_offset)
                          & 0x400u)
                         == 0
                  && kernel.pending_interrupts() == 1 && intc.is_pending(11),
              "0xFFF0 + 576 wraps to 0x0230 with OVFF and no EQUF");
    }

    // The P03 unified machine (decision 0030): one frame of BUSCLK ticks
    // split across the service and idle quanta reaches the identical
    // timer state, kernel time words and pending queue as any other
    // split of the same total. Three wirings of one frame over identical
    // timers on every clock - a single frame advance, sixteen services
    // plus the tail, and two halves - must match in the whole kernel
    // snapshot blob and the full timer register photo.
    {
        constexpr std::uint32_t frame = Kernel::busclk_per_frame;
        constexpr std::uint32_t slice = Kernel::service_time_slice;
        const auto program_clocks = [](GuestState& state, TimerUnit& timer,
                                       Kernel& kernel, IntcUnit& intc) {
            timer.map_into(state.memory());
            kernel.set_timer_unit(&timer);
            intc.map_into(state.memory());
            kernel.set_intc_unit(&intc);
            constexpr std::uint32_t timer0 = TimerUnit::window_base;
            constexpr std::uint32_t timer1 =
                TimerUnit::window_base + TimerUnit::timer_stride;
            constexpr std::uint32_t timer2 =
                TimerUnit::window_base + 2 * TimerUnit::timer_stride;
            constexpr std::uint32_t timer3 =
                TimerUnit::window_base + 3 * TimerUnit::timer_stride;
            state.memory().write_word(timer0 + TimerUnit::count_offset, 0x1000u);
            state.memory().write_word(timer0 + TimerUnit::compare_offset, 0x8000u);
            state.memory().write_word(timer0 + TimerUnit::mode_offset, 0x380u);
            state.memory().write_word(timer1 + TimerUnit::count_offset, 0x2000u);
            state.memory().write_word(timer1 + TimerUnit::compare_offset, 0x4000u);
            state.memory().write_word(timer1 + TimerUnit::mode_offset, 0x381u);
            state.memory().write_word(timer2 + TimerUnit::count_offset, 0x1000u);
            state.memory().write_word(timer2 + TimerUnit::compare_offset, 0x3000u);
            state.memory().write_word(timer2 + TimerUnit::mode_offset, 0x382u);
            state.memory().write_word(timer3 + TimerUnit::count_offset, 0);
            state.memory().write_word(timer3 + TimerUnit::compare_offset, 0x100u);
            state.memory().write_word(timer3 + TimerUnit::mode_offset, 0x383u);
        };
        struct ClockPhoto {
            std::vector<std::uint8_t> kernel_blob;
            std::vector<std::pair<std::uint32_t, std::uint32_t>> timer_regs;
            std::uint32_t pending = 0;
        };
        const auto run_wiring = [&](int wiring) {
            Kernel kernel;
            GuestState state = make_state();
            TimerUnit timer;
            IntcUnit intc;
            program_clocks(state, timer, kernel, intc);
            if (wiring == 0) {
                kernel.advance_busclk(state, frame);
            } else if (wiring == 1) {
                for (int step = 0; step < 16; ++step) {
                    kernel.advance_service_time(state);
                }
                kernel.advance_busclk(state, frame - 16 * slice);
            } else {
                kernel.advance_busclk(state, frame / 2);
                kernel.advance_busclk(state, frame / 2);
            }
            ClockPhoto photo;
            photo.kernel_blob = kernel.save_kernel_state();
            photo.timer_regs = timer.registers_snapshot();
            photo.pending = kernel.pending_interrupts();
            return photo;
        };
        const ClockPhoto whole = run_wiring(0);
        const ClockPhoto serviced = run_wiring(1);
        const ClockPhoto halved = run_wiring(2);
        const auto same_photo = [](const ClockPhoto& left,
                                   const ClockPhoto& right) {
            return left.kernel_blob == right.kernel_blob
                && left.timer_regs == right.timer_regs
                && left.pending == right.pending;
        };
        check(same_photo(whole, serviced) && same_photo(whole, halved),
              "service/idle splits of one frame reach the identical time state");
        // Non-vacuous: every timer fired exactly once, and the exact
        // divisions show their counts (T2 0x1000 + 9600 = 0x3580, T3 262).
        const auto reg_in = [](const ClockPhoto& photo, std::uint32_t address) {
            for (const auto& [at, value] : photo.timer_regs) {
                if (at == address) {
                    return value;
                }
            }
            return 0xFFFFFFFFu;
        };
        constexpr std::uint32_t timer2 =
            TimerUnit::window_base + 2 * TimerUnit::timer_stride;
        constexpr std::uint32_t timer3 =
            TimerUnit::window_base + 3 * TimerUnit::timer_stride;
        check(whole.pending == 4
                  && reg_in(whole, timer2 + TimerUnit::count_offset) == 0x3580u
                  && reg_in(whole, timer3 + TimerUnit::count_offset) == 262u,
              "one frame moves every clock and queues each timer once");
    }

    // Combined compare-plus-overflow through the unified machine: one
    // service-sized advance sets both flags and queues the timer's
    // single cause once (occurrence before eligibility: no handler, no
    // mask); the next advance coalesces onto it.
    {
        Kernel kernel;
        GuestState state = make_state();
        TimerUnit timer;
        timer.map_into(state.memory());
        kernel.set_timer_unit(&timer);
        IntcUnit intc;
        intc.map_into(state.memory());
        kernel.set_intc_unit(&intc);
        constexpr std::uint32_t timer2 =
            TimerUnit::window_base + 2 * TimerUnit::timer_stride;
        state.memory().write_word(timer2 + TimerUnit::count_offset, 0xFFF0u);
        state.memory().write_word(timer2 + TimerUnit::compare_offset, 0x0005u);
        state.memory().write_word(timer2 + TimerUnit::mode_offset, 0x00000382u);
        kernel.advance_service_time(state);
        check(state.memory().read_word(timer2 + TimerUnit::count_offset)
                      == 0x0230u
                  && (state.memory().read_word(timer2 + TimerUnit::mode_offset)
                          & 0xC00u)
                         == 0xC00u
                  && kernel.pending_interrupts() == 1 && intc.is_pending(11),
              "one service reports a combined compare and overflow once");
        kernel.advance_service_time(state);
        check(state.memory().read_word(timer2 + TimerUnit::count_offset)
                      == 0x0470u
                  && kernel.pending_interrupts() == 1,
              "the next advance coalesces onto the pending cause");
    }

    // The scheduler offers an opportunity between timer events: the first
    // compare dispatches the registered handler, its return restores the
    // interrupted context, and the guest's COMP reprogram steers the next
    // advance to the new target (decision 0030).
    {
        Kernel kernel;
        GuestState state = make_state();
        ServiceTable services;
        kernel.register_services(services);
        TimerUnit timer;
        timer.map_into(state.memory());
        kernel.set_timer_unit(&timer);
        IntcUnit intc;
        intc.map_into(state.memory());
        kernel.set_intc_unit(&intc);
        constexpr std::uint32_t timer2 =
            TimerUnit::window_base + 2 * TimerUnit::timer_stride;
        state.memory().write_word(timer2 + TimerUnit::count_offset, 0);
        state.memory().write_word(timer2 + TimerUnit::compare_offset, 576);
        state.memory().write_word(timer2 + TimerUnit::mode_offset, 0x00000182u);
        state.write_gpr32(4, 11);
        state.write_gpr32(5, 0x00100400u);
        state.write_gpr32(7, 0);
        check(kernel.add_intc_handler(state) == ServiceOutcome::Handled,
              "the timer handler registers");
        state.write_gpr32(4, 11);
        check(kernel.enable_intc(state) == ServiceOutcome::Handled,
              "the timer mask opens");
        const ServiceHandler* return_handler =
            services.find(Kernel::patch_return_service);
        check(return_handler != nullptr, "the return service is registered");
        const std::uint32_t interrupted_pc = 0x00100020u;
        state.set_pc(interrupted_pc);
        kernel.advance_service_time(state);
        check(kernel.pending_interrupts() == 1, "the first compare pends");
        check(kernel.start_interrupt(state) && state.pc() == 0x00100400u
                  && state.read_gpr32(4) == 11,
              "the first compare dispatches its handler");
        check((*return_handler)(state) == ServiceOutcome::NoRunnableThread
                  && state.pc() == interrupted_pc
                  && kernel.pending_interrupts() == 0,
              "the handler return stays idle with the context restored");
        // The handler's work, as guest writes: acknowledge EQUF and move
        // COMP out to 2000. Three more services cross it during the third.
        state.memory().write_word(timer2 + TimerUnit::compare_offset, 2000);
        state.memory().write_word(timer2 + TimerUnit::mode_offset, 0x582u);
        kernel.advance_service_time(state);
        kernel.advance_service_time(state);
        check((state.memory().read_word(timer2 + TimerUnit::mode_offset)
                   & 0x400u)
                      == 0
                  && kernel.pending_interrupts() == 0,
              "the reprogrammed COMP stays silent before arrival");
        kernel.advance_service_time(state);
        check(state.memory().read_word(timer2 + TimerUnit::count_offset)
                      == 2304u
                  && (state.memory().read_word(timer2 + TimerUnit::mode_offset)
                          & 0x400u)
                         != 0
                  && kernel.pending_interrupts() == 1,
              "the reprogrammed COMP fires on arrival");
        check(kernel.start_interrupt(state) && state.pc() == 0x00100400u,
              "the second compare offers its own dispatch");
        check((*return_handler)(state) == ServiceOutcome::NoRunnableThread
                  && state.pc() == interrupted_pc,
              "the second return stays idle with the context restored");
    }

    // The guest extended-time rhythm over the unified machine: an OVFE
    // timer with COMP out of reach advances 3 wraps plus a tail of
    // 12345. Acknowledging OVFF between small advances counts every wrap
    // exactly; one giant advance sets the single sticky bit once, which
    // is why the guest must acknowledge between advances.
    {
        constexpr std::uint32_t total = 3 * 65536 + 12345;
        constexpr std::uint32_t mode0 =
            TimerUnit::window_base + TimerUnit::mode_offset;
        const auto program_overflow = [](GuestState& state, TimerUnit& timer,
                                         Kernel& kernel) {
            timer.map_into(state.memory());
            kernel.set_timer_unit(&timer);
            state.memory().write_word(
                TimerUnit::window_base + TimerUnit::count_offset, 0);
            state.memory().write_word(
                TimerUnit::window_base + TimerUnit::compare_offset, 0xFFFFu);
            state.memory().write_word(mode0, 0x280u);
        };
        Kernel kernel;
        GuestState state = make_state();
        TimerUnit timer;
        program_overflow(state, timer, kernel);
        std::uint32_t overflows = 0;
        for (std::uint32_t done = 0; done < total;) {
            const std::uint32_t rest = total - done;
            const std::uint32_t chunk = rest > 1000 ? 1000 : rest;
            kernel.advance_busclk(state, chunk);
            done += chunk;
            if ((state.memory().read_word(mode0) & 0x800u) != 0) {
                ++overflows;
                state.memory().write_word(mode0, 0xA80u);  // keep CUE|OVFE, ack OVFF
            }
        }
        check(overflows == 3
                  && state.memory().read_word(
                         TimerUnit::window_base + TimerUnit::count_offset)
                         == 12345u,
              "acknowledged advances count every overflow");
        Kernel single;
        GuestState single_state = make_state();
        TimerUnit single_timer;
        program_overflow(single_state, single_timer, single);
        single.advance_busclk(single_state, total);
        check((single_state.memory().read_word(mode0) & 0x800u) != 0
                  && single.pending_interrupts() == 1,
              "one giant advance holds a single sticky overflow");
    }

    // The frame accumulator is shared between the quanta: sixteen
    // services (2,359,296 ticks) leave the frame incomplete with nothing
    // queued; registering VBlank and completing the frame with one small
    // advance queues exactly one VBlank, and each further full frame
    // offers exactly one more (decision 0030).
    {
        Kernel kernel;
        GuestState state = make_state();
        ServiceTable services;
        kernel.register_services(services);
        IntcUnit intc;
        intc.map_into(state.memory());
        kernel.set_intc_unit(&intc);
        for (int step = 0; step < 16; ++step) {
            kernel.advance_service_time(state);
        }
        check(kernel.pending_interrupts() == 0,
              "sixteen services leave the frame incomplete");
        state.write_gpr32(4, 2);
        state.write_gpr32(5, 0x00100400u);
        state.write_gpr32(7, 0);
        check(kernel.add_intc_handler(state) == ServiceOutcome::Handled,
              "the VBlank handler registers late");
        state.write_gpr32(4, 2);
        check(kernel.enable_intc(state) == ServiceOutcome::Handled,
              "the VBlank mask opens late");
        kernel.advance_busclk(state, Kernel::busclk_per_frame
                                         - 16 * Kernel::service_time_slice);
        check(kernel.pending_interrupts() == 1,
              "completing the frame queues VBlank");
        const std::uint32_t interrupted_pc = 0x00100020u;
        state.set_pc(interrupted_pc);
        const ServiceHandler* return_handler =
            services.find(Kernel::patch_return_service);
        check(return_handler != nullptr && kernel.start_interrupt(state)
                  && state.pc() == 0x00100400u,
              "the frame's VBlank dispatches");
        check((*return_handler)(state) == ServiceOutcome::NoRunnableThread
                  && kernel.pending_interrupts() == 0,
              "the VBlank return stays idle and drains the queue");
        kernel.advance_busclk(state, Kernel::busclk_per_frame);
        check(kernel.pending_interrupts() == 1,
              "the next full frame offers exactly one more VBlank");
    }

    // Gate and ZeroReturn through the unified machine: a gated timer
    // holds its count across service and frame advances with nothing
    // queued, while a ZeroReturn timer restarts every service-sized
    // period with an edge the guest acknowledges between services.
    {
        Kernel kernel;
        GuestState state = make_state();
        TimerUnit timer;
        timer.map_into(state.memory());
        kernel.set_timer_unit(&timer);
        constexpr std::uint32_t timer0 = TimerUnit::window_base;
        state.memory().write_word(timer0 + TimerUnit::count_offset, 0x100u);
        state.memory().write_word(timer0 + TimerUnit::compare_offset, 0x200u);
        state.memory().write_word(timer0 + TimerUnit::mode_offset, 0x394u);
        for (int step = 0; step < 3; ++step) {
            kernel.advance_service_time(state);
        }
        kernel.advance_busclk(state, Kernel::busclk_per_frame);
        check(state.memory().read_word(timer0 + TimerUnit::count_offset)
                      == 0x100u
                  && kernel.pending_interrupts() == 0,
              "a gated timer holds across both quanta");
        constexpr std::uint32_t timer1 =
            TimerUnit::window_base + TimerUnit::timer_stride;
        state.memory().write_word(timer1 + TimerUnit::count_offset, 0);
        state.memory().write_word(timer1 + TimerUnit::compare_offset, 576);
        state.memory().write_word(timer1 + TimerUnit::mode_offset, 0x1C2u);
        std::uint32_t edges = 0;
        for (int round = 0; round < 3; ++round) {
            kernel.advance_service_time(state);
            if ((state.memory().read_word(timer1 + TimerUnit::mode_offset)
                     & 0x400u)
                != 0) {
                ++edges;
            }
            state.memory().write_word(timer1 + TimerUnit::mode_offset, 0x5C2u);
        }
        check(edges == 3
                  && state.memory().read_word(timer1 + TimerUnit::count_offset)
                         == 0,
              "ZeroReturn restarts every service-sized period");
    }

    // Coalescing (decision 0025): a cause that is already pending stays
    // a single entry, like the hardware status bit the handler reads and
    // clears — queue entries carry no payload, so a repeat adds nothing
    // but backlog (463k stale frames observed without it).
    {
        Kernel kernel;
        kernel.raise_interrupt(2);
        check(kernel.pending_interrupts() == 1,
              "a first VBlank queues");
        kernel.raise_interrupt(2);
        check(kernel.pending_interrupts() == 1,
              "a pending cause coalesces instead of stacking");
        kernel.raise_interrupt(11);
        check(kernel.pending_interrupts() == 2,
              "a different cause still queues");
        kernel.raise_interrupt(11);
        check(kernel.pending_interrupts() == 2,
              "the second cause coalesces too");
    }

    // The file server's open answers from the disc image (decision 0017): the
    // request's path at +8, the reply {handle, size}; a path the disc does
    // not have, or a machine without a disc, answers handle 0.
    {
        class FakeDisc final : public gt4recomp::DiscFiles {
        public:
            [[nodiscard]] std::uint64_t file_size(std::string_view path) const override {
                return path == "cdrom0:\\IRX\\SIO2MAN.IRX;1" ? 6641ull : 0ull;
            }
            [[nodiscard]] std::uint32_t file_extent(std::string_view) const override {
                return 0;
            }
            void read_file(std::string_view, std::uint64_t,
                           std::span<std::uint8_t>) const override {
                throw std::runtime_error("the fake disc serves no reads");
            }
        };

        Kernel kernel;
        GuestState state = make_state();
        FakeDisc disc;
        constexpr std::uint32_t request = 0x00100700;
        const auto write_path = [&state](const char* text) {
            std::uint32_t offset = 8;
            for (const char* cursor = text; *cursor != '\0'; ++cursor, ++offset) {
                state.memory().write_byte(request + offset,
                                          static_cast<std::uint8_t>(*cursor));
            }
            state.memory().write_byte(request + offset, 0);
        };
        std::uint8_t reply[64] = {};
        write_path("cdrom0:\\IRX\\SIO2MAN.IRX;1");
        kernel.set_disc_files(&disc);
        const std::uint32_t open_length =
            kernel.answer_file_open(state, request, 512, reply, sizeof reply);
        check(open_length == 16
                  && reply[0] != 0 && reply[1] == 0 && reply[2] == 0 && reply[3] == 0
                  && reply[4] == 0xF1 && reply[5] == 0x19 && reply[6] == 0
                  && reply[7] == 0,
              "the file open answers the disc's handle and size");
        write_path("cdrom0:\\IRX\\NOPE.IRX;1");
        std::uint8_t missing[64] = {};
        check(kernel.answer_file_open(state, request, 512, missing, sizeof missing) == 16
                  && missing[0] == 0 && missing[4] == 0,
              "a path the disc does not have answers handle 0");
        kernel.set_disc_files(nullptr);
        std::uint8_t nodisc[64] = {};
        check(kernel.answer_file_open(state, request, 512, nodisc, sizeof nodisc) == 16
                  && nodisc[0] == 0,
              "a machine without a disc answers handle 0");
    }

    // The game's own CD read answers from the disc image: the request is
    // {LBA, byte count, EE destination} and the sectors land in the guest
    // (decision 0019).
    {
        class FakeSectors final : public gt4recomp::DiscByteSource {
        public:
            [[nodiscard]] std::uint64_t size() const override {
                return 32 * 2048;
            }
            void read(std::uint64_t offset,
                      std::span<std::uint8_t> destination) const override {
                std::fill(destination.begin(), destination.end(), 0);
                if (offset == 16 * 2048 && destination.size() >= 5) {
                    destination[1] = 'C';
                    destination[2] = 'D';
                    destination[3] = '0';
                    destination[4] = '0';
                    destination[5] = '1';
                }
            }
        };

        Kernel kernel;
        GuestState state = make_state();
        FakeSectors sectors;
        kernel.set_disc_sectors(&sectors);
        constexpr std::uint32_t request = 0x00100800;
        constexpr std::uint32_t destination = 0x00100900;
        state.memory().write_word(request + 0, 16);    // the LBA
        state.memory().write_word(request + 4, 2048);  // one sector
        state.memory().write_word(request + 8, destination);
        check(kernel.answer_disc_read(state, request) == 2048
                  && state.memory().read_byte(destination + 1) == 'C'
                  && state.memory().read_byte(destination + 5) == '1',
              "the disc read lands the sector in the guest");
        state.memory().write_word(request + 0, 0x7FFFFFFF);  // far outside
        bool threw = false;
        try {
            (void)kernel.answer_disc_read(state, request);
        } catch (const std::runtime_error&) {
            threw = true;
        }
        check(threw, "a read outside the disc image stops loudly");
        kernel.set_disc_sectors(nullptr);
        check(kernel.answer_disc_read(state, request) == 2048
                  && state.memory().read_byte(destination + 1) == 0,
              "a machine without a disc answers zeros");
    }

    // The game's own block cache (the PRTS server, sid 0x53545250): the
    // read keeps the block and answers its handle — which the client checks
    // is non-zero before it builds its file object — and the copy-out lands
    // the cached bytes in the guest (decision 0021).
    {
        class FakeBlocks final : public gt4recomp::DiscByteSource {
        public:
            [[nodiscard]] std::uint64_t size() const override {
                return 64 * 2048;
            }
            void read(std::uint64_t offset,
                      std::span<std::uint8_t> destination) const override {
                std::fill(destination.begin(), destination.end(), 0);
                // Mark the block's first byte with its sector number, so the
                // test can tell which block was copied out.
                if (!destination.empty()) {
                    destination[0] = static_cast<std::uint8_t>(offset / 2048);
                }
            }
        };

        Kernel kernel;
        GuestState state = make_state();
        FakeBlocks blocks;
        kernel.set_disc_sectors(&blocks);
        constexpr std::uint32_t request = 0x00100A00;
        constexpr std::uint32_t destination = 0x00100B00;
        state.memory().write_word(request + 0, 3);       // the LBA
        state.memory().write_word(request + 4, 0x40);    // the byte count
        state.memory().write_word(request + 8, 0x8000);  // the flags
        const std::uint32_t handle = kernel.answer_prts_read(state, request);
        check(handle != 0, "the block cache answers a handle");
        state.memory().write_word(request + 0, handle);
        state.memory().write_word(request + 4, destination);
        state.memory().write_word(request + 8, 0x20);
        check(kernel.answer_prts_copy(state, request) == 0x20
                  && state.memory().read_byte(destination) == 3,
              "the copy-out lands the cached block in the guest");
        // Sequential copy-outs advance through the block: the second one
        // lands the next bytes, not the first ones again (decision 0021).
        constexpr std::uint32_t second_destination = 0x00100C00;
        state.memory().write_word(request + 0, handle);
        state.memory().write_word(request + 4, second_destination);
        state.memory().write_word(request + 8, 0x20);
        check(kernel.answer_prts_copy(state, request) == 0x20
                  && state.memory().read_byte(second_destination) == 0,
              "the next copy-out continues where the previous stopped");
        check(kernel.answer_prts_copy(state, request) == 0,
              "a consumed block copies nothing more");
        state.memory().write_word(request + 0, 0x0000DEADu);
        check(kernel.answer_prts_copy(state, request) == 0,
              "an unknown block handle copies nothing");
        state.memory().write_word(request + 0, 5);
        state.memory().write_word(request + 4, 0x20);
        const std::uint32_t second = kernel.answer_prts_read(state, request);
        check(second != 0 && second != handle,
              "each block read answers a fresh handle");
        state.memory().write_word(request + 0, 0x7FFFFFFF);  // far outside
        bool threw = false;
        try {
            (void)kernel.answer_prts_read(state, request);
        } catch (const std::runtime_error&) {
            threw = true;
        }
        check(threw, "a block read outside the disc image stops loudly");
        kernel.set_disc_sectors(nullptr);
        check(kernel.answer_prts_read(state, request) == 0,
              "a machine without a disc answers no handle");
    }

    // The kernel snapshot round-trips every populated structure and keeps
    // serving from it (checkpoint slice C2).
    {
        class FakeBlocks final : public gt4recomp::DiscByteSource {
        public:
            [[nodiscard]] std::uint64_t size() const override {
                return 64 * 2048;
            }
            void read(std::uint64_t offset,
                      std::span<std::uint8_t> destination) const override {
                std::fill(destination.begin(), destination.end(), 0);
                if (!destination.empty()) {
                    destination[0] = static_cast<std::uint8_t>(offset / 2048);
                }
            }
        };

        const auto contexts_equal = [](const RegisterContext& left,
                                       const RegisterContext& right) {
            return left.gpr == right.gpr && left.gpr_high == right.gpr_high
                && left.fpr == right.fpr && left.hi == right.hi
                && left.lo == right.lo && left.hi1 == right.hi1
                && left.lo1 == right.lo1
                && left.fpu_accumulator == right.fpu_accumulator
                && left.fpu_control == right.fpu_control
                && left.shift_amount_cache == right.shift_amount_cache
                && left.cp0 == right.cp0 && left.vu0_vf == right.vu0_vf
                && left.vu0_vi == right.vu0_vi
                && left.vu0_clip_flag == right.vu0_clip_flag
                && left.vu0_acc == right.vu0_acc
                && left.vu0_mac_flag == right.vu0_mac_flag
                && left.vu0_status_flag == right.vu0_status_flag
                && left.pc == right.pc;
        };
        const auto threads_equal = [&](const Kernel& left, const Kernel& right) {
            if (left.threads().size() != right.threads().size()) {
                return false;
            }
            for (std::size_t index = 0; index < left.threads().size(); ++index) {
                const KernelThread& a = left.threads()[index];
                const KernelThread& b = right.threads()[index];
                if (a.id != b.id || a.status != b.status
                    || a.function != b.function || a.stack != b.stack
                    || a.stack_size != b.stack_size || a.gp != b.gp
                    || a.initial_priority != b.initial_priority
                    || a.current_priority != b.current_priority
                    || a.attr != b.attr || a.option != b.option
                    || a.wait_type != b.wait_type || a.wait_id != b.wait_id
                    || a.wakeup_count != b.wakeup_count
                    || !contexts_equal(a.context, b.context)) {
                    return false;
                }
            }
            return true;
        };

        Kernel kernel;
        GuestState state = make_state();
        ServiceTable services;
        kernel.register_services(services);
        FakeBlocks blocks;
        kernel.set_disc_sectors(&blocks);
        check(setup_root(kernel, state), "snapshot root ready");
        check(start_second_thread(kernel, state, 8, 0), "snapshot worker ready");
        write_sema_struct(state, 2, 1);
        state.write_gpr32(4, sema_struct);
        check(kernel.create_sema(state) == ServiceOutcome::Handled
                  && state.read_gpr32(2) == 3,
              "snapshot semaphore ready");
        state.write_gpr32(4, 3);
        check(kernel.signal_sema(state) == ServiceOutcome::Handled,
              "snapshot semaphore signaled");
        state.write_gpr32(4, 0x83);
        state.write_gpr32(5, 0x5B73C8);
        check(kernel.set_syscall(state) == ServiceOutcome::Handled,
              "snapshot patch ready");
        state.write_gpr32(4, 2);
        state.write_gpr32(5, 0x00ABCDEF);
        state.write_gpr32(7, 0x1111);
        check(kernel.add_intc_handler(state) == ServiceOutcome::Handled
                  && state.read_gpr32(2) == 1,
              "snapshot interrupt handler ready");
        state.write_gpr32(4, 1);
        state.write_gpr32(5, 0x00ABCDF0);
        state.write_gpr32(7, 0x2222);
        check(kernel.add_dmac_handler(state) == ServiceOutcome::Handled
                  && state.read_gpr32(2) == 2,
              "snapshot DMA handler ready");
        state.memory().write_word(thread_struct, 0x12345678);
        state.write_gpr32(4, thread_struct);
        check(kernel.set_osd_config(state) == ServiceOutcome::Handled
                  && kernel.osd_config() == 0x12345678u,
              "snapshot OSD value ready");
        constexpr std::uint32_t request = 0x00100A00;
        constexpr std::uint32_t first_chunk = 0x00100B00;
        constexpr std::uint32_t next_chunk = 0x00100C00;
        state.memory().write_word(request + 0, 3);
        state.memory().write_word(request + 4, 0x40);
        state.memory().write_word(request + 8, 0x8000);
        const std::uint32_t handle = kernel.answer_prts_read(state, request);
        check(handle != 0, "snapshot block ready");
        state.memory().write_word(request + 0, handle);
        state.memory().write_word(request + 4, first_chunk);
        state.memory().write_word(request + 8, 0x10);
        check(kernel.answer_prts_copy(state, request) == 0x10
                  && state.memory().read_byte(first_chunk) == 3,
              "snapshot block partly consumed");

        // The SIF reset records the IOP image to boot; the snapshot must
        // carry it (slice 75: load_kernel_state self-moved the member and
        // dropped the parsed image, so resume disagreed with fresh state).
        constexpr std::uint32_t reset_packet = 0x00100200;
        constexpr std::uint32_t reset_descriptors = 0x00100300;
        const std::string reset_image = "rom0:UDNL cdrom0:\\IOPRP300.IMG;1";
        RegisterBank sif_window(0x1000F200u, 0x100u);
        sif_window.map_into(state.memory());
        state.memory().write_word(0x1000F230u, 0x00020000u);
        state.memory().write_word(reset_packet + 0, 104);
        state.memory().write_word(reset_packet + 8, 0x80000003u);
        state.memory().write_word(reset_packet + 16,
                                  static_cast<std::uint32_t>(reset_image.size()));
        state.memory().write_word(reset_packet + 20, 0);
        for (std::uint32_t index = 0; index < reset_image.size(); ++index) {
            state.memory().write_byte(
                reset_packet + 24 + index,
                static_cast<std::uint8_t>(reset_image[index]));
        }
        state.memory().write_word(reset_descriptors + 8, 104);
        state.memory().write_word(reset_descriptors + 0, reset_packet);
        state.memory().write_word(reset_descriptors + 4, 0x00080000u);
        state.write_gpr32(4, reset_descriptors);
        state.write_gpr32(5, 1);
        check(kernel.sif_set_dma(state) == ServiceOutcome::Handled
                  && kernel.sif_iop_image() == reset_image,
               "snapshot IOP image ready");
        const std::vector<std::uint8_t> blob = kernel.save_kernel_state();
        Kernel restored;
        restored.load_kernel_state(blob);
        check(restored.sif_iop_image() == kernel.sif_iop_image()
                  && !restored.sif_iop_image().empty(),
               "the IOP image restores instead of dropping");
        check(threads_equal(kernel, restored), "the threads restore");
        check(kernel.semaphores().size() == restored.semaphores().size()
                  && kernel.semaphores()[0].count
                         == restored.semaphores()[0].count,
              "the semaphores restore");
        check(kernel.patched_handler(0x83) == restored.patched_handler(0x83)
                  && restored.patched_handler(0x83) == 0x5B73C8u,
              "the syscall patch restores");
        check(kernel.osd_config() == restored.osd_config()
                  && kernel.interrupt_handlers().size()
                         == restored.interrupt_handlers().size()
                  && kernel.dmac_handlers().size()
                         == restored.dmac_handlers().size()
                  && kernel.pending_interrupts()
                         == restored.pending_interrupts()
                  && kernel.deferred_call_count()
                         == restored.deferred_call_count()
                  && kernel.current_thread_id() == restored.current_thread_id(),
              "handlers, counts and ids restore");
        check(restored.save_kernel_state() == blob,
              "save-load-save is byte identical");
        // The restored block keeps serving from its cursor, not from zero:
        // pre-fill the buffer so zeros prove the copy ran.
        for (std::uint32_t offset = 0; offset < 0x10; ++offset) {
            state.memory().write_byte(next_chunk + offset, 0xFF);
        }
        state.memory().write_word(request + 0, handle);
        state.memory().write_word(request + 4, next_chunk);
        state.memory().write_word(request + 8, 0x10);
        check(restored.answer_prts_copy(state, request) == 0x10
                  && state.memory().read_byte(next_chunk) == 0,
              "the restored block continues from its cursor");
        std::vector<std::uint8_t> bad_magic = blob;
        bad_magic[0] = 'X';
        check([&] {
                  try {
                      restored.load_kernel_state(bad_magic);
                  } catch (const std::runtime_error&) {
                      return true;
                  }
                  return false;
              }(),
              "a bad kernel magic throws");
        check([&] {
                  try {
                      restored.load_kernel_state({blob.data(), 10});
                  } catch (const std::runtime_error&) {
                      return true;
                  }
                  return false;
              }(),
              "a truncated kernel snapshot throws");
        std::vector<std::uint8_t> trailing = blob;
        trailing.push_back(0);
        check([&] {
                  try {
                      restored.load_kernel_state(trailing);
                  } catch (const std::runtime_error&) {
                      return true;
                  }
                  return false;
              }(),
              "trailing kernel bytes throw");
    }

    // The volume registration (RPC 2) and the volume query (RPC 4): the
    // model recomputes the library's checksum over the same block it serves
    // and answers the registered volume's "volume space size" (decision
    // 0020).
    {
        std::vector<std::uint8_t> descriptor(2048, 0);
        descriptor[0] = 1;
        std::memcpy(descriptor.data() + 1, "CD001", 5);
        descriptor[0x50] = 0x00;
        descriptor[0x51] = 0x01;  // the volume space size 0x100
        std::uint32_t checksum = 0;
        for (std::uint32_t index = 0; index < descriptor.size(); ++index) {
            checksum += static_cast<std::uint32_t>(descriptor[index]) * (index + 1);
        }

        class FakeVolume final : public gt4recomp::DiscByteSource {
        public:
            explicit FakeVolume(std::vector<std::uint8_t> block)
                : block_(std::move(block)) {}
            [[nodiscard]] std::uint64_t size() const override {
                return 32 * 2048;
            }
            void read(std::uint64_t offset,
                      std::span<std::uint8_t> destination) const override {
                std::fill(destination.begin(), destination.end(), 0);
                if (offset == 16 * 2048 && destination.size() >= block_.size()) {
                    std::memcpy(destination.data(), block_.data(), block_.size());
                }
            }

        private:
            std::vector<std::uint8_t> block_;
        };

        Kernel kernel;
        GuestState state = make_state();
        FakeVolume volume(descriptor);
        kernel.set_disc_sectors(&volume);
        constexpr std::uint32_t request = 0x00100800;
        state.memory().write_word(request + 0, 16);  // the descriptor block
        state.memory().write_word(request + 4, checksum);
        check(kernel.answer_disc_volume(state, request) == 2048,
              "the volume registration accepts the library's checksum");
        std::uint8_t reply[8] = {};
        check(kernel.answer_disc_volume_size(reply, sizeof reply) == 8
                  && reply[0] == 1 && reply[1] == 0 && reply[2] == 0
                  && reply[3] == 0 && reply[4] == 0x00 && reply[5] == 0x01
                  && reply[6] == 0 && reply[7] == 0,
              "the volume query answers the registered volume's size");
        state.memory().write_word(request + 4, checksum + 1);
        bool threw = false;
        try {
            (void)kernel.answer_disc_volume(state, request);
        } catch (const std::runtime_error&) {
            threw = true;
        }
        check(threw, "a checksum the image does not produce stops loudly");
        std::uint8_t none[8] = {};
        Kernel empty_kernel;
        check(empty_kernel.answer_disc_volume_size(none, sizeof none) == 8
                  && none[0] == 0 && none[4] == 0,
              "a machine without a disc answers no volume");
    }

    // A run with no runnable thread is the NoRunnableThread outcome, and
    // the machine is explicitly idle afterwards (decision 0031).
    {
        Kernel kernel;
        GuestState state = make_state();
        check(setup_root(kernel, state), "root ready");
        check(kernel.exit_thread(state) == ServiceOutcome::NoRunnableThread,
              "exiting the last thread leaves nothing to run");
        check(kernel.threads()[0].status == ThreadDormant,
              "the exited thread is dormant");
        check(kernel.current_thread_id() == 0,
              "the machine names the idle thread once nothing runs");
        check(kernel.get_thread_id(state) == ServiceOutcome::Handled
                  && state.read_gpr32(2) == 0,
              "GetThreadId at idle names the idle thread");
    }

    // P04 (decision 0031): two handlers for one cause keep their own
    // arguments through the chain, framed as (cause, argument, pc), and
    // the last return restores the interrupted frame over a running
    // thread.
    {
        Kernel kernel;
        GuestState state = make_state();
        ServiceTable services;
        kernel.register_services(services);
        IntcUnit intc;
        intc.map_into(state.memory());
        kernel.set_intc_unit(&intc);
        check(setup_root(kernel, state), "chained root ready");
        state.write_gpr32(4, 7);
        state.write_gpr32(5, 0x00100400u);
        state.write_gpr32(7, 0x00AAA000u);
        check(kernel.add_intc_handler(state) == ServiceOutcome::Handled
                  && state.read_gpr32(2) == 1,
              "the first chained handler registers");
        state.write_gpr32(4, 7);
        state.write_gpr32(5, 0x00100480u);
        state.write_gpr32(7, 0x00BBB000u);
        check(kernel.add_intc_handler(state) == ServiceOutcome::Handled
                  && state.read_gpr32(2) == 2,
              "the second chained handler registers");
        state.write_gpr32(4, 7);
        kernel.enable_intc(state);
        kernel.raise_interrupt(7);
        // Distinctive live registers prove the frame sets every slot.
        const std::uint32_t interrupted_pc = 0x00100020u;
        state.set_pc(interrupted_pc);
        state.write_gpr32(5, 0xDEADu);
        state.write_gpr32(6, 0xBEEFu);
        check(kernel.start_interrupt(state) && state.pc() == 0x00100400u
                  && state.read_gpr32(4) == 7
                  && state.read_gpr32(5) == 0x00AAA000u
                  && state.read_gpr32(6) == interrupted_pc,
              "the first handler keeps its own argument with the pc in a2");
        const ServiceHandler* return_handler =
            services.find(Kernel::patch_return_service);
        check(return_handler != nullptr
                  && (*return_handler)(state) == ServiceOutcome::Jumped
                  && state.pc() == 0x00100480u
                  && state.read_gpr32(4) == 7
                  && state.read_gpr32(5) == 0x00BBB000u
                  && state.read_gpr32(6) == interrupted_pc,
              "the chained handler keeps its own argument too");
        check((*return_handler)(state) == ServiceOutcome::Jumped
                  && state.pc() == interrupted_pc
                  && state.read_gpr32(5) == 0xDEADu
                  && state.read_gpr32(6) == 0xBEEFu
                  && kernel.current_thread_id() == 1
                  && kernel.threads()[0].status == ThreadRun,
              "the last return restores the frame over the running thread");
    }

    // P04 (decision 0031): the DMAC registrations carry their argument
    // the same way, framed with the channel and the interrupted pc.
    {
        Kernel kernel;
        GuestState state = make_state();
        ServiceTable services;
        kernel.register_services(services);
        DmacStatusUnit dmac;
        dmac.map_into(state.memory());
        kernel.set_dmac_unit(&dmac);
        check(setup_root(kernel, state), "dmac-arg root ready");
        state.write_gpr32(4, 2);  // DMAC channel 2 (GIF)
        state.write_gpr32(5, 0x00100400u);
        state.write_gpr32(7, 0x00CCC000u);
        check(kernel.add_dmac_handler(state) == ServiceOutcome::Handled,
              "the DMAC handler registers with its argument");
        state.write_gpr32(4, 2);
        kernel.enable_dmac(state);
        kernel.raise_dmac_completion(2);
        const std::uint32_t interrupted_pc = 0x00100020u;
        state.set_pc(interrupted_pc);
        check(kernel.start_interrupt(state) && state.pc() == 0x00100400u
                  && state.read_gpr32(4) == 2
                  && state.read_gpr32(5) == 0x00CCC000u
                  && state.read_gpr32(6) == interrupted_pc,
              "the DMAC frame carries channel, argument and pc");
        const ServiceHandler* return_handler =
            services.find(Kernel::patch_return_service);
        check(return_handler != nullptr
                  && (*return_handler)(state) == ServiceOutcome::Jumped
                  && state.pc() == interrupted_pc,
              "the DMAC return restores the interrupted context");
    }

    // P04 (decision 0031): interrupted-idle. Blocking the last thread
    // names the idle thread; GetThreadId reports it even inside the
    // handler; a thread the handler wakes is dispatched on return, while
    // a return that woke nothing stays idle with the frame preserved
    // and no waiter restored as RUN.
    {
        const auto reach_idle = [](Kernel& kernel, GuestState& state) {
            if (!setup_root(kernel, state)) {
                return false;
            }
            if (!start_second_thread(kernel, state, 0, 0)) {
                return false;
            }
            if (kernel.get_thread_id(state) != ServiceOutcome::Handled
                || state.read_gpr32(2) != 1) {
                return false;
            }
            write_sema_struct(state, 1, 0);
            state.write_gpr32(4, sema_struct);
            kernel.create_sema(state);  // id 3, count 0
            state.set_pc(0x00100040u);
            state.write_gpr32(4, 3);
            if (kernel.wait_sema(state) != ServiceOutcome::Switched
                || kernel.current_thread_id() != 2) {
                return false;
            }
            state.set_pc(0x00100050u);
            return kernel.sleep_thread(state)
                == ServiceOutcome::NoRunnableThread;
        };
        const auto arm_cause = [](Kernel& kernel, GuestState& state) {
            state.write_gpr32(4, 7);
            state.write_gpr32(5, 0x00100400u);
            state.write_gpr32(7, 0x00AAA000u);
            if (kernel.add_intc_handler(state) != ServiceOutcome::Handled) {
                return false;
            }
            state.write_gpr32(4, 7);
            if (kernel.enable_intc(state) != ServiceOutcome::Handled) {
                return false;
            }
            kernel.raise_interrupt(7);
            return true;
        };

        // The handler wakes the worker: the return dispatches it.
        {
            Kernel kernel;
            GuestState state = make_state();
            ServiceTable services;
            kernel.register_services(services);
            IntcUnit intc;
            intc.map_into(state.memory());
            kernel.set_intc_unit(&intc);
            check(reach_idle(kernel, state)
                      && kernel.current_thread_id() == 0,
                  "blocking the last thread names the idle thread");
            check(kernel.get_thread_id(state) == ServiceOutcome::Handled
                      && state.read_gpr32(2) == 0,
                  "GetThreadId at idle names the idle thread");
            check(arm_cause(kernel, state), "the idle cause is armed");
            const std::uint32_t interrupted_pc = 0x00100060u;
            state.set_pc(interrupted_pc);
            check(kernel.start_interrupt(state)
                      && state.pc() == 0x00100400u
                      && state.read_gpr32(4) == 7
                      && state.read_gpr32(5) == 0x00AAA000u
                      && state.read_gpr32(6) == interrupted_pc,
                  "the idle injection frames cause, argument and pc");
            check(kernel.get_thread_id(state) == ServiceOutcome::Handled
                      && state.read_gpr32(2) == 0,
                  "GetThreadId inside the idle handler still names idle");
            state.write_gpr32(4, 2);
            check(kernel.wakeup_thread(state) == ServiceOutcome::Handled,
                  "the handler wakes the worker");
            const ServiceHandler* return_handler =
                services.find(Kernel::patch_return_service);
            check(return_handler != nullptr
                      && (*return_handler)(state) == ServiceOutcome::Jumped
                      && kernel.current_thread_id() == 2
                      && kernel.threads()[1].status == ThreadRun
                      && kernel.threads()[0].status == ThreadWait
                      && state.pc() == 0x00100054u
                      && state.read_gpr32(2) == 0,
                  "the idle return dispatches the woken worker, not the waiter");
        }

        // Nothing woken: the return stays idle, frame preserved.
        {
            Kernel kernel;
            GuestState state = make_state();
            ServiceTable services;
            kernel.register_services(services);
            IntcUnit intc;
            intc.map_into(state.memory());
            kernel.set_intc_unit(&intc);
            check(reach_idle(kernel, state), "the quiet machine reaches idle");
            check(arm_cause(kernel, state), "the quiet cause is armed");
            const std::uint32_t interrupted_pc = 0x00100060u;
            state.set_pc(interrupted_pc);
            check(kernel.start_interrupt(state), "the quiet handler starts");
            const ServiceHandler* return_handler =
                services.find(Kernel::patch_return_service);
            check(return_handler != nullptr
                      && (*return_handler)(state)
                             == ServiceOutcome::NoRunnableThread
                      && state.pc() == interrupted_pc
                      && kernel.current_thread_id() == 0
                      && kernel.threads()[0].status == ThreadWait
                      && kernel.threads()[1].status == ThreadWait,
                  "with nothing woken the machine stays idle, no WAIT as RUN");
        }
    }

    // P04 (decision 0031): a live handler chain round-trips the snapshot
    // with each argument, and the chain continues after the restore.
    {
        Kernel kernel;
        GuestState state = make_state();
        ServiceTable services;
        kernel.register_services(services);
        IntcUnit intc;
        intc.map_into(state.memory());
        kernel.set_intc_unit(&intc);
        check(setup_root(kernel, state), "snapshot-chain root ready");
        state.write_gpr32(4, 7);
        state.write_gpr32(5, 0x00100400u);
        state.write_gpr32(7, 0x00AAA000u);
        check(kernel.add_intc_handler(state) == ServiceOutcome::Handled,
              "the snapshot chain registers first");
        state.write_gpr32(4, 7);
        state.write_gpr32(5, 0x00100480u);
        state.write_gpr32(7, 0x00BBB000u);
        check(kernel.add_intc_handler(state) == ServiceOutcome::Handled,
              "the snapshot chain registers second");
        state.write_gpr32(4, 7);
        kernel.enable_intc(state);
        kernel.raise_interrupt(7);
        const std::uint32_t interrupted_pc = 0x00100020u;
        state.set_pc(interrupted_pc);
        check(kernel.start_interrupt(state)
                  && kernel.deferred_call_count() == 1,
              "the chain is live before the snapshot");
        const std::vector<std::uint8_t> blob = kernel.save_kernel_state();
        Kernel restored;
        restored.load_kernel_state(blob);
        check(restored.deferred_call_count() == 1
                  && restored.save_kernel_state() == blob,
              "the live chain restores and re-saves identically");
        ServiceTable restored_services;
        restored.register_services(restored_services);
        const ServiceHandler* return_handler =
            restored_services.find(Kernel::patch_return_service);
        check(return_handler != nullptr
                  && (*return_handler)(state) == ServiceOutcome::Jumped
                  && state.pc() == 0x00100480u
                  && state.read_gpr32(5) == 0x00BBB000u
                  && state.read_gpr32(6) == interrupted_pc,
              "the restored chain frames the second argument");
        check((*return_handler)(state) == ServiceOutcome::Jumped
                  && state.pc() == interrupted_pc,
              "the restored chain ends by restoring the frame");
    }

    if (failures != 0) {
        return 1;
    }
    std::cout << "kernel threads, semaphores and the cooperative scheduler behave as specified\n";
    return 0;
    } catch (const std::exception& error) {
        // Headless runs must never hang on a runtime dialog: report an
        // escaping exception as text instead (slice 30's abort hunt).
        std::cerr << "UNCAUGHT: " << error.what() << '\n';
        return 2;
    } catch (...) {
        std::cerr << "UNCAUGHT unknown exception\n";
        return 2;
    }
}

