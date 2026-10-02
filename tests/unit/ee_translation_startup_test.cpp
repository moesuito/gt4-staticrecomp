// Compares the natively translated startup block against the interpreter on
// the complete final state: all 32 registers in both halves, HI/LO, the FPU
// file, FCR31, the accumulator, the shift cache, the pc, and the written .bss
// window. The translated run goes through the boundary driver (run_once),
// which executes the module entry and classifies the stop; the translator
// emits a halt at the first BIOS syscall, exactly where the interpreter stops.
// The generated header is produced into the build tree from the local CORE;
// nothing game-derived is committed.
#include "translated-startup.hpp"

#include "gt4recomp/ee_driver.hpp"
#include "gt4recomp/ee_interpreter.hpp"
#include "gt4recomp/executable_image.hpp"
#include "verified_core.hpp"

#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
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
constexpr std::uint32_t bss_end = 0x008A215C;
constexpr std::uint32_t first_syscall = 0x001001C8;
constexpr std::uint32_t junk_margin = 0x100;
constexpr std::uint64_t step_limit = 4'000'000;

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

} // namespace

int wmain(int argc, wchar_t* argv[]) {
#if defined(_MSC_VER) && defined(_DEBUG)
    _CrtSetReportMode(_CRT_ERROR, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ERROR, _CRTDBG_FILE_STDERR);
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
#endif
    if (argc != 2) {
        std::cerr << "Usage: ee_translation_startup_tests CORE.GT4\n";
        return 2;
    }
    try {
        const auto core = gt4recomp::tools::read_verified_core(argv[1]);
        const auto image = reconstruct_core(core);

        auto translated_state = make_startup_state(image);
        const ModuleEntry entries[] = {
            { entry, &translated::function_00100008 },
        };
        ServiceTable services;  // none registered: the syscall stays a boundary
        Driver driver(translated_state, module_from_entries(entries));
        const RunResult run_result = driver.run(services, RunOptions{});
        const Boundary& boundary = run_result.boundary;

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

        int failures = 0;
        const auto check = [&](bool passed, const char* label) {
            if (!passed) {
                std::cerr << label << '\n';
                ++failures;
            }
        };
        check(result.outcome == StepOutcome::Exception && result.pc == first_syscall,
              "the interpreted run stopped at the first BIOS syscall");
        check(boundary.kind == BoundaryKind::Syscall && boundary.pc == first_syscall
                  && boundary.service == 0x3Cu,
              "the driver stopped at the first BIOS syscall (service 0x3C)");
        check(run_result.stats.module_calls == 1 && run_result.stats.interpreted_steps == 0,
              "the startup ran as translated code, never through the bridge");
        check(translated_state.pc() == first_syscall,
              "the translated run stopped at the same pc");
        bool registers_match = true;
        for (std::uint8_t index = 0; index < 32; ++index) {
            if (translated_state.read_gpr64(index) != interpreted_state.read_gpr64(index)
                || translated_state.read_gpr_high64(index)
                    != interpreted_state.read_gpr_high64(index)) {
                std::cerr << "register " << static_cast<unsigned>(index) << " differs\n";
                registers_match = false;
            }
        }
        check(registers_match, "all 32 registers match in both halves");
        bool state_matches = translated_state.hi() == interpreted_state.hi()
            && translated_state.lo() == interpreted_state.lo()
            && translated_state.hi1() == interpreted_state.hi1()
            && translated_state.lo1() == interpreted_state.lo1()
            && translated_state.fpu_accumulator() == interpreted_state.fpu_accumulator()
            && translated_state.fpu_control() == interpreted_state.fpu_control()
            && translated_state.shift_amount_cache() == interpreted_state.shift_amount_cache();
        for (std::uint8_t index = 0; index < 32 && state_matches; ++index) {
            state_matches = state_matches
                && translated_state.read_fpr(index) == interpreted_state.read_fpr(index);
        }
        check(state_matches, "HI/LO, FPU file, FCR31, accumulator and shift cache match");
        bool memory_matches = true;
        for (std::uint32_t address = bss_start - junk_margin;
             address < bss_end + junk_margin; address += 8) {
            if (translated_state.memory().read_doubleword(address)
                != interpreted_state.memory().read_doubleword(address)) {
                std::cerr << "memory differs at 0x" << std::hex << address << std::dec << '\n';
                memory_matches = false;
                break;
            }
        }
        check(memory_matches, "the written .bss window matches byte for byte");
        if (failures != 0) {
            return 1;
        }
        std::cout << "translated startup matches the interpreter after " << steps
                  << " instructions; .bss window and full register state identical\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAILURE: " << error.what() << '\n';
        return 1;
    }
}
