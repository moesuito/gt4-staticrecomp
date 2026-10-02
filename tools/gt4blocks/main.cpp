#include "gt4recomp/ee_disassemble.hpp"
#include "gt4recomp/ee_flow.hpp"
#include "parse_number.hpp"
#include "verified_core.hpp"

#include <iomanip>
#include <iostream>
#include <sstream>
#include <span>
#include <stdexcept>
#include <string>

namespace {

std::string hex_value(std::uint32_t value, int width) {
    std::ostringstream output;
    output << std::hex << std::setfill('0') << std::setw(width) << value;
    return output.str();
}

} // namespace

int wmain(int argc, wchar_t* argv[]) {
    if (argc != 4) {
        std::cerr << "Usage: gt4blocks CORE.GT4 start-address instruction-limit\n"
                     "Numbers: decimal or 0x-prefixed hex. Listing: stdout; block summary: stderr.\n";
        return 2;
    }
    try {
        const auto start = gt4recomp::tools::parse_number(argv[2]);
        const auto limit = gt4recomp::tools::parse_number(argv[3]);
        const auto core = gt4recomp::tools::read_verified_core(argv[1]);
        const auto image = gt4recomp::reconstruct_core(core);
        const auto block = gt4recomp::ee::build_basic_block(image.text, start, limit);

        for (std::uint32_t pc = block.start; pc < block.end_exclusive; pc += 4) {
            const auto offset = static_cast<std::size_t>(pc - image.text.guest_address);
            const auto bytes = std::span<const std::uint8_t, 4>(image.text.bytes.data() + offset, 4);
            const auto word = gt4recomp::ee::read_instruction_word(bytes);
            std::cout << hex_value(pc, 8) << ": " << hex_value(word, 8) << "  "
                      << gt4recomp::ee::format_instruction(word, pc) << '\n';
        }
        std::cerr << "block=0x" << hex_value(block.start, 8)
                  << " end=0x" << hex_value(block.end_exclusive, 8)
                  << " instructions=" << block.instruction_count
                  << " ending=" << gt4recomp::ee::flow_name(block.ending)
                  << " target_known=" << (block.target_known ? 1 : 0)
                  << " target=0x" << hex_value(block.target, 8)
                  << " continuation=0x" << hex_value(block.continuation, 8)
                  << " reason=" << block.stop_reason
                  << " delay_slot_unsupported=" << (block.delay_slot_unsupported ? 1 : 0)
                  << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "ERROR: " << error.what() << '\n';
        return 1;
    }
}
