// MIT License. Copyright (c) 2026 João Alano and GT4Recomp contributors.
// Written from docs/hw-renderer-spec/CORE2-SPEC.md and CORE2-DESIGN.md (TASK-027 core rewrite), clean room
// (decision 0070). Section numbers in comments refer to CORE2-SPEC.

#include "core2_renderer.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <unordered_map>

#include "core2_gpu.hpp"
#include "hw_batch.hpp"
#include "hw_clut.hpp"
#include "hw_front_end.hpp"
#include "hw_gs_display.hpp"
#include "hw_gs_registers.hpp"
#include "hw_gs_types.hpp"
#include "hw_memory.hpp"
#include "../cpuref/cpuref_memory.hpp"

namespace hwgs::core2 {
namespace {

using memory::BufferView;
using memory::Psm;

constexpr std::uint32_t kAllBlocks = memory::kBlocksTotal;
constexpr std::uint32_t kAgeLimitFrames = 60;      // 6.2
constexpr std::uint64_t kRecentUploadDraws = 500;  // 2.4b

namespace priv {
constexpr std::uint32_t kPage = 0x12000000u;
constexpr std::uint32_t kPmode = kPage + 0x00;
constexpr std::uint32_t kSmode1 = kPage + 0x10;
constexpr std::uint32_t kSmode2 = kPage + 0x20;
constexpr std::uint32_t kSyncv = kPage + 0x60;
constexpr std::uint32_t kDispfb1 = kPage + 0x70;
constexpr std::uint32_t kDisplay1 = kPage + 0x80;
constexpr std::uint32_t kDispfb2 = kPage + 0x90;
constexpr std::uint32_t kDisplay2 = kPage + 0xA0;
constexpr std::uint32_t kBgcolor = kPage + 0xE0;
constexpr std::uint32_t kCsr = kPage + 0x1000;
}  // namespace priv

// 1.3: format classes. 32- and 24-bit colour share a target, as do Z32 and Z24.
enum class FormatClass : std::uint8_t { Colour32, Colour16, Depth32, Depth16, None };

FormatClass ClassOf(Psm psm) {
    switch (psm) {
        case Psm::Ct32:
        case Psm::Ct24: return FormatClass::Colour32;
        case Psm::Ct16:
        case Psm::Ct16S: return FormatClass::Colour16;
        case Psm::Z32:
        case Psm::Z24: return FormatClass::Depth32;
        case Psm::Z16:
        case Psm::Z16S: return FormatClass::Depth16;
        default: return FormatClass::None;
    }
}

bool IsDepthClass(FormatClass c) { return c == FormatClass::Depth32 || c == FormatClass::Depth16; }

// Bits of a stored word a write in `psm` owns: a 24-bit view leaves the top byte to the 32-bit one.
std::uint32_t WriteMaskOf(Psm psm) {
    return psm == Psm::Ct24 || psm == Psm::Z24 ? 0x00FFFFFFu : 0xFFFFFFFFu;
}

PixelRect Union(const PixelRect &a, const PixelRect &b) {
    if (a.Empty()) return b;
    if (b.Empty()) return a;
    return PixelRect{std::min(a.x0, b.x0), std::min(a.y0, b.y0), std::max(a.x1, b.x1), std::max(a.y1, b.y1)};
}

PixelRect Intersect(const PixelRect &a, const PixelRect &b) {
    return PixelRect{std::max(a.x0, b.x0), std::max(a.y0, b.y0), std::min(a.x1, b.x1), std::min(a.y1, b.y1)};
}

bool Contains(const PixelRect &outer, const PixelRect &inner) {
    return !outer.Empty() && outer.x0 <= inner.x0 && outer.y0 <= inner.y0 && outer.x1 >= inner.x1 && outer.y1 >= inner.y1;
}

// A conservative range of GS blocks (modulo 16384) a rectangle of a buffer can touch: whole page rows, plus one page
// when the base is not page aligned; count >= 16384 means "all memory" (the same rule as the old core's spans).
struct BlockSpan {
    std::uint32_t begin = 0;
    std::uint32_t count = 0;
};

BlockSpan SpanOfRect(const BufferView &view, std::int32_t x0, std::int32_t y0, std::int32_t x1, std::int32_t y1) {
    if (x1 < x0 || y1 < y0) return BlockSpan{};
    if (x0 < 0 || y0 < 0 || x1 >= 2048 || y1 >= 2048) return BlockSpan{0, kAllBlocks};
    const memory::PsmInfo &info = memory::psmInfo(view.psm);
    // A small rectangle gets its exact blocks: the conservative page rows below add a whole page when the base is
    // not page aligned, and near the end of memory that page wraps to block 0 (GT4 writes 8 x 8 pixels at block
    // 16328: the spill page reached the display buffer at block 0 and its top rows were patched from memory).
    {
        const std::int32_t bw = info.blockWidth, bh = info.blockHeight;
        const std::int64_t cells = static_cast<std::int64_t>(x1 / bw - x0 / bw + 1) * (y1 / bh - y0 / bh + 1);
        if (cells <= 1024) {
            std::uint32_t first = kAllBlocks, last = 0;
            for (std::int32_t by = y0 / bh; by <= y1 / bh; ++by) {
                for (std::int32_t bx = x0 / bw; bx <= x1 / bw; ++bx) {
                    const std::uint32_t block = memory::pixelSlot(view, static_cast<std::uint32_t>(bx * bw),
                                                                  static_cast<std::uint32_t>(by * bh))
                                                    .byteOffset /
                                                memory::kBlockBytes;
                    first = std::min(first, block);
                    last = std::max(last, block);
                }
            }
            // A range that wraps the end of memory (first near 0, last near the top) stays conservative.
            if (last - first < kAllBlocks / 2) return BlockSpan{first, last - first + 1u};
        }
    }
    const std::uint32_t bufferWidth = std::max<std::uint32_t>(view.bufferWidth, 1u) * 64u;
    const std::uint32_t pagesPerRow = std::max<std::uint32_t>(1u, (bufferWidth + info.pageWidth - 1u) / info.pageWidth);
    const std::uint32_t firstRow = static_cast<std::uint32_t>(y0) / info.pageHeight;
    std::uint32_t rows = static_cast<std::uint32_t>(y1) / info.pageHeight - firstRow + 1u;
    if (static_cast<std::uint32_t>(x1) >= bufferWidth) rows += 1u + static_cast<std::uint32_t>(x1) / bufferWidth;
    const std::uint64_t spill = (view.basePointer % memory::kBlocksPerPage) != 0 ? memory::kBlocksPerPage : 0;
    const std::uint64_t count = static_cast<std::uint64_t>(rows) * pagesPerRow * memory::kBlocksPerPage + spill;
    if (count >= kAllBlocks) return BlockSpan{0, kAllBlocks};
    return BlockSpan{(view.basePointer + firstRow * pagesPerRow * memory::kBlocksPerPage) % kAllBlocks,
                     static_cast<std::uint32_t>(count)};
}

BlockSpan SpanOfRect(const BufferView &view, const PixelRect &r) { return SpanOfRect(view, r.x0, r.y0, r.x1, r.y1); }

bool SpansOverlap(const BlockSpan &a, const BlockSpan &b) {
    if (a.count == 0 || b.count == 0) return false;
    if (a.count >= kAllBlocks || b.count >= kAllBlocks) return true;
    const std::uint32_t ab = (b.begin + kAllBlocks - a.begin) % kAllBlocks;
    const std::uint32_t ba = (a.begin + kAllBlocks - b.begin) % kAllBlocks;
    return ab < a.count || ba < b.count;
}

std::uint64_t FieldOf(std::uint64_t value, unsigned shift, unsigned width) {
    return (value >> shift) & ((1ull << width) - 1ull);
}

// A rectangle the EE wrote into GS memory, and in which format (4.1).
struct Dirty {
    PixelRect rect;
    Psm psm = Psm::Ct32;
};

// 1.2: one target.
struct Target {
    bool alive = false;
    FormatClass cls = FormatClass::None;
    BufferView view{0, 0, Psm::Ct32};  // base, width and the format of its last draw
    ImageId image = kNoImage;
    std::uint32_t width = 0, height = 0;  // native size of its image
    PixelRect valid;                      // content that is defined
    PixelRect drawn;                      // drawn since the last readback
    std::vector<Dirty> dirty;             // newer in GS memory than in the image
    bool fullWordWritten = false;         // a 32-bit (or Z32) draw happened: the top byte is the target's
    std::uint64_t lastDraw = 0;
    std::uint64_t lastDrawVblank = 0;
    std::uint32_t unusedFrames = 0;
    bool usedThisFrame = false;
};

struct RecentUpload {
    BufferView view{0, 0, Psm::Ct32};
    PixelRect rect;
    BlockSpan span;
    std::uint64_t drawNumber = 0;
};

// A texture decoded from GS memory (3.7) in a 1x image of its own, valid while none of its pages were written.
struct TextureEntry {
    writeback::TextureKey key;
    BlockSpan span;
    std::uint64_t sequence = 0;
    ImageId image = kNoImage;
    std::uint64_t bytes = 0;
    std::uint64_t lastUse = 0;
};

constexpr std::uint64_t kTextureCacheBytes = static_cast<std::uint64_t>(768) << 20;  // 1x texels kept on the GPU

std::uint64_t KeyHash(const writeback::TextureKey &k) {
    const std::uint64_t words[] = {k.tbp, k.tbw, k.psm, k.width, k.height, k.ta0, k.ta1, k.aem, k.paletteHash};
    std::uint64_t hash = 1469598103934665603ull;
    for (std::uint64_t w : words) {
        hash ^= w;
        hash *= 1099511628211ull;
    }
    return hash;
}

int RowFieldOfDisplayField(bool csrFieldOdd) { return csrFieldOdd ? 0 : 1; }

}  // namespace

struct Renderer::Impl final : public BatchSink, public SyncSink, public TransferSink, public writeback::TextureCacheView {
    // ---- State ---------------------------------------------------------------------------------------------
    cpuref::LocalMemorySink memory_;
    FrontEnd frontEnd_;
    Gpu gpu_;
    std::uint32_t scale_ = 1;
    float shift_ = 1.0f;
    bool failed_ = false;
    std::string failure_;
    writeback::Counters counters;
    Core2Counters stats;

