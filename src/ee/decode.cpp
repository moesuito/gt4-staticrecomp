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
    case 0x38:
        return instruction.rs == 0 ? Operation::Dsll : Operation::Unsupported;
    case 0x3a:
        return instruction.rs == 0 ? Operation::Dsrl : Operation::Unsupported;
    case 0x3b:
        return instruction.rs == 0 ? Operation::Dsra : Operation::Unsupported;
    case 0x3c:
        return instruction.rs == 0 ? Operation::Dsll32 : Operation::Unsupported;
    case 0x3e:
        return instruction.rs == 0 ? Operation::Dsrl32 : Operation::Unsupported;
    case 0x3f:
        return instruction.rs == 0 ? Operation::Dsra32 : Operation::Unsupported;
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
    case 0x0d:
        // BREAK carries a code the same way; it traps like SYSCALL and the
        // handler is not modeled.
        return Operation::Break;
    case 0x0f:
        // SYNC is the pipeline barrier; its completion code in bits 10-6 does
        // not change the modeled effect, which is a no-op without caches or
        // pipeline stalls (the reference implementation does the same).
        return Operation::Sync;
    case 0x10: return Operation::Mfhi;
    case 0x11: return Operation::Mthi;
    case 0x12: return Operation::Mflo;
    case 0x13: return Operation::Mtlo;
    case 0x29: return Operation::Mtsa;
    default:
        break;
    }

    // Register ALU encodings in this subset require a zero shift field.
    if (instruction.shift_amount != 0) {
        return Operation::Unsupported;
    }
    switch (instruction.function) {
    case 0x04: return Operation::Sllv;
    case 0x06: return Operation::Srlv;
    case 0x07: return Operation::Srav;
    case 0x0a: return Operation::Movz;
    case 0x0b: return Operation::Movn;
    case 0x14: return Operation::Dsllv;
    case 0x16: return Operation::Dsrlv;
    case 0x17: return Operation::Dsrav;
    case 0x18: return Operation::Mult;
    case 0x19: return Operation::Multu;
    case 0x1a: return Operation::Div;
    case 0x1b: return Operation::Divu;
    case 0x21: return Operation::Addu;
    case 0x23: return Operation::Subu;
    case 0x24: return Operation::And;
    case 0x25: return Operation::Or;
    case 0x26: return Operation::Xor;
    case 0x27: return Operation::Nor;
    case 0x2a: return Operation::Slt;
    case 0x2b: return Operation::Sltu;
    case 0x2d: return Operation::Daddu;
    case 0x2f: return Operation::Dsubu;
    default: return Operation::Unsupported;
    }
}

Operation decode_mmi0(const DecodedInstruction& instruction) {
    switch (instruction.shift_amount) {
    case 0x00: return Operation::Paddw;
    case 0x01: return Operation::Psubw;
    case 0x02: return Operation::Pcgtw;
    case 0x03: return Operation::Pmaxw;
    case 0x04: return Operation::Paddh;
    case 0x05: return Operation::Psubh;
    case 0x06: return Operation::Pcgth;
    case 0x07: return Operation::Pmaxh;
    case 0x08: return Operation::Paddb;
    case 0x09: return Operation::Psubb;
    case 0x0a: return Operation::Pcgtb;
    case 0x10: return Operation::Paddsw;
    case 0x11: return Operation::Psubsw;
    case 0x12: return Operation::Pextlw;
    case 0x13: return Operation::Ppacw;
    case 0x14: return Operation::Paddsh;
    case 0x15: return Operation::Psubsh;
    case 0x16: return Operation::Pextlh;
    case 0x17: return Operation::Ppach;
    case 0x18: return Operation::Paddsb;
    case 0x19: return Operation::Psubsb;
    case 0x1a: return Operation::Pextlb;
    case 0x1b: return Operation::Ppacb;
    case 0x1e: return Operation::Pext5;
    case 0x1f: return Operation::Ppac5;
    default: return Operation::Unsupported;
    }
}

