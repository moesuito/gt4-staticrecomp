#pragma once

#include <cstdint>
#include <span>
#include <string_view>

namespace gt4recomp::ee {

enum class Operation {
    Unsupported,
    // Register arithmetic and logic.
    Addu, Subu, And, Or, Xor, Nor, Slt, Sltu, Daddu, Dsubu, Movz, Movn,
    // Immediates, shifts and upper immediates.
    Addiu, Daddiu, Andi, Ori, Xori, Slti, Sltiu, Sll, Srl, Sra, Lui,
    Sllv, Srlv, Srav, Dsll, Dsrl, Dsra, Dsll32, Dsrl32, Dsra32, Dsllv, Dsrlv, Dsrav,
    // Memory access.
    Lb, Lbu, Lh, Lhu, Lw, Lwu, Sw, Sh, Ld, Sd, Sb, Lq, Sq,
    Lwl, Lwr, Swl, Swr, Ldl, Ldr, Sdl, Sdr,
    // The cache hint and the prefetch hint: decoded so real code flows past
    // them; both have no effect in this model, like the reference.
    Cache, Pref,
    // Relative branches; the REGIMM family compares rs against zero.
    Beq, Bne, Beql, Bnel, Blez, Bgtz, Blezl, Bgtzl,
    Bltz, Bgez, Bltzl, Bgezl, Bltzal, Bgezal, Bltzall, Bgezall,
    // Direct and register jumps.
    J, Jal, Jr, Jalr,
    // Exception boundary.
    Syscall, Break,
    // Special register moves, synchronization and the MMI shift cache.
    Mfhi, Mthi, Mflo, Mtlo, Sync, Mfhi1, Mthi1, Mflo1, Mtlo1, Mtsa, Mtsab, Mtsah,
    // Multiply and divide, including the second HI/LO bank used by the MMI
    // compact forms and the accumulate variants.
    Mult, Multu, Div, Divu, Madd, Maddu, Mult1, Multu1, Div1, Divu1, Madd1, Maddu1,
    // CP0: system-coprocessor moves and the interrupt-enable pair.
    Mfc0, Mtc0, Ei, Di, Eret,
    // COP1: register moves, FPU memory access, single-precision arithmetic,
    // accumulator forms, comparisons, conversions and conditional branches.
    Mfc1, Cfc1, Mtc1, Ctc1, Lwc1, Swc1,
    AddS, SubS, MulS, DivS, SqrtS, AbsS, MovS, NegS, MaxS, MinS, RsqrtS,
    AddaS, SubaS, MulaS, MaddaS, MsubaS, MaddS, MsubS,
    CF, CEq, CLt, CLe, CvtS, CvtW,
    Bc1f, Bc1t, Bc1fl, Bc1tl,
    // MMI: parallel lane arithmetic and logic.
    Paddw, Psubw, Paddh, Psubh, Paddb, Psubb,
    Paddsw, Psubsw, Paddsh, Psubsh, Paddsb, Psubsb,
    Padduw, Psubuw, Padduh, Psubuh, Paddub, Psubub,
    Pcgtw, Pcgth, Pcgtb, Pceqw, Pceqh, Pceqb,
    Pmaxw, Pmaxh, Pminw, Pminh, Pabsw, Pabsh,
    Pand, Por, Pxor, Pnor,
    Psllh, Psrlh, Psrah, Psllw, Psrlw, Psraw, Psllvw, Psrlvw, Psravw,
    Pextlw, Pextlh, Pextlb, Pextuw, Pextuh, Pextub,
    Ppacw, Ppach, Ppacb, Pext5, Ppac5, Padsbh,
    Pinth, Pinteh, Pcpyld, Pcpyud, Pcpyh, Pexeh, Prevh, Pexew, Pexch, Pexcw, Prot3w,
    // MMI: HI/LO moves and the state-backed shift.
    Pmfhi, Pmflo, Pmthi, Pmtlo, Pmfhl, Pmthl, Qfsrv, Plzcw
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

    // COP1 names the same bit ranges differently: fs is bits 15-11, ft is bits
    // 20-16 and fd is bits 10-6. MMI uses bits 10-6 as its group index, which
    // is why that field is shared between shift amounts and MMI selection.
    [[nodiscard]] std::uint8_t cop1_fs() const { return rd; }
    [[nodiscard]] std::uint8_t cop1_ft() const { return rt; }
    [[nodiscard]] std::uint8_t cop1_fd() const { return shift_amount; }

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