    display::DisplayRegisters displayRegs_;
    std::array<std::uint32_t, 2> previousDby_{};
    std::array<std::uint8_t, 3> background_{};
    bool oddField_ = false;
    writeback::OutputSettings settings_;
    std::uint64_t vblanks_ = 0;

    std::vector<Target> targets_;
    std::uint64_t drawCounter_ = 0;
    std::deque<RecentUpload> recentUploads_;

    std::array<std::uint64_t, memory::kPagesTotal> pageWrites_{};
    std::uint64_t writeSequence_ = 0;
    std::unordered_map<std::uint64_t, TextureEntry> textures_;
    std::unordered_map<std::uint64_t, std::vector<ImageId>> texturePool_;  // free 1x images by size
    // The GLM (GPU copy of GS memory) holds a page as of glmSequence_[page]; never uploaded pages are stale.
    std::array<std::uint64_t, memory::kPagesTotal> glmSequence_{};
    std::array<bool, memory::kPagesTotal> glmUploaded_{};
    std::uint64_t textureBytes_ = 0;
    std::uint64_t textureUses_ = 0;
    // Set by find() for the batch being translated: the image of a cached texture.
    ImageId textureHit_ = kNoImage;
    // Set by the texture-from-target hook for the batch being translated.
    std::int32_t textureTarget_ = -1;

    // 7.2: the channel-shuffle run in progress.
    bool toneActive_ = false;
    std::uint32_t toneFbmsk_ = 0;

    std::optional<LocalToHostSetup> pendingRead_;

    std::vector<gpu::GpuVertex> vertexScratch_;
    std::vector<Qword> gifScratch_;

    static FrontEndOptions Core2FrontEndOptions() {
        FrontEndOptions options;
        options.vertexTrace = false;
        return options;
    }

    Impl() : frontEnd_(Core2FrontEndOptions(), *this, *this, *this) {
        frontEnd_.SetLocalMemory(memory_.Local().bytes().data());
        // A CLUT load reads GS memory. GT4 draws some of its palettes (the race colour filter's ramp), so a load over
        // a drawn target reduces that target into memory first, like any host read of drawn pixels (4.2).
        frontEnd_.SetBeforeLocalRead([this](const BlockRange &range) {
            const BlockSpan span{range.begin, range.count};
            for (const Target &t : targets_) {
                if (t.alive && !t.drawn.Empty() && SpansOverlap(TargetSpan(t, t.drawn), span)) {
                    stats.clutReadbacks++;
                    Readback(span);
                    return;
                }
            }
        });
    }

    void Fail(const std::string &error) {
        if (failed_) return;
        failed_ = true;
        failure_ = error;
        std::fprintf(stderr, "[core2] failure: %s\n", error.c_str());
    }

    // ---- GS memory page versions (texture cache, 3.7) --------------------------------------------------------
    void MarkWritten(const BlockSpan &span) {
        if (span.count == 0) return;
        ++writeSequence_;
        if (span.count >= kAllBlocks) {
            pageWrites_.fill(writeSequence_);
            return;
        }
        const std::uint32_t firstPage = span.begin / memory::kBlocksPerPage;
        const std::uint32_t lastBlock = span.begin + span.count - 1u;
        for (std::uint32_t page = firstPage; page <= lastBlock / memory::kBlocksPerPage; ++page) {
            pageWrites_[page % memory::kPagesTotal] = writeSequence_;
        }
    }

    bool UnchangedSince(const BlockSpan &span, std::uint64_t sequence) const {
        if (span.count >= kAllBlocks) {
            for (std::uint64_t value : pageWrites_) {
                if (value > sequence) return false;
            }
            return true;
        }
        const std::uint32_t firstPage = span.begin / memory::kBlocksPerPage;
        const std::uint32_t lastBlock = span.begin + span.count - 1u;
        for (std::uint32_t page = firstPage; page <= lastBlock / memory::kBlocksPerPage; ++page) {
            if (pageWrites_[page % memory::kPagesTotal] > sequence) return false;
        }
        return true;
    }

    static std::uint64_t SizeKey(std::uint32_t width, std::uint32_t height) {
        return (static_cast<std::uint64_t>(width) << 32) | height;
    }

    // Images of forgotten textures are reused for new ones of the same size (no allocation per decode). A later
    // decode into one is recorded after every draw that sampled it, so it never changes what those draws see.
    ImageId TextureImage(std::uint32_t width, std::uint32_t height) {
        auto &pool = texturePool_[SizeKey(width, height)];
        if (!pool.empty()) {
            const ImageId image = pool.back();
            pool.pop_back();
            return image;
        }
        std::string error;
        const ImageId image = gpu_.CreateImage(width, height, false, error);
        if (image == kNoImage) Fail(error);
        return image;
    }

    void ForgetTexture(std::unordered_map<std::uint64_t, TextureEntry>::iterator it) {
        texturePool_[SizeKey(it->second.key.width, it->second.key.height)].push_back(it->second.image);
        textureBytes_ -= std::min(textureBytes_, it->second.bytes);
        textures_.erase(it);
    }

    // TextureCacheView: a hit returns an empty vector (the texels are already on the GPU, textureHit_ says where).
    const std::vector<std::uint32_t> *find(const writeback::TextureKey &key, const BufferView &) override {
        static const std::vector<std::uint32_t> kOnGpu;
        auto it = textures_.find(KeyHash(key));
        if (it == textures_.end() || !(it->second.key == key)) return nullptr;
        if (!UnchangedSince(it->second.span, it->second.sequence)) {
            ForgetTexture(it);
            return nullptr;
        }
        it->second.lastUse = ++textureUses_;
        textureHit_ = it->second.image;
        return &kOnGpu;
    }

    // A fresh decode: placed in the atlas when the batch is drawn (PlaceInAtlas), keyed here.
    writeback::TextureKey pendingKey_;
    BufferView pendingView_{0, 0, Psm::Ct32};
    bool pendingStore_ = false;
    void store(const writeback::TextureKey &key, const BufferView &view, const std::vector<std::uint32_t> &) override {
        pendingKey_ = key;
        pendingView_ = view;
        pendingStore_ = true;
    }

    // 3.7: the GLM pages a texture reads are made current (uploaded where GS memory changed since).
    bool MakeGlmCurrent(const BlockSpan &span) {
        std::vector<std::pair<std::uint32_t, std::uint32_t>> ranges;
        static const bool full = std::getenv("GT4_CORE2_GLM_FULL") != nullptr;  // diagnostics A/B
        auto add = [&](std::uint32_t page) {
            if (!full && glmUploaded_[page] && glmSequence_[page] >= pageWrites_[page]) return;
            glmUploaded_[page] = true;
            glmSequence_[page] = writeSequence_;
            const std::uint32_t offset = page * memory::kPageBytes;
            if (!ranges.empty() && ranges.back().first + ranges.back().second == offset) {
                ranges.back().second += memory::kPageBytes;
            } else {
                ranges.push_back({offset, memory::kPageBytes});
            }
        };
        if (span.count >= kAllBlocks) {
            for (std::uint32_t page = 0; page < memory::kPagesTotal; ++page) add(page);
        } else if (span.count != 0) {
            const std::uint32_t firstPage = span.begin / memory::kBlocksPerPage;
            const std::uint32_t lastBlock = span.begin + span.count - 1u;
            for (std::uint32_t page = firstPage; page <= lastBlock / memory::kBlocksPerPage; ++page) {
                add(page % memory::kPagesTotal);
            }
        }
        std::string error;
        if (!gpu_.UploadGlm(memory_.Local().bytes().data(), ranges, error)) {
            Fail(error);
            return false;
        }
        return true;
    }

