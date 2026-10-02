#include "gt4recomp/ee_decode.hpp"
#include "gt4recomp/ee_disassemble.hpp"
#include "gt4recomp/ee_flow.hpp"
#include "hex_value.hpp"
#include "parse_number.hpp"
#include "verified_core.hpp"

#include <algorithm>
#include <cstdint>
#include <deque>
#include <fstream>
#include <iostream>
#include <map>
#include <set>
#include <sstream>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

using namespace gt4recomp;
using namespace gt4recomp::ee;
using gt4recomp::tools::hex_value;

namespace {

constexpr std::size_t max_module_functions = 256;

// One function of the translated module: its reachable instructions, the
// labels its transfers need, and the extent used for emission.
struct TranslationUnit {
    std::uint32_t entry = 0;
    std::set<std::uint32_t> reachable;
    std::set<std::uint32_t> labels;
    std::uint32_t extent_end = 0;
    // The scan starts at the lowest reachable address: a function may be
    // entered above one of its own transfers (tail thunks).
    std::uint32_t scan_start = 0;
    // Boundary addresses the module stops at: syscalls encountered by the walk
    // and the optional explicit halt. The emitted module sets the pc there and
    // returns, exactly where the interpreter stops.
    std::set<std::uint32_t> halts;
};

std::uint32_t word_at(const ImageRecord& text, std::uint32_t address) {
    const auto offset = static_cast<std::size_t>(address - text.guest_address);
    const auto bytes = std::span<const std::uint8_t, 4>(text.bytes.data() + offset, 4);
    return read_instruction_word(bytes);
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
        code << "state.write_gpr64(" << rd << ", detail::less_than_signed_64(state.read_gpr64("
             << rs << "), state.read_gpr64(" << rt << ")) ? 1ull : 0ull);";
        break;
    case Operation::Sltu:
        code << "state.write_gpr64(" << rd << ", (state.read_gpr64(" << rs
             << ") < state.read_gpr64(" << rt << ")) ? 1ull : 0ull);";
        break;
    case Operation::Slti:
        code << "state.write_gpr64(" << rt
             << ", detail::less_than_signed_64(state.read_gpr64(" << rs << "), 0x"
             << hex_value(static_cast<std::uint64_t>(static_cast<std::int64_t>(displacement)), 16)
             << "ull) ? 1ull : 0ull);";
        break;
    case Operation::Sltiu:
        code << "state.write_gpr64(" << rt << ", (state.read_gpr64(" << rs << ") < 0x"
             << hex_value(static_cast<std::uint64_t>(static_cast<std::int64_t>(displacement)), 16)
             << "ull) ? 1ull : 0ull);";
        break;
    case Operation::Xori:
        code << "state.write_gpr64(" << rt << ", state.read_gpr64(" << rs << ") ^ 0x"
             << hex_value(instruction.immediate, 4) << "ull);";
        break;
    case Operation::Daddu:
        code << "state.write_gpr64(" << rd << ", state.read_gpr64(" << rs
             << ") + state.read_gpr64(" << rt << "));";
        break;
    case Operation::Movz:
        code << "if (state.read_gpr64(" << rt << ") == 0) { state.write_gpr64(" << rd
             << ", state.read_gpr64(" << rs << ")); }";
        break;
    case Operation::Movn:
        code << "if (state.read_gpr64(" << rt << ") != 0) { state.write_gpr64(" << rd
             << ", state.read_gpr64(" << rs << ")); }";
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
    case Operation::Sra:
        code << "state.write_gpr32(" << rd
             << ", detail::arithmetic_shift_right_32(state.read_gpr32(" << rt
             << "), " << static_cast<unsigned>(instruction.shift_amount) << "));";
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
    case Operation::Lb:
        code << "state.write_gpr32(" << rt
             << ", detail::sign_extend_8(state.memory().read_byte(detail::effective_address(state, "
             << rs << ", " << displacement << "))));";
        break;
    case Operation::Lbu:
        code << "state.write_gpr32(" << rt
             << ", state.memory().read_byte(detail::effective_address(state, " << rs
             << ", " << displacement << ")));";
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
    case Operation::Lq:
        // LQ silently aligns to 16 bytes and moves both register halves.
        code << "{\n"
             << "        const std::uint32_t address = detail::effective_address(state, " << rs
             << ", " << displacement << ") & ~0xfu;\n"
             << "        state.write_gpr64(" << rt
             << ", state.memory().read_doubleword(address));\n"
             << "        state.write_gpr_high64(" << rt
             << ", state.memory().read_doubleword(address + 8));\n"
             << "    }";
        break;
    case Operation::Sq:
        code << "{\n"
             << "        const std::uint32_t address = detail::effective_address(state, " << rs
             << ", " << displacement << ") & ~0xfu;\n"
             << "        state.memory().write_doubleword(address, state.read_gpr64(" << rt
             << "));\n"
             << "        state.memory().write_doubleword(address + 8, state.read_gpr_high64(" << rt
             << "));\n"
             << "    }";
        break;
    case Operation::Mthi:
        code << "state.set_hi(state.read_gpr64(" << rs << "));";
        break;
    case Operation::Mthi1:
        code << "state.set_hi1(state.read_gpr64(" << rs << "));";
        break;
    case Operation::Mtlo:
        code << "state.set_lo(state.read_gpr64(" << rs << "));";
        break;
    case Operation::Mtlo1:
        code << "state.set_lo1(state.read_gpr64(" << rs << "));";
        break;
    case Operation::Mtsah:
        code << "state.set_shift_amount_cache(((state.read_gpr32(" << rs << ") & 0x7u) ^ 0x"
             << hex_value(instruction.immediate & 0x7u, 1) << "u) << 1);";
        break;
    case Operation::Mtc1:
        // The FPU register number lives in the rd field for COP1 moves.
        code << "state.write_fpr(" << rd << ", state.read_gpr32(" << rt << "));";
        break;
    case Operation::Ctc1:
        if (instruction.cop1_fs() == 31) {
            code << "state.set_fpu_control(state.read_gpr32(" << rt << "));";
        } else {
            code << "; // ctc1 to a control register other than FCR31 has no effect";
        }
        break;
    case Operation::Sync:
        code << "; // sync: the pipeline barrier has no effect in this model";
        break;
    case Operation::Cache:
        code << "; // cache hint: no effect in this model";
        break;
    case Operation::Plzcw:
        code << "state.write_gpr64(" << rd
             << ", detail::plzcw_words(state.read_gpr64(" << rs << ")));";
        break;
    case Operation::AddaS:
        // The casts matter: uint8_t streams as a character, which would emit
        // an invisible control byte instead of the register number.
        code << "state.set_fpu_accumulator(detail::normalize_fpu_result(\n"
             << "        detail::hardware_float(state.read_fpr("
             << static_cast<unsigned>(instruction.cop1_fs())
             << ")) + detail::hardware_float(state.read_fpr("
             << static_cast<unsigned>(instruction.cop1_ft()) << ")),\n"
             << "        detail::fpu_flag_o | detail::fpu_flag_so,\n"
             << "        detail::fpu_flag_u | detail::fpu_flag_su, state));";
        break;
    case Operation::Padduw:
        // The 128-bit result needs both halves written from pre-read sources;
        // the two writes are independent (low reads the low halves, high the
        // high halves), so aliasing rd with rs or rt stays correct.
        code << "state.write_gpr64(" << rd << ", detail::combine_words(\n"
             << "        detail::padduw_lane(detail::lane_word(state.read_gpr64(" << rs
             << "), 0), detail::lane_word(state.read_gpr64(" << rt << "), 0)),\n"
             << "        detail::padduw_lane(detail::lane_word(state.read_gpr64(" << rs
             << "), 1), detail::lane_word(state.read_gpr64(" << rt << "), 1))));\n"
             << "    state.write_gpr_high64(" << rd << ", detail::combine_words(\n"
             << "        detail::padduw_lane(detail::lane_word(state.read_gpr_high64(" << rs
             << "), 0), detail::lane_word(state.read_gpr_high64(" << rt << "), 0)),\n"
             << "        detail::padduw_lane(detail::lane_word(state.read_gpr_high64(" << rs
             << "), 1), detail::lane_word(state.read_gpr_high64(" << rt << "), 1))));";
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

std::string call_expression(std::uint32_t target) {
    return "function_" + hex_value(target, 8) + "(state);";
}

// The body of one translated function, in execution-correct order.
std::string emit_unit_body(const ImageRecord& text, const TranslationUnit& unit) {
    std::ostringstream body;
    std::set<std::uint32_t> consumed_delay_slots;
    if (unit.scan_start < unit.entry) {
        // The scan begins below the entry (a tail thunk's target); a caller
        // must still start executing at the entry address.
        body << "    goto " << label_for(unit.entry)
             << "; // the entry lies above the scan start\n\n";
    }
    for (std::uint32_t address = unit.scan_start; address < unit.extent_end; address += 4) {
        const auto word = word_at(text, address);
        const auto instruction = decode(word);
        const auto flow = classify(instruction, address);

        if (unit.halts.contains(address)) {
            // The model boundary: stop exactly where the interpreter stops,
            // with the pc at (not past) this word.
            if (unit.labels.contains(address)) {
                body << label_for(address) << ":;\n";
            }
            body << "    // 0x" << hex_value(address, 8) << ": " << hex_value(word, 8) << "  "
                 << format_instruction(word, address) << "\n"
                 << "    state.set_pc(0x" << hex_value(address, 8) << "u);\n"
                 << "    return;\n\n";
            continue;
        }
        if (!unit.reachable.contains(address) && !unit.labels.contains(address)) {
            // Collapse runs of addresses that carry no code (a tail thunk's
            // target can sit far below its caller) into one comment.
            std::uint32_t next = address;
            while (next < unit.extent_end && !unit.reachable.contains(next)
                   && !unit.labels.contains(next) && !unit.halts.contains(next)) {
                next += 4;
            }
            body << "    // " << ((next - address) / 4) << " word(s) with no reachable code (0x"
                 << hex_value(address, 8) << " to 0x" << hex_value(next, 8) << ")\n\n";
            address = next - 4;  // the loop increment moves to `next`
            continue;
        }
        if (unit.labels.contains(address)) {
            body << label_for(address) << ":;\n";
        }
        const auto comment = [&](std::uint32_t at, std::uint32_t comment_word,
                                 const char* indent = "    ") {
            body << indent << "// 0x" << hex_value(at, 8) << ": "
                 << hex_value(comment_word, 8) << "  "
                 << format_instruction(comment_word, at) << "\n";
        };
        // Emitting one instruction with the address attached to any failure,
        // so rejections name the exact word.
        const auto statement_at = [&](std::uint32_t at) {
            try {
                return statement_for(decode(word_at(text, at)));
            } catch (const std::logic_error&) {
                throw std::runtime_error("No C++ statement for "
                    + format_instruction(word_at(text, at), at) + " at 0x"
                    + hex_value(at, 8));
            }
        };
        if (consumed_delay_slots.contains(address)) {
            body << "    // 0x" << hex_value(address, 8) << ": " << hex_value(word, 8)
                 << "  delay slot handled above\n\n";
            continue;
        }

        switch (flow.kind) {
        case FlowKind::FallThrough:
            comment(address, word);
            body << "    " << statement_at(address) << "\n\n";
            break;
        case FlowKind::Branch: {
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
                comment(address + 4, word_at(text, address + 4), "        ");
                body << "        " << statement_at(address + 4)
                     << " // delay slot (runs only when taken)\n";
                body << "        goto " << label_for(flow.target) << ";\n";
                body << "    }\n\n";
            } else {
                if (writes_link_register(instruction.operation)) {
                    body << "    if (" << taken << ") { state.write_gpr64(31, 0x"
                         << hex_value(address + 8, 8) << "u); }\n";
                }
                comment(address + 4, word_at(text, address + 4));
                body << "    " << statement_at(address + 4)
                     << " // delay slot (always executes)\n";
                body << "    if (" << taken << ") goto " << label_for(flow.target)
                     << ";\n\n";
            }
            consumed_delay_slots.insert(address + 4);
            break;
        }
        case FlowKind::Jump: {
            comment(address, word);
            comment(address + 4, word_at(text, address + 4));
            body << "    " << statement_at(address + 4)
                 << " // delay slot (always executes)\n";
            body << "    goto " << label_for(flow.target) << ";\n\n";
            consumed_delay_slots.insert(address + 4);
            break;
        }
        case FlowKind::Call: {
            // Direct calls only (validated earlier). The link is written, the
            // delay slot runs, then the callee executes; execution resumes at
            // pc+8, which is the next emitted statement.
            comment(address, word);
            body << "    state.write_gpr64(31, 0x" << hex_value(address + 8, 8)
                 << "u); // link\n";
            comment(address + 4, word_at(text, address + 4));
            body << "    " << statement_at(address + 4)
                 << " // delay slot (always executes)\n";
            body << "    " << call_expression(flow.target) << "\n";
            body << "    if (state.pc() != 0x" << hex_value(address + 8, 8)
                 << "u) { return; } // a service boundary stopped the callee\n\n";
            consumed_delay_slots.insert(address + 4);
            break;
        }
        case FlowKind::Return:
            comment(address, word);
            comment(address + 4, word_at(text, address + 4));
            body << "    " << statement_at(address + 4)
                 << " // delay slot (always executes)\n";
            body << "    state.set_pc(static_cast<std::uint32_t>(state.read_gpr64(31)));"
                 << " // return to ra\n\n";
            consumed_delay_slots.insert(address + 4);
            break;
        default:
            throw std::logic_error("unexpected flow kind during emission");
        }
    }
    return body.str();
}

} // namespace

