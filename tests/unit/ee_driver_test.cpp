// Unit tests for the boundary driver, with no game data: a fake module proves
// that run_once executes the entry at the current pc exactly once, and
// classify_boundary names each stop shape the translator emits (syscall,
// break, eret, an unknown indirect target, an unsupported word, a jr-ra
// return, a trapping stop at an ordinary word, and an unmapped pc).
#include "gt4recomp/ee_driver.hpp"

#include <cstdint>
#include <iostream>
#include <utility>

using namespace gt4recomp::ee;

namespace {

constexpr std::uint32_t window_base = 0x00100000;
constexpr std::size_t window_size = 0x1000;

// The hand-assembled boundary words the tests write into the window.
constexpr std::uint32_t syscall_word = 0x0000000Cu;   // syscall
constexpr std::uint32_t break_word = 0x0000000Du;     // break
constexpr std::uint32_t eret_word = 0x42000018u;      // eret
constexpr std::uint32_t jalr_word = 0x0320F809u;      // jalr ra, t9
constexpr std::uint32_t unsupported_word = 0x00200000u;  // sll with rs != 0
constexpr std::uint32_t ordinary_word = 0x00000000u;  // sll zero, zero, 0

GuestState make_state(std::uint32_t pc) {
    GuestMemory memory(window_base, window_size);
    GuestState state(std::move(memory));
    state.set_pc(pc);
    return state;
}

// Fake module functions: each one behaves like a generated function and
// leaves the pc where the translator would.
void write_register_and_stop_at_syscall(GuestState& state) {
    state.write_gpr64(2, 0x1234u);  // proves the entry executed
    state.set_pc(0x00100010u);
}

void stop_at_break(GuestState& state) { state.set_pc(0x00100020u); }
void stop_at_eret(GuestState& state) { state.set_pc(0x00100030u); }
void stop_at_jalr(GuestState& state) { state.set_pc(0x00100040u); }
void stop_at_unsupported(GuestState& state) { state.set_pc(0x00100050u); }
void stop_at_ordinary(GuestState& state) { state.set_pc(0x00100060u); }
void stop_at_return_target(GuestState& state) { state.set_pc(0x00100070u); }
void stop_at_unmapped(GuestState& state) { state.set_pc(0x00200000u); }
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

} // namespace

int main() {
    int failures = 0;
    const auto check = [&](bool passed, const char* label) {
        if (!passed) { std::cerr << label << '\n'; ++failures; }
    };

    // ModuleCatalog only reports the addresses it holds.
    {
        const ModuleEntry entries[] = {
            {0x00100000u, &stop_at_break},
            {0x00100004u, &stop_at_eret},
        };
        const ModuleCatalog catalog{entries};
        check(catalog.find(0x00100000u) != nullptr, "catalog finds an entry");
        check(catalog.find(0x00100008u) == nullptr, "catalog rejects a missing address");
    }

    // run_once executes the entry at the current pc and classifies the stop.
    {
        GuestState state = make_state(window_base);
        state.memory().write_word(0x00100010u, syscall_word);
        state.write_gpr64(3, 0x42u);
        const ModuleEntry entries[] = {{window_base, &write_register_and_stop_at_syscall}};
        Driver driver(state, ModuleCatalog{entries});
        const Boundary boundary = driver.run_once();
        check(state.read_gpr64(2) == 0x1234u, "run_once executed the module entry");
        check(boundary.kind == BoundaryKind::Syscall, "run_once classified the syscall");
        check(boundary.pc == 0x00100010u, "run_once reported the stop pc");
        check(boundary.word == syscall_word, "run_once reported the boundary word");
        check(boundary.service == 0x42u, "run_once read the service number from v1");
    }

    // An address without an entry is a boundary: nothing executes.
    {
        GuestState state = make_state(window_base);
        const ModuleEntry entries[] = {{window_base + 4, &stop_at_break}};
        Driver driver(state, ModuleCatalog{entries});
        const Boundary boundary = driver.run_once();
        check(boundary.kind == BoundaryKind::NoEntry, "no entry is a boundary");
        check(boundary.pc == window_base && state.pc() == window_base,
              "no entry leaves the pc untouched");
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

    // The remaining fake entries prove the run_once classification paths the
    // fake module functions above can reach.
    {
        const struct {
            void (*execute)(GuestState&);
            BoundaryKind expected;
        } runs[] = {
            {&stop_at_break, BoundaryKind::Break},
            {&stop_at_eret, BoundaryKind::ExceptionReturn},
            {&stop_at_jalr, BoundaryKind::IndirectTransfer},
            {&stop_at_unsupported, BoundaryKind::UnsupportedWord},
            {&stop_at_ordinary, BoundaryKind::InstructionStop},
            {&stop_at_unmapped, BoundaryKind::Unmapped},
            {&return_through_ra, BoundaryKind::Returned},
        };
        for (const auto& run : runs) {
            GuestState state = make_state(window_base);
            state.memory().write_word(0x00100020u, break_word);
            state.memory().write_word(0x00100030u, eret_word);
            state.memory().write_word(0x00100040u, jalr_word);
            state.memory().write_word(0x00100050u, unsupported_word);
            state.memory().write_word(0x00100060u, ordinary_word);
            state.memory().write_word(0x00100070u, ordinary_word);
            state.write_gpr64(31, 0x00100070u);
            const ModuleEntry entries[] = {{window_base, run.execute}};
            Driver driver(state, ModuleCatalog{entries});
            const Boundary boundary = driver.run_once();
            if (boundary.kind != run.expected) {
                std::cerr << "run_once path: expected kind "
                          << static_cast<int>(run.expected) << ", got "
                          << static_cast<int>(boundary.kind) << '\n';
                ++failures;
            }
        }
    }

    if (failures != 0) {
        return 1;
    }
    std::cout << "driver boundary classification matches every emitted stop shape\n";
    return 0;
}
