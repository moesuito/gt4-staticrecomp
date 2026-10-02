#include "gt4recomp/ee_functions.hpp"

#include <deque>
#include <set>
#include <span>
#include <stdexcept>
#include <utility>

namespace gt4recomp::ee {

const char* function_evidence_name(FunctionEvidence evidence) {
    switch (evidence) {
    case FunctionEvidence::ElfEntry: return "elf-entry";
    case FunctionEvidence::Seed: return "seed";
    case FunctionEvidence::DirectCall: return "direct-call";
    }
    return "seed";
}

FunctionMap build_function_map(const ImageRecord& text, std::span<const FunctionSeed> seeds,
                               std::uint32_t max_functions,
                               std::uint32_t max_blocks_per_function) {
    if (max_functions == 0 || max_blocks_per_function == 0) {
        throw std::runtime_error("Expected nonzero function and block limits");
    }

    // Seeds are validated before any traversal, so an invalid start is
    // reported even when a cap would otherwise stop before reaching it.
    const std::uint64_t text_end = static_cast<std::uint64_t>(text.guest_address) + text.bytes.size();
    for (const auto& seed : seeds) {
        if (seed.address % 4 != 0 || seed.address < text.guest_address
            || static_cast<std::uint64_t>(seed.address) + 4 > text_end) {
            throw std::runtime_error("Expected aligned seeds inside file-backed text");
        }
    }

    FunctionMap map;
    std::deque<FunctionSeed> candidates(seeds.begin(), seeds.end());
    std::set<std::uint32_t> visited;

    while (!candidates.empty()) {
        if (map.functions.size() == max_functions) {
            std::set<std::uint32_t> pending_seen;
            for (const auto& candidate : candidates) {
                if (!visited.contains(candidate.address)
                    && pending_seen.insert(candidate.address).second) {
                    map.pending.push_back(candidate.address);
                }
            }
            map.limited = true;
            break;
        }
        const auto candidate = candidates.front();
        candidates.pop_front();
        if (!visited.insert(candidate.address).second) {
            continue;
        }

        const auto graph = build_control_flow_graph(
            text, std::span<const std::uint32_t>(&candidate.address, 1), max_blocks_per_function);

        FunctionRecord record;
        record.entry = candidate.address;
        record.evidence = candidate.evidence;
        record.block_count = graph.nodes.size();
        record.limited = graph.limited;
        for (const auto& node : graph.nodes) {
            record.instruction_count += node.block.instruction_count;
            if (node.successors.empty()) {
                ++record.open_ends;
            }
        }
        for (const auto target : graph.call_targets) {
            record.call_targets.push_back(target);
            if (!visited.contains(target)) {
                candidates.push_back({target, FunctionEvidence::DirectCall});
            }
        }
        map.functions.push_back(std::move(record));
    }
    return map;
}

} // namespace gt4recomp::ee