int wmain(int argc, wchar_t* argv[]) {
    if (argc < 4 || argc > 6) {
        std::cerr << "Usage: gt4translate CORE.GT4 start-address max-instructions "
                     "[output-file [halt-address]]\n"
                     "Translates a function and its direct call tree into a C++ header:\n"
                     "plain instructions, conditional branches (including likely and link\n"
                     "forms), in-function jumps, direct 'jal' calls (translated recursively)\n"
                     "and multiple 'jr ra' returns. With a halt address the walk stops there\n"
                     "instead of translating further (a syscall or an unsupported word); the\n"
                     "emitted module sets the pc at that address and returns, exactly where\n"
                     "the interpreter stops. Indirect calls, exceptions and unsupported words\n"
                     "are rejected with context. max-instructions bounds the whole module.\n"
                     "Without an output file the header is printed to stdout.\n";
        return 2;
    }
    try {
        const auto start = gt4recomp::tools::parse_number(argv[2]);
        const auto max_instructions = gt4recomp::tools::parse_number(argv[3]);
        const std::uint32_t halt_address =
            argc == 6 ? gt4recomp::tools::parse_number(argv[5]) : 0;
        if (halt_address != 0 && halt_address % 4 != 0) {
            throw std::runtime_error("Expected an aligned halt address");
        }
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

        // Translate the direct call tree, bounded by the instruction budget.
        std::map<std::uint32_t, TranslationUnit> units;
        bool halt_seen = false;
        std::deque<std::uint32_t> pending;
        pending.push_back(start);
        auto budget = static_cast<std::size_t>(max_instructions);
        while (!pending.empty()) {
            std::uint32_t entry = pending.front();
            pending.pop_front();
            if (units.contains(entry)) {
                continue;
            }
            if (units.size() >= max_module_functions) {
                throw std::runtime_error("The call tree exceeds the function limit");
            }

            const auto graph = build_control_flow_graph(
                text, std::span<const std::uint32_t>(&entry, 1), max_instructions);
            if (graph.limited) {
                throw std::runtime_error("Function at 0x" + hex_value(entry, 8)
                                         + " exceeds the instruction limit");
            }
            TranslationUnit unit;
            unit.entry = entry;
            for (const auto& node : graph.nodes) {
                for (std::uint32_t address = node.block.start;
                     address < node.block.end_exclusive; address += 4) {
                    unit.reachable.insert(address);
                }
                unit.extent_end = std::max(unit.extent_end, node.block.end_exclusive);
                if (node.block.ending == FlowKind::Exception) {
                    // A syscall the walk reached: the module stops there,
                    // exactly like the interpreter, and callers propagate the
                    // stop through the call check. Exception has no delay
                    // slot, so the syscall is the block's last word.
                    const std::uint32_t syscall_address = node.block.end_exclusive - 4;
                    unit.reachable.erase(syscall_address);
                    unit.halts.insert(syscall_address);
                }
            }
            if (halt_address != 0 && unit.reachable.contains(halt_address)) {
                // The explicit halt address must be a boundary the interpreter
                // stops on, and it is not translated either.
                const auto halt_flow =
                    classify(decode(word_at(text, halt_address)), halt_address);
                if (halt_flow.kind != FlowKind::Exception
                    && halt_flow.kind != FlowKind::Unsupported) {
                    throw std::runtime_error(
                        "A halt address must be a syscall or an unsupported word");
                }
                unit.reachable.erase(halt_address);
                unit.halts.insert(halt_address);
                halt_seen = true;
            } else if (halt_address != 0 && unit.halts.contains(halt_address)) {
                // The walk already found this boundary as a syscall.
                halt_seen = true;
            }
            if (unit.reachable.empty()) {
                throw std::runtime_error("Function at 0x" + hex_value(entry, 8)
                                         + ": no reachable instructions");
            }
            unit.scan_start = std::min(unit.entry, *unit.reachable.begin());
            if (unit.scan_start < unit.entry) {
                // The walk went below the entry (a tail thunk's target): the
                // emission must still begin execution at the entry address.
                unit.labels.insert(unit.entry);
            }
            if (unit.reachable.size() > budget) {
                throw std::runtime_error("The call tree exceeds the instruction budget");
            }
            budget -= unit.reachable.size();

            std::vector<std::uint32_t> calls;
            for (const auto address : unit.reachable) {
                const auto word = word_at(text, address);
                const auto instruction = decode(word);
                const auto flow = classify(instruction, address);
                const auto reject = [&](const char* reason) {
                    throw std::runtime_error(std::string(reason) + " in function 0x"
                                             + hex_value(entry, 8) + ": "
                                             + format_instruction(word, address) + " at 0x"
                                             + hex_value(address, 8));
                };
                switch (flow.kind) {
                case FlowKind::FallThrough:
                    break;
                case FlowKind::Return:
                    if (!unit.reachable.contains(address + 4)) {
                        reject("A return has no reachable delay slot");
                    }
                    break;
                case FlowKind::Branch:
                case FlowKind::Jump:
                    if (!unit.reachable.contains(flow.target)
                        && !unit.halts.contains(flow.target)) {
                        reject("A transfer leaves the function");
                    }
                    unit.labels.insert(flow.target);
                    if (!unit.reachable.contains(address + 4)) {
                        reject("A transfer has no reachable delay slot");
                    }
                    break;
                case FlowKind::Call:
                    if (instruction.operation != Operation::Jal) {
                        reject("Indirect calls are not supported");
                    }
                    if (!unit.reachable.contains(address + 4)) {
                        reject("A call has no reachable delay slot");
                    }
                    calls.push_back(flow.target);
                    break;
                default:
                    reject("Not supported by this translator");
                }
            }
            for (const auto address : unit.reachable) {
                const auto instruction = decode(word_at(text, address));
                const auto flow = classify(instruction, address);
                if ((flow.kind == FlowKind::Branch || flow.kind == FlowKind::Jump
                     || flow.kind == FlowKind::Call || flow.kind == FlowKind::Return)
                    && unit.labels.contains(address + 4)) {
                    throw std::runtime_error("A branch targets a delay slot at 0x"
                                             + hex_value(address + 4, 8));
                }
            }

            for (const auto target : calls) {
                if (!units.contains(target)) {
                    pending.push_back(target);
                }
            }
            units.emplace(entry, std::move(unit));
        }

        if (halt_address != 0 && !halt_seen) {
            throw std::runtime_error("The halt address 0x" + hex_value(halt_address, 8)
                                     + " is not reached by the translated code");
        }

        std::size_t total_instructions = 0;
        for (const auto& [entry, unit] : units) {
            total_instructions += unit.reachable.size();
        }

        std::ostringstream output;
        output << "#pragma once\n\n"
               << "// Generated by gt4translate from the pinned Gran Turismo 4 (USA) v2.00\n"
               << "// CORE. Entry 0x" << hex_value(start, 8) << ", " << units.size()
               << " function(s), " << total_instructions << " instructions.\n"
               << "// This file is derived from game code: keep it in ignored directories\n"
               << "// and never commit it.\n\n"
               << "#include \"gt4recomp/ee_state.hpp\"\n\n"
               << "#include <bit>\n"
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
               << "[[nodiscard]] inline std::uint32_t sign_extend_8(std::uint8_t value) {\n"
               << "    return (value & 0x80u) != 0 ? (0xffffff00u | value) : value;\n"
               << "}\n\n"
               << "[[nodiscard]] inline bool less_than_signed_64(std::uint64_t left,\n"
               << "                                              std::uint64_t right) {\n"
               << "    return (left ^ 0x8000000000000000ull) < (right ^ 0x8000000000000000ull);\n"
               << "}\n\n"
               << "[[nodiscard]] inline std::uint32_t arithmetic_shift_right_32(std::uint32_t value,\n"
               << "                                                              std::uint8_t shift) {\n"
               << "    if (shift == 0) {\n"
               << "        return value;\n"
               << "    }\n"
               << "    const std::uint32_t shifted = value >> shift;\n"
               << "    return (value & 0x80000000u) != 0\n"
               << "               ? (shifted | (0xffffffffu << (32 - shift)))\n"
               << "               : shifted;\n"
               << "}\n\n"
               << "[[nodiscard]] inline std::uint32_t lane_word(std::uint64_t value, int lane) {\n"
               << "    return static_cast<std::uint32_t>(value >> (32 * lane));\n"
               << "}\n\n"
               << "[[nodiscard]] inline std::uint64_t combine_words(std::uint32_t low,\n"
               << "                                                  std::uint32_t high) {\n"
               << "    return static_cast<std::uint64_t>(low)\n"
               << "        | (static_cast<std::uint64_t>(high) << 32);\n"
               << "}\n\n"
               << "[[nodiscard]] inline std::uint32_t padduw_lane(std::uint32_t left,\n"
               << "                                                std::uint32_t right) {\n"
               << "    const std::uint64_t sum = static_cast<std::uint64_t>(left)\n"
               << "        + static_cast<std::uint64_t>(right);\n"
               << "    return sum > 0xffffffffull ? 0xffffffffu\n"
               << "                               : static_cast<std::uint32_t>(sum);\n"
               << "}\n\n"
               << "constexpr std::uint32_t fpu_flag_o = 0x00008000u;\n"
               << "constexpr std::uint32_t fpu_flag_u = 0x00004000u;\n"
               << "constexpr std::uint32_t fpu_flag_so = 0x00000010u;\n"
               << "constexpr std::uint32_t fpu_flag_su = 0x00000008u;\n"
               << "constexpr std::uint32_t largest_finite_bits = 0x7f7fffffu;\n\n"
               << "// PLZCW's per-word count: negative values invert, zero counts 32,\n"
               << "// and the instruction stores one less than the count.\n"
               << "[[nodiscard]] inline std::uint32_t count_leading_sign_bits(std::uint32_t value) {\n"
               << "    if ((value & 0x80000000u) != 0) {\n"
               << "        value = ~value;\n"
               << "    }\n"
               << "    return value == 0 ? 32u : static_cast<std::uint32_t>(std::countl_zero(value));\n"
               << "}\n\n"
               << "[[nodiscard]] inline std::uint64_t plzcw_words(std::uint64_t source) {\n"
               << "    const std::uint32_t low_count =\n"
               << "        count_leading_sign_bits(static_cast<std::uint32_t>(source)) - 1;\n"
               << "    const std::uint32_t high_count =\n"
               << "        count_leading_sign_bits(static_cast<std::uint32_t>(source >> 32)) - 1;\n"
               << "    return (static_cast<std::uint64_t>(high_count) << 32) | low_count;\n"
               << "}\n\n"
               << "// The PS2 FPU has no denormals and saturates at the largest finite\n"
               << "// value; this mirrors the interpreter's hardware_float exactly.\n"
               << "[[nodiscard]] inline float hardware_float(std::uint32_t bits) {\n"
               << "    const std::uint32_t exponent = bits & 0x7f800000u;\n"
               << "    if (exponent == 0) {\n"
               << "        return std::bit_cast<float>(bits & 0x80000000u);\n"
               << "    }\n"
               << "    if (exponent == 0x7f800000u) {\n"
               << "        return std::bit_cast<float>((bits & 0x80000000u) | largest_finite_bits);\n"
               << "    }\n"
               << "    return std::bit_cast<float>(bits);\n"
               << "}\n\n"
               << "// Applies the interpreter's overflow/underflow rules to a computed\n"
               << "// result: saturation and flush with the same flag transitions.\n"
               << "[[nodiscard]] inline std::uint32_t normalize_fpu_result(\n"
               << "    float value, std::uint32_t overflow_flags, std::uint32_t underflow_flags,\n"
               << "    ee::GuestState& state) {\n"
               << "    std::uint32_t bits = std::bit_cast<std::uint32_t>(value);\n"
               << "    if ((bits & 0x7fffffffu) == 0x7f800000u) {\n"
               << "        bits = (bits & 0x80000000u) | largest_finite_bits;\n"
               << "        state.set_fpu_control(state.fpu_control() | overflow_flags);\n"
               << "        return bits;\n"
               << "    }\n"
               << "    if ((overflow_flags & fpu_flag_o) != 0) {\n"
               << "        state.set_fpu_control(state.fpu_control() & ~fpu_flag_o);\n"
               << "    }\n"
               << "    if ((bits & 0x7f800000u) == 0 && (bits & 0x007fffffu) != 0) {\n"
               << "        bits &= 0x80000000u;\n"
               << "        state.set_fpu_control(state.fpu_control() | underflow_flags);\n"
               << "        return bits;\n"
               << "    }\n"
               << "    if ((underflow_flags & fpu_flag_u) != 0) {\n"
               << "        state.set_fpu_control(state.fpu_control() & ~fpu_flag_u);\n"
               << "    }\n"
               << "    return bits;\n"
               << "}\n\n"
               << "} // namespace detail\n\n";
        for (const auto& [entry, unit] : units) {
            output << "inline void function_" << hex_value(entry, 8)
                   << "(ee::GuestState& state);\n";
        }
        output << "\n";
        for (const auto& [entry, unit] : units) {
            output << "inline void function_" << hex_value(entry, 8)
                   << "(ee::GuestState& state) {\n"
                   << emit_unit_body(text, unit) << "}\n\n";
        }
        output << "} // namespace gt4recomp::translated\n";

        if (argc >= 5) {
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
