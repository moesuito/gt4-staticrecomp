#include "gt4recomp/ee_decode.hpp"

#include <array>
#include <cstdint>
#include <iostream>
#include <iterator>
#include <string_view>

using namespace gt4recomp::ee;

int main() {
    int failures = 0;
    const auto check = [&](bool passed, std::uint32_t word, std::string_view detail) {
        if (!passed) {
            std::cerr << "word 0x" << std::hex << word << ": " << detail << '\n';
            ++failures;
        }
    };

    // Literal words and expected operands were selected by hand, not encoded
    // by a helper that could share a field-position bug with the decoder.
    struct RegisterCase {
        std::uint32_t word;
        Operation operation;
        std::string_view name;
        int rs, rt, rd, shift, function;
    };
    const RegisterCase register_cases[] = {
        {0x012a4021, Operation::Addu, "addu", 9, 10, 8, 0, 0x21},
        {0x03e0f821, Operation::Addu, "addu", 31, 0, 31, 0, 0x21},
        {0x012a4023, Operation::Subu, "subu", 9, 10, 8, 0, 0x23},
        {0x012a4024, Operation::And, "and", 9, 10, 8, 0, 0x24},
        {0x012a4025, Operation::Or, "or", 9, 10, 8, 0, 0x25},
        {0x012a4026, Operation::Xor, "xor", 9, 10, 8, 0, 0x26},
        {0x03e00008, Operation::Jr, "jr", 31, 0, 0, 0, 8},
        {0x00000008, Operation::Jr, "jr", 0, 0, 0, 0, 8},
        {0x00094100, Operation::Sll, "sll", 0, 9, 8, 4, 0},
        {0x001fffc0, Operation::Sll, "sll", 0, 31, 31, 31, 0},
        {0x00000000, Operation::Sll, "sll", 0, 0, 0, 0, 0},
        {0x00094102, Operation::Srl, "srl", 0, 9, 8, 4, 2},
        {0x001fffc2, Operation::Srl, "srl", 0, 31, 31, 31, 2},
        {0x012a402a, Operation::Slt, "slt", 9, 10, 8, 0, 0x2a},
        {0x012a402b, Operation::Sltu, "sltu", 9, 10, 8, 0, 0x2b},
        {0x012a402d, Operation::Daddu, "daddu", 9, 10, 8, 0, 0x2d},
        {0x012a400a, Operation::Movz, "movz", 9, 10, 8, 0, 0x0a},
        {0x012a400b, Operation::Movn, "movn", 9, 10, 8, 0, 0x0b},
        {0x012a0018, Operation::Mult, "mult", 9, 10, 0, 0, 0x18},
        {0x012a0019, Operation::Multu, "multu", 9, 10, 0, 0, 0x19},
        {0x012a001a, Operation::Div, "div", 9, 10, 0, 0, 0x1a},
        {0x012a001b, Operation::Divu, "divu", 9, 10, 0, 0, 0x1b},
        {0x00022103, Operation::Sra, "sra", 0, 2, 4, 4, 0x03},
        {0x0040f809, Operation::Jalr, "jalr", 2, 0, 31, 0, 0x09},
        {0x0000000c, Operation::Syscall, "syscall", 0, 0, 0, 0, 0x0c},
        {0x0000040f, Operation::Sync, "sync", 0, 0, 0, 16, 0x0f},
        {0x00001010, Operation::Mfhi, "mfhi", 0, 0, 2, 0, 0x10},
        {0x00400011, Operation::Mthi, "mthi", 2, 0, 0, 0, 0x11},
        {0x00001012, Operation::Mflo, "mflo", 0, 0, 2, 0, 0x12},
        {0x00400013, Operation::Mtlo, "mtlo", 2, 0, 0, 0, 0x13},
    };
    for (const auto& expected : register_cases) {
        const auto actual = decode(expected.word);
        check(actual.word == expected.word && actual.primary_opcode == 0,
              expected.word, "R-format word/opcode");
        check(actual.operation == expected.operation && mnemonic(actual.operation) == expected.name,
              expected.word, "operation/mnemonic");
        check(actual.rs == expected.rs && actual.rt == expected.rt && actual.rd == expected.rd
              && actual.shift_amount == expected.shift && actual.function == expected.function,
              expected.word, "R-format fields");
    }

    struct ImmediateCase {
        std::uint32_t word;
        Operation operation;
        std::string_view name;
        int opcode, rs, rt;
        std::uint16_t immediate;
        std::int32_t signed_value;
    };
    const ImmediateCase immediate_cases[] = {
        {0x3044000f, Operation::Andi, "andi", 12, 2, 4, 0x000f, 15},
        {0x33ffffff, Operation::Andi, "andi", 12, 31, 31, 0xffff, -1},
        {0x3484fff0, Operation::Ori, "ori", 13, 4, 4, 0xfff0, -16},
        {0x34028000, Operation::Ori, "ori", 13, 0, 2, 0x8000, -32768},
        {0x25280001, Operation::Addiu, "addiu", 9, 9, 8, 0x0001, 1},
        {0x27bdfff0, Operation::Addiu, "addiu", 9, 29, 29, 0xfff0, -16},
        {0x2408ffff, Operation::Addiu, "addiu", 9, 0, 8, 0xffff, -1},
        {0x27ff8000, Operation::Addiu, "addiu", 9, 31, 31, 0x8000, -32768},
        {0x24087fff, Operation::Addiu, "addiu", 9, 0, 8, 0x7fff, 32767},
        {0x24080000, Operation::Addiu, "addiu", 9, 0, 8, 0x0000, 0},
        {0x3c081234, Operation::Lui, "lui", 15, 0, 8, 0x1234, 4660},
        {0x3c1fffff, Operation::Lui, "lui", 15, 0, 31, 0xffff, -1},
        {0x8fa80010, Operation::Lw, "lw", 35, 29, 8, 0x0010, 16},
        {0x8fe8fffc, Operation::Lw, "lw", 35, 31, 8, 0xfffc, -4},
        {0xafa80010, Operation::Sw, "sw", 43, 29, 8, 0x0010, 16},
        {0xafe8fffc, Operation::Sw, "sw", 43, 31, 8, 0xfffc, -4},
        {0x11090003, Operation::Beq, "beq", 4, 8, 9, 0x0003, 3},
        {0x1109fffe, Operation::Beq, "beq", 4, 8, 9, 0xfffe, -2},
        {0x15090003, Operation::Bne, "bne", 5, 8, 9, 0x0003, 3},
        {0x1509fffe, Operation::Bne, "bne", 5, 8, 9, 0xfffe, -2},
        {0x8603000c, Operation::Lh, "lh", 33, 16, 3, 0x000c, 12},
        {0xa0400000, Operation::Sb, "sb", 40, 2, 0, 0x0000, 0},
        {0xdfb00000, Operation::Ld, "ld", 55, 29, 16, 0x0000, 0},
        {0xffbf0000, Operation::Sd, "sd", 63, 29, 31, 0x0000, 0},
        {0x50400004, Operation::Beql, "beql", 20, 2, 0, 0x0004, 4},
        {0x54600005, Operation::Bnel, "bnel", 21, 3, 0, 0x0005, 5},
        {0x90820003, Operation::Lbu, "lbu", 36, 4, 2, 0x0003, 3},
        {0x80820003, Operation::Lb, "lb", 32, 4, 2, 0x0003, 3},
        {0x2c820006, Operation::Sltiu, "sltiu", 11, 4, 2, 0x0006, 6},
        {0x28820006, Operation::Slti, "slti", 10, 4, 2, 0x0006, 6},
        {0x38420054, Operation::Xori, "xori", 14, 2, 2, 0x0054, 84},
        {0x38081234, Operation::Xori, "xori", 14, 0, 8, 0x1234, 4660},
        {0x1840000c, Operation::Blez, "blez", 6, 2, 0, 0x000c, 12},
        {0x1d200004, Operation::Bgtz, "bgtz", 7, 9, 0, 0x0004, 4},
        {0x06000009, Operation::Bltz, "bltz", 1, 16, 0, 0x0009, 9},
        {0x05210004, Operation::Bgez, "bgez", 1, 9, 1, 0x0004, 4},
        {0x05220004, Operation::Bltzl, "bltzl", 1, 9, 2, 0x0004, 4},
        {0x0603fffc, Operation::Bgezl, "bgezl", 1, 16, 3, 0xfffc, -4},
        {0x05300004, Operation::Bltzal, "bltzal", 1, 9, 16, 0x0004, 4},
        {0x05310004, Operation::Bgezal, "bgezal", 1, 9, 17, 0x0004, 4},
        {0x05320004, Operation::Bltzall, "bltzall", 1, 9, 18, 0x0004, 4},
        {0x05330004, Operation::Bgezall, "bgezall", 1, 9, 19, 0x0004, 4},
        {0x70000c28, Operation::Padduw, "padduw", 0x1c, 0, 0, 0x0c28, 3112},
        {0x70000011, Operation::Mthi1, "mthi1", 0x1c, 0, 0, 0x0011, 17},
        {0x70000013, Operation::Mtlo1, "mtlo1", 0x1c, 0, 0, 0x0013, 19},
        {0x70431008, Operation::Paddw, "paddw", 0x1c, 2, 3, 0x1008, 4104},
        {0x04190000, Operation::Mtsah, "mtsah", 0x01, 0, 25, 0x0000, 0},
        {0x44800000, Operation::Mtc1, "mtc1", 0x11, 4, 0, 0x0000, 0},
        {0x44c0f800, Operation::Ctc1, "ctc1", 0x11, 6, 0, 0xf800, -2048},
        {0x46010018, Operation::AddaS, "adda.s", 0x11, 16, 1, 0x0018, 24},
        {0x45010002, Operation::Bc1t, "bc1t", 0x11, 8, 1, 0x0002, 2},
        {0xc4800010, Operation::Lwc1, "lwc1", 0x31, 4, 0, 0x0010, 16},
        {0x7c400000, Operation::Sq, "sq", 0x1f, 2, 0, 0x0000, 0},
        {0x78220000, Operation::Lq, "lq", 0x1e, 1, 2, 0x0000, 0},
        {0x9c820000, Operation::Lwu, "lwu", 0x27, 4, 2, 0x0000, 0},
        {0x94820000, Operation::Lhu, "lhu", 0x25, 4, 2, 0x0000, 0},
        {0xa4820000, Operation::Sh, "sh", 0x29, 4, 2, 0x0000, 0},
        {0x88830003, Operation::Lwl, "lwl", 0x22, 4, 3, 0x0003, 3},
        {0x98830003, Operation::Lwr, "lwr", 0x26, 4, 3, 0x0003, 3},
        {0xa8830003, Operation::Swl, "swl", 0x2a, 4, 3, 0x0003, 3},
        {0xb8830003, Operation::Swr, "swr", 0x2e, 4, 3, 0x0003, 3},
        {0x70000000, Operation::Madd, "madd", 0x1c, 0, 0, 0x0000, 0},
        {0x70000001, Operation::Maddu, "maddu", 0x1c, 0, 0, 0x0001, 1},
        {0x70000004, Operation::Plzcw, "plzcw", 0x1c, 0, 0, 0x0004, 4},
        {0x70000018, Operation::Mult1, "mult1", 0x1c, 0, 0, 0x0018, 24},
        {0x70000019, Operation::Multu1, "multu1", 0x1c, 0, 0, 0x0019, 25},
        {0x7000001a, Operation::Div1, "div1", 0x1c, 0, 0, 0x001a, 26},
        {0x7000001b, Operation::Divu1, "divu1", 0x1c, 0, 0, 0x001b, 27},
        {0x70000020, Operation::Madd1, "madd1", 0x1c, 0, 0, 0x0020, 32},
        {0x70000021, Operation::Maddu1, "maddu1", 0x1c, 0, 0, 0x0021, 33},
    };
    for (const auto& expected : immediate_cases) {
        const auto actual = decode(expected.word);
        check(actual.word == expected.word && actual.primary_opcode == expected.opcode,
              expected.word, "I-format word/opcode");
        check(actual.operation == expected.operation && mnemonic(actual.operation) == expected.name,
              expected.word, "operation/mnemonic");
        check(actual.rs == expected.rs && actual.rt == expected.rt
              && actual.immediate == expected.immediate
              && actual.signed_immediate() == expected.signed_value,
              expected.word, "I-format fields/sign extension");
    }

    struct JumpCase {
        std::uint32_t word;
        Operation operation;
        std::string_view name;
        int opcode;
        std::uint32_t index;
    };
    const JumpCase jump_cases[] = {
        {0x08040000, Operation::J, "j", 2, 0x00040000},
        {0x0bffffff, Operation::J, "j", 2, 0x03ffffff},
        {0x0c040000, Operation::Jal, "jal", 3, 0x00040000},
        {0x0c000000, Operation::Jal, "jal", 3, 0},
    };
    for (const auto& expected : jump_cases) {
        const auto actual = decode(expected.word);
        check(actual.word == expected.word && actual.primary_opcode == expected.opcode
              && actual.jump_index == expected.index, expected.word, "J-format fields");
        check(actual.operation == expected.operation && mnemonic(actual.operation) == expected.name,
              expected.word, "operation/mnemonic");
    }

    // Outside the implemented subset, plus nonzero fixed fields. Unsupported
    // is our policy; it makes no claim about a hardware reserved-instruction trap.
    const std::uint32_t unsupported[] = {
        0x0000000d, 0x70000002, 0x46800000, 0x40036000, 0x041a0000,
        0x00294100, 0x00294102, 0x03e10008, 0x03e00808, 0x03e00048,
        0x012a4061, 0x012a4063, 0x012a4064, 0x012a4065, 0x012a4066,
        0x3c281234, 0x19280004, 0x1d280004, 0x0120f849,
    };
    for (const auto word : unsupported) {
        const auto actual = decode(word);
        check(actual.operation == Operation::Unsupported && actual.word == word
              && mnemonic(actual.operation) == "unsupported", word, "unsupported preserved");
    }

    const std::array<std::uint8_t, 4> stack_adjustment = {0xf0, 0xff, 0xbd, 0x27};
    check(read_instruction_word(stack_adjustment) == 0x27bdfff0, 0x27bdfff0, "little endian");
    const std::array<std::uint8_t, 4> high_bit = {0x10, 0x00, 0xa8, 0xaf};
    check(read_instruction_word(high_bit) == 0xafa80010, 0xafa80010, "unsigned byte shifts");

    if (failures != 0) {
        return 1;
    }
    std::cout << std::size(register_cases) + std::size(immediate_cases) + std::size(jump_cases)
              << " hand-selected instructions, " << std::size(unsupported)
              << " unsupported encodings, endian checks passed\n";
    return 0;
}