Operation decode_mmi1(const DecodedInstruction& instruction) {
    switch (instruction.shift_amount) {
    case 0x01: return Operation::Pabsw;
    case 0x02: return Operation::Pceqw;
    case 0x03: return Operation::Pminw;
    case 0x04: return Operation::Padsbh;
    case 0x05: return Operation::Pabsh;
    case 0x06: return Operation::Pceqh;
    case 0x07: return Operation::Pminh;
    case 0x0a: return Operation::Pceqb;
    case 0x10: return Operation::Padduw;
    case 0x11: return Operation::Psubuw;
    case 0x12: return Operation::Pextuw;
    case 0x14: return Operation::Padduh;
    case 0x15: return Operation::Psubuh;
    case 0x16: return Operation::Pextuh;
    case 0x18: return Operation::Paddub;
    case 0x19: return Operation::Psubub;
    case 0x1a: return Operation::Pextub;
    case 0x1b: return Operation::Qfsrv;
    default: return Operation::Unsupported;
    }
}

Operation decode_mmi2(const DecodedInstruction& instruction) {
    switch (instruction.shift_amount) {
    case 0x02: return Operation::Psllvw;
    case 0x03: return Operation::Psrlvw;
    case 0x08: return Operation::Pmfhi;
    case 0x09: return Operation::Pmflo;
    case 0x0a: return Operation::Pinth;
    case 0x0e: return Operation::Pcpyld;
    case 0x12: return Operation::Pand;
    case 0x13: return Operation::Pxor;
    case 0x1a: return Operation::Pexeh;
    case 0x1b: return Operation::Prevh;
    case 0x1e: return Operation::Pexew;
    case 0x1f: return Operation::Prot3w;
    default: return Operation::Unsupported;
    }
}

Operation decode_mmi3(const DecodedInstruction& instruction) {
    switch (instruction.shift_amount) {
    case 0x03: return Operation::Psravw;
    case 0x08: return Operation::Pmthi;
    case 0x09: return Operation::Pmtlo;
    case 0x0a: return Operation::Pinteh;
    case 0x0e: return Operation::Pcpyud;
    case 0x12: return Operation::Por;
    case 0x13: return Operation::Pnor;
    case 0x1a: return Operation::Pexch;
    case 0x1b: return Operation::Pcpyh;
    case 0x1e: return Operation::Pexcw;
    default: return Operation::Unsupported;
    }
}

// MMI selects one of four 32-entry tables in the function field; the table
// index is the five-bit field at bits 10-6, the same bits that hold shift
// amounts elsewhere in the encoding.
Operation decode_mmi(const DecodedInstruction& instruction) {
    switch (instruction.function) {
    case 0x00: return Operation::Madd;
    case 0x01: return Operation::Maddu;
    case 0x04: return Operation::Plzcw;
    case 0x08: return decode_mmi0(instruction);
    case 0x09: return decode_mmi2(instruction);
    case 0x28: return decode_mmi1(instruction);
    case 0x29: return decode_mmi3(instruction);
    case 0x10: return Operation::Mfhi1;
    case 0x11: return Operation::Mthi1;
    case 0x12: return Operation::Mflo1;
    case 0x13: return Operation::Mtlo1;
    case 0x18: return Operation::Mult1;
    case 0x19: return Operation::Multu1;
    case 0x1a: return Operation::Div1;
    case 0x1b: return Operation::Divu1;
    case 0x20: return Operation::Madd1;
    case 0x21: return Operation::Maddu1;
    case 0x30: return Operation::Pmfhl;
    case 0x31: return Operation::Pmthl;
    case 0x34: return Operation::Psllh;
    case 0x36: return Operation::Psrlh;
    case 0x37: return Operation::Psrah;
    case 0x3c: return Operation::Psllw;
    case 0x3e: return Operation::Psrlw;
    case 0x3f: return Operation::Psraw;
    default: return Operation::Unsupported;
    }
}

