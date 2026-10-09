#include "gt4recomp/ee_checkpoint.hpp"
#include "gt4recomp/ee_state.hpp"

#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

using namespace gt4recomp::ee;

namespace {

bool contexts_equal(const RegisterContext& left, const RegisterContext& right) {
    return left.gpr == right.gpr && left.gpr_high == right.gpr_high
        && left.fpr == right.fpr && left.hi == right.hi && left.lo == right.lo
        && left.hi1 == right.hi1 && left.lo1 == right.lo1
        && left.fpu_accumulator == right.fpu_accumulator
        && left.fpu_control == right.fpu_control
        && left.shift_amount_cache == right.shift_amount_cache
        && left.cp0 == right.cp0 && left.vu0_vf == right.vu0_vf
        && left.vu0_vi == right.vu0_vi
        && left.vu0_clip_flag == right.vu0_clip_flag
        && left.vu0_acc == right.vu0_acc
        && left.vu0_mac_flag == right.vu0_mac_flag
        && left.vu0_status_flag == right.vu0_status_flag && left.pc == right.pc;
}

RegisterContext make_context() {
    RegisterContext context;
    for (std::size_t index = 0; index < 32; ++index) {
        context.gpr[index] = 0x1000 + index;
        context.gpr_high[index] = 0x2000 + index;
        context.fpr[index] = 0x3000 + static_cast<std::uint32_t>(index);
        context.cp0[index] = 0x4000 + static_cast<std::uint32_t>(index);
        context.vu0_vi[index] = 0x5000 + static_cast<std::uint32_t>(index);
        for (std::size_t lane = 0; lane < 4; ++lane) {
            context.vu0_vf[index][lane] =
                0x6000 + static_cast<std::uint32_t>(index * 4 + lane);
        }
    }
    context.hi = 0x7001;
    context.lo = 0x7002;
    context.hi1 = 0x7003;
    context.lo1 = 0x7004;
    context.fpu_accumulator = 0x7005;
    context.fpu_control = 0x7006;
    context.shift_amount_cache = 0x7007;
    context.vu0_clip_flag = 0x7008;
    for (std::size_t lane = 0; lane < 4; ++lane) {
        context.vu0_acc[lane] = 0x7010 + static_cast<std::uint32_t>(lane);
    }
    context.vu0_mac_flag = 0x7020;
    context.vu0_status_flag = 0x7030;
    context.pc = 0x00100008;
    return context;
}

} // namespace

