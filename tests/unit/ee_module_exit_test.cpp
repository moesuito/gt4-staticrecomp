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
};

constexpr std::uint32_t dest_tail[] = {0x24080001u, 0x00200000u};
constexpr std::uint32_t seed_entries[] = {
    jr_case, eret_case, break_case, syscall_case, ind_known,
    ind_unknown, leaf, beql_case, ind_known2, leaf2,
};

GuestState make_state(std::uint32_t pc) {
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

    if (failures != 0) {
        std::cerr << "exit-name probe: " << exit_name(BoundaryKind::StepLimit) << '\n';
        return 1;
    }
    std::cout << "synthetic module exits match the interpreter on 11 legs: "
                 "effect and stop reason\n";
    return 0;
}
