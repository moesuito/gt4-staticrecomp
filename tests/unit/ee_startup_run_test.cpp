// Runs the pinned game's own startup code: the interpreter starts at the ELF
// entry (0x00100008) and executes until the first BIOS syscall. Everything up
// to that boundary is register/FPU clearing plus the .bss clear loops, so the
// result is checkable without any PS2 service modeling: the .bss window we
// pre-fill with junk must come back zero, and the HI/LO and FPU state that the
// prologue clears must stay clear. Uses the local CORE; nothing game-derived
// is committed.
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
constexpr std::uint32_t bss_end = 0x008A215C;  // the clear loops' exit value (v1)
constexpr std::uint32_t first_syscall = 0x001001C8;
constexpr std::uint32_t junk_margin = 0x100;
constexpr std::uint64_t step_limit = 4'000'000;

} // namespace

int wmain(int argc, wchar_t* argv[]) {
#if defined(_MSC_VER) && defined(_DEBUG)
    _CrtSetReportMode(_CRT_ERROR, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ERROR, _CRTDBG_FILE_STDERR);
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
#endif
    if (argc != 2) {
        std::cerr << "Usage: ee_startup_run_tests CORE.GT4\n";
        return 2;
    }
    try {
        const auto core = gt4recomp::tools::read_verified_core(argv[1]);
        const auto image = reconstruct_core(core);

        // One flat window from the text base up past .bss, keeping the image's
        // own addresses. The neighborhood of .bss is pre-filled with junk so
        // the clearing loops are observed doing their work.
        const std::uint32_t window_base = image.text.guest_address;
        const std::uint32_t window_end = bss_end + 0x1000;
        GuestMemory memory(window_base, window_end - window_base);
        memory.write_bytes(image.text.guest_address, image.text.bytes);
        memory.write_bytes(image.data.guest_address, image.data.bytes);
        std::vector<std::uint8_t> junk(bss_end - bss_start + 2 * junk_margin, 0xAA);
        memory.write_bytes(bss_start - junk_margin, junk);

        GuestState state(std::move(memory));
        state.set_pc(entry);
        Interpreter interpreter(state);

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
        check(result.outcome == StepOutcome::Exception && result.pc == first_syscall
                  && result.operation == Operation::Syscall,
              "startup stopped at the first BIOS syscall");
        check(steps < step_limit, "step limit not exhausted");
        check(state.read_gpr64(2) == bss_end, "clear loops ended at the .bss end address");
        check(state.memory().read_word(bss_start) == 0, ".bss head cleared");
        check(state.memory().read_word(0x00700000) == 0, ".bss middle cleared");
        check(state.memory().read_word(0x008A2150) == 0, ".bss last 16-byte block cleared");
        check(state.memory().read_byte(bss_end - 1) == 0, ".bss tail byte cleared");
        check(state.memory().read_byte(bss_start - 1) == 0xAA, "byte below .bss untouched");
        check(state.memory().read_byte(bss_end) == 0xAA, "byte at the end untouched");
        check(state.hi() == 0 && state.lo() == 0 && state.hi1() == 0 && state.lo1() == 0,
              "HI/LO stayed cleared by the prologue");
        bool fpu_clear = state.fpu_accumulator() == 0 && state.fpu_control() == 0;
        for (std::uint8_t index = 0; index < 32; ++index) {
            fpu_clear = fpu_clear && state.read_fpr(index) == 0;
        }
        check(fpu_clear, "FPU state stayed cleared by the prologue");
        if (failures != 0) {
            return 1;
        }
        std::cout << "startup ran " << steps << " instructions from 0x" << std::hex << entry
                  << " to the first BIOS syscall (0x" << first_syscall << ") with .bss cleared\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAILURE: " << error.what() << '\n';
        return 1;
    }
}