int main() {
    int failures = 0;
    const auto check = [&](bool passed, const char* label) {
        if (!passed) { std::cerr << label << '\n'; ++failures; }
    };
    const auto throws = []<typename Action>(Action&& action) {
        try {
            action();
        } catch (const std::runtime_error&) {
            return true;
        }
        return false;
    };

    // A two-region memory with a pattern in each region.
    GuestMemory memory(0x00100000, 0x100);
    memory.map_region(0x70000000, 0x40);
    memory.enable_segment_alias();
    for (std::uint32_t offset = 0; offset < 0x100; ++offset) {
        memory.write_byte(0x00100000 + offset,
                           static_cast<std::uint8_t>(offset & 0xFFu));
    }
    for (std::uint32_t offset = 0; offset < 0x40; ++offset) {
        memory.write_byte(0x70000000 + offset,
                           static_cast<std::uint8_t>(0xA0 + offset));
    }

    const RegisterContext context = make_context();
    const std::vector<std::uint8_t> blob =
        save_snapshot(context, memory.segment_alias_enabled(),
                      memory.regions_snapshot());
    // 8 magic + 1484 context + 4 alias + 4 count + (4 + 4 + 0x100)
    // + (4 + 4 + 0x40) bytes.
    check(blob.size() == 8 + 1484 + 4 + 4 + (8 + 0x100) + (8 + 0x40),
          "the snapshot size is exact");

    // Mutate everything, then restore and compare.
    GuestMemory restored(0x00100000, 0x100);
    restored.map_region(0x70000000, 0x40);
    const Snapshot snapshot = load_snapshot(blob);
    check(contexts_equal(snapshot.context, context), "the context parses back");
    check(snapshot.segment_alias, "the alias flag parses back");
    check(snapshot.regions.size() == 2 && snapshot.regions[0].base == 0x00100000
              && snapshot.regions[1].bytes.size() == 0x40,
          "the regions parse back");
    restore_memory(restored, snapshot);
    check(restored.segment_alias_enabled(), "the alias restores");
    bool bytes_match = true;
    for (std::uint32_t offset = 0; offset < 0x100; ++offset) {
        bytes_match = bytes_match
            && restored.read_byte(0x00100000 + offset)
                == static_cast<std::uint8_t>(offset & 0xFFu);
    }
    for (std::uint32_t offset = 0; offset < 0x40; ++offset) {
        bytes_match = bytes_match
            && restored.read_byte(0x70000000 + offset)
                == static_cast<std::uint8_t>(0xA0 + offset);
    }
    check(bytes_match, "the region bytes restore");

    // GuestState round-trips through its own accessors too.
    GuestState state(std::move(restored));
    state.restore_registers(snapshot.context);
    check(contexts_equal(state.save_registers(), context),
          "the state round-trips registers");

    // Malformed blobs stop loudly.
    std::vector<std::uint8_t> bad_magic = blob;
    bad_magic[0] = 'X';
    check(throws([&] { (void)load_snapshot(bad_magic); }),
          "a bad magic throws");
    check(throws([&] { (void)load_snapshot({blob.data(), 10}); }),
          "a truncation throws");
    std::vector<std::uint8_t> bad_alias = blob;
    bad_alias[8 + 1484] = 2;
    check(throws([&] { (void)load_snapshot(bad_alias); }),
          "a bad alias flag throws");

    // A different geometry refuses the restore.
    GuestMemory other_geometry(0x00200000, 0x100);
    check(throws([&] { restore_memory(other_geometry, snapshot); }),
          "a geometry mismatch throws");

    // The checkpoint file frames the compatibility identity, the
    // provenance, the service count and the three sections.
    {
        CheckpointFile file;
        file.services_handled = 90000;
        file.context_memory = blob;
        file.kernel = {1, 2, 3};
        file.banks = {4, 5};
        file.provenance.git_commit = "abc123";
        file.provenance.binary = "gt4boot Debug MSVC 1944";
        file.provenance.core_sha256 = "core-hash";
        file.provenance.disc = "absent";
        file.provenance.time_policy = "service-clock 1ms/service";
        const std::vector<std::uint8_t> framed = save_checkpoint_file(file);
        check(framed.size() >= 7
                  && framed[0] == 'G' && framed[1] == 'T' && framed[2] == '4'
                  && framed[3] == 'C' && framed[4] == 'P' && framed[5] == 'T'
                  && framed[6] == '3',
              "the checkpoint file carries the GT4CPT3 format magic");
        const CheckpointFile parsed = load_checkpoint_file(framed);
        check(parsed.services_handled == 90000
                  && parsed.context_memory == blob
                  && parsed.kernel == std::vector<std::uint8_t>{1, 2, 3}
                  && parsed.banks == std::vector<std::uint8_t>{4, 5}
                  && parsed.compatibility.time_model
                    == current_model_compatibility().time_model
                  && parsed.provenance.git_commit == "abc123"
                  && parsed.provenance.disc == "absent",
              "the checkpoint file parses back with identity and provenance");
        std::vector<std::uint8_t> bad_file_magic = framed;
        bad_file_magic[0] = 'X';
        check(throws([&] { (void)load_checkpoint_file(bad_file_magic); }),
              "a bad file magic throws");
        check(throws([&] { (void)load_checkpoint_file({framed.data(), 9}); }),
              "a truncated file throws");
        std::vector<std::uint8_t> trailing = framed;
        trailing.push_back(0);
        check(throws([&] { (void)load_checkpoint_file(trailing); }),
              "trailing file bytes throw");
    }

    // Model compatibility gates the restore; provenance never does.
    {
        CheckpointFile file;
        file.services_handled = 400;
        file.context_memory = blob;
        file.kernel = {1};
        file.banks = {2};
        file.provenance.git_commit = "writer-commit";
        const std::vector<std::uint8_t> framed = save_checkpoint_file(file);
        check(describe_compatibility_mismatch(current_model_compatibility())
                  .empty(),
              "the current model matches itself");
        // An editorial change (a new commit, another binary) with the same
        // semantics stays loadable.
        CheckpointFile editorial = file;
        editorial.provenance.git_commit = "another-commit";
        editorial.provenance.binary = "another-binary";
        const CheckpointFile reparsed =
            load_checkpoint_file(save_checkpoint_file(editorial));
        check(reparsed.services_handled == 400
                  && reparsed.provenance.git_commit == "another-commit",
              "provenance alone never invalidates a checkpoint");
        // Each semantic domain refused on its own, naming the domain.
        const auto refuses_naming = [&](ModelCompatibility bumped,
                                        const char* domain) {
            CheckpointFile other = file;
            other.compatibility = bumped;
            bool named = false;
            try {
                (void)load_checkpoint_file(save_checkpoint_file(other));
            } catch (const std::runtime_error& error) {
                named = std::string(error.what()).find(domain)
                    != std::string::npos;
            }
            return named;
        };
        ModelCompatibility time_bump = current_model_compatibility();
        time_bump.time_model += 1;
        ModelCompatibility interrupt_bump = current_model_compatibility();
        interrupt_bump.interrupt_model += 1;
        ModelCompatibility kernel_bump = current_model_compatibility();
        kernel_bump.kernel_model += 1;
        ModelCompatibility rpc_bump = current_model_compatibility();
        rpc_bump.rpc_model += 1;
        ModelCompatibility translation_bump = current_model_compatibility();
        translation_bump.translation_model += 1;
        check(refuses_naming(time_bump, "time"),
              "a foreign time model is refused naming time");
        check(refuses_naming(interrupt_bump, "interrupt"),
              "a foreign interrupt model is refused naming interrupt");
        check(refuses_naming(kernel_bump, "kernel"),
              "a foreign kernel model is refused naming kernel");
        check(refuses_naming(rpc_bump, "rpc"),
              "a foreign rpc model is refused naming rpc");
        check(refuses_naming(translation_bump, "translation"),
              "a foreign translation model is refused naming translation");
        // Pre-P00 files carry no semantic identity: forensic, never a
        // resume source, and the message says so.
        std::vector<std::uint8_t> old_magic = framed;
        old_magic[6] = '1';
        bool forensic = false;
        try {
            (void)load_checkpoint_file(old_magic);
        } catch (const std::runtime_error& error) {
            forensic = std::string(error.what()).find("forensic")
                != std::string::npos;
        }
        check(forensic, "a pre-P00 checkpoint is refused as forensic");
        // GT4CPT2 predates the semaphore id-space fix (decision 0039): the
        // refusal names that too instead of a generic bad magic.
        std::vector<std::uint8_t> id_space_magic = framed;
        id_space_magic[6] = '2';
        bool id_space_forensic = false;
        try {
            (void)load_checkpoint_file(id_space_magic);
        } catch (const std::runtime_error& error) {
            id_space_forensic = std::string(error.what()).find("id-space")
                != std::string::npos;
        }
        check(id_space_forensic,
              "a GT4CPT2 checkpoint is refused as forensic");
    }

    // The provenance section round-trips its five fields standalone.
    {
        CheckpointProvenance provenance;
        provenance.git_commit = "deadbeef";
        provenance.binary = "gt4boot Debug MSVC 1944";
        provenance.core_sha256 = "core-hash";
        provenance.disc = "absent";
        provenance.time_policy = "service-clock 1ms/service";
        const std::vector<std::uint8_t> section =
            save_provenance_section(provenance);
        const CheckpointProvenance parsed = load_provenance_section(section);
        check(parsed.git_commit == "deadbeef"
                  && parsed.binary == "gt4boot Debug MSVC 1944"
                  && parsed.core_sha256 == "core-hash"
                  && parsed.disc == "absent"
                  && parsed.time_policy == "service-clock 1ms/service",
              "the provenance section parses back");
        std::vector<std::uint8_t> bad_magic = section;
        bad_magic[0] = 'X';
        check(throws([&] { (void)load_provenance_section(bad_magic); }),
              "a bad provenance magic throws");
        check(throws(
                  [&] { (void)load_provenance_section({section.data(), 9}); }),
              "a truncated provenance section throws");
        std::vector<std::uint8_t> trailing = section;
        trailing.push_back(0);
        check(throws([&] { (void)load_provenance_section(trailing); }),
              "trailing provenance bytes throw");
    }

    // Autosave photo names round-trip; foreign names are refused so
    // rotation never touches files it did not write (decision 0027).
    {
        check(format_autosave_name(400) == "ckpt-400.bin",
              "the photo name formats");
        std::uint64_t parsed = 0;
        check(parse_autosave_name("ckpt-400.bin", parsed) && parsed == 400,
              "the photo name parses back");
        check(parse_autosave_name("ckpt-0.bin", parsed) && parsed == 0,
              "service zero parses");
        check(!parse_autosave_name("ckpt-test.bin", parsed),
              "a non-numeric photo name is refused");
        check(!parse_autosave_name("ckpt-.bin", parsed),
              "an empty photo count is refused");
        check(!parse_autosave_name("ckpt-400.txt", parsed),
              "a wrong photo suffix is refused");
        check(!parse_autosave_name("ckpt-400.bin.bak", parsed),
              "a suffixed photo name is refused");
        check(!parse_autosave_name("manual.bin", parsed),
              "a foreign name is refused");
        check(!parse_autosave_name("ckpt-12x.bin", parsed),
              "a mixed photo count is refused");
        check(!parse_autosave_name("ckpt-99999999999999999999999.bin",
                                   parsed),
              "an overflowing photo count is refused");
    }

    // Rotation selects oldest-first: beyond keep, then over the byte
    // cap with the fresh photo protected.
    {
        const auto is_beyond_keep = [](AutosaveEvictReason reason) {
            return reason == AutosaveEvictReason::BeyondKeep;
        };
        const auto is_over_cap = [](AutosaveEvictReason reason) {
            return reason == AutosaveEvictReason::OverByteCap;
        };
        const std::vector<AutosavePhoto> four =
            {{200, 10}, {400, 10}, {600, 10}, {800, 10}};
        const std::vector<AutosaveEviction> keep_two =
            select_autosave_evictions(four, 2, false, 0, 800);
        check(keep_two.size() == 2 && keep_two[0].services == 200
                  && keep_two[1].services == 400
                  && is_beyond_keep(keep_two[0].reason)
                  && is_beyond_keep(keep_two[1].reason),
              "rotation evicts the oldest beyond keep");
        check(select_autosave_evictions(four, 4, false, 0, 800).empty(),
              "a fitting keep evicts nothing");
        check(select_autosave_evictions({}, 2, true, 100, 800).empty(),
              "an empty directory evicts nothing");
        // 40 bytes kept against a 25-byte cap: 200 goes (30 left, still
        // over), then 400 (20 left, under) — both for the cap.
        const std::vector<AutosaveEviction> capped =
            select_autosave_evictions(four, 4, true, 25, 800);
        check(capped.size() == 2 && capped[0].services == 200
                  && capped[1].services == 400
                  && is_over_cap(capped[0].reason)
                  && is_over_cap(capped[1].reason),
              "the byte cap evicts oldest-first");
        // One photo alone over the cap stays: the run never deletes the
        // photo it just wrote.
        const std::vector<AutosaveEviction> lone =
            select_autosave_evictions({{800, 50}}, 1, true, 25, 800);
        check(lone.empty(), "an over-cap fresh photo stays");
        // Rotation is strictly by service number: when higher-numbered
        // files already fill the keep window, even the fresh photo goes.
        const std::vector<AutosaveEviction> stale_higher =
            select_autosave_evictions({{400, 10}, {1000, 10}}, 1, false, 0,
                                      400);
        check(stale_higher.size() == 1
                  && stale_higher[0].services == 400,
              "rotation counts entries, not freshness");
    }

    return failures == 0 ? 0 : 1;
}
