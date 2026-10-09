// Unit tests for the boundary driver and its service layer, with no game
// data: fake modules prove that the driver executes entries, trusts the
// explicit exit reason each one reports (never a pc == ra guess), resolves
// syscalls through the service table, bridges through the interpreter when
// the module cannot pass a boundary, and reports what nothing can resolve.
#include "gt4recomp/ee_driver.hpp"

#include <cstdint>
#include <initializer_list>
#include <iostream>
#include <utility>
#include <vector>

using namespace gt4recomp::ee;

namespace {

constexpr std::uint32_t window_base = 0x00100000;
constexpr std::size_t window_size = 0x1000;

// The hand-assembled words the tests write into the window.
constexpr std::uint32_t syscall_word = 0x0000000Cu;      // syscall
constexpr std::uint32_t break_word = 0x0000000Du;        // break
constexpr std::uint32_t eret_word = 0x42000018u;         // eret
constexpr std::uint32_t jalr_word = 0x0320F809u;         // jalr ra, t9
constexpr std::uint32_t jump_self_word = 0x0804001Cu;    // j 0x00100070
constexpr std::uint32_t unsupported_word = 0x00200000u;  // sll with rs != 0
constexpr std::uint32_t ordinary_word = 0x24080001u;     // addiu t0, zero, 1

GuestState make_state(std::uint32_t pc) {
    GuestMemory memory(window_base, window_size);
    GuestState state(std::move(memory));
    state.set_pc(pc);
    return state;
}

void write_words(GuestState& state, std::uint32_t address,
                 std::initializer_list<std::uint32_t> words) {
    for (const std::uint32_t word : words) {
        state.memory().write_word(address, word);
        address += 4;
    }
}

// Fake module functions: each one behaves like a generated function and
// reports the explicit exit reason the emitter would produce for the same
// stop, leaving the pc where the translator would.
BoundaryKind stop_at_syscall(GuestState& state) {
    state.set_pc(0x00100010u);
    return BoundaryKind::Syscall;
}
BoundaryKind stop_at_jalr(GuestState& state) {
    state.set_pc(0x00100040u);
    return BoundaryKind::IndirectTransfer;
}
BoundaryKind stop_at_break(GuestState& state) {
    state.set_pc(0x00100020u);
    return BoundaryKind::Break;
}
BoundaryKind stop_at_eret(GuestState& state) {
    // What the translator emits for eret: derive the pc from CP0, clear the
    // level the return leaves, and report the applied return. The pc never
    // sits at the eret word itself, unlike the old mock.
    const std::uint32_t status = state.read_cp0(12);
    if ((status & 0x00000004u) != 0) {
        state.set_pc(state.read_cp0(30));
        state.write_cp0(12, status & ~0x00000004u);
    } else {
        state.set_pc(state.read_cp0(14));
        state.write_cp0(12, status & ~0x00000002u);
    }
    return BoundaryKind::ExceptionReturn;
}
BoundaryKind stop_at_unsupported(GuestState& state) {
    state.set_pc(0x00100050u);
    return BoundaryKind::UnsupportedWord;
}
BoundaryKind stop_at_ordinary(GuestState& state) {
    state.set_pc(0x00100060u);
    return BoundaryKind::InstructionStop;
}
BoundaryKind stop_at_unmapped(GuestState& state) {
    state.set_pc(0x00200000u);
    return BoundaryKind::Unmapped;
}
BoundaryKind return_to_loop(GuestState& state) {
    // A real module stop is a boundary word or a return; the loop then runs
    // in the bridge until the work budget stops it.
    state.write_gpr64(31, 0x00100070u);
    state.set_pc(0x00100070u);
    return BoundaryKind::Returned;
}
BoundaryKind return_through_ra(GuestState& state) {
    state.set_pc(static_cast<std::uint32_t>(state.read_gpr64(31)));
    return BoundaryKind::Returned;
}
// A known indirect target reached through the module's own table: the leaf
// reports its reason and the dispatching entry propagates it unwritten, like
// generated code does after detail::call_entry.
BoundaryKind leaf_returns(GuestState& state) {
    state.write_gpr64(3, 9);
    state.set_pc(static_cast<std::uint32_t>(state.read_gpr64(31)));
    return BoundaryKind::Returned;
}
BoundaryKind stop_at_known_jr(GuestState& state) {
    state.write_gpr64(2, state.read_gpr64(2) + 7);  // the delay slot, once
    return leaf_returns(state);
}
BoundaryKind leaf_traps(GuestState& state) {
    state.set_pc(0x00100020u);
    return BoundaryKind::Break;
}
BoundaryKind stop_at_known_jr_to_trap(GuestState& state) {
    return leaf_traps(state);
}
BoundaryKind stop_at_self_transfer(GuestState& state) {
    // A pending transfer whose word is itself a module entry: running the
    // entry again could only repeat the stop, so the bridge must run next.
    state.set_pc(window_base);
    return BoundaryKind::IndirectTransfer;
}

struct StopCase {
    const char* label;
    std::uint32_t pc;
    std::uint32_t word;
    std::uint32_t ra;
    BoundaryKind expected;
};

const StopCase stop_cases[] = {
    {"syscall", 0x00100010u, syscall_word, 0, BoundaryKind::Syscall},
    {"break", 0x00100020u, break_word, 0, BoundaryKind::Break},
    {"eret", 0x00100030u, eret_word, 0, BoundaryKind::ExceptionReturn},
    {"jalr", 0x00100040u, jalr_word, 0, BoundaryKind::IndirectTransfer},
    {"unsupported", 0x00100050u, unsupported_word, 0, BoundaryKind::UnsupportedWord},
    {"ordinary", 0x00100060u, ordinary_word, 0, BoundaryKind::InstructionStop},
    {"trap-equals-ra", 0x00100020u, break_word, 0x00100020u, BoundaryKind::Break},
    {"unmapped", 0x00200000u, 0, 0, BoundaryKind::Unmapped},
    {"misaligned", 0x00100001u, 0, 0, BoundaryKind::Unmapped},
};

// A service that records its call and answers in v0.
struct FakeService {
    std::uint32_t calls = 0;
    std::uint32_t answer = 0;
};

ServiceHandler make_fake_service(FakeService& service) {
    return [&service](GuestState& state) {
        ++service.calls;
        state.write_gpr64(2, service.answer);
        return ServiceOutcome::Handled;
    };
}

} // namespace