    // A texture from GS memory (3.7): decoded on the GPU into a 1x image, kept while its pages are unchanged; the
    // least recently used textures go when the cache is over its budget.
    ImageId MemoryTexture(const writeback::BatchTranslation &t) {
        writeback::TextureKey key;
        key.tbp = t.glmBp;
        key.tbw = t.glmBw;
        key.psm = t.glmPsm;
        key.width = t.texWidth;
        key.height = t.texHeight;
        if (t.glmIndexed) key.paletteHash = writeback::HashPaletteWords(t.palette);
        const std::uint64_t hash = KeyHash(key);
        auto found = textures_.find(hash);
        static const bool cacheOn = std::getenv("GT4_CORE2_TEXCACHE") == nullptr ||
                                    std::strcmp(std::getenv("GT4_CORE2_TEXCACHE"), "0") != 0;  // diagnostics A/B
        if (cacheOn && found != textures_.end() && found->second.key == key &&
            UnchangedSince(found->second.span, found->second.sequence)) {
            found->second.lastUse = ++textureUses_;
            stats.atlasHits++;
            return found->second.image;
        }
        if (found != textures_.end()) ForgetTexture(found);
        const auto psm = memory::decodePsm(t.glmPsm);
        if (!psm) return kNoImage;
        const BufferView view{t.glmBp, t.glmBw, *psm};
        const BlockSpan span = SpanOfRect(view, 0, 0, static_cast<std::int32_t>(t.texWidth) - 1,
                                          static_cast<std::int32_t>(t.texHeight) - 1);
        if (!MakeGlmCurrent(span)) return kNoImage;
        const ImageId image = TextureImage(t.texWidth, t.texHeight);
        if (image == kNoImage) return kNoImage;
        std::string error;
        if (!gpu_.DecodeTexture(image, t.glmBp, t.glmBw, t.glmPsm, t.texWidth, t.texHeight,
                                t.glmIndexed ? t.palette.data() : nullptr, error)) {
            Fail(error);
            return kNoImage;
        }
        TextureEntry entry;
        entry.key = key;
        entry.span = span;
        entry.sequence = writeSequence_;
        entry.image = image;
        entry.bytes = static_cast<std::uint64_t>(t.texWidth) * t.texHeight * 4u;
        entry.lastUse = ++textureUses_;
        textureBytes_ += entry.bytes;
        textures_[hash] = entry;
        while (textureBytes_ > kTextureCacheBytes && textures_.size() > 1) {
            auto oldest = textures_.end();
            for (auto it = textures_.begin(); it != textures_.end(); ++it) {
                if (it->first == hash) continue;
                if (oldest == textures_.end() || it->second.lastUse < oldest->second.lastUse) oldest = it;
            }
            if (oldest == textures_.end()) break;
            ForgetTexture(oldest);
        }
        return image;
    }

    // Images in the pool past the budget are released (the pool only saves allocations).
    void TrimTexturePool() {
        std::uint64_t pooled = 0;
        for (auto &[size, images] : texturePool_) {
            const std::uint64_t each = (size >> 32) * (size & 0xFFFFFFFFu) * 4u;
            while (!images.empty() && (pooled + each > kTextureCacheBytes / 4)) {
                gpu_.DestroyImage(images.back());
                images.pop_back();
            }
            pooled += each * images.size();
        }
    }

    // ---- Targets ---------------------------------------------------------------------------------------------
    BlockSpan TargetSpan(const Target &t, const PixelRect &r) const { return SpanOfRect(t.view, r); }

    void DestroyTarget(Target &t) {
        if (!t.alive) return;
        gpu_.DestroyImage(t.image);
        t = Target{};
    }

    // Makes the image of `t` hold every rectangle the EE wrote into it (4.1, applied lazily at the next use).
    bool ApplyDirty(Target &t) {
        if (t.dirty.empty()) return true;
        std::vector<Dirty> pending;
        pending.swap(t.dirty);
        for (const Dirty &d : pending) {
            const PixelRect r = Intersect(d.rect, PixelRect{0, 0, static_cast<std::int32_t>(t.width) - 1,
                                                             static_cast<std::int32_t>(t.height) - 1});
            if (r.Empty()) continue;
            const std::uint32_t w = static_cast<std::uint32_t>(r.x1 - r.x0 + 1);
            const std::uint32_t h = static_cast<std::uint32_t>(r.y1 - r.y0 + 1);
            std::vector<std::uint32_t> words(static_cast<std::size_t>(w) * h);
            const BufferView source{t.view.basePointer, t.view.bufferWidth, d.psm};
            memory_.Local().readRect(source, static_cast<std::uint32_t>(r.x0), static_cast<std::uint32_t>(r.y0), w, h,
                                     words.data());
            std::string error;
            if (!gpu_.Patch(t.image, r.x0, r.y0, r.x1, r.y1, words, WriteMaskOf(d.psm), error)) {
                Fail(error);
                return false;
            }
            t.valid = Union(t.valid, r);
            stats.dirtyPatches++;
            stats.dirtyPatchPixels += static_cast<std::uint64_t>(w) * h;
        }
        return true;
    }

    bool Grow(Target &t, std::uint32_t width, std::uint32_t height) {
        const std::uint32_t maxSide = gpu_.MaxNativeSide();
        width = std::min(width, maxSide);
        height = std::min(height, maxSide);
        if (width <= t.width && height <= t.height) return true;
        // Rows grow in steps of 64 so a target that is drawn a few rows further each time grows rarely.
        const std::uint32_t newWidth = std::max(t.width, width);
        const std::uint32_t newHeight = std::min(maxSide, std::max(t.height, (height + 63u) / 64u * 64u));
        std::string error;
        if (!gpu_.GrowImage(t.image, newWidth, newHeight, error)) {
            Fail(error);
            return false;
        }
        t.width = newWidth;
        t.height = newHeight;
        stats.targetsGrown++;
        return true;
    }

    // 2.4b: the areas of a new target that recent EE uploads cover become dirty rectangles.
    void SeedFromRecentUploads(Target &t) {
        const BlockSpan whole = TargetSpan(t, PixelRect{0, 0, static_cast<std::int32_t>(t.width) - 1,
                                                        static_cast<std::int32_t>(t.height) - 1});
        for (const RecentUpload &u : recentUploads_) {
            if (drawCounter_ - u.drawNumber > kRecentUploadDraws) continue;
            if (!SpansOverlap(u.span, whole)) continue;
            AddUploadDirty(t, u.view, u.rect, u.span);
        }
    }

    Target *CreateTarget(const BufferView &view, FormatClass cls, const PixelRect &rect) {
        const std::uint32_t maxSide = gpu_.MaxNativeSide();
        const std::uint32_t width = std::min(std::max<std::uint32_t>(view.bufferWidth, 1u) * 64u, maxSide);
        const std::uint32_t height =
            std::min(maxSide, (static_cast<std::uint32_t>(std::max(rect.y1, 0)) + 1u + 63u) / 64u * 64u);
        std::string error;
        const ImageId image = gpu_.CreateImage(width, height, true, error);
        if (image == kNoImage) {
            Fail(error);
            return nullptr;
        }
        Target fresh;
        fresh.alive = true;
        fresh.cls = cls;
        fresh.view = view;
        fresh.image = image;
        fresh.width = width;
        fresh.height = height;
        Target *slot = nullptr;
        for (Target &t : targets_) {
            if (!t.alive) {
                slot = &t;
                break;
            }
        }
        if (!slot) {
            targets_.push_back(Target{});
            slot = &targets_.back();
        }
        *slot = fresh;
        stats.targetsCreated++;
        SeedFromRecentUploads(*slot);
        return slot;
    }

