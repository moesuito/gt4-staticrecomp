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
#include "gt4build_info.hpp"
#include "gt4recomp/disc_image.hpp"
#include "gt4recomp/ee_checkpoint.hpp"
#include "gt4recomp/ee_compare.hpp"
#include "gt4recomp/ee_device.hpp"
#include "gt4recomp/ee_driver.hpp"
#include "gt4recomp/ee_interpreter.hpp"
#include "gt4recomp/ee_kernel.hpp"
#include "gt4recomp/ee_services.hpp"
#include "gt4recomp/ee_timer.hpp"
#include "gt4recomp/executable_image.hpp"
#include "verified_core.hpp"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <map>
#include <optional>
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

// The device register windows the boot touches so far: the timer block, the
// DMAC and SIF0 channel control, the SIF register block, the GS register
// block (a memory region), and the GIF/VIF0/VIF1 register and FIFO windows.
// The VIF0/VIF1/GIF DMA channels run the transfer their registers describe
// (normal QWC from MADR, or the source chain at TADR) into a retained device
// sink and report the channel's DMAC completion (channels 0/1/2) only after
// the implemented transfer concludes; anything outside the subset stops
// loudly instead of completing (decisions 0011/0029 and 0033). INTC causes
// 4/5 are VIF command events and INTC 9 is Timer0: none of them is a DMA
// completion, so a GIF completion never touches Timer0.
struct BootDevices {
    explicit BootDevices(std::function<void(std::uint32_t)> raise_dmac)
        : vif0_dma(0x10008000u, 0x1000u, 0, raise_dmac),
          vif1_dma(0x10009000u, 0x1000u, 1, raise_dmac),
          gif_dma(0x1000A000u, 0x1000u, 2, raise_dmac) {
    }

    TimerUnit timer;
    DmacStatusUnit dmac;
    RegisterBank sif0{0x1000C000u, 0x100u};
    RegisterBank sif_registers{0x1000F200u, 0x100u};
    RegisterBank gif{0x10003000u, 0x800u};
    RegisterBank vif0{0x10003800u, 0x400u};
    RegisterBank vif1{0x10003C00u, 0x400u};
    RegisterBank vif0_fifo{0x10004000u, 0x1000u};
    RegisterBank vif1_fifo{0x10005000u, 0x1000u};
    RegisterBank gif_fifo{0x10006000u, 0x1000u};
    RegisterBank ipu{0x10002000u, 0x1000u};
    RegisterBank ipu_fifo{0x10007000u, 0x1000u};
    DmaChannel vif0_dma;
    DmaChannel vif1_dma;
    DmaChannel gif_dma;
    RegisterBank ipu_port{0x1000B000u, 0x1000u};
    RegisterBank spr_dma{0x1000D000u, 0x1000u};
    IntcUnit intc;
    RegisterBank sio{0x1000F100u, 0x100u};

    // Hands the kernel the units behind the MMIO windows so occurrences
    // set status bits, enables reach the masks, and the tick advances move
    // the typed counters. Both engines wire identically, so the
    // differential stays exact.
    void wire_kernel(Kernel& kernel) {
        kernel.set_timer_unit(&timer);
        kernel.set_intc_unit(&intc);
        kernel.set_dmac_unit(&dmac);
    }

    void map_into(GuestMemory& memory) {
        timer.map_into(memory);
        dmac.map_into(memory);
        sif0.map_into(memory);
        sif_registers.map_into(memory);
        gif.map_into(memory);
        vif0.map_into(memory);
        vif1.map_into(memory);
        vif0_fifo.map_into(memory);
        vif1_fifo.map_into(memory);
        gif_fifo.map_into(memory);
        ipu.map_into(memory);
        ipu_fifo.map_into(memory);
        vif0_dma.map_into(memory);
        vif1_dma.map_into(memory);
        gif_dma.map_into(memory);
        ipu_port.map_into(memory);
        spr_dma.map_into(memory);
        intc.map_into(memory);
        sio.map_into(memory);
    }

    // Follows a GuestMemory move: the DMA engines read tags and payloads
    // through this pointer, so it must name the live memory, not the local
    // the mapping was built on.
    void rebind_dma(GuestMemory& memory) noexcept {
        vif0_dma.rebind_memory(memory);
        vif1_dma.rebind_memory(memory);
        gif_dma.rebind_memory(memory);
    }
};

// The full EE RAM as one flat zero-filled window with the image's own
// addresses and the junk pre-fill around .bss the startup tests use. The
// segment alias is on because the SDK's kernel search reads the low 512 KiB
// through KSEG0/KSEG1 (0x80000000/0xA0000000, both mapping physical 0), and
// the device windows are mapped so the init can read and write them. The
// EE's 16 KiB scratchpad lives at 0x70000000 as its own memory region, and
// the GS register block (8 KiB at 0x12000000) is modeled as storage: the
// model stores what the guest writes, including the 64-bit register writes
// the 32-bit register banks cannot hold, and returns it on reads. No GS
// behavior (drawing, register masks, read-back semantics) is emulated.
GuestState make_boot_state(const ExecutableImage& image, BootDevices& devices) {
    GuestMemory memory(0, ram_size);
    memory.enable_segment_alias();
    memory.map_region(0x70000000u, 0x4000u);
    memory.map_region(0x12000000u, 0x2000u);
    devices.map_into(memory);
    memory.write_bytes(image.text.guest_address, image.text.bytes);
    memory.write_bytes(image.data.guest_address, image.data.bytes);
    std::vector<std::uint8_t> junk(bss_end - bss_start + 2 * junk_margin, 0xAA);
    memory.write_bytes(bss_start - junk_margin, junk);
    GuestState state(std::move(memory));
    state.set_pc(entry);
    devices.rebind_dma(state.memory());
    return state;
}

ServiceTable make_boot_services(Kernel& kernel) {
    ServiceTable services;
    services.add(0x3Du, setup_heap);
    services.add(0x64u, flush_cache);
    kernel.register_services(services);  // includes SetupThread (0x3C)
    return services;
}

// The device banks in map_into order: the snapshot order is code, so the
// same binary restores exactly what it saved.
std::vector<BankRegisters> snapshot_banks(const BootDevices& devices) {
    return {
        devices.timer.registers_snapshot(),
        devices.dmac.registers_snapshot(),
        devices.sif0.registers_snapshot(),
        devices.sif_registers.registers_snapshot(),
        devices.gif.registers_snapshot(),
        devices.vif0.registers_snapshot(),
        devices.vif1.registers_snapshot(),
        devices.vif0_fifo.registers_snapshot(),
        devices.vif1_fifo.registers_snapshot(),
        devices.gif_fifo.registers_snapshot(),
        devices.ipu.registers_snapshot(),
        devices.ipu_fifo.registers_snapshot(),
        devices.vif0_dma.registers_snapshot(),
        devices.vif1_dma.registers_snapshot(),
        devices.gif_dma.registers_snapshot(),
        devices.ipu_port.registers_snapshot(),
        devices.spr_dma.registers_snapshot(),
        devices.intc.registers_snapshot(),
        devices.sio.registers_snapshot(),
    };
}

