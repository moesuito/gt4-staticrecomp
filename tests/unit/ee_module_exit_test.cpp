// End-to-end P05 fixtures: the header under test is emitted by the real
// gt4translate from tests/data/synth-module-exits.txt (original synthetic
// words, no game content), compiled in, and executed through the boundary
// driver. Every leg runs the same stop twice - once as translated code
// through the driver, once with the interpreter - and compares the full
// effect (registers, cp0, pc, memory) plus the stop reason.
#include "translated-p05-exits.hpp"

#include "gt4recomp/ee_driver.hpp"
#include "gt4recomp/ee_interpreter.hpp"

#include <cstdint>
#include <iostream>
#include <functional>

using namespace gt4recomp;
using namespace gt4recomp::ee;

namespace {

constexpr std::uint32_t window_base = 0x00100000;
constexpr std::size_t window_size = 0x4000;
constexpr std::uint32_t jr_case = 0x00100000u;
constexpr std::uint32_t eret_case = 0x00100010u;
constexpr std::uint32_t break_case = 0x00100014u;
constexpr std::uint32_t syscall_case = 0x00100020u;
constexpr std::uint32_t ind_known = 0x00100030u;
constexpr std::uint32_t ind_unknown = 0x00100038u;
constexpr std::uint32_t leaf = 0x00100040u;
constexpr std::uint32_t ra_landing = 0x00100050u;
constexpr std::uint32_t beql_case = 0x00100060u;
constexpr std::uint32_t beql_slot = 0x00100064u;
constexpr std::uint32_t ind_known2 = 0x00100074u;
constexpr std::uint32_t leaf2 = 0x0010007Cu;
constexpr std::uint32_t common_dest = 0x00103000u;
constexpr std::uint32_t unknown_target = 0x00100FF0u;

// The translated image, word for word the spec file's table;
// tests/data/synth-module-exits.txt is the authority on the layout.
constexpr std::uint32_t image_words[] = {
    0x03E00008u, 0x241F2000u, 0x00000000u, 0x00000000u,
    0x42000018u,
    0x0000000Du,
    0x00000000u, 0x00000000u,
    0x0000000Cu, 0x24080001u, 0x00200000u, 0x00000000u,
    0x01000008u, 0x24420007u,
    0x01000008u, 0x24420007u,
    0x24030009u, 0x03E00008u, 0x00000000u, 0x00000000u,
    0x00200000u, 0x00000000u, 0x00000000u, 0x00000000u,
    0x51000002u, 0x0000000Cu, 0x24080002u, 0x03E00008u, 0x00000000u,
    0x01000008u, 0x00000000u,
    0x0000000Du,
    0x24080003u, 0x2508FFFFu, 0x1500FFFEu, 0x24420001u, 0x03E00008u, 0x00000000u,
    0x0C04002Bu, 0x24420001u, 0x24420002u, 0x0000000Du, 0x00000000u,
    0x24420003u, 0x03E00008u, 0x00000000u,
    0x0C04001Fu, 0x24420001u, 0x24420002u, 0x0000000Du,
    0x0100F809u, 0x24420001u, 0x24420002u, 0x0000000Du,
    0x0804003Au, 0x24420001u, 0x00200000u, 0x00000000u, 0x24420002u, 0x0000000Du,
    0x01095020u, 0x03E00008u, 0x00000000u,
    0x03E00008u, 0x01095020u, 0x00200000u,
    0x11000004u, 0x24420001u, 0x08040046u, 0x24420002u, 0x03E00008u, 0x24420004u,
    0x0000000Du,
    0x03E00008u, 0x00200000u, 0x00200000u,
    0x51000002u, 0x24420001u, 0x24420002u, 0x03E00008u, 0x00000000u,
    0x01004009u, 0x00000000u, 0x0000000Du,
    0xAD090000u, 0x24420001u, 0x0000000Du,
};

constexpr std::uint32_t dest_tail[] = {0x24080001u, 0x00200000u};
constexpr std::uint32_t seed_entries[] = {
    jr_case, eret_case, break_case, syscall_case, ind_known,
    ind_unknown, leaf, beql_case, ind_known2, leaf2,
};

GuestState make_state(std::uint32_t pc, GuestWorkCounter* work = nullptr) {
    GuestMemory memory(window_base, window_size);
    for (std::size_t index = 0; index < std::size(image_words); ++index) {
        memory.write_word(window_base + static_cast<std::uint32_t>(index * 4),
                          image_words[index]);
    }
    memory.write_word(common_dest, dest_tail[0]);
    memory.write_word(common_dest + 4, dest_tail[1]);
    memory.write_word(unknown_target, dest_tail[0]);
    memory.write_word(unknown_target + 4, dest_tail[1]);
    GuestState state(std::move(memory));
    state.set_pc(pc);
    state.set_guest_work_counter(work);
    return state;
}

Module make_module() {
    return Module{
        [](std::uint32_t address) { return translated::has_entry(address); },
        [](GuestState& state, std::uint32_t address) {
            return translated::call_entry(state, address);
        },
    };
}

const char* exit_name(BoundaryKind kind) {
    switch (kind) {
    case BoundaryKind::Syscall: return "syscall";
    case BoundaryKind::Break: return "break";
    case BoundaryKind::ExceptionReturn: return "exception-return";
    case BoundaryKind::UnsupportedWord: return "unsupported-word";
    case BoundaryKind::IndirectTransfer: return "indirect-transfer";
    case BoundaryKind::Returned: return "returned";
    case BoundaryKind::InstructionStop: return "instruction-stop";
    case BoundaryKind::IllegalDelaySlot: return "illegal-delay-slot";
    case BoundaryKind::Unmapped: return "unmapped";
    case BoundaryKind::NoRunnableThread: return "no-runnable-thread";
    case BoundaryKind::StepLimit: return "step-limit";
    }
    return "unknown";
}

struct StopObservation {
    StepResult result{};
    int steps = 0;
};

// Steps until the first non-executed outcome, like the driver's bridge.
StopObservation run_to_stop(GuestState& state) {
    StopObservation observation;
    Interpreter interpreter(state);
    for (int index = 0; index < 16; ++index) {
        ++observation.steps;
        observation.result = interpreter.step();
        if (observation.result.outcome != StepOutcome::Executed) {
            return observation;
        }
    }
    return observation;
}

// The whole effect both engines must agree on: every GPR, all of CP0, the
// pc, and every word of the window. The fixture words never touch the FPU
// file or HI/LO, so both sides carry their identical initial values there.
bool states_match(const GuestState& left, const GuestState& right,
                  const char* label) {
    bool ok = true;
    for (std::uint8_t index = 0; index < 32; ++index) {
        if (left.read_gpr64(index) != right.read_gpr64(index)) {
            std::cerr << label << ": r" << static_cast<unsigned>(index)
                      << " differs\n";
            ok = false;
        }
        if (left.read_cp0(index) != right.read_cp0(index)) {
            std::cerr << label << ": cp0[" << static_cast<unsigned>(index)
                      << "] differs\n";
            ok = false;
        }
    }
    if (left.pc() != right.pc()) {
        std::cerr << label << ": pc differs\n";
        ok = false;
    }
    for (std::uint32_t address = window_base;
         address < window_base + static_cast<std::uint32_t>(window_size);
         address += 4) {
        if (left.memory().read_word(address) != right.memory().read_word(address)) {
            std::cerr << label << ": memory differs at 0x" << std::hex << address
                      << std::dec << '\n';
            ok = false;
            break;
        }
    }
    return ok;
}

} // namespace

