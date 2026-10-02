#include "gt4recomp/ee_functions.hpp"
#include "hex_value.hpp"
#include "parse_number.hpp"
#include "verified_core.hpp"

#include <cstdint>
#include <iostream>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

using gt4recomp::tools::hex_value;

namespace {

std::string call_targets_text(const std::vector<std::uint32_t>& targets) {
    if (targets.empty()) {
        return "none";
    }
    std::string result;
    for (const auto target : targets) {
        if (!result.empty()) {
            result += ',';
        }
        result += "0x" + hex_value(target, 8);
    }
    return result;
}

} // namespace

int wmain(int argc, wchar_t* argv[]) {
    if (argc != 5) {
        std::cerr << "Usage: gt4funcs CORE.GT4 start-address max-functions max-blocks-per-function\n"
                     "Numbers: decimal or 0x-prefixed hex. Function lines: stdout; summary: stderr.\n";
        return 2;
    }
    try {
        const auto start = gt4recomp::tools::parse_number(argv[2]);
        const auto max_functions = gt4recomp::tools::parse_number(argv[3]);
        const auto max_blocks = gt4recomp::tools::parse_number(argv[4]);
        const auto core = gt4recomp::tools::read_verified_core(argv[1]);
        const auto image = gt4recomp::reconstruct_core(core);

        const gt4recomp::ee::FunctionSeed entry{image.entry_address,
                                                gt4recomp::ee::FunctionEvidence::ElfEntry};
        const gt4recomp::ee::FunctionSeed seed{start, gt4recomp::ee::FunctionEvidence::Seed};
        const gt4recomp::ee::FunctionSeed seeds[] = {entry, seed};
        const auto map = gt4recomp::ee::build_function_map(image.text, seeds, max_functions,
                                                           max_blocks);

        std::uint64_t blocks_sum = 0;
        std::uint64_t instructions_sum = 0;
        std::set<std::uint32_t> discovered_calls;
        for (const auto& function : map.functions) {
            blocks_sum += function.block_count;
            instructions_sum += function.instruction_count;
            for (const auto target : function.call_targets) {
                discovered_calls.insert(target);
            }
            std::cout << "function=0x" << hex_value(function.entry, 8)
                      << " evidence=" << gt4recomp::ee::function_evidence_name(function.evidence)
                      << " blocks=" << function.block_count
                      << " instructions=" << function.instruction_count
                      << " open_ends=" << function.open_ends
                      << " limited=" << (function.limited ? 1 : 0)
                      << " call_targets=" << call_targets_text(function.call_targets)
                      << '\n';
        }
        std::cerr << "funcmap functions=" << map.functions.size()
                  << " blocks_sum=" << blocks_sum
                  << " instructions_sum=" << instructions_sum
                  << " discovered_calls=" << discovered_calls.size()
                  << " pending=" << map.pending.size()
                  << " limited=" << (map.limited ? 1 : 0)
                  << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "ERROR: " << error.what() << '\n';
        return 1;
    }
}
