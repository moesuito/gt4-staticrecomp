// Compares the gt4translate output for function 0x00549378 (an integer
// sequence built on SRA) against the interpreter on several input states. The
// translated header is generated into the build tree from the local CORE;
// nothing game-derived is committed.
#include "translated-00549378.hpp"

#include "gt4recomp/ee_interpreter.hpp"
#include "gt4recomp/executable_image.hpp"
#include "verified_core.hpp"

#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>

#if defined(_MSC_VER)
#include <crtdbg.h>
#include <cstdlib>
#endif

using namespace gt4recomp;
using namespace gt4recomp::ee;

namespace {

constexpr std::uint32_t function_entry = 0x00549378;

struct InputState {
    const char* label;
    std::uint32_t a0;
    std::uint32_t ra;
    std::uint64_t junk_r9, junk_r12;
};

const InputState states[] = {
    {"zero", 0x00000000u, 0x001003F0u, 0, 0},
    {"low nibble", 0x0000000Fu, 0x00100000u, 1, 2},
    {"all ones", 0xFFFFFFFFu, 0x00100400u, 0xFFFFFFFFFFFFFFFFull, 3},
    {"sign bit", 0x80000000u, 0x00100500u, 4, 0xDEADBEEFCAFEBABEull},
    {"pattern", 0x12345678u, 0x001001C0u, 5, 6},
    {"mixed", 0xDEADBEEFu, 0x00100600u, 0x0123456789ABCDEFull, 7},
};

void apply_state(GuestState& state, const InputState& input) {
    state.write_gpr64(4, input.a0);
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
    for (std::size_t offset = 0; offset < translated.memory().size(); offset += 4) {
        const auto address = static_cast<std::uint32_t>(
            translated.memory().base() + offset);
        if (translated.memory().read_word(address) != interpreted.memory().read_word(address)) {
            std::cerr << input.label << ": memory mismatch at 0x" << std::hex << address
                      << std::dec << '\n';
            ok = false;
            break;
        }
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
        std::cerr << "Usage: ee_translation_549378_tests CORE.GT4\n";
        return 2;
    }
    try {
        const auto core = gt4recomp::tools::read_verified_core(argv[1]);
        const auto image = reconstruct_core(core);
        const auto& text = image.text;

        int failures = 0;
        for (const auto& input : states) {
            GuestState translated_state(GuestMemory(text.guest_address, text.bytes.size()));
            translated_state.memory().write_bytes(text.guest_address, text.bytes);
            apply_state(translated_state, input);
            translated::function_00549378(translated_state);

            GuestState interpreted_state(GuestMemory(text.guest_address, text.bytes.size()));
            interpreted_state.memory().write_bytes(text.guest_address, text.bytes);
            apply_state(interpreted_state, input);
            Interpreter interpreter(interpreted_state);
            bool returned = false;
            for (int step = 0; step < 16; ++step) {
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
        std::cout << "translated 0x00549378 matches the interpreter on "
                  << std::size(states) << " input states\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAILURE: " << error.what() << '\n';
        return 1;
    }
}
