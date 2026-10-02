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
                  const std::vector<std::uint32_t>& words) {
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

void load_program(GuestMemory& memory, std::uint32_t address,
                  std::initializer_list<std::uint32_t> words) {
    load_program(memory, address, std::vector<std::uint32_t>(words));
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

    // 64-bit comparisons and the new integer operations: SLT/SLTIU use the
    // full registers (a DADDU-built value distinguishes them from the old
    // 32-bit reading), and SRA fills with the sign.
    {
        auto state = make_state();
        load_program(state.memory(), base,
                     {0x3C088000, 0x0108682D, 0x00084903, 0x01A0502A, 0x000D582A, 0x2DAE0001});
        state.set_pc(base);
        Interpreter interpreter(state);
        run_steps(interpreter, 6);
        check(state.read_gpr64(13) == 0xffffffff00000000ull, "daddu builds a 64-bit value");
        check(state.read_gpr64(9) == 0xfffffffff8000000ull, "sra fills with the sign");
        check(state.read_gpr64(10) == 1, "slt compares 64-bit signed values");
        check(state.read_gpr64(11) == 0, "slt negative direction");
        check(state.read_gpr64(14) == 0, "sltiu compares 64-bit unsigned values");
    }

    // LB and LBU differ in extension: the same byte reads negative or up.
    {
        auto state = make_state();
        state.memory().write_byte(base + 0x40, 0x80);
        load_program(state.memory(), base, {0x3C080010, 0x81090040, 0x910A0040});
        state.set_pc(base);
        Interpreter interpreter(state);
        run_steps(interpreter, 3);
        check(state.read_gpr64(9) == 0xffffffffffffff80ull, "lb sign-extends");
        check(state.read_gpr64(10) == 0x80ull, "lbu zero-extends");
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
        load_program(state.memory(), base, {0x70000002});
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

    // The game's startup prologue, rebuilt here from its encodings: clear the
    // working registers (padduw rN, r0, r0 for r1..r29), both HI/LO pairs, the
    // MMI shift cache, all 32 FPU registers, the FPU accumulator and FCR31.
    // Every location is preloaded with junk so the clearing is observed.
    {
        auto state = make_state();
        std::vector<std::uint32_t> program;
        for (std::uint32_t reg = 1; reg <= 29; ++reg) {
            program.push_back(0x70000028u | (reg << 11) | (0x10u << 6));
        }
        program.push_back(0x00000011u);  // mthi r0
        program.push_back(0x70000011u);  // mthi1 r0
        program.push_back(0x00000013u);  // mtlo r0
        program.push_back(0x70000013u);  // mtlo1 r0
        program.push_back(0x04190000u);  // mtsah r0, 0
        for (std::uint32_t fpr = 0; fpr < 32; ++fpr) {
            program.push_back(0x44800000u | (fpr << 11));  // mtc1 r0, fN
        }
        program.push_back(0x46010018u);  // adda.s f0, f1: clears the accumulator
        program.push_back(0x0000040fu);  // sync
        program.push_back(0x44c0f800u);  // ctc1 r0, f31
        load_program(state.memory(), base, program);

        for (std::uint8_t reg = 1; reg < 32; ++reg) {
            state.write_gpr64(reg, 0x1111111111111111ull);
            state.write_gpr_high64(reg, 0x2222222222222222ull);
        }
        state.set_hi(0x3333333333333333ull);
        state.set_lo(0x4444444444444444ull);
        state.set_hi1(0x5555555555555555ull);
        state.set_lo1(0x6666666666666666ull);
        state.set_shift_amount_cache(31);
        for (std::uint8_t fpr = 0; fpr < 32; ++fpr) {
            state.write_fpr(fpr, 0x3f800000u);
        }
        state.set_fpu_accumulator(0x40490fdbu);
        state.set_fpu_control(0xffffffffu);

        state.set_pc(base);
        Interpreter interpreter(state);
        const auto result = run_steps(interpreter, 69);
        check(result.outcome == StepOutcome::Executed && state.pc() == base + 69 * 4,
              "startup prologue ran to its end");
        bool registers_cleared = true;
        for (std::uint8_t reg = 1; reg <= 29; ++reg) {
            registers_cleared = registers_cleared && state.read_gpr64(reg) == 0
                && state.read_gpr_high64(reg) == 0;
        }
        check(registers_cleared, "padduw cleared r1..r29 in both halves");
        check(state.read_gpr64(30) == 0x1111111111111111ull
                  && state.read_gpr64(31) == 0x1111111111111111ull,
              "r30 and r31 keep their values");
        check(state.hi() == 0 && state.lo() == 0 && state.hi1() == 0 && state.lo1() == 0,
              "mthi/mtlo cleared both HI/LO pairs");
        check(state.shift_amount_cache() == 0, "mtsah cleared the shift cache");
        bool fpus_cleared = true;
        for (std::uint8_t fpr = 0; fpr < 32; ++fpr) {
            fpus_cleared = fpus_cleared && state.read_fpr(fpr) == 0;
        }
        check(fpus_cleared, "mtc1 cleared all FPU registers");
        check(state.fpu_accumulator() == 0, "adda.s cleared the FPU accumulator");
        check(state.fpu_control() == 0, "ctc1 cleared FCR31");
    }

    // FPU arithmetic with hand-computed bit patterns: 1.5f + 2.25f = 3.75f,
    // and the compare drives the FCR31 condition flag (bit 23) that the bc1t
    // branches read. A taken branch still runs its delay slot and resumes one
    // word further.
    {
        auto state = make_state();
        load_program(state.memory(), base,
                     {0x3C083FC0,    // lui t0, 0x3fc0        (1.5f)
                      0x3C094010,    // lui t1, 0x4010        (2.25f)
                      0x44880000,    // mtc1 t0, f0
                      0x44890800,    // mtc1 t1, f1
                      0x46010080,    // add.s f2, f0, f1
                      0x46021032,    // c.eq.s f2, f2
                      0x45010002,    // bc1t +2               (taken)
                      0x240A0111,    // addiu t2, zero, 0x111 (delay slot)
                      0x240B0222,    // addiu t3, zero, 0x222 (skipped)
                      0x240C0333,    // addiu t4, zero, 0x333 (branch target)
                      0x45010002,    // bc1t +2               (taken again)
                      0x240D0444,    // addiu t5, zero, 0x444 (delay slot)
                      0x240E0555,    // addiu t6, zero, 0x555 (skipped)
                      0x240F0666});  // addiu t7, zero, 0x666 (target)
        state.set_pc(base);
        Interpreter interpreter(state);
        run_steps(interpreter, 14);
        check(state.read_fpr(2) == 0x40700000u, "add.s produced 3.75f");
        check((state.fpu_control() & 0x00800000u) != 0, "c.eq.s set the condition flag");
        check(state.read_gpr64(10) == 0x111, "delay slot ran under the taken bc1t");
        check(state.read_gpr64(11) == 0, "word after the delay slot was skipped");
        check(state.read_gpr64(12) == 0x333, "execution resumed at the target");
        check(state.read_gpr64(13) == 0x444, "second delay slot ran");
        check(state.read_gpr64(14) == 0, "second skip held");
        check(state.read_gpr64(15) == 0x666, "second target reached");
    }

    // MMI lanes with hand-computed saturation: padduw clamps unsigned word
    // adds at 0xffffffff, paddsw clamps signed word adds at 0x7fffffff, and
    // pextlw interleaves the low halves of rs and rt across all four lanes.
    {
        auto state = make_state();
        load_program(state.memory(), base,
                     {0x2402FFFF,    // addiu v0, zero, -1     (0xffff...ff)
                      0x24080001,    // addiu t0, zero, 1
                      0x2409FFFF,    // addiu t1, zero, -1
                      0x00094842,    // srl t1, t1, 1          (0x7fffffff)
                      0x704C1428,    // padduw v0, v0, t0
                      0x71281C08,    // paddsw v1, t1, t0
                      0x71092488});  // pextlw a0, t0, t1
        state.set_pc(base);
        Interpreter interpreter(state);
        run_steps(interpreter, 7);
        check(state.read_gpr64(2) == 0xffffffffffffffffull,
              "padduw saturated both words of the low half");
        check(state.read_gpr_high64(2) == 0,
              "padduw upper lanes held zero plus zero");
        check(state.read_gpr64(3) == 0x7fffffffull, "paddsw clamped the signed sum");
        check(state.read_gpr64(4) == 0x000000017fffffffull,
              "pextlw interleaved the low words");
        check(state.read_gpr_high64(4) == 0, "pextlw upper lanes came from zero sources");
    }

    // movz and movn: the destination changes only when the full 64-bit rt is
    // zero (movz) or nonzero (movn); the condition register is rt, not rs.
    {
        auto state = make_state();
        load_program(state.memory(), base,
                     {0x24080001,    // addiu t0, zero, 1
                      0x0100100a,    // movz v0, t0, zero   (rt = 0: moves)
                      0x0100180b,    // movn v1, t0, zero   (rt = 0: does not move)
                      0x0108200a});  // movz a0, t0, t0     (rt != 0: does not move)
        state.set_pc(base);
        Interpreter interpreter(state);
        run_steps(interpreter, 4);
        check(state.read_gpr64(2) == 1, "movz moves when rt is zero");
        check(state.read_gpr64(3) == 0, "movn does not move when rt is zero");
        check(state.read_gpr64(4) == 0, "movz does not move when rt is nonzero");
    }

    // Multiplication and division with hand-computed results: 7 * -3 = -21
    // (LO holds -21 sign-extended, HI the high part), -7 / 3 truncates toward
    // zero (LO = -2, HI = -1) and division by zero signals through LO/HI.
    {
        auto state = make_state();
        load_program(state.memory(), base,
                     {0x24080007,    // addiu t0, zero, 7
                      0x2409FFFD,    // addiu t1, zero, -3
                      0x01090018,    // mult t0, t1
                      0x240AFFF9,    // addiu t2, zero, -7
                      0x240B0003,    // addiu t3, zero, 3
                      0x014B001A,    // div t2, t3
                      0x240C0005,    // addiu t4, zero, 5
                      0x0180001A});  // div t4, zero
        state.set_pc(base);
        Interpreter interpreter(state);
        run_steps(interpreter, 3);
        check(state.lo() == 0xffffffffffffffebull, "mult LO holds the signed product");
        check(state.hi() == 0xffffffffffffffffull, "mult HI holds the high part");
        run_steps(interpreter, 3);
        check(state.lo() == 0xfffffffffffffffeull, "div truncates toward zero");
        check(state.hi() == 0xffffffffffffffffull, "div remainder keeps the sign");
        run_steps(interpreter, 2);
        check(state.lo() == 0xffffffffffffffffull, "div by zero signals in LO");
        check(state.hi() == 5ull, "div by zero keeps the dividend in HI");
    }

    // The MMI compact forms write the second HI/LO bank: 3 * 5 through
    // multu1, read back with mflo1/mfhi1.
    {
        auto state = make_state();
        load_program(state.memory(), base,
                     {0x24080003,    // addiu t0, zero, 3
                      0x24090005,    // addiu t1, zero, 5
                      0x71090019,    // multu1 t0, t1
                      0x70001012,    // mflo1 v0
                      0x70001810});  // mfhi1 v1
        state.set_pc(base);
        Interpreter interpreter(state);
        run_steps(interpreter, 5);
        check(state.lo1() == 15 && state.hi1() == 0, "multu1 wrote the second bank");
        check(state.lo() == 0 && state.hi() == 0, "the first bank stayed clear");
        check(state.read_gpr64(2) == 15, "mflo1 read the second bank");
        check(state.read_gpr64(3) == 0, "mfhi1 read the second bank");
    }

    // Unaligned loads and stores with hand-computed merges. The data window
    // sits at +0x100 (beyond the program): 0x11223344 at +0x100 and
    // 0xDEADBEEF at +0x108. The lwl/lwr results follow the reference
    // mask/shift tables, and lwr with a nonzero shift keeps the register's
    // upper half (the lwr a2 check at the end).
    {
        auto state = make_state();
        state.memory().write_word(base + 0x100, 0x11223344);
        state.memory().write_word(base + 0x108, 0xDEADBEEF);
        load_program(state.memory(), base,
                     {0x3C090010,    // lui t1, 0x10
                      0x25290100,    // addiu t1, t1, 0x100
                      0x89220000,    // lwl v0, 0x0(t1)
                      0x89230002,    // lwl v1, 0x2(t1)
                      0x99240001,    // lwr a0, 0x1(t1)
                      0x99270000,    // lwr a3, 0x0(t1)
                      0x3C0B8000,    // lui t3, 0x8000
                      0x016B582D,    // daddu t3, t3, t3   (0xffffffff00000000)
                      0x3C0C2ABB,    // lui a4, 0x2abb
                      0x358CCCDD,    // ori a4, a4, 0xccdd
                      0x016C302D,    // daddu a2, t3, a4   (0xffffffff2abbccdd)
                      0x99260002,    // lwr a2, 0x2(t1)
                      0x3C08AABB,    // lui t0, 0xaabb
                      0x3508CCDD,    // ori t0, t0, 0xccdd
                      0xA9280008,    // swl t0, 0x8(t1)
                      0xB9280009,    // swr t0, 0x9(t1)
                      0xB928000B});  // swr t0, 0xb(t1)
        state.set_pc(base);
        Interpreter interpreter(state);
        run_steps(interpreter, 17);
        check(state.read_gpr64(2) == 0x0000000044000000ull, "lwl shift 0");
        check(state.read_gpr64(3) == 0x0000000022334400ull, "lwl shift 2");
        check(state.read_gpr64(4) == 0x0000000000112233ull, "lwr shift 1");
        check(state.read_gpr64(7) == 0x0000000011223344ull, "lwr shift 0 sign-extends");
        check(state.read_gpr64(6) == 0xffffffff2abb1122ull,
              "lwr shift 2 keeps the register's upper half");
        check(state.memory().read_word(base + 0x108) == 0xddccddaaull,
              "swl/swr merge the stored bytes");
    }

    // PLZCW counts the leading sign-equal bits minus one for each of the low
    // two source words: zero counts 31, 0x80000000 counts 0, 1 counts 30.
    {
        auto state = make_state();
        load_program(state.memory(), base,
                     {0x24080000,    // addiu t0, zero, 0
                      0x71001004,    // plzcw v0, t0
                      0x3C098000,    // lui t1, 0x8000
                      0x71201804,    // plzcw v1, t1
                      0x240A0001,    // addiu t2, zero, 1
                      0x71401004});  // plzcw v0, t2
        state.set_pc(base);
        Interpreter interpreter(state);
        run_steps(interpreter, 2);
        check(state.read_gpr64(2) == 0x0000001f0000001full, "plzcw of zero words");
        run_steps(interpreter, 2);
        check(state.read_gpr64(3) == 0x0000001f00000000ull, "plzcw of the sign bit");
        run_steps(interpreter, 2);
        check(state.read_gpr64(2) == 0x0000001f0000001eull, "plzcw of one");
    }

    // COP0 with hand-computed values: the Status register starts at the
    // live-observed 0x40000000, mfc0 applies the readable-bits mask, mtc0
    // writes through, ei/di toggle EIE in kernel mode only, the Config write
    // protects the cache-size bits, and break stops at the trap boundary.
    {
        auto state = make_state();
        load_program(state.memory(), base,
                     {0x40086000,    // mfc0 t0, Status
                      0x40886000,    // mtc0 t0, Status
                      0x42000038,    // ei
                      0x42000039,    // di
                      0x40026000,    // mfc0 v0, Status
                      0x0000000D});  // break
        state.set_pc(base);
        Interpreter interpreter(state);
        run_steps(interpreter, 1);
        check(state.read_gpr64(8) == 0x40000000ull, "mfc0 reads the masked Status");
        run_steps(interpreter, 1);
        check(state.read_cp0(12) == 0x40000000u, "mtc0 wrote the value back");
        run_steps(interpreter, 1);
        check(state.read_cp0(12) == 0x40010000u, "ei set EIE in kernel mode");
        run_steps(interpreter, 1);
        check(state.read_cp0(12) == 0x40000000u, "di cleared EIE again");
        run_steps(interpreter, 1);
        check(state.read_gpr64(2) == 0x40000000ull, "the toggled status reads back");
        const auto stopped = interpreter.step();
        check(stopped.outcome == StepOutcome::Exception && stopped.pc == base + 20
                  && stopped.operation == Operation::Break,
              "break stops at the trap boundary");
    }

    // With KSU set to supervisor mode, ei is gated out: EIE never changes.
    {
        auto state = make_state();
        load_program(state.memory(), base,
                     {0x24080008,    // addiu t0, zero, 8   (KSU = supervisor)
                      0x40886000,    // mtc0 t0, Status
                      0x42000038,    // ei                  (gated out)
                      0x40026000});  // mfc0 v0, Status
        state.set_pc(base);
        Interpreter interpreter(state);
        run_steps(interpreter, 4);
        check(state.read_cp0(12) == 8u, "ei did not set EIE outside kernel mode");
        check(state.read_gpr64(2) == 8ull, "the status read back unchanged");
    }

    // mtc0 to Config protects the cache-size bits and reports the fixed ones.
    {
        auto state = make_state();
        load_program(state.memory(), base,
                     {0x3C08FFFF,    // lui t0, 0xffff
                      0x40888000,    // mtc0 t0, Config
                      0x40038000});  // mfc0 v1, Config
        state.set_pc(base);
        Interpreter interpreter(state);
        run_steps(interpreter, 3);
        check(state.read_cp0(16) == 0xffff0440u, "Config write masked and fixed bits set");
        check(state.read_gpr64(3) == 0xffffffffffff0440ull, "Config reads sign-extended");
    }

    // 64-bit and variable shifts with hand-computed results: dsll32 moves the
    // word to the high half, dsra32 fills with the sign, dsrav shifts a full
    // 64-bit value and the V-forms take their amount from rs.
    {
        auto state = make_state();
        load_program(state.memory(), base,
                     {0x24080001,    // addiu t0, zero, 1
                      0x00084938,    // dsll t1, t0, 4          (0x10)
                      0x00094B3C,    // dsll32 t1, t1, 12       (1 << 44)
                      0x00094B3E,    // dsrl32 t1, t1, 12       (back to 0x10)
                      0x2402FFFF,    // addiu v0, zero, -1
                      0x0002503C,    // dsll32 t2, v0, 0        (0xffffffff00000000)
                      0x000A5BFF,    // dsra32 t3, t2, 31       (all ones)
                      0x240C0004,    // addiu t4, zero, 4
                      0x01886804,    // sllv t5, t0, t4         (0x10)
                      0x018A7017,    // dsrav t6, t2, t4        (0xfffffffff0000000)
                      0x0182C807});  // srav t9, v0, t4         (all ones)
        state.set_pc(base);
        Interpreter interpreter(state);
        run_steps(interpreter, 11);
        check(state.read_gpr64(9) == 0x10ull, "dsll/dsll32/dsrl32 round-trip");
        check(state.read_gpr64(10) == 0xffffffff00000000ull, "dsll32 moved the word up");
        check(state.read_gpr64(11) == 0xffffffffffffffffull, "dsra32 filled with the sign");
        check(state.read_gpr64(13) == 0x10ull, "sllv took its amount from rs");
        check(state.read_gpr64(14) == 0xfffffffff0000000ull, "dsrav shifted the 64-bit value");
        check(state.read_gpr64(25) == 0xffffffffffffffffull, "srav filled with the sign");
    }

    // Unaligned doubleword loads and stores: the ldr/ldl pair reassembles the
    // eight bytes at +0x103 and the sdr/sdl pair writes a value back across
    // the same window, with every merged word checked.
    {
        auto state = make_state();
        state.memory().write_doubleword(base + 0x100, 0x1122334455667788ull);
        state.memory().write_doubleword(base + 0x108, 0x99aabbccddeeff00ull);
        load_program(state.memory(), base,
                     {0x3C090010,    // lui t1, 0x10
                      0x25290100,    // addiu t1, t1, 0x100
                      0x6D280003,    // ldr t0, 0x3(t1)
                      0x6928000A,    // ldl t0, 0xa(t1)
                      0x3C0AAABB,    // lui t2, 0xaabb
                      0x354ACCDD,    // ori t2, t2, 0xccdd
                      0x000A503C,    // dsll32 t2, t2, 0     (0xaabbccdd00000000)
                      0x340BEEFF,    // ori t3, zero, 0xeeff
                      0x000B5C38,    // dsll t3, t3, 16      (0xeeff0000)
                      0x356B0011,    // ori t3, t3, 0x0011   (0xeeff0011)
                      0x014B502D,    // daddu t2, t2, t3     (0xaabbccddeeff0011)
                      0xB52A0003,    // sdr t2, 0x3(t1)
                      0xB12A000A});  // sdl t2, 0xa(t1)
        state.set_pc(base);
        Interpreter interpreter(state);
        run_steps(interpreter, 13);
        check(state.read_gpr64(8) == 0xeeff001122334455ull,
              "ldr/ldl reassembled the unaligned value");
        check(state.read_gpr64(10) == 0xaabbccddeeff0011ull, "the value to store was built");
        // The merged words, hand-computed: the SDR result is
        // (value << 24) | (memory & 0xffffff), the SDL result is
        // (value >> 40) | (memory & 0xffffffffff000000).
        check(state.memory().read_word(base + 0x100) == 0x11667788u, "sdr wrote the merged low word");
        check(state.memory().read_word(base + 0x104) == 0xddeeff00u, "sdr wrote the merged high word");
        check(state.memory().read_word(base + 0x108) == 0xddaabbccu, "sdl merged its three bytes");
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
