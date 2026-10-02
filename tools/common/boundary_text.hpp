#pragma once

// Human-readable names for the driver's boundary kinds and the interpreter's
// step outcomes, shared by the driver tools (gt4run, gt4boot).

#include "gt4recomp/ee_driver.hpp"
#include "gt4recomp/ee_interpreter.hpp"

namespace gt4recomp::tools {

inline const char* boundary_kind_name(ee::BoundaryKind kind) {
    switch (kind) {
    case ee::BoundaryKind::Syscall: return "syscall";
    case ee::BoundaryKind::Break: return "break";
    case ee::BoundaryKind::ExceptionReturn: return "exception-return";
    case ee::BoundaryKind::UnsupportedWord: return "unsupported-word";
    case ee::BoundaryKind::IndirectTransfer: return "indirect-transfer";
    case ee::BoundaryKind::Returned: return "returned";
    case ee::BoundaryKind::InstructionStop: return "instruction-stop";
    case ee::BoundaryKind::IllegalDelaySlot: return "illegal-delay-slot";
    case ee::BoundaryKind::Unmapped: return "unmapped";
    case ee::BoundaryKind::StepLimit: return "step-limit";
    }
    return "unknown";
}

inline const char* step_outcome_name(ee::StepOutcome outcome) {
    switch (outcome) {
    case ee::StepOutcome::Executed: return "still-executing";
    case ee::StepOutcome::Unsupported: return "unsupported-word";
    case ee::StepOutcome::Exception: return "exception";
    case ee::StepOutcome::IllegalDelaySlot: return "illegal-delay-slot";
    }
    return "unknown";
}

} // namespace gt4recomp::tools
