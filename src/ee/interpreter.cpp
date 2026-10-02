#include "gt4recomp/ee_interpreter.hpp"
#include "gt4recomp/ee_flow.hpp"

#include <stdexcept>

namespace gt4recomp::ee {
namespace {

constexpr std::uint8_t link_register = 31;

bool is_negative_64(std::uint64_t value) {
    return (value & 0x8000000000000000ull) != 0;
}

bool branch_taken(const DecodedInstruction& instruction, const GuestState& state) {
    const auto left = state.read_gpr64(instruction.rs);
    switch (instruction.operation) {
    case Operation::Beq:
    case Operation::Beql:
        return left == state.read_gpr64(instruction.rt);
    case Operation::Bne:
    case Operation::Bnel:
        return left != state.read_gpr64(instruction.rt);
    case Operation::Blez:
        return is_negative_64(left) || left == 0;
    case Operation::Bgtz:
        return !is_negative_64(left) && left != 0;
    case Operation::Bltz:
    case Operation::Bltzl:
    case Operation::Bltzal:
    case Operation::Bltzall:
        return is_negative_64(left);
    case Operation::Bgez:
    case Operation::Bgezl:
    case Operation::Bgezal:
    case Operation::Bgezall:
        return !is_negative_64(left);
    default:
        throw std::logic_error("branch condition requested for a non-branch operation");
    }
}

// Likely branches (the L and ALL forms) nullify their delay slot when the
// branch is not taken.
bool has_likely_suffix(Operation operation) {
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

std::uint32_t sign_extended_16(std::uint16_t value) {
    return (value & 0x8000u) != 0 ? (0xffff0000u | value) : value;
}

bool less_than_signed_32(std::uint32_t left, std::uint32_t right) {
    // Flipping the sign bit turns the unsigned comparison into a signed one
    // without relying on conversion semantics.
    return (left ^ 0x80000000u) < (right ^ 0x80000000u);
}

// GPR[rs] + sign-extended immediate, truncated to the 32-bit address model.
std::uint32_t effective_address(const GuestState& state, const DecodedInstruction& instruction) {
    const std::uint64_t base = state.read_gpr64(instruction.rs);
    const auto displacement = static_cast<std::uint64_t>(
        static_cast<std::int64_t>(instruction.signed_immediate()));
    return static_cast<std::uint32_t>(base + displacement);
}

void execute_plain(const DecodedInstruction& instruction, GuestState& state) {
    switch (instruction.operation) {
    case Operation::Addu:
        state.write_gpr32(instruction.rd,
                          state.read_gpr32(instruction.rs) + state.read_gpr32(instruction.rt));
        break;
    case Operation::Subu:
        state.write_gpr32(instruction.rd,
                          state.read_gpr32(instruction.rs) - state.read_gpr32(instruction.rt));
        break;
    case Operation::And:
        state.write_gpr64(instruction.rd,
                          state.read_gpr64(instruction.rs) & state.read_gpr64(instruction.rt));
        break;
    case Operation::Or:
        state.write_gpr64(instruction.rd,
                          state.read_gpr64(instruction.rs) | state.read_gpr64(instruction.rt));
        break;
    case Operation::Xor:
        state.write_gpr64(instruction.rd,
                          state.read_gpr64(instruction.rs) ^ state.read_gpr64(instruction.rt));
        break;
    case Operation::Slt:
        state.write_gpr64(instruction.rd,
                          less_than_signed_32(state.read_gpr32(instruction.rs),
                                              state.read_gpr32(instruction.rt)) ? 1 : 0);
        break;
    case Operation::Sltu:
        state.write_gpr64(instruction.rd,
                          state.read_gpr32(instruction.rs) < state.read_gpr32(instruction.rt) ? 1 : 0);
        break;
    case Operation::Daddu:
        state.write_gpr64(instruction.rd,
                          state.read_gpr64(instruction.rs) + state.read_gpr64(instruction.rt));
        break;
    case Operation::Addiu:
        state.write_gpr32(instruction.rt,
                          state.read_gpr32(instruction.rs)
                              + static_cast<std::uint32_t>(instruction.signed_immediate()));
        break;
    case Operation::Andi:
        state.write_gpr64(instruction.rt, state.read_gpr64(instruction.rs) & instruction.immediate);
        break;
    case Operation::Ori:
        state.write_gpr64(instruction.rt, state.read_gpr64(instruction.rs) | instruction.immediate);
        break;
    case Operation::Lui:
        state.write_gpr32(instruction.rt, static_cast<std::uint32_t>(instruction.immediate) << 16);
        break;
    case Operation::Sll:
        state.write_gpr32(instruction.rd,
                          state.read_gpr32(instruction.rt) << instruction.shift_amount);
        break;
    case Operation::Srl:
        state.write_gpr32(instruction.rd,
                          state.read_gpr32(instruction.rt) >> instruction.shift_amount);
        break;
    case Operation::Lw: {
        const auto address = effective_address(state, instruction);
        state.write_gpr32(instruction.rt, state.memory().read_word(address));
        break;
    }
    case Operation::Lh: {
        const auto address = effective_address(state, instruction);
        state.write_gpr32(instruction.rt, sign_extended_16(state.memory().read_halfword(address)));
        break;
    }
    case Operation::Ld: {
        const auto address = effective_address(state, instruction);
        state.write_gpr64(instruction.rt, state.memory().read_doubleword(address));
        break;
    }
    case Operation::Sw: {
        const auto address = effective_address(state, instruction);
        state.memory().write_word(address, state.read_gpr32(instruction.rt));
        break;
    }
    case Operation::Sb: {
        const auto address = effective_address(state, instruction);
        state.memory().write_byte(address,
                                  static_cast<std::uint8_t>(state.read_gpr64(instruction.rt) & 0xff));
        break;
    }
    case Operation::Sd: {
        const auto address = effective_address(state, instruction);
        state.memory().write_doubleword(address, state.read_gpr64(instruction.rt));
        break;
    }
    default:
        throw std::logic_error("non-plain instruction reached the plain executor");
    }
}

} // namespace

Interpreter::Interpreter(GuestState& state) : state_(state) {}

StepResult Interpreter::step() {
    const auto pc = state_.pc();
    const auto instruction = decode(state_.memory().read_word(pc));
    const auto flow = classify(instruction, pc);

    if (transfer_pending_) {
        // We are executing a delay slot. A transfer here is architecturally
        // undefined; stop before executing it. An undecodable word stops as
        // unsupported. Everything else executes before the pending transfer.
        if (flow.kind == FlowKind::Unsupported) {
            return StepResult{StepOutcome::Unsupported, pc, instruction.operation};
        }
        if (flow.kind != FlowKind::FallThrough) {
            return StepResult{StepOutcome::IllegalDelaySlot, pc, instruction.operation};
        }
        execute_plain(instruction, state_);
        transfer_pending_ = false;
        state_.set_pc(transfer_target_);
        return StepResult{StepOutcome::Executed, pc, instruction.operation};
    }

    switch (flow.kind) {
    case FlowKind::Unsupported:
        return StepResult{StepOutcome::Unsupported, pc, instruction.operation};
    case FlowKind::Exception:
        // The exception handler is not modeled; stop at the boundary.
        return StepResult{StepOutcome::Exception, pc, instruction.operation};
    case FlowKind::FallThrough:
        execute_plain(instruction, state_);
        state_.set_pc(pc + 4);
        return StepResult{StepOutcome::Executed, pc, instruction.operation};
    case FlowKind::Branch: {
        if (branch_taken(instruction, state_)) {
            if (writes_link_register(instruction.operation)) {
                state_.write_gpr64(link_register, pc + 8);
            }
            transfer_target_ = flow.target;
            transfer_pending_ = true;
            state_.set_pc(pc + 4);
        } else if (has_likely_suffix(instruction.operation)) {
            state_.set_pc(pc + 8);  // the delay slot is nullified
        } else {
            state_.set_pc(pc + 4);  // the delay slot still runs
        }
        return StepResult{StepOutcome::Executed, pc, instruction.operation};
    }
    case FlowKind::Jump:
        transfer_target_ = flow.target;
        transfer_pending_ = true;
        state_.set_pc(pc + 4);
        return StepResult{StepOutcome::Executed, pc, instruction.operation};
    case FlowKind::Call:
        if (instruction.operation == Operation::Jal) {
            state_.write_gpr64(link_register, pc + 8);
            transfer_target_ = flow.target;
        } else {
            // JALR: read the target before writing the link register, so a
            // shared rd == rs encoding still jumps to the old value.
            const auto target = static_cast<std::uint32_t>(state_.read_gpr64(instruction.rs));
            state_.write_gpr64(instruction.rd, pc + 8);
            transfer_target_ = target;
        }
        transfer_pending_ = true;
        state_.set_pc(pc + 4);
        return StepResult{StepOutcome::Executed, pc, instruction.operation};
    case FlowKind::Return:
    case FlowKind::IndirectJump:
        // JR targets the low 32 bits of the register in the 32-bit model.
        transfer_target_ = static_cast<std::uint32_t>(state_.read_gpr64(instruction.rs));
        transfer_pending_ = true;
        state_.set_pc(pc + 4);
        return StepResult{StepOutcome::Executed, pc, instruction.operation};
    }
    throw std::logic_error("unhandled flow kind");
}

} // namespace gt4recomp::ee
