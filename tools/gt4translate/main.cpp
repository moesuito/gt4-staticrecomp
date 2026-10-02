#include "gt4recomp/ee_decode.hpp"
#include "gt4recomp/ee_disassemble.hpp"
#include "gt4recomp/ee_flow.hpp"
#include "hex_value.hpp"
#include "parse_number.hpp"
#include "verified_core.hpp"

#include <algorithm>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <set>
#include <sstream>
#include <span>
#include <stdexcept>
#include <string>

using namespace gt4recomp;
using namespace gt4recomp::ee;
using gt4recomp::tools::hex_value;

namespace {

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

// The branch condition, evaluated exactly like the interpreter does.
std::string condition_for(const DecodedInstruction& instruction) {
    std::ostringstream code;
    const auto rs = static_cast<unsigned>(instruction.rs);
    const auto rt = static_cast<unsigned>(instruction.rt);
    switch (instruction.operation) {
    case Operation::Beq:
    case Operation::Beql:
        code << "(state.read_gpr64(" << rs << ") == state.read_gpr64(" << rt << "))";
        break;
    case Operation::Bne:
    case Operation::Bnel:
        code << "(state.read_gpr64(" << rs << ") != state.read_gpr64(" << rt << "))";
        break;
    case Operation::Blez:
        code << "((state.read_gpr64(" << rs
             << ") & 0x8000000000000000ull) != 0 || state.read_gpr64(" << rs << ") == 0)";
        break;
    case Operation::Bgtz:
        code << "((state.read_gpr64(" << rs
             << ") & 0x8000000000000000ull) == 0 && state.read_gpr64(" << rs << ") != 0)";
        break;
    case Operation::Bltz:
    case Operation::Bltzl:
    case Operation::Bltzal:
    case Operation::Bltzall:
        code << "((state.read_gpr64(" << rs << ") & 0x8000000000000000ull) != 0)";
        break;
    case Operation::Bgez:
    case Operation::Bgezl:
    case Operation::Bgezal:
    case Operation::Bgezall:
        code << "((state.read_gpr64(" << rs << ") & 0x8000000000000000ull) == 0)";
        break;
    default:
        throw std::logic_error("no condition for this branch operation");
    }
    return code.str();
}

std::string label_for(std::uint32_t address) {
    return "label_" + hex_value(address, 8);
}

} // namespace

