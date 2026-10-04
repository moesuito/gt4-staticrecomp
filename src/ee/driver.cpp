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

// Builds the reported boundary from the current state after a module stop:
// the pc, the guest word there when mapped, and the v1 service number for a
// pending syscall. The kind comes from the module's exit reason, never from
// an inference over the registers.
Boundary report_boundary(const GuestState& state, BoundaryKind kind) {
    Boundary boundary{kind, state.pc(), 0, 0};
    if ((boundary.pc & 0x3u) == 0 && state.memory().contains(boundary.pc, 4)) {
        boundary.word = state.memory().read_word(boundary.pc);
    }
    if (kind == BoundaryKind::Syscall) {
        // The PS2 ABI passes the service number in v1 (register 3).
        boundary.service = state.read_gpr32(3);
    }
    return boundary;
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
                    return entry.execute(state);
                }
            }
            throw std::logic_error(
                "The driver called an address that is not a module entry");
        },
    };
}

Boundary classify_boundary(const GuestState& state) {
    const std::uint32_t pc = state.pc();

    // No pc == ra inference here: a return is reported by the module that
    // applied it, and a trap or an ordinary word can sit at the link
    // address. The word at the pc names every stop this view can see.
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
    if (options.advance_time) {
        options.advance_time(state_);
    }
    if (outcome == ServiceOutcome::Handled) {
        state_.set_pc(pc + 4);
    }
    return outcome;
}

bool Driver::pending_transfer() const noexcept {
    return interpreter_.pending_transfer();
}

RunResult Driver::run(ServiceTable& services, const RunOptions& options) {
    RunResult result;
    bridge_step_due_ = false;
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
        // Translated code runs only when the pc names a module entry, no
        // interpreted delay slot is in flight, and the bridge does not own
        // the next step; anything else is the bridge's.
        if (!bridge_step_due_ && !interpreter_.pending_transfer()
            && module_.has_entry(state_.pc())) {
            const std::uint32_t entry_pc = state_.pc();
            BoundaryKind module_exit;
            try {
                module_exit = module_.call_entry(state_, entry_pc);
            } catch (const std::exception& error) {
                throw std::runtime_error(fault_context(entry_pc, error));
            }
            ++result.stats.module_calls;
            // The exit reason is the module's own report. A return or an
            // applied eret continues the run; a pending indirect transfer
            // continues through the bridge first (see below). Anything
            // applied already (a service, a link write, a delay slot) is
            // never repeated: the module only stops with the reason where
            // the interpreter stops, and both engines resume exactly there.
            if (module_exit == BoundaryKind::Syscall) {
                const ServiceOutcome outcome = handle_syscall(
                    state_.pc(), state_.read_gpr32(3), services, options,
                    result.stats);
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
                    result.boundary = report_boundary(
                        state_, BoundaryKind::NoRunnableThread);
                    return result;
                }
                result.boundary = report_boundary(state_, BoundaryKind::Syscall);
                return result;
            }
            switch (module_exit) {
            case BoundaryKind::Returned:
            case BoundaryKind::ExceptionReturn:
                // The module handed control back in a state the loop can
                // continue from: a return to the captured target or an eret
                // whose derived pc it already applied.
                continue;
            case BoundaryKind::IndirectTransfer: {
                // A pending transfer the module left for the bridge. The stop
                // applied nothing, so running the same entry again could only
                // repeat it: the bridge owns the next step even when the
                // transfer word is itself a module entry.
                bridge_step_due_ = true;
                continue;
            }
            default:
                result.boundary = report_boundary(state_, module_exit);
                return result;
            }
        }

        // The bridge: one interpreted instruction, exactly as the
        // differential tests verify against the module. This is also where
        // a pending transfer lands: the module applied nothing there, so
        // the link, the slot and the jump all run here, exactly once.
        const std::uint32_t pc = state_.pc();
        bridge_step_due_ = false;
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
