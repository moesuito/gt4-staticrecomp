// Compares the gt4translate output for the cache-flush loop at 0x005b0f78
// against the interpreter. The function issues cache-line hints over an
// address range; the hints are no-ops in both implementations, so the visible
// state is registers and the continuation - and this function contains the
// critical edge where the loop back-edge targets the beq's own delay slot
// (0x005b0fcc), the pattern the translator now represents with a standalone
// copy. The translated header is generated into the build tree from the local
// CORE; nothing game-derived is committed.
#include "translated-005b0f78.hpp"

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

constexpr std::uint32_t function_entry = 0x005B0F78;
constexpr std::uint32_t buffer_base = 0x00101000;

struct InputState {
    const char* label;
    std::uint32_t start;
    std::uint32_t size;
    std::uint32_t ra;
    std::uint64_t junk_r9, junk_r12;
};

const InputState states[] = {
    {"empty range", buffer_base, 0, 0x001003F0u, 0, 0},
    {"one byte", buffer_base, 1, 0x00100000u, 1, 2},
    {"misaligned short", buffer_base + 3, 63, 0x00100400u, 0xFFFFFFFFFFFFFFFFull, 3},
    {"aligned line", buffer_base, 64, 0x00100500u, 4, 0xDEADBEEFCAFEBABEull},
    {"line plus one", buffer_base, 65, 0x001001C0u, 5, 6},
    {"eight lines", buffer_base + 17, 0x200, 0x00100600u, 0x0123456789ABCDEFull, 7},
    {"sixteen lines", buffer_base, 0x1000, 0x00100700u, 8, 9},
};

void apply_state(GuestState& state, const InputState& input) {
    state.write_gpr64(4, input.start);
    state.write_gpr64(5, input.size);
    state.write_gpr64(31, input.ra);
    state.write_gpr64(9, input.junk_r9);
    state.write_gpr64(12, input.junk_r12);
    state.set_pc(function_entry);
}

bool compare_states(const GuestState& translated, const GuestState& interpreted,
                    const InputState& input) {
    bool ok = true;
    for (int index = 0; index < 32; ++index) {
        const auto reg = static_cast<std::uint8_t>(index);
        if (translated.read_gpr64(reg) != interpreted.read_gpr64(reg)) {
            std::cerr << input.label << ": r" << index << " mismatch\n";
            ok = false;
        }
    }
    if (translated.pc() != interpreted.pc()) {
        std::cerr << input.label << ": pc mismatch\n";
        ok = false;
    }
    return ok;
}

} // namespace

int wmain(int argc, wchar_t* argv[]) {
#if defined(_MSC_VER) && defined(_DEBUG)
    _CrtSetReportMode(_CRT_ERROR, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ERROR, _CRTDBG_FILE_STDERR);
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
#endif
    if (argc != 2) {
        std::cerr << "Usage: ee_translation_cacheflush_tests CORE.GT4\n";
        return 2;
    }
    try {
        const auto core = gt4recomp::tools::read_verified_core(argv[1]);
        const auto image = reconstruct_core(core);

        int failures = 0;
        for (const auto& input : states) {
            const auto make_state = [&]() {
                GuestState state(GuestMemory(image.text.guest_address, image.text.bytes.size()));
                state.memory().write_bytes(image.text.guest_address, image.text.bytes);
                apply_state(state, input);
                return state;
            };
            auto translated_state = make_state();
            translated::function_005b0f78(translated_state);

            auto interpreted_state = make_state();
            Interpreter interpreter(interpreted_state);
            bool returned = false;
            for (int step = 0; step < 8192; ++step) {
                const auto result = interpreter.step();
                if (result.outcome != StepOutcome::Executed) {
                    std::cerr << input.label << ": interpreter stopped at 0x" << std::hex
                              << result.pc << std::dec << '\n';
                    break;
                }
                if (interpreted_state.pc() == input.ra) {
                    returned = true;
                    break;
                }
            }
            if (!returned) {
                std::cerr << input.label << ": interpreter did not return to ra\n";
                ++failures;
                continue;
            }
            if (translated_state.pc() != input.ra) {
                std::cerr << input.label << ": translated function did not return to ra\n";
                ++failures;
                continue;
            }
            if (!compare_states(translated_state, interpreted_state, input)) {
                ++failures;
            }
        }
        if (failures != 0) {
            return 1;
        }
        std::cout << "translated 0x005b0f78 matches the interpreter on "
                  << std::size(states) << " input states (critical-edge loop included)\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAILURE: " << error.what() << '\n';
        return 1;
    }
}
