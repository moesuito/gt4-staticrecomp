#pragma once

// The BIOS service layer: the EE kernel calls the game issues with the
// syscall instruction, implemented as host functions. The handler runs with
// the guest at the syscall word (pc at the syscall, v1 the service number)
// and performs the service's effect; the driver then continues at pc + 4.
// A service number without a registered handler is a boundary the caller
// must resolve; it is never guessed.

#include "gt4recomp/ee_state.hpp"

#include <cstdint>
#include <functional>
#include <vector>

namespace gt4recomp::ee {

// What a service handler did. The driver uses this to decide whether the
// program continues at pc + 4, another thread's context became live, or the
// run cannot continue.
enum class ServiceOutcome {
    Handled,           // the service completed; continue at pc + 4
    Switched,          // the current thread blocked; another context is live
    Jumped,            // the service set pc and ra: continue in guest code
    NoRunnableThread,  // the current thread blocked and nothing can run
    Unhandled          // no handler: the syscall stays a boundary
};

using ServiceHandler = std::function<ServiceOutcome(GuestState& state)>;

// The registered services, looked up by the number in v1.
class ServiceTable {
public:
    void add(std::uint32_t number, ServiceHandler handler);
    // Drops the handler for the number; the syscall becomes a boundary again.
    void remove(std::uint32_t number) noexcept;
    [[nodiscard]] const ServiceHandler* find(std::uint32_t number) const noexcept;

private:
    struct Entry {
        std::uint32_t number;
        ServiceHandler handler;
    };
    std::vector<Entry> entries_;
};

// SetupHeap(heap_start, heap_size): validates the heap request. No verified
// path reads the kernel heap structure back yet, so the model records
// nothing; EndOfHeap and the allocator become services when a caller needs
// them. The boot call passes the end of .bss and -1 ("to the end of
// memory"). High confidence on the call contract (public ps2sdk ABI); the
// heap structure itself is Unknown.
ServiceOutcome setup_heap(GuestState& state);  // 0x3D

// FlushCache(operation): the model has no caches, so the call completes with
// no effect, exactly like the reference's CACHE hint (M15/M17 policy).
ServiceOutcome flush_cache(GuestState& state);  // 0x64

} // namespace gt4recomp::ee
