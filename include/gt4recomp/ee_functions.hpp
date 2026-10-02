#pragma once

#include "gt4recomp/ee_flow.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace gt4recomp::ee {

// Why an address is treated as a function entry. Only recorded evidence is
// used; a guess is never an entry.
enum class FunctionEvidence {
    ElfEntry,    // the image's declared entry address
    Seed,        // caller-provided starting point
    DirectCall   // a direct JAL target observed in an analyzed block
};

struct FunctionSeed {
    std::uint32_t address = 0;
    FunctionEvidence evidence = FunctionEvidence::Seed;
};

// One discovered function: a bounded traversal started at its entry. Counts
// describe the blocks reachable from that entry within the per-function cap.
// Reachable sets may overlap between functions (tail jumps, shared stubs), so
// they are evidence of reachability, not proven function boundaries.
struct FunctionRecord {
    std::uint32_t entry = 0;
    FunctionEvidence evidence = FunctionEvidence::Seed;
    std::size_t block_count = 0;
    std::uint64_t instruction_count = 0;
    std::size_t open_ends = 0;                // reachable blocks without a static successor
    std::vector<std::uint32_t> call_targets;  // unique direct targets inside this traversal
    bool limited = false;                     // the per-function block cap was reached
};

struct FunctionMap {
    std::vector<FunctionRecord> functions;  // discovery order
    std::vector<std::uint32_t> pending;     // discovered entries not yet traversed when capped
    bool limited = false;                   // the function cap was reached
};

// Iteratively traverse the selected text: each analyzed function contributes
// its direct call targets as new candidates until none remain or a cap is
// reached. Register calls (JALR) cannot produce entries. Throws
// std::runtime_error for zero caps or an invalid seed.
[[nodiscard]] FunctionMap build_function_map(
    const ImageRecord& text, std::span<const FunctionSeed> seeds,
    std::uint32_t max_functions, std::uint32_t max_blocks_per_function);

[[nodiscard]] const char* function_evidence_name(FunctionEvidence evidence);

} // namespace gt4recomp::ee
