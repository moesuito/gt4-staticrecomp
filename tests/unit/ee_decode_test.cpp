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
        {0x012a4020, Operation::Add, "add", 9, 10, 8, 0, 0x20},
        {0x012a4022, Operation::Sub, "sub", 9, 10, 8, 0, 0x22},
        {0x012a402c, Operation::Dadd, "dadd", 9, 10, 8, 0, 0x2c},
        {0x012a402e, Operation::Dsub, "dsub", 9, 10, 8, 0, 0x2e},
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
        {0x012a402f, Operation::Dsubu, "dsubu", 9, 10, 8, 0, 0x2f},
        {0x012a400a, Operation::Movz, "movz", 9, 10, 8, 0, 0x0a},
        {0x012a400b, Operation::Movn, "movn", 9, 10, 8, 0, 0x0b},
        {0x012a4027, Operation::Nor, "nor", 9, 10, 8, 0, 0x27},
        {0x012a0018, Operation::Mult, "mult", 9, 10, 0, 0, 0x18},
        {0x012a0019, Operation::Multu, "multu", 9, 10, 0, 0, 0x19},
        {0x012a001a, Operation::Div, "div", 9, 10, 0, 0, 0x1a},
        {0x012a001b, Operation::Divu, "divu", 9, 10, 0, 0, 0x1b},
        {0x00022103, Operation::Sra, "sra", 0, 2, 4, 4, 0x03},
        {0x01494004, Operation::Sllv, "sllv", 10, 9, 8, 0, 0x04},
        {0x01494006, Operation::Srlv, "srlv", 10, 9, 8, 0, 0x06},
        {0x01494007, Operation::Srav, "srav", 10, 9, 8, 0, 0x07},
        {0x01494014, Operation::Dsllv, "dsllv", 10, 9, 8, 0, 0x14},
        {0x01494016, Operation::Dsrlv, "dsrlv", 10, 9, 8, 0, 0x16},
        {0x01494017, Operation::Dsrav, "dsrav", 10, 9, 8, 0, 0x17},
        {0x00094138, Operation::Dsll, "dsll", 0, 9, 8, 4, 0x38},
        {0x0009413a, Operation::Dsrl, "dsrl", 0, 9, 8, 4, 0x3a},
        {0x0009413b, Operation::Dsra, "dsra", 0, 9, 8, 4, 0x3b},
        {0x0009413c, Operation::Dsll32, "dsll32", 0, 9, 8, 4, 0x3c},
        {0x0009413e, Operation::Dsrl32, "dsrl32", 0, 9, 8, 4, 0x3e},
        {0x0009413f, Operation::Dsra32, "dsra32", 0, 9, 8, 4, 0x3f},
        {0x0040f809, Operation::Jalr, "jalr", 2, 0, 31, 0, 0x09},
        {0x0000000c, Operation::Syscall, "syscall", 0, 0, 0, 0, 0x0c},
        {0x0000000d, Operation::Break, "break", 0, 0, 0, 0, 0x0d},
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
        {0xbd180000, Operation::Cache, "cache", 0x2f, 8, 24, 0x0000, 0},
        {0x40026000, Operation::Mfc0, "mfc0", 0x10, 0, 2, 0x6000, 24576},
        {0x40826000, Operation::Mtc0, "mtc0", 0x10, 4, 2, 0x6000, 24576},
        {0x42000038, Operation::Ei, "ei", 0x10, 16, 0, 0x0038, 56},
        {0x42000039, Operation::Di, "di", 0x10, 16, 0, 0x0039, 57},
        {0x42000018, Operation::Eret, "eret", 0x10, 16, 0, 0x0018, 24},
        {0x48a80800, Operation::Qmtc2, "qmtc2", 0x12, 5, 8, 0x0800, 2048},
        {0x48290800, Operation::Qmfc2, "qmfc2", 0x12, 1, 9, 0x0800, 2048},
        {0x484aa000, Operation::Cfc2, "cfc2", 0x12, 2, 10, 0xa000, -24576},
        {0x48cab000, Operation::Ctc2, "ctc2", 0x12, 6, 10, 0xb000, -20480},
        {0xd8420000, Operation::Lqc2, "lqc2", 0x36, 2, 2, 0x0000, 0},
        {0xf8410010, Operation::Sqc2, "sqc2", 0x3e, 2, 1, 0x0010, 16},
        {0x4a0002ff, Operation::Vnop, "vnop", 0x12, 16, 0, 0x02ff, 767},
        // VU macro arithmetic: hand-assembled forms plus observed words from
        // the pinned text (0x4BE1E1BC, 0x4B010841, 0x4BE0F84B, 0x4BF8A33C,
        // 0x4A6103BE, 0x4BC532FE and 0x4BE1097D).
        {0x4BE20928, Operation::Vadd, "vadd", 0x12, 31, 2, 0x0928, 2344},
        {0x4BE1112C, Operation::Vsub, "vsub", 0x12, 31, 1, 0x112c, 4396},
        {0x4BE3094B, Operation::Vmaddw, "vmaddw", 0x12, 31, 3, 0x094B, 2379},
        {0x4BE0F84B, Operation::Vmaddw, "vmaddw", 0x12, 31, 0, 0xf84b, -1973},
        {0x4BE311BC, Operation::Vmulax, "vmulax", 0x12, 31, 3, 0x11bc, 4540},
        {0x4BE1E1BC, Operation::Vmulax, "vmulax", 0x12, 31, 1, 0xe1bc, -7748},
        {0x4B010841, Operation::Vaddy, "vaddy", 0x12, 24, 1, 0x0841, 2113},
        {0x4BE6297D, Operation::Vftoi4, "vftoi4", 0x12, 31, 6, 0x297d, 10621},
        {0x4BE1097D, Operation::Vftoi4, "vftoi4", 0x12, 31, 1, 0x097d, 2429},
        {0x4BE7333C, Operation::Vmove, "vmove", 0x12, 31, 7, 0x333c, 13116},
        {0x4BF8A33C, Operation::Vmove, "vmove", 0x12, 31, 24, 0xa33c, -23748},
        {0x4BE20BBC, Operation::Vdiv, "vdiv", 0x12, 31, 2, 0x0bbc, 3004},
        {0x4A6103BE, Operation::Vrsqrt, "vrsqrt", 0x12, 19, 1, 0x03be, 958},
        {0x4BC532FE, Operation::Vopmula, "vopmula", 0x12, 30, 5, 0x32fe, 13054},
        {0x4BE110F0, Operation::Viadd, "viadd", 0x12, 31, 1, 0x10f0, 4336},
        {0x4BE31432, Operation::Viaddi, "viaddi", 0x12, 31, 3, 0x1432, 5170},
        {0x4A0003BF, Operation::Vwaitq, "vwaitq", 0x12, 16, 0, 0x03bf, 959},
        // VCALLMS runs VU0 micro code, which this model does not have.
        {0x4A003838, Operation::Unsupported, "unsupported", 0x12, 16, 0, 0x3838, 14392},
        {0x68a30007, Operation::Ldl, "ldl", 0x1a, 5, 3, 0x0007, 7},
        {0x6c830007, Operation::Ldr, "ldr", 0x1b, 4, 3, 0x0007, 7},
        {0xb0830007, Operation::Sdl, "sdl", 0x2c, 4, 3, 0x0007, 7},
        {0xb4830000, Operation::Sdr, "sdr", 0x2d, 4, 3, 0x0000, 0},
        {0xcca00040, Operation::Pref, "pref", 0x33, 5, 0, 0x0040, 64},
        {0x5840000c, Operation::Blezl, "blezl", 0x16, 2, 0, 0x000c, 12},
        {0x5c400005, Operation::Bgtzl, "bgtzl", 0x17, 2, 0, 0x0005, 5},
        {0x66100001, Operation::Daddiu, "daddiu", 0x19, 16, 16, 0x0001, 1},
        {0x70000000, Operation::Madd, "madd", 0x1c, 0, 0, 0x0000, 0},
        {0x70000001, Operation::Maddu, "maddu", 0x1c, 0, 0, 0x0001, 1},
        {0x70000004, Operation::Plzcw, "plzcw", 0x1c, 0, 0, 0x0004, 4},
        {0x70000018, Operation::Mult1, "mult1", 0x1c, 0, 0, 0x0018, 24},
        {0x70000019, Operation::Multu1, "multu1", 0x1c, 0, 0, 0x0019, 25},
        {0x7000001a, Operation::Div1, "div1", 0x1c, 0, 0, 0x001a, 26},
        {0x7000001b, Operation::Divu1, "divu1", 0x1c, 0, 0, 0x001b, 27},
        {0x70000020, Operation::Madd1, "madd1", 0x1c, 0, 0, 0x0020, 32},
        {0x70000021, Operation::Maddu1, "maddu1", 0x1c, 0, 0, 0x0021, 33},
        // The trapping immediate forms and the parallel multiply/divide
        // family, including words observed in the pinned text (0x70421409,
        // 0x704E0709, 0x7181C329).
        {0x21280001, Operation::Addi, "addi", 0x08, 9, 8, 0x0001, 1},
        {0x61280001, Operation::Daddi, "daddi", 0x18, 9, 8, 0x0001, 1},
        {0x71095409, Operation::Pmaddh, "pmaddh", 0x1c, 8, 9, 0x5409, 21513},
        {0x70421409, Operation::Pmaddh, "pmaddh", 0x1c, 2, 2, 0x1409, 5129},
        {0x71095509, Operation::Pmsubh, "pmsubh", 0x1c, 8, 9, 0x5509, 21769},
        {0x71095709, Operation::Pmulth, "pmulth", 0x1c, 8, 9, 0x5709, 22281},
        {0x704E0709, Operation::Pmulth, "pmulth", 0x1c, 2, 14, 0x0709, 1801},
        {0x71095309, Operation::Pmultw, "pmultw", 0x1c, 8, 9, 0x5309, 21257},
        {0x71095329, Operation::Pmultuw, "pmultuw", 0x1c, 8, 9, 0x5329, 21289},
        {0x7181C329, Operation::Pmultuw, "pmultuw", 0x1c, 12, 1, 0xc329, -15575},
        {0x71095029, Operation::Pmadduw, "pmadduw", 0x1c, 8, 9, 0x5029, 20521},
        {0x71095349, Operation::Pdivw, "pdivw", 0x1c, 8, 9, 0x5349, 21321},
        {0x71095369, Operation::Pdivuw, "pdivuw", 0x1c, 8, 9, 0x5369, 21353},
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
        0x00000005, 0x70000002, 0x46800000, 0x42000001, 0x041a0000,
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
