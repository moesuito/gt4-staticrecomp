#include "gt4recomp/ee_disassemble.hpp"
#include "gt4recomp/ee_decode.hpp"

#include <array>
#include <iomanip>
#include <map>
#include <ostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace gt4recomp::ee {
namespace {

constexpr std::array<std::string_view, 32> register_names = {
    "zero", "at", "v0", "v1", "a0", "a1", "a2", "a3",
    "t0", "t1", "t2", "t3", "t4", "t5", "t6", "t7",
    "s0", "s1", "s2", "s3", "s4", "s5", "s6", "s7",
    "t8", "t9", "k0", "k1", "gp", "sp", "fp", "ra"
};

std::string float_register(std::uint8_t index) {
    return "f" + std::to_string(index);
}

// CP0 registers have architectural names; unknown numbers fall back to their
// index so a listing stays unambiguous.
std::string cp0_register(std::uint8_t index) {
    switch (index) {
    case 0: return "Index";
    case 1: return "Random";
    case 2: return "EntryLo0";
    case 3: return "EntryLo1";
    case 4: return "Context";
    case 5: return "PageMask";
    case 6: return "Wired";
    case 8: return "BadVAddr";
    case 9: return "Count";
    case 10: return "EntryHi";
    case 11: return "Compare";
    case 12: return "Status";
    case 13: return "Cause";
    case 14: return "EPC";
    case 15: return "PRId";
    case 16: return "Config";
    case 17: return "LLAddr";
    case 18: return "WatchLO";
    case 19: return "WatchHI";
    case 24: return "Debug";
    case 25: return "PCCR";
    case 26: return "DEPC";
    case 27: return "PerfCnt";
    case 28: return "ErrCtl";
    case 29: return "CacheErr";
    case 30: return "ErrorEPC";
    case 31: return "DESAVE";
    default: return "cp0_" + std::to_string(index);
    }
}

std::string hex_value(std::uint32_t value, int width = 0) {
    std::ostringstream output;
    output << std::hex << std::setfill('0') << std::setw(width) << value;
    return output.str();
}

std::string signed_hex(std::int32_t value) {
    if (value < 0) {
        // Widen before negation so even INT32_MIN has a representable magnitude.
        const auto magnitude = static_cast<std::uint32_t>(-static_cast<std::int64_t>(value));
        return "-0x" + hex_value(magnitude);
    }
    return "0x" + hex_value(static_cast<std::uint32_t>(value));
}

std::string target_text(std::uint32_t target) {
    return "0x" + hex_value(target, 8);
}

std::string unsupported_family(const DecodedInstruction& instruction) {
    std::string family = "opcode=0x" + hex_value(instruction.primary_opcode, 2);
    if (instruction.primary_opcode == 0 || instruction.primary_opcode == 0x1c) {
        family += " function=0x" + hex_value(instruction.function, 2);
    } else if (instruction.primary_opcode == 1) {
        family += " rt=0x" + hex_value(instruction.rt, 2);
    } else if (instruction.primary_opcode >= 0x10 && instruction.primary_opcode <= 0x12) {
        family += " rs=0x" + hex_value(instruction.rs, 2);
    }
    return family;
}

} // namespace

