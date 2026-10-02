#pragma once

#include <cstdint>
#include <span>
#include <string_view>

namespace gt4recomp::ee {

enum class Operation {
    Unsupported,
    // Register arithmetic and logic.
    Addu, Subu, And, Or, Xor, Slt, Sltu, Daddu,
    // Immediates, shifts and upper immediates.
    Addiu, Andi, Ori, Sll, Srl, Lui,
    // Memory access.
    Lh, Lw, Sw, Ld, Sd, Sb,
    // Relative branches; the REGIMM family compares rs against zero.
    Beq, Bne, Beql, Bnel, Blez, Bgtz,
    Bltz, Bgez, Bltzl, Bgezl, Bltzal, Bgezal, Bltzall, Bgezall,
    // Direct and register jumps.
    J, Jal, Jr, Jalr,
    // Exception boundary.
    Syscall
};

// These are overlapping views of the encoded bits, not a list of operands.
// For example, rd and shift_amount are not operands of an I-format instruction.
struct DecodedInstruction {
    Operation operation = Operation::Unsupported;
    std::uint32_t word = 0;
    std::uint8_t primary_opcode = 0;
    std::uint8_t rs = 0;
    std::uint8_t rt = 0;
    std::uint8_t rd = 0;
    std::uint8_t shift_amount = 0;
    std::uint8_t function = 0;
    std::uint16_t immediate = 0;
    std::uint32_t jump_index = 0;

    // Interpret the immediate as signed without narrowing to a signed 16-bit type.
    [[nodiscard]] std::int32_t signed_immediate() const;
};

// The fixed extent requires exactly four bytes. Callers check buffer bounds.
[[nodiscard]] std::uint32_t read_instruction_word(
    std::span<const std::uint8_t, 4> bytes);
// Branch target: PC+4 plus the signed immediate scaled by four, wrapped within
// the 32-bit guest address model.
[[nodiscard]] std::uint32_t relative_branch_target(
    std::uint32_t pc, const DecodedInstruction& instruction) noexcept;
// Jump target: the upper four bits of PC+4 combined with the 26-bit index.
[[nodiscard]] std::uint32_t absolute_jump_target(
    std::uint32_t pc, const DecodedInstruction& instruction) noexcept;
[[nodiscard]] DecodedInstruction decode(std::uint32_t word);
[[nodiscard]] std::string_view mnemonic(Operation operation);

} // namespace gt4recomp::ee
