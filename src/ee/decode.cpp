#include "gt4recomp/ee_decode.hpp"

namespace gt4recomp::ee {
namespace {

Operation decode_special(const DecodedInstruction& instruction) {
    switch (instruction.function) {
    case 0x00:
        return instruction.rs == 0 ? Operation::Sll : Operation::Unsupported;
    case 0x02:
        return instruction.rs == 0 ? Operation::Srl : Operation::Unsupported;
    case 0x03:
        return instruction.rs == 0 ? Operation::Sra : Operation::Unsupported;
    case 0x08:
        if (instruction.rt == 0 && instruction.rd == 0 && instruction.shift_amount == 0) {
            return Operation::Jr;
        }
        return Operation::Unsupported;
    case 0x09:
        // JALR selects its link register in rd; rt and bits 10-6 are fixed zero.
        if (instruction.rt == 0 && instruction.shift_amount == 0) {
            return Operation::Jalr;
        }
        return Operation::Unsupported;
    case 0x0c:
        // The 20-bit SYSCALL code occupies bits 25-6, so no zero-shift rule.
        return Operation::Syscall;
    default:
        break;
    }

    // Register ALU encodings in this subset require a zero shift field.
    if (instruction.shift_amount != 0) {
        return Operation::Unsupported;
    }
    switch (instruction.function) {
    case 0x21: return Operation::Addu;
    case 0x23: return Operation::Subu;
    case 0x24: return Operation::And;
    case 0x25: return Operation::Or;
    case 0x26: return Operation::Xor;
    case 0x2a: return Operation::Slt;
    case 0x2b: return Operation::Sltu;
    case 0x2d: return Operation::Daddu;
    default: return Operation::Unsupported;
    }
}

} // namespace

std::int32_t DecodedInstruction::signed_immediate() const {
    const auto value = static_cast<std::int32_t>(immediate);
    return immediate >= 0x8000 ? value - 0x10000 : value;
}

std::uint32_t relative_branch_target(std::uint32_t pc,
                                     const DecodedInstruction& instruction) noexcept {
    // Multiplication, not shifting a negative signed value; the conversion back
    // to uint32_t intentionally wraps within the guest address model.
    const std::int64_t target = static_cast<std::int64_t>(pc) + 4
        + static_cast<std::int64_t>(instruction.signed_immediate()) * 4;
    return static_cast<std::uint32_t>(target);
}

std::uint32_t absolute_jump_target(std::uint32_t pc,
                                   const DecodedInstruction& instruction) noexcept {
    const std::uint32_t next_pc = pc + 4;
    return (next_pc & 0xf0000000u) | (instruction.jump_index << 2);
}

std::uint32_t read_instruction_word(std::span<const std::uint8_t, 4> bytes) {
    // Promote before shifting: the top byte must remain unsigned.
    return static_cast<std::uint32_t>(bytes[0])
        | (static_cast<std::uint32_t>(bytes[1]) << 8)
        | (static_cast<std::uint32_t>(bytes[2]) << 16)
        | (static_cast<std::uint32_t>(bytes[3]) << 24);
}

DecodedInstruction decode(std::uint32_t word) {
    DecodedInstruction result;
    result.word = word;
    result.primary_opcode = static_cast<std::uint8_t>((word >> 26) & 0x3f);
    result.rs = static_cast<std::uint8_t>((word >> 21) & 0x1f);
    result.rt = static_cast<std::uint8_t>((word >> 16) & 0x1f);
    result.rd = static_cast<std::uint8_t>((word >> 11) & 0x1f);
    result.shift_amount = static_cast<std::uint8_t>((word >> 6) & 0x1f);
    result.function = static_cast<std::uint8_t>(word & 0x3f);
    result.immediate = static_cast<std::uint16_t>(word & 0xffff);
    result.jump_index = word & 0x03ffffff;

    switch (result.primary_opcode) {
    case 0x00: result.operation = decode_special(result); break;
    case 0x01:
        // REGIMM: one branch family, selected by rt, comparing rs against zero.
        switch (result.rt) {
        case 0x00: result.operation = Operation::Bltz; break;
        case 0x01: result.operation = Operation::Bgez; break;
        case 0x02: result.operation = Operation::Bltzl; break;
        case 0x03: result.operation = Operation::Bgezl; break;
        case 0x10: result.operation = Operation::Bltzal; break;
        case 0x11: result.operation = Operation::Bgezal; break;
        case 0x12: result.operation = Operation::Bltzall; break;
        case 0x13: result.operation = Operation::Bgezall; break;
        default: break;
        }
        break;
    case 0x02: result.operation = Operation::J; break;
    case 0x03: result.operation = Operation::Jal; break;
    case 0x04: result.operation = Operation::Beq; break;
    case 0x05: result.operation = Operation::Bne; break;
    case 0x06:
        if (result.rt == 0) {
            result.operation = Operation::Blez;
        }
        break;
    case 0x07:
        if (result.rt == 0) {
            result.operation = Operation::Bgtz;
        }
        break;
    case 0x09: result.operation = Operation::Addiu; break;
    case 0x0a: result.operation = Operation::Slti; break;
    case 0x0b: result.operation = Operation::Sltiu; break;
    case 0x0c: result.operation = Operation::Andi; break;
    case 0x0d: result.operation = Operation::Ori; break;
    case 0x0e: result.operation = Operation::Xori; break;
    case 0x0f:
        if (result.rs == 0) {
            result.operation = Operation::Lui;
        }
        break;
    case 0x14: result.operation = Operation::Beql; break;
    case 0x15: result.operation = Operation::Bnel; break;
    case 0x20: result.operation = Operation::Lb; break;
    case 0x21: result.operation = Operation::Lh; break;
    case 0x23: result.operation = Operation::Lw; break;
    case 0x24: result.operation = Operation::Lbu; break;
    case 0x28: result.operation = Operation::Sb; break;
    case 0x2b: result.operation = Operation::Sw; break;
    case 0x37: result.operation = Operation::Ld; break;
    case 0x3f: result.operation = Operation::Sd; break;
    default: break; // Preserve the original word and fields for diagnostics.
    }
    return result;
}

std::string_view mnemonic(Operation operation) {
    switch (operation) {
    case Operation::Addiu: return "addiu";
    case Operation::Andi: return "andi";
    case Operation::Ori: return "ori";
    case Operation::Addu: return "addu";
    case Operation::Subu: return "subu";
    case Operation::And: return "and";
    case Operation::Or: return "or";
    case Operation::Xor: return "xor";
    case Operation::Lui: return "lui";
    case Operation::Lw: return "lw";
    case Operation::Sw: return "sw";
    case Operation::Beq: return "beq";
    case Operation::Bne: return "bne";
    case Operation::J: return "j";
    case Operation::Jal: return "jal";
    case Operation::Jr: return "jr";
    case Operation::Sll: return "sll";
    case Operation::Srl: return "srl";
    case Operation::Sra: return "sra";
    case Operation::Slt: return "slt";
    case Operation::Sltu: return "sltu";
    case Operation::Slti: return "slti";
    case Operation::Sltiu: return "sltiu";
    case Operation::Xori: return "xori";
    case Operation::Lb: return "lb";
    case Operation::Lbu: return "lbu";
    case Operation::Daddu: return "daddu";
    case Operation::Lh: return "lh";
    case Operation::Sb: return "sb";
    case Operation::Ld: return "ld";
    case Operation::Sd: return "sd";
    case Operation::Beql: return "beql";
    case Operation::Bnel: return "bnel";
    case Operation::Blez: return "blez";
    case Operation::Bgtz: return "bgtz";
    case Operation::Bltz: return "bltz";
    case Operation::Bgez: return "bgez";
    case Operation::Bltzl: return "bltzl";
    case Operation::Bgezl: return "bgezl";
    case Operation::Bltzal: return "bltzal";
    case Operation::Bgezal: return "bgezal";
    case Operation::Bltzall: return "bltzall";
    case Operation::Bgezall: return "bgezall";
    case Operation::Jalr: return "jalr";
    case Operation::Syscall: return "syscall";
    case Operation::Unsupported: return "unsupported";
    }
    return "unsupported";
}

} // namespace gt4recomp::ee