    // 2.1, 2.2, 2.4: the target a draw writes (`view` from FRAME or ZBUF).
    Target *DrawTarget(const BufferView &view, const PixelRect &rect) {
        const FormatClass cls = ClassOf(view.psm);
        if (cls == FormatClass::None) return nullptr;
        // 2.1: the target of this base, class and width. One of another width is discarded when it is old (not drawn
        // in this frame or the last one); a recent one stays beside the new target (GT4 draws its frame memory
        // as 640 and as 128 pixels wide in the same frame).
        Target *found = nullptr;
        for (Target &t : targets_) {
            if (!t.alive || t.cls != cls || t.view.basePointer != view.basePointer) continue;
            if (t.view.bufferWidth != view.bufferWidth) {
                if (vblanks_ > t.lastDrawVblank + 1) {
                    DestroyTarget(t);
                    stats.targetsReplaced++;
                }
                continue;
            }
            if (!found || t.lastDraw > found->lastDraw) found = &t;
        }
        if (!found) found = CreateTarget(view, cls, rect);
        if (!found) return nullptr;
        if (!Grow(*found, static_cast<std::uint32_t>(std::max(rect.x1, 0)) + 1u,
                  static_cast<std::uint32_t>(std::max(rect.y1, 0)) + 1u)) {
            return nullptr;
        }
        found->view.psm = view.psm;
        return found;
    }

    // 3.2: a colour target whose base equals the texture's and whose format class and width match.
    Target *TextureTarget(const BufferView &view, const PixelRect &footprint) {
        const FormatClass cls = ClassOf(view.psm);
        if (cls != FormatClass::Colour32 && cls != FormatClass::Colour16) return nullptr;
        Target *found = nullptr;
        for (Target &t : targets_) {
            if (!t.alive || t.cls != cls || t.view.basePointer != view.basePointer ||
                t.view.bufferWidth != view.bufferWidth) {
                continue;
            }
            if (!found || t.lastDraw > found->lastDraw) found = &t;
        }
        if (!found) return nullptr;
        if (Intersect(Union(found->valid, found->drawn), footprint).Empty() && found->dirty.empty()) return nullptr;
        return found;
    }

    // ---- Coherence (4) ---------------------------------------------------------------------------------------
    // 4.1: an EE write into `rect` of `view`: a dirty rectangle in every target it lands on.
    void AddUploadDirty(Target &t, const BufferView &view, const PixelRect &rect, const BlockSpan &span) {
        const FormatClass cls = ClassOf(view.psm);
        if (view.basePointer == t.view.basePointer && view.bufferWidth == t.view.bufferWidth && cls == t.cls) {
            t.dirty.push_back(Dirty{rect, view.psm});
            return;
        }
        // Another base, width or format: the target's page rows the write touches, read in the target's format.
        const memory::PsmInfo &info = memory::psmInfo(t.view.psm);
        const std::uint32_t pagesPerRow = std::max<std::uint32_t>(
            1u, (std::max<std::uint32_t>(t.view.bufferWidth, 1u) * 64u + info.pageWidth - 1u) / info.pageWidth);
        const std::uint32_t rowBlocks = pagesPerRow * memory::kBlocksPerPage;
        PixelRect rows;
        for (std::uint32_t row = 0; row * info.pageHeight < t.height; ++row) {
            const BlockSpan rowSpan{(t.view.basePointer + row * rowBlocks) % kAllBlocks, rowBlocks};
            if (!SpansOverlap(rowSpan, span)) continue;
            rows = Union(rows, PixelRect{0, static_cast<std::int32_t>(row * info.pageHeight),
                                         static_cast<std::int32_t>(t.width) - 1,
                                         static_cast<std::int32_t>((row + 1) * info.pageHeight) - 1});
        }
        if (!rows.Empty()) t.dirty.push_back(Dirty{rows, t.view.psm});
    }

    void OnMemoryWritten(const BufferView &view, const PixelRect &rect, bool remember) {
        const BlockSpan span = SpanOfRect(view, rect);
        MarkWritten(span);
        for (Target &t : targets_) {
            if (!t.alive) continue;
            const BlockSpan whole = TargetSpan(t, PixelRect{0, 0, static_cast<std::int32_t>(t.width) - 1,
                                                            static_cast<std::int32_t>(t.height) - 1});
            if (!SpansOverlap(whole, span)) continue;
            AddUploadDirty(t, view, rect, span);
            static const long long traceAt = std::getenv("GT4_CORE2_BATCH_TRACE_AT")
                                                 ? std::atoll(std::getenv("GT4_CORE2_BATCH_TRACE_AT"))
                                                 : -1;  // diagnostics only
            if (traceAt >= 0 && static_cast<long long>(vblanks_) == traceAt && !t.dirty.empty()) {
                const PixelRect &d = t.dirty.back().rect;
                std::fprintf(stderr,
                             "[core2] dirty target=%u/%u/%02x from write %u/%u/%02x rect=%d,%d,%d,%d -> %d,%d,%d,%d\n",
                             t.view.basePointer, t.view.bufferWidth, static_cast<unsigned>(t.view.psm),
                             view.basePointer, view.bufferWidth, static_cast<unsigned>(view.psm), rect.x0, rect.y0,
                             rect.x1, rect.y1, d.x0, d.y0, d.x1, d.y1);
            }
            // 4.1: a target the write covers entirely is deleted, drawn or not: memory now holds newer content for
            // all of it (GT4 streams textures over the memory of its small effect targets; a kept target would later
            // write its old pixels back over the new texture).
            if (!t.dirty.empty() && t.dirty.back().psm == t.view.psm && Contains(t.dirty.back().rect, t.valid) &&
                !t.valid.Empty()) {
                DestroyTarget(t);
                stats.targetsReplaced++;
            }
        }
        if (remember) {
            recentUploads_.push_back(RecentUpload{view, rect, span, drawCounter_});
            while (!recentUploads_.empty() && drawCounter_ - recentUploads_.front().drawNumber > kRecentUploadDraws) {
                recentUploads_.pop_front();
            }
            while (recentUploads_.size() > 4096) recentUploads_.pop_front();
        }
    }

    // 4.2: every target over `span` writes its drawn area (inside `limit` when it is given in the same buffer) back
    // to GS memory, oldest draw first so the newest content wins.
    bool Readback(const BlockSpan &span, const BufferView *limitView = nullptr, const PixelRect *limit = nullptr) {
        std::vector<Target *> order;
        for (Target &t : targets_) {
            if (!t.alive || t.drawn.Empty()) continue;
            if (!SpansOverlap(TargetSpan(t, t.drawn), span)) continue;
            order.push_back(&t);
        }
        std::sort(order.begin(), order.end(), [](const Target *a, const Target *b) { return a->lastDraw < b->lastDraw; });
        for (Target *t : order) {
            PixelRect area = t->drawn;
            if (limitView && limit && limitView->basePointer == t->view.basePointer &&
                limitView->bufferWidth == t->view.bufferWidth && ClassOf(limitView->psm) == t->cls) {
                area = Intersect(area, *limit);
            }
            area = Intersect(area, PixelRect{0, 0, static_cast<std::int32_t>(t->width) - 1,
                                             static_cast<std::int32_t>(t->height) - 1});
            if (area.Empty()) continue;
            if (!ApplyDirty(*t)) return false;
            std::vector<std::uint32_t> words;
            std::string error;
            if (!gpu_.Reduce(t->image, area.x0, area.y0, area.x1, area.y1, words, error)) {
                Fail(error);
                return false;
            }
            // A 32/24 target that was only ever drawn as 24-bit leaves the top byte to memory.
            Psm psm = t->view.psm;
            if (!t->fullWordWritten) {
                if (t->cls == FormatClass::Colour32) psm = Psm::Ct24;
                if (t->cls == FormatClass::Depth32) psm = Psm::Z24;
            } else {
                if (t->cls == FormatClass::Colour32) psm = Psm::Ct32;
                if (t->cls == FormatClass::Depth32) psm = Psm::Z32;
            }
            const BufferView out{t->view.basePointer, t->view.bufferWidth, psm};
            static const bool trace = std::getenv("GT4_CORE2_READBACK_TRACE") != nullptr;  // diagnostics only
            if (trace) {
                std::fprintf(stderr, "[core2] readback vblank=%llu target bp=%u bw=%u psm=%u size=%ux%u area=%d,%d,%d,%d "
                             "span=%u+%u\n", static_cast<unsigned long long>(vblanks_), t->view.basePointer,
                             t->view.bufferWidth, static_cast<unsigned>(psm), t->width, t->height, area.x0, area.y0,
                             area.x1, area.y1, span.begin, span.count);
            }
            const std::uint32_t w = static_cast<std::uint32_t>(area.x1 - area.x0 + 1);
            const std::uint32_t h = static_cast<std::uint32_t>(area.y1 - area.y0 + 1);
            memory_.Local().writeRect(out, static_cast<std::uint32_t>(area.x0), static_cast<std::uint32_t>(area.y0), w, h,
                                      words.data());
            MarkWritten(SpanOfRect(out, area));
            if (Contains(area, t->drawn)) t->drawn = PixelRect{};
            stats.readbacks++;
            stats.readbackPixels += static_cast<std::uint64_t>(w) * h;
        }
        return true;
    }

