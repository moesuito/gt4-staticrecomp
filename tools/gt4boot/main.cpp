// gt4boot runs the whole game as one translated module under the boundary
// driver: from the ELF entry, through the first BIOS services (SetupThread,
// SetupHeap, FlushCache), until a boundary nothing resolves. The module is
// generated from the pinned CORE into the ignored build tree; nothing
// game-derived is committed. With --compare-interpreter the same run is
// repeated by the interpreter with the same service table, and the stop and
// the full final state must match. That is the automated check (the
// gt4boot_services CTest); --services N bounds how many services are
// handled, so the run can stop at a chosen point.
#include "translated-whole-program.hpp"

#include "boundary_text.hpp"
#include "gt4recomp/ee_driver.hpp"
#include "gt4recomp/ee_interpreter.hpp"
#include "gt4recomp/ee_kernel.hpp"
#include "gt4recomp/ee_services.hpp"
#include "gt4recomp/ee_timer.hpp"
#include "gt4recomp/executable_image.hpp"
#include "verified_core.hpp"

#include <cstdint>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <limits>
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

constexpr std::uint32_t entry = 0x00100008;
constexpr std::uint32_t bss_start = 0x006D5E00;
constexpr std::uint32_t bss_end = 0x008A215C;  // the clear loops' exit value (v1)
constexpr std::uint32_t junk_margin = 0x100;
constexpr std::uint32_t ram_size = 0x2000000;  // the EE's 32 MiB
constexpr std::uint64_t default_step_limit = 200'000'000;

// The full EE RAM as one flat zero-filled window with the image's own
// addresses and the junk pre-fill around .bss the startup tests use. The
// segment alias is on because the SDK's kernel search reads the low 512 KiB
// through KSEG0/KSEG1 (0x80000000/0xA0000000, both mapping physical 0), and
// the timer unit's register window is mapped so InitAlarm/InitTimer can read
// and write it.
GuestState make_boot_state(const ExecutableImage& image, TimerUnit& timer) {
    GuestMemory memory(0, ram_size);
    memory.enable_segment_alias();
    timer.map_into(memory);
    memory.write_bytes(image.text.guest_address, image.text.bytes);
    memory.write_bytes(image.data.guest_address, image.data.bytes);
    std::vector<std::uint8_t> junk(bss_end - bss_start + 2 * junk_margin, 0xAA);
    memory.write_bytes(bss_start - junk_margin, junk);
    GuestState state(std::move(memory));
    state.set_pc(entry);
    return state;
}

ServiceTable make_boot_services(Kernel& kernel) {
    ServiceTable services;
    services.add(0x3Du, setup_heap);
    services.add(0x64u, flush_cache);
    kernel.register_services(services);  // includes SetupThread (0x3C)
    return services;
}

Module make_boot_module() {
    return Module{
        [](std::uint32_t address) { return translated::has_entry(address); },
        [](GuestState& state, std::uint32_t address) {
            translated::call_entry(state, address);
        },
    };
}

// FNV-1a over the whole RAM window: one digest covering text, data, .bss,
// the heap and the stack.
std::uint64_t memory_digest(const GuestState& state) {
    const std::uint64_t offset_basis = 14695981039346656037ull;
    const std::uint64_t prime = 1099511628211ull;
    std::uint64_t hash = offset_basis;
    for (std::uint32_t address = 0; address < ram_size; ++address) {
        hash ^= state.memory().read_byte(address);
        hash *= prime;
    }
    return hash;
}

