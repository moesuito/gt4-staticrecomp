// MIT License. Copyright (c) 2026 João Alano and GT4Recomp contributors.
// Written from docs/hw-renderer-spec/CORE2-SPEC.md and CORE2-DESIGN.md (TASK-027 core rewrite), clean room
// (decision 0070).
//
// CORE2, the CoreGS 2.0 core rewrite (owner decision 2026-10-28): a target cache in which every target is ONE image
// at the internal scale. GS memory (the CPU shadow memory) holds what the EE uploaded; target content reaches it only
// through readbacks, reduced by point sampling (CORE2-SPEC 4.2). A target is never rebuilt from 1x memory, except
// for the rectangles the EE really uploaded into it (4.1). writeback::Renderer forwards to this class when
// GT4_HW_CORE=2.
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "../writeback/hw_gs_writeback.hpp"

namespace hwgs::core2 {

// The session counters of CORE2 (replay summary and the [hw] core2 line of the F8 report).
struct Core2Counters {
    std::uint64_t targetsCreated = 0;
    std::uint64_t targetsAged = 0;        // deleted after 60 frames unused (6.2)
    std::uint64_t targetsReplaced = 0;    // replaced on a width change (2.1) or full dirty coverage (4.1)
    std::uint64_t targetsGrown = 0;
    std::uint64_t texturesFromTargets = 0;
    std::uint64_t texturesFromMemory = 0;
    std::uint64_t atlasHits = 0;
    std::uint64_t dirtyPatches = 0;
    std::uint64_t dirtyPatchPixels = 0;
    std::uint64_t readbacks = 0;
    std::uint64_t readbackPixels = 0;
    std::uint64_t toneRuns = 0;
    std::uint64_t toneDrawsSkipped = 0;
    std::uint64_t toneAlphaOnlyUnsupported = 0;  // 7.2 alpha-only variant: drawn normally (not implemented yet)
    std::uint64_t displayFromTargets = 0;
    std::uint64_t displayFromMemory = 0;
    std::uint64_t indexedFromTargetReadbacks = 0;  // interim 3.5: a T8/T4 texture over a drawn target
    std::uint64_t directFromTargetReadbacks = 0;   // a 16/24/32-bit memory texture over a drawn target (not a hit)
    std::uint64_t clutReadbacks = 0;               // CLUT loads over a drawn target (GT4 draws palettes)
};

class Renderer {
public:
    Renderer();
    ~Renderer();
    Renderer(const Renderer &) = delete;
    Renderer &operator=(const Renderer &) = delete;

    void SetScale(std::uint32_t scale, float shift);
    std::uint32_t Scale() const;
    bool Init(const gpu::VulkanHandles &handles, std::string &error);

    bool GifTransfer(std::uint32_t path, const std::uint8_t *bytes, std::size_t byteCount);
    bool GifBusy(std::uint32_t path) const;
    void GifReset(std::uint32_t pathMask);
    bool WritePrivileged(std::uint32_t address, std::uint64_t value);
    void ResetGs();
    void ClearVram();
    std::size_t ReadFifo(void *destination, std::size_t byteCount);
    void Flush();

    bool ComposeFrameGpu(writeback::GpuFrame &out);
    bool ReadGpuFrame(const writeback::GpuFrame &frame, std::vector<std::uint32_t> &pixels);
    void ComposeFrame(writeback::FrameOutput &out);
    void SetDisplayField(bool odd);
    bool DisplayField() const;
    void SetOutputSettings(const writeback::OutputSettings &settings);

    // The shadow memory after writing every target's drawn area back (a host read of the whole memory).
    const std::uint8_t *Slm();
    const writeback::Counters &GetCounters() const;
    const Core2Counters &Core2Stats() const;
    bool Failed() const;
    const std::string &FailureText() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace hwgs::core2
