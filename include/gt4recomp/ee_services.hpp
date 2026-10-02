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

using ServiceHandler = std::function<void(GuestState& state)>;

// The registered services, looked up by the number in v1.
class ServiceTable {
public:
    void add(std::uint32_t number, ServiceHandler handler);
    [[nodiscard]] const ServiceHandler* find(std::uint32_t number) const noexcept;

private:
    struct Entry {
        std::uint32_t number;
        ServiceHandler handler;
    };
    std::vector<Entry> entries_;
};

// SetupThread(gp, stack, stack_size, args, root): registers the caller as
// the root thread and returns the thread's stack pointer in v0, which
// ps2sdk's crt0 stores into sp. The model returns the top of the
// caller-provided stack region aligned down to 16 bytes: the region must be
// mapped, and the exact offset below the region top that the BIOS applies is
// not observable in the verified paths (the menu RAM dump shows the boot
// stack anchored at the top of the region). High confidence, from the public
// ps2sdk ABI plus the preserved boot frames; the thread bookkeeping the BIOS
// also performs (TCB, args, root function) is not modeled.
void setup_thread(GuestState& state);  // 0x3C

// SetupHeap(heap_start, heap_size): validates the heap request. No verified
// path reads the kernel heap structure back yet, so the model records
// nothing; EndOfHeap and the allocator become services when a caller needs
// them. The boot call passes the end of .bss and -1 ("to the end of
// memory"). High confidence on the call contract (public ps2sdk ABI); the
// heap structure itself is Unknown.
void setup_heap(GuestState& state);  // 0x3D

// FlushCache(operation): the model has no caches, so the call completes with
// no effect, exactly like the reference's CACHE hint (M15/M17 policy).
void flush_cache(GuestState& state);  // 0x64

} // namespace gt4recomp::ee
