#include "gt4recomp/ee_services.hpp"

#include <stdexcept>
#include <string>
#include <utility>

namespace gt4recomp::ee {
namespace {

// The context helpers below name the register by its ABI role so the checks
// read like the kernel prototypes they model.
std::uint32_t register_argument(const GuestState& state, std::uint8_t index) {
    return state.read_gpr32(index);
}

std::string unsigned_hex(std::uint32_t value) {
    static const char digits[] = "0123456789abcdef";
    std::string text = "0x";
    bool started = false;
    for (int shift = 28; shift >= 0; shift -= 4) {
        const std::uint32_t digit = (value >> shift) & 0xfu;
        if (digit != 0 || started || shift == 0) {
            text.push_back(digits[digit]);
            started = true;
        }
    }
    return text;
}

} // namespace

void ServiceTable::add(std::uint32_t number, ServiceHandler handler) {
    for (Entry& entry : entries_) {
        if (entry.number == number) {
            entry.handler = std::move(handler);
            return;
        }
    }
    entries_.push_back(Entry{number, std::move(handler)});
}

const ServiceHandler* ServiceTable::find(std::uint32_t number) const noexcept {
    for (const Entry& entry : entries_) {
        if (entry.number == number) {
            return &entry.handler;
        }
    }
    return nullptr;
}

void setup_thread(GuestState& state) {
    const std::uint32_t stack = register_argument(state, 5);       // a1
    const std::uint32_t stack_size = register_argument(state, 6);  // a2
    // a0 (gp), a3 (args) and t0 (root function) are recorded by the BIOS;
    // the model has no scheduler that could consult them.
    if (stack_size == 0) {
        throw std::runtime_error("SetupThread with a zero stack size");
    }
    const std::uint64_t region_end =
        static_cast<std::uint64_t>(stack) + stack_size;
    if (region_end > 0x100000000ull) {
        throw std::runtime_error(
            "SetupThread stack region " + unsigned_hex(stack) + " + "
            + unsigned_hex(stack_size) + " leaves the 32-bit address space");
    }
    if (!state.memory().contains(stack, stack_size)) {
        throw std::runtime_error(
            "SetupThread stack region " + unsigned_hex(stack) + " + "
            + unsigned_hex(stack_size) + " is outside the mapped guest memory");
    }
    const std::uint32_t stack_pointer =
        static_cast<std::uint32_t>(region_end & ~0xfull);
    state.write_gpr64(2, stack_pointer);  // v0
}

void setup_heap(GuestState& state) {
    const std::uint32_t heap_start = register_argument(state, 4);  // a0
    const std::uint32_t heap_size = register_argument(state, 5);   // a1
    if (heap_start == 0) {
        throw std::runtime_error("SetupHeap without a heap start address");
    }
    if (heap_size == 0) {
        throw std::runtime_error("SetupHeap with a zero heap size");
    }
    if (!state.memory().contains(heap_start, 4)) {
        throw std::runtime_error("SetupHeap start " + unsigned_hex(heap_start)
                                 + " is outside the mapped guest memory");
    }
    // -1 means "to the end of memory" (the crt0's own convention). The heap
    // structure is not created yet: no verified path reads it back.
}

void flush_cache(GuestState&) {
    // No caches in the model; the operation completes.
}

} // namespace gt4recomp::ee
