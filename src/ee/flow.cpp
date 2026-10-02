#include "gt4recomp/ee_flow.hpp"

#include <algorithm>
#include <cstddef>
#include <deque>
#include <set>
#include <span>
#include <stdexcept>
#include <utility>

namespace gt4recomp::ee {

InstructionFlow classify(const DecodedInstruction& instruction, std::uint32_t pc) {
    InstructionFlow flow;
    switch (instruction.operation) {
    // Every relative branch: one delay slot, target from the signed offset.
    case Operation::Beq:
    case Operation::Bne:
    case Operation::Beql:
    case Operation::Bnel:
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
        flow.kind = FlowKind::Branch;
        flow.has_delay_slot = true;
        flow.target_known = true;
        flow.target = relative_branch_target(pc, instruction);
        break;
    case Operation::J:
        flow.kind = FlowKind::Jump;
        flow.has_delay_slot = true;
        flow.target_known = true;
        flow.target = absolute_jump_target(pc, instruction);
        break;
    case Operation::Jal:
        flow.kind = FlowKind::Call;
        flow.has_delay_slot = true;
        flow.target_known = true;
        flow.target = absolute_jump_target(pc, instruction);
        break;
    case Operation::Jalr:
        // The target comes from a register; it is not statically known.
        flow.kind = FlowKind::Call;
        flow.has_delay_slot = true;
        break;
    case Operation::Jr:
        // jr ra is the ABI return convention; anything else is a computed jump.
        flow.kind = instruction.rs == 31 ? FlowKind::Return : FlowKind::IndirectJump;
        flow.has_delay_slot = true;
        break;
    case Operation::Syscall:
        // Control leaves through the exception handler; SYSCALL has no delay slot.
        flow.kind = FlowKind::Exception;
        break;
    case Operation::Unsupported:
        flow.kind = FlowKind::Unsupported;
        break;
    default:
        flow.kind = FlowKind::FallThrough;
        break;
    }
    return flow;
}

const char* flow_name(FlowKind kind) {
    switch (kind) {
    case FlowKind::FallThrough: return "fall-through";
    case FlowKind::Branch: return "branch";
    case FlowKind::Jump: return "jump";
    case FlowKind::Call: return "call";
    case FlowKind::Return: return "return";
    case FlowKind::IndirectJump: return "indirect-jump";
    case FlowKind::Exception: return "exception";
    case FlowKind::Unsupported: return "unsupported";
    }
    return "unsupported";
}

bool is_likely_branch(Operation operation) {
    switch (operation) {
    case Operation::Beql:
    case Operation::Bnel:
    case Operation::Bltzl:
    case Operation::Bgezl:
    case Operation::Bltzall:
    case Operation::Bgezall:
        return true;
    default:
        return false;
    }
}

bool writes_link_register(Operation operation) {
    switch (operation) {
    case Operation::Bltzal:
    case Operation::Bgezal:
    case Operation::Bltzall:
    case Operation::Bgezall:
        return true;
    default:
        return false;
    }
}

BasicBlock build_basic_block(const ImageRecord& text, std::uint32_t start,
                             std::uint32_t max_instructions) {
    const std::uint64_t text_end = static_cast<std::uint64_t>(text.guest_address) + text.bytes.size();
    if (max_instructions == 0 || start % 4 != 0 || text.guest_address % 4 != 0
        || start < text.guest_address || text_end > 0x100000000ull
        || static_cast<std::uint64_t>(start) + 4 > text_end) {
        throw std::runtime_error(
            "Expected a nonzero limit and an aligned start inside file-backed text");
    }

    const auto flow_at = [&](std::uint32_t address) {
        const auto offset = static_cast<std::size_t>(address - text.guest_address);
        const auto bytes = std::span<const std::uint8_t, 4>(text.bytes.data() + offset, 4);
        return classify(decode(read_instruction_word(bytes)), address);
    };

    BasicBlock block;
    block.start = start;
    std::uint32_t pc = start;

    while (true) {
        if (block.instruction_count == max_instructions) {
            block.ending = FlowKind::FallThrough;
            block.continuation = pc;
            block.stop_reason = "instruction-limit";
            break;
        }
        if (static_cast<std::uint64_t>(pc) + 4 > text_end) {
            block.ending = FlowKind::FallThrough;
            block.continuation = pc;
            block.stop_reason = "range";
            break;
        }

        const auto current = flow_at(pc);
        ++block.instruction_count;
        pc += 4;

        if (current.kind == FlowKind::FallThrough) {
            continue;
        }
        if (current.kind == FlowKind::Unsupported) {
            block.ending = FlowKind::Unsupported;
            block.stop_reason = "unsupported";
            break;
        }

        // A control transfer ends the block. Record its statically known facts
        // before consuming the delay slot, which is part of this block.
        block.ending = current.kind;
        block.target_known = current.target_known;
        block.target = current.target;
        if (current.kind == FlowKind::Branch || current.kind == FlowKind::Call) {
            // Branches continue after the delay slot; calls resume there because
            // the encoding writes pc+8 as the return address.
            block.continuation = pc + 4;
        }
        block.stop_reason = flow_name(current.kind);

        if (!current.has_delay_slot) {
            break;
        }
        if (static_cast<std::uint64_t>(pc) + 4 > text_end) {
            // The delay slot is outside file-backed text. Keep the transfer's own
            // facts and report the range stop; the slot is not consumed.
            block.stop_reason = "range";
            break;
        }
        const auto delay = flow_at(pc);
        ++block.instruction_count;
        if (delay.kind == FlowKind::Unsupported) {
            block.delay_slot_unsupported = true;
            pc += 4;
            break;
        }
        if (delay.kind != FlowKind::FallThrough) {
            // A branch or jump inside a delay slot is architecturally undefined;
            // claim nothing about where control goes.
            block.ending = FlowKind::Unsupported;
            block.target_known = false;
            block.target = 0;
            block.continuation = 0;
            block.stop_reason = "branch-in-delay-slot";
            pc += 4;
            break;
        }
        pc += 4;
        break;
    }

    block.end_exclusive = pc;
    return block;
}

ControlFlowGraph build_control_flow_graph(const ImageRecord& text,
                                          std::span<const std::uint32_t> seeds,
                                          std::uint32_t max_blocks) {
    if (max_blocks == 0) {
        throw std::runtime_error("Expected a nonzero block limit");
    }
    const std::uint64_t text_end = static_cast<std::uint64_t>(text.guest_address) + text.bytes.size();
    // Within one traversal a linear run is bounded by the text extents, not by
    // an artificial per-block number.
    const auto max_block_instructions = static_cast<std::uint32_t>(text.bytes.size() / 4);

    ControlFlowGraph graph;
    std::deque<std::uint32_t> pending(seeds.begin(), seeds.end());
    std::set<std::uint32_t> visited;

    while (!pending.empty()) {
        if (graph.nodes.size() == max_blocks) {
            graph.limited = true;
            break;
        }
        const auto start = pending.front();
        pending.pop_front();
        if (!visited.insert(start).second) {
            continue;
        }

        CfgNode node;
        node.block = build_basic_block(text, start, max_block_instructions);

        const auto follow = [&](std::uint32_t address) {
            if (address < text.guest_address
                || static_cast<std::uint64_t>(address) + 4 > text_end) {
                ++graph.outside_text_successors;
                return;
            }
            if (std::find(node.successors.begin(), node.successors.end(), address)
                == node.successors.end()) {
                node.successors.push_back(address);
                pending.push_back(address);
            }
        };

        switch (node.block.ending) {
        case FlowKind::Branch:
            if (node.block.target_known) {
                follow(node.block.target);
            }
            follow(node.block.continuation);
            break;
        case FlowKind::Jump:
            if (node.block.target_known) {
                follow(node.block.target);
            }
            break;
        case FlowKind::Call:
            if (node.block.target_known
                && std::find(graph.call_targets.begin(), graph.call_targets.end(),
                             node.block.target) == graph.call_targets.end()) {
                // Direct callees are recorded for the function map; their
                // bodies are separate flows and are not enqueued here.
                graph.call_targets.push_back(node.block.target);
            }
            follow(node.block.continuation);
            break;
        case FlowKind::FallThrough:
        case FlowKind::Return:
        case FlowKind::IndirectJump:
        case FlowKind::Exception:
        case FlowKind::Unsupported:
            // Truncations and dynamic transfers have no static successor.
            break;
        }

        graph.nodes.push_back(std::move(node));
    }
    return graph;
}

} // namespace gt4recomp::ee
