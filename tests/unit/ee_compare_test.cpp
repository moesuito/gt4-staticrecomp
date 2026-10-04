// Unit tests for the widened state comparator (decision 0036, PLAN.md
// P08): canonical snapshots of the live registers, every mapped RAM
// region, the kernel tables and the device banks, with the first
// divergent component named. The three blind-spot fixtures below differ
// in exactly one place the old comparator never read (the scratchpad,
// a semaphore count, a device register) and prove the old main-RAM-only
// view would have passed them blind.
#include "gt4recomp/disc_image.hpp"
#include "gt4recomp/ee_compare.hpp"
#include "gt4recomp/ee_device.hpp"
#include "gt4recomp/ee_kernel.hpp"
#include "gt4recomp/ee_timer.hpp"

#include <cstdint>
#include <iostream>
#include <span>
#include <string>

using namespace gt4recomp::ee;

namespace {

constexpr std::uint32_t scratchpad_base = 0x70000000u;
constexpr std::uint32_t scratchpad_size = 0x4000u;
constexpr std::uint32_t sema_struct = 0x00010040u;
constexpr std::uint32_t thread_struct = 0x00010100u;
constexpr std::uint32_t request_block = 0x00010200u;
constexpr std::uint32_t copy_destination = 0x00010300u;
constexpr std::uint32_t root_stack = 0x000F0000u;
constexpr std::uint32_t root_stack_size = 0x800u;

// Main RAM at zero (where the kernel's tables and buffers live) plus the
// scratchpad as a second mapped region, like the boot geometry.
GuestState make_state() {
    GuestMemory memory(0, 0x00100000);
    memory.map_region(scratchpad_base, scratchpad_size);
    return GuestState(std::move(memory));
}

void write_sema_struct(GuestState& state, std::uint32_t max_count,
                       std::uint32_t init_count) {
    state.memory().write_word(sema_struct + 0x00, 0);
    state.memory().write_word(sema_struct + 0x04, max_count);
    state.memory().write_word(sema_struct + 0x08, init_count);
    state.memory().write_word(sema_struct + 0x0C, 0);
    state.memory().write_word(sema_struct + 0x10, 0);
    state.memory().write_word(sema_struct + 0x14, 0);
}

void write_thread_struct(GuestState& state, std::uint32_t priority) {
    state.memory().write_word(thread_struct + 0x00, 0);
    state.memory().write_word(thread_struct + 0x04, 0x00010400u);
    state.memory().write_word(thread_struct + 0x08, 0x00010500u);
    state.memory().write_word(thread_struct + 0x0C, 0x100u);
    state.memory().write_word(thread_struct + 0x10, 0x00010600u);
    state.memory().write_word(thread_struct + 0x14, priority);
    state.memory().write_word(thread_struct + 0x18, 0);
    state.memory().write_word(thread_struct + 0x1C, 0);
    state.memory().write_word(thread_struct + 0x20, 0);
}

// Old comparator's memory view: a digest over the main RAM window only.
// The scratchpad never entered it, so a scratchpad-only divergence
// passed blind.
std::uint64_t main_ram_digest(const GuestState& state) {
    std::uint64_t hash = 14695981039346656037ull;
    for (std::uint32_t address = 0; address < 0x00100000u; ++address) {
        hash ^= state.memory().read_byte(address);
        hash *= 1099511628211ull;
    }
    return hash;
}

bool mentions(const std::optional<std::string>& difference, const char* text) {
    return difference.has_value()
        && difference->find(text) != std::string::npos;
}

class FakeBlocks final : public gt4recomp::DiscByteSource {
public:
    [[nodiscard]] std::uint64_t size() const override {
        return 64 * 2048;
    }
    void read(std::uint64_t offset,
              std::span<std::uint8_t> destination) const override {
        std::fill(destination.begin(), destination.end(), 0);
        if (!destination.empty()) {
            destination[0] = static_cast<std::uint8_t>(offset / 2048);
        }
    }
};

} // namespace

