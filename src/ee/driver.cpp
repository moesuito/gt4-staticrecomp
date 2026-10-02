#include "gt4recomp/ee_driver.hpp"

#include <iomanip>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <utility>
#include <vector>

namespace gt4recomp::ee {
namespace {

// A guest access that leaves the mapped window propagates as an error from
// GuestMemory; naming the pc makes the boundary diagnosable without a
// debugger.
std::string fault_context(std::uint32_t pc, const std::exception& error) {
    std::ostringstream message;
    message << "Guest fault at pc 0x" << std::hex << std::setfill('0')
            << std::setw(8) << pc << ": " << error.what();
    return message.str();
}

} // namespace

Module module_from_entries(std::span<const ModuleEntry> entries) {
    // The closures own a copy of the list, so the returned module outlives
    // the caller's array.
    auto table = std::make_shared<const std::vector<ModuleEntry>>(
        entries.begin(), entries.end());
    return Module{
        [table](std::uint32_t address) {
            for (const ModuleEntry& entry : *table) {
                if (entry.address == address) {
                    return true;
                }
            }
            return false;
        },
        [table](GuestState& state, std::uint32_t address) {
            for (const ModuleEntry& entry : *table) {
                if (entry.address == address) {
                    entry.execute(state);
                    return;
                }
            }
            throw std::logic_error(
                "The driver called an address that is not a module entry");
        },
    };
}

Boundary classify_boundary(const GuestState& state) {
    const std::uint32_t pc = state.pc();

    // A normal return through jr ra leaves the pc at the link register's
    // value; that is the only signal a plain translated function gives. A
    // trapping stop whose address happens to equal ra is indistinguishable
    // and would be reported as a return; no such case has been observed.
    if (pc == static_cast<std::uint32_t>(state.read_gpr64(31))) {
        return Boundary{BoundaryKind::Returned, pc, 0, 0};
    }
    if ((pc & 0x3u) != 0 || !state.memory().contains(pc, 4)) {
        return Boundary{BoundaryKind::Unmapped, pc, 0, 0};
    }

    const std::uint32_t word = state.memory().read_word(pc);
    const auto instruction = decode(word);
    switch (instruction.operation) {
    case Operation::Syscall:
        // The PS2 ABI passes the service number in v1 (register 3).
        return Boundary{BoundaryKind::Syscall, pc, word, state.read_gpr32(3)};
    case Operation::Break:
        return Boundary{BoundaryKind::Break, pc, word, 0};
    case Operation::Eret:
        return Boundary{BoundaryKind::ExceptionReturn, pc, word, 0};
    case Operation::Jr:
    case Operation::Jalr:
        // The module dispatches known runtime targets itself, so a stop at
        // the transfer word means the target was not in the module.
        return Boundary{BoundaryKind::IndirectTransfer, pc, word, 0};
    case Operation::Unsupported:
        return Boundary{BoundaryKind::UnsupportedWord, pc, word, 0};
    default:
        // The translator stops at an ordinary word only for a trapping
        // arithmetic overflow; the pc alone does not carry that reason.
        return Boundary{BoundaryKind::InstructionStop, pc, word, 0};
    }
}

Boundary boundary_from_step(const StepResult& step, const GuestState& state) {
    Boundary boundary;
    boundary.pc = step.pc;
    if (step.outcome == StepOutcome::Unsupported) {
        boundary.kind = BoundaryKind::UnsupportedWord;
        if ((step.pc & 0x3u) == 0 && state.memory().contains(step.pc, 4)) {
            boundary.word = state.memory().read_word(step.pc);
        }
        return boundary;
    }
    if (step.outcome == StepOutcome::IllegalDelaySlot) {
        boundary.kind = BoundaryKind::IllegalDelaySlot;
        return boundary;
    }
    // Exception: the operation says which stable stop it is. The syscall
    // number is read from v1, the PS2 ABI's convention.
    switch (step.operation) {
    case Operation::Syscall:
        boundary.kind = BoundaryKind::Syscall;
        boundary.service = state.read_gpr32(3);
        break;
    case Operation::Break:
        boundary.kind = BoundaryKind::Break;
        break;
    default:
        boundary.kind = BoundaryKind::InstructionStop;
        break;
    }
    return boundary;
}

Driver::Driver(GuestState& state, Module module)
    : state_(state), module_(std::move(module)), interpreter_(state) {}

ServiceOutcome Driver::handle_syscall(std::uint32_t pc, std::uint32_t service,
                                      ServiceTable& services, const RunOptions& options,
                                      DriverStats& stats) {
    if (stats.services_handled >= options.service_limit) {
        return ServiceOutcome::Unhandled;
    }
    const ServiceHandler* handler = services.find(service);
    if (handler == nullptr) {
        return ServiceOutcome::Unhandled;
    }
    if (options.on_service) {
        options.on_service(service, pc);
    }
    const ServiceOutcome outcome = (*handler)(state_);
    if (outcome == ServiceOutcome::Unhandled) {
        return ServiceOutcome::Unhandled;
    }
    ++stats.services_handled;
    if (outcome == ServiceOutcome::Handled) {
        state_.set_pc(pc + 4);
    }
    return outcome;
}

RunResult Driver::run(ServiceTable& services, const RunOptions& options) {
    RunResult result;
    while (true) {
        if (result.stats.module_calls + result.stats.interpreted_steps
            >= options.step_limit) {
            result.boundary = Boundary{BoundaryKind::StepLimit, state_.pc(), 0, 0};
            return result;
        }
        // An interrupt can only be delivered at a clean unit boundary.
        if (!interpreter_.pending_transfer() && options.start_interrupt
            && options.start_interrupt(state_)) {
            continue;
        }
        // Translated code runs only when the pc names a module entry and no
        // interpreted delay slot is in flight; anything else is the bridge's.
        if (!interpreter_.pending_transfer() && module_.has_entry(state_.pc())) {
            const std::uint32_t entry_pc = state_.pc();
            try {
                module_.call_entry(state_, entry_pc);
            } catch (const std::exception& error) {
                throw std::runtime_error(fault_context(entry_pc, error));
            }
            ++result.stats.module_calls;
            const Boundary boundary = classify_boundary(state_);
            if (boundary.kind == BoundaryKind::Syscall) {
                const ServiceOutcome outcome = handle_syscall(
                    boundary.pc, boundary.service, services, options, result.stats);
                if (outcome == ServiceOutcome::Handled
                    || outcome == ServiceOutcome::Switched
                    || outcome == ServiceOutcome::Jumped) {
                    continue;
                }
                if (outcome == ServiceOutcome::NoRunnableThread) {
                    if (options.start_idle_interrupt
                        && options.start_idle_interrupt(state_)) {
                        continue;
                    }
                    result.boundary = Boundary{BoundaryKind::NoRunnableThread,
                                               boundary.pc, boundary.word,
                                               boundary.service};
                    return result;
                }
                result.boundary = boundary;
                return result;
            }
            switch (boundary.kind) {
            case BoundaryKind::Returned:
            case BoundaryKind::IndirectTransfer:
            case BoundaryKind::ExceptionReturn:
                // The module handed control back in a state the interpreter
                // can continue from: a return to ra, a runtime transfer the
                // module could not dispatch, or an eret whose derived pc it
                // already applied.
                continue;
            default:
                result.boundary = boundary;
                return result;
            }
        }

        // The bridge: one interpreted instruction, exactly as the
        // differential tests verify against the module.
        const std::uint32_t pc = state_.pc();
        if ((pc & 0x3u) != 0 || !state_.memory().contains(pc, 4)) {
            result.boundary = Boundary{BoundaryKind::Unmapped, pc, 0, 0};
            return result;
        }
        const StepResult step = [&] {
            try {
                return interpreter_.step();
            } catch (const std::exception& error) {
                throw std::runtime_error(fault_context(pc, error));
            }
        }();
        ++result.stats.interpreted_steps;
        if (step.outcome == StepOutcome::Executed) {
            continue;
        }
        if (step.outcome == StepOutcome::Exception
            && step.operation == Operation::Syscall
            && !interpreter_.pending_transfer()) {
            const ServiceOutcome outcome = handle_syscall(
                step.pc, state_.read_gpr32(3), services, options, result.stats);
            if (outcome == ServiceOutcome::Handled
                || outcome == ServiceOutcome::Switched
                || outcome == ServiceOutcome::Jumped) {
                continue;
            }
            if (outcome == ServiceOutcome::NoRunnableThread) {
                if (options.start_idle_interrupt
                    && options.start_idle_interrupt(state_)) {
                    continue;
                }
                result.boundary = boundary_from_step(step, state_);
                result.boundary.kind = BoundaryKind::NoRunnableThread;
                return result;
            }
        }
        result.boundary = boundary_from_step(step, state_);
        return result;
    }
}

} // namespace gt4recomp::ee