std::string format_instruction(std::uint32_t word, std::uint32_t pc) {
    const auto instruction = decode(word);
    std::ostringstream output;
    output << mnemonic(instruction.operation);
    const auto rs = register_names[instruction.rs];
    const auto rt = register_names[instruction.rt];
    const auto rd = register_names[instruction.rd];
    switch (instruction.operation) {
    case Operation::Addu:
    case Operation::Subu:
    case Operation::And:
    case Operation::Or:
    case Operation::Xor:
    case Operation::Slt:
    case Operation::Sltu:
    case Operation::Daddu:
    case Operation::Movz:
    case Operation::Movn:
        output << ' ' << rd << ", " << rs << ", " << rt;
        break;
    case Operation::Addiu:
    case Operation::Slti:
    case Operation::Sltiu:
        output << ' ' << rt << ", " << rs << ", " << signed_hex(instruction.signed_immediate());
        break;
    case Operation::Lui:
        output << ' ' << rt << ", 0x" << hex_value(instruction.immediate);
        break;
    case Operation::Andi:
    case Operation::Ori:
    case Operation::Xori:
        output << ' ' << rt << ", " << rs << ", 0x" << hex_value(instruction.immediate);
        break;
    case Operation::Lb:
    case Operation::Lbu:
    case Operation::Lh:
    case Operation::Lhu:
    case Operation::Lw:
    case Operation::Lwu:
    case Operation::Sw:
    case Operation::Sh:
    case Operation::Ld:
    case Operation::Sd:
    case Operation::Sb:
    case Operation::Lq:
    case Operation::Sq:
    case Operation::Lwl:
    case Operation::Lwr:
    case Operation::Swl:
    case Operation::Swr:
        output << ' ' << rt << ", " << signed_hex(instruction.signed_immediate()) << '(' << rs << ')';
        break;
    case Operation::Cache:
        // The operation code occupies the rt field for the cache hint.
        output << " 0x" << hex_value(instruction.rt, 2) << ", "
               << signed_hex(instruction.signed_immediate()) << '(' << rs << ')';
        break;
    case Operation::Beq:
    case Operation::Bne:
    case Operation::Beql:
    case Operation::Bnel:
        output << ' ' << rs << ", " << rt << ", "
               << target_text(relative_branch_target(pc, instruction));
        break;
    case Operation::Blez:
    case Operation::Bgtz:
    case Operation::Bltz:
    case Operation::Bgez:
    case Operation::Bltzl:
    case Operation::Bgezl:
    case Operation::Bltzal:
    case Operation::Bgezal:
    case Operation::Bltzall:
    case Operation::Bgezall:
        output << ' ' << rs << ", " << target_text(relative_branch_target(pc, instruction));
        break;
    case Operation::J:
    case Operation::Jal:
        output << ' ' << target_text(absolute_jump_target(pc, instruction));
        break;
    case Operation::Jr:
        output << ' ' << rs;
        break;
    case Operation::Jalr:
        output << ' ' << rd << ", " << rs;
        break;
    case Operation::Syscall: {
        const std::uint32_t code = (word >> 6) & 0xfffffu;
        if (code != 0) {
            output << " 0x" << hex_value(code);
        }
        break;
    }
    case Operation::Sll:
    case Operation::Srl:
    case Operation::Sra:
    case Operation::Dsll:
    case Operation::Dsrl:
    case Operation::Dsra:
    case Operation::Dsll32:
    case Operation::Dsrl32:
    case Operation::Dsra32:
        output << ' ' << rd << ", " << rt << ", 0x" << hex_value(instruction.shift_amount);
        break;
    case Operation::Sllv:
    case Operation::Srlv:
    case Operation::Srav:
    case Operation::Dsllv:
    case Operation::Dsrlv:
    case Operation::Dsrav:
        // The shift amount comes from rs, as in the SLLV/SLL family.
        output << ' ' << rd << ", " << rt << ", " << rs;
        break;
    case Operation::Mfc1:
    case Operation::Mtc1:
        output << ' ' << rt << ", " << float_register(instruction.cop1_fs());
        break;
    case Operation::Cfc1:
    case Operation::Ctc1:
        // The operand names a control register: FCR0 and FCR31 have the
        // conventional fir/fcsr names.
        output << ' ' << rt << ", ";
        if (instruction.cop1_fs() == 0) {
            output << "fir";
        } else if (instruction.cop1_fs() == 31) {
            output << "fcsr";
        } else {
            output << float_register(instruction.cop1_fs());
        }
        break;
    case Operation::Lwc1:
    case Operation::Swc1:
        output << ' ' << float_register(instruction.rt) << ", "
               << signed_hex(instruction.signed_immediate()) << '(' << rs << ')';
        break;
    case Operation::AddS:
    case Operation::SubS:
    case Operation::MulS:
    case Operation::DivS:
    case Operation::AbsS:
    case Operation::MovS:
    case Operation::NegS:
    case Operation::MaxS:
    case Operation::MinS:
    case Operation::RsqrtS:
    case Operation::AddaS:
    case Operation::SubaS:
    case Operation::MulaS:
    case Operation::MaddaS:
    case Operation::MsubaS:
    case Operation::MaddS:
    case Operation::MsubS:
        output << ' ' << float_register(instruction.cop1_fd()) << ", "
               << float_register(instruction.cop1_fs()) << ", "
               << float_register(instruction.cop1_ft());
        break;
    case Operation::SqrtS:
        // sqrt.s reads only ft; the fs field is not an operand.
        output << ' ' << float_register(instruction.cop1_fd()) << ", "
               << float_register(instruction.cop1_ft());
        break;
    case Operation::CF:
    case Operation::CEq:
    case Operation::CLt:
    case Operation::CLe:
        output << ' ' << float_register(instruction.cop1_fs()) << ", "
               << float_register(instruction.cop1_ft());
        break;
    case Operation::CvtS:
    case Operation::CvtW:
        output << ' ' << float_register(instruction.cop1_fd()) << ", "
               << float_register(instruction.cop1_fs());
        break;
    case Operation::Bc1f:
    case Operation::Bc1t:
    case Operation::Bc1fl:
    case Operation::Bc1tl:
        output << ' ' << target_text(relative_branch_target(pc, instruction));
        break;
    case Operation::Mthi:
    case Operation::Mtlo:
    case Operation::Mtsa:
    case Operation::Mthi1:
    case Operation::Mtlo1:
    case Operation::Pmthi:
    case Operation::Pmtlo:
        output << ' ' << rs;
        break;
    case Operation::Mfhi:
    case Operation::Mflo:
    case Operation::Mfhi1:
    case Operation::Mflo1:
    case Operation::Pmfhi:
    case Operation::Pmflo:
        output << ' ' << rd;
        break;
    case Operation::Mtsab:
    case Operation::Mtsah:
        output << ' ' << rs << ", 0x" << hex_value(instruction.immediate);
        break;
    case Operation::Sync:
        // The five-bit completion code names the barrier flavor; zero is the
        // plain form.
        if (instruction.shift_amount != 0) {
            output << " 0x" << hex_value(instruction.shift_amount);
        }
        break;
    case Operation::Paddw:
    case Operation::Psubw:
    case Operation::Paddh:
    case Operation::Psubh:
    case Operation::Paddb:
    case Operation::Psubb:
    case Operation::Paddsw:
    case Operation::Psubsw:
    case Operation::Paddsh:
    case Operation::Psubsh:
    case Operation::Paddsb:
    case Operation::Psubsb:
    case Operation::Padduw:
    case Operation::Psubuw:
    case Operation::Padduh:
    case Operation::Psubuh:
    case Operation::Paddub:
    case Operation::Psubub:
    case Operation::Pcgtw:
    case Operation::Pcgth:
    case Operation::Pcgtb:
    case Operation::Pceqw:
    case Operation::Pceqh:
    case Operation::Pceqb:
    case Operation::Pmaxw:
    case Operation::Pmaxh:
    case Operation::Pminw:
    case Operation::Pminh:
    case Operation::Pand:
    case Operation::Por:
    case Operation::Pxor:
    case Operation::Pnor:
    case Operation::Pextlw:
    case Operation::Pextlh:
    case Operation::Pextlb:
    case Operation::Pextuw:
    case Operation::Pextuh:
    case Operation::Pextub:
    case Operation::Ppacw:
    case Operation::Ppach:
    case Operation::Ppacb:
    case Operation::Padsbh:
    case Operation::Pinth:
    case Operation::Pinteh:
    case Operation::Pcpyld:
    case Operation::Pcpyud:
    case Operation::Qfsrv:
        output << ' ' << rd << ", " << rs << ", " << rt;
        break;
    case Operation::Pabsw:
    case Operation::Pabsh:
    case Operation::Pext5:
    case Operation::Ppac5:
    case Operation::Pexeh:
    case Operation::Prevh:
    case Operation::Pexew:
    case Operation::Pexch:
    case Operation::Pexcw:
    case Operation::Pcpyh:
        output << ' ' << rd << ", " << rt;
        break;
    case Operation::Psllh:
    case Operation::Psrlh:
    case Operation::Psrah:
    case Operation::Psllw:
    case Operation::Psrlw:
    case Operation::Psraw:
        output << ' ' << rd << ", " << rt << ", 0x" << hex_value(instruction.shift_amount);
        break;
    case Operation::Psllvw:
    case Operation::Psrlvw:
    case Operation::Psravw:
        // The shift amount comes from rs; rt holds the data.
        output << ' ' << rd << ", " << rt << ", " << rs;
        break;
    case Operation::Pmfhl: {
        // The five variants share the mnemonic; the suffix names the format.
        static constexpr std::array<std::string_view, 5> suffixes = {
            ".lw", ".uw", ".slw", ".lh", ".sh"
        };
        if (instruction.shift_amount < suffixes.size()) {
            output << suffixes[instruction.shift_amount];
        }
        output << ' ' << rd;
        break;
    }
    case Operation::Pmthl:
        output << " pmthl.lw " << rs;
        break;
    case Operation::Mfc0:
    case Operation::Mtc0:
        output << ' ' << rt << ", " << cp0_register(instruction.rd);
        break;
    case Operation::Ei:
    case Operation::Di:
        break;
    case Operation::Break: {
        // The 20-bit code occupies bits 25-6, like SYSCALL's.
        const std::uint32_t code = (word >> 6) & 0xfffffu;
        if (code != 0) {
            output << " 0x" << hex_value(code);
        }
        break;
    }
    case Operation::Mult:
    case Operation::Multu:
    case Operation::Div:
    case Operation::Divu:
    case Operation::Madd:
    case Operation::Maddu:
    case Operation::Mult1:
    case Operation::Multu1:
    case Operation::Div1:
    case Operation::Divu1:
    case Operation::Madd1:
    case Operation::Maddu1:
        output << ' ' << rs << ", " << rt;
        break;
    case Operation::Plzcw:
        output << ' ' << rd << ", " << rs;
        break;
    case Operation::Unsupported:
        output << " 0x" << hex_value(word, 8) << " ; " << unsupported_family(instruction);
        break;
    }
    return output.str();
}

