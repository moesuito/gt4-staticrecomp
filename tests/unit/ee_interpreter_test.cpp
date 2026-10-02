#include "gt4recomp/ee_interpreter.hpp"

#include <cstdint>
#include <initializer_list>
#include <iostream>
#include <stdexcept>
#include <utility>
#include <vector>

#if defined(_MSC_VER)
#include <crtdbg.h>
#include <cstdlib>
#endif

using namespace gt4recomp;
using namespace gt4recomp::ee;

namespace {

constexpr std::uint32_t base = 0x00100000;
constexpr std::size_t region_size = 0x1000;

void load_program(GuestMemory& memory, std::uint32_t address,
                  std::initializer_list<std::uint32_t> words) {
    std::vector<std::uint8_t> bytes;
    bytes.reserve(words.size() * 4);
    for (const auto word : words) {
        bytes.push_back(static_cast<std::uint8_t>(word & 0xff));
        bytes.push_back(static_cast<std::uint8_t>((word >> 8) & 0xff));
        bytes.push_back(static_cast<std::uint8_t>((word >> 16) & 0xff));
        bytes.push_back(static_cast<std::uint8_t>((word >> 24) & 0xff));
    }
    memory.write_bytes(address, bytes);
}

GuestState make_state() {
    return GuestState(GuestMemory(base, region_size));
}

StepResult run_steps(Interpreter& interpreter, int count) {
    StepResult result;
    for (int index = 0; index < count; ++index) {
        result = interpreter.step();
        if (result.outcome != StepOutcome::Executed) {
            break;
        }
    }
    return result;
}

} // namespace