// Every piece of guest state the boot can change, plus the memory digest.
bool states_match(const GuestState& left, const GuestState& right) {
    for (std::uint8_t index = 0; index < 32; ++index) {
        if (left.read_gpr64(index) != right.read_gpr64(index)
            || left.read_gpr_high64(index) != right.read_gpr_high64(index)
            || left.read_fpr(index) != right.read_fpr(index)
            || left.read_cp0(index) != right.read_cp0(index)) {
            std::cerr << "state differs at register " << static_cast<unsigned>(index) << '\n';
            return false;
        }
        for (std::uint8_t lane = 0; lane < 4; ++lane) {
            if (left.read_vf_lane(index, lane) != right.read_vf_lane(index, lane)) {
                std::cerr << "VU0 VF" << static_cast<unsigned>(index) << " lane "
                          << static_cast<unsigned>(lane) << " differs\n";
                return false;
            }
        }
        if (left.read_vi(index) != right.read_vi(index)) {
            std::cerr << "VU0 VI" << static_cast<unsigned>(index) << " differs\n";
            return false;
        }
    }
    for (std::uint8_t lane = 0; lane < 4; ++lane) {
        if (left.read_acc_lane(lane) != right.read_acc_lane(lane)) {
            std::cerr << "VU0 accumulator lane " << static_cast<unsigned>(lane)
                      << " differs\n";
            return false;
        }
    }
    if (left.hi() != right.hi() || left.lo() != right.lo()
        || left.hi1() != right.hi1() || left.lo1() != right.lo1()
        || left.fpu_accumulator() != right.fpu_accumulator()
        || left.fpu_control() != right.fpu_control()
        || left.shift_amount_cache() != right.shift_amount_cache()
        || left.vu0_clip_flag() != right.vu0_clip_flag()
        || left.vu0_mac_flag() != right.vu0_mac_flag()
        || left.vu0_status_flag() != right.vu0_status_flag()
        || left.pc() != right.pc()) {
        std::cerr << "state differs in HI/LO, the FPU, VU0 flags, the shift cache or the pc\n";
        return false;
    }
    if (memory_digest(left) != memory_digest(right)) {
        std::cerr << "state differs in the guest memory window\n";
        return false;
    }
    return true;
}

struct ReferenceResult {
    Boundary boundary;
    std::uint64_t interpreted_steps = 0;
    std::uint64_t services_handled = 0;
};

// The differential reference: the interpreter with the same service table
// and the same service limit as the driver, written separately from the
// driver's loop on purpose.
ReferenceResult run_reference(GuestState& state, ServiceTable& services,
                              std::uint64_t step_limit,
                              std::uint64_t service_limit) {
    ReferenceResult result;
    Interpreter interpreter(state);
    while (true) {
        const std::uint32_t pc = state.pc();
        if ((pc & 0x3u) != 0 || !state.memory().contains(pc, 4)) {
            result.boundary = Boundary{BoundaryKind::Unmapped, pc, 0, 0};
            return result;
        }
        if (result.interpreted_steps >= step_limit) {
            result.boundary = Boundary{BoundaryKind::StepLimit, pc, 0, 0};
            return result;
        }
        const StepResult step = interpreter.step();
        ++result.interpreted_steps;
        if (step.outcome == StepOutcome::Executed) {
            continue;
        }
        if (step.outcome == StepOutcome::Exception
            && step.operation == Operation::Syscall
            && !interpreter.pending_transfer()
            && result.services_handled < service_limit) {
            const std::uint32_t service = state.read_gpr32(3);
            if (const ServiceHandler* handler = services.find(service)) {
                const ServiceOutcome outcome = (*handler)(state);
                if (outcome == ServiceOutcome::Handled) {
                    ++result.services_handled;
                    state.set_pc(step.pc + 4);
                    continue;
                }
                if (outcome == ServiceOutcome::Switched
                    || outcome == ServiceOutcome::Jumped) {
                    ++result.services_handled;
                    continue;
                }
                if (outcome == ServiceOutcome::NoRunnableThread) {
                    ++result.services_handled;
                    result.boundary = boundary_from_step(step, state);
                    result.boundary.kind = BoundaryKind::NoRunnableThread;
                    return result;
                }
            }
        }
        result.boundary = boundary_from_step(step, state);
        return result;
    }
}