int wmain(int argc, wchar_t* argv[]) {
    if (argc != 4 && argc != 5) {
        std::cerr << "Usage: gt4translate CORE.GT4 start-address max-instructions [output-file]\n"
                     "Translates one function into a C++ header: plain instructions,\n"
                     "conditional branches (including likely and link forms), in-function\n"
                     "jumps and multiple 'jr ra' returns. Calls, indirect transfers and\n"
                     "unsupported words are rejected with context. Without an output file\n"
                     "the header is printed to stdout.\n";
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

        // The reachable blocks define the function's extent, so trailing
        // unrelated words are never translated.
        const std::uint32_t seeds[] = {start};
        const auto graph = build_control_flow_graph(text, seeds, max_instructions);
        if (graph.limited) {
            throw std::runtime_error("Function control flow exceeds the instruction limit");
        }
        std::set<std::uint32_t> reachable;
        std::uint32_t extent_end = start;
        for (const auto& node : graph.nodes) {
            for (std::uint32_t address = node.block.start;
                 address < node.block.end_exclusive; address += 4) {
                reachable.insert(address);
            }
            extent_end = std::max(extent_end, node.block.end_exclusive);
        }
        if (reachable.size() > max_instructions) {
            throw std::runtime_error("Function exceeds the instruction limit");
        }
        if (reachable.empty() || *reachable.begin() < start) {
            throw std::runtime_error("A transfer leaves the function below its entry");
        }

        // Validation: every reachable instruction must be translatable, and
        // every transfer target must stay inside the reachable set.
        std::set<std::uint32_t> labels;
        for (const auto address : reachable) {
            const auto word = word_at(address);
            const auto instruction = decode(word);
            const auto flow = classify(instruction, address);
            const auto reject = [&](const char* reason) {
                throw std::runtime_error(std::string(reason) + ": "
                                         + format_instruction(word, address) + " at 0x"
                                         + hex_value(address, 8));
            };
            switch (flow.kind) {
            case FlowKind::FallThrough:
                break;
            case FlowKind::Return:
                if (!reachable.contains(address + 4)) {
                    throw std::runtime_error("A return has no reachable delay slot at 0x"
                                             + hex_value(address, 8));
                }
                break;
            case FlowKind::Branch:
            case FlowKind::Jump:
                if (!reachable.contains(flow.target)) {
                    throw std::runtime_error("A transfer leaves the function: "
                                             + format_instruction(word, address) + " at 0x"
                                             + hex_value(address, 8) + " targets 0x"
                                             + hex_value(flow.target, 8));
                }
                labels.insert(flow.target);
                if (!reachable.contains(address + 4)) {
                    throw std::runtime_error("A transfer has no reachable delay slot at 0x"
                                             + hex_value(address, 8));
                }
                break;
            default:
                reject("Not supported by this translator slice");
            }
        }
        // A label inside a delay slot cannot be represented safely.
        for (const auto address : reachable) {
            const auto instruction = decode(word_at(address));
            const auto flow = classify(instruction, address);
            if ((flow.kind == FlowKind::Branch || flow.kind == FlowKind::Jump
                 || flow.kind == FlowKind::Return)
                && labels.contains(address + 4)) {
                throw std::runtime_error("A branch targets a delay slot at 0x"
                                         + hex_value(address + 4, 8));
            }
        }

        std::ostringstream body;
        std::set<std::uint32_t> consumed_delay_slots;
        for (std::uint32_t address = start; address < extent_end; address += 4) {
            const auto word = word_at(address);
            const auto instruction = decode(word);
            const auto flow = classify(instruction, address);

            if (!reachable.contains(address)) {
                body << "    // unreachable word 0x" << hex_value(address, 8) << ": "
                     << hex_value(word, 8) << " (not translated)\n\n";
                continue;
            }
            if (labels.contains(address)) {
                body << label_for(address) << ":;\n";
            }
            const auto comment = [&](std::uint32_t at, std::uint32_t comment_word,
                                     const char* indent = "    ") {
                body << indent << "// 0x" << hex_value(at, 8) << ": "
                     << hex_value(comment_word, 8) << "  "
                     << format_instruction(comment_word, at) << "\n";
            };
            if (consumed_delay_slots.contains(address)) {
                body << "    // 0x" << hex_value(address, 8) << ": " << hex_value(word, 8)
                     << "  delay slot handled above\n\n";
                continue;
            }

            switch (flow.kind) {
            case FlowKind::FallThrough:
                comment(address, word);
                body << "    " << statement_for(instruction) << "\n\n";
                break;
            case FlowKind::Branch: {
                const auto delay_instruction = decode(word_at(address + 4));
                const auto taken = "taken_" + hex_value(address, 8);
                comment(address, word);
                body << "    const bool " << taken << " = " << condition_for(instruction)
                     << ";\n";
                if (is_likely_branch(instruction.operation)) {
                    body << "    if (" << taken << ") {\n";
                    if (writes_link_register(instruction.operation)) {
                        body << "        state.write_gpr64(31, 0x" << hex_value(address + 8, 8)
                             << "u);\n";
                    }
                    comment(address + 4, word_at(address + 4), "        ");
                    body << "        " << statement_for(delay_instruction)
                         << " // delay slot (runs only when taken)\n";
                    body << "        goto " << label_for(flow.target) << ";\n";
                    body << "    }\n\n";
                } else {
                    if (writes_link_register(instruction.operation)) {
                        body << "    if (" << taken << ") { state.write_gpr64(31, 0x"
                             << hex_value(address + 8, 8) << "u); }\n";
                    }
                    comment(address + 4, word_at(address + 4));
                    body << "    " << statement_for(delay_instruction)
                         << " // delay slot (always executes)\n";
                    body << "    if (" << taken << ") goto " << label_for(flow.target)
                         << ";\n\n";
                }
                consumed_delay_slots.insert(address + 4);
                break;
            }
            case FlowKind::Jump: {
                comment(address, word);
                comment(address + 4, word_at(address + 4));
                body << "    " << statement_for(decode(word_at(address + 4)))
                     << " // delay slot (always executes)\n";
                body << "    goto " << label_for(flow.target) << ";\n\n";
                consumed_delay_slots.insert(address + 4);
                break;
            }
            case FlowKind::Return:
                comment(address, word);
                comment(address + 4, word_at(address + 4));
                body << "    " << statement_for(decode(word_at(address + 4)))
                     << " // delay slot (always executes)\n";
                body << "    state.set_pc(static_cast<std::uint32_t>(state.read_gpr64(31)));"
                     << " // return to ra\n\n";
                consumed_delay_slots.insert(address + 4);
                break;
            default:
                throw std::logic_error("unexpected flow kind during emission");
            }
        }

        std::ostringstream output;
        output << "#pragma once\n\n"
               << "// Generated by gt4translate from the pinned Gran Turismo 4 (USA) v2.00\n"
               << "// CORE. Entry 0x" << hex_value(start, 8) << ", " << reachable.size()
               << " reachable instructions.\n"
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
               << "inline void function_" << hex_value(start, 8) << "(ee::GuestState& state) {\n"
               << body.str()
               << "}\n\n} // namespace gt4recomp::translated\n";

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
