// MIT License. Copyright (c) 2026 João Alano and GT4Recomp contributors.
// Written from docs/hw-renderer-spec/CORE2-SPEC.md and CORE2-DESIGN.md (TASK-027 core rewrite), clean room
// (decision 0070).
//
// The GPU side of CORE2. Every target is one r32ui storage image of GS words at the internal scale (colour and depth
// alike); nothing is kept at 1x. Draws use the existing vertex and uber pixel shaders (blending, depth, alpha test,
// DATE, dither and fog in the shader on storage images). Compute passes patch native rectangles from GS memory into
// a target (4.1), reduce a target to 1x words for a readback (4.2), run GT4's channel-shuffle hook (7.2) and merge the
// display (5.3). Work is recorded into one command buffer and submitted at the end of a frame, or earlier when the CPU
// needs a result. The public header stays Vulkan-free.
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "../writeback/hw_gs_writeback_gpu.hpp"

namespace hwgs::core2 {

// A GPU image handle (index into the image table). kNoImage: none.
using ImageId = std::int32_t;
constexpr ImageId kNoImage = -1;

// One batch of draws into a colour target (and a depth target), as CORE2's renderer hands it over.
struct DrawBatch {
    ImageId frame = kNoImage;     // colour target (required)
    ImageId depth = kNoImage;     // depth target, or kNoImage when the draw neither tests nor writes depth
    ImageId texture = kNoImage;   // CORE2-SPEC 3.2: a target sampled at scale (tex_scale = s), else kNoImage
    // A texture decoded from GS memory (3.7): a 1x image of its own, texel (0, 0) at the image origin.
    ImageId memoryTexture = kNoImage;
    gpu::GpuPixelState state;
    const std::vector<gpu::GpuVertex> *vertices = nullptr;
    const std::vector<gpu::GpuDraw> *draws = nullptr;
    std::uint32_t zMax = 0xFFFFFFFFu;
};

// Diagnostics for the F8 report and the replay summary.
struct Core2GpuStats {
    std::uint64_t batches = 0;
    std::uint64_t draws = 0;
    std::uint64_t patches = 0;
    std::uint64_t reductions = 0;
    std::uint64_t tones = 0;
    std::uint64_t composes = 0;
    std::uint64_t deinterlaces = 0;
    std::uint64_t fxaaFrames = 0;
    std::uint64_t submissions = 0;
    std::uint64_t waits = 0;
    std::uint64_t atlasUploads = 0;
    std::uint64_t atlasResets = 0;
    std::uint64_t imageBytes = 0;
};

class Gpu {
public:
    Gpu();
    ~Gpu();
    Gpu(const Gpu &) = delete;
    Gpu &operator=(const Gpu &) = delete;

    // `scale` is the internal scale s; `shift` the 06.3 half-pixel shift factor of the vertex stage.
    bool Init(const gpu::VulkanHandles &handles, std::uint32_t scale, float shift, std::string &error);
    bool Ready() const;
    std::uint32_t Scale() const;
    std::uint32_t MaxNativeSide() const;  // the largest native side an image can have at this scale

    // Images of `nativeWidth` x `nativeHeight` GS pixels (the image is that size times s; `scaled` false: 1x).
    // New images are cleared to zero.
    ImageId CreateImage(std::uint32_t nativeWidth, std::uint32_t nativeHeight, bool scaled, std::string &error);
    // Grows an image (1.4): a new image of the new size gets the old content copied across at scale.
    bool GrowImage(ImageId id, std::uint32_t nativeWidth, std::uint32_t nativeHeight, std::string &error);
    void DestroyImage(ImageId id);  // released once the GPU is done with it
    void ImageSize(ImageId id, std::uint32_t &nativeWidth, std::uint32_t &nativeHeight) const;
    // Copies a native rectangle of one image into another at the same position, at scale (2.4a, same format class).
    bool CopyImage(ImageId from, ImageId to, std::int32_t x0, std::int32_t y0, std::int32_t x1, std::int32_t y1,
                   std::string &error);

