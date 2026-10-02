// Compares the gt4translate output for function 0x00101C28 (a two-instruction
// trampoline whose jalr ends the translated flow) against the interpreter.
// The module stops at the jalr with the pc at the transfer, before executing
// it; the interpreter is stepped up to that same address, so the comparison
// covers the identical prefix. The translated header is generated into the
// build tree from the local CORE; nothing game-derived is committed.
#include "translated-00101c28.hpp"

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

constexpr std::uint32_t function_entry = 0x00101C28;
constexpr std::uint32_t jalr_address = 0x00101C30;
constexpr std::uint32_t scratch_start = 0x00100200;
constexpr std::uint32_t scratch_end = 0x00101000;
constexpr std::uint32_t stack_pointer = 0x00100800;
constexpr std::uint32_t return_address = 0x00100000;

struct InputState {
    const char* label;
    std::uint64_t a0;  // the indirect target register: the module stops before
                       // it matters, so any value must give the same prefix
};

const InputState states[] = {
    {"zero target", 0},
    {"code target", 0x00101C28ull},
    {"scratch target", 0x00100400ull},
};

void fill_pattern(GuestState& state) {
    // Every word holds a mapped scratch address, so nothing the stubs touch
    // can leave the window.
    for (std::uint32_t address = scratch_start; address < scratch_end; address += 4) {
        state.memory().write_word(address, scratch_start + 0x200);
    }
}

void apply_state(GuestState& state, const InputState& input) {
    fill_pattern(state);
    state.write_gpr64(4, input.a0);
    state.write_gpr64(29, stack_pointer);
    state.write_gpr64(31, return_address);
    state.set_pc(function_entry);
}

bool compare_states(const GuestState& translated, const GuestState& interpreted,
                    const char* label) {
    bool ok = true;
    for (int index = 0; index < 32; ++index) {
        const auto reg = static_cast<std::uint8_t>(index);
        if (translated.read_gpr64(reg) != interpreted.read_gpr64(reg)) {
            std::cerr << label << ": r" << index << " mismatch\n";
            ok = false;
            break;
        }
    }
    if (translated.pc() != interpreted.pc()) {
        std::cerr << label << ": pc mismatch\n";
        ok = false;
    }
    if (translated.hi() != interpreted.hi() || translated.lo() != interpreted.lo()
        || translated.hi1() != interpreted.hi1() || translated.lo1() != interpreted.lo1()) {
        std::cerr << label << ": HI/LO mismatch\n";
        ok = false;
    }
    for (std::uint32_t address = scratch_start; address < scratch_end; address += 8) {
        if (translated.memory().read_doubleword(address)
            != interpreted.memory().read_doubleword(address)) {
            std::cerr << label << ": memory mismatch at 0x" << std::hex << address
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
        std::cerr << "Usage: ee_translation_101c28_tests CORE.GT4\n";
        return 2;
    }
    try {
        const auto core = gt4recomp::tools::read_verified_core(argv[1]);
        const auto image = reconstruct_core(core);

        int failures = 0;
        for (const auto& input : states) {
            const auto make_state = [&]() {
                constexpr std::uint32_t window_end = 0x008A3000;
                GuestMemory memory(image.text.guest_address,
                                   window_end - image.text.guest_address);
                memory.write_bytes(image.text.guest_address, image.text.bytes);
                memory.write_bytes(image.data.guest_address, image.data.bytes);
                GuestState state(std::move(memory));
                apply_state(state, input);
                return state;
            };
            auto translated_state = make_state();
            translated::function_00101c28(translated_state);
            if (translated_state.pc() != jalr_address) {
                std::cerr << input.label << ": translated function stopped at 0x" << std::hex
                          << translated_state.pc() << std::dec << '\n';
                ++failures;
                continue;
            }

            auto interpreted_state = make_state();
            Interpreter interpreter(interpreted_state);
            bool stopped = false;
            for (int step = 0; step < 64; ++step) {
                if (interpreted_state.pc() == jalr_address) {
                    stopped = true;
                    break;
                }
                const auto result = interpreter.step();
                if (result.outcome != StepOutcome::Executed) {
                    std::cerr << input.label << ": interpreter stopped at 0x" << std::hex
                              << result.pc << std::dec << '\n';
                    break;
                }
            }
            if (!stopped) {
                std::cerr << input.label << ": interpreter did not reach the jalr\n";
                ++failures;
                continue;
            }
            if (!compare_states(translated_state, interpreted_state, input.label)) {
                ++failures;
            }
        }
        if (failures != 0) {
            return 1;
        }
        std::cout << "translated 0x00101c28 matches the interpreter on "
                  << std::size(states) << " input states (indirect-call boundary)\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAILURE: " << error.what() << '\n';
        return 1;
    }
}