void restore_banks(BootDevices& devices,
                    const std::vector<BankRegisters>& banks) {
    if (banks.size() != 19) {
        throw std::runtime_error("The checkpoint holds the wrong bank count");
    }
    devices.timer.restore_registers(banks[0]);
    devices.dmac.restore_registers(banks[1]);
    devices.sif0.restore_registers(banks[2]);
    devices.sif_registers.restore_registers(banks[3]);
    devices.gif.restore_registers(banks[4]);
    devices.vif0.restore_registers(banks[5]);
    devices.vif1.restore_registers(banks[6]);
    devices.vif0_fifo.restore_registers(banks[7]);
    devices.vif1_fifo.restore_registers(banks[8]);
    devices.gif_fifo.restore_registers(banks[9]);
    devices.ipu.restore_registers(banks[10]);
    devices.ipu_fifo.restore_registers(banks[11]);
    devices.vif0_dma.restore_registers(banks[12]);
    devices.vif1_dma.restore_registers(banks[13]);
    devices.gif_dma.restore_registers(banks[14]);
    devices.ipu_port.restore_registers(banks[15]);
    devices.spr_dma.restore_registers(banks[16]);
    devices.intc.restore_registers(banks[17]);
    devices.sio.restore_registers(banks[18]);
}

// The snapshot_banks order as diagnostic labels: the comparator matches
// banks by name, so these only name the diagnosis, never the semantics.
const std::vector<std::string>& bank_names() {
    static const std::vector<std::string> names = {
        "timer", "dmac", "sif0", "sif", "gif", "vif0", "vif1",
        "vif0-fifo", "vif1-fifo", "gif-fifo", "ipu", "ipu-fifo",
        "vif0-dma", "vif1-dma", "gif-dma", "ipu-port", "spr", "intc",
        "sio",
    };
    return names;
}

// The same observation snapshot_banks takes, with the labels above, for
// the widened differential. Still observation only: registering a value
// reads device storage, never a guest MMIO access.
std::vector<NamedBank> snapshot_named_banks(const BootDevices& devices) {
    const std::vector<BankRegisters> banks = snapshot_banks(devices);
    const std::vector<std::string>& names = bank_names();
    if (banks.size() != names.size()) {
        throw std::runtime_error(
            "The bank snapshot outgrew its diagnostic labels");
    }
    std::vector<NamedBank> named;
    named.reserve(banks.size());
    for (std::size_t index = 0; index < banks.size(); ++index) {
        named.push_back({names[index], banks[index]});
    }
    return named;
}

std::vector<std::uint8_t> read_file_bytes(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        throw std::runtime_error("Cannot open " + path.string());
    }
    return {std::istreambuf_iterator<char>(file),
            std::istreambuf_iterator<char>()};
}

void write_file_bytes(const std::filesystem::path& path,
                      std::span<const std::uint8_t> bytes) {
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file) {
        throw std::runtime_error("Cannot write " + path.string());
    }
    file.write(reinterpret_cast<const char*>(bytes.data()),
               static_cast<std::streamsize>(bytes.size()));
    file.flush();
    if (!file) {
        throw std::runtime_error("Failed writing " + path.string());
    }
}

// The write-time provenance every checkpoint photo carries (decision
// 0028): commit, binary, verified input, disc attachment and time policy.
// Deterministic by construction — no timestamps, no host paths — so two
// identical runs write byte-identical photos (the autosave determinism
// CTest). The disc hash is not verified at open (its check lives in the
// pre-run fingerprint step), so the field says exactly that.
CheckpointProvenance make_provenance(bool disc_attached,
                                     const std::filesystem::path& disc_path) {
    CheckpointProvenance provenance;
    provenance.git_commit = GT4RECOMP_GIT_COMMIT;
#if defined(_MSC_VER)
    provenance.binary = std::string("gt4boot ") + GT4RECOMP_BUILD_TYPE
        + " MSVC " + std::to_string(_MSC_VER);
#elif defined(__clang__)
    provenance.binary = std::string("gt4boot ") + GT4RECOMP_BUILD_TYPE
        + " clang " + __clang_version__;
#elif defined(__GNUC__)
    provenance.binary = std::string("gt4boot ") + GT4RECOMP_BUILD_TYPE + " GCC "
        + __VERSION__;
#else
    provenance.binary = std::string("gt4boot ") + GT4RECOMP_BUILD_TYPE
        + " unknown-compiler";
#endif
    provenance.core_sha256 = gt4recomp::tools::pinned_core_sha256();
    provenance.disc = disc_attached
        ? "attached:" + disc_path.filename().string()
            + " (hash unverified at open)"
        : "absent";
    provenance.time_policy =
        "service-clock 1ms/service; idle 1 frame/interrupt; budget 2000000";
    return provenance;
}

// One line naming the run's identity for the §22.3 run record: commit,
// binary, verified input, model semantics and time policy. The disc line
// below adds the attachment; provenance lines never carry guest addresses,
// so --quiet keeps them.
void print_run_identity(const CheckpointProvenance& provenance) {
    const ModelCompatibility model = current_model_compatibility();
    std::cout << "run provenance: commit " << provenance.git_commit
              << "; binary " << provenance.binary << "; core "
              << provenance.core_sha256 << "; model time=" << model.time_model
              << " interrupt=" << model.interrupt_model << " kernel="
              << model.kernel_model << " rpc="
              << model.rpc_model << " translation=" << model.translation_model
              << "; time " << provenance.time_policy << '\n';
}

// One line naming a checkpoint file's stored identity at resume time: its
// semantic versions plus who wrote it. Provenance never gates acceptance —
// this is diagnosis for the run record.
void print_checkpoint_identity(const CheckpointFile& file) {
    std::cout << "checkpoint model: time=" << file.compatibility.time_model
              << " interrupt=" << file.compatibility.interrupt_model
              << " kernel=" << file.compatibility.kernel_model
              << " rpc=" << file.compatibility.rpc_model << " translation="
              << file.compatibility.translation_model
              << "; written by commit " << file.provenance.git_commit
              << "; binary " << file.provenance.binary << "; core "
              << file.provenance.core_sha256 << "; disc "
              << file.provenance.disc << "; time "
              << file.provenance.time_policy << '\n';
}

// Writes the whole stop state to a checkpoint file: the model-compatibility
// identity, the write-time provenance, the service count, the
// context+memory section, the kernel section and the device banks, framed
// as GT4CPT3. Shared by --checkpoint-at and autosave (decision 0027) so
// both paths write byte-identical files for the same stop state. Returns
// the framed size for the log line.
std::uint64_t write_stop_checkpoint(const GuestState& state,
                                    const Kernel& kernel,
                                    const BootDevices& devices,
                                    const CheckpointProvenance& provenance,
                                    std::uint64_t services_handled,
                                    const std::filesystem::path& path) {
    CheckpointFile file;
    file.compatibility = current_model_compatibility();
    file.provenance = provenance;
    file.services_handled = services_handled;
    file.context_memory = save_snapshot(
        state.save_registers(), state.memory().segment_alias_enabled(),
        state.memory().regions_snapshot());
    file.kernel = kernel.save_kernel_state();
    file.banks = save_bank_section(snapshot_banks(devices));
    const std::vector<std::uint8_t> framed = save_checkpoint_file(file);
    write_file_bytes(path, framed);
    return framed.size();
}

// The first photo moment strictly above a service count: multiples of
// the interval, so photo names stay a pure function of the count (no
// host timing enters). The max value is the "no moment reachable"
// sentinel, never a photo name.
std::uint64_t next_autosave_photo(std::uint64_t base, std::uint64_t every) {
    const std::uint64_t aligned = base / every * every;
    if (aligned > std::numeric_limits<std::uint64_t>::max() - every) {
        return std::numeric_limits<std::uint64_t>::max();
    }
    return aligned + every;
}

