// Compares the gt4translate output for the tail thunk at 0x005b27f8 against
// the interpreter. The thunk loads an argument and jumps (through a delay
// slot) down to the BIOS syscall trampoline at 0x005adcc0; the translated
// module stops at the syscall exactly where the interpreter stops, with the
// pc at the syscall word and every other piece of state identical. The
// generated header is produced into the build tree from the local CORE;
// nothing game-derived is committed.
#include "translated-005b27f8.hpp"

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

constexpr std::uint32_t entry = 0x005B27F8;
constexpr std::uint32_t service_argument_address = 0x00658344;  // loaded by the thunk
constexpr std::uint32_t syscall_address = 0x005ADCC4;
constexpr std::uint32_t window_end = 0x008A3000;

struct InputState {
    const char* label;
    std::uint32_t argument;  // the word the thunk loads into a0
    std::uint32_t input_a0;
    std::uint32_t ra;
    std::uint64_t junk_r9, junk_r12;
};

const InputState states[] = {
    {"zero argument", 0, 0, 0x001003F0u, 0, 0},
    {"pattern", 0x11223344u, 0x77777777u, 0x00100000u, 1, 2},
    {"all ones", 0xFFFFFFFFu, 0, 0x00100400u, 0xFFFFFFFFFFFFFFFFull, 3},
    {"high bit", 0x80000000u, 5, 0x00100500u, 4, 0xDEADBEEFCAFEBABEull},
    {"mixed", 0xDEADBEEFu, 0x12345678u, 0x001001C0u, 5, 6},
};

GuestState make_state(const ExecutableImage& image, const InputState& input) {
    GuestMemory memory(image.text.guest_address, window_end - image.text.guest_address);
    memory.write_bytes(image.text.guest_address, image.text.bytes);
    memory.write_bytes(image.data.guest_address, image.data.bytes);
    GuestState state(std::move(memory));
    state.memory().write_word(service_argument_address, input.argument);
    state.write_gpr64(4, input.input_a0);
    state.write_gpr64(31, input.ra);
    state.write_gpr64(9, input.junk_r9);
    state.write_gpr64(12, input.junk_r12);
    state.set_pc(entry);
    return state;
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
    if (translated.memory().read_word(service_argument_address)
        != interpreted.memory().read_word(service_argument_address)) {
        std::cerr << input.label << ": service argument word mismatch\n";
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
        std::cerr << "Usage: ee_translation_thunk_tests CORE.GT4\n";
        return 2;
    }
    try {
        const auto core = gt4recomp::tools::read_verified_core(argv[1]);
        const auto image = reconstruct_core(core);

        int failures = 0;
        for (const auto& input : states) {
            auto translated_state = make_state(image, input);
            translated::function_005b27f8(translated_state);

            auto interpreted_state = make_state(image, input);
            Interpreter interpreter(interpreted_state);
            StepResult result;
            for (int step = 0; step < 16; ++step) {
                result = interpreter.step();
                if (result.outcome != StepOutcome::Executed) {
                    break;
                }
            }
            if (result.outcome != StepOutcome::Exception || result.pc != syscall_address
                || result.operation != Operation::Syscall) {
                std::cerr << input.label << ": interpreter did not stop at the service\n";
                ++failures;
                continue;
            }
            if (translated_state.pc() != syscall_address) {
                std::cerr << input.label << ": translated module stopped elsewhere (0x"
                          << std::hex << translated_state.pc() << std::dec << ")\n";
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
        std::cout << "translated thunk 0x005b27f8 matches the interpreter on "
                  << std::size(states) << " input states, both stopping at service 0x42\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAILURE: " << error.what() << '\n';
        return 1;
    }
}
