#include "gt4recomp/ee_interpreter.hpp"
#include "gt4recomp/ee_state.hpp"

#include <cstdint>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#if defined(_MSC_VER)
#include <crtdbg.h>
#include <cstdlib>
#endif

using namespace gt4recomp;
using namespace gt4recomp::ee;

namespace {

constexpr std::uint32_t code_base = 0x00100000;
constexpr std::size_t region_size = 0x1000;

struct MemoryExpectation {
    std::uint32_t address = 0;
    int width = 1;
    std::uint64_t value = 0;
};

struct ProgramFixture {
    std::string name;
    std::vector<std::pair<std::uint8_t, std::uint64_t>> initial_registers;
    std::uint32_t data_address = 0;
    std::vector<std::uint8_t> data_bytes;
    std::vector<std::uint32_t> words;
    std::vector<std::pair<std::uint8_t, std::uint64_t>> expected_registers;
    std::vector<MemoryExpectation> expected_memory;
    std::uint32_t expected_pc = 0;
};

std::uint64_t parse_hex(const std::string& token) {
    return std::stoull(token, nullptr, 16);
}

std::vector<std::uint8_t> parse_hex_bytes(const std::string& token) {
    if (token.size() % 2 != 0) {
        throw std::runtime_error("Odd-length hex byte string");
    }
    std::vector<std::uint8_t> bytes;
    bytes.reserve(token.size() / 2);
    for (std::size_t index = 0; index < token.size(); index += 2) {
        bytes.push_back(static_cast<std::uint8_t>(
            std::stoul(token.substr(index, 2), nullptr, 16)));
    }
    return bytes;
}

std::vector<ProgramFixture> read_fixture(const std::string& path) {
    std::ifstream input(path);
    if (!input) {
        throw std::runtime_error("Cannot open fixture: " + path);
    }
    std::vector<ProgramFixture> programs;
    ProgramFixture current;
    bool open = false;
    std::string line;
    int line_number = 0;
    while (std::getline(input, line)) {
        ++line_number;
        std::istringstream tokens(line);
        std::string keyword;
        if (!(tokens >> keyword) || keyword.starts_with('#')) {
            continue;
        }
        try {
            if (keyword == "program") {
                current = ProgramFixture{};
                tokens >> current.name;
                open = true;
            } else if (keyword == "end") {
                programs.push_back(std::move(current));
                current = ProgramFixture{};
                open = false;
            } else if (!open) {
                throw std::runtime_error("Data line outside a program block");
            } else if (keyword == "init" || keyword == "expect") {
                std::string register_text;
                std::string value_text;
                tokens >> register_text >> value_text;
                const auto reg = static_cast<std::uint8_t>(std::stoul(register_text.substr(1)));
                const auto value = parse_hex(value_text);
                if (keyword == "init") {
                    current.initial_registers.emplace_back(reg, value);
                } else {
                    current.expected_registers.emplace_back(reg, value);
                }
            } else if (keyword == "data") {
                std::string address_text;
                std::string bytes_text;
                tokens >> address_text >> bytes_text;
                current.data_address = static_cast<std::uint32_t>(parse_hex(address_text));
                current.data_bytes = parse_hex_bytes(bytes_text);
            } else if (keyword == "word") {
                std::string word_text;
                tokens >> word_text;
                current.words.push_back(static_cast<std::uint32_t>(parse_hex(word_text)));
            } else if (keyword == "expect_mem") {
                MemoryExpectation expectation;
                std::string address_text;
                std::string width_text;
                std::string value_text;
                tokens >> address_text >> width_text >> value_text;
                expectation.address = static_cast<std::uint32_t>(parse_hex(address_text));
                expectation.width = std::stoi(width_text);
                expectation.value = parse_hex(value_text);
                current.expected_memory.push_back(expectation);
            } else if (keyword == "expect_pc") {
                std::string value_text;
                tokens >> value_text;
                current.expected_pc = static_cast<std::uint32_t>(parse_hex(value_text));
            } else {
                throw std::runtime_error("Unknown keyword: " + keyword);
            }
        } catch (const std::exception& error) {
            throw std::runtime_error("Fixture error at line " + std::to_string(line_number)
                                     + ": " + error.what());
        }
    }
    return programs;
}

std::uint64_t read_expected(const GuestMemory& memory, const MemoryExpectation& expectation) {
    switch (expectation.width) {
    case 1: return memory.read_byte(expectation.address);
    case 2: return memory.read_halfword(expectation.address);
    case 4: return memory.read_word(expectation.address);
    case 8: return memory.read_doubleword(expectation.address);
    default: throw std::runtime_error("Unsupported expectation width");
    }
}

bool run_program(const ProgramFixture& program) {
    GuestState state(GuestMemory(code_base, region_size));
    for (const auto& [reg, value] : program.initial_registers) {
        state.write_gpr64(reg, value);
    }
    if (!program.data_bytes.empty()) {
        state.memory().write_bytes(program.data_address, program.data_bytes);
    }
    for (std::size_t index = 0; index < program.words.size(); ++index) {
        state.memory().write_word(code_base + static_cast<std::uint32_t>(index * 4),
                                  program.words[index]);
    }
    state.set_pc(code_base);

    Interpreter interpreter(state);
    bool failed = false;
    for (std::size_t index = 0; index < program.words.size(); ++index) {
        const auto result = interpreter.step();
        if (result.outcome != StepOutcome::Executed) {
            std::cerr << program.name << ": unexpected stop at 0x" << std::hex
                      << result.pc << std::dec << '\n';
            return false;
        }
    }
    for (const auto& [reg, value] : program.expected_registers) {
        if (state.read_gpr64(reg) != value) {
            std::cerr << program.name << ": r" << static_cast<int>(reg) << " mismatch\n";
            failed = true;
        }
    }
    for (const auto& expectation : program.expected_memory) {
        if (read_expected(state.memory(), expectation) != expectation.value) {
            std::cerr << program.name << ": memory mismatch at 0x" << std::hex
                      << expectation.address << std::dec << '\n';
            failed = true;
        }
    }
    if (state.pc() != program.expected_pc) {
        std::cerr << program.name << ": pc mismatch\n";
        failed = true;
    }
    return !failed;
}

} // namespace

int main(int argc, char* argv[]) {
#if defined(_MSC_VER) && defined(_DEBUG)
    // Report CRT errors to stderr instead of a modal dialog; a crash must fail
    // the test run, not block it.
    _CrtSetReportMode(_CRT_ERROR, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ERROR, _CRTDBG_FILE_STDERR);
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
#endif
    if (argc != 2) {
        std::cerr << "Usage: ee_synth_tests <fixture-file>\n";
        return 2;
    }
    try {
        const auto programs = read_fixture(argv[1]);
        if (programs.empty()) {
            std::cerr << "fixture contains no programs\n";
            return 1;
        }
        int passed = 0;
        for (const auto& program : programs) {
            if (run_program(program)) {
                ++passed;
            }
        }
        if (passed != static_cast<int>(programs.size())) {
            return 1;
        }
        std::cout << passed << " synthetic programs passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "fixture failure: " << error.what() << '\n';
        return 1;
    }
}