// Names why a photo moment produced no file, for the end-of-run note.
std::string skip_reason_text(const Boundary& boundary,
                             bool transfer_in_flight) {
    if (boundary.kind == BoundaryKind::StepLimit) {
        return "step limit";
    }
    if (boundary.kind == BoundaryKind::NoRunnableThread) {
        return "idle (no runnable thread)";
    }
    if (transfer_in_flight) {
        return "transfer in flight";
    }
    return std::string("fault (")
        + gt4recomp::tools::boundary_kind_name(boundary.kind) + ")";
}

// The managed photos already sitting in the directory: names matching
// ckpt-<n>.bin with their byte sizes. Foreign files are ignored, never
// deleted. Unreadable sizes skip the entry instead of killing the run.
std::vector<AutosavePhoto> inventory_autosave_dir(
    const std::filesystem::path& dir) {
    std::vector<AutosavePhoto> photos;
    std::error_code list_error;
    if (!std::filesystem::exists(dir, list_error) || list_error) {
        return photos;
    }
    std::filesystem::directory_iterator end;
    std::filesystem::directory_iterator cursor(dir, list_error);
    if (list_error) {
        return photos;
    }
    for (; cursor != end; cursor.increment(list_error)) {
        if (list_error) {
            break;
        }
        if (!cursor->is_regular_file()) {
            continue;
        }
        std::uint64_t services = 0;
        if (!parse_autosave_name(cursor->path().filename().string(),
                                 services)) {
            continue;
        }
        std::error_code size_error;
        const std::uint64_t bytes = cursor->file_size(size_error);
        if (size_error) {
            continue;
        }
        photos.push_back({services, bytes});
    }
    return photos;
}

const char* eviction_reason_text(AutosaveEvictReason reason) {
    return reason == AutosaveEvictReason::BeyondKeep ? "beyond keep"
                                                    : "over byte cap";
}

// Deletes the rotation's picks and reports each one, keeping the
// in-memory inventory in step for the next photo.
void apply_autosave_rotation(const std::filesystem::path& dir,
                             std::vector<AutosavePhoto>& inventory,
                             std::uint64_t keep, bool has_byte_cap,
                             std::uint64_t max_bytes,
                             std::uint64_t just_written) {
    const std::vector<AutosaveEviction> doomed = select_autosave_evictions(
        inventory, keep, has_byte_cap, max_bytes, just_written);
    for (const AutosaveEviction& eviction : doomed) {
        const std::filesystem::path victim =
            dir / format_autosave_name(eviction.services);
        std::error_code remove_error;
        std::filesystem::remove(victim, remove_error);
        if (remove_error) {
            throw std::runtime_error("Cannot evict " + victim.string());
        }
        std::cout << "checkpoint evicted " << victim.filename().string()
                  << " (" << eviction_reason_text(eviction.reason) << ")\n";
        inventory.erase(
            std::remove_if(inventory.begin(), inventory.end(),
                           [&](const AutosavePhoto& photo) {
                               return photo.services == eviction.services;
                           }),
            inventory.end());
    }
}

struct AutosaveOutcome {
    RunResult result;
    std::vector<std::string> skipped_moments;
};

// Runs the boot leg by leg for autosave (decision 0027): each leg
// handles up to the next photo multiple, and a clean leg end writes
// ckpt-<cumulative>.bin through the shared save path. A leg that
// falls short ends the run — the machine stopped on its own (fault,
// step limit, idle), so there is nothing further to photo. The
// returned stats accumulate across legs, with services counted from
// the run start (resume base excluded), matching the recount rule.
AutosaveOutcome run_with_autosave(
    Driver& driver, ServiceTable& services, RunOptions leg_options,
    const GuestState& state, const Kernel& kernel,
    const BootDevices& devices, const CheckpointProvenance& provenance,
    std::uint64_t resume_base,
    std::uint64_t service_limit, std::uint64_t step_limit,
    std::uint64_t every, const std::filesystem::path& dir,
    std::uint64_t keep, bool has_byte_cap, std::uint64_t max_bytes,
    std::vector<AutosavePhoto>& inventory) {
    AutosaveOutcome outcome;
    const std::uint64_t unlimited = std::numeric_limits<std::uint64_t>::max();
    std::uint64_t cumulative = resume_base;
    std::uint64_t remaining_services = service_limit;
    std::uint64_t used_steps = 0;
    std::uint64_t total_module_calls = 0;
    std::uint64_t total_interpreted_steps = 0;
    std::uint64_t next_photo = next_autosave_photo(cumulative, every);
    while (true) {
        std::uint64_t leg_target = remaining_services;
        bool photo_due = false;
        if (next_photo != unlimited && next_photo > cumulative
            && (remaining_services == unlimited
                || next_photo - cumulative <= remaining_services)) {
            leg_target = next_photo - cumulative;
            photo_due = true;
        }
        leg_options.service_limit = leg_target;
        // The step budget spans legs like the service budget does, so
        // --steps means the same total with or without autosave.
        leg_options.step_limit =
            (step_limit == unlimited) ? unlimited : step_limit - used_steps;
        const RunResult leg = driver.run(services, leg_options);
        const std::uint64_t leg_handled = leg.stats.services_handled;
        cumulative += leg_handled;
        used_steps += leg.stats.module_calls + leg.stats.interpreted_steps;
        total_module_calls += leg.stats.module_calls;
        total_interpreted_steps += leg.stats.interpreted_steps;
        if (remaining_services != unlimited) {
            remaining_services -= leg_handled;
        }
        outcome.result = leg;
        // The 0022 clean-stop predicate: exactly the photo count, at a
        // syscall boundary, with no transfer in flight.
        const bool clean = leg_handled == leg_target
            && leg.boundary.kind == BoundaryKind::Syscall
            && !driver.pending_transfer();
        if (photo_due && clean) {
            const std::filesystem::path photo_path =
                dir / format_autosave_name(cumulative);
            std::error_code make_error;
            std::filesystem::create_directories(dir, make_error);
            if (make_error) {
                throw std::runtime_error("Cannot create " + dir.string());
            }
            const std::uint64_t framed_size = write_stop_checkpoint(
                state, kernel, devices, provenance, cumulative, photo_path);
            std::cout << "checkpoint saved at " << cumulative
                      << " services (cumulative since boot): "
                      << photo_path.string() << " (" << framed_size
                      << " bytes)\n";
            // Upsert: a re-run into a non-empty directory overwrites the
            // photo file, so the inventory must replace the stale entry
            // for the same count instead of counting it twice (which
            // would evict the file just written).
            inventory.erase(
                std::remove_if(inventory.begin(), inventory.end(),
                               [&](const AutosavePhoto& photo) {
                                   return photo.services == cumulative;
                               }),
                inventory.end());
            inventory.push_back({cumulative, framed_size});
            apply_autosave_rotation(dir, inventory, keep, has_byte_cap,
                                    max_bytes, cumulative);
        } else if (photo_due) {
            outcome.skipped_moments.push_back(
                "checkpoint skipped at " + std::to_string(next_photo)
                + " (" + skip_reason_text(leg.boundary,
                                          driver.pending_transfer())
                + ")");
            break;
        } else {
            break;
        }
        if (service_limit != unlimited && remaining_services == 0) {
            break;
        }
        next_photo = next_autosave_photo(cumulative, every);
    }
    outcome.result.stats.module_calls = total_module_calls;
    outcome.result.stats.interpreted_steps = total_interpreted_steps;
    outcome.result.stats.services_handled = cumulative - resume_base;
    return outcome;
}