    bool ReadbackAll() { return Readback(BlockSpan{0, kAllBlocks}); }

    // ---- TransferSink ----------------------------------------------------------------------------------------
    void InvalidateDestination(const UploadCommit &c) override { memory_.InvalidateDestination(c); }

    void CommitUpload(const UploadCommit &c, const std::uint8_t *bytes, std::size_t byteCount) override {
        memory_.CommitUpload(c, bytes, byteCount);
        const auto psm = memory::decodePsm(c.dpsm);
        if (!psm || c.rrw == 0 || c.rrh == 0) {
            MarkWritten(BlockSpan{0, kAllBlocks});
            return;
        }
        const BufferView view{c.dbp, c.dbw, *psm};
        const PixelRect rect{static_cast<std::int32_t>(c.dsax), static_cast<std::int32_t>(c.dsay),
                             static_cast<std::int32_t>(c.dsax + c.rrw - 1u), static_cast<std::int32_t>(c.dsay + c.rrh - 1u)};
        if (rect.x1 >= 2048 || rect.y1 >= 2048) {
            MarkWritten(BlockSpan{0, kAllBlocks});
            for (Target &t : targets_) {
                if (t.alive) t.dirty.push_back(Dirty{PixelRect{0, 0, static_cast<std::int32_t>(t.width) - 1,
                                                              static_cast<std::int32_t>(t.height) - 1},
                                                    t.view.psm});
            }
            return;
        }
        OnMemoryWritten(view, rect, true);
    }

    void LocalToLocalMove(const LocalMoveCommit &move) override {
        const auto spsm = memory::decodePsm(move.spsm);
        const auto dpsm = memory::decodePsm(move.dpsm);
        if (spsm && move.rrw != 0 && move.rrh != 0) {
            const BufferView source{move.sbp, move.sbw, *spsm};
            const PixelRect rect{static_cast<std::int32_t>(move.ssax), static_cast<std::int32_t>(move.ssay),
                                 static_cast<std::int32_t>(move.ssax + move.rrw - 1u),
                                 static_cast<std::int32_t>(move.ssay + move.rrh - 1u)};
            if (!Readback(SpanOfRect(source, rect), &source, &rect)) return;
        } else if (!ReadbackAll()) {
            return;
        }
        memory_.LocalToLocalMove(move);
        if (dpsm && move.rrw != 0 && move.rrh != 0) {
            const BufferView destination{move.dbp, move.dbw, *dpsm};
            const PixelRect rect{static_cast<std::int32_t>(move.dsax), static_cast<std::int32_t>(move.dsay),
                                 static_cast<std::int32_t>(move.dsax + move.rrw - 1u),
                                 static_cast<std::int32_t>(move.dsay + move.rrh - 1u)};
            OnMemoryWritten(destination, rect, true);
        } else {
            MarkWritten(BlockSpan{0, kAllBlocks});
        }
    }

    void BeginLocalToHost(const LocalToHostSetup &setup) override {
        pendingRead_ = setup;
        memory_.BeginLocalToHost(setup);
    }

    void CancelLocalToHost() override {
        pendingRead_.reset();
        memory_.CancelLocalToHost();
    }

    bool OnSync(SyncKind, std::uint32_t, std::uint32_t) override { return false; }

    // ---- 7.2 GT4's channel-shuffle hook ------------------------------------------------------------------------
    // A possible channel-shuffle draw (addendum: detection). Only the first draw of a run needs its texture base to
    // resolve to a colour target; later draws (GT4's copy-back strips read the temporary page) only need the shape.
    bool PossibleShuffle(const Batch &batch) const {
        const BatchState &state = batch.state;
        const std::uint64_t prim = state.effectivePrim;
        if (((prim >> primbit::kTme) & 1u) == 0) return false;
        if (batch.cls != PrimClass::Sprite) return false;
        const std::uint64_t tex0 = state.ctx.tex0;
        if (tex0::Psm(tex0) != 0x13u) return false;  // T8
        if (batch.vertices.size() < 2) return false;
        std::int32_t minX = INT32_MAX, minY = INT32_MAX, maxX = INT32_MIN, maxY = INT32_MIN;
        for (const Vertex &v : batch.vertices) {
            minX = std::min(minX, v.x4);
            minY = std::min(minY, v.y4);
            maxX = std::max(maxX, v.x4);
            maxY = std::max(maxY, v.y4);
        }
        const std::int32_t boxW = (maxX - minX) >> 4;
        const std::int32_t boxH = (maxY - minY) >> 4;
        if (batch.vertices.size() <= 2 && boxW <= 8 && boxH <= 8) return false;
        const std::int32_t firstW = std::abs(batch.vertices[1].x4 - batch.vertices[0].x4) >> 4;
        const std::int32_t firstH = std::abs(batch.vertices[1].y4 - batch.vertices[0].y4) >> 4;
        const std::uint64_t clamp = state.ctx.clamp;
        const bool regionMode = clampreg::Wms(clamp) >= 2 || clampreg::Wmt(clamp) >= 2;
        const bool shape = firstW == 8 || firstH == 2 || regionMode;
        if (!shape || boxW > 64) return false;
        // The strip case: a texture twice as wide as the frame, page aligned.
        if (boxH > 64 && tex0::Tbw(tex0) != 2u * frame::Fbw(state.ctx.frame)) return false;
        return true;
    }

    Target *ShuffleSource(const Batch &batch) {
        const std::uint32_t base = tex0::Tbp0(batch.state.ctx.tex0);
        Target *found = nullptr;
        for (Target &t : targets_) {
            if (!t.alive || t.cls != FormatClass::Colour32 || t.view.basePointer != base) continue;
            if (!found || t.lastDraw > found->lastDraw) found = &t;
        }
        return found;
    }

    // True when the batch belongs to a shuffle run and was replaced (skipped).
    bool ToneHook(const Batch &batch) {
        if (!PossibleShuffle(batch)) {
            toneActive_ = false;
            return false;
        }
        const std::uint32_t fbmsk = frame::Fbmsk(batch.state.ctx.frame);
        if (toneActive_ && fbmsk == toneFbmsk_) {
            stats.toneDrawsSkipped++;
            return true;
        }
        Target *target = ShuffleSource(batch);
        if (!target) {
            toneActive_ = false;
            return false;
        }
        if (fbmsk == 0x00FFFFFFu) {
            stats.toneAlphaOnlyUnsupported++;
            toneActive_ = false;
            return false;
        }
        // A new run: the palette as the first draw's CLUT load left it (CPSM, CSA, TEXA of this draw).
        const std::uint64_t tex0 = batch.state.ctx.tex0;
        const clut::Palette palette =
            frontEnd_.Clut().Decode(tex0::Psm(tex0), tex0::Cpsm(tex0), tex0::Csa(tex0), batch.state.env.texa);
        if (!ApplyDirty(*target)) return true;
        const PixelRect extent = Union(target->valid, target->drawn);
        if (!extent.Empty()) {
            std::string error;
            if (!gpu_.Tone(target->image, static_cast<std::uint32_t>(extent.x1 + 1),
                           static_cast<std::uint32_t>(extent.y1 + 1), palette.entries, error)) {
                Fail(error);
                return true;
            }
            target->drawn = Union(target->drawn, PixelRect{0, 0, extent.x1, extent.y1});
            target->lastDraw = ++drawCounter_;
            target->usedThisFrame = true;
        }
        static const bool trace = std::getenv("GT4_CORE2_TONE_TRACE") != nullptr;  // diagnostics only
        if (trace) {
            std::fprintf(stderr,
                         "[core2] tone run vblank=%llu fbmsk=%08x frame=%u tex=%u target=%u extent=%d,%d cbp=%u "
                         "cpsm=%u csa=%u palette 0:%08x 64:%08x 128:%08x 192:%08x 255:%08x\n",
                         static_cast<unsigned long long>(vblanks_), fbmsk,
                         frame::Fbp(batch.state.ctx.frame) * 32u, tex0::Tbp0(tex0), target->view.basePointer,
                         extent.x1, extent.y1, static_cast<unsigned>(tex0::Cbp(tex0)),
                         static_cast<unsigned>(tex0::Cpsm(tex0)), static_cast<unsigned>(tex0::Csa(tex0)),
                         palette.entries[0], palette.entries[64], palette.entries[128], palette.entries[192],
                         palette.entries[255]);
        }
        toneActive_ = true;
        toneFbmsk_ = fbmsk;
        stats.toneRuns++;
        stats.toneDrawsSkipped++;
        counters.toneRuns++;
        return true;
    }

