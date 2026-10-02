#pragma once

// The driver executes a translated module as a program: it calls the module
// entry that owns the current pc, then reports the boundary where the module
// returned control. The translator leaves the pc at the stop address (or at
// ra after a normal return), so the driver classifies the stop from the guest
// state alone; nothing is guessed past a boundary. This is the first slice:
// no service layer exists yet, so a syscall boundary is reported, not handled.

#include "gt4recomp/ee_decode.hpp"
#include "gt4recomp/ee_state.hpp"

#include <cstdint>
#include <span>

namespace gt4recomp::ee {

// Why the module returned control. Each kind maps to a stop shape the
// translator emits (docs/reverse-engineering/m30-driver-first-slice.md).
enum class BoundaryKind {
    NoEntry,           // pc has no module entry; the driver cannot execute it
    Syscall,           // pc at a syscall; the PS2 service number is in v1
    Break,             // pc at a break
    ExceptionReturn,   // eret derived the pc from CP0
    UnsupportedWord,   // pc at a word outside the model
    IndirectTransfer,  // pc at a jalr/jr whose runtime target is not in the module
    Returned,          // pc equals ra: the module returned through jr ra
    InstructionStop,   // pc at an ordinary instruction; with the current
                       // translator this is a trapping arithmetic overflow
    Unmapped           // pc outside the guest memory window or misaligned
};

struct Boundary {
    BoundaryKind kind = BoundaryKind::NoEntry;
    std::uint32_t pc = 0;       // where the module stopped
    std::uint32_t word = 0;     // the guest word at pc; zero when unmapped
    std::uint32_t service = 0;  // Syscall only: the full v1 value
};

// One callable function of a translated module.
struct ModuleEntry {
    std::uint32_t address;
    void (*execute)(GuestState&);
};

// The entries of one module. The driver calls an address only when it is
// present here; anything else comes back as a boundary for the caller.
struct ModuleCatalog {
    std::span<const ModuleEntry> entries;

    [[nodiscard]] const ModuleEntry* find(std::uint32_t address) const noexcept;
};

// Classifies the module's stop from the guest state: the word at the pc and,
// for a normal return, the link register.
[[nodiscard]] Boundary classify_boundary(const GuestState& state);

// Runs one translated module under the driver. A boundary is never guessed
// past: an address without an entry and an unknown runtime target both come
// back as described stops.
class Driver {
public:
    Driver(GuestState& state, ModuleCatalog module);

    // The pc must name a module entry; otherwise the result is NoEntry and
    // nothing executes. A throwing module propagates its error to the caller.
    [[nodiscard]] Boundary run_once();

private:
    GuestState& state_;
    ModuleCatalog module_;
};

} // namespace gt4recomp::ee