    // Texture atlas (1x, decoded RGBA8 texels from GS memory). Allocate returns false when the atlas is full; the
    // caller then forgets every cached place and calls ResetAtlas.
    bool AllocateAtlas(std::uint32_t width, std::uint32_t height, std::uint32_t &x, std::uint32_t &y);
    void ResetAtlas();
    bool UploadAtlas(std::uint32_t x, std::uint32_t y, std::uint32_t width, std::uint32_t height,
                     const std::uint32_t *texels, std::string &error);
    // Writes width x height texels (row-major) at the origin of a 1x image.
    bool UploadImage(ImageId id, std::uint32_t width, std::uint32_t height, const std::uint32_t *texels,
                     std::string &error);

    bool Draw(const DrawBatch &batch, std::string &error);

    // The GPU copy of GS memory (the GLM, 4 MiB) that textures are decoded from (3.7): byte ranges of `memory`
    // (offset, size) copied into it.
    bool UploadGlm(const std::uint8_t *memory, const std::vector<std::pair<std::uint32_t, std::uint32_t>> &ranges,
                   std::string &error);
    // Decodes a texture from the GLM into a 1x image (texel (0, 0) at its origin): stored words for direct formats
    // (the pixel shader expands CT16/CT24), RGBA8 through `palette` (256 entries) for indexed ones.
    bool DecodeTexture(ImageId id, std::uint32_t bp, std::uint32_t bw, std::uint32_t psm, std::uint32_t width,
                       std::uint32_t height, const std::uint32_t *palette, std::string &error);

    // 4.1: native rectangle (inclusive) of stored words, row-major, written into the image at scale; `mask` selects
    // the bits written.
    bool Patch(ImageId id, std::int32_t x0, std::int32_t y0, std::int32_t x1, std::int32_t y1,
               const std::vector<std::uint32_t> &words, std::uint32_t mask, std::string &error);
    // 4.1 from the GLM: the rectangle read on the GPU in (bp, bw, psm) addressing (UploadGlm first).
    bool PatchFromGlm(ImageId id, std::uint32_t bp, std::uint32_t bw, std::uint32_t psm, std::int32_t x0,
                      std::int32_t y0, std::int32_t x1, std::int32_t y1, std::uint32_t mask, std::string &error);
    // 4.2: the native rectangle (inclusive) reduced to 1x stored words (submits and waits).
    bool Reduce(ImageId id, std::int32_t x0, std::int32_t y0, std::int32_t x1, std::int32_t y1,
                std::vector<std::uint32_t> &words, std::string &error);
    // 7.2: GT4's channel-shuffle hook over the image's native rectangle [0, width) x [0, height).
    bool Tone(ImageId id, std::uint32_t nativeWidth, std::uint32_t nativeHeight, const std::uint32_t palette[256],
              std::string &error);

    // 5.3: the display merge. `words` is the compose push block (28 words, core2_compose.comp); sources are the
    // images of circuits 1 and 2 (kNoImage: not shown). The result is the display image (RGBA8 words, GENERAL).
    bool Compose(const std::uint32_t words[28], ImageId sourceA, ImageId sourceB, std::uint32_t outWidth,
                 std::uint32_t outHeight, std::string &error);
    std::uint64_t DisplayImage() const;  // VkImage
    // 13.4 at scale (IN-405): deinterlaces the top-left step.width x step.height pixels of the display image in
    // place with the old core's kernel and step (hw_deinterlace_scaled.comp: the display image is first copied into
    // history entry `current`). The history images (four merge images, the woven image, the last adaptive output)
    // follow the display size; the caller keeps which entry holds which field.
    bool Deinterlace(const gpu::GpuDrawer::DeinterlaceStep &step, std::string &error);
    // 13.5 (IN-376, IN-405 in core 2): FXAA of the display image's top-left width x height pixels into the FXAA image.
    bool Fxaa(std::uint32_t width, std::uint32_t height, std::string &error);
    std::uint64_t FxaaImage() const;  // VkImage
    // `fxaa`: read the FXAA image instead of the display image.
    bool ReadDisplay(std::uint32_t width, std::uint32_t height, std::vector<std::uint32_t> &pixels, std::string &error,
                     bool fxaa = false);
    // Diagnostics: the top-left width x height words of an image (submits and waits).
    bool ReadImage(ImageId id, std::uint32_t width, std::uint32_t height, std::vector<std::uint32_t> &words,
                   std::string &error);

    bool Submit(bool wait, std::string &error);
    const Core2GpuStats &Stats() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace hwgs::core2
