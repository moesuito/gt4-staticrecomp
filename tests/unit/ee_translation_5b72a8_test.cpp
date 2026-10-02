// Differential regression test for the translator's "jr ra" emission: DIntr
// (0x005b72a8) returns through jr ra in the middle of its extent, followed by
// the taken-path block. Before the fix the generated code fell through into
// that block and returned a second time, leaving v0 = 0 instead of the
// previous interrupt-enable state — the bug that kept the boot's timer
// initialization from restoring EIE. The test runs the function on two input
// states (interrupts enabled and disabled) and compares all registers and the
// pc with the interpreter. The generated header is produced into the build
// tree from the local CORE; nothing game-derived is committed.
#include "translated-005b72a8.hpp"

#include "gt4recomp/ee_interpreter.hpp"
#include "gt4recomp/executable_image.hpp"
#include "verified_core.hpp"

#include <cstdint>
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

constexpr std::uint32_t entry = 0x005B72A8;
constexpr std::uint32_t window_base = 0x00100000;
constexpr std::uint32_t window_end = 0x008A3000;

struct InputState {
    const char* label;
    std::uint32_t status;
    std::uint32_t ra;
};

const InputState states[] = {
    {"interrupts enabled", 0x70030C11u, 0x00100100u},
    {"interrupts disabled", 0x70020C11u, 0x00100200u},
};

GuestState make_state(const ExecutableImage& image, const InputState& input) {
    GuestMemory memory(window_base, window_end - window_base);
    memory.write_bytes(image.text.guest_address, image.text.bytes);
    memory.write_bytes(image.data.guest_address, image.data.bytes);
    GuestState state(std::move(memory));
    state.write_cp0(12, input.status);
    state.write_gpr64(31, input.ra);
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
        std::cerr << "Usage: ee_translation_5b72a8_tests CORE.GT4\n";
        return 2;
    }
    try {
        const auto core = gt4recomp::tools::read_verified_core(argv[1]);
        const auto image = reconstruct_core(core);

        int failures = 0;
        for (const auto& input : states) {
            auto translated_state = make_state(image, input);
            translated::function_005b72a8(translated_state);

            auto interpreted_state = make_state(image, input);
            Interpreter interpreter(interpreted_state);
            StepResult result;
            for (int step = 0; step < 32; ++step) {
                result = interpreter.step();
                if (result.outcome != StepOutcome::Executed
                    || interpreted_state.pc() == input.ra) {
                    break;
                }
            }

            if (result.outcome != StepOutcome::Executed
                || interpreted_state.pc() != input.ra) {
                std::cerr << input.label << ": the interpreter did not return through ra\n";
                ++failures;
                continue;
            }
            if (translated_state.pc() != input.ra) {
                std::cerr << input.label << ": the module did not return through ra\n";
                ++failures;
                continue;
            }
            // Hand-computed contract: v0 reports the previous EIE state and
            // DIntr clears EIE.
            const std::uint32_t expected_v0 =
                (input.status & 0x00010000u) != 0 ? 1u : 0u;
            if (translated_state.read_gpr32(2) != expected_v0
                || (translated_state.read_cp0(12) & 0x00010000u) != 0) {
                std::cerr << input.label << ": v0 or the cleared EIE is wrong\n";
                ++failures;
            }
            bool registers_match = true;
            for (std::uint8_t index = 0; index < 32; ++index) {
                if (translated_state.read_gpr64(index)
                    != interpreted_state.read_gpr64(index)) {
                    std::cerr << input.label << ": register "
                              << static_cast<unsigned>(index) << " differs\n";
                    registers_match = false;
                }
            }
            if (!registers_match
                || translated_state.read_cp0(12) != interpreted_state.read_cp0(12)) {
                ++failures;
            }
        }
        if (failures != 0) {
            return 1;
        }
        std::cout << "translated DIntr 0x005b72a8 matches the interpreter on "
                  << std::size(states)
                  << " input states, returning through ra without falling through\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAILURE: " << error.what() << '\n';
        return 1;
    }
}
