#include "gt4recomp/ee_decode.hpp"
#include "gt4recomp/ee_disassemble.hpp"
#include "gt4recomp/ee_flow.hpp"
#include "hex_value.hpp"
#include "parse_number.hpp"
#include "verified_core.hpp"

#include <cstdint>
#include <fstream>
#include <iostream>
#include <sstream>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

using namespace gt4recomp;
using namespace gt4recomp::ee;
using gt4recomp::tools::hex_value;

namespace {

struct InstructionLine {
    std::uint32_t pc = 0;
    std::uint32_t word = 0;
    std::string assembly;
    std::string statement;
};

bool is_return_to_ra(const DecodedInstruction& instruction) {
    return instruction.operation == Operation::Jr && instruction.rs == 31;
}

// One C++ statement reproducing the instruction's effect on GuestState. The
// expressions mirror the interpreter's execution rules exactly.
std::string statement_for(const DecodedInstruction& instruction) {
    std::ostringstream code;
    const auto rs = static_cast<unsigned>(instruction.rs);
    const auto rt = static_cast<unsigned>(instruction.rt);
    const auto rd = static_cast<unsigned>(instruction.rd);
    const auto displacement = instruction.signed_immediate();
    switch (instruction.operation) {
    case Operation::Addu:
        code << "state.write_gpr32(" << rd << ", state.read_gpr32(" << rs
             << ") + state.read_gpr32(" << rt << "));";
        break;
    case Operation::Subu:
        code << "state.write_gpr32(" << rd << ", state.read_gpr32(" << rs
             << ") - state.read_gpr32(" << rt << "));";
        break;
    case Operation::And:
        code << "state.write_gpr64(" << rd << ", state.read_gpr64(" << rs
             << ") & state.read_gpr64(" << rt << "));";
        break;
    case Operation::Or:
        code << "state.write_gpr64(" << rd << ", state.read_gpr64(" << rs
             << ") | state.read_gpr64(" << rt << "));";
        break;
    case Operation::Xor:
        code << "state.write_gpr64(" << rd << ", state.read_gpr64(" << rs
             << ") ^ state.read_gpr64(" << rt << "));";
        break;
    case Operation::Slt:
        code << "state.write_gpr64(" << rd << ", ((state.read_gpr32(" << rs
             << ") ^ 0x80000000u) < (state.read_gpr32(" << rt
             << ") ^ 0x80000000u)) ? 1ull : 0ull);";
        break;
    case Operation::Sltu:
        code << "state.write_gpr64(" << rd << ", (state.read_gpr32(" << rs
             << ") < state.read_gpr32(" << rt << ")) ? 1ull : 0ull);";
        break;
    case Operation::Daddu:
        code << "state.write_gpr64(" << rd << ", state.read_gpr64(" << rs
             << ") + state.read_gpr64(" << rt << "));";
        break;
    case Operation::Addiu:
        code << "state.write_gpr32(" << rt << ", state.read_gpr32(" << rs
             << ") + static_cast<std::uint32_t>(" << displacement << "));";
        break;
    case Operation::Andi:
        code << "state.write_gpr64(" << rt << ", state.read_gpr64(" << rs
             << ") & 0x" << hex_value(instruction.immediate, 4) << "ull);";
        break;
    case Operation::Ori:
        code << "state.write_gpr64(" << rt << ", state.read_gpr64(" << rs
             << ") | 0x" << hex_value(instruction.immediate, 4) << "ull);";
        break;
    case Operation::Lui:
        code << "state.write_gpr32(" << rt << ", 0x"
             << hex_value(static_cast<std::uint32_t>(instruction.immediate) << 16, 8) << "u);";
        break;
    case Operation::Sll:
        code << "state.write_gpr32(" << rd << ", state.read_gpr32(" << rt
             << ") << " << static_cast<unsigned>(instruction.shift_amount) << ");";
        break;
    case Operation::Srl:
        code << "state.write_gpr32(" << rd << ", state.read_gpr32(" << rt
             << ") >> " << static_cast<unsigned>(instruction.shift_amount) << ");";
        break;
    case Operation::Lw:
        code << "state.write_gpr32(" << rt
             << ", state.memory().read_word(detail::effective_address(state, " << rs
             << ", " << displacement << ")));";
        break;
    case Operation::Lh:
        code << "state.write_gpr32(" << rt
             << ", detail::sign_extend_16(state.memory().read_halfword("
                "detail::effective_address(state, " << rs << ", " << displacement << "))));";
        break;
    case Operation::Ld:
        code << "state.write_gpr64(" << rt
             << ", state.memory().read_doubleword(detail::effective_address(state, " << rs
             << ", " << displacement << ")));";
        break;
    case Operation::Sw:
        code << "state.memory().write_word(detail::effective_address(state, " << rs << ", "
             << displacement << "), state.read_gpr32(" << rt << "));";
        break;
    case Operation::Sb:
        code << "state.memory().write_byte(detail::effective_address(state, " << rs
             << ", " << displacement << "), static_cast<std::uint8_t>(state.read_gpr64("
             << rt << ") & 0xff));";
        break;
    case Operation::Sd:
        code << "state.memory().write_doubleword(detail::effective_address(state, " << rs
             << ", " << displacement << "), state.read_gpr64(" << rt << "));";
        break;
    default:
        throw std::logic_error("no C++ statement for this operation");
    }
    return code.str();
}

} // namespace