void disassemble_region(const ImageRecord& text, std::uint32_t start,
                        std::uint32_t instruction_count,
                        std::ostream& listing, std::ostream& report) {
    const std::uint64_t text_end = static_cast<std::uint64_t>(text.guest_address) + text.bytes.size();
    const std::uint64_t end = static_cast<std::uint64_t>(start)
        + static_cast<std::uint64_t>(instruction_count) * 4;
    if (instruction_count == 0 || start % 4 != 0 || text.guest_address % 4 != 0
        || start < text.guest_address || end > text_end || text_end > 0x100000000ull) {
        throw std::runtime_error("Expected a nonempty aligned range wholly inside file-backed text");
    }
    std::map<std::string, std::uint32_t> unsupported_counts;
    std::uint32_t unsupported_total = 0;
    for (std::uint32_t index = 0; index < instruction_count; ++index) {
        const auto pc = static_cast<std::uint32_t>(static_cast<std::uint64_t>(start) + index * 4ull);
        const auto offset = static_cast<std::size_t>(pc - text.guest_address);
        const auto bytes = std::span<const std::uint8_t, 4>(text.bytes.data() + offset, 4);
        const auto word = read_instruction_word(bytes);
        const auto instruction = decode(word);
        listing << hex_value(pc, 8) << ": " << hex_value(word, 8) << "  "
                << format_instruction(word, pc) << '\n';
        if (instruction.operation == Operation::Unsupported) {
            ++unsupported_total;
            ++unsupported_counts[unsupported_family(instruction)];
        }
    }
    report << "region=0x" << hex_value(start, 8) << " words=" << instruction_count
           << " supported=" << instruction_count - unsupported_total
           << " unsupported=" << unsupported_total << '\n';
    for (const auto& [family, count] : unsupported_counts) {
        report << "unsupported " << family << " count=" << count << '\n';
    }
    if (!listing || !report) {
        throw std::runtime_error("Could not write the complete disassembly/report");
    }
}

} // namespace gt4recomp::ee
