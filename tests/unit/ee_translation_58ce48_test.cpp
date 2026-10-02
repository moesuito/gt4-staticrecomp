// Compares the gt4translate output for the whole call tree of 0x0058ce48
// (57 functions, 2,588 instructions - the largest verified module so far)
// against the interpreter. The entry is a tiny wrapper: it checks a flag and
// either returns a byte comparison or runs a lazy initializer whose tree
// reaches BIOS services, where both implementations stop at the same
// boundary. The translated header is generated into the build tree from the
// local CORE; nothing game-derived is committed.
#include "translated-0058ce48.hpp"

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

constexpr std::uint32_t entry = 0x0058CE48;
constexpr std::uint32_t flag_address = 0x00657AD0;
constexpr std::uint32_t byte_address = 0x00657AD4;
constexpr std::uint32_t window_end = 0x008A3000;
constexpr std::uint64_t step_limit = 100000;

struct InputState {
    const char* label;
    std::uint8_t flag;
    std::uint8_t byte;
    std::uint32_t ra;
    std::uint32_t sp;
    std::uint64_t junk_r9, junk_r12;
};

const InputState states[] = {
    {"initialized with the marker", 1, 0x54, 0x001003F0u, 0x00100900u, 0, 0},
    {"initialized with another byte", 1, 0x00, 0x00100000u, 0x00100980u, 1, 2},
    {"initialization runs", 0, 0x54, 0x00100400u, 0x00100A00u, 0xFFFFFFFFFFFFFFFFull, 3},
    {"initialization runs again", 0, 0xFF, 0x001001C0u, 0x00100A80u, 5, 6},
};

void apply_state(GuestState& state, const InputState& input) {
    state.memory().write_byte(flag_address, input.flag);
    state.memory().write_byte(byte_address, input.byte);
    state.write_gpr64(29, input.sp);
    state.write_gpr64(31, input.ra);
    state.write_gpr64(9, input.junk_r9);
    state.write_gpr64(12, input.junk_r12);
    state.set_pc(entry);
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
        std::cerr << input.label << ": pc mismatch (translated 0x" << std::hex
                  << translated.pc() << ", interpreted 0x" << interpreted.pc() << std::dec
                  << ")\n";
        ok = false;
    }
    if (translated.hi() != interpreted.hi() || translated.lo() != interpreted.lo()) {
        std::cerr << input.label << ": HI/LO mismatch\n";
        ok = false;
    }
    // The wrapper's data neighborhood and the storage the initializer tree
    // works in; compared 8 bytes at a time.
    for (const auto [begin, end] : {
             std::pair{0x00650000u, 0x00658000u}, {0x006D0000u, 0x006E0000u}}) {
        for (std::uint32_t address = begin; address < end; address += 8) {
            if (translated.memory().read_doubleword(address)
                != interpreted.memory().read_doubleword(address)) {
                std::cerr << input.label << ": memory mismatch at 0x" << std::hex << address
                          << std::dec << '\n';
                ok = false;
                break;
            }
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
        std::cerr << "Usage: ee_translation_58ce48_tests CORE.GT4\n";
        return 2;
    }
    try {
        const auto core = gt4recomp::tools::read_verified_core(argv[1]);
        const auto image = reconstruct_core(core);

        int failures = 0;
        for (const auto& input : states) {
            const auto make_state = [&]() {
                GuestMemory memory(image.text.guest_address,
                                   window_end - image.text.guest_address);
                memory.write_bytes(image.text.guest_address, image.text.bytes);
                memory.write_bytes(image.data.guest_address, image.data.bytes);
                GuestState state(std::move(memory));
                apply_state(state, input);
                return state;
            };
            auto translated_state = make_state();
            translated::function_0058ce48(translated_state);

            auto interpreted_state = make_state();
            Interpreter interpreter(interpreted_state);
            std::uint64_t steps = 0;
            for (; steps < step_limit; ++steps) {
                const auto result = interpreter.step();
                if (result.outcome != StepOutcome::Executed) {
                    break;
                }
                if (interpreted_state.pc() == input.ra) {
                    break;  // the wrapper returned; the module stops here too
                }
            }
            if (steps >= step_limit) {
                std::cerr << input.label << ": step limit exhausted\n";
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
        std::cout << "translated module 0x0058ce48 (57 functions) matches the interpreter on "
                  << std::size(states) << " input states\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAILURE: " << error.what() << '\n';
        return 1;
    }
}