int main() {
    int failures = 0;
    const auto check = [&](bool passed, const char* label) {
        if (!passed) { std::cerr << label << '\n'; ++failures; }
    };
    try {

    // Control: identical machines compare equal at every level.
    {
        GuestState left = make_state();
        GuestState right = make_state();
        Kernel left_kernel;
        Kernel right_kernel;
        check(!compare_guest_states(left, right).has_value(),
              "identical guest states compare equal");
        check(!left_kernel.describe_kernel_difference(right_kernel)
                   .has_value(),
              "fresh kernels compare equal");
        check(!compare_full_states(left, left_kernel, {}, right,
                                   right_kernel, {})
                   .has_value(),
              "identical full states compare equal");
    }

    // A scratchpad-only divergence is detected and named, while the old
    // main-RAM-only digest stays equal on both sides.
    {
        GuestState left = make_state();
        GuestState right = make_state();
        right.memory().write_byte(scratchpad_base + 0x40, 0xAB);
        check(main_ram_digest(left) == main_ram_digest(right),
              "the old main-RAM digest passes a scratchpad change blind");
        const std::optional<std::string> difference =
            compare_guest_states(left, right);
        check(mentions(difference, "0x70000000")
                  && mentions(difference, "0x70000040"),
              "the widened comparator names the scratchpad address");
    }

    // A main-RAM divergence still reports its own region and address.
    {
        GuestState left = make_state();
        GuestState right = make_state();
        right.memory().write_byte(0x100, 0x01);
        const std::optional<std::string> difference =
            compare_guest_states(left, right);
        check(mentions(difference, "0x00000100"),
              "a main-RAM change names its address");
    }

    // A missing mapped region is reported instead of misaligned.
    {
        GuestState left = make_state();
        GuestMemory plain(0, 0x00100000);
        GuestState right(std::move(plain));
        check(mentions(compare_guest_states(left, right), "count"),
              "a missing scratchpad region reports the region count");
    }

    // Live-register divergences name the field.
    {
        GuestState left = make_state();
        GuestState right = make_state();
        right.write_gpr32(29, 0x006DE6D0u);
        check(mentions(compare_guest_states(left, right), "gpr[29]"),
              "a GPR change names the register");
    }
    {
        RegisterContext left;
        RegisterContext right;
        right.pc = 0x004abae4u;
        check(mentions(compare_contexts(left, right, "registers"), "pc"),
              "a pc change names the pc");
        right = RegisterContext{};
        right.cp0[12] = 0x70030c10u;
        check(mentions(compare_contexts(left, right, "registers"),
                       "cp0[12]"),
              "a CP0 change names the register");
        right = RegisterContext{};
        right.vu0_vf[3][1] = 0x3F800000u;
        check(mentions(compare_contexts(left, right, "registers"),
                       "vu0_vf[3][1]"),
              "a vector lane change names the lane");
    }

    // Populated kernels still compare equal after identical setup.
    {
        GuestState left = make_state();
        GuestState right = make_state();
        Kernel left_kernel;
        Kernel right_kernel;
        std::pair<Kernel*, GuestState*> legs[] = {{&left_kernel, &left},
                                                 {&right_kernel, &right}};
        for (auto& [kernel, state] : legs) {
            state->write_gpr32(5, root_stack);
            state->write_gpr32(6, root_stack_size);
            if (kernel->setup_thread(*state) != ServiceOutcome::Handled) {
                throw std::runtime_error("the setup leg failed");
            }
            write_sema_struct(*state, 2, 1);
            state->write_gpr32(4, sema_struct);
            if (kernel->create_sema(*state) != ServiceOutcome::Handled
                || state->read_gpr32(2) != 3) {
                throw std::runtime_error("the semaphore leg failed");
            }
        }
        check(!left_kernel.describe_kernel_difference(right_kernel)
                   .has_value(),
              "identically built kernels compare equal");
        check(!compare_full_states(left, left_kernel, {}, right,
                                   right_kernel, {})
                   .has_value(),
              "identically built full states compare equal");
    }

    // A semaphore-only divergence names the semaphore and the field.
    // The live states are re-synced by hand, so the old comparator
    // (registers plus main RAM) would pass this pair blind.
    {
        GuestState left = make_state();
        GuestState right = make_state();
        Kernel left_kernel;
        Kernel right_kernel;
        std::pair<Kernel*, GuestState*> legs[] = {{&left_kernel, &left},
                                                 {&right_kernel, &right}};
        for (auto& [kernel, state] : legs) {
            write_sema_struct(*state, 2, 1);
            state->write_gpr32(4, sema_struct);
            (void)kernel->create_sema(*state);
        }
        left.write_gpr32(4, 3);
        (void)left_kernel.signal_sema(left);
        right.write_gpr32(4, 3);   // the signal's live-state effects
        right.write_gpr64(2, 0);   // mirrored without signaling
        check(!compare_guest_states(left, right).has_value()
                  && main_ram_digest(left) == main_ram_digest(right),
              "only the semaphore differs: the old view passes blind");
        const std::optional<std::string> difference =
            left_kernel.describe_kernel_difference(right_kernel);
        check(mentions(difference, "semaphore 3")
                  && mentions(difference, "count"),
              "a semaphore-only change names the semaphore and count");
        check(mentions(compare_full_states(left, left_kernel, {}, right,
                                           right_kernel, {}),
                       "semaphore 3"),
              "the full comparator reaches the semaphore too");
    }

    // A wakeup-only divergence names the thread's wakeup count.
    {
        GuestState left = make_state();
        GuestState right = make_state();
        Kernel left_kernel;
        Kernel right_kernel;
        std::pair<Kernel*, GuestState*> legs[] = {{&left_kernel, &left},
                                                 {&right_kernel, &right}};
        for (auto& [kernel, state] : legs) {
            state->write_gpr32(5, root_stack);
            state->write_gpr32(6, root_stack_size);
            (void)kernel->setup_thread(*state);
            write_thread_struct(*state, 8);
            state->write_gpr32(4, thread_struct);
            (void)kernel->create_thread(*state);
        }
        left.write_gpr32(4, 2);
        (void)left_kernel.wakeup_thread(left);
        right.write_gpr32(4, 2);   // the wakeup's live-state effects
        right.write_gpr64(2, 0);   // mirrored without waking
        check(!compare_guest_states(left, right).has_value(),
              "only the wakeup count differs");
        const std::optional<std::string> difference =
            left_kernel.describe_kernel_difference(right_kernel);
        check(mentions(difference, "thread 2")
                  && mentions(difference, "wakeup count"),
              "a wakeup-only change names the thread and the count");
    }

    // A handler-argument divergence names the registration's argument.
    {
        GuestState left = make_state();
        GuestState right = make_state();
        Kernel left_kernel;
        Kernel right_kernel;
        left.write_gpr32(4, 2);
        left.write_gpr32(5, 0x00100400u);
        left.write_gpr32(7, 0x77u);
        (void)left_kernel.add_intc_handler(left);
        right.write_gpr32(4, 2);
        right.write_gpr32(5, 0x00100400u);
        right.write_gpr32(7, 0x78u);
        (void)right_kernel.add_intc_handler(right);
        right.write_gpr32(7, 0x77u);  // re-sync: the argument is recorded
        check(!compare_guest_states(left, right).has_value(),
              "only the recorded argument differs");
        check(mentions(left_kernel.describe_kernel_difference(right_kernel),
                       "argument"),
              "a handler-argument change names the argument");
    }

    // A pending-queue divergence names the queue, with no device wired.
    {
        Kernel left_kernel;
        Kernel right_kernel;
        left_kernel.raise_interrupt(2);
        check(mentions(
                  left_kernel.describe_kernel_difference(right_kernel),
                  "interrupt queue"),
              "a pending-only change names the interrupt queue");
    }

    // A service-clock leftover divergence names the accumulator.
    {
        GuestState left = make_state();
        Kernel left_kernel;
        Kernel right_kernel;
        left_kernel.advance_service_time(left);
        check(mentions(
                  left_kernel.describe_kernel_difference(right_kernel),
                  "service clock accumulator"),
              "a clock-leftover change names the accumulator");
    }

    // A block-cache cursor divergence names the handle and the cursor.
    {
        GuestState left = make_state();
        GuestState right = make_state();
        Kernel left_kernel;
        Kernel right_kernel;
        FakeBlocks blocks;
        left_kernel.set_disc_sectors(&blocks);
        right_kernel.set_disc_sectors(&blocks);
        std::pair<Kernel*, GuestState*> legs[] = {{&left_kernel, &left},
                                                 {&right_kernel, &right}};
        for (auto& [kernel, state] : legs) {
            state->memory().write_word(request_block + 0, 3);
            state->memory().write_word(request_block + 4, 0x40);
            state->memory().write_word(request_block + 8, 0x8000);
            if (kernel->answer_prts_read(*state, request_block) == 0) {
                throw std::runtime_error("the cache read leg failed");
            }
        }
        left.memory().write_word(request_block + 0, 1);
        left.memory().write_word(request_block + 4, copy_destination);
        left.memory().write_word(request_block + 8, 0x20);
        (void)left_kernel.answer_prts_copy(left, request_block);
        right.memory().write_word(request_block + 0, 1);
        right.memory().write_word(request_block + 4, copy_destination);
        right.memory().write_word(request_block + 8, 0x10);
        (void)right_kernel.answer_prts_copy(right, request_block);
        check(mentions(left_kernel.describe_kernel_difference(right_kernel),
                       "cursor"),
              "a cursor-only change names the cursor");
    }

    // A device-register divergence names the bank and the register.
    // The live states stay identical, so the old comparator would pass
    // this pair blind too.
    {
        GuestState left_state = make_state();
        GuestState right_state = make_state();
        Kernel left_kernel;
        Kernel right_kernel;
        TimerUnit left_timer;
        TimerUnit right_timer;
        left_timer.write_register(0x10001020u, 4, 0x1234u);
        right_timer.write_register(0x10001020u, 4, 0x1234u);
        right_timer.write_register(0x10001020u, 4, 0x1235u);
        const std::vector<NamedBank> left_banks = {
            {"timer", left_timer.registers_snapshot()}};
        const std::vector<NamedBank> right_banks = {
            {"timer", right_timer.registers_snapshot()}};
        check(!compare_guest_states(left_state, right_state).has_value(),
              "only the device register differs");
        const std::optional<std::string> section =
            compare_bank_sections(left_banks, right_banks);
        check(mentions(section, "timer")
                  && mentions(section, "0x10001020"),
              "a device-register change names the bank and register");
        check(mentions(compare_full_states(left_state, left_kernel,
                                           left_banks, right_state,
                                           right_kernel, right_banks),
                       "0x10001020"),
              "the full comparator reaches the device bank too");
    }

    // Incidental container order never diverges: the same entries in a
    // different insertion order compare equal.
    {
        RegisterBank left_bank(0x1000F200u, 0x100u);
        RegisterBank right_bank(0x1000F200u, 0x100u);
        left_bank.write_register(0x1000F200u, 4, 1);
        left_bank.write_register(0x1000F210u, 4, 2);
        right_bank.write_register(0x1000F210u, 4, 2);
        right_bank.write_register(0x1000F200u, 4, 1);
        const std::vector<NamedBank> left_banks = {
            {"sif", left_bank.registers_snapshot()}};
        const std::vector<NamedBank> right_banks = {
            {"sif", right_bank.registers_snapshot()}};
        check(!compare_bank_sections(left_banks, right_banks).has_value(),
              "reordered bank entries compare equal");
    }

    } catch (const std::exception& error) {
        std::cerr << "threw: " << error.what() << '\n';
        return 1;
    }
    if (failures != 0) {
        std::cerr << failures << " ee_compare checks failed\n";
        return 1;
    }
    std::cout << "ee_compare green\n";
    return 0;
}