Operation decode_cop0(const DecodedInstruction& instruction) {
    switch (instruction.rs) {
    case 0x00: return Operation::Mfc0;
    case 0x04: return Operation::Mtc0;
    case 0x10:
        // The C0 function field holds the control operations.
        switch (instruction.function) {
        case 0x18: return Operation::Eret;
        case 0x38: return Operation::Ei;
        case 0x39: return Operation::Di;
        default: return Operation::Unsupported;
        }
    default:
        return Operation::Unsupported;
    }
}

Operation decode_cop2_special(const DecodedInstruction& instruction) {
    // The VU macro encoding: functions 0x00-0x3B dispatch through the
    // standard table (the arithmetic land in later slices); 0x3C-0x3F go
    // through the packed secondary index the reference computes from bits
    // 1-0 and 9-4 of the word.
    if (instruction.function < 0x3c) {
        return Operation::Unsupported;
    }
    const std::uint32_t index =
        (instruction.word & 0x3u) | ((instruction.word >> 4) & 0x7cu);
    if (index == 47) {
        return Operation::Vnop;  // the table's full no-operation
    }
    return Operation::Unsupported;
}

Operation decode_cop2(const DecodedInstruction& instruction) {
    switch (instruction.rs) {
    case 0x01: return Operation::Qmfc2;
    case 0x02: return Operation::Cfc2;
    case 0x05: return Operation::Qmtc2;
    case 0x06: return Operation::Ctc2;
    case 0x10: case 0x11: case 0x12: case 0x13:
    case 0x14: case 0x15: case 0x16: case 0x17:
    case 0x18: case 0x19: case 0x1a: case 0x1b:
    case 0x1c: case 0x1d: case 0x1e: case 0x1f:
        return decode_cop2_special(instruction);
    default:
        return Operation::Unsupported;
    }
}

