#pragma once

#include "gt4recomp/ee_decode.hpp"
#include "gt4recomp/executable_image.hpp"

#include <cstdint>
#include <string>

namespace gt4recomp::ee {

// How a decoded instruction leaves normal sequential execution. Every branch,
// jump, call and return owns exactly one delay slot: the word after it runs
// before the transfer happens. Unsupported words are reported, never guessed;
// nothing is assumed about them, including whether they transfer control.
enum class FlowKind {
    FallThrough,   // execution continues at the next word
    Branch,        // conditional relative transfer; target and fall-through exist
    Jump,          // unconditional direct transfer to an absolute target
    Call,          // transfer that records a return address (JAL or JALR)
    Return,        // jump through the return-address register (jr ra)
    IndirectJump,  // computed transfer through another register
    Exception,     // SYSCALL: control leaves through the exception handler
    Unsupported    // outside the decoded subset; the walker stops with context
};

// Classification of one decoded instruction, resolved against its guest PC.
struct InstructionFlow {
    FlowKind kind = FlowKind::Unsupported;
    bool has_delay_slot = false;
    bool target_known = false;
    std::uint32_t target = 0;  // guest address when target_known; not a host pointer
};

[[nodiscard]] InstructionFlow classify(const DecodedInstruction& instruction, std::uint32_t pc);
[[nodiscard]] const char* flow_name(FlowKind kind);

// One straight-line run of instructions ending at a control transfer, an
// unsupported word, the caller's limit or the end of file-backed text. The
// ending transfer's delay slot is consumed and counts toward the block; it is
// always consumed even when that exceeds the caller's limit by one, because a
// transfer without its delay slot would misdescribe execution order.
struct BasicBlock {
    std::uint32_t start = 0;          // first instruction address
    std::uint32_t end_exclusive = 0;  // first address after the block
    std::uint32_t instruction_count = 0;
    FlowKind ending = FlowKind::FallThrough;
    bool target_known = false;
    std::uint32_t target = 0;        // static target of the ending transfer
    std::uint32_t continuation = 0;  // fall-through of a branch, return point of a
                                     // call, or the resume address for limit and
                                     // range stops; zero when none is static
    bool delay_slot_unsupported = false;  // ending transfer followed by an
                                          // undecodable word inside the block
    std::string stop_reason;  // stable token: branch, jump, call, return,
                              // indirect-jump, exception, unsupported,
                              // instruction-limit, range, branch-in-delay-slot
};

// Walk one basic block forward from a file-backed address. The walk never
// follows a transfer; successors are recorded, not visited. Throws
// std::runtime_error for an unaligned start outside file-backed text or a zero
// instruction limit.
[[nodiscard]] BasicBlock build_basic_block(const ImageRecord& text, std::uint32_t start,
                                           std::uint32_t max_instructions);

} // namespace gt4recomp::ee
