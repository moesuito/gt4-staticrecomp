#pragma once

// The driver executes a translated module as a program. It runs the module
// entry that owns the current pc; when the module stops at a boundary it
// cannot pass (a syscall, an unknown indirect target, a jr-ra return), the
// driver resolves it: registered BIOS services run, and the step-by-step
// interpreter — the reference the module was verified against — fills the
// gap until the next module entry. A boundary that nothing can resolve is
// reported, never guessed past.

#include "gt4recomp/ee_decode.hpp"
#include "gt4recomp/ee_interpreter.hpp"
#include "gt4recomp/ee_services.hpp"
#include "gt4recomp/ee_state.hpp"

#include <cstdint>
#include <functional>
#include <limits>
#include <span>

namespace gt4recomp::ee {

// Why the run stopped. Each kind maps to a stop shape the translator emits
// (docs/reverse-engineering/m30-driver-first-slice.md) or to a driver limit.
enum class BoundaryKind {
    Syscall,           // pc at a syscall; the PS2 service number is in v1
    Break,             // pc at a break
    ExceptionReturn,   // eret derived the pc from CP0
    UnsupportedWord,   // pc at a word outside the model
    IndirectTransfer,  // pc at a jalr/jr whose runtime target is not in the module
    Returned,          // pc equals ra: the module returned through jr ra
    InstructionStop,   // pc at an ordinary instruction; with the current
                       // translator this is a trapping arithmetic overflow
    IllegalDelaySlot,  // a transfer inside a delay slot; stopped before it
    Unmapped,          // pc outside the guest memory window or misaligned
    NoRunnableThread,  // the kernel has no thread that can run
    StepLimit          // the work budget ran out
};

struct Boundary {
    BoundaryKind kind = BoundaryKind::Unmapped;
    std::uint32_t pc = 0;       // where the run stopped
    std::uint32_t word = 0;     // the guest word at pc; zero when unmapped
    std::uint32_t service = 0;  // Syscall only: the full v1 value
};

// One callable function of a translated module.
struct ModuleEntry {
    std::uint32_t address;
    void (*execute)(GuestState&);
};

// The entry table of a translated module. The generated header provides
// has_entry/call_entry over every function it contains; small modules and
// tests adapt a fixed list with module_from_entries.
struct Module {
    std::function<bool(std::uint32_t address)> has_entry;
    std::function<void(GuestState& state, std::uint32_t address)> call_entry;
};

[[nodiscard]] Module module_from_entries(std::span<const ModuleEntry> entries);

// Classifies a stop from the guest state: the word at the pc and, for a
// normal return, the link register. This is the module-stop view, where the
// pc is the only signal; an interpreter stop carries its reason directly and
// uses boundary_from_step instead.
[[nodiscard]] Boundary classify_boundary(const GuestState& state);

// The boundary named by an interpreter stop. The step outcome says why the
// instruction did not complete, so this is exact where classify_boundary
// would have to infer (a trapping stop at pc == ra, for example).
[[nodiscard]] Boundary boundary_from_step(const StepResult& step,
                                          const GuestState& state);

struct RunOptions {
    // Interpreted instructions plus module entry calls before StepLimit.
    std::uint64_t step_limit = 100'000'000;
    // The driver stops before what would be the (service_limit + 1)-th
    // handled service; the boundary then still carries the syscall.
    std::uint64_t service_limit = std::numeric_limits<std::uint64_t>::max();
    // Optional trace hook, called just before a service handler runs.
    std::function<void(std::uint32_t service, std::uint32_t pc)> on_service;
    // Optional interrupt source: called at unit boundaries (no interpreted
    // delay slot pending). Returns true when it started a handler; the run
    // then continues in the handler's context. False when nothing is
    // pending.
    std::function<bool(GuestState& state)> start_interrupt;
    // Optional idle interrupt source: called when a service reports that no
    // thread can run. Returns true when it injected an interrupt (the run
    // continues in the handler's context); false when the model is stuck.
    std::function<bool(GuestState& state)> start_idle_interrupt;
    // Optional time source: called once per handled service, after the
    // handler ran, so the model's clocks advance while code runs and not
    // only at idleness. The reference loop calls the same kernel method at
    // its own service boundaries, keeping the two runs in step.
    std::function<void(GuestState& state)> advance_time;
};

struct DriverStats {
    std::uint64_t module_calls = 0;       // translated entries executed
    std::uint64_t interpreted_steps = 0;  // bridge instructions interpreted
    std::uint64_t services_handled = 0;
};

struct RunResult {
    Boundary boundary;
    DriverStats stats;
};

// Runs the module under a service table. A module entry executes translated
// code; every other pc is interpreted one instruction at a time, exactly as
// the differential tests verify. A throwing module or service propagates its
// error to the caller; the driver never turns one into a silent stop.
class Driver {
public:
    Driver(GuestState& state, Module module);

    [[nodiscard]] RunResult run(ServiceTable& services, const RunOptions& options);

private:
    // Runs a registered service for the syscall at pc. Handled continues at
    // pc + 4; Switched means the kernel already restored another thread's
    // context; NoRunnableThread stops the run; Unhandled leaves the boundary
    // for the caller.
    ServiceOutcome handle_syscall(std::uint32_t pc, std::uint32_t service,
                                  ServiceTable& services, const RunOptions& options,
                                  DriverStats& stats);

    GuestState& state_;
    Module module_;
    Interpreter interpreter_;
};

} // namespace gt4recomp::ee