    // ---- Batches ---------------------------------------------------------------------------------------------
    void OnBatch(Batch &&batch) override {
        if (failed_) return;
        counters.batches++;
        if (ToneHook(batch)) return;

        writeback::BatchTranslation t;
        textureHit_ = kNoImage;
        textureTarget_ = -1;
        pendingStore_ = false;
        const writeback::TextureFromTargetHook fromTarget = [this](const BufferView &view, std::uint32_t,
                                                                   std::uint32_t, const PixelRect &footprint) {
            Target *target = TextureTarget(view, footprint);
            if (!target) return -1;
            textureTarget_ = static_cast<std::int32_t>(target - targets_.data());
            return textureTarget_;
        };
        // 3.7: every texture that is not a target hit is decoded from GS memory on the GPU (the GLM). Interim 3.5: an
        // indexed texture over a drawn target reads memory, so that target's drawn area is reduced into memory first
        // (the reference reduces it on the GPU at 1x; the result is the same).
        const writeback::TextureFromGlmHook fromGlm = [this](const BufferView &view, std::uint32_t width,
                                                             std::uint32_t height, bool indexed, bool &gpuClut) {
            gpuClut = false;
            static const bool readAll = std::getenv("GT4_CORE2_TEXTURE_READBACK") == nullptr ||
                                        std::strcmp(std::getenv("GT4_CORE2_TEXTURE_READBACK"), "0") != 0;
            static const bool indexedReadback = std::getenv("GT4_CORE2_INDEXED_READBACK") == nullptr ||
                                                std::strcmp(std::getenv("GT4_CORE2_INDEXED_READBACK"), "0") != 0;
            if ((indexed && indexedReadback) || (!indexed && readAll)) {
                const PixelRect rect{0, 0, static_cast<std::int32_t>(width) - 1,
                                     static_cast<std::int32_t>(height) - 1};
                const BlockSpan span = SpanOfRect(view, rect);
                for (const Target &target : targets_) {
                    if (target.alive && !target.drawn.Empty() && SpansOverlap(TargetSpan(target, target.drawn), span)) {
                        static const bool trace = std::getenv("GT4_CORE2_READBACK_TRACE") != nullptr;
                        if (trace) {
                            std::fprintf(stderr, "[core2] texture over target vblank=%llu tex bp=%u bw=%u psm=%u %ux%u\n",
                                         static_cast<unsigned long long>(vblanks_), view.basePointer, view.bufferWidth,
                                         static_cast<unsigned>(view.psm), width, height);
                        }
                        if (indexed) {
                            stats.indexedFromTargetReadbacks++;
                        } else {
                            stats.directFromTargetReadbacks++;
                        }
                        Readback(span);
                        break;
                    }
                }
            }
            return true;
        };
        writeback::TranslateBatchHooked(batch, memory_.Local(), frontEnd_.Clut(), t, counters, {}, this, fromTarget,
                                        fromGlm);
        if (t.skippedTexture) {
            counters.batchesSkippedTexture++;
            return;
        }
        if (t.draws.empty()) {
            counters.batchesEmpty++;
            return;
        }
        if (failed_) return;

        Target *frameTarget = DrawTarget(t.frameView, t.rect);
        if (!frameTarget) {
            counters.batchesEmpty++;
            return;
        }
        const std::size_t frameIndex = static_cast<std::size_t>(frameTarget - targets_.data());
        const bool depthUsed = [&] {
            const auto &st = t.state;
            const bool testReads = st.fields[gpu::gpufield::Ztest] != 0 && st.fields[gpu::gpufield::Ztst] != 1u;
            return testReads || st.fields[gpu::gpufield::Zmsk] == 0;
        }();
        Target *depthTarget = depthUsed ? DrawTarget(t.zView, t.rect) : nullptr;
        frameTarget = &targets_[frameIndex];  // DrawTarget may have grown the table
        if (depthUsed && !depthTarget) return;
        if (!ApplyDirty(*frameTarget) || (depthTarget && !ApplyDirty(*depthTarget))) return;
        Target *textureTarget = textureTarget_ >= 0 ? &targets_[static_cast<std::size_t>(textureTarget_)] : nullptr;
        if (textureTarget && textureTarget->alive && !ApplyDirty(*textureTarget)) return;

        DrawBatch draw;
        draw.frame = frameTarget->image;
        draw.depth = depthTarget ? depthTarget->image : kNoImage;
        draw.texture = textureTarget && textureTarget->alive ? textureTarget->image : kNoImage;
        draw.state = t.state;
        draw.vertices = &t.vertices;
        draw.draws = &t.draws;
        const std::uint32_t zFormat = t.state.fields[gpu::gpufield::Zformat];
        draw.zMax = zFormat == 1 ? 0xFFFFFFu : zFormat == 2 ? 0xFFFFu : 0xFFFFFFFFu;
        if (t.hasTexture && draw.texture == kNoImage) {
            if (!t.glmTexture) return;  // every memory texture is decoded on the GPU (the hook above)
            draw.memoryTexture = MemoryTexture(t);
            if (draw.memoryTexture == kNoImage) return;
            stats.texturesFromMemory++;
        } else if (draw.texture != kNoImage) {
            stats.texturesFromTargets++;
            textureTarget->usedThisFrame = true;
        }
        static const long long traceAt = std::getenv("GT4_CORE2_BATCH_TRACE_AT")
                                             ? std::atoll(std::getenv("GT4_CORE2_BATCH_TRACE_AT"))
                                             : -1;  // diagnostics only
        if (traceAt >= 0 && static_cast<long long>(vblanks_) == traceAt) {
            const std::uint64_t tex0 = batch.state.ctx.tex0;
            std::fprintf(stderr,
                         "[core2] batch frame=%u/%u/%02x z=%u tex=%u/%u/%02x from=%s rect=%d,%d,%d,%d prims=%zu "
                         "fbmsk=%08x\n",
                         t.frameView.basePointer, t.frameView.bufferWidth, static_cast<unsigned>(t.frameView.psm),
                         depthTarget ? t.zView.basePointer : 0u, t.hasTexture ? tex0::Tbp0(tex0) : 0u,
                         t.hasTexture ? tex0::Tbw(tex0) : 0u, t.hasTexture ? tex0::Psm(tex0) : 0u,
                         !t.hasTexture ? "-" : draw.texture != kNoImage ? "target" : "memory", t.rect.x0, t.rect.y0,
                         t.rect.x1, t.rect.y1, t.draws.size(), t.state.fields[gpu::gpufield::Fbmsk]);
        }
        std::string error;
        if (!gpu_.Draw(draw, error)) {
            Fail(error);
            return;
        }
        counters.gpuDraws += t.draws.size();
        if (t.blended) counters.batchesBlended++;
        const std::uint64_t number = ++drawCounter_;
        frameTarget->valid = Union(frameTarget->valid, t.rect);
        frameTarget->drawn = Union(frameTarget->drawn, t.rect);
        frameTarget->lastDraw = number;
        frameTarget->lastDrawVblank = vblanks_;
        frameTarget->usedThisFrame = true;
        if (t.frameView.psm == Psm::Ct32 && t.state.fields[gpu::gpufield::Fbmsk] != 0xFFFFFFFFu) {
            frameTarget->fullWordWritten = true;
        }
        if (depthTarget) {
            depthTarget->usedThisFrame = true;
            if (t.state.fields[gpu::gpufield::Zmsk] == 0) {
                depthTarget->valid = Union(depthTarget->valid, t.rect);
                depthTarget->drawn = Union(depthTarget->drawn, t.rect);
                depthTarget->lastDraw = number;
                depthTarget->lastDrawVblank = vblanks_;
                if (t.zView.psm == Psm::Z32) depthTarget->fullWordWritten = true;
            }
        }
    }

    // ---- Display (5) -----------------------------------------------------------------------------------------
    // The image a circuit shows: a colour target at its base (5.1), else a 1x image of GS memory (5.2).
    ImageId memoryImage_ = kNoImage;
    std::uint32_t memoryImageWidth_ = 0, memoryImageHeight_ = 0;

