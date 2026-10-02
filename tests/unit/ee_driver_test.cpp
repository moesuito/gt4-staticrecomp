// Unit tests for the boundary driver and its service layer, with no game
// data: fake modules prove that the driver executes entries, resolves
// syscalls through the service table, bridges through the interpreter when
// the module cannot pass a boundary, and reports what nothing can resolve.
#include "gt4recomp/ee_driver.hpp"

#include <cstdint>
#include <initializer_list>
#include <iostream>
#include <utility>

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
// leaves the pc where the translator would.
void stop_at_syscall(GuestState& state) { state.set_pc(0x00100010u); }
void stop_at_jalr(GuestState& state) { state.set_pc(0x00100040u); }
void stop_at_break(GuestState& state) { state.set_pc(0x00100020u); }
void stop_at_eret(GuestState& state) { state.set_pc(0x00100030u); }
void stop_at_unsupported(GuestState& state) { state.set_pc(0x00100050u); }
void stop_at_ordinary(GuestState& state) { state.set_pc(0x00100060u); }
void stop_at_unmapped(GuestState& state) { state.set_pc(0x00200000u); }
void return_to_loop(GuestState& state) {
    // A real module stop is a boundary word or a return; the loop then runs
    // in the bridge until the work budget stops it.
    state.write_gpr64(31, 0x00100070u);
    state.set_pc(0x00100070u);
}
void return_through_ra(GuestState& state) {
    state.set_pc(static_cast<std::uint32_t>(state.read_gpr64(31)));
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
    {"returned", 0x00100070u, ordinary_word, 0x00100070u, BoundaryKind::Returned},
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
        if (mapped && boundary.word != test.word
            && test.expected != BoundaryKind::Returned) {
            // A return is classified before the word is read; its boundary
            // carries no word.
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