Operation decode_cop1(const DecodedInstruction& instruction) {
    switch (instruction.rs) {
    case 0x00: return Operation::Mfc1;
    case 0x02: return Operation::Cfc1;
    case 0x04: return Operation::Mtc1;
    case 0x06: return Operation::Ctc1;
    case 0x08:
        switch (instruction.rt) {
        case 0x00: return Operation::Bc1f;
        case 0x01: return Operation::Bc1t;
        case 0x02: return Operation::Bc1fl;
        case 0x03: return Operation::Bc1tl;
        default: return Operation::Unsupported;
        }
    case 0x10:
        // Single-precision arithmetic, selected by the six-bit function field.
        switch (instruction.function) {
        case 0x00: return Operation::AddS;
        case 0x01: return Operation::SubS;
        case 0x02: return Operation::MulS;
        case 0x03: return Operation::DivS;
        case 0x04: return Operation::SqrtS;
        case 0x05: return Operation::AbsS;
        case 0x06: return Operation::MovS;
        case 0x07: return Operation::NegS;
        case 0x16: return Operation::RsqrtS;
        case 0x18: return Operation::AddaS;
        case 0x19: return Operation::SubaS;
        case 0x1a: return Operation::MulaS;
        case 0x1c: return Operation::MaddS;
        case 0x1d: return Operation::MsubS;
        case 0x1e: return Operation::MaddaS;
        case 0x1f: return Operation::MsubaS;
        case 0x24: return Operation::CvtW;
        case 0x28: return Operation::MaxS;
        case 0x29: return Operation::MinS;
        case 0x30: return Operation::CF;
        case 0x32: return Operation::CEq;
        case 0x34: return Operation::CLt;
        case 0x36: return Operation::CLe;
        default: return Operation::Unsupported;
        }
    case 0x14:
        // cvt.s.w is the only word-format conversion in the subset.
        return instruction.function == 0x20 ? Operation::CvtS : Operation::Unsupported;
    default:
        return Operation::Unsupported;
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
        case 0x18: result.operation = Operation::Mtsab; break;
        case 0x19: result.operation = Operation::Mtsah; break;
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
    case 0x16: result.operation = Operation::Blezl; break;
    case 0x07:
        if (result.rt == 0) {
            result.operation = Operation::Bgtz;
        }
        break;
    case 0x17: result.operation = Operation::Bgtzl; break;
    case 0x19: result.operation = Operation::Daddiu; break;
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
    case 0x11: result.operation = decode_cop1(result); break;
    case 0x10: result.operation = decode_cop0(result); break;
    case 0x1c: result.operation = decode_mmi(result); break;
    case 0x1e: result.operation = Operation::Lq; break;
    case 0x1f: result.operation = Operation::Sq; break;
    case 0x1a: result.operation = Operation::Ldl; break;
    case 0x1b: result.operation = Operation::Ldr; break;
    case 0x14: result.operation = Operation::Beql; break;
    case 0x15: result.operation = Operation::Bnel; break;
    case 0x20: result.operation = Operation::Lb; break;
    case 0x21: result.operation = Operation::Lh; break;
    case 0x22: result.operation = Operation::Lwl; break;
    case 0x23: result.operation = Operation::Lw; break;
    case 0x24: result.operation = Operation::Lbu; break;
    case 0x25: result.operation = Operation::Lhu; break;
    case 0x26: result.operation = Operation::Lwr; break;
    case 0x27: result.operation = Operation::Lwu; break;
    case 0x28: result.operation = Operation::Sb; break;
    case 0x29: result.operation = Operation::Sh; break;
    case 0x2a: result.operation = Operation::Swl; break;
    case 0x2b: result.operation = Operation::Sw; break;
    case 0x2e: result.operation = Operation::Swr; break;
    case 0x2c: result.operation = Operation::Sdl; break;
    case 0x2d: result.operation = Operation::Sdr; break;
    case 0x31: result.operation = Operation::Lwc1; break;
    case 0x36: result.operation = Operation::Lqc2; break;
    case 0x3e: result.operation = Operation::Sqc2; break;
    case 0x12: result.operation = decode_cop2(result); break;
    case 0x2f: result.operation = Operation::Cache; break;
    case 0x33: result.operation = Operation::Pref; break;
    case 0x37: result.operation = Operation::Ld; break;
    case 0x39: result.operation = Operation::Swc1; break;
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
    case Operation::Sllv: return "sllv";
    case Operation::Srlv: return "srlv";
    case Operation::Srav: return "srav";
    case Operation::Dsll: return "dsll";
    case Operation::Dsrl: return "dsrl";
    case Operation::Dsra: return "dsra";
    case Operation::Dsll32: return "dsll32";
    case Operation::Dsrl32: return "dsrl32";
    case Operation::Dsra32: return "dsra32";
    case Operation::Dsllv: return "dsllv";
    case Operation::Dsrlv: return "dsrlv";
    case Operation::Dsrav: return "dsrav";
    case Operation::Slt: return "slt";
    case Operation::Sltu: return "sltu";
    case Operation::Slti: return "slti";
    case Operation::Sltiu: return "sltiu";
    case Operation::Xori: return "xori";
    case Operation::Lb: return "lb";
    case Operation::Lbu: return "lbu";
    case Operation::Daddu: return "daddu";
    case Operation::Dsubu: return "dsubu";
    case Operation::Movz: return "movz";
    case Operation::Movn: return "movn";
    case Operation::Lh: return "lh";
    case Operation::Sb: return "sb";
    case Operation::Ld: return "ld";
    case Operation::Sd: return "sd";
    case Operation::Lq: return "lq";
    case Operation::Sq: return "sq";
    case Operation::Ldl: return "ldl";
    case Operation::Ldr: return "ldr";
    case Operation::Sdl: return "sdl";
    case Operation::Sdr: return "sdr";
    case Operation::Cache: return "cache";
    case Operation::Pref: return "pref";
    case Operation::Lhu: return "lhu";
    case Operation::Lwu: return "lwu";
    case Operation::Sh: return "sh";
    case Operation::Lwl: return "lwl";
    case Operation::Lwr: return "lwr";
    case Operation::Swl: return "swl";
    case Operation::Swr: return "swr";
    case Operation::Plzcw: return "plzcw";
    case Operation::Mult: return "mult";
    case Operation::Multu: return "multu";
    case Operation::Div: return "div";
    case Operation::Divu: return "divu";
    case Operation::Madd: return "madd";
    case Operation::Maddu: return "maddu";
    case Operation::Mult1: return "mult1";
    case Operation::Multu1: return "multu1";
    case Operation::Div1: return "div1";
    case Operation::Divu1: return "divu1";
    case Operation::Madd1: return "madd1";
    case Operation::Maddu1: return "maddu1";
    case Operation::Beql: return "beql";
    case Operation::Bnel: return "bnel";
    case Operation::Blez: return "blez";
    case Operation::Bgtz: return "bgtz";
    case Operation::Blezl: return "blezl";
    case Operation::Bgtzl: return "bgtzl";
    case Operation::Daddiu: return "daddiu";
    case Operation::Nor: return "nor";
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
    case Operation::Break: return "break";
    case Operation::Mfc0: return "mfc0";
    case Operation::Mtc0: return "mtc0";
    case Operation::Ei: return "ei";
    case Operation::Di: return "di";
    case Operation::Eret: return "eret";
    case Operation::Qmfc2: return "qmfc2";
    case Operation::Qmtc2: return "qmtc2";
    case Operation::Cfc2: return "cfc2";
    case Operation::Ctc2: return "ctc2";
    case Operation::Lqc2: return "lqc2";
    case Operation::Sqc2: return "sqc2";
    case Operation::Vnop: return "vnop";
    case Operation::Mfhi: return "mfhi";
    case Operation::Mthi: return "mthi";
    case Operation::Mflo: return "mflo";
    case Operation::Mtlo: return "mtlo";
    case Operation::Sync: return "sync";
    case Operation::Mfhi1: return "mfhi1";
    case Operation::Mthi1: return "mthi1";
    case Operation::Mflo1: return "mflo1";
    case Operation::Mtlo1: return "mtlo1";
    case Operation::Mtsa: return "mtsa";
    case Operation::Mtsab: return "mtsab";
    case Operation::Mtsah: return "mtsah";
    case Operation::Mfc1: return "mfc1";
    case Operation::Cfc1: return "cfc1";
    case Operation::Mtc1: return "mtc1";
    case Operation::Ctc1: return "ctc1";
    case Operation::Lwc1: return "lwc1";
    case Operation::Swc1: return "swc1";
    case Operation::AddS: return "add.s";
    case Operation::SubS: return "sub.s";
    case Operation::MulS: return "mul.s";
    case Operation::DivS: return "div.s";
    case Operation::SqrtS: return "sqrt.s";
    case Operation::AbsS: return "abs.s";
    case Operation::MovS: return "mov.s";
    case Operation::NegS: return "neg.s";
    case Operation::MaxS: return "max.s";
    case Operation::MinS: return "min.s";
    case Operation::RsqrtS: return "rsqrt.s";
    case Operation::AddaS: return "adda.s";
    case Operation::SubaS: return "suba.s";
    case Operation::MulaS: return "mula.s";
    case Operation::MaddaS: return "madda.s";
    case Operation::MsubaS: return "msuba.s";
    case Operation::MaddS: return "madd.s";
    case Operation::MsubS: return "msub.s";
    case Operation::CF: return "c.f";
    case Operation::CEq: return "c.eq";
    case Operation::CLt: return "c.lt";
    case Operation::CLe: return "c.le";
    case Operation::CvtS: return "cvt.s.w";
    case Operation::CvtW: return "cvt.w.s";
    case Operation::Bc1f: return "bc1f";
    case Operation::Bc1t: return "bc1t";
    case Operation::Bc1fl: return "bc1fl";
    case Operation::Bc1tl: return "bc1tl";
    case Operation::Paddw: return "paddw";
    case Operation::Psubw: return "psubw";
    case Operation::Paddh: return "paddh";
    case Operation::Psubh: return "psubh";
    case Operation::Paddb: return "paddb";
    case Operation::Psubb: return "psubb";
    case Operation::Paddsw: return "paddsw";
    case Operation::Psubsw: return "psubsw";
    case Operation::Paddsh: return "paddsh";
    case Operation::Psubsh: return "psubsh";
    case Operation::Paddsb: return "paddsb";
    case Operation::Psubsb: return "psubsb";
    case Operation::Padduw: return "padduw";
    case Operation::Psubuw: return "psubuw";
    case Operation::Padduh: return "padduh";
    case Operation::Psubuh: return "psubuh";
    case Operation::Paddub: return "paddub";
    case Operation::Psubub: return "psubub";
    case Operation::Pcgtw: return "pcgtw";
    case Operation::Pcgth: return "pcgth";
    case Operation::Pcgtb: return "pcgtb";
    case Operation::Pceqw: return "pceqw";
    case Operation::Pceqh: return "pceqh";
    case Operation::Pceqb: return "pceqb";
    case Operation::Pmaxw: return "pmaxw";
    case Operation::Pmaxh: return "pmaxh";
    case Operation::Pminw: return "pminw";
    case Operation::Pminh: return "pminh";
    case Operation::Pabsw: return "pabsw";
    case Operation::Pabsh: return "pabsh";
    case Operation::Pand: return "pand";
    case Operation::Por: return "por";
    case Operation::Pxor: return "pxor";
    case Operation::Pnor: return "pnor";
    case Operation::Psllh: return "psllh";
    case Operation::Psrlh: return "psrlh";
    case Operation::Psrah: return "psrah";
    case Operation::Psllw: return "psllw";
    case Operation::Psrlw: return "psrlw";
    case Operation::Psraw: return "psraw";
    case Operation::Psllvw: return "psllvw";
    case Operation::Psrlvw: return "psrlvw";
    case Operation::Psravw: return "psravw";
    case Operation::Pextlw: return "pextlw";
    case Operation::Pextlh: return "pextlh";
    case Operation::Pextlb: return "pextlb";
    case Operation::Pextuw: return "pextuw";
    case Operation::Pextuh: return "pextuh";
    case Operation::Pextub: return "pextub";
    case Operation::Ppacw: return "ppacw";
    case Operation::Ppach: return "ppach";
    case Operation::Ppacb: return "ppacb";
    case Operation::Pext5: return "pext5";
    case Operation::Ppac5: return "ppac5";
    case Operation::Padsbh: return "padsbh";
    case Operation::Pinth: return "pinth";
    case Operation::Pinteh: return "pinteh";
    case Operation::Pcpyld: return "pcpyld";
    case Operation::Pcpyud: return "pcpyud";
    case Operation::Pcpyh: return "pcpyh";
    case Operation::Pexeh: return "pexeh";
    case Operation::Prevh: return "prevh";
    case Operation::Pexew: return "pexew";
    case Operation::Pexch: return "pexch";
    case Operation::Pexcw: return "pexcw";
    case Operation::Prot3w: return "prot3w";
    case Operation::Pmfhi: return "pmfhi";
    case Operation::Pmflo: return "pmflo";
    case Operation::Pmthi: return "pmthi";
    case Operation::Pmtlo: return "pmtlo";
    case Operation::Pmfhl: return "pmfhl";
    case Operation::Pmthl: return "pmthl";
    case Operation::Qfsrv: return "qfsrv";
    case Operation::Unsupported: return "unsupported";
    }
    return "unsupported";
}

} // namespace gt4recomp::ee
