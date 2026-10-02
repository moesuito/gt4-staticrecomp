#include "gt4recomp/ee_flow.hpp"

#include <cstdint>
#include <initializer_list>
#include <iostream>
#include <stdexcept>
#include <utility>
#include <vector>

using namespace gt4recomp;
using namespace gt4recomp::ee;

namespace {

ImageRecord make_text(std::uint32_t address, std::initializer_list<std::uint32_t> words) {
    std::vector<std::uint8_t> bytes;
    bytes.reserve(words.size() * 4);
    for (const auto word : words) {
        bytes.push_back(static_cast<std::uint8_t>(word & 0xff));
        bytes.push_back(static_cast<std::uint8_t>((word >> 8) & 0xff));
        bytes.push_back(static_cast<std::uint8_t>((word >> 16) & 0xff));
        bytes.push_back(static_cast<std::uint8_t>((word >> 24) & 0xff));
    }
    return ImageRecord{address, std::move(bytes)};
}

} // namespace

int main() {
    int failures = 0;
    const auto check = [&](bool passed, const char* label) {
        if (!passed) { std::cerr << label << '\n'; ++failures; }
    };

    // A branch with a fall-through continuation and a return target reachable
    // only through the branch, plus a block entered in the middle by the
    // fall-through path.
    const auto base = 0x00001000u;
    const auto text = make_text(base, {0x27bdfff0, 0x11090002, 0x0080982d,
                                       0x8fa80000, 0x03e00008, 0x0080982d});
    const std::uint32_t seed[] = {base};
    const auto graph = build_control_flow_graph(text, seed, 10);
    check(graph.nodes.size() == 3, "graph node count");
    check(!graph.limited && graph.outside_text_successors == 0, "graph bounds");
    check(graph.call_targets.empty(), "no call targets");
    check(graph.nodes[0].block.start == base
          && graph.nodes[0].block.end_exclusive == base + 0x0c
          && graph.nodes[0].successors.size() == 2
          && graph.nodes[0].successors[0] == base + 0x10
          && graph.nodes[0].successors[1] == base + 0x0c, "branch node successors");
    check(graph.nodes[1].block.start == base + 0x10
          && graph.nodes[1].block.ending == FlowKind::Return
          && graph.nodes[1].successors.empty(), "return node");
    check(graph.nodes[2].block.start == base + 0x0c
          && graph.nodes[2].block.ending == FlowKind::Return, "fall-through node");

    // A direct call records its target but continues at the return address;
    // the callee body is a separate flow and is not visited here.
    const auto call_text = make_text(0x2000, {0x0c080000, 0x0080982d, 0x27bdfff0});
    const std::uint32_t call_seed[] = {0x2000};
    const auto call_graph = build_control_flow_graph(call_text, call_seed, 10);
    check(call_graph.call_targets.size() == 1 && call_graph.call_targets[0] == 0x00200000,
          "call target recorded");
    check(call_graph.nodes.size() == 2
          && call_graph.nodes[0].successors.size() == 1
          && call_graph.nodes[0].successors[0] == 0x2008,
          "call continues at the return address");
    check(call_graph.nodes[1].block.start == 0x2008
          && call_graph.nodes[1].block.stop_reason == "range",
          "call continuation block");

    // Static successors that leave file-backed text are counted, not followed.
    const auto edge_text = make_text(0x3000, {0x11090004, 0x0080982d});
    const std::uint32_t edge_seed[] = {0x3000};
    const auto edge_graph = build_control_flow_graph(edge_text, edge_seed, 10);
    check(edge_graph.nodes.size() == 1 && edge_graph.nodes[0].successors.empty(),
          "outside successors are not followed");
    check(edge_graph.outside_text_successors == 2, "outside successors counted");

    // The cap bounds visited blocks; queued work that remains sets limited.
    const auto chain_text = make_text(0x4000, {0x27bdfff0, 0x27bdfff0, 0x27bdfff0, 0x27bdfff0});
    const std::uint32_t one_seed[] = {0x4000};
    const auto drained_graph = build_control_flow_graph(chain_text, one_seed, 1);
    check(drained_graph.nodes.size() == 1 && !drained_graph.limited,
          "drained traversal is not limited");
    const std::uint32_t two_seeds[] = {0x4000, 0x4004};
    const auto capped_graph = build_control_flow_graph(chain_text, two_seeds, 1);
    check(capped_graph.nodes.size() == 1 && capped_graph.limited, "cap stops traversal");
    const std::uint32_t duplicate_seeds[] = {0x4000, 0x4000};
    const auto duplicate_graph = build_control_flow_graph(chain_text, duplicate_seeds, 10);
    check(duplicate_graph.nodes.size() == 1, "duplicate seeds collapse");

    // Empty seeds produce an empty graph; a zero cap is rejected.
    const std::span<const std::uint32_t> no_seeds;
    const auto empty_graph = build_control_flow_graph(chain_text, no_seeds, 10);
    check(empty_graph.nodes.empty() && !empty_graph.limited, "empty seeds");
    bool zero_limit_rejected = false;
    try {
        (void)build_control_flow_graph(chain_text, no_seeds, 0);
    } catch (const std::runtime_error&) {
        zero_limit_rejected = true;
    }
    check(zero_limit_rejected, "zero block limit rejected");

    // A malformed seed is rejected by the underlying block walk.
    const std::uint32_t bad_seed[] = {0x4001};
    bool bad_seed_rejected = false;
    try {
        (void)build_control_flow_graph(chain_text, bad_seed, 10);
    } catch (const std::runtime_error&) {
        bad_seed_rejected = true;
    }
    check(bad_seed_rejected, "unaligned seed rejected");

    if (failures != 0) {
        return 1;
    }
    std::cout << "control-flow graph fixtures passed\n";
    return 0;
}
