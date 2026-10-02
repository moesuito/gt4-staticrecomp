#include "gt4recomp/ee_driver.hpp"

namespace gt4recomp::ee {

const ModuleEntry* ModuleCatalog::find(std::uint32_t address) const noexcept {
    for (const ModuleEntry& entry : entries) {
        if (entry.address == address) {
            return &entry;
        }
    }
    return nullptr;
}

Boundary classify_boundary(const GuestState& state) {
    const std::uint32_t pc = state.pc();

    // A normal return through jr ra leaves the pc at the link register's
    // value; that is the only signal a plain translated function gives. A
    // trapping stop whose address happens to equal ra is indistinguishable
    // and would be reported as a return; no such case has been observed.
    if (pc == static_cast<std::uint32_t>(state.read_gpr64(31))) {
        return Boundary{BoundaryKind::Returned, pc, 0, 0};
    }
    if ((pc & 0x3u) != 0 || !state.memory().contains(pc, 4)) {
        return Boundary{BoundaryKind::Unmapped, pc, 0, 0};
    }

    const std::uint32_t word = state.memory().read_word(pc);
    const auto instruction = decode(word);
    switch (instruction.operation) {
    case Operation::Syscall:
        // The PS2 ABI passes the service number in v1 (register 3).
        return Boundary{BoundaryKind::Syscall, pc, word, state.read_gpr32(3)};
    case Operation::Break:
        return Boundary{BoundaryKind::Break, pc, word, 0};
    case Operation::Eret:
        return Boundary{BoundaryKind::ExceptionReturn, pc, word, 0};
    case Operation::Jr:
    case Operation::Jalr:
        // The module dispatches known runtime targets itself, so a stop at
        // the transfer word means the target was not in the module.
        return Boundary{BoundaryKind::IndirectTransfer, pc, word, 0};
    case Operation::Unsupported:
        return Boundary{BoundaryKind::UnsupportedWord, pc, word, 0};
    default:
        // The translator stops at an ordinary word only for a trapping
        // arithmetic overflow; the pc alone does not carry that reason.
        return Boundary{BoundaryKind::InstructionStop, pc, word, 0};
    }
}

Driver::Driver(GuestState& state, ModuleCatalog module)
    : state_(state), module_(module) {}

Boundary Driver::run_once() {
    const ModuleEntry* entry = module_.find(state_.pc());
    if (entry == nullptr) {
        return Boundary{BoundaryKind::NoEntry, state_.pc(), 0, 0};
    }
    entry->execute(state_);
    return classify_boundary(state_);
}

} // namespace gt4recomp::ee
