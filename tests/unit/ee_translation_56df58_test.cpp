// Compares the gt4translate output for function 0x0056DF58 (a 133-instruction
// vector convert/scale loop whose body runs the MMI and VU0 macro operations)
// against the interpreter. The generated module reaches those operations
// through the runtime executor, so this verifies the intrinsic path end to
// end. The translated header is generated into the build tree from the local
// CORE; nothing game-derived is committed.
#include "translated-0056df58.hpp"

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

constexpr std::uint32_t function_entry = 0x0056DF58;
constexpr std::uint32_t scratch_start = 0x00100200;
constexpr std::uint32_t scratch_end = 0x00101000;
constexpr std::uint32_t stack_pointer = 0x00100800;
constexpr std::uint32_t return_address = 0x00100000;

struct InputState {
    const char* label;
    std::uint32_t outer_count;  // t0: the outer loop iterations
    std::uint32_t inner_count;  // t1: the vector element count
    std::uint32_t seed;
};

const InputState states[] = {
    {"single pass", 1, 8, 0x12345678u},
    {"two outer passes", 2, 12, 0x0BADF00Du},
    {"zero count", 1, 0, 0x00000001u},
};

void fill_pattern(GuestState& state, std::uint32_t address, std::uint32_t words,
                  std::uint32_t seed) {
    std::uint32_t value = seed;
    for (std::uint32_t index = 0; index < words; ++index) {
        state.memory().write_word(address + index * 4, value);
        value = value * 1664525u + 1013904223u;  // a deterministic walk
    }
}

void apply_state(GuestState& state, const InputState& input) {
    fill_pattern(state, scratch_start, (scratch_end - scratch_start) / 4, input.seed);
    // Parameters: a0 selects an input word, t2 is the array the tail loop also
    // writes, a1/a2 feed the unaligned vector reads, a3 receives the final
    // store; t0 and t1 bound the loops.
    state.write_gpr64(4, scratch_start);
    state.write_gpr64(5, scratch_start + 0x80);
    state.write_gpr64(6, scratch_start + 0xC0);
    state.write_gpr64(7, scratch_start + 0x100);
    state.write_gpr64(8, input.outer_count);
    state.write_gpr64(9, input.inner_count);
    state.write_gpr64(10, scratch_start + 0x140);
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
    // The whole scratch window: the inputs, the tail loop's array, the final
    // store target and the saved registers.
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
        std::cerr << "Usage: ee_translation_56df58_tests CORE.GT4\n";
        return 2;
    }
    try {
        const auto core = gt4recomp::tools::read_verified_core(argv[1]);
        const auto image = reconstruct_core(core);

        int failures = 0;
        for (const auto& input : states) {
            const auto make_state = [&]() {
                // The scratch window sits inside the text mapping but far from
                // the function's code, so the module and the interpreter can
                // both read and write it.
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
            translated::function_0056df58(translated_state);

            auto interpreted_state = make_state();
            Interpreter interpreter(interpreted_state);
            bool returned = false;
            for (int step = 0; step < 100000; ++step) {
                const auto result = interpreter.step();
                if (result.outcome != StepOutcome::Executed) {
                    std::cerr << input.label << ": interpreter stopped at 0x" << std::hex
                              << result.pc << std::dec << '\n';
                    break;
                }
                if (interpreted_state.pc() == return_address) {
                    returned = true;
                    break;
                }
            }
            if (!returned) {
                std::cerr << input.label << ": interpreter did not return to ra\n";
                ++failures;
                continue;
            }
            if (translated_state.pc() != return_address) {
                std::cerr << input.label << ": translated function did not return to ra\n";
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
        std::cout << "translated 0x0056df58 matches the interpreter on "
                  << std::size(states) << " input states\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAILURE: " << error.what() << '\n';
        return 1;
    }
}