Module make_boot_module() {
    return Module{
        [](std::uint32_t address) { return translated::has_entry(address); },
        [](GuestState& state, std::uint32_t address) {
            return translated::call_entry(state, address);
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

// Every piece of guest state the boot can change: the live registers,
// every mapped RAM region (main memory, the scratchpad, the GS block),
// the kernel tables (threads with their saved contexts, semaphores,
// handlers, the pending queue, SIF/RPC state, the service-clock
// leftovers, disc and block-cache handles) and every device bank.
// The old comparator only covered the live registers and the main RAM
// window, so a scratchpad-only, semaphore-only or device-register-only
// divergence passed blind; the widened one names the first divergent
// component. Observation only: nothing here changes either side.
bool states_match(const GuestState& left_state, const Kernel& left_kernel,
                  const BootDevices& left_devices,
                  const GuestState& right_state, const Kernel& right_kernel,
                  const BootDevices& right_devices) {
    const std::optional<std::string> difference = compare_full_states(
        left_state, left_kernel, snapshot_named_banks(left_devices),
        right_state, right_kernel, snapshot_named_banks(right_devices));
    if (difference.has_value()) {
        std::cerr << "state differs at " << *difference << '\n';
        return false;
    }
    return true;
}

struct ReferenceResult {
    Boundary boundary;
    std::uint64_t interpreted_steps = 0;
    std::uint64_t services_handled = 0;
};

// The differential reference: the interpreter with the same service table,
// kernel and limits as the driver, written separately from the driver's loop
// on purpose.
ReferenceResult run_reference(GuestState& state, ServiceTable& services,
                              Kernel& kernel, std::uint64_t step_limit,
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
        if (!interpreter.pending_transfer() && kernel.start_interrupt(state)) {
            continue;
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
                    kernel.advance_service_time(state);
                    state.set_pc(step.pc + 4);
                    continue;
                }
                if (outcome == ServiceOutcome::Switched
                    || outcome == ServiceOutcome::Jumped) {
                    ++result.services_handled;
                    kernel.advance_service_time(state);
                    continue;
                }
                if (outcome == ServiceOutcome::NoRunnableThread) {
                    ++result.services_handled;
                    kernel.advance_service_time(state);
                    if (kernel.deliver_idle_interrupt(state)) {
                        continue;
                    }
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
    std::cerr << "Usage: gt4boot CORE.GT4 [--services N] [--steps N] [--disc IMAGE] "
                 "[--compare-interpreter] [--threads] [--dump ADDRESS LENGTH]\n"
                 "       [--checkpoint-at N PATH] [--resume PATH] [--verify-resume PATH]\n"
                 "       [--checkpoint-every K DIR --keep M [--max-bytes B]] [--quiet]\n"
                 "       [--strict-rpc]\n"
                 "  --services N          handle at most N services, then stop at the next\n"
                 "                        syscall (default: no limit)\n"
                 "  --steps N             stop after N translated calls plus interpreted\n"
                 "                        instructions (default: 200000000)\n"
                 "  --disc IMAGE          serve the game's file requests from an ISO9660\n"
                 "                        disc image (the pinned ISO); without it file\n"
                 "                        opens answer \"not found\"\n"
                 "  --compare-interpreter repeat the run in the interpreter and require the\n"
                 "                        stop and the full final state to match\n"
                 "  --threads             print the kernel's thread table after the run\n"
                 "                        (with the slice-73 RPC inventory)\n"
                 "  --strict-rpc          stop loudly at the first unknown (SID,\n"
                 "                        function) RPC pair instead of answering\n"
                 "                        the silent empty result (default off,\n"
                 "                        so the boot keeps its behavior)\n"
                 "  --dump ADDRESS LENGTH print LENGTH bytes of guest memory at the stop,\n"
                 "                        eight words per line (both values hexadecimal;\n"
                 "                        may be repeated)\n"
                 "  --checkpoint-at N PATH\n"
                 "                        when a run stops at exactly N leg services, save\n"
                 "                        a checkpoint file (pair with --services N)\n"
                 "  --resume PATH         rebuild the boot and resume it from a checkpoint\n"
                 "                        file (leg counters recount from zero;\n"
                 "                        pre-P00 and foreign-semantics files refuse)\n"
                 "  --verify-resume PATH  resume from a checkpoint, run --services more,\n"
                 "                        run the same total fresh, and require identical\n"
                 "                        states (uses --services for the resumed leg)\n"
                 "  --checkpoint-every K DIR\n"
                 "                        photo the run every K services into DIR as\n"
                 "                        ckpt-<total>.bin (clean stops only; dirty\n"
                 "                        moments skip quietly, never abort)\n"
                 "  --keep M              with --checkpoint-every, retain the newest M\n"
                 "                        photos, deleting the oldest first\n"
                 "  --max-bytes B         with --checkpoint-every, also evict oldest\n"
                 "                        photos past this total-bytes cap (the fresh\n"
                 "                        photo is never evicted by its own run)\n"
                 "  --quiet               suppress the per-service trace line\n";
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
    std::uint64_t step_limit = default_step_limit;
    std::filesystem::path core_path;
    bool print_threads = false;
    // Strict RPC mode (slice 73, P07): off by default so the boot keeps
    // its current behavior; on, the first unknown pair stops loudly.
    bool strict_rpc = false;
    // Checkpointing: --checkpoint-at saves after exactly N services (pair
    // with --services N); --resume restarts from a file (leg counters recount
    // from zero); --verify-resume replays both ways and requires identical
    // states, using --services for the resumed leg. Autosave
    // (--checkpoint-every K DIR --keep M [--max-bytes B], decision 0027)
    // photos the run every K services instead of once; --quiet suppresses
    // only the per-service trace line. Unset autosave counts use the max
    // sentinel, like checkpoint_at.
    std::uint64_t checkpoint_at = std::numeric_limits<std::uint64_t>::max();
    std::filesystem::path checkpoint_path;
    std::filesystem::path resume_path;
    std::filesystem::path verify_resume_path;
    std::uint64_t autosave_every = std::numeric_limits<std::uint64_t>::max();
    std::filesystem::path autosave_dir;
    std::uint64_t autosave_keep = 0;
    bool autosave_has_byte_cap = false;
    std::uint64_t autosave_max_bytes = 0;
    bool quiet = false;
    // The stop-time memory views the caller asked for, as address and byte
    // count. They are read after the run, so they show the state the run
    // stopped in.
    std::vector<std::pair<std::uint32_t, std::uint32_t>> dumps;
    std::filesystem::path disc_path;
    for (int index = 1; index < argc; ++index) {
        const std::wstring argument = argv[index];
        if (argument == L"--compare-interpreter") {
            compare_interpreter = true;
        } else if (argument == L"--threads") {
            print_threads = true;
        } else if (argument == L"--strict-rpc") {
            strict_rpc = true;
        } else if (argument == L"--dump" && index + 2 < argc) {
            const std::uint32_t address = static_cast<std::uint32_t>(
                std::stoul(argv[++index], nullptr, 16));
            const std::uint32_t length = static_cast<std::uint32_t>(
                std::stoul(argv[++index], nullptr, 16));
            dumps.push_back({address, length});
        } else if (argument == L"--disc" && index + 1 < argc) {
            disc_path = argv[++index];
        } else if (argument == L"--services" && index + 1 < argc) {
            service_limit = std::stoull(argv[++index]);
        } else if (argument == L"--steps" && index + 1 < argc) {
            step_limit = std::stoull(argv[++index]);
        } else if (argument == L"--checkpoint-at" && index + 2 < argc) {
            checkpoint_at = std::stoull(argv[++index]);
            checkpoint_path = argv[++index];
        } else if (argument == L"--resume" && index + 1 < argc) {
            resume_path = argv[++index];
        } else if (argument == L"--verify-resume" && index + 1 < argc) {
            verify_resume_path = argv[++index];
        } else if (argument == L"--checkpoint-every" && index + 2 < argc) {
            autosave_every = std::stoull(argv[++index]);
            autosave_dir = argv[++index];
        } else if (argument == L"--keep" && index + 1 < argc) {
            autosave_keep = std::stoull(argv[++index]);
        } else if (argument == L"--max-bytes" && index + 1 < argc) {
            autosave_has_byte_cap = true;
            autosave_max_bytes = std::stoull(argv[++index]);
        } else if (argument == L"--quiet") {
            quiet = true;
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
    const bool want_checkpoint = checkpoint_at
        != std::numeric_limits<std::uint64_t>::max();
    const bool want_resume = !resume_path.empty();
    const bool want_verify = !verify_resume_path.empty();
    // Autosave (decision 0027): unset counts use the max sentinel. A zero
    // interval or keep is a usage error, not "off"; --keep/--max-bytes
    // without --checkpoint-every are usage errors too.
    const bool want_autosave = autosave_every
        != std::numeric_limits<std::uint64_t>::max();
    if (want_autosave && autosave_every == 0) {
        std::cerr << "--checkpoint-every needs a positive service count\n";
        usage();
        return 2;
    }
    if (want_autosave && autosave_keep == 0) {
        std::cerr << "--checkpoint-every needs --keep with a positive count\n";
        usage();
        return 2;
    }
    if (!want_autosave && (autosave_keep != 0 || autosave_has_byte_cap)) {
        std::cerr << "--keep/--max-bytes need --checkpoint-every\n";
        usage();
        return 2;
    }
    if (autosave_has_byte_cap && autosave_max_bytes == 0) {
        std::cerr << "--max-bytes needs a positive byte count\n";
        usage();
        return 2;
    }
    // Chained checkpoints: --checkpoint-at saves from a resumed leg too
    // (decision 0024). The save still requires a clean service stop at
    // exactly the requested count, so every link is exact; counts stay
    // leg-relative because the driver recounts services from zero after
    // a resume. --verify-resume still stands alone (its fresh total
    // replays from the boot, which a chain link is not).
    if (want_verify && (want_checkpoint || want_resume)) {
        std::cerr << "verify-resume does not combine with checkpoint or resume\n";
        usage();
        return 2;
    }
    if (want_verify && compare_interpreter) {
        std::cerr << "verify-resume states its own verdict; it does not combine "
                     "with compare-interpreter\n";
        usage();
        return 2;
    }
    // Autosave stands alone from --verify-resume and --compare-interpreter
    // (decision 0027): a verify leg replays from the boot, which an
    // autosave leg is not, and the reference comparison covers one total.
    if (want_autosave && want_verify) {
        std::cerr << "checkpoint-every does not combine with verify-resume\n";
        usage();
        return 2;
    }
    if (want_autosave && compare_interpreter) {
        std::cerr << "checkpoint-every does not combine with compare-interpreter\n";
        usage();
        return 2;
    }
    // Rotation owns every ckpt-<n>.bin in the autosave directory, so a
    // hand-placed manual path inside it would be eaten: refuse loudly.
    if (want_autosave && want_checkpoint && !checkpoint_path.empty()
        && std::filesystem::absolute(checkpoint_path).parent_path()
            == std::filesystem::absolute(autosave_dir)) {
        std::cerr << "checkpoint-at path must not sit inside the autosave directory\n";
        usage();
        return 2;
    }

    try {
        const auto core = gt4recomp::tools::read_verified_core(core_path);
        const auto image = reconstruct_core(core);

        // The disc image the file services read from, when the caller named
        // one. Without it the game's file opens answer "not found", exactly
        // like a console without a disc.
        std::unique_ptr<gt4recomp::DiscByteSource> disc_source;
        std::unique_ptr<gt4recomp::Iso9660Image> disc_image;
        std::unique_ptr<gt4recomp::DiscSectors> disc_sectors;
        if (!disc_path.empty()) {
            disc_source = gt4recomp::open_disc_file(disc_path.string());
            disc_image = std::make_unique<gt4recomp::Iso9660Image>(
                std::move(disc_source));
            // The game's own CD driver reads raw sectors, so the tool keeps
            // a second handle on the image for it. The sector source derives
            // the disc's volumes, so a dual-layer disc's second volume is
            // found by its logical blocks (decision 0020).
            disc_sectors = std::make_unique<gt4recomp::DiscSectors>(
                gt4recomp::open_disc_file(disc_path.string()));
            std::cout << "disc: " << disc_path.string() << " ("
                      << disc_image->directory_names("").size()
                      << " root entries";
            if (disc_sectors->second_volume_lba() != 0) {
                std::cout << "; a second volume begins at logical block 0x"
                          << std::hex << disc_sectors->second_volume_lba()
                          << std::dec << ", stored "
                          << disc_sectors->second_volume_shift()
                          << " blocks early";
            }
            std::cout << ")\n";
        }

        // The write-time provenance every photo of this run carries, and
        // the run's own identity line for the §22.3 run record.
        const CheckpointProvenance run_provenance =
            make_provenance(disc_image != nullptr, disc_path);
        print_run_identity(run_provenance);

        // Verify-resume runs the checkpoint file's leg and the same total
        // fresh, then requires identical stops and states. --services sets
        // the resumed leg; the direct leg runs the file's count plus that.
        // Both legs need budgets that let them finish their services.
        if (want_verify) {
            const CheckpointFile verify_file =
                load_checkpoint_file(read_file_bytes(verify_resume_path));
            // load_checkpoint_file already refused pre-P00 files and foreign
            // semantics; what remains is diagnosis for the run record.
            std::cout << "verify-resume from " << verify_resume_path.string()
                      << " (checkpoint cumulative " << verify_file.services_handled
                      << " services; this leg recounts from zero)\n";
            print_checkpoint_identity(verify_file);
            const std::uint64_t base_services = verify_file.services_handled;
            if (service_limit
                > std::numeric_limits<std::uint64_t>::max() - base_services) {
                throw std::runtime_error("The verify-resume total overflows");
            }
            const std::uint64_t total_services = base_services + service_limit;
            const auto wire_options = [](Kernel& kernel, RunOptions& options,
                                         std::uint64_t leg_services,
                                         std::uint64_t leg_steps) {
                options.step_limit = leg_steps;
                options.service_limit = leg_services;
                options.start_interrupt = [&kernel](GuestState& running) {
                    return kernel.start_interrupt(running);
                };
                options.start_idle_interrupt =
                    [&kernel](GuestState& running) {
                        return kernel.deliver_idle_interrupt(running);
                    };
                options.advance_time = [&kernel](GuestState& running) {
                    kernel.advance_service_time(running);
                };
            };
            const auto relink_disc = [&](Kernel& kernel) {
                if (disc_image != nullptr) {
                    kernel.set_disc_files(disc_image.get());
                }
                if (disc_sectors != nullptr) {
                    kernel.set_disc_sectors(disc_sectors.get());
                }
            };
            // The resumed leg: rebuild identically, apply the snapshot, run
            // the requested extra services.
            Kernel resumed_kernel;
            resumed_kernel.set_strict_rpc(strict_rpc);
            relink_disc(resumed_kernel);
            ServiceTable resumed_services = make_boot_services(resumed_kernel);
            BootDevices resumed_devices(
                [&resumed_kernel](std::uint32_t channel) {
                    resumed_kernel.raise_dmac_completion(channel);
                });
            resumed_devices.wire_kernel(resumed_kernel);
            GuestState resumed_state = make_boot_state(image, resumed_devices);
            {
                const Snapshot snapshot =
                    load_snapshot(verify_file.context_memory);
                resumed_state.restore_registers(snapshot.context);
                restore_memory(resumed_state.memory(), snapshot);
                resumed_kernel.load_kernel_state(verify_file.kernel);
                restore_banks(resumed_devices,
                              load_bank_section(verify_file.banks));
            }
            RunOptions resumed_options;
            wire_options(resumed_kernel, resumed_options, service_limit,
                         step_limit);
            Driver resumed_driver(resumed_state, make_boot_module());
            const RunResult resumed_result =
                resumed_driver.run(resumed_services, resumed_options);
            // The direct leg: the same total from the entry, uninterrupted.
            Kernel direct_kernel;
            direct_kernel.set_strict_rpc(strict_rpc);
            relink_disc(direct_kernel);
            ServiceTable direct_services = make_boot_services(direct_kernel);
            BootDevices direct_devices(
                [&direct_kernel](std::uint32_t channel) {
                    direct_kernel.raise_dmac_completion(channel);
                });
            direct_devices.wire_kernel(direct_kernel);
            GuestState direct_state = make_boot_state(image, direct_devices);
            RunOptions direct_options;
            wire_options(direct_kernel, direct_options, total_services,
                         step_limit);
            Driver direct_driver(direct_state, make_boot_module());
            const RunResult direct_result =
                direct_driver.run(direct_services, direct_options);
            const Boundary& resumed_boundary = resumed_result.boundary;
            const Boundary& direct_boundary = direct_result.boundary;
            if (resumed_boundary.kind != direct_boundary.kind
                || resumed_boundary.pc != direct_boundary.pc
                || resumed_boundary.service != direct_boundary.service) {
                std::cerr << "the resumed run stopped at "
                          << gt4recomp::tools::boundary_kind_name(
                                 resumed_boundary.kind)
                          << " 0x" << std::hex << std::setfill('0')
                          << std::setw(8) << resumed_boundary.pc << std::dec
                          << std::setfill(' ')
                          << " but the direct run stopped at "
                          << gt4recomp::tools::boundary_kind_name(
                                 direct_boundary.kind)
                          << " 0x" << std::hex << std::setfill('0')
                          << std::setw(8) << direct_boundary.pc << std::dec
                          << std::setfill(' ') << '\n';
                throw std::runtime_error("Resumed and direct stops differ");
            }
            if (!states_match(resumed_state, resumed_kernel, resumed_devices,
                              direct_state, direct_kernel, direct_devices)) {
                throw std::runtime_error("Resumed and direct states differ");
            }
            std::cout << "resume states identical (" << total_services
                      << " services cumulative since boot, digest 0x" << std::hex
                      << memory_digest(resumed_state) << std::dec << ")\n";
            return 0;
        }

        Kernel driver_kernel;
        driver_kernel.set_strict_rpc(strict_rpc);
        if (disc_image != nullptr) {
            driver_kernel.set_disc_files(disc_image.get());
        }
        if (disc_sectors != nullptr) {
            driver_kernel.set_disc_sectors(disc_sectors.get());
        }
        ServiceTable services = make_boot_services(driver_kernel);
        BootDevices driver_devices([&driver_kernel](std::uint32_t channel) {
            driver_kernel.raise_dmac_completion(channel);
        });
        driver_devices.wire_kernel(driver_kernel);
        auto driver_state = make_boot_state(image, driver_devices);
        // Option-A poll points (decision 0034): translated code asks the
        // kernel to deliver a synchronously raised DMA completion at the
        // next guest instruction, the pc the interpreter uses. The hook
        // lives on this state object, so a resumed run keeps it (the
        // snapshot applies onto the same object); the reference engine
        // never runs translated code, so it needs none.
        driver_state.set_dma_start_poll([&driver_kernel](GuestState& running) {
            return driver_kernel.start_interrupt(running);
        });
        // Resuming rebuilds everything identically, then applies the
        // snapshot over it: registers, RAM bytes, kernel state and device
        // registers in map order. Counters recount from zero, so --services
        // on a resumed run means that many more services; the stats line
        // below reports both the leg count and the cumulative total.
        std::uint64_t resume_services = 0;
        if (want_resume) {
            const CheckpointFile file =
                load_checkpoint_file(read_file_bytes(resume_path));
            std::cout << "resumed from " << resume_path.string()
                      << " (checkpoint cumulative " << file.services_handled
                      << " services; this leg recounts from zero)\n";
            print_checkpoint_identity(file);
            const Snapshot snapshot = load_snapshot(file.context_memory);
            driver_state.restore_registers(snapshot.context);
            restore_memory(driver_state.memory(), snapshot);
            driver_kernel.load_kernel_state(file.kernel);
            restore_banks(driver_devices, load_bank_section(file.banks));
            resume_services = file.services_handled;
        }
        RunOptions options;
        options.step_limit = step_limit;
        options.service_limit = service_limit;
        options.on_service = [quiet](std::uint32_t service, std::uint32_t pc) {
            if (quiet) {
                return;
            }
            std::cout << "service 0x" << std::hex << service << std::dec
                      << " at 0x" << std::hex << std::setfill('0') << std::setw(8)
                      << pc << std::dec << std::setfill(' ') << '\n';
        };
        options.start_interrupt = [&driver_kernel](GuestState& state) {
            return driver_kernel.start_interrupt(state);
        };
        options.start_idle_interrupt = [&driver_kernel](GuestState& state) {
            return driver_kernel.deliver_idle_interrupt(state);
        };
        options.advance_time = [&driver_kernel](GuestState& state) {
            driver_kernel.advance_service_time(state);
        };

        // The stop-time memory views the caller asked for: each line shows
        // eight words with the address at the left, so a structure's fields
        // can be read off after a run. The views print after a normal stop
        // and also when the guest faults, because they are what explains
        // the fault.
        const auto print_dumps = [&driver_state, &dumps]() {
            for (const auto& [address, length] : dumps) {
                std::cout << "dump 0x" << std::hex << std::setfill('0')
                          << std::setw(8) << address << std::dec << " ("
                          << length << " bytes):\n";
                for (std::uint32_t offset = 0; offset < length; offset += 4) {
                    if (offset % 32 == 0) {
                        std::cout << "  " << std::hex << std::setfill('0')
                                  << std::setw(8) << (address + offset) << ':';
                    }
                    std::cout << ' ' << std::hex << std::setfill('0')
                              << std::setw(8)
                              << driver_state.memory().read_word(address + offset)
                              << std::dec << std::setfill(' ');
                    if (offset % 32 == 28 || offset + 4 >= length) {
                        std::cout << '\n';
                    }
                }
            }
        };

        Driver driver(driver_state, make_boot_module());
        RunResult result;
        std::vector<std::string> skipped_moments;
        try {
            if (!want_autosave) {
                result = driver.run(services, options);
            } else {
                // Autosave photos the run leg by leg; the returned stats
                // accumulate across legs so the report below reads totals.
                std::vector<AutosavePhoto> inventory =
                    inventory_autosave_dir(autosave_dir);
                AutosaveOutcome autosave = run_with_autosave(
                    driver, services, options, driver_state, driver_kernel,
                    driver_devices, run_provenance, resume_services, service_limit,
                    step_limit, autosave_every, autosave_dir, autosave_keep,
                    autosave_has_byte_cap, autosave_max_bytes, inventory);
                result = autosave.result;
                skipped_moments = std::move(autosave.skipped_moments);
            }
        } catch (...) {
            print_dumps();
            throw;
        }
        for (const std::string& note : skipped_moments) {
            std::cout << note << '\n';
        }
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
                  << ", services handled " << result.stats.services_handled;
        // Leg-relative vs cumulative (decision 0028): a resumed run
        // recounts from zero, so its leg count plus the checkpoint base is
        // the cumulative total since boot; a fresh run's leg is its total.
        if (want_resume || want_autosave) {
            std::cout << " (leg-relative; cumulative "
                      << (resume_services + result.stats.services_handled)
                      << " since boot)";
        } else {
            std::cout << " (fresh run: leg == cumulative)";
        }
        std::cout << '\n';
        // Checkpointing saves the whole stop state, but only from a clean
        // service stop at exactly the requested count: a fault, a step
        // limit, or a transfer in flight refuses loudly instead of writing
        // a snapshot the resume could not faithfully continue.
        if (want_checkpoint) {
            if (result.stats.services_handled != checkpoint_at
                || boundary.kind != BoundaryKind::Syscall
                || driver.pending_transfer()) {
                throw std::runtime_error(
                    "Checkpoint requested, but the run stopped elsewhere");
            }
            const std::uint64_t framed_size = write_stop_checkpoint(
                driver_state, driver_kernel, driver_devices, run_provenance,
                checkpoint_at, checkpoint_path);
            std::cout << "checkpoint saved at " << checkpoint_at
                      << " services: " << checkpoint_path.string() << " ("
                      << framed_size << " bytes)";
            // The file holds the leg count (decision 0024's recount rule);
            // name the scope so a chained photo is never mistaken for a
            // cumulative total.
            if (want_resume || want_autosave) {
                std::cout << " [leg-relative; cumulative "
                          << (resume_services + checkpoint_at)
                          << " since boot]";
            } else {
                std::cout << " [cumulative]";
            }
            std::cout << '\n';
        }
        if (print_threads) {
            // The kernel's thread table at the stop: id, status bits, the
            // wait reason when waiting, priority, entry and resume pc.
            for (const KernelThread& thread : driver_kernel.threads()) {
                std::cout << "thread " << thread.id << ": status 0x" << std::hex
                          << thread.status << std::dec << ", wait " << thread.wait_type
                          << "/" << thread.wait_id << ", prio " << thread.current_priority
                          << ", entry 0x" << std::hex << std::setfill('0') << std::setw(8)
                          << thread.function << ", pc 0x" << std::setw(8)
                          << thread.context.pc << std::dec << std::setfill(' ') << '\n';
            }
            std::cout << "deferred calls: " << driver_kernel.deferred_call_count()
                      << ", pending interrupts: " << driver_kernel.pending_interrupts()
                      << '\n';
            // The registered interrupt and DMA handlers at the stop.
            for (const KernelInterruptHandler& registration :
                 driver_kernel.interrupt_handlers()) {
                std::cout << "intc handler: cause " << registration.cause
                          << ", handler 0x" << std::hex << std::setfill('0')
                          << std::setw(8) << registration.handler << std::dec
                          << std::setfill(' ') << '\n';
            }
            for (const KernelInterruptHandler& registration :
                 driver_kernel.dmac_handlers()) {
                std::cout << "dmac handler: channel " << registration.cause
                          << ", handler 0x" << std::hex << std::setfill('0')
                          << std::setw(8) << registration.handler << std::dec
                          << std::setfill(' ') << '\n';
            }
            // The SIF RPC servers the game has bound: the inventory the
            // async-IOP work starts from.
            for (const std::uint32_t sid : driver_kernel.sif_server_sids()) {
                std::cout << "sif server: sid 0x" << std::hex << std::setfill('0')
                          << std::setw(8) << sid << std::dec
                          << std::setfill(' ') << '\n';
            }
            // Slice-73 RPC telemetry (P07): the per-SID bind counts and
            // the per-(SID, function) call inventory from the real boot
            // above, with the honest class, the candidate name and the
            // consumer note. Completion is the synchronous end packet
            // plus the DMAC channel-5 queueing; no per-pair async
            // callback is tracked, so callback reads none-tracked.
            for (const auto& [sid, bind] : driver_kernel.rpc_bind_stats()) {
                std::cout << "rpc bind: sid 0x" << std::hex
                          << std::setfill('0') << std::setw(8) << sid
                          << std::dec << std::setfill(' ')
                          << " count " << bind.count
                          << " first-thread " << bind.first_thread
                          << " first-pc 0x" << std::hex << std::setfill('0')
                          << std::setw(8) << bind.first_pc << std::dec
                          << std::setfill(' ') << " threads";
                for (const auto& [thread, count] : bind.counts_by_thread) {
                    std::cout << " " << thread << "x" << count;
                }
                std::cout << '\n';
            }
            std::uint64_t rpc_unknown_calls = 0;
            for (const auto& [key, stats] : driver_kernel.rpc_pair_stats()) {
                (void)key;
                const RpcPairClass pair_class =
                    Kernel::classify_rpc_pair(stats.sid, stats.function);
                if (pair_class == RpcPairClass::Unknown) {
                    rpc_unknown_calls += stats.calls;
                }
                std::cout << "rpc pair: sid 0x" << std::hex
                          << std::setfill('0') << std::setw(8) << stats.sid
                          << " fn 0x" << std::setw(8) << stats.function
                          << std::dec << std::setfill(' ')
                          << " calls " << stats.calls << " class "
                          << Kernel::rpc_pair_class_name(pair_class)
                          << " candidate \""
                          << Kernel::rpc_pair_candidate_name(stats.sid)
                          << "\" threads";
                for (const auto& [thread, count] : stats.calls_by_thread) {
                    std::cout << " " << thread << "x" << count;
                }
                std::cout << " pcs 0x" << std::hex << std::setfill('0')
                          << std::setw(8) << stats.first_pc << "/0x"
                          << std::setw(8) << stats.last_pc << std::dec
                          << std::setfill(' ')
                          << " send " << stats.first_send_size << "/"
                          << stats.max_send_size << " recv "
                          << stats.first_recv_size << "/"
                          << stats.max_recv_size << " result "
                          << stats.first_result_size << "/"
                          << stats.max_result_size << " recvbuf 0x"
                          << std::hex << std::setfill('0') << std::setw(8)
                          << stats.first_recv_buffer << std::dec
                          << std::setfill(' ')
                          << " completion sync-end-packet+dmac5"
                          << " callback none-tracked consumer \""
                          << Kernel::rpc_pair_note(stats.sid, stats.function)
                          << "\"\n";
            }
            std::cout << "rpc inventory: "
                      << driver_kernel.rpc_pair_stats().size() << " pairs, "
                      << rpc_unknown_calls << " unknown calls\n";
            // The DMA channel registers: a channel with the STR bit (0x100)
            // still set was started and never completed. MADR/QWC/TADR show
            // the post-transfer state, and the start line counts what the
            // engine moved (decision 0033).
            const auto print_channel = [](const char* name, std::uint32_t chcr) {
                std::cout << "dma " << name << " chcr 0x" << std::hex << std::setfill('0')
                          << std::setw(8) << chcr << std::dec
                          << std::setfill(' ') << '\n';
            };
            const auto print_dma_channel = [](const char* name,
                                              const DmaChannel& channel,
                                              std::uint32_t base) {
                std::cout << "dma " << name << " chcr 0x" << std::hex << std::setfill('0')
                          << std::setw(8) << channel.register_value(base)
                          << " madr 0x" << std::setw(8)
                          << channel.register_value(base + DmaChannel::madr_offset)
                          << " qwc 0x" << std::setw(8)
                          << channel.register_value(base + DmaChannel::qwc_offset)
                          << " tadr 0x" << std::setw(8)
                          << channel.register_value(base + DmaChannel::tadr_offset)
                          << std::dec << std::setfill(' ');
                std::uint64_t start_bytes = 0;
                std::uint64_t start_tags = 0;
                for (const DmaChannel::StartRecord& start : channel.starts()) {
                    start_bytes += start.bytes_moved;
                    start_tags += start.tags_walked;
                }
                std::cout << " starts " << channel.starts().size()
                          << " tags " << start_tags << " bytes " << start_bytes
                          << " sink " << channel.payload_byte_count()
                          << " hash 0x" << std::hex << std::setfill('0')
                          << std::setw(8) << channel.payload_hash() << std::dec
                          << std::setfill(' ');
                // The walked chains by (first, last) tag id: the captured
                // shape of every start (decision 0033).
                std::map<std::uint32_t, std::uint64_t> chain_shapes;
                for (const DmaChannel::StartRecord& start : channel.starts()) {
                    if (!start.completed) {
                        continue;
                    }
                    chain_shapes[(start.first_tag_id << 3)
                                 | start.last_tag_id] += 1;
                }
                const char* tag_name[8] = {"refe", "cnt",  "next", "ref",
                                           "refs", "call", "ret",  "end"};
                for (const auto& [shape, count] : chain_shapes) {
                    const std::uint32_t first = (shape >> 3) & 0x1FFFu;
                    const std::uint32_t last = shape & 7u;
                    std::cout << " ["
                              << (first <= DmaChannel::tag_end
                                      ? tag_name[first]
                                      : "normal")
                              << ".."
                              << (last <= DmaChannel::tag_end
                                      ? tag_name[last]
                                      : "normal")
                              << " x" << count << "]";
                }
                std::cout << '\n';
            };
            print_dma_channel("vif0", driver_devices.vif0_dma, 0x10008000u);
            print_dma_channel("vif1", driver_devices.vif1_dma, 0x10009000u);
            print_dma_channel("gif", driver_devices.gif_dma, 0x1000A000u);
            // SIF0 stays a plain bank: the game enables it through the
            // SifSetDChain service (the model's own 0x184 write), and every
            // SIF payload moves through the SifSetDma service path, never
            // through a guest-programmed register start. MADR/QWC/TADR here
            // show no such start exists to complete (decision 0033).
            std::cout << "dma sif0 chcr 0x" << std::hex << std::setfill('0')
                      << std::setw(8)
                      << driver_devices.sif0.register_value(0x1000C000u)
                      << " madr 0x" << std::setw(8)
                      << driver_devices.sif0.register_value(0x1000C010u)
                      << " qwc 0x" << std::setw(8)
                      << driver_devices.sif0.register_value(0x1000C020u)
                      << " tadr 0x" << std::setw(8)
                      << driver_devices.sif0.register_value(0x1000C030u)
                      << std::dec << std::setfill(' ') << '\n';
            // The DMAC completion status the channels reported into: a set
            // CIS bit with no handler run is pending, not lost (0029).
            std::cout << "dmac stat 0x" << std::hex << std::setfill('0')
                      << std::setw(8)
                      << driver_devices.dmac.register_value(0x1000E010u)
                      << std::dec << std::setfill(' ') << '\n';
            print_channel("spr0", driver_devices.spr_dma.register_value(0x1000D000u));
            print_channel("spr1", driver_devices.spr_dma.register_value(0x1000D400u));
            // The four timers: count, mode and compare as the guest left
            // them.
            for (std::uint32_t index = 0; index < TimerUnit::timer_count; ++index) {
                const std::uint32_t timer_base =
                    TimerUnit::window_base + index * TimerUnit::timer_stride;
                std::cout << "timer " << index << ": count 0x" << std::hex
                          << std::setfill('0') << std::setw(8)
                          << driver_devices.timer.register_value(
                                 timer_base + TimerUnit::count_offset)
                          << ", mode 0x" << std::setw(8)
                          << driver_devices.timer.register_value(
                                 timer_base + TimerUnit::mode_offset)
                          << ", comp 0x" << std::setw(8)
                          << driver_devices.timer.register_value(
                                 timer_base + TimerUnit::compare_offset)
                          << std::dec << std::setfill(' ') << '\n';
            }
        }

        print_dumps();

        if (compare_interpreter) {
            Kernel reference_kernel;
            reference_kernel.set_strict_rpc(strict_rpc);
            if (disc_image != nullptr) {
                reference_kernel.set_disc_files(disc_image.get());
            }
            if (disc_sectors != nullptr) {
                reference_kernel.set_disc_sectors(disc_sectors.get());
            }
            ServiceTable reference_services = make_boot_services(reference_kernel);
            BootDevices reference_devices([&reference_kernel](std::uint32_t channel) {
                reference_kernel.raise_dmac_completion(channel);
            });
            reference_devices.wire_kernel(reference_kernel);
            auto reference_state = make_boot_state(image, reference_devices);
            // A resumed run sits N services in: the reference must run the
            // same total from the entry.
            std::uint64_t reference_total = service_limit;
            if (want_resume) {
                if (service_limit
                    > std::numeric_limits<std::uint64_t>::max()
                          - resume_services) {
                    throw std::runtime_error(
                        "The compare total overflows after resume");
                }
                reference_total = resume_services + service_limit;
            }
            const ReferenceResult reference = run_reference(
                reference_state, reference_services, reference_kernel,
                default_step_limit, reference_total);
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
            if (!states_match(driver_state, driver_kernel, driver_devices,
                              reference_state, reference_kernel,
                              reference_devices)) {
                std::cerr << "the driver and the interpreter states differ\n";
                return 1;
            }
            std::cout << "interpreter: " << reference.interpreted_steps
                      << " instructions, state identical (registers, RAM "
                         "regions, kernel, device banks)\n";
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAILURE: " << error.what() << '\n';
        return 1;
    }
}