int main() {
    int failures = 0;
    const auto check = [&](bool passed, const char* label) {
        if (!passed) {
            std::cerr << label << '\n';
            ++failures;
        }
    };

    // The test memory and the translated entry table agree on the layout.
    for (const std::uint32_t entry : seed_entries) {
        if (!translated::has_entry(entry)) {
            std::cerr << "entry table misses seed 0x" << std::hex << entry
                      << std::dec << '\n';
            return 1;
        }
    }
    check(!translated::has_entry(common_dest), "the common destination is bridged");
    check(!translated::has_entry(unknown_target), "the unknown target is bridged");
    check(!translated::has_entry(ra_landing), "the ra landing is bridged");

    // A service that records its call and answers in v0.
    std::uint32_t service_calls = 0;
    ServiceTable services;
    services.add(0x42u, [&service_calls](GuestState& state) {
        ++service_calls;
        state.write_gpr64(2, 7);
        return ServiceOutcome::Handled;
    });
    ServiceTable no_services;

    // 1. jr ra with the slot rewriting ra: the old target applies, ra updates.
    {
        GuestState direct = make_state(jr_case);
        direct.write_gpr64(31, common_dest);
        const BoundaryKind exit = translated::function_00100000(direct);
        check(exit == BoundaryKind::Returned, "jr: the module reports a return");
        check(direct.pc() == common_dest && direct.read_gpr64(31) == 0x2000u,
              "jr: the old target applies and the slot still rewrites ra");

        GuestState driven = make_state(jr_case);
        driven.write_gpr64(31, common_dest);
        Driver driver(driven, make_module());
        const RunResult result = driver.run(no_services, RunOptions{});
        check(result.stats.module_calls == 1 && result.stats.interpreted_steps == 2,
              "jr: one module call plus the two bridged tail words");
        check(result.boundary.kind == BoundaryKind::UnsupportedWord
                  && result.boundary.pc == common_dest + 4,
              "jr: the bridge stopped past the returned-to destination");

        GuestState reference = make_state(jr_case);
        reference.write_gpr64(31, common_dest);
        const StopObservation stop = run_to_stop(reference);
        check(stop.result.outcome == StepOutcome::Unsupported
                  && stop.result.pc == common_dest + 4,
              "jr: the interpreter stopped at the same tail word");
        check(states_match(driven, reference, "jr"), "jr: effects match");
    }

    // 2-3. eret from EXL and ERL to the common ordinary destination.
    for (const bool error_level : {false, true}) {
        const char* label = error_level ? "eret-erl" : "eret-exl";
        GuestState direct = make_state(eret_case);
        direct.write_gpr64(31, window_base);
        direct.write_cp0(12, error_level ? 0x00000004u : 0x00000002u);
        direct.write_cp0(error_level ? 30 : 14, common_dest);
        const BoundaryKind exit = translated::function_00100010(direct);
        check(exit == BoundaryKind::ExceptionReturn, "eret: the module reports the applied return");
        check(direct.pc() == common_dest && direct.read_cp0(12) == 0,
              "eret: the destination applies and the level clears");

        GuestState driven = make_state(eret_case);
        driven.write_gpr64(31, window_base);
        driven.write_cp0(12, error_level ? 0x00000004u : 0x00000002u);
        driven.write_cp0(error_level ? 30 : 14, common_dest);
        Driver driver(driven, make_module());
        const RunResult result = driver.run(no_services, RunOptions{});
        check(result.stats.module_calls == 1 && result.stats.interpreted_steps == 2,
              "eret: the applied return continued into the bridge");
        check(result.boundary.kind == BoundaryKind::UnsupportedWord
                  && result.boundary.pc == common_dest + 4,
              "eret: the bridge stopped past the common destination");

        GuestState reference = make_state(eret_case);
        reference.write_gpr64(31, window_base);
        reference.write_cp0(12, error_level ? 0x00000004u : 0x00000002u);
        reference.write_cp0(error_level ? 30 : 14, common_dest);
        const StopObservation stop = run_to_stop(reference);
        check(stop.result.outcome == StepOutcome::Unsupported
                  && stop.result.pc == common_dest + 4,
              "eret: the interpreter stopped at the same tail word");
        check(states_match(driven, reference, label), "eret: effects match");
        check(driven.read_cp0(12) == 0, "eret: the level stayed cleared");
    }

    // 4. break with pc equal to ra stops as a trap.
    {
        GuestState direct = make_state(break_case);
        direct.write_gpr64(31, break_case);
        const BoundaryKind exit = translated::function_00100014(direct);
        check(exit == BoundaryKind::Break, "break: the module reports the trap");

        GuestState driven = make_state(break_case);
        driven.write_gpr64(31, break_case);
        Driver driver(driven, make_module());
        RunOptions options;
        options.step_limit = 10;
        const RunResult result = driver.run(no_services, options);
        check(result.stats.module_calls == 1 && result.stats.interpreted_steps == 0,
              "break: one call, no bridge steps, no repeat");
        check(result.boundary.kind == BoundaryKind::Break
                  && result.boundary.pc == break_case,
              "break: pc equal to ra still stops as a trap");

        GuestState reference = make_state(break_case);
        reference.write_gpr64(31, break_case);
        const StopObservation stop = run_to_stop(reference);
        check(stop.result.outcome == StepOutcome::Exception
                  && stop.result.operation == Operation::Break,
              "break: the interpreter names the same trap");
        check(states_match(driven, reference, "break"), "break: effects match");
    }

    // 5. syscall on the plain path, unhandled: the stop carries the service.
    {
        GuestState direct = make_state(syscall_case);
        direct.write_gpr64(3, 0x42u);
        const BoundaryKind exit = translated::function_00100020(direct);
        check(exit == BoundaryKind::Syscall, "syscall: the module reports the pending call");
        check(direct.pc() == syscall_case, "syscall: the pc stays at the word");

        GuestState driven = make_state(syscall_case);
        driven.write_gpr64(3, 0x42u);
        Driver driver(driven, make_module());
        const RunResult result = driver.run(no_services, RunOptions{});
        check(result.stats.module_calls == 1 && result.stats.interpreted_steps == 0,
              "syscall: the unhandled call stops the run");
        check(result.boundary.kind == BoundaryKind::Syscall
                  && result.boundary.pc == syscall_case
                  && result.boundary.service == 0x42u,
              "syscall: the boundary carries the pc and the v1 service");

        GuestState reference = make_state(syscall_case);
        reference.write_gpr64(3, 0x42u);
        const StopObservation stop = run_to_stop(reference);
        check(stop.result.outcome == StepOutcome::Exception
                  && stop.result.operation == Operation::Syscall
                  && stop.result.pc == syscall_case,
              "syscall: the interpreter stops at the same word");
        check(states_match(driven, reference, "syscall"), "syscall: effects match");
        check(service_calls == 0, "syscall: no service ran on the unhandled path");
    }

    // 6. The same syscall with a handler: it runs exactly once, then the
    // bridge continues past pc + 4.
    {
        service_calls = 0;
        GuestState driven = make_state(syscall_case);
        driven.write_gpr64(3, 0x42u);
        Driver driver(driven, make_module());
        const RunResult result = driver.run(services, RunOptions{});
        check(service_calls == 1 && result.stats.services_handled == 1,
              "syscall: the service ran exactly once");
        check(driven.read_gpr64(2) == 7, "syscall: the service answer landed");
        check(result.stats.module_calls == 1 && result.stats.interpreted_steps == 2,
              "syscall: the bridge stepped the two tail words");
        check(result.boundary.kind == BoundaryKind::UnsupportedWord
                  && result.boundary.pc == syscall_case + 8,
              "syscall: the run stopped past the handled call");
        check(driven.pc() == syscall_case + 8
                  && driven.read_gpr64(8) == 1u,
              "syscall: the tail effect applied once");
    }

    // 7. Known indirect target: the module dispatches internally, the slot
    // ran once, the leaf's return propagates.
    {
        GuestState direct = make_state(ind_known);
        direct.write_gpr64(8, leaf);
        direct.write_gpr64(31, ra_landing);
        const BoundaryKind exit = translated::function_00100030(direct);
        check(exit == BoundaryKind::Returned, "known: the leaf return propagates");
        check(direct.pc() == ra_landing, "known: the run returned through ra");
        check(direct.read_gpr64(2) == 7 && direct.read_gpr64(3) == 9,
              "known: the slot ran once before the leaf effect");

        GuestState driven = make_state(ind_known);
        driven.write_gpr64(8, leaf);
        driven.write_gpr64(31, ra_landing);
        Driver driver(driven, make_module());
        const RunResult result = driver.run(no_services, RunOptions{});
        check(result.stats.module_calls == 1 && result.stats.interpreted_steps == 1,
              "known: everything translated, one bridged word");
        check(result.boundary.kind == BoundaryKind::UnsupportedWord
                  && result.boundary.pc == ra_landing,
              "known: the bridge stopped at the ra landing");

        GuestState reference = make_state(ind_known);
        reference.write_gpr64(8, leaf);
        reference.write_gpr64(31, ra_landing);
        const StopObservation stop = run_to_stop(reference);
        check(stop.result.outcome == StepOutcome::Unsupported
                  && stop.result.pc == ra_landing,
              "known: the interpreter stopped at the same word");
        check(states_match(driven, reference, "known"), "known: effects match");
    }

    // 8. Unknown indirect target: the module stops with the slot untouched
    // and the bridge applies the transfer exactly once.
    {
        GuestState direct = make_state(ind_unknown);
        direct.write_gpr64(8, unknown_target);
        direct.write_gpr64(31, ra_landing);
        const BoundaryKind exit = translated::function_00100038(direct);
        check(exit == BoundaryKind::IndirectTransfer,
              "unknown: the module reports the pending transfer");
        check(direct.pc() == ind_unknown && direct.read_gpr64(2) == 0,
              "unknown: the pc stays at the transfer with the slot untouched");
        check(direct.read_gpr64(31) == ra_landing, "unknown: no link was written");

        GuestState driven = make_state(ind_unknown);
        driven.write_gpr64(8, unknown_target);
        driven.write_gpr64(31, ra_landing);
        Driver driver(driven, make_module());
        const RunResult result = driver.run(no_services, RunOptions{});
        check(result.stats.module_calls == 1 && result.stats.interpreted_steps == 4,
              "unknown: the bridge applied the transfer, slot and tail");
        check(result.boundary.kind == BoundaryKind::UnsupportedWord
                  && result.boundary.pc == unknown_target + 4,
              "unknown: the run stopped past the reached target");
        check(driven.read_gpr64(2) == 7, "unknown: the slot ran exactly once");

        GuestState reference = make_state(ind_unknown);
        reference.write_gpr64(8, unknown_target);
        reference.write_gpr64(31, ra_landing);
        const StopObservation stop = run_to_stop(reference);
        check(stop.result.outcome == StepOutcome::Unsupported
                  && stop.result.pc == unknown_target + 4,
              "unknown: the interpreter stopped at the same word");
        check(states_match(driven, reference, "unknown"), "unknown: effects match");
    }

    // 9. Likely branch taken with a syscall in the taken-only slot: the
    // module stops at the slot word, where the interpreter stops too.
    {
        GuestState direct = make_state(beql_case);
        direct.write_gpr64(8, 0);
        direct.write_gpr64(3, 0x42u);
        direct.write_gpr64(31, ra_landing);
        const BoundaryKind exit = translated::function_00100060(direct);
        check(exit == BoundaryKind::Syscall, "beql-taken: the module reports the slot call");
        check(direct.pc() == beql_slot, "beql-taken: the pc stays at the slot");

        GuestState driven = make_state(beql_case);
        driven.write_gpr64(8, 0);
        driven.write_gpr64(3, 0x42u);
        driven.write_gpr64(31, ra_landing);
        Driver driver(driven, make_module());
        const RunResult result = driver.run(no_services, RunOptions{});
        check(result.stats.module_calls == 1 && result.stats.interpreted_steps == 0,
              "beql-taken: the slot call stops the run unhandled");
        check(result.boundary.kind == BoundaryKind::Syscall
                  && result.boundary.pc == beql_slot
                  && result.boundary.service == 0x42u,
              "beql-taken: the boundary carries the slot pc and service");

        GuestState reference = make_state(beql_case);
        reference.write_gpr64(8, 0);
        reference.write_gpr64(3, 0x42u);
        reference.write_gpr64(31, ra_landing);
        const StopObservation stop = run_to_stop(reference);
        check(stop.result.outcome == StepOutcome::Exception
                  && stop.result.operation == Operation::Syscall
                  && stop.result.pc == beql_slot,
              "beql-taken: the interpreter stops at the same slot");
        check(states_match(driven, reference, "beql-taken"), "beql-taken: effects match");
    }

    // 10. The same branch not taken: the slot nullifies and the run returns.
    {
        GuestState direct = make_state(beql_case);
        direct.write_gpr64(8, 1);
        direct.write_gpr64(31, ra_landing);
        const BoundaryKind exit = translated::function_00100060(direct);
        check(exit == BoundaryKind::Returned, "beql-live: the run returns");
        check(direct.pc() == ra_landing && direct.read_gpr64(8) == 2u,
              "beql-live: the fall-through effect applied and ra is intact");

        GuestState driven = make_state(beql_case);
        driven.write_gpr64(8, 1);
        driven.write_gpr64(31, ra_landing);
        Driver driver(driven, make_module());
        const RunResult result = driver.run(no_services, RunOptions{});
        check(result.stats.module_calls == 1 && result.stats.interpreted_steps == 1,
              "beql-live: one call plus the bridged landing");
        check(result.boundary.kind == BoundaryKind::UnsupportedWord
                  && result.boundary.pc == ra_landing,
              "beql-live: the bridge stopped at the landing");

        GuestState reference = make_state(beql_case);
        reference.write_gpr64(8, 1);
        reference.write_gpr64(31, ra_landing);
        const StopObservation stop = run_to_stop(reference);
        check(stop.result.outcome == StepOutcome::Unsupported
                  && stop.result.pc == ra_landing,
              "beql-live: the interpreter stopped at the same word");
        check(states_match(driven, reference, "beql-live"), "beql-live: effects match");
    }

    // 11. A trap reached only through the internal dispatch propagates
    // without being overwritten by the dispatching entry.
    {
        GuestState direct = make_state(ind_known2);
        direct.write_gpr64(8, leaf2);
        direct.write_gpr64(31, ra_landing);
        const BoundaryKind exit = translated::function_00100074(direct);
        check(exit == BoundaryKind::Break, "inner-trap: the reason propagates");
        check(direct.pc() == leaf2, "inner-trap: the pc stays at the trap");

        GuestState driven = make_state(ind_known2);
        driven.write_gpr64(8, leaf2);
        driven.write_gpr64(31, ra_landing);
        Driver driver(driven, make_module());
        const RunResult result = driver.run(no_services, RunOptions{});
        check(result.stats.module_calls == 1 && result.stats.interpreted_steps == 0,
              "inner-trap: the nested stop ends the run");
        check(result.boundary.kind == BoundaryKind::Break
                  && result.boundary.pc == leaf2,
              "inner-trap: the driver reports the inner trap");

        GuestState reference = make_state(ind_known2);
        reference.write_gpr64(8, leaf2);
        reference.write_gpr64(31, ra_landing);
        const StopObservation stop = run_to_stop(reference);
        check(stop.result.outcome == StepOutcome::Exception
                  && stop.result.operation == Operation::Break
                  && stop.result.pc == leaf2,
              "inner-trap: the interpreter stops at the same trap");
        check(states_match(driven, reference, "inner-trap"), "inner-trap: effects match");
    }

    // Observation is tested against hand counts, not merely against itself.
    // A failed step is an attempt, not another completed instruction.
    const auto check_work = [&](const char* label, std::uint32_t entry,
                                std::uint64_t direct_count, std::uint64_t run_count,
                                const std::function<void(GuestState&)>& configure,
                                bool accept_service = false) {
        GuestWorkCounter direct_work;
        auto direct = make_state(entry, &direct_work);
        direct.write_gpr64(31, ra_landing);
        configure(direct);
        (void)translated::call_entry(direct, entry);
        check(direct_work.completed_instructions == direct_count, label);

        GuestWorkCounter driven_work;
        auto driven = make_state(entry, &driven_work);
        driven.write_gpr64(31, ra_landing);
        configure(driven);
        Driver driver(driven, make_module());
        const auto run = driver.run(accept_service ? services : no_services, RunOptions{});
        check(driven_work.completed_instructions == run_count, label);
        check(driven_work.accepted_services == (accept_service ? 1u : 0u), label);

        GuestWorkCounter reference_work;
        auto reference = make_state(entry, &reference_work);
        reference.write_gpr64(31, ra_landing);
        configure(reference);
        Interpreter interpreter(reference);
        bool stopped = false;
        for (int attempt = 0; attempt < 100; ++attempt) {
            const auto step = interpreter.step();
            if (step.outcome == StepOutcome::Executed) {
                continue;
            }
            // Separate test service owner: the interpreter stops before the
            // syscall; only successful acceptance contributes its word.
            if (accept_service && step.operation == Operation::Syscall
                && !interpreter.pending_transfer()) {
                const auto* handler = services.find(reference.read_gpr32(3));
                if (handler != nullptr && (*handler)(reference) == ServiceOutcome::Handled) {
                    reference.record_accepted_service();
                    reference.set_pc(step.pc + 4);
                    continue;
                }
            }
            stopped = true;
            break;
        }
        check(stopped, label);
        check(reference_work.completed_instructions == run_count, label);
        check(reference_work.accepted_services == driven_work.accepted_services, label);
        check(states_match(driven, reference, label), label);

        const auto before = driven_work.completed_instructions;
        const auto again = driver.run(no_services, RunOptions{});
        check(again.boundary.pc == run.boundary.pc
                  && driven_work.completed_instructions == before,
              "repeated native stop is not completed work");
        (void)interpreter.step();
        check(reference_work.completed_instructions == run_count,
              "repeated interpreter stop is not completed work");

        auto unobserved = make_state(entry);
        unobserved.write_gpr64(31, ra_landing);
        configure(unobserved);
        Driver plain_driver(unobserved, make_module());
        const auto plain = plain_driver.run(accept_service ? services : no_services, RunOptions{});
        check(plain.boundary.pc == run.boundary.pc && plain.boundary.kind == run.boundary.kind
                  && states_match(driven, unobserved, label),
              "work observation cannot change guest effects or boundary");
    };
    const auto unchanged = [](GuestState&) {};
    check_work("work jr+slot+bridge tail = 3", jr_case, 2, 3,
               [](GuestState& s) { s.write_gpr64(31, common_dest); });
    for (bool error_level : {false, true}) {
        check_work("work eret+tail = 2", eret_case, 1, 2,
                   [error_level](GuestState& s) {
                       s.write_cp0(12, error_level ? 4u : 2u);
                       s.write_cp0(error_level ? 30 : 14, common_dest);
                   });
    }
    check_work("work break = 0", break_case, 0, 0, unchanged);
    const auto service42 = [](GuestState& s) { s.write_gpr64(3, 0x42u); };
    check_work("work unhandled syscall = 0", syscall_case, 0, 0, service42);
    check_work("work accepted syscall+tail = 2", syscall_case, 0, 2, service42, true);
    check_work("work known indirect = 5", ind_known, 5, 5,
               [](GuestState& s) { s.write_gpr64(8, leaf); });
    check_work("work unknown indirect = 0 native / 3 bridged", ind_unknown, 0, 3,
               [](GuestState& s) { s.write_gpr64(8, unknown_target); });
    check_work("work likely trap = 1", beql_case, 1, 1, service42);
    check_work("work nullified syscall = 4", beql_case, 4, 4,
               [](GuestState& s) { s.write_gpr64(8, 1); });
    check_work("work internal trap = 2", ind_known2, 2, 2,
               [](GuestState& s) { s.write_gpr64(8, leaf2); });
    check_work("work 3-pass loop = 1+3*3+2 = 12", 0x00100080u, 12, 12, unchanged);
    check_work("work direct call+leaf+continuation = 6", 0x00100098u, 6, 6, unchanged);
    check_work("work direct callee trap = 2", 0x001000B8u, 2, 2, unchanged);
    check_work("work known jalr = 6", 0x001000C8u, 6, 6,
               [](GuestState& s) { s.write_gpr64(8, 0x001000ACu); });
    check_work("work unknown jalr = 0 native / 3 bridged", 0x001000C8u, 0, 3,
               [](GuestState& s) { s.write_gpr64(8, unknown_target); });
    check_work("work jump+slot+target = 3", 0x001000D8u, 3, 3, unchanged);
    const auto overflow = [](GuestState& s) {
        s.write_gpr64(8, 0x7FFFFFFFu);
        s.write_gpr64(9, 1);
    };
    check_work("work trapping effect = 0", 0x001000F0u, 0, 0, overflow);
    check_work("work successful checked fallback+return = 3", 0x001000F0u, 3, 3, unchanged);
    check_work("work completed jr before trapping slot = 1", 0x001000FCu, 1, 1, overflow);
    check_work("work standalone slot path = 3", 0x00100108u, 3, 3, unchanged);
    check_work("work inline slot path = 6", 0x00100108u, 6, 6,
               [](GuestState& s) { s.write_gpr64(8, 1); });
    check_work("work unsupported slot = 1", 0x00100124u, 1, 1, unchanged);
    check_work("work likely taken ordinary slot = 4", 0x00100130u, 4, 4, unchanged);
    check_work("work likely nullified ordinary slot = 4", 0x00100130u, 4, 4,
               [](GuestState& s) { s.write_gpr64(8, 1); });
    check_work("work jalr rd==rs = 5", 0x00100144u, 5, 5,
               [](GuestState& s) { s.write_gpr64(8, 0x001000ACu); });

    {
        GuestWorkCounter native_work;
        auto state = make_state(0x00100150u, &native_work);
        // Ordinary synthetic RAM at a CHCR address; no hardware model is
        // inferred here. The existing poll hook stands in for an injector.
        state.memory().map_region(0x1000A000u, 4);
        state.write_gpr64(8, 0x1000A000u);
        state.write_gpr64(9, 0x100u);
        state.set_dma_start_poll([&native_work, &check](GuestState& running) {
            check(native_work.completed_instructions == 1
                      && running.memory().read_word(0x1000A000u) == 0x100u,
                  "DMA poll observes a completed, counted store before unwinding");
            return true;
        });
        const auto exit = translated::call_entry(state, 0x00100150u);
        check(exit == BoundaryKind::Returned && state.pc() == 0x00100154u
                  && native_work.completed_instructions == 1 && state.read_gpr64(2) == 0,
              "native poll exit does not execute or count the obsolete continuation");
        Driver driver(state, make_module());
        (void)driver.run(no_services, RunOptions{});
        check(native_work.completed_instructions == 2,
              "continuation after a poll exit counts once in the bridge");

        GuestWorkCounter reference_work;
        auto reference = make_state(0x00100150u, &reference_work);
        reference.memory().map_region(0x1000A000u, 4);
        reference.write_gpr64(8, 0x1000A000u);
        reference.write_gpr64(9, 0x100u);
        (void)run_to_stop(reference);
        check(reference_work.completed_instructions == 2
                  && states_match(state, reference, "DMA poll work"),
              "counted native poll exit matches the completed interpreter path");
    }

    // Confirm the preexisting service-in-likely-slot gap, not a new promise
    // of parity: the native stop has no pending transfer, the interpreter's
    // taken branch does. No scheduling or trap-resume semantics change here.
    {
        GuestWorkCounter native_work;
        auto state = make_state(beql_case, &native_work);
        state.write_gpr64(3, 0x42u);
        state.write_gpr64(31, ra_landing);
        Driver driver(state, make_module());
        const auto native = driver.run(services, RunOptions{});
        GuestWorkCounter reference_work;
        auto reference = make_state(beql_case, &reference_work);
        reference.write_gpr64(3, 0x42u);
        reference.write_gpr64(31, ra_landing);
        Interpreter interpreter(reference);
        (void)interpreter.step();
        const auto stop = interpreter.step();
        check(native_work.completed_instructions == 5 && native_work.accepted_services == 1
                  && native.boundary.pc == ra_landing,
              "known gap: native likely-slot service is accepted");
        check(reference_work.completed_instructions == 1 && reference_work.accepted_services == 0
                  && stop.outcome == StepOutcome::Exception && stop.pc == beql_slot
                  && interpreter.pending_transfer(),
              "known gap: interpreter leaves the likely-slot service pending");
    }

    // Acceptance outcomes count the syscall once, including private returns
    // that restore/jump contexts and the no-runnable-thread boundary.
    for (const auto outcome : {ServiceOutcome::Handled, ServiceOutcome::Switched,
                               ServiceOutcome::Jumped, ServiceOutcome::NoRunnableThread,
                               ServiceOutcome::Unhandled}) {
        GuestWorkCounter work;
        auto state = make_state(syscall_case, &work);
        state.write_gpr64(3, 0x100u);
        ServiceTable table;
        table.add(0x100u, [outcome](GuestState& s) {
            if (outcome == ServiceOutcome::Switched || outcome == ServiceOutcome::Jumped) {
                const auto context = s.save_registers();
                s.restore_registers(context);
                s.set_pc(syscall_case + 4);
            }
            return outcome;
        });
        Driver driver(state, make_module());
        (void)driver.run(table, RunOptions{});
        const auto expected = outcome == ServiceOutcome::Unhandled ? 0u
            : outcome == ServiceOutcome::NoRunnableThread ? 1u : 2u;
        check(work.completed_instructions == expected
                  && work.accepted_services == (outcome == ServiceOutcome::Unhandled ? 0u : 1u),
              "each accepted outcome records exactly one service word");
    }
    {
        GuestWorkCounter work;
        auto state = make_state(syscall_case, &work);
        service42(state);
        Driver driver(state, make_module());
        RunOptions limited;
        limited.service_limit = 0;
        (void)driver.run(services, limited);
        check(work.completed_instructions == 0, "budget-limited syscall is not accepted work");
        (void)driver.run(services, RunOptions{});
        check(work.completed_instructions == 2 && work.accepted_services == 1,
              "segmented stop/resume counts only the eventual completed service and tail");
    }

    if (failures != 0) {
        std::cerr << "exit-name probe: " << exit_name(BoundaryKind::StepLimit) << '\n';
        return 1;
    }
    std::cout << "synthetic module exits match the interpreter on 11 legs: "
                 "effect and stop reason; 26 hand-counted work paths, "
                 "service outcomes, segmented stops and DMA poll verified; "
                 "treated likely-slot service gap retained explicitly\n";
    return 0;
}
