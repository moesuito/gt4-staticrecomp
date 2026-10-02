#pragma once

#include "gt4recomp/ee_decode.hpp"
#include "gt4recomp/ee_state.hpp"

#include <cstdint>

namespace gt4recomp::ee {

// Why a step could not complete normally. Stopped outcomes are stable: the PC
// is left at the offending word, so stepping again repeats the same result
// until the caller changes something.
enum class StepOutcome {
    Executed,         // one instruction completed; PC advanced or a transfer applied
    Unsupported,      // outside the decoded subset; stopped at the word
    Exception,        // SYSCALL boundary; the handler is not modeled
    IllegalDelaySlot  // a transfer inside a delay slot; stopped before executing it
};

struct StepResult {
    StepOutcome outcome = StepOutcome::Executed;
    std::uint32_t pc = 0;  // address of the executed or offending word
    Operation operation = Operation::Unsupported;
};

// One-instruction-at-a-time interpreter over GuestState. Control transfers take
// effect after their delay slot; a likely branch skips its delay slot when not
// taken. Invalid memory accesses propagate as std::runtime_error from
// GuestMemory; the interpreter never guesses past a boundary.
class Interpreter {
public:
    explicit Interpreter(GuestState& state);

    [[nodiscard]] StepResult step();

private:
    GuestState& state_;
    bool transfer_pending_ = false;
    std::uint32_t transfer_target_ = 0;
};

} // namespace gt4recomp::ee