int main() {
    int failures = 0;
    const auto check = [&](bool passed, const char* label) {
        if (!passed) { std::cerr << label << '\n'; ++failures; }
    };

    // Slice 99 audit controls, not architectural acceptance: record the
    // CURRENT bridge event eligibility. The ordinary not-taken slot is
    // exposed to a callback; reference equivalence is a separate question.
    struct BranchAuditCase {
        std::uint32_t word;
        bool likely;
        std::uint64_t not_taken_operand;
    };
    const BranchAuditCase audited_branches[] = {
        {0x11000002u, false, 1u},                   // BEQ t0,zero,+2
        {0x05010002u, false, 0xFFFFFFFFFFFFFFFFull}, // BGEZ t0,+2
        {0x51000002u, true, 1u},                    // BEQL t0,zero,+2
    };
    for (const auto& audited_branch : audited_branches) {
        const bool likely = audited_branch.likely;
        for (const bool taken : {false, true}) {
            const auto branch = audited_branch.word;
            const auto expected_probes = taken
                ? std::vector<std::uint32_t>{window_base, window_base + 12}
                : likely
                    ? std::vector<std::uint32_t>{window_base, window_base + 8, window_base + 12}
                    : std::vector<std::uint32_t>{window_base, window_base + 4,
                                                 window_base + 8, window_base + 12};
            {
                GuestWorkCounter work;
                auto state = make_state(window_base);
                state.set_guest_work_counter(&work);
                write_words(state, window_base,
                            {branch, 0x24020011u, 0x24020022u, break_word});
                state.write_gpr64(8, taken ? 0u : audited_branch.not_taken_operand);
                std::vector<std::uint32_t> probes;
                RunOptions options;
                options.start_interrupt = [&probes](GuestState& running) {
                    probes.push_back(running.pc());
                    return false;
                };
                Driver driver(state, module_from_entries({}));
                ServiceTable services;
                const auto result = driver.run(services, options);
                check(probes == expected_probes,
                      "audit: exact current branch/slot callback PCs");
                check(result.boundary.kind == BoundaryKind::Break
                          && state.read_gpr64(2) == (taken ? 0x11u : 0x22u)
                          && work.completed_instructions == (!taken && !likely ? 3u : 2u),
                      "audit: false event probe does not change completed branch effects");
            }
            // Synthetic delivery redirects to a BREAK, without claiming to
            // emulate CP0/BIOS. One completed word makes an event due. Test
            // both uninterrupted run and same-driver segmented execution.
            for (const bool segmented : {false, true}) {
                GuestWorkCounter work;
                auto state = make_state(window_base);
                state.set_guest_work_counter(&work);
                write_words(state, window_base,
                            {branch, 0x24020011u, 0x24020022u, break_word});
                write_words(state, window_base + 0x100, {break_word});
                state.write_gpr64(8, taken ? 0u : audited_branch.not_taken_operand);
                Driver driver(state, module_from_entries({}));
                ServiceTable services;
                if (segmented) {
                    RunOptions one_attempt;
                    one_attempt.step_limit = 1;
                    const auto pause = driver.run(services, one_attempt);
                    check(pause.boundary.kind == BoundaryKind::StepLimit
                              && work.completed_instructions == 1
                              && driver.pending_transfer() == taken,
                          "audit: budget stop preserves current pending-slot state");
                }
                RegisterContext interrupted;
                bool delivered = false;
                RunOptions options;
                options.start_interrupt = [&](GuestState& running) {
                    if (delivered || work.completed_instructions < 1) {
                        return false;
                    }
                    interrupted = running.save_registers();
                    delivered = true;
                    running.set_pc(window_base + 0x100);
                    return true;
                };
                const auto result = driver.run(services, options);
                const auto expected_pc = taken ? window_base + 12
                    : likely ? window_base + 8 : window_base + 4;
                check(delivered && interrupted.pc == expected_pc
                          && interrupted.gpr[2] == (taken ? 0x11u : 0u)
                          && work.completed_instructions == (taken ? 2u : 1u)
                          && result.boundary.kind == BoundaryKind::Break
                          && result.boundary.pc == window_base + 0x100,
                      "audit: current first due event exposes only ordinary not-taken slot");
            }
        }
    }

    // Same ordinary branch with a syscall slot: the CURRENT taken path
    // refuses acceptance; the not-taken bridge treats it as a
    // plain syscall. Keep this visible without choosing BIOS trap resume.
    for (const bool taken : {false, true}) {
        GuestWorkCounter work;
        auto state = make_state(window_base);
        state.set_guest_work_counter(&work);
        write_words(state, window_base, {0x11000002u, syscall_word, ordinary_word, break_word});
        state.write_gpr64(8, taken ? 0u : 1u);
        state.write_gpr64(3, 0x42u);
        FakeService service;
        ServiceTable services;
        services.add(0x42u, make_fake_service(service));
        Driver driver(state, module_from_entries({}));
        const auto result = driver.run(services, RunOptions{});
        check(service.calls == (taken ? 0u : 1u)
                  && work.accepted_services == service.calls
                  && work.completed_instructions == (taken ? 1u : 3u)
                  && driver.pending_transfer() == taken
                  && result.boundary.kind == (taken ? BoundaryKind::Syscall : BoundaryKind::Break)
                  && result.boundary.pc == window_base + (taken ? 4u : 12u),
              "audit: ordinary not-taken syscall slot acceptance differs from taken");
    }

    // module_from_entries answers for exactly the listed addresses.
    {
        const ModuleEntry entries[] = {
            {0x00100000u, &stop_at_break},
            {0x00100004u, &stop_at_eret},
        };
        const Module module = module_from_entries(entries);
        check(module.has_entry(0x00100000u), "module finds an entry");
        check(!module.has_entry(0x00100008u), "module rejects a missing address");
    }

    // Every stop shape classifies as its own kind.
    for (const StopCase& test : stop_cases) {
        GuestState state = make_state(test.pc);
        state.write_gpr64(31, test.ra);
        const bool mapped = (test.pc & 0x3u) == 0 && state.memory().contains(test.pc, 4);
        if (mapped) {
            state.memory().write_word(test.pc, test.word);
        }
        const Boundary boundary = classify_boundary(state);
        if (boundary.kind != test.expected) {
            std::cerr << test.label << ": expected kind "
                      << static_cast<int>(test.expected) << ", got "
                      << static_cast<int>(boundary.kind) << '\n';
            ++failures;
        }
        if (mapped && boundary.word != test.word) {
            std::cerr << test.label << ": boundary word mismatch\n";
            ++failures;
        }
    }

    // The service table finds handlers by number and replaces duplicates.
    {
        ServiceTable services;
        FakeService first;
        FakeService second;
        services.add(0x42u, make_fake_service(first));
        services.add(0x42u, make_fake_service(second));
        check(services.find(0x42u) != nullptr, "service table finds a handler");
        check(services.find(0x43u) == nullptr, "service table rejects a missing number");
        GuestState state = make_state(window_base);
        (*services.find(0x42u))(state);
        check(first.calls == 0 && second.calls == 1,
              "a duplicate service registration replaces the handler");
    }

    // A module entry executes and stops at an unhandled syscall.
    {
        GuestState state = make_state(window_base);
        write_words(state, 0x00100010u, {syscall_word});
        state.write_gpr64(3, 0x42u);
        const ModuleEntry entries[] = {{window_base, &stop_at_syscall}};
        ServiceTable services;
        Driver driver(state, module_from_entries(entries));
        const RunResult result = driver.run(services, RunOptions{});
        check(result.stats.module_calls == 1 && result.stats.interpreted_steps == 0,
              "the module entry executed and nothing was interpreted");
        check(result.boundary.kind == BoundaryKind::Syscall
                  && result.boundary.pc == 0x00100010u
                  && result.boundary.service == 0x42u,
              "the unhandled syscall is the reported boundary");
    }

    // A registered service is handled inline and the bridge continues from
    // pc + 4 until the service limit stops the next syscall unhandled.
    {
        GuestState state = make_state(window_base);
        write_words(state, 0x00100010u, {syscall_word, ordinary_word, syscall_word});
        state.write_gpr64(3, 0x42u);
        const ModuleEntry entries[] = {{window_base, &stop_at_syscall}};
        ServiceTable services;
        FakeService service;
        service.answer = 0x7u;
        services.add(0x42u, make_fake_service(service));
        RunOptions options;
        options.step_limit = 100;
        options.service_limit = 1;
        Driver driver(state, module_from_entries(entries));
        const RunResult result = driver.run(services, options);
        check(service.calls == 1 && result.stats.services_handled == 1,
              "the service ran once");
        check(state.read_gpr64(2) == 0x7u, "the service effect is in the state");
        check(result.stats.interpreted_steps == 2,
              "the bridge interpreted the instruction between syscalls and the stop");
        check(result.boundary.kind == BoundaryKind::Syscall
                  && result.boundary.pc == 0x00100018u,
              "the second syscall is reported once the service limit is reached");
    }

    // An unknown indirect target is not a stop: the bridge interprets the
    // transfer and whoever it lands on.
    {
        GuestState state = make_state(window_base);
        write_words(state, 0x00100040u, {jalr_word, ordinary_word, syscall_word, unsupported_word});
        state.write_gpr64(25, 0x00100048u);  // t9: the jalr target
        state.write_gpr64(3, 0x42u);
        const ModuleEntry entries[] = {{window_base, &stop_at_jalr}};
        ServiceTable services;
        FakeService service;
        services.add(0x42u, make_fake_service(service));
        RunOptions options;
        options.step_limit = 100;
        Driver driver(state, module_from_entries(entries));
        const RunResult result = driver.run(services, options);
        check(result.stats.module_calls == 1, "the module stopped at the transfer");
        check(result.stats.interpreted_steps == 4,
              "the bridge interpreted the jalr, its delay slot, the syscall and the stop");
        check(result.stats.services_handled == 1, "the reached service was handled");
        check(result.boundary.kind == BoundaryKind::UnsupportedWord
                  && result.boundary.pc == 0x0010004Cu,
              "the run stopped at the unsupported word after the service");
    }

    // A normal return hands control back to the bridge.
    {
        GuestState state = make_state(window_base);
        write_words(state, 0x00100060u, {unsupported_word});
        state.write_gpr64(31, 0x00100060u);
        const ModuleEntry entries[] = {{window_base, &return_through_ra}};
        ServiceTable services;
        Driver driver(state, module_from_entries(entries));
        const RunResult result = driver.run(services, RunOptions{});
        check(result.stats.module_calls == 1 && result.stats.interpreted_steps == 1,
              "the return was bridged by one interpreted step");
        check(result.boundary.kind == BoundaryKind::UnsupportedWord,
              "what the return led to is the reported boundary");
    }

    // A run that cannot leave the window reports Unmapped without stepping.
    {
        GuestState state = make_state(window_base);
        const ModuleEntry entries[] = {{window_base, &stop_at_unmapped}};
        ServiceTable services;
        Driver driver(state, module_from_entries(entries));
        const RunResult result = driver.run(services, RunOptions{});
        check(result.boundary.kind == BoundaryKind::Unmapped
                  && result.boundary.pc == 0x00200000u,
              "an unmapped pc is a boundary");
        check(result.stats.interpreted_steps == 0, "nothing was stepped at an unmapped pc");
    }

    // A trap whose pc equals ra stops as a trap, never as a return.
    {
        GuestState state = make_state(window_base);
        write_words(state, 0x00100020u, {break_word});
        state.write_gpr64(31, 0x00100020u);
        const ModuleEntry entries[] = {{window_base, &stop_at_break}};
        ServiceTable services;
        Driver driver(state, module_from_entries(entries));
        const RunResult result = driver.run(services, RunOptions{});
        check(result.stats.module_calls == 1 && result.stats.interpreted_steps == 0,
              "the trap stopped the module without bridge steps");
        check(result.boundary.kind == BoundaryKind::Break
                  && result.boundary.pc == 0x00100020u,
              "a trap with pc equal to ra is reported as a trap");
    }

    // An applied eret continues at the derived pc: EXL and ERL land on the
    // same ordinary destination, which the bridge then steps through.
    for (const bool error_level : {false, true}) {
        GuestState state = make_state(window_base);
        write_words(state, 0x00100080u, {ordinary_word, unsupported_word});
        state.write_gpr64(31, window_base);  // ra differs from the destination
        state.write_cp0(12, error_level ? 0x00000004u : 0x00000002u);
        state.write_cp0(error_level ? 30 : 14, 0x00100080u);
        const ModuleEntry entries[] = {{window_base, &stop_at_eret}};
        ServiceTable services;
        Driver driver(state, module_from_entries(entries));
        const RunResult result = driver.run(services, RunOptions{});
        check(result.stats.module_calls == 1 && result.stats.interpreted_steps == 2,
              error_level ? "the ERL return reached the bridge and stepped twice"
                          : "the EXL return reached the bridge and stepped twice");
        check(result.boundary.kind == BoundaryKind::UnsupportedWord
                  && result.boundary.pc == 0x00100084u,
              error_level ? "the ERL run stopped past the common destination"
                          : "the EXL run stopped past the common destination");
        check(state.read_cp0(12) == 0, "the applied return cleared its level");
    }

    // A known indirect target dispatches inside the module: the delay slot
    // ran once there, so the bridge only steps what the return lands on.
    {
        GuestState state = make_state(window_base);
        write_words(state, 0x00100090u, {unsupported_word});
        state.write_gpr64(31, 0x00100090u);
        const ModuleEntry entries[] = {{window_base, &stop_at_known_jr}};
        ServiceTable services;
        Driver driver(state, module_from_entries(entries));
        const RunResult result = driver.run(services, RunOptions{});
        check(result.stats.module_calls == 1 && result.stats.interpreted_steps == 1,
              "the internal dispatch ran the slot and the leaf in one call");
        check(state.read_gpr64(2) == 7 && state.read_gpr64(3) == 9,
              "the slot effect ran exactly once before the leaf effect");
        check(result.boundary.kind == BoundaryKind::UnsupportedWord
                  && result.boundary.pc == 0x00100090u,
              "the propagated return landed on ra and the bridge reported it");
    }

    // A trap inside an internal call propagates without being overwritten by
    // the dispatching entry.
    {
        GuestState state = make_state(window_base);
        write_words(state, 0x00100020u, {break_word});
        state.write_gpr64(31, 0x00100090u);
        const ModuleEntry entries[] = {{window_base, &stop_at_known_jr_to_trap}};
        ServiceTable services;
        Driver driver(state, module_from_entries(entries));
        const RunResult result = driver.run(services, RunOptions{});
        check(result.stats.module_calls == 1 && result.stats.interpreted_steps == 0,
              "the inner trap stopped the outer call without bridge steps");
        check(result.boundary.kind == BoundaryKind::Break
                  && result.boundary.pc == 0x00100020u,
              "the inner trap reason reached the driver unchanged");
    }

    // A pending transfer at the entry's own address still reaches the
    // bridge: the entry runs once, the transfer and its slot apply there.
    {
        GuestState state = make_state(window_base);
        write_words(state, window_base, {jalr_word, ordinary_word, syscall_word, unsupported_word});
        state.write_gpr64(25, window_base + 8);  // t9: the jalr target
        state.write_gpr64(3, 0x42u);
        const ModuleEntry entries[] = {{window_base, &stop_at_self_transfer}};
        ServiceTable services;
        FakeService service;
        services.add(0x42u, make_fake_service(service));
        Driver driver(state, module_from_entries(entries));
        const RunResult result = driver.run(services, RunOptions{});
        check(result.stats.module_calls == 1, "the pending entry ran exactly once");
        check(result.stats.interpreted_steps == 4,
              "the bridge applied the transfer, its slot, the service call and the stop");
        check(service.calls == 1, "the reached service ran once");
        check(result.boundary.kind == BoundaryKind::UnsupportedWord
                  && result.boundary.pc == window_base + 12,
              "the run stopped past the reached target");
    }

    // The interpreted-side budget bounds a looping bridge.
    {
        GuestState state = make_state(window_base);
        write_words(state, 0x00100070u, {jump_self_word, 0x00000000u});
        const ModuleEntry entries[] = {{window_base, &return_to_loop}};
        ServiceTable services;
        RunOptions options;
        options.step_limit = 5;
        Driver driver(state, module_from_entries(entries));
        const RunResult result = driver.run(services, options);
        if (result.boundary.kind != BoundaryKind::StepLimit) {
            std::cerr << "work budget: got kind " << static_cast<int>(result.boundary.kind)
                      << " at 0x" << std::hex << result.boundary.pc << std::dec << '\n';
        }
        check(result.boundary.kind == BoundaryKind::StepLimit,
              "the work budget reports StepLimit");
        if (result.stats.module_calls != 1 || result.stats.interpreted_steps != 4) {
            std::cerr << "work budget: module calls " << result.stats.module_calls
                      << ", interpreted steps " << result.stats.interpreted_steps << '\n';
        }
        check(result.stats.module_calls == 1 && result.stats.interpreted_steps == 4,
              "the budget counts the module call and the interpreted instructions");
    }

    if (failures != 0) {
        return 1;
    }
    std::cout << "driver entries, services and bridge behave as specified\n";
    return 0;
}
