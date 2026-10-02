// gt4run runs a translated module as a program. The startup module is
// generated from the pinned CORE at build time and compiled into this tool;
// the driver executes it from the ELF entry, classifies the boundary where it
// stops, and reports it. With --compare-interpreter the same run is repeated
// by the interpreter and the full final state must match; that is the
// automated check (the gt4run_startup CTest). Nothing game-derived is
// committed: the module lives in the ignored build tree.
#include "translated-startup.hpp"

#include "gt4recomp/ee_driver.hpp"
#include "gt4recomp/ee_interpreter.hpp"
#include "gt4recomp/executable_image.hpp"
#include "verified_core.hpp"

#include <cstdint>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#if defined(_MSC_VER)
#include <crtdbg.h>
#include <cstdlib>
#endif

using namespace gt4recomp;
using namespace gt4recomp::ee;

namespace {

constexpr std::uint32_t entry = 0x00100008;
constexpr std::uint32_t bss_start = 0x006D5E00;
constexpr std::uint32_t bss_end = 0x008A215C;  // the clear loops' exit value (v1)
constexpr std::uint32_t junk_margin = 0x100;
constexpr std::uint64_t step_limit = 4'000'000;

// The same flat-window fixture the startup tests use: one region from the
// text base past .bss, the image's own addresses, and a junk pre-fill around
// .bss so the clearing loops are observed doing their work.
GuestState make_startup_state(const ExecutableImage& image) {
    const std::uint32_t window_base = image.text.guest_address;
    const std::uint32_t window_end = bss_end + 0x1000;
    GuestMemory memory(window_base, window_end - window_base);
    memory.write_bytes(image.text.guest_address, image.text.bytes);
    memory.write_bytes(image.data.guest_address, image.data.bytes);
    std::vector<std::uint8_t> junk(bss_end - bss_start + 2 * junk_margin, 0xAA);
    memory.write_bytes(bss_start - junk_margin, junk);
    GuestState state(std::move(memory));
    state.set_pc(entry);
    return state;
}

const char* boundary_name(BoundaryKind kind) {
    switch (kind) {
    case BoundaryKind::NoEntry: return "no-entry";
    case BoundaryKind::Syscall: return "syscall";
    case BoundaryKind::Break: return "break";
    case BoundaryKind::ExceptionReturn: return "exception-return";
    case BoundaryKind::UnsupportedWord: return "unsupported-word";
    case BoundaryKind::IndirectTransfer: return "indirect-transfer";
    case BoundaryKind::Returned: return "returned";
    case BoundaryKind::InstructionStop: return "instruction-stop";
    case BoundaryKind::Unmapped: return "unmapped";
    }
    return "unknown";
}

// The interpreter's stop outcome in the same vocabulary as the driver's
// boundary, so the comparison reports one agreed stop.
const char* interpreted_stop_name(StepOutcome outcome) {
    switch (outcome) {
    case StepOutcome::Executed: return "still-executing";
    case StepOutcome::Unsupported: return "unsupported-word";
    case StepOutcome::Exception: return "exception";
    case StepOutcome::IllegalDelaySlot: return "illegal-delay-slot";
    }
    return "unknown";
}

// FNV-1a over the whole guest window: a compact digest of every byte the run
// could have touched, text, data and .bss alike.
std::uint64_t memory_digest(const GuestState& state) {
    const std::uint64_t offset_basis = 14695981039346656037ull;
    const std::uint64_t prime = 1099511628211ull;
    std::uint64_t hash = offset_basis;
    const std::uint32_t window_base = state.memory().base();
    const std::uint32_t window_end = window_base
        + static_cast<std::uint32_t>(state.memory().size());
    for (std::uint32_t address = window_base; address < window_end; ++address) {
        hash ^= state.memory().read_byte(address);
        hash *= prime;
    }
    return hash;
}

// The full state the startup run can change, field by field, plus the memory
// digest. VU0 is not compared: the startup path never reaches it.
bool states_match(const GuestState& left, const GuestState& right) {
    for (std::uint8_t index = 0; index < 32; ++index) {
        if (left.read_gpr64(index) != right.read_gpr64(index)
            || left.read_gpr_high64(index) != right.read_gpr_high64(index)
            || left.read_fpr(index) != right.read_fpr(index)
            || left.read_cp0(index) != right.read_cp0(index)) {
            std::cerr << "state differs at register " << static_cast<unsigned>(index) << '\n';
            return false;
        }
    }
    if (left.hi() != right.hi() || left.lo() != right.lo()
        || left.hi1() != right.hi1() || left.lo1() != right.lo1()
        || left.fpu_accumulator() != right.fpu_accumulator()
        || left.fpu_control() != right.fpu_control()
        || left.shift_amount_cache() != right.shift_amount_cache()
        || left.pc() != right.pc()) {
        std::cerr << "state differs in HI/LO, the FPU, the shift cache or the pc\n";
        return false;
    }
    if (memory_digest(left) != memory_digest(right)) {
        std::cerr << "state differs in the guest memory window\n";
        return false;
    }
    return true;
}

void usage() {
    std::cerr << "Usage: gt4run CORE.GT4 [--compare-interpreter]\n";
}

} // namespace

