#include "gt4recomp/ee_flow.hpp"
#include "hex_value.hpp"
#include "parse_number.hpp"
#include "verified_core.hpp"

#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

using gt4recomp::tools::hex_value;

namespace {

std::string successors_text(const std::vector<std::uint32_t>& successors) {
    if (successors.empty()) {
        return "none";
    }
    std::string result;
    for (const auto address : successors) {
        if (!result.empty()) {
            result += ',';
        }
        result += "0x" + hex_value(address, 8);
    }
    return result;
}

} // namespace

int wmain(int argc, wchar_t* argv[]) {
    if (argc != 4) {
        std::cerr << "Usage: gt4cfg CORE.GT4 start-address max-blocks\n"
                     "Numbers: decimal or 0x-prefixed hex. Block lines: stdout; summary: stderr.\n";
        return 2;
    }
    try {
        const auto start = gt4recomp::tools::parse_number(argv[2]);
        const auto max_blocks = gt4recomp::tools::parse_number(argv[3]);
        const auto core = gt4recomp::tools::read_verified_core(argv[1]);
        const auto image = gt4recomp::reconstruct_core(core);

        const std::uint32_t seeds[] = {start};
        const auto graph = gt4recomp::ee::build_control_flow_graph(image.text, seeds, max_blocks);

        std::uint64_t instructions = 0;
        std::size_t edges = 0;
        std::size_t open_ends = 0;
        for (const auto& node : graph.nodes) {
            instructions += node.block.instruction_count;
            edges += node.successors.size();
            if (node.successors.empty()) {
                ++open_ends;
            }
            std::cout << "block=0x" << hex_value(node.block.start, 8)
                      << " end=0x" << hex_value(node.block.end_exclusive, 8)
                      << " instructions=" << node.block.instruction_count
                      << " ending=" << gt4recomp::ee::flow_name(node.block.ending)
                      << " target_known=" << (node.block.target_known ? 1 : 0)
                      << " target=0x" << hex_value(node.block.target, 8)
                      << " continuation=0x" << hex_value(node.block.continuation, 8)
                      << " reason=" << node.block.stop_reason
                      << " successors=" << successors_text(node.successors)
                      << '\n';
        }
        std::cerr << "cfg blocks=" << graph.nodes.size()
                  << " instructions=" << instructions
                  << " edges=" << edges
                  << " call_targets=" << graph.call_targets.size()
                  << " open_ends=" << open_ends
                  << " outside_text=" << graph.outside_text_successors
                  << " limited=" << (graph.limited ? 1 : 0)
                  << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "ERROR: " << error.what() << '\n';
        return 1;
    }
}