int wmain(int argc, wchar_t* argv[]) {
    if (argc != 4 && argc != 5) {
        std::cerr << "Usage: gt4translate CORE.GT4 start-address max-instructions [output-file]\n"
                     "Translates one straight-line leaf function ending in 'jr ra' into a C++\n"
                     "header. Without an output file the header is printed to stdout.\n";
        return 2;
    }
    try {
        const auto start = gt4recomp::tools::parse_number(argv[2]);
        const auto max_instructions = gt4recomp::tools::parse_number(argv[3]);
        const auto core = gt4recomp::tools::read_verified_core(argv[1]);
        const auto image = reconstruct_core(core);
        const auto& text = image.text;
        const std::uint64_t text_end =
            static_cast<std::uint64_t>(text.guest_address) + text.bytes.size();
        if (max_instructions == 0 || start % 4 != 0 || start < text.guest_address
            || static_cast<std::uint64_t>(start) + 4 > text_end) {
            throw std::runtime_error(
                "Expected an aligned start inside file-backed text and a nonzero limit");
        }

        const auto word_at = [&](std::uint32_t address) {
            const auto offset = static_cast<std::size_t>(address - text.guest_address);
            const auto bytes = std::span<const std::uint8_t, 4>(text.bytes.data() + offset, 4);
            return read_instruction_word(bytes);
        };

        std::vector<InstructionLine> lines;
        std::uint32_t pc = start;
        bool returned = false;
        for (std::uint32_t index = 0; index < max_instructions; ++index) {
            if (static_cast<std::uint64_t>(pc) + 4 > text_end) {
                break;
            }
            const auto word = word_at(pc);
            const auto instruction = decode(word);
            const auto flow = classify(instruction, pc);
            if (is_return_to_ra(instruction)) {
                // The delay slot executes before the return; it must be plain.
                const auto delay_pc = pc + 4;
                if (static_cast<std::uint64_t>(delay_pc) + 4 > text_end) {
                    throw std::runtime_error("'jr ra' has no delay slot inside the text");
                }
                const auto delay_word = word_at(delay_pc);
                const auto delay = decode(delay_word);
                if (classify(delay, delay_pc).kind != FlowKind::FallThrough) {
                    throw std::runtime_error("The 'jr ra' delay slot at 0x"
                                             + hex_value(delay_pc, 8)
                                             + " is not a plain instruction");
                }
                lines.push_back({delay_pc, delay_word, format_instruction(delay_word, delay_pc),
                                 statement_for(delay)});
                lines.push_back(
                    {pc, word,
                     format_instruction(word, pc) + " (the delay slot above runs first)",
                     "state.set_pc(static_cast<std::uint32_t>(state.read_gpr64(31)));"
                     " // return to ra"});
                returned = true;
                break;
            }
            if (flow.kind != FlowKind::FallThrough) {
                throw std::runtime_error("Only a final 'jr ra' transfer is supported; found "
                                         + format_instruction(word, pc) + " at 0x"
                                         + hex_value(pc, 8));
            }
            lines.push_back({pc, word, format_instruction(word, pc), statement_for(instruction)});
            pc += 4;
        }
        if (!returned) {
            throw std::runtime_error("No returning 'jr ra' found within the instruction limit");
        }

        std::ostringstream output;
        output << "#pragma once\n\n"
               << "// Generated by gt4translate from the pinned Gran Turismo 4 (USA) v2.00\n"
               << "// CORE. Entry 0x" << hex_value(start, 8) << ", " << lines.size()
               << " instructions.\n"
               << "// This file is derived from game code: keep it in ignored directories\n"
               << "// and never commit it.\n\n"
               << "#include \"gt4recomp/ee_state.hpp\"\n\n"
               << "#include <cstdint>\n\n"
               << "namespace gt4recomp::translated {\n\n"
               << "namespace detail {\n\n"
               << "[[nodiscard]] inline std::uint32_t effective_address(\n"
               << "    const ee::GuestState& state, std::uint8_t base_register,\n"
               << "    std::int32_t displacement) {\n"
               << "    return static_cast<std::uint32_t>(\n"
               << "        state.read_gpr64(base_register)\n"
               << "        + static_cast<std::uint64_t>(static_cast<std::int64_t>(displacement)));\n"
               << "}\n\n"
               << "[[nodiscard]] inline std::uint32_t sign_extend_16(std::uint16_t value) {\n"
               << "    return (value & 0x8000u) != 0 ? (0xffff0000u | value) : value;\n"
               << "}\n\n"
               << "} // namespace detail\n\n"
               << "inline void function_" << hex_value(start, 8) << "(ee::GuestState& state) {\n";
        for (const auto& line : lines) {
            output << "    // 0x" << hex_value(line.pc, 8) << ": " << hex_value(line.word, 8)
                   << "  " << line.assembly << "\n"
                   << "    " << line.statement << "\n\n";
        }
        output << "}\n\n} // namespace gt4recomp::translated\n";

        if (argc == 5) {
            std::ofstream file(argv[4], std::ios::binary);
            if (!file) {
                throw std::runtime_error("Cannot open the output file");
            }
            file << output.str();
            if (!file) {
                throw std::runtime_error("Failed writing the output file");
            }
        } else {
            std::cout << output.str();
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "ERROR: " << error.what() << '\n';
        return 1;
    }
}