void usage() {
    std::cerr << "Usage: gt4boot CORE.GT4 [--services N] [--compare-interpreter]\n"
                 "  --services N          handle at most N services, then stop at the next\n"
                 "                        syscall (default: no limit)\n"
                 "  --compare-interpreter repeat the run in the interpreter and require the\n"
                 "                        stop and the full final state to match\n";
}

} // namespace

int wmain(int argc, wchar_t* argv[]) {
#if defined(_MSC_VER) && defined(_DEBUG)
    _CrtSetReportMode(_CRT_ERROR, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ERROR, _CRTDBG_FILE_STDERR);
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
#endif
    bool compare_interpreter = false;
    std::uint64_t service_limit = std::numeric_limits<std::uint64_t>::max();
    std::filesystem::path core_path;
    for (int index = 1; index < argc; ++index) {
        const std::wstring argument = argv[index];
        if (argument == L"--compare-interpreter") {
            compare_interpreter = true;
        } else if (argument == L"--services" && index + 1 < argc) {
            service_limit = std::stoull(argv[++index]);
        } else if (core_path.empty()) {
            core_path = argument;
        } else {
            usage();
            return 2;
        }
    }
    if (core_path.empty()) {
        usage();
        return 2;
    }

    try {
        const auto core = gt4recomp::tools::read_verified_core(core_path);
        const auto image = reconstruct_core(core);

        Kernel driver_kernel;
        ServiceTable services = make_boot_services(driver_kernel);
        TimerUnit driver_timer;
        auto driver_state = make_boot_state(image, driver_timer);
        RunOptions options;
        options.step_limit = default_step_limit;
        options.service_limit = service_limit;
        options.on_service = [](std::uint32_t service, std::uint32_t pc) {
            std::cout << "service 0x" << std::hex << service << std::dec
                      << " at 0x" << std::hex << std::setfill('0') << std::setw(8)
                      << pc << std::dec << std::setfill(' ') << '\n';
        };

        Driver driver(driver_state, make_boot_module());
        const RunResult result = driver.run(services, options);
        const Boundary& boundary = result.boundary;

        std::cout << "boundary: " << gt4recomp::tools::boundary_kind_name(boundary.kind)
                  << " 0x" << std::hex << std::setfill('0') << std::setw(8) << boundary.pc
                  << std::dec << std::setfill(' ');
        if (boundary.kind == BoundaryKind::Syscall) {
            std::cout << " service 0x" << std::hex << boundary.service << std::dec;
        }
        std::cout << '\n'
                  << "stats: module calls " << result.stats.module_calls
                  << ", interpreted steps " << result.stats.interpreted_steps
                  << ", services handled " << result.stats.services_handled << '\n';

        if (compare_interpreter) {
            TimerUnit reference_timer;
            auto reference_state = make_boot_state(image, reference_timer);
            Kernel reference_kernel;
            ServiceTable reference_services = make_boot_services(reference_kernel);
            const ReferenceResult reference = run_reference(
                reference_state, reference_services, default_step_limit, service_limit);
            if (reference.boundary.kind != boundary.kind
                || reference.boundary.pc != boundary.pc
                || reference.boundary.service != boundary.service) {
                std::cerr << "the driver stopped at "
                          << gt4recomp::tools::boundary_kind_name(boundary.kind) << " 0x"
                          << std::hex << std::setfill('0') << std::setw(8) << boundary.pc
                          << std::dec << std::setfill(' ') << " but the interpreter stopped at "
                          << gt4recomp::tools::boundary_kind_name(reference.boundary.kind)
                          << " 0x" << std::hex << std::setfill('0') << std::setw(8)
                          << reference.boundary.pc << std::dec << std::setfill(' ') << '\n';
                return 1;
            }
            if (!states_match(driver_state, reference_state)) {
                std::cerr << "the driver and the interpreter states differ\n";
                return 1;
            }
            std::cout << "interpreter: " << reference.interpreted_steps
                      << " instructions, state identical (registers, HI/LO, FPU, VU0, "
                         "CP0, pc, memory digest)\n";
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAILURE: " << error.what() << '\n';
        return 1;
    }
}