    bool CircuitSource(const display::CircuitRect &circuit, ImageId &image, std::uint32_t &sourceScale,
                       std::uint32_t &readX, std::uint32_t &readY) {
        const auto psm = memory::decodePsm(circuit.psm);
        if (!psm) return false;
        const BufferView view{memory::blockOfPage(circuit.fbp), circuit.fbw, *psm};
        const FormatClass cls = ClassOf(*psm);
        Target *found = nullptr;
        for (Target &t : targets_) {
            if (!t.alive || IsDepthClass(t.cls) || t.view.basePointer != view.basePointer) continue;
            if (t.cls != cls) continue;
            if (!found || t.lastDraw > found->lastDraw) found = &t;
        }
        const std::int32_t dbx = circuit.dbx, dby = circuit.dby;
        if (found && dbx >= 0 && dby >= 0) {
            if (!Grow(*found, static_cast<std::uint32_t>(dbx) + circuit.read_width,
                      static_cast<std::uint32_t>(dby) + circuit.read_height) ||
                !ApplyDirty(*found)) {
                return false;
            }
            found->usedThisFrame = true;
            image = found->image;
            sourceScale = scale_;
            readX = static_cast<std::uint32_t>(dbx);
            readY = static_cast<std::uint32_t>(dby);
            stats.displayFromTargets++;
            return true;
        }
        // 5.2: no target holds the buffer (FMVs, loading screens the EE uploaded): GS memory at 1x.
        const std::uint32_t w = circuit.read_width, h = circuit.read_height;
        if (w == 0 || h == 0 || w > 2048 || h > 2048) return false;
        std::string error;
        if (memoryImage_ == kNoImage || memoryImageWidth_ < w || memoryImageHeight_ < h) {
            if (memoryImage_ != kNoImage) gpu_.DestroyImage(memoryImage_);
            memoryImageWidth_ = std::max(memoryImageWidth_, w);
            memoryImageHeight_ = std::max(memoryImageHeight_, h);
            memoryImage_ = gpu_.CreateImage(memoryImageWidth_, memoryImageHeight_, false, error);
            if (memoryImage_ == kNoImage) {
                Fail(error);
                return false;
            }
        }
        std::vector<std::uint32_t> words(static_cast<std::size_t>(w) * h);
        for (std::uint32_t y = 0; y < h; ++y) {
            for (std::uint32_t x = 0; x < w; ++x) {
                const std::uint32_t sx = static_cast<std::uint32_t>(dbx + static_cast<std::int32_t>(x)) & 2047u;
                const std::uint32_t sy = static_cast<std::uint32_t>(dby + static_cast<std::int32_t>(y)) & 2047u;
                words[static_cast<std::size_t>(y) * w + x] = memory_.Local().readPixel(view, sx, sy);
            }
        }
        if (!gpu_.Patch(memoryImage_, 0, 0, static_cast<std::int32_t>(w) - 1, static_cast<std::int32_t>(h) - 1, words,
                        0xFFFFFFFFu, error)) {
            Fail(error);
            return false;
        }
        image = memoryImage_;
        sourceScale = 1;
        readX = 0;
        readY = 0;
        stats.displayFromMemory++;
        return true;
    }

    bool ComposeGpu(writeback::GpuFrame &out) {
        out = writeback::GpuFrame{};
        if (failed_ || !gpu_.Ready()) return false;
        frontEnd_.FlushBatch(FlushReason::VBlank);
        const display::DisplayLayout layout = display::compute_layout(displayRegs_, previousDby_);
        if (!layout.valid) {
            counters.blankFrames++;
            return false;
        }
        std::array<bool, 2> available{};
        for (std::size_t i = 0; i < 2; ++i) {
            const display::CircuitRect &c = layout.circuits[i];
            available[i] = c.enabled && c.read_width != 0 && c.read_height != 0;
        }
        const display::PmodeFields pmode = display::decode_pmode(displayRegs_.pmode);
        const display::MergePlan plan =
            display::plan_merge(pmode, layout.circuits[0], layout.circuits[1], available[0], available[1]);
        if (plan.outcome == display::MergeOutcome::KeepPrevious) return false;
        // Step 1 (CORE2-DESIGN): no deinterlace yet; the field offset of 13.3 step 3 is applied at scale.
        const display::OutputSize size = display::merge_output_size(layout, true, 1);
        if (size.width == 0 || size.height == 0) return false;
        std::uint32_t words[28] = {};
        const std::uint32_t outWidth = size.width * scale_;
        const std::uint32_t outHeight = size.height * scale_;
        words[0] = outWidth;
        words[1] = outHeight;
        words[2] = scale_;
        words[3] = std::uint32_t{background_[0]} | (std::uint32_t{background_[1]} << 8) |
                   (std::uint32_t{background_[2]} << 16) | (std::min<std::uint32_t>(pmode.alp, 255u) << 24);
        words[4] = std::min<std::uint32_t>(pmode.alp, 255u);
        words[5] = pmode.mmod ? 1u : 0u;
        ImageId sources[2] = {kNoImage, kNoImage};
        const bool use[2] = {plan.outcome == display::MergeOutcome::Composite && plan.use_rc1,
                             plan.outcome == display::MergeOutcome::Composite && plan.use_rc2};
        const int fieldPhase = RowFieldOfDisplayField(oddField_) ^ (settings_.bottomFieldFirst ? 1 : 0);
        for (std::size_t i = 0; i < 2; ++i) {
            if (!use[i]) continue;
            const display::CircuitRect &c = layout.circuits[i];
            std::uint32_t sourceScale = 1, readX = 0, readY = 0;
            if (!CircuitSource(c, sources[i], sourceScale, readX, readY)) {
                sources[i] = kNoImage;
                continue;
            }
            std::uint32_t *w = &words[6 + 10 * i];
            const std::int32_t offsetRows =
                layout.really_interlaced
                    ? display::interlace_offset_rows(true, layout.ffmd, display::DeinterlaceMode::Off, 1, fieldPhase)
                    : 0;
            w[0] = 1u;
            w[1] = c.psm == 0x01u ? 1u : (c.psm == 0x02u || c.psm == 0x0Au) ? 2u : 0u;
            w[2] = sourceScale;
            w[3] = readX;
            w[4] = readY;
            w[5] = c.read_width;
            w[6] = c.read_height;
            w[7] = static_cast<std::uint32_t>(c.screen_x);
            w[8] = static_cast<std::uint32_t>(static_cast<std::int32_t>(c.screen_y) + offsetRows);
            w[9] = c.display_width;
            words[26 + i] = c.display_height;
        }
        std::string error;
        if (!gpu_.Compose(words, sources[0], sources[1], outWidth, outHeight, error) || !gpu_.Submit(false, error)) {
            Fail(error);
            return false;
        }
        counters.presentationFrames++;
        counters.gpuComposedFrames++;
        counters.scaledComposedFrames++;
        out.valid = true;
        out.width = outWidth;
        out.height = outHeight;
        out.image = gpu_.DisplayImage();
        out.fxaa = false;
        return true;
    }

