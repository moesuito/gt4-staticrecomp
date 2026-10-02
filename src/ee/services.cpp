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

void ServiceTable::remove(std::uint32_t number) noexcept {
    for (auto entry = entries_.begin(); entry != entries_.end(); ++entry) {
        if (entry->number == number) {
            entries_.erase(entry);
            return;
        }
    }
}

const ServiceHandler* ServiceTable::find(std::uint32_t number) const noexcept {
    for (const Entry& entry : entries_) {
        if (entry.number == number) {
            return &entry.handler;
        }
    }
    return nullptr;
}

ServiceOutcome setup_heap(GuestState& state) {
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
    return ServiceOutcome::Handled;
}

ServiceOutcome flush_cache(GuestState&) {
    // No caches in the model; the operation completes.
    return ServiceOutcome::Handled;
}

} // namespace gt4recomp::ee