int run_tests() {
    int failures = 0;
    const auto check = [&](bool passed, const char* label) {
        if (!passed) { std::cerr << label << '\n'; ++failures; }
    };
    const auto throws = []<typename Action>(Action&& action) {
        try {
            action();
        } catch (const std::runtime_error&) {
            return true;
        }
        return false;
    };

    // Straight-line arithmetic with the sign rules: addiu 0xffff is -1, slt is
    // signed (0xffffffff < 1), sltu is unsigned (0xffffffff < 1 is false) and
    // lui 0x8000 sign-extends its 32-bit result.
    {
        auto state = make_state();
        load_program(state.memory(), base,
                     {0x2408FFFF, 0x24090001, 0x0109502A, 0x0109582B, 0x3C0D8000});
        state.set_pc(base);
        Interpreter interpreter(state);
        run_steps(interpreter, 5);
        check(state.read_gpr64(8) == 0xffffffffffffffffull, "addiu sign-extends -1");
        check(state.read_gpr64(9) == 1, "addiu positive value");
        check(state.read_gpr64(10) == 1, "slt compares signed");
        check(state.read_gpr64(11) == 0, "sltu compares unsigned");
        check(state.read_gpr64(13) == 0xffffffff80000000ull, "lui sign-extends");
        check(state.pc() == base + 20, "straight-line pc advance");
    }

    // A likely branch that is not taken nullifies its delay slot; the marker
    // instruction after the branch must never execute.
    {
        auto state = make_state();
        load_program(state.memory(), base,
                     {0x2408FFFF, 0x24090001, 0x0109502A, 0x0109582B,
                      0x51400001, 0x240C7FFF, 0x240D1234});
        state.set_pc(base);
        Interpreter interpreter(state);
        run_steps(interpreter, 6);
        check(state.read_gpr64(10) == 1, "beql condition register");
        check(state.read_gpr64(12) == 0, "nullified delay slot never ran");
        check(state.read_gpr64(13) == 0x1234, "execution continued past the branch");
        check(state.pc() == base + 0x1c, "skipped exactly one word");
    }

    // jal computes ra = pc + 8; the delay slot runs before the jump; jr ra
    // returns after its own delay slot.
    {
        auto state = make_state();
        load_program(state.memory(), base,
                     {0x0C040004, 0x24080005, 0x24090006, 0x240C0099,
                      0x03E0502D, 0x03E00008, 0x240B0007});
        state.set_pc(base);
        Interpreter interpreter(state);
        run_steps(interpreter, 5);
        check(state.read_gpr64(31) == base + 8, "jal links pc + 8");
        check(state.read_gpr64(8) == 5, "jal delay slot ran before the jump");
        check(state.read_gpr64(10) == base + 8, "callee observed the link value");
        check(state.read_gpr64(11) == 7, "jr returned after its delay slot");
        check(state.pc() == base + 8, "execution resumed at the return address");
        run_steps(interpreter, 1);
        check(state.read_gpr64(9) == 6 && state.pc() == base + 12, "execution continued");
    }

    // JALR reads its target before writing the link register, so a shared
    // rd == rs encoding still jumps to the old value.
    {
        auto state = make_state();
        load_program(state.memory(), base, {0x3C080010, 0x01004009, 0x24090009});
        state.set_pc(base);
        Interpreter interpreter(state);
        run_steps(interpreter, 3);
        check(state.read_gpr64(8) == base + 12, "jalr linked into the shared register");
        check(state.read_gpr64(9) == 9, "jalr delay slot ran");
        check(state.pc() == base, "jalr jumped to the pre-write value");
    }

    // bltzal links only when taken; bltzall neither links nor runs its delay
    // slot when not taken.
    {
        auto state = make_state();
        load_program(state.memory(), base,
                     {0x2408FFFF, 0x05100002, 0x24090005, 0x240A0006, 0x240B0007});
        state.set_pc(base);
        Interpreter interpreter(state);
        run_steps(interpreter, 4);
        check(state.read_gpr64(31) == base + 12, "bltzal links pc + 8 when taken");
        check(state.read_gpr64(9) == 5, "bltzal delay slot ran");
        check(state.read_gpr64(10) == 0, "skipped word untouched");
        check(state.read_gpr64(11) == 7 && state.pc() == base + 20, "taken path continued");

        auto second = make_state();
        load_program(second.memory(), base, {0x24080001, 0x05120002, 0x24090005, 0x240A0006});
        second.set_pc(base);
        Interpreter second_interpreter(second);
        run_steps(second_interpreter, 3);
        check(second.read_gpr64(31) == 0, "bltzall does not link when not taken");
        check(second.read_gpr64(9) == 0, "bltzall nullified its delay slot");
        check(second.read_gpr64(10) == 6 && second.pc() == base + 16, "not-taken path continued");
    }

    // Illegal delay slot: a transfer in the slot stops before executing it.
    {
        auto state = make_state();
        load_program(state.memory(), base, {0x10000001, 0x0C040004, 0x24080001});
        state.set_pc(base);
        Interpreter interpreter(state);
        const auto first = interpreter.step();
        check(first.outcome == StepOutcome::Executed && state.pc() == base + 4,
              "taken branch announces the delay slot");
        const auto second = interpreter.step();
        check(second.outcome == StepOutcome::IllegalDelaySlot && second.pc == base + 4,
              "transfer in a delay slot stops with context");
        check(state.read_gpr64(31) == 0, "the illegal transfer did not execute");
    }

    // Unsupported words and syscall stop where they stand, with the pc
    // unchanged, and the stop is stable when stepped again.
    {
        auto state = make_state();
        load_program(state.memory(), base, {0x70000000});
        state.set_pc(base);
        Interpreter interpreter(state);
        const auto first = interpreter.step();
        check(first.outcome == StepOutcome::Unsupported && first.pc == base
              && first.operation == Operation::Unsupported && state.pc() == base,
              "unsupported word stops in place");
        check(interpreter.step().outcome == StepOutcome::Unsupported,
              "unsupported stop is stable");
    }
    {
        auto state = make_state();
        load_program(state.memory(), base, {0x0000000C});
        state.set_pc(base);
        Interpreter interpreter(state);
        const auto first = interpreter.step();
        check(first.outcome == StepOutcome::Exception && first.pc == base && state.pc() == base,
              "syscall stops at the exception boundary");
    }

    // Straight-line memory operations: loads sign-extend their width, stores
    // write the low bits, and doubleword access round-trips all 64 bits.
    {
        auto state = make_state();
        load_program(state.memory(), base,
                     {0x3C080010, 0x3C098000, 0xAD090100, 0x8D0A0100, 0x850B0102,
                      0x850C0100, 0xA1090104, 0xDD0D0100, 0xFD090108, 0xDD0E0108});
        state.set_pc(base);
        Interpreter interpreter(state);
        run_steps(interpreter, 10);
        check(state.read_gpr64(10) == 0xffffffff80000000ull, "lw sign-extends");
        check(state.read_gpr64(11) == 0xffffffffffff8000ull, "lh sign-extends");
        check(state.read_gpr64(12) == 0, "lh of the low halfword");
        check(state.read_gpr64(13) == 0x0000000080000000ull, "ld after a word store");
        check(state.read_gpr64(14) == 0xffffffff80000000ull, "sd/ld round-trip 64 bits");
        check(state.memory().read_byte(base + 0x104) == 0, "sb stored the low byte");
    }

    // A countdown loop: three decrements through taken branches, then the
    // not-taken exit keeps the final delay slot.
    {
        auto state = make_state();
        load_program(state.memory(), 0x00100800, {0x24080003, 0x2508FFFF, 0x1500FFFE, 0x24090001});
        state.set_pc(0x00100800);
        Interpreter interpreter(state);
        run_steps(interpreter, 10);
        check(state.read_gpr64(8) == 0, "loop counter reached zero");
        check(state.read_gpr64(9) == 1, "delay slot ran on every iteration");
        check(state.pc() == 0x00100810, "loop exited after the final delay slot");
    }

    // Fetching outside the mapped region propagates the memory error.
    {
        auto state = make_state();
        state.set_pc(0x00010000);
        Interpreter interpreter(state);
        check(throws([&] { (void)interpreter.step(); }), "unmapped fetch throws");
    }

    if (failures != 0) {
        return 1;
    }
    std::cout << "interpreter fixtures passed\n";
    return 0;
}

int main() {
#if defined(_MSC_VER) && defined(_DEBUG)
    // Report CRT errors to stderr instead of a modal dialog, and do not pop a
    // message box from abort(): a crash must fail the test run, not block it.
    _CrtSetReportMode(_CRT_ERROR, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ERROR, _CRTDBG_FILE_STDERR);
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
#endif
    try {
        return run_tests();
    } catch (const std::exception& error) {
        std::cerr << "unexpected exception: " << error.what() << '\n';
        return 1;
    }
}