    // ---- VBlank ---------------------------------------------------------------------------------------------
    void Age() {
        for (Target &t : targets_) {
            if (!t.alive) continue;
            if (t.usedThisFrame) {
                t.unusedFrames = 0;
            } else if (++t.unusedFrames > kAgeLimitFrames) {
                // 6.2: deleted without readback. Its drawn area is lost to memory, as in the reference; a later
                // host read of it sees what memory held.
                DestroyTarget(t);
                stats.targetsAged++;
                continue;
            }
            t.usedThisFrame = false;
        }
    }
};

// ---- Renderer -----------------------------------------------------------------------------------------------

namespace {

// The [core2] session line (replay summary, game log every 600 VBlanks).
void PrintCore2Stats(const Core2Counters &c, const Core2GpuStats &g, std::uint64_t vblanks) {
    std::fprintf(stderr,
                 "[core2] vblank=%llu targets created=%llu aged=%llu replaced=%llu grown=%llu textures targets=%llu "
                 "memory=%llu cache_hits=%llu decodes=%llu dirty=%llu dirty_px=%llu readbacks=%llu "
                 "readback_px=%llu tone_runs=%llu tone_skipped=%llu tone_alpha_only=%llu display targets=%llu "
                 "memory=%llu indexed_readbacks=%llu direct_readbacks=%llu clut_readbacks=%llu gpu batches=%llu draws=%llu submits=%llu "
                 "waits=%llu image_mb=%.1f\n",
                 (unsigned long long)vblanks, (unsigned long long)c.targetsCreated, (unsigned long long)c.targetsAged,
                 (unsigned long long)c.targetsReplaced, (unsigned long long)c.targetsGrown,
                 (unsigned long long)c.texturesFromTargets, (unsigned long long)c.texturesFromMemory,
                 (unsigned long long)c.atlasHits, (unsigned long long)g.atlasUploads,
                 (unsigned long long)c.dirtyPatches, (unsigned long long)c.dirtyPatchPixels,
                 (unsigned long long)c.readbacks, (unsigned long long)c.readbackPixels, (unsigned long long)c.toneRuns,
                 (unsigned long long)c.toneDrawsSkipped, (unsigned long long)c.toneAlphaOnlyUnsupported,
                 (unsigned long long)c.displayFromTargets, (unsigned long long)c.displayFromMemory,
                 (unsigned long long)c.indexedFromTargetReadbacks, (unsigned long long)c.directFromTargetReadbacks,
                 (unsigned long long)c.clutReadbacks,
                 (unsigned long long)g.batches, (unsigned long long)g.draws, (unsigned long long)g.submissions,
                 (unsigned long long)g.waits, static_cast<double>(g.imageBytes) / (1024.0 * 1024.0));
}

}  // namespace

Renderer::Renderer() : impl_(std::make_unique<Impl>()) {}
Renderer::~Renderer() { PrintCore2Stats(impl_->stats, impl_->gpu_.Stats(), impl_->vblanks_); }

void Renderer::SetScale(std::uint32_t scale, float shift) {
    impl_->scale_ = std::max<std::uint32_t>(scale, 1u);
    impl_->shift_ = shift;
}

std::uint32_t Renderer::Scale() const { return impl_->scale_; }

bool Renderer::Init(const gpu::VulkanHandles &handles, std::string &error) {
    return impl_->gpu_.Init(handles, impl_->scale_, impl_->shift_, error);
}

bool Renderer::GifTransfer(std::uint32_t path, const std::uint8_t *bytes, std::size_t byteCount) {
    if (path == 0 || path > 3 || byteCount % 16 != 0) return false;
    Impl &s = *impl_;
    const std::size_t count = byteCount / 16;
    if constexpr (std::endian::native == std::endian::little && sizeof(Qword) == 16) {
        if (reinterpret_cast<std::uintptr_t>(bytes) % alignof(Qword) == 0) {
            s.frontEnd_.GifTransfer(path, reinterpret_cast<const Qword *>(bytes), count);
            return true;
        }
    }
    s.gifScratch_.resize(count);
    std::memcpy(s.gifScratch_.data(), bytes, byteCount);
    s.frontEnd_.GifTransfer(path, s.gifScratch_.data(), count);
    return true;
}

bool Renderer::GifBusy(std::uint32_t path) const { return impl_->frontEnd_.GifBusy(path); }

void Renderer::GifReset(std::uint32_t pathMask) {
    impl_->frontEnd_.GifReset(pathMask);
    impl_->memory_.ResetRead();
    impl_->pendingRead_.reset();
}

bool Renderer::WritePrivileged(std::uint32_t address, std::uint64_t value) {
    using namespace priv;
    auto &regs = impl_->displayRegs_;
    switch (address) {
        case kPmode: regs.pmode = value; return true;
        case kSmode1: regs.smode1 = value; return true;
        case kSmode2: regs.smode2 = value; return true;
        case kSyncv: regs.syncv = value; return true;
        case kDispfb1: regs.circuits[0].dispfb = value; return true;
        case kDisplay1: regs.circuits[0].display = value; return true;
        case kDispfb2: regs.circuits[1].dispfb = value; return true;
        case kDisplay2: regs.circuits[1].display = value; return true;
        case kBgcolor:
            impl_->background_[0] = static_cast<std::uint8_t>(FieldOf(value, 0, 8));
            impl_->background_[1] = static_cast<std::uint8_t>(FieldOf(value, 8, 8));
            impl_->background_[2] = static_cast<std::uint8_t>(FieldOf(value, 16, 8));
            return true;
        case kCsr:
            if (FieldOf(value, 9, 1) != 0) ResetGs();
            return true;
        default:
            return false;
    }
}

void Renderer::ResetGs() {
    Impl &s = *impl_;
    s.ReadbackAll();  // 4.4: a reset keeps local memory, so every drawn area is stored first
    s.frontEnd_.ResetGs();
    s.frontEnd_.GifReset(7u);
    s.memory_.ResetRead();
    s.pendingRead_.reset();
    s.displayRegs_ = {};
    s.displayRegs_.anti_blur = s.settings_.antiBlur;
    s.previousDby_ = {};
    s.background_ = {};
    s.toneActive_ = false;
}

void Renderer::ClearVram() {
    Impl &s = *impl_;
    for (Target &t : s.targets_) s.DestroyTarget(t);
    auto bytes = s.memory_.Local().bytes();
    std::fill(bytes.begin(), bytes.end(), std::uint8_t{0});
    s.MarkWritten(BlockSpan{0, kAllBlocks});
    s.recentUploads_.clear();
}

std::size_t Renderer::ReadFifo(void *destination, std::size_t byteCount) {
    Impl &s = *impl_;
    if (byteCount == 0) return 0;
    s.frontEnd_.FlushBatch(FlushReason::Transfer);
    if (s.pendingRead_ && s.memory_.LocalToHostPending()) {
        const LocalToHostSetup &setup = *s.pendingRead_;
        const auto psm = memory::decodePsm(setup.spsm);
        if (psm && setup.rrw != 0 && setup.rrh != 0 && setup.ssax + setup.rrw <= 2048u && setup.ssay + setup.rrh <= 2048u) {
            const BufferView view{setup.sbp, setup.sbw, *psm};
            const PixelRect rect{static_cast<std::int32_t>(setup.ssax), static_cast<std::int32_t>(setup.ssay),
                                 static_cast<std::int32_t>(setup.ssax + setup.rrw - 1u),
                                 static_cast<std::int32_t>(setup.ssay + setup.rrh - 1u)};
            s.Readback(SpanOfRect(view, rect), &view, &rect);
        } else {
            s.ReadbackAll();
        }
        s.pendingRead_.reset();
    }
    bool overrun = false;
    const std::vector<std::uint8_t> data = s.memory_.ReadLocalToHost(byteCount, overrun);
    if (overrun) s.counters.fifoOverruns++;
    if (s.memory_.ReadDrained()) s.frontEnd_.WriteRegister(reg::kTrxdir, 3);
    std::memcpy(destination, data.data(), data.size());
    return data.size();
}

void Renderer::Flush() {
    Impl &s = *impl_;
    static const BlockRange kEverything{0, kAllBlocks};
    s.frontEnd_.VBlank(&kEverything, 1);
    s.frontEnd_.FlushBatch(FlushReason::VBlank);
    ++s.vblanks_;
    if (s.displayRegs_.smode2 & 1u) {
        ++s.counters.vblanksInterlaced;
    } else {
        ++s.counters.vblanksProgressive;
    }
    s.Age();
    s.TrimTexturePool();
    if (s.vblanks_ % 600 == 0) PrintCore2Stats(s.stats, s.gpu_.Stats(), s.vblanks_);
    std::string error;
    if (s.gpu_.Ready() && !s.gpu_.Submit(false, error)) s.Fail(error);
}

bool Renderer::ComposeFrameGpu(writeback::GpuFrame &out) { return impl_->ComposeGpu(out); }

bool Renderer::ReadGpuFrame(const writeback::GpuFrame &frame, std::vector<std::uint32_t> &pixels) {
    if (!frame.valid) return false;
    std::string error;
    if (!impl_->gpu_.ReadDisplay(frame.width, frame.height, pixels, error)) {
        impl_->Fail(error);
        return false;
    }
    return true;
}

void Renderer::ComposeFrame(writeback::FrameOutput &out) {
    out = writeback::FrameOutput{};
    writeback::GpuFrame frame;
    if (!ComposeFrameGpu(frame)) return;
    if (!ReadGpuFrame(frame, out.pixels)) return;
    out.valid = true;
    out.width = frame.width;
    out.height = frame.height;
}

void Renderer::SetDisplayField(bool odd) { impl_->oddField_ = odd; }
bool Renderer::DisplayField() const { return impl_->oddField_; }

void Renderer::SetOutputSettings(const writeback::OutputSettings &settings) {
    impl_->settings_ = settings;
    impl_->displayRegs_.anti_blur = settings.antiBlur;
}

const std::uint8_t *Renderer::Slm() {
    impl_->frontEnd_.FlushBatch(FlushReason::Transfer);
    impl_->ReadbackAll();
    return impl_->memory_.Local().bytes().data();
}

const writeback::Counters &Renderer::GetCounters() const { return impl_->counters; }
const Core2Counters &Renderer::Core2Stats() const { return impl_->stats; }
bool Renderer::Failed() const { return impl_->failed_; }
const std::string &Renderer::FailureText() const { return impl_->failure_; }

}  // namespace hwgs::core2