int wmain(int argc, wchar_t* argv[]) {
#if defined(_MSC_VER) && defined(_DEBUG)
    _CrtSetReportMode(_CRT_ERROR, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ERROR, _CRTDBG_FILE_STDERR);
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
#endif
    bool compare_interpreter = false;
    std::filesystem::path core_path;
    for (int index = 1; index < argc; ++index) {
        const std::wstring argument = argv[index];
        if (argument == L"--compare-interpreter") {
            compare_interpreter = true;
        } else if (core_path.empty()) {
            core_path = argument;
        } else {
            usage();
            return 2;
        }
    }
    if (core_path.empty()) {
        usage();
        return 2;
    }

    try {
        const auto core = gt4recomp::tools::read_verified_core(core_path);
        const auto image = reconstruct_core(core);

        auto driver_state = make_startup_state(image);
        const ModuleEntry entries[] = {
            { entry, &translated::function_00100008 },
        };
        Driver driver(driver_state, ModuleCatalog{entries});
        const Boundary boundary = driver.run_once();

        std::cout << "boundary: " << boundary_name(boundary.kind) << " 0x"
                  << std::hex << std::setfill('0') << std::setw(8) << boundary.pc
                  << std::dec << std::setfill(' ');
        if (boundary.kind == BoundaryKind::Syscall) {
            std::cout << " service 0x" << std::hex << boundary.service << std::dec;
        }
        std::cout << '\n';

        if (boundary.kind != BoundaryKind::Syscall) {
            std::cerr << "gt4run expected the startup to stop at its first syscall\n";
            return 1;
        }

        if (compare_interpreter) {
            auto interpreted_state = make_startup_state(image);
            Interpreter interpreter(interpreted_state);
            StepResult result;
            std::uint64_t steps = 0;
            for (; steps < step_limit; ++steps) {
                result = interpreter.step();
                if (result.outcome != StepOutcome::Executed) {
                    break;
                }
            }
            if (result.outcome != StepOutcome::Exception || result.pc != boundary.pc) {
                std::cerr << "interpreter stopped differently: "
                          << interpreted_stop_name(result.outcome) << " at 0x"
                          << std::hex << result.pc << std::dec << '\n';
                return 1;
            }
            if (!states_match(driver_state, interpreted_state)) {
                std::cerr << "the driver and the interpreter states differ\n";
                return 1;
            }
            std::cout << "interpreter: " << steps << " instructions, state identical ("
                      << "registers, HI/LO, FPU, shift cache, pc, memory digest)\n";
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAILURE: " << error.what() << '\n';
        return 1;
    }
}
