// MIT License. Copyright (c) 2026 João Alano and GT4Recomp contributors.
// Written from docs/hw-renderer-spec/CORE2-SPEC.md and CORE2-DESIGN.md (TASK-027 core rewrite), clean room
// (decision 0070). Vulkan entry points are called through pointers (hw_gs_vulkan_loader.hpp, device table).

// Vulkan create-info structs are initialised with sType only (pNext defaults to null by the spec).
#pragma clang diagnostic ignored "-Wmissing-field-initializers"

#include "core2_gpu.hpp"

#include "../writeback/hw_gs_vulkan_device_table.hpp"
#include "../shaders/hw_shader_blobs.hpp"
#include "../memory/hw_memory.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <map>
#include <mutex>
#include <thread>
#include <unordered_set>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <unordered_map>

namespace hwgs::core2 {
namespace {

constexpr VkDeviceSize kStagingBytes = static_cast<VkDeviceSize>(256) << 20;
constexpr VkDeviceSize kVertexBytes = static_cast<VkDeviceSize>(64) << 20;
constexpr VkDeviceSize kUniformBytes = static_cast<VkDeviceSize>(16) << 20;
constexpr VkDeviceSize kReadbackBytes = static_cast<VkDeviceSize>(64) << 20;
constexpr VkDeviceSize kPixelStateBytes = 256;  // one GpuPixelState block (hw_pixel_uber.frag)
constexpr std::uint32_t kAtlasSide = 4096;      // 1x texel atlas
constexpr std::uint32_t kGraphicsPushBytes = 32;
constexpr std::uint32_t kComputePushBytes = 128;
constexpr std::uint32_t kTransientSets = 32768;

bool check(VkResult result, const char *what, std::string &error) {
  if (result == VK_SUCCESS) return true;
  error = std::string(what) + " failed (VkResult " + std::to_string(static_cast<int>(result)) + ")";
  return false;
}

std::uint32_t FloatBits(float value) {
  std::uint32_t bits = 0;
  std::memcpy(&bits, &value, sizeof bits);
  return bits;
}

struct Buffer {
  VkBuffer buffer = VK_NULL_HANDLE;
  VkDeviceMemory memory = VK_NULL_HANDLE;
  void *mapped = nullptr;
  VkDeviceSize size = 0;
  VkDeviceSize used = 0;  // ring cursor for this recording, inside the recording's share
  VkDeviceSize base = 0;  // the recording's share: [base, base + size / slotCount) (S4: the slots take turns)
};

struct Image {
  VkImage image = VK_NULL_HANDLE;
  VkDeviceMemory memory = VK_NULL_HANDLE;
  VkImageView view = VK_NULL_HANDLE;
  std::uint32_t nativeWidth = 0, nativeHeight = 0;
  std::uint32_t width = 0, height = 0;  // in output pixels
  std::uint32_t scale = 1;
  VkDeviceSize bytes = 0;
  bool alive = false;
  std::uint32_t serial = 0;  // B4: unique per make_image, so a cached descriptor set never names a reused image
  std::int32_t block = -1;   // S2: the image memory block it lives in (-1: an allocation of its own)
  VkDeviceSize offset = 0;   // S2: its place in that block
};

// B4: the images bound by one draw descriptor set, by serial.
struct DrawSetKey {
  std::uint32_t frame = 0, depth = 0, texels = 0, texture = 0;
  bool operator==(const DrawSetKey &other) const {
    return frame == other.frame && depth == other.depth && texels == other.texels && texture == other.texture;
  }
};
struct DrawSetKeyHash {
  std::size_t operator()(const DrawSetKey &k) const {
    std::uint64_t h = (static_cast<std::uint64_t>(k.frame) << 32 | k.depth) * 0x9E3779B97F4A7C15ull;
    h ^= (static_cast<std::uint64_t>(k.texels) << 32 | k.texture) + 0x7F4A7C159E3779B9ull + (h << 6) + (h >> 2);
    return static_cast<std::size_t>(h);
  }
};

// B4: device memory kept in finished images for reuse by size (see Impl::retire).
constexpr VkDeviceSize kRecycledImageBytes = static_cast<VkDeviceSize>(128) << 20;
constexpr VkDeviceSize kRecycledImageBytesIntegrated = static_cast<VkDeviceSize>(32) << 20;  // IN-431

}  // namespace

struct Gpu::Impl {
  VkInstance instance = VK_NULL_HANDLE;
  VkPhysicalDevice physical = VK_NULL_HANDLE;
  VkDevice device = VK_NULL_HANDLE;
  VkQueue queue = VK_NULL_HANDLE;
  VkPhysicalDeviceMemoryProperties memoryProperties{};
  VkPhysicalDeviceLimits limits{};
  std::uint32_t scale = 1;
  float shift = 1.0f;
  bool interlock = false;
  bool ready = false;

  VkCommandPool pool = VK_NULL_HANDLE;
  // Two recordings alternate (double buffering): the CPU records one while the GPU runs the other, each with its own
  // command buffer, fence, descriptor pool and share of every ring.
  struct Slot {
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    VkDescriptorPool pool = VK_NULL_HANDLE;
    bool inFlight = false;
    std::uint32_t passTimestamps = 0;  // B4: draw pass timestamp pairs written in this slot's recording
    std::vector<Image> doomed;  // released once this slot's submission is complete
    std::uint64_t serial = 0;   // IN-439: the number of the submission this slot carries (1, 2, ...)
  };
  // S4: up to kMaxSlots submissions in flight (GT4_CORE2_SLOTS, default 8, 2 to 8). With two, the CPU could run only
  // one submission ahead of the GPU: with the GPU at a low clock and full of small dependent commands, the GS thread
  // waited for it in the middle of frames (draw stage waits of 12-30 ms in the owner's menus and the Arcade race
  // start), and submitting more often made it worse.
  static constexpr std::uint32_t kMaxSlots = 8;
  Slot slots[kMaxSlots];
  std::uint32_t slotCount = [] {
    const char *text = std::getenv("GT4_CORE2_SLOTS");
    const long value = text ? std::strtol(text, nullptr, 10) : 8;
    return static_cast<std::uint32_t>(std::clamp<long>(value, 2, static_cast<long>(kMaxSlots)));
  }();
  // B4: per slot, the start and end of its command buffer, then up to kPassTimestampPairs draw passes (start, end);
  // read when the slot's fence has signalled.
  static constexpr std::uint32_t kPassTimestampPairs = 64;
  static constexpr std::uint32_t kTimestampsPerSlot = 2u + 2u * kPassTimestampPairs;
  VkQueryPool timestamps = VK_NULL_HANDLE;
  // IN-422: the window of the next texture decode (SetDecodeWindow).
  const std::uint8_t *windowMemory = nullptr;
  std::uint32_t windowFirstPage = 0, windowPages = 0;

  // Copies the decode window's pages into the staging ring (wrapping at the end of GS memory) and points `glm` at
  // them; returns the push words of the window (base word, word count), {0, 0} without a window.
  bool bind_window(VkDescriptorBufferInfo &glm, std::uint32_t &baseWord, std::uint32_t &words, std::string &error) {
    baseWord = 0;
    words = 0;
    if (!windowMemory) return true;
    const VkDeviceSize bytes = static_cast<VkDeviceSize>(windowPages) * 8192u;
    const VkDeviceSize align = std::max<VkDeviceSize>(limits.minStorageBufferOffsetAlignment, 16);
    VkDeviceSize offset = 0;
    if (!ring(staging, bytes, align, offset, error)) return false;
    std::uint8_t *out = static_cast<std::uint8_t *>(staging.mapped) + offset;
    const std::uint32_t first = windowFirstPage % 512u;
    const std::uint32_t head = std::min(windowPages, 512u - first);
    std::memcpy(out, windowMemory + static_cast<std::size_t>(first) * 8192u, static_cast<std::size_t>(head) * 8192u);
    if (head < windowPages) {
      std::memcpy(out + static_cast<std::size_t>(head) * 8192u, windowMemory,
                  static_cast<std::size_t>(windowPages - head) * 8192u);
    }
    glm = VkDescriptorBufferInfo{staging.buffer, offset, bytes};
    baseWord = first * 2048u;
    words = windowPages * 2048u;
    windowMemory = nullptr;
    stats.windowDecodes++;
    stats.windowBytes += bytes;
    return true;
  }
  // B4: one pipeline statistics query per slot over its whole command buffer (primitives, fragment and compute
  // invocations), where the device enables them.
  VkQueryPool statistics = VK_NULL_HANDLE;
  double timestampNs = 0.0;  // nanoseconds per timestamp tick (0: no timestamps)
  std::uint32_t slot = 0;           // the slot being recorded
  std::uint32_t lastSubmitted = kMaxSlots - 1;  // the slot submitted last (set to slotCount - 1 at Init)
  VkCommandBuffer cmd = VK_NULL_HANDLE;  // slots[slot].cmd
  VkFence fence = VK_NULL_HANDLE;
  bool recording = false;
  // IN-410: the recording is submitted every `submitEvery` draw batches, so the GPU draws while the CPU records the
  // rest of the frame and a readback in mid-frame (GT4's flare reads) waits only for the work after the last
  // submission. GT4_CORE2_SUBMIT_EVERY=<batches>, 0 = only when the CPU needs a result or at the end of the frame.
  // S4: 32 since more submissions can be in flight (was 96 with two slots).
  std::uint32_t submitEvery = [] {
    const char *text = std::getenv("GT4_CORE2_SUBMIT_EVERY");
    return text ? static_cast<std::uint32_t>(std::strtoul(text, nullptr, 10)) : 32u;
  }();
  std::uint32_t batchesSinceSubmit = 0;
  // IN-451: speculative depth rejection before texturing in the pixel shader (exact); GT4_CORE2_EARLY_Z=0 turns it
  // off (A/B, the shader's constant 103).
  bool earlyZ = std::getenv("GT4_CORE2_EARLY_Z") == nullptr || std::strcmp(std::getenv("GT4_CORE2_EARLY_Z"), "0") != 0;
  // Draw descriptor sets of this recording by their four images (a batch with the same targets reuses its set).
  std::unordered_map<DrawSetKey, VkDescriptorSet, DrawSetKeyHash> drawSets;
  // B4: images whose last use finished, kept for reuse by size instead of freed: every new image was a device memory
  // allocation and several driver calls, and core 2 replaces 5-6 targets and pooled textures per night VBlank.
  // GT4_CORE2_IMAGE_POOL=0 frees them at once (A/B).
  std::vector<Image> recycled;
  VkDeviceSize recycledBytes = 0;
  bool integrated = false;  // IN-431: VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU
  bool recycleImages = std::getenv("GT4_CORE2_IMAGE_POOL") == nullptr || std::strcmp(std::getenv("GT4_CORE2_IMAGE_POOL"), "0") != 0;
  std::uint32_t nextSerial = 1;
  bool renderPassOpen = false;
  bool drewSinceBarrier = false;

  Buffer staging, vertices, uniforms, readback;
  // IN-439: results of the reductions taken later (ReduceLater), kLaterSlots of up to kLaterWords words each, and the
  // submission that carries each one. The recording in progress becomes submission submitCount + 1.
  static constexpr std::uint32_t kLaterSlots = 64, kLaterWords = 1024;
  Buffer later;
  struct LaterReduce {
    std::uint64_t serial = 0;  // 0: free
    std::uint32_t count = 0;
  };
  LaterReduce laterReduces[kLaterSlots];
  std::uint32_t laterNext = 0;
  std::uint64_t submitCount = 0;

  std::vector<Image> images;
  std::vector<ImageId> freeIds;
  std::vector<Image> doomed;  // destroyed once the submission that may use them is complete

  Image atlas;
  std::uint32_t atlasShelfX = 0, atlasShelfY = 0, atlasShelfHeight = 0;
  Image display;
  // IN-405: deinterlace (hw_deinterlace_scaled.comp: 0 history[4], 4 woven, 5 previous output, 6 output) and FXAA
  // (hw_postfx_fxaa.comp: 0 source, 1 output), each with its own set layout; the images follow the display size.
  Image deinterlaceHistory[gpu::kGpuDeinterlaceHistory];
  Image deinterlaceWoven, deinterlaceOutput, fxaa;
  // IN-432: presentation resampling (core2_present_scale.comp: 0 source, 1 output); two passes ping-pong through
  // presentA and presentB, the last one writes presentOut.
  Image presentA, presentB, presentOut;
  VkDescriptorSetLayout presentSetLayout = VK_NULL_HANDLE;
  VkPipelineLayout presentLayout = VK_NULL_HANDLE;
  VkShaderModule presentModule = VK_NULL_HANDLE;
  VkPipeline presentPipeline = VK_NULL_HANDLE;
  VkDescriptorSetLayout deinterlaceSetLayout = VK_NULL_HANDLE, fxaaSetLayout = VK_NULL_HANDLE;
  VkPipelineLayout deinterlaceLayout = VK_NULL_HANDLE, fxaaLayout = VK_NULL_HANDLE;
  VkShaderModule deinterlaceModule = VK_NULL_HANDLE, fxaaModule = VK_NULL_HANDLE;
  VkPipeline deinterlacePipeline = VK_NULL_HANDLE, fxaaPipeline = VK_NULL_HANDLE;

  VkDescriptorSetLayout drawSetLayout = VK_NULL_HANDLE;
  VkPipelineLayout drawLayout = VK_NULL_HANDLE;
  VkRenderPass renderPass = VK_NULL_HANDLE;
  VkFramebuffer framebuffer = VK_NULL_HANDLE;
  std::uint32_t framebufferWidth = 0, framebufferHeight = 0;
  VkShaderModule vertexModule = VK_NULL_HANDLE, fragmentModule = VK_NULL_HANDLE, interlockModule = VK_NULL_HANDLE;
  VkPipeline pipelines[3] = {};  // points, lines, triangles
  VkPipeline interlockTriangles = VK_NULL_HANDLE;

  // IN-437: the interlocked triangle pipeline specialised per draw state. hw_pixel_uber.frag reads the state block
  // and branches on about 50 of its fields in every wave; with the fields as specialization constants the compiler
  // folds those branches (about 3x less fragment cost at 12x in the fill benchmark, IN-436). The source and
  // the values are the same, so a specialised draw is exactly the uber draw. Variants compile on a worker thread; a
  // batch whose variant is not ready yet draws with interlockTriangles. GT4_CORE2_SPECIALIZE=0 turns this off;
  // =sync compiles each variant on first use (deterministic, for the tests).
  enum class SpecMode { off, async, sync };
  SpecMode specMode = [] {
    const char *text = std::getenv("GT4_CORE2_SPECIALIZE");
    if (text && (std::strcmp(text, "0") == 0 || std::strcmp(text, "off") == 0)) return SpecMode::off;
    if (text && std::strcmp(text, "sync") == 0) return SpecMode::sync;
    return SpecMode::async;
  }();
  static constexpr std::uint32_t kSpecFieldCount = 30;
  // Diagnostics: GT4_CORE2_SPEC_FIELDS=<hex> specialises only the key slots whose bit is set (slot 30: the push word,
  // slot 31: the anisotropy of a mipmapped draw).
  const std::uint32_t specFieldMask = [] {
    const char *text = std::getenv("GT4_CORE2_SPEC_FIELDS");
    return text ? static_cast<std::uint32_t>(std::strtoul(text, nullptr, 16)) : 0xFFFFFFFFu;
  }();
  static constexpr std::size_t kMaxSpecPipelines = 2048;  // beyond this many draw states the uber pipeline draws
  // The specialised fields, then the push word, then the anisotropy (tex_origin_x bits 21-23) of a mipmapped draw.
  using SpecKey = std::array<std::uint32_t, kSpecFieldCount + 2>;
  struct SpecKeyHash {
    std::size_t operator()(const SpecKey &key) const {
      std::uint64_t h = 1469598103934665603ull;
      for (std::uint32_t v : key) h = (h ^ v) * 1099511628211ull;
      return static_cast<std::size_t>(h);
    }
  };
  std::unordered_map<SpecKey, VkPipeline, SpecKeyHash> specLocal;  // GS thread: variants known to be ready
  std::unordered_set<SpecKey, SpecKeyHash> specSeen;               // GS thread: variants ready, compiling or queued
  std::mutex specMutex;  // guards the members below up to specThread, except the atomic
  std::condition_variable specWake;
  std::deque<SpecKey> specQueue;
  std::vector<std::pair<SpecKey, VkPipeline>> specDone;  // compiled, not yet moved into specLocal
  std::vector<VkPipeline> specAll;                       // every compiled variant (destroyed with the renderer)
  bool specStop = false;
  std::uint64_t specCompileNs = 0, specCompileMaxNs = 0, specFailures = 0;
  std::atomic<std::uint32_t> specDoneCount{0};
  std::uint32_t specDoneSeen = 0;  // GS thread: specDoneCount when specDone was last emptied
  std::thread specThread;

  VkDescriptorSetLayout computeSetLayout = VK_NULL_HANDLE;
  VkPipelineLayout computeLayout = VK_NULL_HANDLE;
  VkShaderModule patchModule = VK_NULL_HANDLE, reduceModule = VK_NULL_HANDLE, toneModule = VK_NULL_HANDLE,
                 composeModule = VK_NULL_HANDLE;
  // IN-442: constant fills (core2_fill.comp). GT4_CORE2_COMPUTE_FILLS=0 draws them as sprites (A/B).
  VkShaderModule fillModule = VK_NULL_HANDLE;
  VkPipeline fillPipeline = VK_NULL_HANDLE;
  const bool computeFills = std::getenv("GT4_CORE2_COMPUTE_FILLS") == nullptr ||
                            std::strcmp(std::getenv("GT4_CORE2_COMPUTE_FILLS"), "0") != 0;
  VkPipeline patchPipeline = VK_NULL_HANDLE, reducePipeline = VK_NULL_HANDLE, tonePipeline = VK_NULL_HANDLE,
             composePipeline = VK_NULL_HANDLE;

  // Texture decode from the GLM (hw_glm_decode.comp): 0 GLM, 1 address tables, 2 palettes, 3 texels, 5 GPU CLUT.
  Buffer glmTables;
  // IN-408: the GPU CLUT (512 halfwords, device local), the CT32 inverse in-page table, the target CLUT kernel.
  VkBuffer clut = VK_NULL_HANDLE;
  VkDeviceMemory clutMemory = VK_NULL_HANDLE;
  Buffer inverseCt32;
  VkShaderModule clutTargetModule = VK_NULL_HANDLE, materializeModule = VK_NULL_HANDLE;
  VkPipeline clutTargetPipeline = VK_NULL_HANDLE, materializePipeline = VK_NULL_HANDLE;
  VkBuffer glm = VK_NULL_HANDLE;
  VkDeviceMemory glmMemory = VK_NULL_HANDLE;
  VkDeviceSize glmTableBytes = 0;
  VkDescriptorSetLayout decodeSetLayout = VK_NULL_HANDLE;
  VkPipelineLayout decodeLayout = VK_NULL_HANDLE;
  VkShaderModule decodeModule = VK_NULL_HANDLE, patchGlmModule = VK_NULL_HANDLE;
  VkPipeline decodePipeline = VK_NULL_HANDLE, patchGlmPipeline = VK_NULL_HANDLE;

  VkDescriptorPool transientPool = VK_NULL_HANDLE;
  std::uint32_t transientSetsUsed = 0;

  Core2GpuStats stats;

  // ---- Memory and objects ----------------------------------------------------------------------------
  bool memory_type(std::uint32_t bits, VkMemoryPropertyFlags wanted, VkMemoryPropertyFlags preferred,
                   std::uint32_t &index) const {
    for (int pass = 0; pass < 2; ++pass) {
      const VkMemoryPropertyFlags need = pass == 0 ? (wanted | preferred) : wanted;
      for (std::uint32_t i = 0; i < memoryProperties.memoryTypeCount; ++i) {
        if ((bits & (1u << i)) && (memoryProperties.memoryTypes[i].propertyFlags & need) == need) {
          index = i;
          return true;
        }
      }
    }
    return false;
  }

  bool make_buffer(Buffer &out, VkDeviceSize size, VkBufferUsageFlags usage, VkMemoryPropertyFlags preferred,
                   std::string &error) {
    VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    info.size = size;
    info.usage = usage;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (!check(gt4_hwd_vkCreateBuffer(device, &info, nullptr, &out.buffer), "vkCreateBuffer", error)) return false;
    VkMemoryRequirements requirements{};
    gt4_hwd_vkGetBufferMemoryRequirements(device, out.buffer, &requirements);
    VkMemoryAllocateInfo allocate{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    allocate.allocationSize = requirements.size;
    if (!memory_type(requirements.memoryTypeBits,
                     VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, preferred,
                     allocate.memoryTypeIndex)) {
      error = "no host-visible memory type for a CORE2 buffer";
      return false;
    }
    if (!check(gt4_hwd_vkAllocateMemory(device, &allocate, nullptr, &out.memory), "vkAllocateMemory", error)) return false;
    if (!check(gt4_hwd_vkBindBufferMemory(device, out.buffer, out.memory, 0), "vkBindBufferMemory", error)) return false;
    if (!check(gt4_hwd_vkMapMemory(device, out.memory, 0, size, 0, &out.mapped), "vkMapMemory", error)) return false;
    out.size = size;
    return true;
  }

  // Reserves `bytes` of a ring for this recording, aligned; submits and waits when the ring is full.
  // S4: one recording's share of a ring, a multiple of 4 KiB so every share starts aligned for any buffer offset rule
  // (dynamic uniform and storage offsets); size / slotCount alone is not aligned for 6 slots.
  VkDeviceSize ring_share(const Buffer &buffer) const {
    return buffer.size / slotCount / 4096u * 4096u;
  }

  bool ring(Buffer &buffer, VkDeviceSize bytes, VkDeviceSize alignment, VkDeviceSize &offset, std::string &error) {
    if (bytes > buffer.size) {
      error = "CORE2 ring request larger than the ring";
      return false;
    }
    const VkDeviceSize half = ring_share(buffer);  // this recording's share
    if (bytes > half) {
      error = "CORE2 ring request larger than a recording's share of the ring";
      return false;
    }
    VkDeviceSize at = (buffer.used + alignment - 1) / alignment * alignment;
    if (at + bytes > half) {
      if (!submit(false, error) || !begin_recording(error)) return false;
      at = 0;
    }
    buffer.used = at + bytes;
    offset = buffer.base + at;
    return true;
  }

  // S2: device memory for images taken from 64 MiB blocks instead of one allocation per image. vkAllocateMemory is a
  // kernel call: at GT4's race start core 2 created 40-160 images per VBlank (texture decodes of new textures), which
  // made 20-65 ms spikes. Images over 16 MiB keep an allocation of their own; empty blocks are kept for reuse.
  // GT4_CORE2_IMAGE_BLOCKS=0 gives every image its own allocation (A/B).
  static constexpr VkDeviceSize kImageBlockBytes = static_cast<VkDeviceSize>(64) << 20;
  static constexpr VkDeviceSize kOwnAllocationBytes = static_cast<VkDeviceSize>(16) << 20;
  struct MemoryBlock {
    VkDeviceMemory memory = VK_NULL_HANDLE;
    std::uint32_t type = 0;
    std::map<VkDeviceSize, VkDeviceSize> free;  // offset -> size of each free range
  };
  std::vector<MemoryBlock> imageBlocks;
  bool imageBlocksOn =
      std::getenv("GT4_CORE2_IMAGE_BLOCKS") == nullptr || std::strcmp(std::getenv("GT4_CORE2_IMAGE_BLOCKS"), "0") != 0;

  // First fit in the blocks of memory type `type`; a new block when none has room.
  bool take_block_memory(const VkMemoryRequirements &requirements, std::uint32_t type, Image &out, std::string &error) {
    const VkDeviceSize alignment = std::max<VkDeviceSize>(requirements.alignment, 1);
    for (int attempt = 0; attempt < 2; ++attempt) {
      for (std::size_t b = 0; b < imageBlocks.size(); ++b) {
        MemoryBlock &block = imageBlocks[b];
        if (block.type != type) continue;
        for (auto range = block.free.begin(); range != block.free.end(); ++range) {
          const VkDeviceSize start = range->first, end = range->first + range->second;
          const VkDeviceSize at = (start + alignment - 1) / alignment * alignment;
          if (at + requirements.size > end) continue;
          block.free.erase(range);
          if (at > start) block.free[start] = at - start;
          if (at + requirements.size < end) block.free[at + requirements.size] = end - (at + requirements.size);
          out.memory = block.memory;
          out.block = static_cast<std::int32_t>(b);
          out.offset = at;
          return true;
        }
      }
      MemoryBlock block;
      block.type = type;
      VkMemoryAllocateInfo allocate{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
      allocate.allocationSize = kImageBlockBytes;
      allocate.memoryTypeIndex = type;
      if (!check(gt4_hwd_vkAllocateMemory(device, &allocate, nullptr, &block.memory), "vkAllocateMemory", error)) {
        return false;
      }
      block.free[0] = kImageBlockBytes;
      imageBlocks.push_back(std::move(block));
      stats.imageBlocks++;
    }
    error = "CORE2 image does not fit an image memory block";
    return false;
  }

  // Returns a block range, merged with its free neighbours.
  void give_block_memory(std::int32_t blockIndex, VkDeviceSize offset, VkDeviceSize size) {
    if (blockIndex < 0 || static_cast<std::size_t>(blockIndex) >= imageBlocks.size()) return;
    std::map<VkDeviceSize, VkDeviceSize> &free = imageBlocks[static_cast<std::size_t>(blockIndex)].free;
    auto next = free.lower_bound(offset);
    if (next != free.end() && offset + size == next->first) {
      size += next->second;
      next = free.erase(next);
    }
    if (next != free.begin()) {
      auto previous = std::prev(next);
      if (previous->first + previous->second == offset) {
        previous->second += size;
        return;
      }
    }
    free[offset] = size;
  }

  bool make_image(Image &out, std::uint32_t nativeWidth, std::uint32_t nativeHeight, std::uint32_t imageScale,
                  std::string &error) {
    out = Image{};
    out.nativeWidth = std::max<std::uint32_t>(nativeWidth, 1u);
    out.nativeHeight = std::max<std::uint32_t>(nativeHeight, 1u);
    out.scale = imageScale;
    out.width = out.nativeWidth * imageScale;
    out.height = out.nativeHeight * imageScale;
    if (out.width > limits.maxImageDimension2D || out.height > limits.maxImageDimension2D) {
      error = "CORE2 image larger than the device limit";
      return false;
    }
    out.serial = nextSerial++;
    // B4: a finished image of the same size is taken over (cleared below like a new one).
    for (std::size_t i = recycled.size(); i-- > 0;) {
      if (recycled[i].width != out.width || recycled[i].height != out.height) continue;
      out.image = recycled[i].image;
      out.memory = recycled[i].memory;
      out.view = recycled[i].view;
      out.bytes = recycled[i].bytes;
      out.block = recycled[i].block;
      out.offset = recycled[i].offset;
      recycledBytes -= recycled[i].bytes;
      recycled.erase(recycled.begin() + static_cast<std::ptrdiff_t>(i));
      out.alive = true;
      stats.recycledImages++;
      return clear_new_image(out, error);
    }
    VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    info.imageType = VK_IMAGE_TYPE_2D;
    info.format = VK_FORMAT_R32_UINT;
    info.extent = {out.width, out.height, 1};
    info.mipLevels = 1;
    info.arrayLayers = 1;
    info.samples = VK_SAMPLE_COUNT_1_BIT;
    info.tiling = VK_IMAGE_TILING_OPTIMAL;
    info.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (!check(gt4_hwd_vkCreateImage(device, &info, nullptr, &out.image), "vkCreateImage", error)) return false;
    VkMemoryRequirements requirements{};
    gt4_hwd_vkGetImageMemoryRequirements(device, out.image, &requirements);
    VkMemoryAllocateInfo allocate{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    allocate.allocationSize = requirements.size;
    if (!memory_type(requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0, allocate.memoryTypeIndex)) {
      error = "no device-local memory type for a CORE2 image";
      return false;
    }
    if (imageBlocksOn && requirements.size <= kOwnAllocationBytes) {
      if (!take_block_memory(requirements, allocate.memoryTypeIndex, out, error)) return false;
    } else {
      if (!check(gt4_hwd_vkAllocateMemory(device, &allocate, nullptr, &out.memory), "vkAllocateMemory", error)) return false;
      stats.imagesAllocated++;
    }
    if (!check(gt4_hwd_vkBindImageMemory(device, out.image, out.memory, out.offset), "vkBindImageMemory", error)) {
      return false;
    }
    VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    view.image = out.image;
    view.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view.format = VK_FORMAT_R32_UINT;
    view.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    if (!check(gt4_hwd_vkCreateImageView(device, &view, nullptr, &out.view), "vkCreateImageView", error)) return false;
    out.bytes = requirements.size;
    out.alive = true;
    stats.imageBytes += out.bytes;
    stats.imagesCreated++;
    return clear_new_image(out, error);
  }

  // UNDEFINED -> GENERAL and zero, in this recording (a recycled image's old content is discarded the same way).
  bool clear_new_image(Image &out, std::string &error) {
    if (!begin_recording(error)) return false;
    end_render_pass();
    VkImageMemoryBarrier toGeneral{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    toGeneral.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    toGeneral.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    toGeneral.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toGeneral.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toGeneral.image = out.image;
    toGeneral.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    toGeneral.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    gt4_hw_vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr,
                                0, nullptr, 1, &toGeneral);
    VkClearColorValue zero{};
    const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    gt4_hwd_vkCmdClearColorImage(cmd, out.image, VK_IMAGE_LAYOUT_GENERAL, &zero, 1, &range);
    full_barrier();
    return true;
  }

  // B4: a doomed image whose submission finished: kept for reuse (newest last; the oldest go past the budget).
  void retire(Image &image) {
    if (!image.alive) return;
    if (!recycleImages) {
      release(image);
      return;
    }
    recycled.push_back(image);
    recycledBytes += image.bytes;
    image = Image{};
    while (recycledBytes > (integrated ? kRecycledImageBytesIntegrated : kRecycledImageBytes) && !recycled.empty()) {
      recycledBytes -= recycled.front().bytes;
      release(recycled.front());
      recycled.erase(recycled.begin());
    }
  }

  void release(Image &image) {
    if (!image.alive) return;
    if (image.view) gt4_hwd_vkDestroyImageView(device, image.view, nullptr);
    if (image.image) gt4_hwd_vkDestroyImage(device, image.image, nullptr);
    if (image.block >= 0) {
      give_block_memory(image.block, image.offset, image.bytes);  // S2
    } else if (image.memory) {
      gt4_hwd_vkFreeMemory(device, image.memory, nullptr);
    }
    stats.imageBytes -= std::min(stats.imageBytes, image.bytes);
    image = Image{};
  }

  void doom(Image &image) {
    if (!image.alive) return;
    doomed.push_back(image);
    image = Image{};
  }

  VkShaderModule make_module(const std::uint32_t *words, std::size_t count, std::string &error) {
    VkShaderModuleCreateInfo info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    info.codeSize = count * sizeof(std::uint32_t);
    info.pCode = words;
    VkShaderModule module = VK_NULL_HANDLE;
    check(gt4_hwd_vkCreateShaderModule(device, &info, nullptr, &module), "vkCreateShaderModule", error);
    return module;
  }

  VkPipeline make_graphics(VkPrimitiveTopology topology, VkShaderModule fragment, std::string &error,
                           const VkSpecializationInfo *specialization = nullptr) {
    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vertexModule;
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fragment;
    stages[1].pName = "main";
    // IN-451: constant 103 (SPEC_EARLY_Z) follows GT4_CORE2_EARLY_Z on every fragment module (a module without the
    // constant ignores the entry).
    VkSpecializationMapEntry entries[64];
    std::uint8_t data[64 * 4];
    std::uint32_t count = 0;
    std::size_t bytes = 0;
    if (specialization) {
      count = specialization->mapEntryCount;
      bytes = specialization->dataSize;
      std::memcpy(entries, specialization->pMapEntries, count * sizeof(VkSpecializationMapEntry));
      std::memcpy(data, specialization->pData, bytes);
    }
    const std::uint32_t earlyZValue = earlyZ ? 1u : 0u;
    entries[count++] = {103u, static_cast<std::uint32_t>(bytes), 4};
    std::memcpy(data + bytes, &earlyZValue, 4);
    bytes += 4;
    const VkSpecializationInfo withEarlyZ{count, entries, bytes, data};
    stages[1].pSpecializationInfo = &withEarlyZ;
    VkVertexInputBindingDescription binding{0, sizeof(gpu::GpuVertex), VK_VERTEX_INPUT_RATE_VERTEX};
    VkVertexInputAttributeDescription attributes[7]{};
    attributes[0] = {0, 0, VK_FORMAT_R32G32_SFLOAT, 0};   // S, T
    attributes[1] = {1, 0, VK_FORMAT_R8G8B8A8_UINT, 8};   // R, G, B, A
    attributes[2] = {2, 0, VK_FORMAT_R32_SFLOAT, 12};     // Q
    attributes[3] = {3, 0, VK_FORMAT_R16G16_UINT, 16};    // X, Y
    attributes[4] = {4, 0, VK_FORMAT_R32_UINT, 20};       // Z
    attributes[5] = {5, 0, VK_FORMAT_R16G16_UINT, 24};    // U, V
    attributes[6] = {6, 0, VK_FORMAT_R8_UNORM, 28};       // F
    VkPipelineVertexInputStateCreateInfo vertexInput{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    vertexInput.vertexBindingDescriptionCount = 1;
    vertexInput.pVertexBindingDescriptions = &binding;
    vertexInput.vertexAttributeDescriptionCount = 7;
    vertexInput.pVertexAttributeDescriptions = attributes;
    VkPipelineInputAssemblyStateCreateInfo assembly{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    assembly.topology = topology;
    VkPipelineViewportStateCreateInfo viewport{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    viewport.viewportCount = 1;
    viewport.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo raster{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    raster.polygonMode = VK_POLYGON_MODE_FILL;
    raster.cullMode = VK_CULL_MODE_NONE;
    raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    raster.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo multisample{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineColorBlendStateCreateInfo blend{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    blend.attachmentCount = 0;  // the fragment stage writes storage images
    VkDynamicState dynamicStates[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamic{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dynamic.dynamicStateCount = 2;
    dynamic.pDynamicStates = dynamicStates;
    VkGraphicsPipelineCreateInfo info{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    info.stageCount = 2;
    info.pStages = stages;
    info.pVertexInputState = &vertexInput;
    info.pInputAssemblyState = &assembly;
    info.pViewportState = &viewport;
    info.pRasterizationState = &raster;
    info.pMultisampleState = &multisample;
    info.pColorBlendState = &blend;
    info.pDynamicState = &dynamic;
    info.layout = drawLayout;
    info.renderPass = renderPass;
    VkPipeline pipeline = VK_NULL_HANDLE;
    check(gt4_hwd_vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &info, nullptr, &pipeline),
          "vkCreateGraphicsPipelines", error);
    return pipeline;
  }

  // IN-437: the state block fields that hw_pixel_uber.frag reads as specialization constants (constant id = 1 + the
  // field index), in key order.
  static constexpr std::uint32_t kSpecFields[kSpecFieldCount] = {
      gpu::gpufield::Format,  gpu::gpufield::Fba,   gpu::gpufield::Dither,    gpu::gpufield::Ate,
      gpu::gpufield::Atst,    gpu::gpufield::Afail, gpu::gpufield::DateOn,    gpu::gpufield::Datm,
      gpu::gpufield::Ztest,   gpu::gpufield::Ztst,  gpu::gpufield::Zmsk,      gpu::gpufield::Zformat,
      gpu::gpufield::BlendOn, gpu::gpufield::Pabe,  gpu::gpufield::BlendA,    gpu::gpufield::BlendB,
      gpu::gpufield::BlendC,  gpu::gpufield::BlendD, gpu::gpufield::Colclamp, gpu::gpufield::TexOn,
      gpu::gpufield::Tfx,     gpu::gpufield::Tcc,   gpu::gpufield::TexelKind, gpu::gpufield::Aem,
      gpu::gpufield::FogOn,   gpu::gpufield::TexFst, gpu::gpufield::TexLinear, gpu::gpufield::WrapS,
      gpu::gpufield::WrapT,   gpu::gpufield::KeepTopByte};

  // IN-437: the key of a draw state. Fields the draw cannot reach are zeroed so equal draws share one variant: the
  // texture fields without TexOn, the blend selectors without BlendOn, the alpha test fields without Ate, DATM
  // without DATE, the depth fields without Ztest, dither unless CT16, the kept top byte unless CT24; of the push word
  // bit 29 (read by the vertex stage only), the texture bits without TexOn, the position depth without Ztest and the
  // fine dither without dither. The shader never reads a zeroed value in that state.
  static SpecKey spec_key(const std::uint32_t *fields, std::uint32_t pushWord) {
    namespace g = gpu::gpufield;
    std::uint32_t f[g::Count] = {};
    for (std::uint32_t index : kSpecFields) f[index] = fields[index];
    std::uint32_t push = pushWord & ~0x20000000u;
    if (f[g::TexOn] == 0) {
      f[g::Tfx] = f[g::Tcc] = f[g::TexelKind] = f[g::Aem] = f[g::TexFst] = f[g::TexLinear] = f[g::WrapS] = f[g::WrapT] = 0;
      push = (push & ~(0x007FFFFFu | 0x01000000u | 0x00800000u | 0x04000000u | 0x80000000u)) | 1u;
    }
    if (f[g::BlendOn] == 0) f[g::Pabe] = f[g::BlendA] = f[g::BlendB] = f[g::BlendC] = f[g::BlendD] = 0;
    if (f[g::Ate] == 0) f[g::Atst] = f[g::Afail] = 0;
    if (f[g::DateOn] == 0) f[g::Datm] = 0;
    if (f[g::Ztest] == 0) {
      f[g::Ztst] = f[g::Zmsk] = f[g::Zformat] = 0;
      push &= ~0x10000000u;
    }
    if (f[g::Format] != 2u) f[g::Dither] = 0;  // HW_FORMAT_CT16
    if (f[g::Dither] == 0) push &= ~0x40000000u;
    if (f[g::Format] != 1u) f[g::KeepTopByte] = 0;  // HW_FORMAT_CT24
    SpecKey key{};
    for (std::uint32_t i = 0; i < kSpecFieldCount; ++i) key[i] = f[kSpecFields[i]];
    key[kSpecFieldCount] = push;
    key[kSpecFieldCount + 1] = (push & 0x00800000u) != 0 ? (fields[g::TexOriginX] >> 21) & 7u : 0u;
    return key;
  }

  // Compiles one variant (on the worker thread, or on the GS thread in sync mode). Key slots outside specFieldMask
  // (diagnostics, GT4_CORE2_SPEC_FIELDS) stay dynamic: the shader reads them from the state block.
  VkPipeline make_specialised(const SpecKey &key, std::string &error) {
    std::uint32_t values[kSpecFieldCount + 2];
    VkSpecializationMapEntry entries[kSpecFieldCount + 2];
    std::uint32_t count = 0;
    for (std::uint32_t i = 0; i < kSpecFieldCount + 2; ++i) {
      if ((specFieldMask >> i & 1u) == 0) continue;
      values[count] = key[i];
      entries[count] = {i == kSpecFieldCount ? 100u : i == kSpecFieldCount + 1 ? 101u : 1u + kSpecFields[i], 4 * count, 4};
      ++count;
    }
    VkSpecializationInfo info{count, entries, 4 * count, values};
    return make_graphics(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, interlockModule, error, &info);  // the driver caches
  }

  // Records a compile time (specMutex held).
  void note_compile(std::uint64_t ns) {
    specCompileNs += ns;
    specCompileMaxNs = std::max(specCompileMaxNs, ns);
  }

  void spec_worker() {
    for (;;) {
      SpecKey key;
      {
        std::unique_lock<std::mutex> lock(specMutex);
        specWake.wait(lock, [&] { return specStop || !specQueue.empty(); });
        if (specStop) return;
        key = specQueue.front();
        specQueue.pop_front();
      }
      const auto start = std::chrono::steady_clock::now();
      std::string error;
      VkPipeline pipeline = make_specialised(key, error);
      const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start);
      {
        std::lock_guard<std::mutex> lock(specMutex);
        note_compile(static_cast<std::uint64_t>(ns.count()));
        if (pipeline) {
          specDone.push_back({key, pipeline});
          specAll.push_back(pipeline);
        } else if (specFailures++ == 0) {
          std::fprintf(stderr, "[core2] a specialised pipeline failed (%s); its draws keep the uber pipeline\n",
                       error.c_str());
        }
      }
      specDoneCount.fetch_add(1, std::memory_order_release);
    }
  }

  // Copies the worker's results and statistics for the GS thread (specMutex held).
  void take_spec_results() {
    for (const auto &[key, pipeline] : specDone) specLocal[key] = pipeline;
    specDone.clear();
    specDoneSeen = specDoneCount.load(std::memory_order_acquire);
    stats.specPipelines = specAll.size();
    stats.specCompileNs = specCompileNs;
    stats.specCompileMaxNs = specCompileMaxNs;
    stats.specFailures = specFailures;
  }

  // The pipeline of an interlocked triangle batch with this state block and push word (GS thread).
  VkPipeline triangle_pipeline(const std::uint32_t *fields, std::uint32_t pushWord) {
    if (specMode == SpecMode::off) return interlockTriangles;
    const SpecKey key = spec_key(fields, pushWord);
    auto found = specLocal.find(key);
    if (found == specLocal.end() && specDoneCount.load(std::memory_order_acquire) != specDoneSeen) {
      std::lock_guard<std::mutex> lock(specMutex);
      take_spec_results();
      found = specLocal.find(key);
    }
    if (found != specLocal.end()) {
      stats.specialisedBatches++;
      return found->second;
    }
    if (specSeen.size() < kMaxSpecPipelines && specSeen.insert(key).second) {
      stats.specRequested = specSeen.size();
      if (specMode == SpecMode::sync) {
        const auto start = std::chrono::steady_clock::now();
        std::string error;
        VkPipeline pipeline = make_specialised(key, error);
        const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start);
        std::lock_guard<std::mutex> lock(specMutex);
        note_compile(static_cast<std::uint64_t>(ns.count()));
        if (pipeline) {
          specAll.push_back(pipeline);
          specLocal[key] = pipeline;
        } else {
          specFailures++;
        }
        take_spec_results();
        if (pipeline) {
          stats.specialisedBatches++;
          return pipeline;
        }
      } else {
        {
          std::lock_guard<std::mutex> lock(specMutex);
          specQueue.push_back(key);
        }
        if (!specThread.joinable()) specThread = std::thread([this] { spec_worker(); });
        specWake.notify_one();
      }
    }
    stats.uberBatches++;
    return interlockTriangles;
  }

  VkPipeline make_compute(VkShaderModule module, std::string &error, VkPipelineLayout layout = VK_NULL_HANDLE) {
    VkComputePipelineCreateInfo info{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    info.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    info.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    info.stage.module = module;
    info.stage.pName = "main";
    info.layout = layout ? layout : computeLayout;
    VkPipeline pipeline = VK_NULL_HANDLE;
    check(gt4_hwd_vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &info, nullptr, &pipeline),
          "vkCreateComputePipelines", error);
    return pipeline;
  }

  // A set layout of storage images only (`counts[i]` images at binding i) and its pipeline layout with `pushBytes`.
  bool image_layouts(const std::uint32_t *counts, std::uint32_t bindings, std::uint32_t pushBytes,
                     VkDescriptorSetLayout &setLayout, VkPipelineLayout &pipelineLayout, std::string &error) {
    std::vector<VkDescriptorSetLayoutBinding> list(bindings);
    for (std::uint32_t i = 0; i < bindings; ++i) {
      list[i] = {i, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, counts[i], VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    }
    VkDescriptorSetLayoutCreateInfo info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    info.bindingCount = bindings;
    info.pBindings = list.data();
    if (!check(gt4_hwd_vkCreateDescriptorSetLayout(device, &info, nullptr, &setLayout), "vkCreateDescriptorSetLayout",
               error)) return false;
    VkPushConstantRange push{VK_SHADER_STAGE_COMPUTE_BIT, 0, pushBytes};
    VkPipelineLayoutCreateInfo layout{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    layout.setLayoutCount = 1;
    layout.pSetLayouts = &setLayout;
    layout.pushConstantRangeCount = 1;
    layout.pPushConstantRanges = &push;
    return check(gt4_hwd_vkCreatePipelineLayout(device, &layout, nullptr, &pipelineLayout), "vkCreatePipelineLayout",
                 error);
  }

  // A 1x-addressed post image of exactly width x height output pixels (recreated when the size changes; `fresh` tells
  // the caller the content is gone).
  bool post_image(Image &image, std::uint32_t width, std::uint32_t height, bool &fresh, std::string &error) {
    fresh = false;
    if (image.alive && image.width == width && image.height == height) return true;
    doom(image);
    fresh = true;
    return make_image(image, width, height, 1, error);
  }

  // ---- Recording ---------------------------------------------------------------------------------------
  void full_barrier() {
    VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    barrier.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    gt4_hw_vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1,
                                &barrier, 0, nullptr, 0, nullptr);
    stats.fullBarriers++;
    drewSinceBarrier = false;
    pendingBufferReads = pendingBufferWrites = 0;  // everything before is ordered against everything after
    pendingReads.clear();
    pendingWrites.clear();
  }

  // IN-419: the accesses recorded since the last full barrier, so a compute or transfer command gets a barrier only
  // when it depends on one of them: it reads or writes what an earlier command wrote, or writes what one read. Core 2
  // recorded a barrier before and after every one (about 2000 per night VBlank, each draining the GPU), though most
  // touch unrelated things (two texture decodes write two different images). Buffers: the GLM and the GPU CLUT;
  // images by serial, whole or one row (the palette rows, IN-420). GT4_CORE2_LAZY_BARRIERS=0 records a barrier around
  // every command again (A/B).
  enum : unsigned { kGlmBuffer = 1u, kClutBuffer = 2u };
  static constexpr std::uint32_t kWholeImage = 0xFFFFFFFFu;
  struct Use {
    std::uint32_t serial = 0;  // 0: no image
    std::uint32_t region = kWholeImage;
    Use(const Image *image) : serial(image ? image->serial : 0u) {}
    Use(const Image *image, std::uint32_t row) : serial(image ? image->serial : 0u), region(row) {}
  };
  // IN-447 (TASK-027 CPU survey, front R2): the images a set touches live in a flat table with generation stamps (an
  // open-addressing index by serial into an entry array), so clear() frees nothing and adding an image after a barrier
  // allocates nothing once the arrays are warm. The answers are those of the former unordered_map of row vectors.
  struct AccessSet {
    struct Entry {
      std::uint32_t serial = 0;
      bool whole = false;
      std::vector<std::uint32_t> rows;  // kept allocated across clears
    };
    struct Slot {
      std::uint32_t serial = 0;
      std::uint32_t generation = 0;  // the slot is in use when this equals the set's generation
      std::uint32_t entry = 0;
    };
    std::vector<Slot> slots = std::vector<Slot>(2048);  // a power of two; doubled when half full
    std::vector<Entry> entries;  // entries[0, live) are this generation's
    std::size_t live = 0;
    std::uint32_t generation = 1;

    std::size_t size() const { return live; }
    std::size_t home(std::uint32_t serial) const { return (serial * 2654435761u) & (slots.size() - 1); }
    const Entry *find(std::uint32_t serial) const {
      for (std::size_t i = home(serial);; i = (i + 1) & (slots.size() - 1)) {
        const Slot &slot = slots[i];
        if (slot.generation != generation) return nullptr;
        if (slot.serial == serial) return &entries[slot.entry];
      }
    }
    Entry &insert(std::uint32_t serial) {
      if (2 * (live + 1) > slots.size()) grow();
      for (std::size_t i = home(serial);; i = (i + 1) & (slots.size() - 1)) {
        Slot &slot = slots[i];
        if (slot.generation == generation) {
          if (slot.serial == serial) return entries[slot.entry];
          continue;
        }
        if (live == entries.size()) entries.emplace_back();
        Entry &entry = entries[live];
        entry.serial = serial;
        entry.whole = false;
        entry.rows.clear();
        slot = Slot{serial, generation, static_cast<std::uint32_t>(live)};
        ++live;
        return entry;
      }
    }
    // Doubles the index and re-inserts this generation's entries (a probe always finds a free slot).
    void grow() {
      std::vector<Slot> bigger(slots.size() * 2);
      slots.swap(bigger);
      for (std::size_t e = 0; e < live; ++e) {
        std::size_t i = home(entries[e].serial);
        while (slots[i].generation == generation) i = (i + 1) & (slots.size() - 1);
        slots[i] = Slot{entries[e].serial, generation, static_cast<std::uint32_t>(e)};
      }
    }
    void add(std::uint32_t serial, std::uint32_t region) {
      if (serial == 0) return;
      Entry &entry = insert(serial);
      if (region == kWholeImage) {
        entry.whole = true;
      } else if (!entry.whole && std::find(entry.rows.begin(), entry.rows.end(), region) == entry.rows.end()) {
        entry.rows.push_back(region);
      }
    }
    bool conflicts(std::uint32_t serial, std::uint32_t region) const {
      if (serial == 0) return false;
      const Entry *entry = find(serial);
      if (!entry) return false;
      return entry->whole || region == kWholeImage ||
             std::find(entry->rows.begin(), entry->rows.end(), region) != entry->rows.end();
    }
    void merge(const AccessSet &other) {
      for (std::size_t i = 0; i < other.live; ++i) {
        const Entry &entry = other.entries[i];
        if (entry.whole) add(entry.serial, kWholeImage);
        for (std::uint32_t row : entry.rows) add(entry.serial, row);
      }
    }
    void clear() {
      live = 0;
      if (++generation == 0) {  // after 2^32 clears: reset the stamps once
        for (Slot &slot : slots) slot.generation = 0;
        generation = 1;
      }
    }
  };
  bool lazyBarriers =
      std::getenv("GT4_CORE2_LAZY_BARRIERS") == nullptr || std::strcmp(std::getenv("GT4_CORE2_LAZY_BARRIERS"), "0") != 0;
  unsigned pendingBufferReads = 0, pendingBufferWrites = 0;
  AccessSet pendingReads, pendingWrites;

  // Records a barrier first when accesses depend on the pending ones, then adds them.
  void order(std::initializer_list<Use> reads, std::initializer_list<Use> writes, unsigned bufferReads,
             unsigned bufferWrites) {
    bool hazard = ((bufferReads | bufferWrites) & pendingBufferWrites) != 0 || (bufferWrites & pendingBufferReads) != 0;
    for (const Use &use : reads) hazard = hazard || pendingWrites.conflicts(use.serial, use.region);
    for (const Use &use : writes) {
      hazard = hazard || pendingWrites.conflicts(use.serial, use.region) || pendingReads.conflicts(use.serial, use.region);
    }
    if (hazard || pendingReads.size() + pendingWrites.size() > 512) {
      full_barrier();
    } else {
      stats.barriersSkipped++;
    }
    pendingBufferReads |= bufferReads;
    pendingBufferWrites |= bufferWrites;
    for (const Use &use : reads) pendingReads.add(use.serial, use.region);
    for (const Use &use : writes) pendingWrites.add(use.serial, use.region);
  }

  // Declares the accesses of the compute or transfer command about to be recorded. With IN-420 the deferred draws are
  // recorded first when the command depends on them (reads what they write, writes what they read or write).
  void touch(const char *label, std::initializer_list<Use> reads, std::initializer_list<Use> writes,
             unsigned bufferReads, unsigned bufferWrites) {
    if (!lazyBarriers) {  // outside_pass and after_op already order every command
      prof_begin(label);
      return;
    }
    if (!deferred.empty()) {
      bool dependsOnDraws = false;
      for (const Use &use : reads) dependsOnDraws = dependsOnDraws || drawWrites.conflicts(use.serial, use.region);
      for (const Use &use : writes) {
        dependsOnDraws = dependsOnDraws || drawWrites.conflicts(use.serial, use.region) ||
                         drawReads.conflicts(use.serial, use.region);
      }
      if (dependsOnDraws) flush_draws();
    }
    order(reads, writes, bufferReads, bufferWrites);
    prof_begin(label);
  }

  // After a compute or transfer command: a barrier at once only without IN-419 (touch orders it otherwise).
  void after_op() {
    if (!lazyBarriers) full_barrier();
    prof_end();
  }

  // S3: GPU cost map (diagnostics, GT4_CORE2_GPU_PROFILE_AT=<vblank>): during one VBlank every compute, transfer and
  // draw command waits for the work before it (a barrier) and is timed on its own (timestamps at the completion of
  // the work before it and of itself); ProfileReport prints the GPU time per kind of command. The barriers make that
  // VBlank slower; the numbers are each command's own cost.
  static constexpr std::uint32_t kProfileQueries = 16384;
  VkQueryPool profilePool = VK_NULL_HANDLE;
  bool profiling = false, profileOpen = false;
  std::uint32_t profileNext = 0;
  std::vector<std::string> profileLabels;  // label of query pair (2i, 2i + 1)
  void prof_begin(const std::string &label) {
    if (!profiling || profileOpen || profileNext + 2u > kProfileQueries) return;
    if (renderPassOpen) {
      fragment_barrier();
    } else {
      full_barrier();
    }
    gt4_hwd_vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, profilePool, profileNext);
    profileLabels.push_back(label);
    profileOpen = true;
  }
  void prof_end() {
    if (!profileOpen) return;
    gt4_hwd_vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, profilePool, profileNext + 1u);
    profileNext += 2u;
    profileOpen = false;
  }

  // IN-420: draw batches recorded later, in one render pass. Core 2 recorded each batch at once, so every texture
  // decode, dirty patch or GLM upload between two batches ended the render pass (250-310 render pass begins per night
  // VBlank, a fixed GPU cost each). Now a batch's commands wait here, and compute and transfer work that does not depend
  // on the waiting batches is recorded before them; work that does records them first, as do submissions, host reads
  // and the rare passes. GT4_CORE2_DEFER_DRAWS=0 records each batch at once (A/B; needs IN-419).
  struct DeferredCommand {
    VkRect2D scissor{};
    VkPipeline pipeline = VK_NULL_HANDLE;
    std::uint32_t first = 0, count = 0;
    bool barrierFirst = false;  // without the pixel interlock every primitive waits for the one before
  };
  struct DeferredDraw {
    VkDescriptorSet set = VK_NULL_HANDLE;
    std::uint32_t uniformOffset = 0;
    VkDeviceSize vertexOffset = 0;
    VkViewport viewport{};
    std::uint32_t push[8] = {};
    bool textureFromTarget = false;
    std::uint32_t firstCommand = 0, commandCount = 0;
    std::string profileLabel;  // S3: only while profiling
  };
  bool deferDraws = lazyBarriers && (std::getenv("GT4_CORE2_DEFER_DRAWS") == nullptr ||
                                     std::strcmp(std::getenv("GT4_CORE2_DEFER_DRAWS"), "0") != 0);
  std::vector<DeferredDraw> deferred;
  std::vector<DeferredCommand> deferredCommands;
  struct FillRect { std::int32_t x0, y0, x1, y1; };  // IN-442 constant fills: output pixels [x0, x1) x [y0, y1)
  std::vector<FillRect> fillRects;                    // IN-447: reused by every constant-fill batch
  AccessSet drawReads, drawWrites;  // what the deferred batches read and write

  void begin_draw_pass() {
    VkRenderPassBeginInfo begin{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    begin.renderPass = renderPass;
    begin.framebuffer = framebuffer;
    begin.renderArea = {{0, 0}, {framebufferWidth, framebufferHeight}};
    gt4_hwd_vkCmdBeginRenderPass(cmd, &begin, VK_SUBPASS_CONTENTS_INLINE);
    stats.renderPasses++;
    renderPassOpen = true;
  }

  // One batch's commands, inside the open render pass; `first` is the pass's first batch (no barrier before it).
  void record_draw(const DeferredDraw &draw, bool first) {
    prof_begin(draw.profileLabel);
    if (!first && drewSinceBarrier && (!interlock || draw.textureFromTarget)) {
      // A texture taken from a target reads pixels other than its own (no interlock order); without pixel interlock
      // every batch waits for the one before.
      fragment_barrier();
    }
    gt4_hwd_vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, drawLayout, 0, 1, &draw.set, 1,
                                    &draw.uniformOffset);
    gt4_hwd_vkCmdBindVertexBuffers(cmd, 0, 1, &vertices.buffer, &draw.vertexOffset);
    gt4_hwd_vkCmdSetViewport(cmd, 0, 1, &draw.viewport);
    gt4_hwd_vkCmdPushConstants(cmd, drawLayout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                               sizeof draw.push, draw.push);
    for (std::uint32_t i = 0; i < draw.commandCount; ++i) {
      const DeferredCommand &command = deferredCommands[draw.firstCommand + i];
      if (command.barrierFirst && drewSinceBarrier) fragment_barrier();
      gt4_hwd_vkCmdSetScissor(cmd, 0, 1, &command.scissor);
      gt4_hwd_vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, command.pipeline);
      gt4_hwd_vkCmdDraw(cmd, command.count, 1, command.first, 0);
      stats.draws++;
      drewSinceBarrier = true;
    }
    prof_end();
  }

  // Records the deferred batches in one render pass, ordered against the pending compute and transfer accesses.
  void flush_draws() {
    if (deferred.empty()) return;
    bool hazard = false;
    for (std::size_t i = 0; i < drawReads.live; ++i) {
      const AccessSet::Entry &entry = drawReads.entries[i];
      if (entry.whole) hazard = hazard || pendingWrites.conflicts(entry.serial, kWholeImage);
      for (std::uint32_t row : entry.rows) hazard = hazard || pendingWrites.conflicts(entry.serial, row);
    }
    for (std::size_t i = 0; i < drawWrites.live; ++i) {
      const AccessSet::Entry &entry = drawWrites.entries[i];
      if (entry.whole) {
        hazard = hazard || pendingWrites.conflicts(entry.serial, kWholeImage) ||
                 pendingReads.conflicts(entry.serial, kWholeImage);
      }
      for (std::uint32_t row : entry.rows) {
        hazard = hazard || pendingWrites.conflicts(entry.serial, row) || pendingReads.conflicts(entry.serial, row);
      }
    }
    if (hazard) {
      full_barrier();
    } else {
      stats.barriersSkipped++;
    }
    const std::uint32_t pair = slots[slot].passTimestamps;
    const bool timed = timestamps && pair < kPassTimestampPairs;
    const std::uint32_t query = slot * kTimestampsPerSlot + 2u + 2u * pair;
    if (timed) gt4_hwd_vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, timestamps, query);
    begin_draw_pass();
    for (std::size_t i = 0; i < deferred.size(); ++i) record_draw(deferred[i], i == 0);
    end_render_pass();
    if (timed) {
      gt4_hwd_vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, timestamps, query + 1u);
      slots[slot].passTimestamps = pair + 1u;
    }
    pendingReads.merge(drawReads);
    pendingWrites.merge(drawWrites);
    drawReads.clear();
    drawWrites.clear();
    deferred.clear();
    deferredCommands.clear();
    stats.drawFlushes++;
  }

  // Inside the render pass (the subpass self-dependency): later fragments see earlier fragment stores.
  void fragment_barrier() {
    stats.fragmentBarriers++;
    VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    gt4_hw_vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0,
                                1, &barrier, 0, nullptr, 0, nullptr);
    drewSinceBarrier = false;
  }

  void end_render_pass() {
    if (!renderPassOpen) return;
    gt4_hwd_vkCmdEndRenderPass(cmd);
    renderPassOpen = false;
  }

  // Leaves the render pass; without IN-419 also orders everything before against everything after (compute,
  // transfer). With it, the command's touch() decides.
  void outside_pass() {
    end_render_pass();
    if (!lazyBarriers) full_barrier();
  }

  bool wait_slot(std::uint32_t index, std::string &error) {
    Slot &s = slots[index];
    if (!s.inFlight) return true;
    const auto waitStart = std::chrono::steady_clock::now();
    if (!check(gt4_hw_vkWaitForFences(device, 1, &s.fence, VK_TRUE, UINT64_MAX), "vkWaitForFences", error)) return false;
    stats.waitNs += static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - waitStart).count());
    stats.waits++;
    if (timestamps) {
      std::uint64_t ticks[2] = {};
      if (gt4_hwd_vkGetQueryPoolResults(device, timestamps, index * kTimestampsPerSlot, 2, sizeof ticks, ticks,
                                        sizeof(std::uint64_t), VK_QUERY_RESULT_64_BIT) == VK_SUCCESS &&
          ticks[1] >= ticks[0]) {
        stats.gpuNs += static_cast<std::uint64_t>(static_cast<double>(ticks[1] - ticks[0]) * timestampNs);
        stats.timedSubmissions++;
      }
      std::uint64_t pass[2 * kPassTimestampPairs] = {};
      if (s.passTimestamps != 0 &&
          gt4_hwd_vkGetQueryPoolResults(device, timestamps, index * kTimestampsPerSlot + 2u, 2u * s.passTimestamps,
                                        sizeof pass, pass, sizeof(std::uint64_t), VK_QUERY_RESULT_64_BIT) == VK_SUCCESS) {
        for (std::uint32_t i = 0; i < s.passTimestamps; ++i) {
          if (pass[2 * i + 1] >= pass[2 * i]) {
            stats.drawPassNs += static_cast<std::uint64_t>(static_cast<double>(pass[2 * i + 1] - pass[2 * i]) * timestampNs);
          }
        }
      }
      s.passTimestamps = 0;
    }
    if (statistics) {
      std::uint64_t counts[3] = {};
      if (gt4_hwd_vkGetQueryPoolResults(device, statistics, index, 1, sizeof counts, counts, sizeof counts,
                                        VK_QUERY_RESULT_64_BIT) == VK_SUCCESS) {
        stats.primitives += counts[0];
        stats.fragments += counts[1];
        stats.computeInvocations += counts[2];
      }
    }
    for (Image &image : s.doomed) retire(image);
    s.doomed.clear();
    s.inFlight = false;
    return true;
  }

  // Waits for everything submitted (a fence signals after every earlier submission on the queue too).
  bool wait_all(std::string &error) {
    for (std::uint32_t i = 0; i < slotCount; ++i) {
      if (!wait_slot(i, error)) return false;
    }
    return true;
  }

  bool begin_recording(std::string &error) {
    if (recording) return true;
    slot = (lastSubmitted + 1u) % slotCount;
    if (!wait_slot(slot, error)) return false;  // the submission before the last one: usually long done
    cmd = slots[slot].cmd;
    fence = slots[slot].fence;
    transientPool = slots[slot].pool;
    if (!check(gt4_hw_vkResetCommandBuffer(cmd, 0), "vkResetCommandBuffer", error)) return false;
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (!check(gt4_hw_vkBeginCommandBuffer(cmd, &begin), "vkBeginCommandBuffer", error)) return false;
    if (!check(gt4_hwd_vkResetDescriptorPool(device, transientPool, 0), "vkResetDescriptorPool", error)) return false;
    if (timestamps) {
      gt4_hwd_vkCmdResetQueryPool(cmd, timestamps, slot * kTimestampsPerSlot, kTimestampsPerSlot);
      gt4_hwd_vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, timestamps, slot * kTimestampsPerSlot);
      slots[slot].passTimestamps = 0;
    }
    if (statistics) {
      gt4_hwd_vkCmdResetQueryPool(cmd, statistics, slot, 1);
      gt4_hwd_vkCmdBeginQuery(cmd, statistics, slot, 0);
    }
    transientSetsUsed = 0;
    drawSets.clear();
    for (Buffer *buffer : {&staging, &vertices, &uniforms}) {
      buffer->used = 0;
      buffer->base = slot * ring_share(*buffer);
    }
    readback.used = 0;  // the readback buffer is only used by waited submissions
    readback.base = 0;
    recording = true;
    // A barrier's first scope covers every command submitted before it on this queue (the presentation's copy of
    // the display image included), so the new recording never overwrites what an earlier submission still reads.
    full_barrier();
    return true;
  }

  bool submit(bool wait, std::string &error) {
    if (!recording) {
      return wait ? wait_all(error) : true;
    }
    flush_draws();
    end_render_pass();
    if (timestamps) {
      gt4_hwd_vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, timestamps, slot * kTimestampsPerSlot + 1u);
    }
    if (statistics) gt4_hwd_vkCmdEndQuery(cmd, statistics, slot);
    if (!check(gt4_hw_vkEndCommandBuffer(cmd), "vkEndCommandBuffer", error)) return false;
    recording = false;
    if (!check(gt4_hw_vkResetFences(device, 1, &fence), "vkResetFences", error)) return false;
    VkSubmitInfo info{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    info.commandBufferCount = 1;
    info.pCommandBuffers = &cmd;
    if (!check(gt4_hw_vkQueueSubmit(queue, 1, &info, fence), "vkQueueSubmit", error)) return false;
    stats.submissions++;
    batchesSinceSubmit = 0;
    slots[slot].inFlight = true;
    slots[slot].serial = ++submitCount;
    slots[slot].doomed.insert(slots[slot].doomed.end(), doomed.begin(), doomed.end());
    doomed.clear();
    lastSubmitted = slot;
    return wait ? wait_all(error) : true;
  }

  bool allocate_set(VkDescriptorSetLayout layout, VkDescriptorSet &set, std::string &error) {
    if (transientSetsUsed >= kTransientSets) {
      if (!submit(false, error) || !begin_recording(error)) return false;
    }
    VkDescriptorSetAllocateInfo allocate{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    allocate.descriptorPool = transientPool;
    allocate.descriptorSetCount = 1;
    allocate.pSetLayouts = &layout;
    if (!check(gt4_hwd_vkAllocateDescriptorSets(device, &allocate, &set), "vkAllocateDescriptorSets", error)) return false;
    transientSetsUsed++;
    return true;
  }

  const Image *image_of(ImageId id) const {
    if (id < 0 || static_cast<std::size_t>(id) >= images.size() || !images[static_cast<std::size_t>(id)].alive) {
      return nullptr;
    }
    return &images[static_cast<std::size_t>(id)];
  }

  // A compute set: binding 0 a storage buffer range, bindings 1..3 storage images.
  bool compute_set(VkBuffer buffer, VkDeviceSize offset, VkDeviceSize range, VkImageView a, VkImageView b,
                   VkImageView c, VkDescriptorSet &set, std::string &error) {
    if (!allocate_set(computeSetLayout, set, error)) return false;
    VkDescriptorBufferInfo bufferInfo{buffer, offset, range};
    VkDescriptorImageInfo imageInfo[3] = {{VK_NULL_HANDLE, a, VK_IMAGE_LAYOUT_GENERAL},
                                          {VK_NULL_HANDLE, b, VK_IMAGE_LAYOUT_GENERAL},
                                          {VK_NULL_HANDLE, c, VK_IMAGE_LAYOUT_GENERAL}};
    VkWriteDescriptorSet writes[4]{};
    for (std::uint32_t i = 0; i < 4; ++i) {
      writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
      writes[i].dstSet = set;
      writes[i].dstBinding = i;
      writes[i].descriptorCount = 1;
      if (i == 0) {
        writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[i].pBufferInfo = &bufferInfo;
      } else {
        writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        writes[i].pImageInfo = &imageInfo[i - 1];
      }
    }
    gt4_hwd_vkUpdateDescriptorSets(device, 4, writes, 0, nullptr);
    return true;
  }

  void dispatch(VkPipeline pipeline, VkDescriptorSet set, const std::uint32_t *push, std::uint32_t pushWords,
                std::uint32_t width, std::uint32_t height) {
    gt4_hwd_vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
    gt4_hwd_vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, computeLayout, 0, 1, &set, 0, nullptr);
    gt4_hwd_vkCmdPushConstants(cmd, computeLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, pushWords * 4u, push);
    gt4_hwd_vkCmdDispatch(cmd, (width + 7u) / 8u, (height + 7u) / 8u, 1);
    after_op();
  }
};

Gpu::Gpu() : impl_(std::make_unique<Impl>()) {}

Gpu::~Gpu() {
  Impl &s = *impl_;
  if (s.specThread.joinable()) {  // IN-437: the worker may be compiling against the device
    {
      std::lock_guard<std::mutex> lock(s.specMutex);
      s.specStop = true;
    }
    s.specWake.notify_all();
    s.specThread.join();
  }
  if (!s.device) return;
  std::string ignored;
  s.submit(true, ignored);
  for (VkPipeline pipeline : s.specAll) gt4_hwd_vkDestroyPipeline(s.device, pipeline, nullptr);
  for (Image &image : s.images) s.release(image);
  for (Image &image : s.doomed) s.release(image);
  for (Image &image : s.recycled) s.release(image);
  s.release(s.atlas);
  s.release(s.display);
  for (Image &image : s.deinterlaceHistory) s.release(image);
  s.release(s.deinterlaceWoven);
  s.release(s.deinterlaceOutput);
  s.release(s.fxaa);
  s.release(s.presentA);
  s.release(s.presentB);
  s.release(s.presentOut);
  if (s.timestamps) gt4_hwd_vkDestroyQueryPool(s.device, s.timestamps, nullptr);
  if (s.statistics) gt4_hwd_vkDestroyQueryPool(s.device, s.statistics, nullptr);
  for (Impl::MemoryBlock &block : s.imageBlocks) gt4_hwd_vkFreeMemory(s.device, block.memory, nullptr);
  // The remaining objects live as long as the device (the renderer is destroyed with the backend).
}

bool Gpu::Ready() const { return impl_->ready; }

bool Gpu::Integrated() const { return impl_->integrated; }
std::uint32_t Gpu::Scale() const { return impl_->scale; }

std::uint32_t Gpu::MaxNativeSide() const {
  return std::min<std::uint32_t>(2048u, impl_->limits.maxImageDimension2D / std::max<std::uint32_t>(impl_->scale, 1u));
}

const Core2GpuStats &Gpu::Stats() const { return impl_->stats; }

bool Gpu::Init(const gpu::VulkanHandles &handles, std::uint32_t scale, float shift, std::string &error) {
  Impl &s = *impl_;
  if (s.ready) return true;
  s.instance = static_cast<VkInstance>(handles.instance);
  s.physical = static_cast<VkPhysicalDevice>(handles.physical);
  s.device = static_cast<VkDevice>(handles.device);
  s.queue = static_cast<VkQueue>(handles.queue);
  s.scale = std::max<std::uint32_t>(scale, 1u);
  s.shift = shift;
  if (!s.instance || !s.physical || !s.device || !s.queue) {
    error = "CORE2 needs instance, physical device, device and queue";
    return false;
  }
  std::vector<std::string> missing;
  if (!gt4::hw::vulkan_device::load_device_entry_points(s.instance, missing)) {
    error = "missing Vulkan entry points:";
    for (const std::string &name : missing) error += " " + name;
    return false;
  }
  gt4_hwd_vkGetPhysicalDeviceMemoryProperties(s.physical, &s.memoryProperties);
  VkPhysicalDeviceProperties properties{};
  gt4_hw_vkGetPhysicalDeviceProperties(s.physical, &properties);
  s.limits = properties.limits;
  s.integrated = properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU;

  VkCommandPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
  poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
  poolInfo.queueFamilyIndex = handles.queueFamily;
  if (!check(gt4_hw_vkCreateCommandPool(s.device, &poolInfo, nullptr, &s.pool), "vkCreateCommandPool", error)) return false;
  VkCommandBufferAllocateInfo allocate{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
  allocate.commandPool = s.pool;
  allocate.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  allocate.commandBufferCount = 1;
  for (Impl::Slot &slot : s.slots) {
    if (!check(gt4_hw_vkAllocateCommandBuffers(s.device, &allocate, &slot.cmd), "vkAllocateCommandBuffers", error)) {
      return false;
    }
    VkFenceCreateInfo fenceInfo{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    if (!check(gt4_hw_vkCreateFence(s.device, &fenceInfo, nullptr, &slot.fence), "vkCreateFence", error)) return false;
  }
  s.lastSubmitted = s.slotCount - 1u;  // S4: the first recording takes slot 0
  if (s.limits.timestampComputeAndGraphics && s.limits.timestampPeriod > 0.0f) {
    VkQueryPoolCreateInfo queryInfo{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
    queryInfo.queryType = VK_QUERY_TYPE_TIMESTAMP;
    queryInfo.queryCount = Impl::kMaxSlots * Impl::kTimestampsPerSlot;
    if (gt4_hwd_vkCreateQueryPool(s.device, &queryInfo, nullptr, &s.timestamps) == VK_SUCCESS) {
      s.timestampNs = s.limits.timestampPeriod;
    }
  }
  {
    VkPhysicalDeviceFeatures2 features{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    gt4_hw_vkGetPhysicalDeviceFeatures2(s.physical, &features);
    if (features.features.pipelineStatisticsQuery) {
      VkQueryPoolCreateInfo queryInfo{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
      queryInfo.queryType = VK_QUERY_TYPE_PIPELINE_STATISTICS;
      queryInfo.queryCount = Impl::kMaxSlots;
      queryInfo.pipelineStatistics = VK_QUERY_PIPELINE_STATISTIC_INPUT_ASSEMBLY_PRIMITIVES_BIT |
                                     VK_QUERY_PIPELINE_STATISTIC_FRAGMENT_SHADER_INVOCATIONS_BIT |
                                     VK_QUERY_PIPELINE_STATISTIC_COMPUTE_SHADER_INVOCATIONS_BIT;
      if (gt4_hwd_vkCreateQueryPool(s.device, &queryInfo, nullptr, &s.statistics) != VK_SUCCESS) {
        s.statistics = VK_NULL_HANDLE;
      }
    }
  }

  const VkBufferUsageFlags stagingUsage =
      VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
  if (!s.make_buffer(s.staging, kStagingBytes, stagingUsage, 0, error)) return false;
  if (!s.make_buffer(s.vertices, kVertexBytes, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, 0, error)) return false;
  if (!s.make_buffer(s.uniforms, kUniformBytes, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, 0, error)) return false;
  if (!s.make_buffer(s.readback, kReadbackBytes, stagingUsage, VK_MEMORY_PROPERTY_HOST_CACHED_BIT, error)) return false;
  if (!s.make_buffer(s.later, static_cast<VkDeviceSize>(Impl::kLaterSlots) * Impl::kLaterWords * 4u, stagingUsage,
                     VK_MEMORY_PROPERTY_HOST_CACHED_BIT, error)) return false;

  // Draw set (hw_pixel_uber.frag): 0 pixel state, 1 frame, 2 depth, 3 1x texel atlas, 4 a target sampled at scale,
  // 5 the depth image again (IN-451: the speculative depth read, a descriptor of its own).
  {
    VkDescriptorSetLayoutBinding bindings[6]{};
    bindings[0] = {0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
    for (std::uint32_t i = 1; i < 6; ++i) {
      bindings[i] = {i, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
    }
    VkDescriptorSetLayoutCreateInfo info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    info.bindingCount = 6;
    info.pBindings = bindings;
    if (!check(gt4_hwd_vkCreateDescriptorSetLayout(s.device, &info, nullptr, &s.drawSetLayout),
               "vkCreateDescriptorSetLayout", error)) return false;
    VkPushConstantRange push{VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, kGraphicsPushBytes};
    VkPipelineLayoutCreateInfo layout{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    layout.setLayoutCount = 1;
    layout.pSetLayouts = &s.drawSetLayout;
    layout.pushConstantRangeCount = 1;
    layout.pPushConstantRanges = &push;
    if (!check(gt4_hwd_vkCreatePipelineLayout(s.device, &layout, nullptr, &s.drawLayout), "vkCreatePipelineLayout",
               error)) return false;
  }
  // Compute set: 0 a storage buffer, 1..3 storage images.
  {
    VkDescriptorSetLayoutBinding bindings[4]{};
    bindings[0] = {0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    for (std::uint32_t i = 1; i < 4; ++i) {
      bindings[i] = {i, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    }
    VkDescriptorSetLayoutCreateInfo info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    info.bindingCount = 4;
    info.pBindings = bindings;
    if (!check(gt4_hwd_vkCreateDescriptorSetLayout(s.device, &info, nullptr, &s.computeSetLayout),
               "vkCreateDescriptorSetLayout", error)) return false;
    VkPushConstantRange push{VK_SHADER_STAGE_COMPUTE_BIT, 0, kComputePushBytes};
    VkPipelineLayoutCreateInfo layout{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    layout.setLayoutCount = 1;
    layout.pSetLayouts = &s.computeSetLayout;
    layout.pushConstantRangeCount = 1;
    layout.pPushConstantRanges = &push;
    if (!check(gt4_hwd_vkCreatePipelineLayout(s.device, &layout, nullptr, &s.computeLayout), "vkCreatePipelineLayout",
               error)) return false;
  }
  {
    VkDescriptorPoolSize sizes[3] = {{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, kTransientSets},
                                     {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 5 * kTransientSets + 64},
                                     {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, kTransientSets}};
    VkDescriptorPoolCreateInfo info{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    info.maxSets = kTransientSets;
    info.poolSizeCount = 3;
    info.pPoolSizes = sizes;
    for (Impl::Slot &slot : s.slots) {
      if (!check(gt4_hwd_vkCreateDescriptorPool(s.device, &info, nullptr, &slot.pool), "vkCreateDescriptorPool", error)) {
        return false;
      }
    }
  }
  // One subpass with no attachments; its self-dependency lets a draw see the storage-image stores of the draws before
  // it (the fragment barrier between batches and, without pixel interlock, between primitives).
  {
    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    VkSubpassDependency self{};
    self.srcSubpass = 0;
    self.dstSubpass = 0;
    self.srcStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    self.dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    self.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    self.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    VkRenderPassCreateInfo info{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    info.subpassCount = 1;
    info.pSubpasses = &subpass;
    info.dependencyCount = 1;
    info.pDependencies = &self;
    if (!check(gt4_hwd_vkCreateRenderPass(s.device, &info, nullptr, &s.renderPass), "vkCreateRenderPass", error)) {
      return false;
    }
    s.framebufferWidth = std::min<std::uint32_t>(s.limits.maxFramebufferWidth, 2048u * s.scale);
    s.framebufferHeight = std::min<std::uint32_t>(s.limits.maxFramebufferHeight, 2048u * s.scale);
    VkFramebufferCreateInfo fb{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
    fb.renderPass = s.renderPass;
    fb.width = s.framebufferWidth;
    fb.height = s.framebufferHeight;
    fb.layers = 1;
    if (!check(gt4_hwd_vkCreateFramebuffer(s.device, &fb, nullptr, &s.framebuffer), "vkCreateFramebuffer", error)) {
      return false;
    }
  }
  s.vertexModule = s.make_module(hwgs::shaders::hw_vertex_smooth_words, hwgs::shaders::hw_vertex_smooth_word_count, error);
  s.fragmentModule =
      s.make_module(hwgs::shaders::hw_pixel_uber_core2_words, hwgs::shaders::hw_pixel_uber_core2_word_count, error);
  if (!s.vertexModule || !s.fragmentModule) return false;
  s.pipelines[0] = s.make_graphics(VK_PRIMITIVE_TOPOLOGY_POINT_LIST, s.fragmentModule, error);
  s.pipelines[1] = s.make_graphics(VK_PRIMITIVE_TOPOLOGY_LINE_LIST, s.fragmentModule, error);
  s.pipelines[2] = s.make_graphics(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, s.fragmentModule, error);
  if (!s.pipelines[0] || !s.pipelines[1] || !s.pipelines[2]) return false;
  if (handles.pixelInterlock) {
    s.interlockModule = s.make_module(hwgs::shaders::hw_pixel_uber_core2_interlock_words,
                                      hwgs::shaders::hw_pixel_uber_core2_interlock_word_count, error);
    if (!s.interlockModule) return false;
    s.interlockTriangles = s.make_graphics(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, s.interlockModule, error);
    if (!s.interlockTriangles) return false;
    s.interlock = true;
  } else {
    s.specMode = Impl::SpecMode::off;  // IN-437: only the interlocked triangle pipeline is specialised
  }
  s.patchModule = s.make_module(hwgs::shaders::core2_patch_words, hwgs::shaders::core2_patch_word_count, error);
  s.reduceModule = s.make_module(hwgs::shaders::core2_reduce_words, hwgs::shaders::core2_reduce_word_count, error);
  s.toneModule = s.make_module(hwgs::shaders::core2_tone_words, hwgs::shaders::core2_tone_word_count, error);
  s.composeModule = s.make_module(hwgs::shaders::core2_compose_words, hwgs::shaders::core2_compose_word_count, error);
  if (!s.patchModule || !s.reduceModule || !s.toneModule || !s.composeModule) return false;
  s.patchPipeline = s.make_compute(s.patchModule, error);
  s.reducePipeline = s.make_compute(s.reduceModule, error);
  s.tonePipeline = s.make_compute(s.toneModule, error);
  s.composePipeline = s.make_compute(s.composeModule, error);
  if (!s.patchPipeline || !s.reducePipeline || !s.tonePipeline || !s.composePipeline) return false;
  s.fillModule = s.make_module(hwgs::shaders::core2_fill_words, hwgs::shaders::core2_fill_word_count, error);
  if (!s.fillModule) return false;
  s.fillPipeline = s.make_compute(s.fillModule, error);
  if (!s.fillPipeline) return false;
  {
    const std::uint32_t deinterlaceCounts[7] = {gpu::kGpuDeinterlaceHistory, 0, 0, 0, 1, 1, 1};
    const std::uint32_t fxaaCounts[2] = {1, 1};
    if (!s.image_layouts(deinterlaceCounts, 7, 14 * 4, s.deinterlaceSetLayout, s.deinterlaceLayout, error) ||
        !s.image_layouts(fxaaCounts, 2, 2 * 4, s.fxaaSetLayout, s.fxaaLayout, error)) {
      return false;
    }
    s.deinterlaceModule = s.make_module(hwgs::shaders::hw_deinterlace_scaled_words,
                                        hwgs::shaders::hw_deinterlace_scaled_word_count, error);
    s.fxaaModule = s.make_module(hwgs::shaders::hw_postfx_fxaa_words, hwgs::shaders::hw_postfx_fxaa_word_count, error);
    if (!s.deinterlaceModule || !s.fxaaModule) return false;
    s.deinterlacePipeline = s.make_compute(s.deinterlaceModule, error, s.deinterlaceLayout);
    s.fxaaPipeline = s.make_compute(s.fxaaModule, error, s.fxaaLayout);
    if (!s.deinterlacePipeline || !s.fxaaPipeline) return false;
    if (!s.image_layouts(fxaaCounts, 2, 6 * 4, s.presentSetLayout, s.presentLayout, error)) return false;
    s.presentModule = s.make_module(hwgs::shaders::core2_present_scale_words,
                                    hwgs::shaders::core2_present_scale_word_count, error);
    if (!s.presentModule) return false;
    s.presentPipeline = s.make_compute(s.presentModule, error, s.presentLayout);
    if (!s.presentPipeline) return false;
  }

  // ---- GLM and the decode kernel ----
  {
    VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    info.size = 4u << 20;
    info.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (!check(gt4_hwd_vkCreateBuffer(s.device, &info, nullptr, &s.glm), "vkCreateBuffer", error)) return false;
    VkMemoryRequirements requirements{};
    gt4_hwd_vkGetBufferMemoryRequirements(s.device, s.glm, &requirements);
    VkMemoryAllocateInfo allocateGlm{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    allocateGlm.allocationSize = requirements.size;
    if (!s.memory_type(requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0,
                       allocateGlm.memoryTypeIndex)) {
      error = "no device-local memory type for the CORE2 GLM";
      return false;
    }
    if (!check(gt4_hwd_vkAllocateMemory(s.device, &allocateGlm, nullptr, &s.glmMemory), "vkAllocateMemory", error) ||
        !check(gt4_hwd_vkBindBufferMemory(s.device, s.glm, s.glmMemory, 0), "vkBindBufferMemory", error)) {
      return false;
    }
    const memory::GpuAddressTables tables = memory::gpuAddressTables();
    std::vector<std::uint32_t> words(tables.directory);
    words.insert(words.end(), tables.entries.begin(), tables.entries.end());
    s.glmTableBytes = words.size() * sizeof(std::uint32_t);
    if (!s.make_buffer(s.glmTables, s.glmTableBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, 0, error)) return false;
    std::memcpy(s.glmTables.mapped, words.data(), static_cast<std::size_t>(s.glmTableBytes));
    VkDescriptorSetLayoutBinding bindings[5]{};
    bindings[0] = {0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    bindings[1] = {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    bindings[2] = {2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    bindings[3] = {3, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    bindings[4] = {5, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    VkDescriptorSetLayoutCreateInfo setInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    setInfo.bindingCount = 5;
    setInfo.pBindings = bindings;
    if (!check(gt4_hwd_vkCreateDescriptorSetLayout(s.device, &setInfo, nullptr, &s.decodeSetLayout),
               "vkCreateDescriptorSetLayout", error)) return false;
    VkPushConstantRange push{VK_SHADER_STAGE_COMPUTE_BIT, 0, kComputePushBytes};
    VkPipelineLayoutCreateInfo layout{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    layout.setLayoutCount = 1;
    layout.pSetLayouts = &s.decodeSetLayout;
    layout.pushConstantRangeCount = 1;
    layout.pPushConstantRanges = &push;
    if (!check(gt4_hwd_vkCreatePipelineLayout(s.device, &layout, nullptr, &s.decodeLayout), "vkCreatePipelineLayout",
               error)) return false;
    s.decodeModule = s.make_module(hwgs::shaders::hw_glm_decode_words, hwgs::shaders::hw_glm_decode_word_count, error);
    if (!s.decodeModule) return false;
    VkComputePipelineCreateInfo pipeline{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    pipeline.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    pipeline.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    pipeline.stage.module = s.decodeModule;
    pipeline.stage.pName = "main";
    pipeline.layout = s.decodeLayout;
    if (!check(gt4_hwd_vkCreateComputePipelines(s.device, VK_NULL_HANDLE, 1, &pipeline, nullptr, &s.decodePipeline),
               "vkCreateComputePipelines", error)) return false;
    s.patchGlmModule =
        s.make_module(hwgs::shaders::core2_patch_glm_words, hwgs::shaders::core2_patch_glm_word_count, error);
    if (!s.patchGlmModule) return false;
    pipeline.stage.module = s.patchGlmModule;
    if (!check(gt4_hwd_vkCreateComputePipelines(s.device, VK_NULL_HANDLE, 1, &pipeline, nullptr, &s.patchGlmPipeline),
               "vkCreateComputePipelines", error)) return false;
    // IN-408: the target CLUT kernel shares the decode set (0 GLM, 1 tables, 2 inverse table, 3 target, 5 CLUT).
    s.clutTargetModule =
        s.make_module(hwgs::shaders::core2_clut_target_words, hwgs::shaders::core2_clut_target_word_count, error);
    if (!s.clutTargetModule) return false;
    pipeline.stage.module = s.clutTargetModule;
    if (!check(gt4_hwd_vkCreateComputePipelines(s.device, VK_NULL_HANDLE, 1, &pipeline, nullptr, &s.clutTargetPipeline),
               "vkCreateComputePipelines", error)) return false;
    s.materializeModule =
        s.make_module(hwgs::shaders::core2_materialize_words, hwgs::shaders::core2_materialize_word_count, error);
    if (!s.materializeModule) return false;
    pipeline.stage.module = s.materializeModule;
    if (!check(gt4_hwd_vkCreateComputePipelines(s.device, VK_NULL_HANDLE, 1, &pipeline, nullptr, &s.materializePipeline),
               "vkCreateComputePipelines", error)) return false;
  }
  // IN-408: the GPU CLUT and the inverse of the CT32 in-page layout (word of a page -> pixel x | y << 8).
  {
    VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    info.size = 512 * 4;
    info.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (!check(gt4_hwd_vkCreateBuffer(s.device, &info, nullptr, &s.clut), "vkCreateBuffer", error)) return false;
    VkMemoryRequirements requirements{};
    gt4_hwd_vkGetBufferMemoryRequirements(s.device, s.clut, &requirements);
    VkMemoryAllocateInfo allocateClut{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    allocateClut.allocationSize = requirements.size;
    if (!s.memory_type(requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0,
                       allocateClut.memoryTypeIndex)) {
      error = "no device-local memory type for the CORE2 GPU CLUT";
      return false;
    }
    if (!check(gt4_hwd_vkAllocateMemory(s.device, &allocateClut, nullptr, &s.clutMemory), "vkAllocateMemory", error) ||
        !check(gt4_hwd_vkBindBufferMemory(s.device, s.clut, s.clutMemory, 0), "vkBindBufferMemory", error)) {
      return false;
    }
    const memory::GpuAddressTables tables = memory::gpuAddressTables();
    const std::uint32_t tableOffset = tables.directory[0];  // PSMCT32 (code 0)
    const std::uint32_t pageWidth = tables.directory[1];
    const std::uint32_t pageHeight = tables.directory[2];
    if (!s.make_buffer(s.inverseCt32, 2048 * 4, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, 0, error)) return false;
    auto *inverse = static_cast<std::uint32_t *>(s.inverseCt32.mapped);
    for (std::uint32_t y = 0; y < pageHeight; ++y) {
      for (std::uint32_t x = 0; x < pageWidth; ++x) {
        const std::uint32_t entry = tables.entries[tableOffset + y * pageWidth + x];
        const std::uint32_t word = (entry & 31u) * 64u + ((entry >> 5) & 255u) / 4u;
        inverse[word & 2047u] = x | (y << 8);
      }
    }
  }

  if (!s.make_image(s.atlas, kAtlasSide, kAtlasSide, 1, error)) return false;
  if (!s.make_image(s.display, 640u * s.scale, 448u * s.scale, 1, error)) return false;
  s.ready = true;
  std::fprintf(stderr, "[core2] ready: scale %u, shift %.3f, pixel interlock %s, max native side %u\n", s.scale,
               static_cast<double>(s.shift), s.interlock ? "yes" : "no", MaxNativeSide());
  return true;
}

ImageId Gpu::CreateImage(std::uint32_t nativeWidth, std::uint32_t nativeHeight, bool scaled, std::string &error) {
  Impl &s = *impl_;
  Image image;
  if (!s.make_image(image, nativeWidth, nativeHeight, scaled ? s.scale : 1u, error)) return kNoImage;
  ImageId id;
  if (!s.freeIds.empty()) {
    id = s.freeIds.back();
    s.freeIds.pop_back();
    s.images[static_cast<std::size_t>(id)] = image;
  } else {
    id = static_cast<ImageId>(s.images.size());
    s.images.push_back(image);
  }
  return id;
}

bool Gpu::GrowImage(ImageId id, std::uint32_t nativeWidth, std::uint32_t nativeHeight, std::string &error) {
  Impl &s = *impl_;
  if (!s.image_of(id)) {
    error = "CORE2 grows an image that does not exist";
    return false;
  }
  Image &old = s.images[static_cast<std::size_t>(id)];
  if (nativeWidth <= old.nativeWidth && nativeHeight <= old.nativeHeight) return true;
  s.flush_draws();
  Image grown;
  if (!s.make_image(grown, std::max(nativeWidth, old.nativeWidth), std::max(nativeHeight, old.nativeHeight), old.scale,
                    error)) {
    return false;
  }
  VkImageCopy region{};
  region.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
  region.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
  region.extent = {old.width, old.height, 1};
  gt4_hwd_vkCmdCopyImage(s.cmd, old.image, VK_IMAGE_LAYOUT_GENERAL, grown.image, VK_IMAGE_LAYOUT_GENERAL, 1, &region);
  s.full_barrier();
  s.doom(old);
  old = grown;
  return true;
}

void Gpu::DestroyImage(ImageId id) {
  Impl &s = *impl_;
  if (!s.image_of(id)) return;
  s.doom(s.images[static_cast<std::size_t>(id)]);
  s.freeIds.push_back(id);
}

void Gpu::ImageSize(ImageId id, std::uint32_t &nativeWidth, std::uint32_t &nativeHeight) const {
  const Image *image = impl_->image_of(id);
  nativeWidth = image ? image->nativeWidth : 0;
  nativeHeight = image ? image->nativeHeight : 0;
}

bool Gpu::CopyImage(ImageId from, ImageId to, std::int32_t x0, std::int32_t y0, std::int32_t x1, std::int32_t y1,
                    std::string &error) {
  Impl &s = *impl_;
  const Image *source = s.image_of(from);
  const Image *destination = s.image_of(to);
  if (!source || !destination) {
    error = "CORE2 copies between images that do not exist";
    return false;
  }
  const std::uint32_t sc = source->scale;
  const std::int32_t right = std::min<std::int32_t>({x1 + 1, static_cast<std::int32_t>(source->nativeWidth),
                                                     static_cast<std::int32_t>(destination->nativeWidth)});
  const std::int32_t bottom = std::min<std::int32_t>({y1 + 1, static_cast<std::int32_t>(source->nativeHeight),
                                                      static_cast<std::int32_t>(destination->nativeHeight)});
  const std::int32_t left = std::max(x0, 0), top = std::max(y0, 0);
  if (right <= left || bottom <= top || source->scale != destination->scale) return true;
  if (!s.begin_recording(error)) return false;
  s.outside_pass();
  s.touch("copy image", {source}, {destination}, 0, 0);
  VkImageCopy region{};
  region.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
  region.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
  region.srcOffset = {left * static_cast<std::int32_t>(sc), top * static_cast<std::int32_t>(sc), 0};
  region.dstOffset = region.srcOffset;
  region.extent = {static_cast<std::uint32_t>(right - left) * sc, static_cast<std::uint32_t>(bottom - top) * sc, 1};
  gt4_hwd_vkCmdCopyImage(s.cmd, source->image, VK_IMAGE_LAYOUT_GENERAL, destination->image, VK_IMAGE_LAYOUT_GENERAL,
                         1, &region);
  s.after_op();
  return true;
}

bool Gpu::AllocateAtlas(std::uint32_t width, std::uint32_t height, std::uint32_t &x, std::uint32_t &y) {
  Impl &s = *impl_;
  if (width > kAtlasSide || height > kAtlasSide) return false;
  if (s.atlasShelfX + width > kAtlasSide) {
    s.atlasShelfY += s.atlasShelfHeight;
    s.atlasShelfX = 0;
    s.atlasShelfHeight = 0;
  }
  if (s.atlasShelfY + height > kAtlasSide) return false;
  x = s.atlasShelfX;
  y = s.atlasShelfY;
  s.atlasShelfX += width;
  s.atlasShelfHeight = std::max(s.atlasShelfHeight, height);
  return true;
}

void Gpu::ResetAtlas() {
  Impl &s = *impl_;
  s.atlasShelfX = s.atlasShelfY = s.atlasShelfHeight = 0;
  s.stats.atlasResets++;
}

bool Gpu::UploadAtlas(std::uint32_t x, std::uint32_t y, std::uint32_t width, std::uint32_t height,
                      const std::uint32_t *texels, std::string &error) {
  Impl &s = *impl_;
  if (!s.begin_recording(error)) return false;
  const VkDeviceSize bytes = static_cast<VkDeviceSize>(width) * height * 4u;
  VkDeviceSize offset = 0;
  if (!s.ring(s.staging, bytes, 16, offset, error)) return false;
  std::memcpy(static_cast<std::uint8_t *>(s.staging.mapped) + offset, texels, static_cast<std::size_t>(bytes));
  s.outside_pass();
  s.touch("upload atlas", {}, {&s.atlas}, 0, 0);
  VkBufferImageCopy region{};
  region.bufferOffset = offset;
  region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
  region.imageOffset = {static_cast<std::int32_t>(x), static_cast<std::int32_t>(y), 0};
  region.imageExtent = {width, height, 1};
  gt4_hwd_vkCmdCopyBufferToImage(s.cmd, s.staging.buffer, s.atlas.image, VK_IMAGE_LAYOUT_GENERAL, 1, &region);
  s.after_op();
  s.stats.atlasUploads++;
  return true;
}

bool Gpu::UploadImage(ImageId id, std::uint32_t width, std::uint32_t height, const std::uint32_t *texels,
                      std::string &error) {
  Impl &s = *impl_;
  const Image *image = s.image_of(id);
  if (!image || width > image->width || height > image->height) {
    error = "CORE2 texture upload outside its image";
    return false;
  }
  if (!s.begin_recording(error)) return false;
  const VkDeviceSize bytes = static_cast<VkDeviceSize>(width) * height * 4u;
  VkDeviceSize offset = 0;
  if (!s.ring(s.staging, bytes, 16, offset, error)) return false;
  std::memcpy(static_cast<std::uint8_t *>(s.staging.mapped) + offset, texels, static_cast<std::size_t>(bytes));
  s.outside_pass();
  s.touch("upload image", {}, {image}, 0, 0);
  VkBufferImageCopy region{};
  region.bufferOffset = offset;
  region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
  region.imageExtent = {width, height, 1};
  gt4_hwd_vkCmdCopyBufferToImage(s.cmd, s.staging.buffer, image->image, VK_IMAGE_LAYOUT_GENERAL, 1, &region);
  s.after_op();
  s.stats.atlasUploads++;
  return true;
}

bool Gpu::Draw(const DrawBatch &batch, std::string &error) {
  Impl &s = *impl_;
  const Image *frame = s.image_of(batch.frame);
  if (!frame || !batch.vertices || !batch.draws) {
    error = "CORE2 draw without a colour target";
    return false;
  }
  if (batch.draws->empty() || batch.vertices->empty()) return true;
  const Image *depth = batch.depth == kNoImage ? frame : s.image_of(batch.depth);
  const Image *texture = batch.texture == kNoImage ? &s.atlas : s.image_of(batch.texture);
  const Image *texels = batch.memoryTexture == kNoImage ? &s.atlas : s.image_of(batch.memoryTexture);
  // IN-409: an indexed memory texture's palette row image goes where a target texture would (binding 4).
  const bool paletted = batch.paletteRow >= 0 && batch.palettes != kNoImage;
  if (paletted) texture = s.image_of(batch.palettes);
  if (!depth || !texture || !texels) {
    error = "CORE2 draw names a depth or texture image that does not exist";
    return false;
  }
  if (!s.begin_recording(error)) return false;
  const VkDeviceSize vertexBytes = batch.vertices->size() * sizeof(gpu::GpuVertex);
  VkDeviceSize vertexOffset = 0, uniformOffset = 0;
  const VkDeviceSize uniformAlign = std::max<VkDeviceSize>(s.limits.minUniformBufferOffsetAlignment, 16);
  if (!s.ring(s.vertices, vertexBytes, sizeof(gpu::GpuVertex), vertexOffset, error)) return false;
  if (!s.ring(s.uniforms, kPixelStateBytes, uniformAlign, uniformOffset, error)) return false;
  if (!s.recording && !s.begin_recording(error)) return false;  // a ring may have submitted
  std::memcpy(static_cast<std::uint8_t *>(s.vertices.mapped) + vertexOffset, batch.vertices->data(),
              static_cast<std::size_t>(vertexBytes));
  gpu::GpuPixelState state = batch.state;
  state.fields[gpu::gpufield::TexOriginX] = batch.mipInfo;  // IN-428: the mip parameters (0 without mips)
  state.fields[gpu::gpufield::TexOriginY] = paletted ? static_cast<std::uint32_t>(batch.paletteRow) : 0u;
  if (paletted) state.fields[gpu::gpufield::TexelKind] = 3u;  // HW_TEXEL_INDEXED
  state.fields[gpu::gpufield::KeepTopByte] = 1u;
  std::memcpy(static_cast<std::uint8_t *>(s.uniforms.mapped) + uniformOffset, state.fields, sizeof state.fields);

  // IN-442: a constant fill as compute dispatches. Every vertex of the batch has the same colour and Z, every
  // primitive is an axis-aligned rectangle (a sprite as two triangles), and the rectangles of covered pixels, clipped
  // exactly as the draw's scissor clips them, are disjoint or repeated (a repeated rectangle of one constant fragment
  // writes what it already wrote: no blend, no DATE). Then each output pixel gets the fragment the uber shader would
  // give it (core2_fill.comp), with storage writes from compute, about twice as fast at high scales. The covered pixels
  // are those the rasteriser covers: hw_vertex.vert puts an edge at scale * X + scale_offset / 2 - 1/320 (no IN-388
  // snap in this core), and a pixel is covered when its centre lies on or right of the left edge and left of the right
  // one. The 1/320 bias keeps every edge at least 0.003 pixel from a pixel centre, more than the float rounding of the
  // edge, so this double-precision count matches the GPU's.
  if (batch.constantFill && s.computeFills && s.fillPipeline) {
    const std::vector<gpu::GpuVertex> &verts = *batch.vertices;
    const gpu::GpuVertex &v0 = verts[batch.draws->front().first];
    const std::uint32_t sc = s.scale;
    const std::int32_t limitX = static_cast<std::int32_t>(std::min(frame->width, s.framebufferWidth));
    const std::int32_t limitY = static_cast<std::int32_t>(std::min(frame->height, s.framebufferHeight));
    using Rect = Impl::FillRect;
    std::vector<Rect> &rects = s.fillRects;  // IN-447: reused across batches
    rects.clear();
    bool eligible = true;
    for (const gpu::GpuDraw &draw : *batch.draws) {
      if (draw.topology != gpu::GpuTopology::Triangles || draw.count == 0 || draw.first + draw.count > verts.size()) {
        eligible = false;
        break;
      }
      std::int32_t nx0 = INT32_MAX, ny0 = INT32_MAX, nx1 = INT32_MIN, ny1 = INT32_MIN;
      for (std::uint32_t i = draw.first; i < draw.first + draw.count && eligible; ++i) {
        const gpu::GpuVertex &v = verts[i];
        const std::int32_t xi = static_cast<std::int16_t>(v.x), yi = static_cast<std::int16_t>(v.y);
        eligible = std::memcmp(v.rgba, v0.rgba, 4) == 0 && v.z == v0.z;
        nx0 = std::min(nx0, xi);  // 12.4 units (the vertices already carry XYOFFSET)
        ny0 = std::min(ny0, yi);
        nx1 = std::max(nx1, xi);
        ny1 = std::max(ny1, yi);
      }
      for (std::uint32_t i = draw.first; i < draw.first + draw.count && eligible; ++i) {  // corners only
        const std::int32_t x = static_cast<std::int16_t>(verts[i].x), y = static_cast<std::int16_t>(verts[i].y);
        eligible = (x == nx0 || x == nx1) && (y == ny0 || y == ny1);
      }
      if (!eligible) break;
      // Covered output pixels [px0, px1) x [py0, py1): centres k + 0.5 in [edge0, edge1).
      auto edge = [&](std::int32_t coordinate) {
        return static_cast<double>(sc) * (coordinate / 16.0) + 0.5 * static_cast<double>(s.shift) - 1.0 / 320.0;
      };
      const std::int32_t px0 = static_cast<std::int32_t>(std::ceil(edge(nx0) - 0.5));
      const std::int32_t px1 = static_cast<std::int32_t>(std::ceil(edge(nx1) - 0.5));
      const std::int32_t py0 = static_cast<std::int32_t>(std::ceil(edge(ny0) - 0.5));
      const std::int32_t py1 = static_cast<std::int32_t>(std::ceil(edge(ny1) - 0.5));
      if (px1 <= px0 || py1 <= py0) continue;  // covers no pixel
      // The draw's scissor (the recording below): the scaled pass's own native rectangle when it has one.
      const bool own = draw.scaledX1 >= draw.scaledX0 && draw.scaledY1 >= draw.scaledY0;
      const std::int32_t cx0 = own ? draw.scaledX0 : draw.x0, cy0 = own ? draw.scaledY0 : draw.y0;
      const std::int32_t cx1 = own ? draw.scaledX1 : draw.x1, cy1 = own ? draw.scaledY1 : draw.y1;
      const std::int32_t s32 = static_cast<std::int32_t>(sc);
      Rect r{std::max({px0, std::clamp(cx0 * s32, 0, limitX), 0}), std::max({py0, std::clamp(cy0 * s32, 0, limitY), 0}),
             std::min(px1, std::clamp((cx1 + 1) * s32, 0, limitX)), std::min(py1, std::clamp((cy1 + 1) * s32, 0, limitY))};
      if (r.x1 <= r.x0 || r.y1 <= r.y0) continue;
      bool repeated = false;
      for (const Rect &o : rects) {
        if (o.x0 == r.x0 && o.y0 == r.y0 && o.x1 == r.x1 && o.y1 == r.y1) {
          repeated = true;
        } else if (r.x0 < o.x1 && o.x0 < r.x1 && r.y0 < o.y1 && o.y0 < r.y1) {
          eligible = false;  // partly overlapping rectangles: drawn in order by the rasteriser
        }
      }
      if (!eligible) break;
      if (!repeated) rects.push_back(r);
    }
    if (eligible) {
      VkDeviceSize stateOffset = 0;
      const VkDeviceSize align = std::max<VkDeviceSize>(s.limits.minStorageBufferOffsetAlignment, 16);
      if (!s.ring(s.staging, sizeof state.fields, align, stateOffset, error)) return false;
      if (!s.recording && !s.begin_recording(error)) return false;
      std::memcpy(static_cast<std::uint8_t *>(s.staging.mapped) + stateOffset, state.fields, sizeof state.fields);
      s.outside_pass();
      s.touch("fill", {frame, depth}, {frame, depth}, 0, 0);
      VkDescriptorSet fillSet = VK_NULL_HANDLE;
      if (!s.compute_set(s.staging.buffer, stateOffset, sizeof state.fields, frame->view, depth->view, frame->view,
                         fillSet, error)) {
        return false;
      }
      const std::uint32_t colour = static_cast<std::uint32_t>(v0.rgba[0]) | static_cast<std::uint32_t>(v0.rgba[1]) << 8 |
                                   static_cast<std::uint32_t>(v0.rgba[2]) << 16 | static_cast<std::uint32_t>(v0.rgba[3]) << 24;
      const std::uint32_t flags = sc > 1 ? 3u : 0u;  // the push word bits 28 and 30 of a scaled draw
      if (s.profiling) {  // diagnostics (S3 profile VBlank): the shape of each fill, for the clear-path survey
        namespace g = gpu::gpufield;
        const auto &f = state.fields;
        for (const Rect &r : rects) {
          std::fprintf(stderr,
                       "[core2] gpu profile fill rect=%d,%d-%d,%d frame=%ux%u depth=%ux%u same=%d format=%u fbmsk=%08x "
                       "dither=%u keep_top=%u ztest=%u ztst=%u zmsk=%u zformat=%u fba=%u colour=%08x z=%08x\n",
                       r.x0, r.y0, r.x1, r.y1, frame->width, frame->height, depth->width, depth->height,
                       frame == depth ? 1 : 0, f[g::Format], f[g::Fbmsk], f[g::Dither], f[g::KeepTopByte], f[g::Ztest],
                       f[g::Ztst], f[g::Zmsk], f[g::Zformat], f[g::Fba], colour, v0.z);
        }
      }
      for (const Rect &r : rects) {
        const std::uint32_t push[9] = {static_cast<std::uint32_t>(r.x0), static_cast<std::uint32_t>(r.y0),
                                       static_cast<std::uint32_t>(r.x1), static_cast<std::uint32_t>(r.y1), colour, v0.z,
                                       batch.zMax, flags, sc};
        // Disjoint rectangles: no barrier between them (dispatch orders nothing, touch ordered the batch).
        gt4_hwd_vkCmdBindPipeline(s.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, s.fillPipeline);
        gt4_hwd_vkCmdBindDescriptorSets(s.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, s.computeLayout, 0, 1, &fillSet, 0,
                                        nullptr);
        gt4_hwd_vkCmdPushConstants(s.cmd, s.computeLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof push, push);
        gt4_hwd_vkCmdDispatch(s.cmd, (static_cast<std::uint32_t>(r.x1 - r.x0) + 7u) / 8u,
                              (static_cast<std::uint32_t>(r.y1 - r.y0) + 7u) / 8u, 1);
        s.stats.fillPixels += static_cast<std::uint64_t>(r.x1 - r.x0) * static_cast<std::uint64_t>(r.y1 - r.y0);
      }
      s.after_op();
      s.stats.computeFills++;
      s.stats.batches++;
      return true;
    }
  }

  VkDescriptorSet set = VK_NULL_HANDLE;
  const DrawSetKey setKey{frame->serial, depth->serial, texels->serial, texture->serial};
  if (auto cached = s.drawSets.find(setKey); cached != s.drawSets.end()) {
    set = cached->second;
  } else {
    if (!s.allocate_set(s.drawSetLayout, set, error)) return false;
    s.drawSets[setKey] = set;
    VkDescriptorBufferInfo uniformInfo{s.uniforms.buffer, 0, kPixelStateBytes};
    VkDescriptorImageInfo imageInfo[5] = {{VK_NULL_HANDLE, frame->view, VK_IMAGE_LAYOUT_GENERAL},
                                          {VK_NULL_HANDLE, depth->view, VK_IMAGE_LAYOUT_GENERAL},
                                          {VK_NULL_HANDLE, texels->view, VK_IMAGE_LAYOUT_GENERAL},
                                          {VK_NULL_HANDLE, texture->view, VK_IMAGE_LAYOUT_GENERAL},
                                          {VK_NULL_HANDLE, depth->view, VK_IMAGE_LAYOUT_GENERAL}};  // IN-451
    VkWriteDescriptorSet writes[6]{};
    for (std::uint32_t i = 0; i < 6; ++i) {
      writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
      writes[i].dstSet = set;
      writes[i].dstBinding = i;
      writes[i].descriptorCount = 1;
      if (i == 0) {
        writes[i].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
        writes[i].pBufferInfo = &uniformInfo;
      } else {
        writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        writes[i].pImageInfo = &imageInfo[i - 1];
      }
    }
    gt4_hwd_vkUpdateDescriptorSets(s.device, 6, writes, 0, nullptr);
  }

  const std::uint32_t sc = s.scale;
  Impl::DeferredDraw record;
  record.set = set;
  record.uniformOffset = static_cast<std::uint32_t>(uniformOffset);
  record.vertexOffset = vertexOffset;
  record.viewport = VkViewport{0.0f, 0.0f, static_cast<float>(frame->width), static_cast<float>(frame->height), 0.0f, 1.0f};
  record.push[0] = FloatBits(static_cast<float>(frame->width));
  record.push[1] = FloatBits(static_cast<float>(frame->height));
  record.push[2] = FloatBits(static_cast<float>(sc));
  record.push[3] = FloatBits(s.shift);
  record.push[4] = 0;  // the batch vertices already carry XYOFFSET
  record.push[5] = 0;
  record.push[6] = batch.zMax;
  // tex_scale: s for a texture sampled from a target (binding 4) with bit 24 set (IN-427: at 1x the scale alone could
  // not tell it from the atlas), 1 for the atlas; at scale the fine dither (8.6) and the floored rasteriser depth (1.6)
  // are on.
  record.push[7] = (batch.texture != kNoImage ? (sc | 0x01000000u) : 1u) | (sc > 1 ? (0x40000000u | 0x10000000u) : 0u) |
                   (batch.mipInfo != 0 ? 0x00800000u : 0u) |  // IN-428: tex_origin_x holds the mip parameters
                   (batch.forceLinear ? 0x80000000u : 0u);    // IN-433: forced bilinear
  record.textureFromTarget = batch.texture != kNoImage;
  if (s.profiling) {
    namespace g = gpu::gpufield;
    const auto &f = batch.state.fields;
    char label[200];
    std::snprintf(label, sizeof label, "draw %ux%u tex=%s blend=%s z=%s%s ate=%u fbmsk=%s prims=%s",
                  frame->width, frame->height,
                  batch.texture != kNoImage ? "target" : batch.memoryTexture != kNoImage ? (paletted ? "indexed" : "memory") : "-",
                  f[g::BlendOn] ? (std::to_string(f[g::BlendA]) + std::to_string(f[g::BlendB]) + std::to_string(f[g::BlendC]) +
                                   std::to_string(f[g::BlendD])).c_str()
                                : "off",
                  f[g::Ztest] && f[g::Ztst] != 1u ? "test" : "-", f[g::Zmsk] == 0 ? "+write" : "", f[g::Ate],
                  f[g::Fbmsk] == 0 ? "0" : "set",
                  batch.draws->size() <= 4 ? "1-4" : batch.draws->size() <= 64 ? "5-64" : batch.draws->size() <= 1024 ? "65-1k" : "1k+");
    record.profileLabel = label;
  }
  const std::int32_t limitX = static_cast<std::int32_t>(std::min(frame->width, s.framebufferWidth));
  const std::int32_t limitY = static_cast<std::int32_t>(std::min(frame->height, s.framebufferHeight));
  // 8.3: the GS scissor and the primitive's box (one native pixel of slack) times s.
  auto scissorOf = [&](std::int32_t x0, std::int32_t y0, std::int32_t x1, std::int32_t y1, VkRect2D &out) {
    const std::int32_t left = std::clamp(x0 * static_cast<std::int32_t>(sc), 0, limitX);
    const std::int32_t top = std::clamp(y0 * static_cast<std::int32_t>(sc), 0, limitY);
    const std::int32_t right = std::clamp((x1 + 1) * static_cast<std::int32_t>(sc), 0, limitX);
    const std::int32_t bottom = std::clamp((y1 + 1) * static_cast<std::int32_t>(sc), 0, limitY);
    if (right <= left || bottom <= top) return false;
    out = {{left, top}, {static_cast<std::uint32_t>(right - left), static_cast<std::uint32_t>(bottom - top)}};
    return true;
  };
  auto boundsOf = [](const gpu::GpuDraw &draw, std::int32_t &x0, std::int32_t &y0, std::int32_t &x1, std::int32_t &y1) {
    const bool own = draw.scaledX1 >= draw.scaledX0 && draw.scaledY1 >= draw.scaledY0;
    x0 = own ? draw.scaledX0 : draw.x0;
    y0 = own ? draw.scaledY0 : draw.y0;
    x1 = own ? draw.scaledX1 : draw.x1;
    y1 = own ? draw.scaledY1 : draw.y1;
  };
  record.firstCommand = static_cast<std::uint32_t>(s.deferredCommands.size());
  const std::uint32_t vertexCount = static_cast<std::uint32_t>(batch.vertices->size());
  bool allTriangles = s.interlock;
  for (const gpu::GpuDraw &draw : *batch.draws) allTriangles = allTriangles && draw.topology == gpu::GpuTopology::Triangles;
  if (allTriangles) {
    std::int32_t x0 = INT32_MAX, y0 = INT32_MAX, x1 = INT32_MIN, y1 = INT32_MIN;
    std::uint32_t first = UINT32_MAX, end = 0;
    for (const gpu::GpuDraw &draw : *batch.draws) {
      if (draw.count == 0 || draw.first + draw.count > vertexCount) continue;
      std::int32_t a, b, c, d;
      boundsOf(draw, a, b, c, d);
      x0 = std::min(x0, a);
      y0 = std::min(y0, b);
      x1 = std::max(x1, c);
      y1 = std::max(y1, d);
      first = std::min(first, draw.first);
      end = std::max(end, draw.first + draw.count);
    }
    Impl::DeferredCommand command;
    if (end > first && scissorOf(x0, y0, x1, y1, command.scissor)) {
      command.pipeline = s.triangle_pipeline(state.fields, record.push[7]);  // IN-437
      command.first = first;
      command.count = end - first;
      s.deferredCommands.push_back(command);
    }
  } else {
    for (const gpu::GpuDraw &draw : *batch.draws) {
      if (draw.count == 0 || draw.first + draw.count > vertexCount) continue;
      std::int32_t a, b, c, d;
      boundsOf(draw, a, b, c, d);
      Impl::DeferredCommand command;
      if (!scissorOf(a, b, c, d, command.scissor)) continue;
      const std::size_t pipeline = draw.topology == gpu::GpuTopology::Points  ? 0
                                   : draw.topology == gpu::GpuTopology::Lines ? 1
                                                                              : 2;
      command.pipeline = s.pipelines[pipeline];
      command.first = draw.first;
      command.count = draw.count;
      command.barrierFirst = true;
      s.deferredCommands.push_back(command);
    }
  }
  record.commandCount = static_cast<std::uint32_t>(s.deferredCommands.size()) - record.firstCommand;

  // IN-419: the batch reads its textures (one palette row) and reads and writes its frame and depth.
  const Impl::Use textureUse = paletted ? Impl::Use(texture, static_cast<std::uint32_t>(batch.paletteRow)) : Impl::Use(texture);
  if (s.deferDraws) {
    // IN-420: recorded with the next render pass.
    for (const Impl::Use &use : {Impl::Use(texels), textureUse, Impl::Use(frame), Impl::Use(depth)}) {
      s.drawReads.add(use.serial, use.region);
    }
    for (const Impl::Use &use : {Impl::Use(frame), Impl::Use(depth)}) s.drawWrites.add(use.serial, use.region);
    s.deferred.push_back(record);
  } else {
    bool first = false;
    if (!s.renderPassOpen) {
      if (s.lazyBarriers) s.order({texels, textureUse, frame, depth}, {frame, depth}, 0, 0);
      s.begin_draw_pass();
      first = true;
    } else if (s.lazyBarriers) {
      for (const Impl::Use &use : {Impl::Use(texels), textureUse, Impl::Use(frame), Impl::Use(depth)}) {
        s.pendingReads.add(use.serial, use.region);
      }
      for (const Impl::Use &use : {Impl::Use(frame), Impl::Use(depth)}) s.pendingWrites.add(use.serial, use.region);
    }
    s.record_draw(record, first);
    s.deferredCommands.resize(record.firstCommand);
  }
  s.stats.batches++;
  if (s.submitEvery != 0 && ++s.batchesSinceSubmit >= s.submitEvery) {
    if (!s.submit(false, error)) return false;
    s.stats.midFrameSubmissions++;
  }
  return true;
}

bool Gpu::UploadGlm(const std::uint8_t *memory,
                    const std::vector<std::pair<std::uint32_t, std::uint32_t>> &ranges, std::string &error) {
  Impl &s = *impl_;
  if (ranges.empty()) return true;
  if (!s.begin_recording(error)) return false;
  std::vector<VkBufferCopy> copies;
  copies.reserve(ranges.size());
  for (const auto &[offset, size] : ranges) {
    if (offset + size > (4u << 20)) continue;
    VkDeviceSize at = 0;
    if (!s.ring(s.staging, size, 16, at, error)) return false;
    std::memcpy(static_cast<std::uint8_t *>(s.staging.mapped) + at, memory + offset, size);
    copies.push_back(VkBufferCopy{at, offset, size});
  }
  s.outside_pass();
  s.touch("glm upload", {}, {}, 0, s.kGlmBuffer);
  if (!copies.empty()) {
    gt4_hwd_vkCmdCopyBuffer(s.cmd, s.staging.buffer, s.glm, static_cast<std::uint32_t>(copies.size()), copies.data());
  }
  s.after_op();
  return true;
}

bool Gpu::DecodeTexture(ImageId id, std::uint32_t bp, std::uint32_t bw, std::uint32_t psm, std::uint32_t width,
                        std::uint32_t height, const std::uint32_t *palette, std::string &error, std::uint32_t originX,
                        std::uint32_t originY) {
  Impl &s = *impl_;
  const Image *image = s.image_of(id);
  if (!image || originX + width > image->width || originY + height > image->height) {
    error = "CORE2 texture decode outside its image";
    return false;
  }
  if (!s.begin_recording(error)) return false;
  VkDeviceSize paletteOffset = 0;
  const VkDeviceSize align = std::max<VkDeviceSize>(s.limits.minStorageBufferOffsetAlignment, 16);
  // The window first: a ring that submits moves to the next share, so later offsets stay in this recording.
  VkDescriptorBufferInfo glmRange{s.glm, 0, 4u << 20};
  std::uint32_t windowBase = 0, windowWords = 0;
  if (!s.bind_window(glmRange, windowBase, windowWords, error)) return false;
  if (!s.ring(s.staging, 256 * 4, align, paletteOffset, error)) return false;
  if (palette) std::memcpy(static_cast<std::uint8_t *>(s.staging.mapped) + paletteOffset, palette, 256 * 4);
  s.outside_pass();
  s.touch("decode", {}, {image}, (windowWords != 0 ? 0u : s.kGlmBuffer) | s.kClutBuffer, 0);
  VkDescriptorSet set = VK_NULL_HANDLE;
  if (!s.allocate_set(s.decodeSetLayout, set, error)) return false;
  VkDescriptorBufferInfo buffers[4] = {glmRange,
                                       {s.glmTables.buffer, 0, s.glmTableBytes},
                                       {s.staging.buffer, paletteOffset, 256 * 4},
                                       {s.clut, 0, 512 * 4}};  // the GPU CLUT: read only in mode 2 (IN-408)
  VkDescriptorImageInfo texels{VK_NULL_HANDLE, image->view, VK_IMAGE_LAYOUT_GENERAL};
  VkWriteDescriptorSet writes[5]{};
  const std::uint32_t bindingOf[5] = {0, 1, 2, 3, 5};
  for (std::uint32_t i = 0; i < 5; ++i) {
    writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[i].dstSet = set;
    writes[i].dstBinding = bindingOf[i];
    writes[i].descriptorCount = 1;
    if (i == 3) {
      writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
      writes[i].pImageInfo = &texels;
    } else {
      writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
      writes[i].pBufferInfo = &buffers[i < 3 ? i : 3];
    }
  }
  gt4_hwd_vkUpdateDescriptorSets(s.device, 5, writes, 0, nullptr);
  std::uint32_t push[22] = {};
  push[0] = bp;
  push[1] = bw;
  push[2] = psm;
  push[3] = width;
  push[4] = height;
  push[5] = palette ? 1u : 0u;
  push[12] = originX;  // IN-428: a mip level's place in the texture image
  push[13] = originY;
  push[20] = windowBase;  // IN-422
  push[21] = windowWords;
  gt4_hwd_vkCmdBindPipeline(s.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, s.decodePipeline);
  gt4_hwd_vkCmdBindDescriptorSets(s.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, s.decodeLayout, 0, 1, &set, 0, nullptr);
  gt4_hwd_vkCmdPushConstants(s.cmd, s.decodeLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof push, push);
  gt4_hwd_vkCmdDispatch(s.cmd, (width + 7u) / 8u, (height + 7u) / 8u, 1);
  s.after_op();
  s.stats.atlasUploads++;
  return true;
}

bool Gpu::ProfileStart(std::string &error) {
  Impl &s = *impl_;
  if (!s.timestamps) return true;  // no timestamps on this device
  if (!s.profilePool) {
    VkQueryPoolCreateInfo info{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
    info.queryType = VK_QUERY_TYPE_TIMESTAMP;
    info.queryCount = Impl::kProfileQueries;
    if (gt4_hwd_vkCreateQueryPool(s.device, &info, nullptr, &s.profilePool) != VK_SUCCESS) return true;
  }
  if (!s.begin_recording(error)) return false;
  s.flush_draws();
  s.end_render_pass();
  gt4_hwd_vkCmdResetQueryPool(s.cmd, s.profilePool, 0, Impl::kProfileQueries);
  s.profileNext = 0;
  s.profileLabels.clear();
  s.profileOpen = false;
  s.profiling = true;
  return true;
}

bool Gpu::ProfileReport(std::string &error) {
  Impl &s = *impl_;
  if (!s.profiling) return true;
  s.prof_end();
  s.profiling = false;
  if (!s.submit(true, error)) return false;
  std::vector<std::uint64_t> ticks(s.profileNext);
  if (s.profileNext == 0 ||
      gt4_hwd_vkGetQueryPoolResults(s.device, s.profilePool, 0, s.profileNext, ticks.size() * sizeof(std::uint64_t),
                                    ticks.data(), sizeof(std::uint64_t),
                                    VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT) != VK_SUCCESS) {
    std::fprintf(stderr, "[core2] gpu profile: no results\n");
    return true;
  }
  struct Kind {
    double total = 0, longest = 0;
    std::uint32_t count = 0;
  };
  std::map<std::string, Kind> kinds;
  double total = 0;
  for (std::size_t i = 0; i < s.profileLabels.size(); ++i) {
    const std::uint64_t a = ticks[2 * i], b = ticks[2 * i + 1];
    const double ms = b >= a ? static_cast<double>(b - a) * s.timestampNs / 1.0e6 : 0.0;
    Kind &kind = kinds[s.profileLabels[i]];
    kind.total += ms;
    kind.longest = std::max(kind.longest, ms);
    kind.count++;
    total += ms;
  }
  std::vector<std::pair<double, std::string>> rows;
  for (const auto &[label, kind] : kinds) rows.push_back({kind.total, label});
  std::sort(rows.rbegin(), rows.rend());
  std::fprintf(stderr, "[core2] gpu profile: %zu commands, %.2f ms of isolated GPU time\n", s.profileLabels.size(), total);
  for (std::size_t i = 0; i < rows.size() && i < 40; ++i) {
    const Kind &kind = kinds[rows[i].second];
    std::fprintf(stderr, "[core2] gpu profile %7.3f ms %5u x (max %.3f) %s\n", kind.total, kind.count, kind.longest,
                 rows[i].second.c_str());
  }
  return true;
}

void Gpu::SetDecodeWindow(const std::uint8_t *memory, std::uint32_t firstPage, std::uint32_t pageCount) {
  impl_->windowMemory = memory;
  impl_->windowFirstPage = firstPage;
  impl_->windowPages = pageCount;
}

bool Gpu::DecodeTextureGpuClut(ImageId id, std::uint32_t bp, std::uint32_t bw, std::uint32_t psm, std::uint32_t width,
                               std::uint32_t height, const GpuPalette &palette, std::string &error, std::uint32_t originX,
                               std::uint32_t originY) {
  Impl &s = *impl_;
  const Image *image = s.image_of(id);
  if (!image || originX + width > image->width || originY + height > image->height) {
    error = "CORE2 texture decode outside its image";
    return false;
  }
  if (!s.begin_recording(error)) return false;
  VkDescriptorBufferInfo glmRange{s.glm, 0, 4u << 20};
  std::uint32_t windowBase = 0, windowWords = 0;
  if (!s.bind_window(glmRange, windowBase, windowWords, error)) return false;
  s.outside_pass();
  s.touch("decode gpu clut", {}, {image}, (windowWords != 0 ? 0u : s.kGlmBuffer) | s.kClutBuffer, 0);
  VkDescriptorSet set = VK_NULL_HANDLE;
  if (!s.allocate_set(s.decodeSetLayout, set, error)) return false;
  VkDescriptorBufferInfo buffers[4] = {glmRange,
                                       {s.glmTables.buffer, 0, s.glmTableBytes},
                                       {s.glmTables.buffer, 0, s.glmTableBytes},  // the palette ring is not read
                                       {s.clut, 0, 512 * 4}};
  VkDescriptorImageInfo texels{VK_NULL_HANDLE, image->view, VK_IMAGE_LAYOUT_GENERAL};
  VkWriteDescriptorSet writes[5]{};
  const std::uint32_t bindingOf[5] = {0, 1, 2, 3, 5};
  for (std::uint32_t i = 0; i < 5; ++i) {
    writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[i].dstSet = set;
    writes[i].dstBinding = bindingOf[i];
    writes[i].descriptorCount = 1;
    if (i == 3) {
      writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
      writes[i].pImageInfo = &texels;
    } else {
      writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
      writes[i].pBufferInfo = &buffers[i < 3 ? i : 3];
    }
  }
  gt4_hwd_vkUpdateDescriptorSets(s.device, 5, writes, 0, nullptr);
  std::uint32_t push[22] = {};  // IN-422: the decode window words last (0: no window)
  push[0] = bp;
  push[1] = bw;
  push[2] = psm;
  push[3] = width;
  push[4] = height;
  push[5] = 2u;  // GPU CLUT buffer
  push[7] = palette.cpsm;
  push[8] = palette.csa;
  push[9] = static_cast<std::uint32_t>(palette.texa & 0xFFu);          // TEXA.TA0
  push[10] = static_cast<std::uint32_t>((palette.texa >> 32) & 0xFFu); // TEXA.TA1
  push[11] = static_cast<std::uint32_t>((palette.texa >> 15) & 1u);    // TEXA.AEM
  push[12] = originX;  // IN-428
  push[13] = originY;
  push[20] = windowBase;  // IN-422
  push[21] = windowWords;
  gt4_hwd_vkCmdBindPipeline(s.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, s.decodePipeline);
  gt4_hwd_vkCmdBindDescriptorSets(s.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, s.decodeLayout, 0, 1, &set, 0, nullptr);
  gt4_hwd_vkCmdPushConstants(s.cmd, s.decodeLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof push, push);
  gt4_hwd_vkCmdDispatch(s.cmd, (width + 7u) / 8u, (height + 7u) / 8u, 1);
  s.after_op();
  s.stats.atlasUploads++;
  return true;
}

bool Gpu::PaletteRowFromGpuClut(ImageId palettes, std::uint32_t row, std::uint32_t psm, const GpuPalette &palette,
                                std::string &error) {
  Impl &s = *impl_;
  const Image *image = s.image_of(palettes);
  if (!image || image->width < 256 || row >= image->height) {
    error = "CORE2 palette row outside the palette image";
    return false;
  }
  if (!s.begin_recording(error)) return false;
  s.outside_pass();
  s.touch("palette row", {Impl::Use(image, row)}, {Impl::Use(image, row)}, s.kClutBuffer, 0);
  VkDescriptorSet set = VK_NULL_HANDLE;
  if (!s.allocate_set(s.decodeSetLayout, set, error)) return false;
  VkDescriptorBufferInfo buffers[4] = {{s.glm, 0, 4u << 20},
                                       {s.glmTables.buffer, 0, s.glmTableBytes},
                                       {s.glmTables.buffer, 0, s.glmTableBytes},  // the palette ring is not read
                                       {s.clut, 0, 512 * 4}};
  VkDescriptorImageInfo texels{VK_NULL_HANDLE, image->view, VK_IMAGE_LAYOUT_GENERAL};
  VkWriteDescriptorSet writes[5]{};
  const std::uint32_t bindingOf[5] = {0, 1, 2, 3, 5};
  for (std::uint32_t i = 0; i < 5; ++i) {
    writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[i].dstSet = set;
    writes[i].dstBinding = bindingOf[i];
    writes[i].descriptorCount = 1;
    if (i == 3) {
      writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
      writes[i].pImageInfo = &texels;
    } else {
      writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
      writes[i].pBufferInfo = &buffers[i < 3 ? i : 3];
    }
  }
  gt4_hwd_vkUpdateDescriptorSets(s.device, 5, writes, 0, nullptr);
  std::uint32_t push[22] = {};  // IN-422: the decode window words last (0: no window)
  push[2] = psm;
  push[3] = 256;
  push[4] = 1;
  push[5] = 5u;  // one palette row from the GPU CLUT
  push[7] = palette.cpsm;
  push[8] = palette.csa;
  push[9] = static_cast<std::uint32_t>(palette.texa & 0xFFu);          // TEXA.TA0
  push[10] = static_cast<std::uint32_t>((palette.texa >> 32) & 0xFFu); // TEXA.TA1
  push[11] = static_cast<std::uint32_t>((palette.texa >> 15) & 1u);    // TEXA.AEM
  push[13] = row;                                                      // origin_y
  gt4_hwd_vkCmdBindPipeline(s.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, s.decodePipeline);
  gt4_hwd_vkCmdBindDescriptorSets(s.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, s.decodeLayout, 0, 1, &set, 0, nullptr);
  gt4_hwd_vkCmdPushConstants(s.cmd, s.decodeLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof push, push);
  gt4_hwd_vkCmdDispatch(s.cmd, 32, 1, 1);  // 256 entries, 8 x 8 groups
  s.after_op();
  s.stats.paletteRows++;
  return true;
}

bool Gpu::ClutFromTarget(ImageId target, const ClutTargetLoad &load, std::string &error) {
  Impl &s = *impl_;
  const Image *image = s.image_of(target);
  if (!image) {
    error = "CORE2 CLUT load from a target image that does not exist";
    return false;
  }
  if (!s.begin_recording(error)) return false;
  s.outside_pass();
  s.touch("clut from target", {image}, {}, s.kGlmBuffer, s.kClutBuffer);
  VkDescriptorSet set = VK_NULL_HANDLE;
  if (!s.allocate_set(s.decodeSetLayout, set, error)) return false;
  VkDescriptorBufferInfo buffers[4] = {{s.glm, 0, 4u << 20},
                                       {s.glmTables.buffer, 0, s.glmTableBytes},
                                       {s.inverseCt32.buffer, 0, 2048 * 4},
                                       {s.clut, 0, 512 * 4}};
  VkDescriptorImageInfo source{VK_NULL_HANDLE, image->view, VK_IMAGE_LAYOUT_GENERAL};
  VkWriteDescriptorSet writes[5]{};
  const std::uint32_t bindingOf[5] = {0, 1, 2, 3, 5};
  for (std::uint32_t i = 0; i < 5; ++i) {
    writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[i].dstSet = set;
    writes[i].dstBinding = bindingOf[i];
    writes[i].descriptorCount = 1;
    if (i == 3) {
      writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
      writes[i].pImageInfo = &source;
    } else {
      writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
      writes[i].pBufferInfo = &buffers[i < 3 ? i : 3];
    }
  }
  gt4_hwd_vkUpdateDescriptorSets(s.device, 5, writes, 0, nullptr);
  std::uint32_t push[18] = {load.cbp,
                            load.cpsm,
                            load.csm,
                            load.csa,
                            load.eightBit ? 1u : 0u,
                            load.cbw,
                            load.cou,
                            load.cov,
                            load.tbp,
                            load.tbw,
                            image->nativeWidth,
                            image->nativeHeight,
                            static_cast<std::uint32_t>(load.heldX0),
                            static_cast<std::uint32_t>(load.heldY0),
                            static_cast<std::uint32_t>(load.heldX1),
                            static_cast<std::uint32_t>(load.heldY1),
                            load.fullWord ? 1u : 0u,
                            FloatBits(static_cast<float>(image->scale))};
  gt4_hwd_vkCmdBindPipeline(s.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, s.clutTargetPipeline);
  gt4_hwd_vkCmdBindDescriptorSets(s.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, s.decodeLayout, 0, 1, &set, 0, nullptr);
  gt4_hwd_vkCmdPushConstants(s.cmd, s.decodeLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof push, push);
  gt4_hwd_vkCmdDispatch(s.cmd, 4, 1, 1);  // 256 entries, 64 per group
  s.after_op();
  s.stats.clutTargetLoads++;
  return true;
}

bool Gpu::MaterializeTarget(ImageId id, std::uint32_t psm, std::uint32_t tbp, std::uint32_t tbw, std::int32_t x0, std::int32_t y0,
                            std::int32_t x1, std::int32_t y1, bool fullWord, std::string &error) {
  Impl &s = *impl_;
  const Image *image = s.image_of(id);
  if (!image || x0 < 0 || y0 < 0 || x1 < x0 || y1 < y0) {
    error = "CORE2 GLM write of an image or rectangle that does not exist";
    return false;
  }
  if (!s.begin_recording(error)) return false;
  s.outside_pass();
  s.touch("glm write (target)", {image}, {}, s.kGlmBuffer, s.kGlmBuffer);
  VkDescriptorSet set = VK_NULL_HANDLE;
  if (!s.allocate_set(s.decodeSetLayout, set, error)) return false;
  VkDescriptorBufferInfo buffers[4] = {{s.glm, 0, 4u << 20},
                                       {s.glmTables.buffer, 0, s.glmTableBytes},
                                       {s.glmTables.buffer, 0, s.glmTableBytes},  // bindings 2 and 5 are not read
                                       {s.clut, 0, 512 * 4}};
  VkDescriptorImageInfo source{VK_NULL_HANDLE, image->view, VK_IMAGE_LAYOUT_GENERAL};
  VkWriteDescriptorSet writes[5]{};
  const std::uint32_t bindingOf[5] = {0, 1, 2, 3, 5};
  for (std::uint32_t i = 0; i < 5; ++i) {
    writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[i].dstSet = set;
    writes[i].dstBinding = bindingOf[i];
    writes[i].descriptorCount = 1;
    if (i == 3) {
      writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
      writes[i].pImageInfo = &source;
    } else {
      writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
      writes[i].pBufferInfo = &buffers[i < 3 ? i : 3];
    }
  }
  gt4_hwd_vkUpdateDescriptorSets(s.device, 5, writes, 0, nullptr);
  const std::uint32_t width = static_cast<std::uint32_t>(x1 - x0 + 1);
  const std::uint32_t height = static_cast<std::uint32_t>(y1 - y0 + 1);
  const std::uint32_t push[9] = {tbp,   tbw, static_cast<std::uint32_t>(x0), static_cast<std::uint32_t>(y0), width,
                                 height, fullWord ? 1u : 0u, FloatBits(static_cast<float>(image->scale)), psm};
  gt4_hwd_vkCmdBindPipeline(s.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, s.materializePipeline);
  gt4_hwd_vkCmdBindDescriptorSets(s.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, s.decodeLayout, 0, 1, &set, 0, nullptr);
  gt4_hwd_vkCmdPushConstants(s.cmd, s.decodeLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof push, push);
  gt4_hwd_vkCmdDispatch(s.cmd, (width + 7u) / 8u, (height + 7u) / 8u, 1);
  s.after_op();
  s.stats.materializations++;
  return true;
}

bool Gpu::WriteClut(std::uint32_t first, std::uint32_t count, const std::uint16_t *halfwords, std::string &error) {
  Impl &s = *impl_;
  if (count == 0) return true;
  if (first + count > 512) {
    error = "CORE2 GPU CLUT write past its 512 halfwords";
    return false;
  }
  if (!s.begin_recording(error)) return false;
  VkDeviceSize offset = 0;
  if (!s.ring(s.staging, count * 4u, 16, offset, error)) return false;
  auto *values = reinterpret_cast<std::uint32_t *>(static_cast<std::uint8_t *>(s.staging.mapped) + offset);
  for (std::uint32_t i = 0; i < count; ++i) values[i] = halfwords[first + i];
  s.outside_pass();
  s.touch("clut write", {}, {}, 0, s.kClutBuffer);
  VkBufferCopy copy{offset, first * 4u, count * 4u};
  gt4_hwd_vkCmdCopyBuffer(s.cmd, s.staging.buffer, s.clut, 1, &copy);
  s.after_op();
  s.stats.clutWrites++;
  return true;
}

bool Gpu::ReadClut(std::uint16_t halfwords[512], std::string &error) {
  Impl &s = *impl_;
  if (!s.begin_recording(error)) return false;
  s.flush_draws();
  s.end_render_pass();
  s.full_barrier();  // a host read or a rare pass: ordered at once
  VkBufferCopy copy{0, 0, 512 * 4};
  gt4_hwd_vkCmdCopyBuffer(s.cmd, s.clut, s.readback.buffer, 1, &copy);
  s.full_barrier();
  if (!s.submit(true, error)) return false;
  const auto *values = static_cast<const std::uint32_t *>(s.readback.mapped);
  for (std::uint32_t i = 0; i < 512; ++i) halfwords[i] = static_cast<std::uint16_t>(values[i]);
  s.stats.clutReads++;
  return true;
}

bool Gpu::PatchFromGlm(ImageId id, std::uint32_t bp, std::uint32_t bw, std::uint32_t psm, std::int32_t x0,
                       std::int32_t y0, std::int32_t x1, std::int32_t y1, std::uint32_t mask, std::string &error) {
  Impl &s = *impl_;
  const Image *image = s.image_of(id);
  if (!image || x0 < 0 || y0 < 0 || x1 < x0 || y1 < y0) {
    error = "CORE2 GLM patch of an image or rectangle that does not exist";
    return false;
  }
  if (!s.begin_recording(error)) return false;
  s.outside_pass();
  s.touch("dirty patch", {image}, {image}, s.kGlmBuffer, 0);
  VkDescriptorSet set = VK_NULL_HANDLE;
  if (!s.allocate_set(s.decodeSetLayout, set, error)) return false;
  VkDescriptorBufferInfo buffers[3] = {{s.glm, 0, 4u << 20}, {s.glmTables.buffer, 0, s.glmTableBytes},
                                       {s.glm, 0, 1024 * 4}};  // bindings 2 and 5 are not read by this kernel
  VkDescriptorImageInfo target{VK_NULL_HANDLE, image->view, VK_IMAGE_LAYOUT_GENERAL};
  VkWriteDescriptorSet writes[5]{};
  const std::uint32_t bindingOf[5] = {0, 1, 2, 3, 5};
  for (std::uint32_t i = 0; i < 5; ++i) {
    writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[i].dstSet = set;
    writes[i].dstBinding = bindingOf[i];
    writes[i].descriptorCount = 1;
    if (i == 3) {
      writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
      writes[i].pImageInfo = &target;
    } else {
      writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
      writes[i].pBufferInfo = &buffers[i < 2 ? i : 2];
    }
  }
  gt4_hwd_vkUpdateDescriptorSets(s.device, 5, writes, 0, nullptr);
  const std::uint32_t width = static_cast<std::uint32_t>(x1 - x0 + 1);
  const std::uint32_t height = static_cast<std::uint32_t>(y1 - y0 + 1);
  const std::uint32_t push[9] = {bp, bw, psm, static_cast<std::uint32_t>(x0), static_cast<std::uint32_t>(y0),
                                 width, height, image->scale, mask};
  gt4_hwd_vkCmdBindPipeline(s.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, s.patchGlmPipeline);
  gt4_hwd_vkCmdBindDescriptorSets(s.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, s.decodeLayout, 0, 1, &set, 0, nullptr);
  gt4_hwd_vkCmdPushConstants(s.cmd, s.decodeLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof push, push);
  gt4_hwd_vkCmdDispatch(s.cmd, (width * image->scale + 7u) / 8u, (height * image->scale + 7u) / 8u, 1);
  s.after_op();
  s.stats.patches++;
  return true;
}

bool Gpu::Patch(ImageId id, std::int32_t x0, std::int32_t y0, std::int32_t x1, std::int32_t y1,
                const std::vector<std::uint32_t> &words, std::uint32_t mask, std::string &error) {
  Impl &s = *impl_;
  const Image *image = s.image_of(id);
  if (!image) {
    error = "CORE2 patches an image that does not exist";
    return false;
  }
  const std::uint32_t width = static_cast<std::uint32_t>(x1 - x0 + 1);
  const std::uint32_t height = static_cast<std::uint32_t>(y1 - y0 + 1);
  if (x0 < 0 || y0 < 0 || x1 < x0 || y1 < y0 || words.size() != static_cast<std::size_t>(width) * height) {
    error = "CORE2 patch rectangle and words do not match";
    return false;
  }
  if (!s.begin_recording(error)) return false;
  const VkDeviceSize bytes = words.size() * 4u;
  VkDeviceSize offset = 0;
  const VkDeviceSize align = std::max<VkDeviceSize>(s.limits.minStorageBufferOffsetAlignment, 16);
  if (!s.ring(s.staging, bytes, align, offset, error)) return false;
  std::memcpy(static_cast<std::uint8_t *>(s.staging.mapped) + offset, words.data(), static_cast<std::size_t>(bytes));
  s.outside_pass();
  // One row is tracked as a row (the palette rows of IN-409; IN-420), anything else as the whole image.
  const std::uint32_t region = y0 == y1 ? static_cast<std::uint32_t>(y0) : Impl::kWholeImage;
  s.touch("patch (cpu words)", {Impl::Use(image, region)}, {Impl::Use(image, region)}, 0, 0);
  VkDescriptorSet set = VK_NULL_HANDLE;
  if (!s.compute_set(s.staging.buffer, offset, bytes, image->view, image->view, image->view, set, error)) return false;
  const std::uint32_t push[7] = {0u, static_cast<std::uint32_t>(x0), static_cast<std::uint32_t>(y0), width, height,
                                 image->scale, mask};
  s.dispatch(s.patchPipeline, set, push, 7, width * image->scale, height * image->scale);
  s.stats.patches++;
  return true;
}

bool Gpu::Reduce(ImageId id, std::int32_t x0, std::int32_t y0, std::int32_t x1, std::int32_t y1,
                 std::vector<std::uint32_t> &words, std::string &error) {
  Impl &s = *impl_;
  const Image *image = s.image_of(id);
  if (!image || x0 < 0 || y0 < 0 || x1 < x0 || y1 < y0) {
    error = "CORE2 reduces an image or rectangle that does not exist";
    return false;
  }
  const std::uint32_t width = static_cast<std::uint32_t>(x1 - x0 + 1);
  const std::uint32_t height = static_cast<std::uint32_t>(y1 - y0 + 1);
  const VkDeviceSize bytes = static_cast<VkDeviceSize>(width) * height * 4u;
  if (bytes > kReadbackBytes) {
    error = "CORE2 reduction larger than the readback buffer";
    return false;
  }
  if (!s.begin_recording(error)) return false;
  s.flush_draws();
  s.end_render_pass();
  s.full_barrier();  // a host read or a rare pass: ordered at once
  VkDescriptorSet set = VK_NULL_HANDLE;
  if (!s.compute_set(s.readback.buffer, 0, bytes, image->view, image->view, image->view, set, error)) return false;
  const std::uint32_t push[6] = {0u, static_cast<std::uint32_t>(x0), static_cast<std::uint32_t>(y0), width, height,
                                 FloatBits(static_cast<float>(image->scale))};
  s.dispatch(s.reducePipeline, set, push, 6, width, height);
  s.full_barrier();  // the host reads the result
  if (!s.submit(true, error)) return false;
  words.resize(static_cast<std::size_t>(width) * height);
  std::memcpy(words.data(), s.readback.mapped, static_cast<std::size_t>(bytes));
  s.stats.reductions++;
  return true;
}

bool Gpu::ReduceMany(const std::vector<ReduceRequest> &requests, std::vector<std::vector<std::uint32_t>> &words,
                     std::string &error) {
  Impl &s = *impl_;
  error.clear();
  const VkDeviceSize align = std::max<VkDeviceSize>(s.limits.minStorageBufferOffsetAlignment, 16);
  std::vector<VkDeviceSize> offsets;
  VkDeviceSize total = 0;
  for (const ReduceRequest &r : requests) {
    const Image *image = s.image_of(r.id);
    if (!image || r.x0 < 0 || r.y0 < 0 || r.x1 < r.x0 || r.y1 < r.y0) {
      error = "CORE2 reduces an image or rectangle that does not exist";
      return false;
    }
    total = (total + align - 1) / align * align;
    offsets.push_back(total);
    total += static_cast<VkDeviceSize>(r.x1 - r.x0 + 1) * static_cast<VkDeviceSize>(r.y1 - r.y0 + 1) * 4u;
  }
  if (total > kReadbackBytes) return false;  // empty error: reduce one by one
  if (!s.begin_recording(error)) return false;
  s.flush_draws();
  s.end_render_pass();
  s.full_barrier();  // a host read: ordered at once
  for (std::size_t i = 0; i < requests.size(); ++i) {
    const ReduceRequest &r = requests[i];
    const Image *image = s.image_of(r.id);
    const std::uint32_t width = static_cast<std::uint32_t>(r.x1 - r.x0 + 1);
    const std::uint32_t height = static_cast<std::uint32_t>(r.y1 - r.y0 + 1);
    VkDescriptorSet set = VK_NULL_HANDLE;
    if (!s.compute_set(s.readback.buffer, offsets[i], static_cast<VkDeviceSize>(width) * height * 4u, image->view,
                       image->view, image->view, set, error)) {
      return false;
    }
    const std::uint32_t push[6] = {0u, static_cast<std::uint32_t>(r.x0), static_cast<std::uint32_t>(r.y0), width, height,
                                   FloatBits(static_cast<float>(image->scale))};
    s.dispatch(s.reducePipeline, set, push, 6, width, height);  // distinct buffer ranges: no barrier between them
    s.stats.reductions++;
  }
  s.full_barrier();  // the host reads the results
  if (!s.submit(true, error)) return false;
  words.resize(requests.size());
  for (std::size_t i = 0; i < requests.size(); ++i) {
    const ReduceRequest &r = requests[i];
    const std::size_t count = static_cast<std::size_t>(r.x1 - r.x0 + 1) * static_cast<std::size_t>(r.y1 - r.y0 + 1);
    words[i].resize(count);
    std::memcpy(words[i].data(), static_cast<const std::uint8_t *>(s.readback.mapped) + offsets[i], count * 4u);
  }
  return true;
}

// IN-439: true once submission `serial` has completed (a fence poll, no wait); one still being recorded has not.
static bool SubmissionComplete(const VkDevice device, const VkFence fence, bool inFlight, std::string &error) {
  if (!inFlight) return true;
  const VkResult result = gt4_hw_vkWaitForFences(device, 1, &fence, VK_TRUE, 0);
  if (result == VK_TIMEOUT) return false;
  return check(result, "vkWaitForFences", error);
}

bool Gpu::ReduceLater(ImageId id, std::int32_t x0, std::int32_t y0, std::int32_t x1, std::int32_t y1,
                      std::uint32_t &ticket, std::string &error) {
  Impl &s = *impl_;
  error.clear();
  const Image *image = s.image_of(id);
  if (!image || x0 < 0 || y0 < 0 || x1 < x0 || y1 < y0) {
    error = "CORE2 reduces an image or rectangle that does not exist";
    return false;
  }
  const std::uint32_t width = static_cast<std::uint32_t>(x1 - x0 + 1);
  const std::uint32_t height = static_cast<std::uint32_t>(y1 - y0 + 1);
  if (static_cast<std::uint64_t>(width) * height > Impl::kLaterWords) return false;
  const std::uint32_t index = s.laterNext;
  Impl::LaterReduce &slotResult = s.laterReduces[index];
  if (slotResult.serial != 0) {  // its last result was never taken: reuse it once that submission is done
    std::vector<std::uint32_t> unused;
    if (!TakeReduced(index, unused, error)) return false;
  }
  if (!s.begin_recording(error)) return false;
  s.flush_draws();
  s.end_render_pass();
  s.full_barrier();  // a host read: ordered at once
  const VkDeviceSize offset = static_cast<VkDeviceSize>(index) * Impl::kLaterWords * 4u;
  VkDescriptorSet set = VK_NULL_HANDLE;
  if (!s.compute_set(s.later.buffer, offset, static_cast<VkDeviceSize>(width) * height * 4u, image->view, image->view,
                     image->view, set, error)) {
    return false;
  }
  const std::uint32_t push[6] = {0u, static_cast<std::uint32_t>(x0), static_cast<std::uint32_t>(y0), width, height,
                                 FloatBits(static_cast<float>(image->scale))};
  s.dispatch(s.reducePipeline, set, push, 6, width, height);
  s.full_barrier();  // the host reads the result after the fence
  s.stats.reductions++;
  s.stats.laterReductions++;
  slotResult.serial = s.submitCount + 1u;  // the recording in progress (compute_set may have submitted the one before)
  slotResult.count = width * height;
  s.laterNext = (index + 1u) % Impl::kLaterSlots;
  ticket = index;
  return true;
}

bool Gpu::TakeReduced(std::uint32_t ticket, std::vector<std::uint32_t> &words, std::string &error) {
  Impl &s = *impl_;
  error.clear();
  if (ticket >= Impl::kLaterSlots || s.laterReduces[ticket].serial == 0) return false;
  Impl::LaterReduce &result = s.laterReduces[ticket];
  if (result.serial > s.submitCount) return false;  // still in the recording
  for (std::uint32_t i = 0; i < s.slotCount; ++i) {
    const Impl::Slot &slot = s.slots[i];
    if (slot.serial != result.serial) continue;
    if (!SubmissionComplete(s.device, slot.fence, slot.inFlight, error)) return false;
    break;
  }  // no slot carries it any more: that slot was waited for and reused, so it is complete
  words.resize(result.count);
  std::memcpy(words.data(), static_cast<const std::uint8_t *>(s.later.mapped) + static_cast<std::size_t>(ticket) *
                                                                                  Impl::kLaterWords * 4u,
              static_cast<std::size_t>(result.count) * 4u);
  result.serial = 0;
  return true;
}

bool Gpu::Tone(ImageId id, std::uint32_t nativeWidth, std::uint32_t nativeHeight, const std::uint32_t palette[256],
               std::string &error) {
  Impl &s = *impl_;
  const Image *image = s.image_of(id);
  if (!image) {
    error = "CORE2 tone pass on an image that does not exist";
    return false;
  }
  if (!s.begin_recording(error)) return false;
  VkDeviceSize offset = 0;
  const VkDeviceSize align = std::max<VkDeviceSize>(s.limits.minStorageBufferOffsetAlignment, 16);
  if (!s.ring(s.staging, 256 * 4, align, offset, error)) return false;
  std::memcpy(static_cast<std::uint8_t *>(s.staging.mapped) + offset, palette, 256 * 4);
  s.outside_pass();
  s.touch("tone", {image}, {image}, 0, 0);
  VkDescriptorSet set = VK_NULL_HANDLE;
  if (!s.compute_set(s.staging.buffer, offset, 256 * 4, image->view, image->view, image->view, set, error)) return false;
  const std::uint32_t width = std::min(nativeWidth, image->nativeWidth) * image->scale;
  const std::uint32_t height = std::min(nativeHeight, image->nativeHeight) * image->scale;
  const std::uint32_t push[2] = {width, height};
  s.dispatch(s.tonePipeline, set, push, 2, width, height);
  s.stats.tones++;
  return true;
}

bool Gpu::Compose(const std::uint32_t words[28], ImageId sourceA, ImageId sourceB, std::uint32_t outWidth,
                  std::uint32_t outHeight, std::string &error) {
  Impl &s = *impl_;
  if (outWidth == 0 || outHeight == 0) {
    error = "CORE2 compose of an empty display";
    return false;
  }
  if (!s.begin_recording(error)) return false;
  if (s.display.width != outWidth || s.display.height != outHeight) {
    s.doom(s.display);
    if (!s.make_image(s.display, outWidth, outHeight, 1, error)) return false;
  }
  const Image *a = s.image_of(sourceA);
  const Image *b = s.image_of(sourceB);
  s.outside_pass();
  s.touch("compose", {a, b}, {&s.display}, 0, 0);
  VkDescriptorSet set = VK_NULL_HANDLE;
  if (!s.compute_set(s.staging.buffer, 0, 16, a ? a->view : s.atlas.view, b ? b->view : s.atlas.view, s.display.view,
                     set, error)) {
    return false;
  }
  s.dispatch(s.composePipeline, set, words, 28, outWidth, outHeight);
  s.stats.composes++;
  return true;
}

std::uint64_t Gpu::DisplayImage() const {
  return reinterpret_cast<std::uint64_t>(impl_->display.image);
}

bool Gpu::Deinterlace(const gpu::GpuDrawer::DeinterlaceStep &step, std::string &error) {
  Impl &s = *impl_;
  if (step.width == 0 || step.height == 0 || step.width > s.display.width || step.height > s.display.height ||
      step.current >= gpu::kGpuDeinterlaceHistory || step.mode < 2 || step.mode > 6) {
    error = "CORE2 deinterlace outside the display image";
    return false;
  }
  if (!s.begin_recording(error)) return false;
  // The history follows the display size. A new size starts a new history: the caller's step then says there is no
  // earlier field and no earlier output (its history is cleared on the same size change).
  bool fresh = false;
  for (Image &image : s.deinterlaceHistory) {
    if (!s.post_image(image, step.width, step.height, fresh, error)) return false;
  }
  if (!s.post_image(s.deinterlaceWoven, step.width, step.height, fresh, error) ||
      !s.post_image(s.deinterlaceOutput, step.width, step.height, fresh, error)) {
    return false;
  }
  s.flush_draws();
  s.end_render_pass();
  s.full_barrier();  // a host read or a rare pass: ordered at once
  VkImageCopy region{};
  region.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
  region.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
  region.extent = {step.width, step.height, 1};
  gt4_hwd_vkCmdCopyImage(s.cmd, s.display.image, VK_IMAGE_LAYOUT_GENERAL, s.deinterlaceHistory[step.current].image,
                         VK_IMAGE_LAYOUT_GENERAL, 1, &region);
  s.full_barrier();
  VkDescriptorSet set = VK_NULL_HANDLE;
  if (!s.allocate_set(s.deinterlaceSetLayout, set, error)) return false;
  VkDescriptorImageInfo history[gpu::kGpuDeinterlaceHistory];
  for (std::uint32_t i = 0; i < gpu::kGpuDeinterlaceHistory; ++i) {
    history[i] = {VK_NULL_HANDLE, s.deinterlaceHistory[i].view, VK_IMAGE_LAYOUT_GENERAL};
  }
  const VkDescriptorImageInfo woven{VK_NULL_HANDLE, s.deinterlaceWoven.view, VK_IMAGE_LAYOUT_GENERAL};
  const VkDescriptorImageInfo previous{VK_NULL_HANDLE, s.deinterlaceOutput.view, VK_IMAGE_LAYOUT_GENERAL};
  const VkDescriptorImageInfo output{VK_NULL_HANDLE, s.display.view, VK_IMAGE_LAYOUT_GENERAL};
  VkWriteDescriptorSet writes[4]{};
  const std::uint32_t bindings[4] = {0, 4, 5, 6};
  const VkDescriptorImageInfo *infos[4] = {history, &woven, &previous, &output};
  for (std::uint32_t i = 0; i < 4; ++i) {
    writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[i].dstSet = set;
    writes[i].dstBinding = bindings[i];
    writes[i].descriptorCount = i == 0 ? gpu::kGpuDeinterlaceHistory : 1u;
    writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    writes[i].pImageInfo = infos[i];
  }
  gt4_hwd_vkUpdateDescriptorSets(s.device, 4, writes, 0, nullptr);
  gt4_hwd_vkCmdBindPipeline(s.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, s.deinterlacePipeline);
  gt4_hwd_vkCmdBindDescriptorSets(s.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, s.deinterlaceLayout, 0, 1, &set, 0, nullptr);
  auto dispatch = [&](std::uint32_t mode) {
    const std::uint32_t push[14] = {mode, step.width, step.height, s.scale, step.rowParity, step.field, step.current,
                                    step.previousSame, step.previousOther, step.haveOutput ? 1u : 0u,
                                    step.thresholdPermille, step.initWoven ? 1u : 0u, step.rowStep,
                                    step.olderOther};
    gt4_hwd_vkCmdPushConstants(s.cmd, s.deinterlaceLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), push);
    gt4_hwd_vkCmdDispatch(s.cmd, (step.width + 7) / 8, (step.height + 7) / 8, 1);
    s.full_barrier();
  };
  if (step.mode == 2 || step.mode == 3) dispatch(1);  // the weave update first
  dispatch(step.mode);
  if (step.mode >= 5) {
    gt4_hwd_vkCmdCopyImage(s.cmd, s.display.image, VK_IMAGE_LAYOUT_GENERAL, s.deinterlaceOutput.image,
                           VK_IMAGE_LAYOUT_GENERAL, 1, &region);
    s.full_barrier();
  }
  s.stats.deinterlaces++;
  return true;
}

bool Gpu::Fxaa(std::uint32_t width, std::uint32_t height, std::string &error) {
  Impl &s = *impl_;
  if (width == 0 || height == 0 || width > s.display.width || height > s.display.height) {
    error = "CORE2 FXAA outside the display image";
    return false;
  }
  if (!s.begin_recording(error)) return false;
  bool fresh = false;
  if (!s.post_image(s.fxaa, s.display.width, s.display.height, fresh, error)) return false;
  s.flush_draws();
  s.end_render_pass();
  s.full_barrier();  // a host read or a rare pass: ordered at once
  VkDescriptorSet set = VK_NULL_HANDLE;
  if (!s.allocate_set(s.fxaaSetLayout, set, error)) return false;
  const VkDescriptorImageInfo images[2] = {{VK_NULL_HANDLE, s.display.view, VK_IMAGE_LAYOUT_GENERAL},
                                           {VK_NULL_HANDLE, s.fxaa.view, VK_IMAGE_LAYOUT_GENERAL}};
  VkWriteDescriptorSet writes[2]{};
  for (std::uint32_t i = 0; i < 2; ++i) {
    writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[i].dstSet = set;
    writes[i].dstBinding = i;
    writes[i].descriptorCount = 1;
    writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    writes[i].pImageInfo = &images[i];
  }
  gt4_hwd_vkUpdateDescriptorSets(s.device, 2, writes, 0, nullptr);
  gt4_hwd_vkCmdBindPipeline(s.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, s.fxaaPipeline);
  gt4_hwd_vkCmdBindDescriptorSets(s.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, s.fxaaLayout, 0, 1, &set, 0, nullptr);
  const std::uint32_t push[2] = {width, height};
  gt4_hwd_vkCmdPushConstants(s.cmd, s.fxaaLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), push);
  gt4_hwd_vkCmdDispatch(s.cmd, (width + 7) / 8, (height + 7) / 8, 1);
  s.full_barrier();
  s.stats.fxaaFrames++;
  return true;
}

std::uint64_t Gpu::FxaaImage() const { return reinterpret_cast<std::uint64_t>(impl_->fxaa.image); }

bool Gpu::PresentScale(bool fromFxaa, std::uint32_t width, std::uint32_t height, std::uint32_t outWidth,
                       std::uint32_t outHeight, float sharpness, std::string &error) {
  Impl &s = *impl_;
  Image &source = fromFxaa ? s.fxaa : s.display;
  if (width == 0 || height == 0 || width > source.width || height > source.height || outWidth == 0 ||
      outHeight == 0 || outWidth > s.limits.maxImageDimension2D || outHeight > s.limits.maxImageDimension2D) {
    error = "CORE2 presentation resampling outside the display image";
    return false;
  }
  if (!s.begin_recording(error)) return false;
  s.flush_draws();
  s.end_render_pass();
  s.full_barrier();  // a rare pass: ordered at once
  // One pass: `from` (fromWidth x fromHeight) into `to` (toWidth x toHeight, made to that size).
  auto pass = [&](std::uint32_t mode, Image &from, std::uint32_t fromWidth, std::uint32_t fromHeight, Image &to,
                  std::uint32_t toWidth, std::uint32_t toHeight) -> bool {
    bool fresh = false;
    if (!s.post_image(to, toWidth, toHeight, fresh, error)) return false;
    VkDescriptorSet set = VK_NULL_HANDLE;
    if (!s.allocate_set(s.presentSetLayout, set, error)) return false;
    const VkDescriptorImageInfo images[2] = {{VK_NULL_HANDLE, from.view, VK_IMAGE_LAYOUT_GENERAL},
                                             {VK_NULL_HANDLE, to.view, VK_IMAGE_LAYOUT_GENERAL}};
    VkWriteDescriptorSet writes[2]{};
    for (std::uint32_t i = 0; i < 2; ++i) {
      writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
      writes[i].dstSet = set;
      writes[i].dstBinding = i;
      writes[i].descriptorCount = 1;
      writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
      writes[i].pImageInfo = &images[i];
    }
    gt4_hwd_vkUpdateDescriptorSets(s.device, 2, writes, 0, nullptr);
    gt4_hwd_vkCmdBindPipeline(s.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, s.presentPipeline);
    gt4_hwd_vkCmdBindDescriptorSets(s.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, s.presentLayout, 0, 1, &set, 0, nullptr);
    const std::uint32_t push[6] = {fromWidth, fromHeight, toWidth, toHeight, mode, FloatBits(sharpness)};
    gt4_hwd_vkCmdPushConstants(s.cmd, s.presentLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), push);
    gt4_hwd_vkCmdDispatch(s.cmd, (toWidth + 7) / 8, (toHeight + 7) / 8, 1);
    s.full_barrier();
    return true;
  };
  constexpr std::uint32_t kHorizontal = 0, kVertical = 1, kEasu = 2, kRcas = 3;
  const bool rcas = sharpness > 0.0f;
  Image *current = &source;
  std::uint32_t cw = width, ch = height;
  // The image a resampling pass writes: the output when nothing follows, otherwise the other scratch image.
  auto next = [&](bool last) -> Image & {
    if (last) return s.presentOut;
    return current == &s.presentA ? s.presentB : s.presentA;
  };
  if (outWidth >= width && outHeight >= height && (outWidth != width || outHeight != height)) {
    Image &to = next(!rcas);
    if (!pass(kEasu, *current, cw, ch, to, outWidth, outHeight)) return false;
    current = &to;
    cw = outWidth;
    ch = outHeight;
  } else {
    if (outWidth != width) {
      Image &to = next(!rcas && outHeight == height);
      if (!pass(kHorizontal, *current, cw, ch, to, outWidth, ch)) return false;
      current = &to;
      cw = outWidth;
    }
    if (outHeight != height) {
      Image &to = next(!rcas);
      if (!pass(kVertical, *current, cw, ch, to, cw, outHeight)) return false;
      current = &to;
      ch = outHeight;
    }
  }
  if (rcas || current == &source) {
    // RCAS (or, at the same size without sharpening, a plain copy through RCAS with no lobe).
    if (!pass(kRcas, *current, cw, ch, s.presentOut, cw, ch)) return false;
  }
  s.stats.presentScales++;
  return true;
}

std::uint64_t Gpu::PresentImage() const { return reinterpret_cast<std::uint64_t>(impl_->presentOut.image); }

bool Gpu::ReadDisplay(std::uint32_t width, std::uint32_t height, std::vector<std::uint32_t> &pixels,
                      std::string &error, bool presentScaled, bool fxaa) {
  Impl &s = *impl_;
  const Image &source = presentScaled ? s.presentOut : fxaa ? s.fxaa : s.display;
  width = std::min(width, source.width);
  height = std::min(height, source.height);
  const VkDeviceSize rowBytes = static_cast<VkDeviceSize>(width) * 4u;
  if (rowBytes == 0 || height == 0 || rowBytes > kReadbackBytes) {
    error = "CORE2 display read outside the readback buffer";
    return false;
  }
  // IN-435: frames larger than the readback buffer (7680 x 5376 at 12x is 165 MB) are read in bands of rows.
  const std::uint32_t bandRows = static_cast<std::uint32_t>(std::min<VkDeviceSize>(height, kReadbackBytes / rowBytes));
  pixels.resize(static_cast<std::size_t>(width) * height);
  for (std::uint32_t top = 0; top < height; top += bandRows) {
    const std::uint32_t rows = std::min(bandRows, height - top);
    if (!s.begin_recording(error)) return false;
    s.flush_draws();
    s.end_render_pass();
    s.full_barrier();  // a host read or a rare pass: ordered at once
    VkBufferImageCopy region{};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageOffset = {0, static_cast<std::int32_t>(top), 0};
    region.imageExtent = {width, rows, 1};
    gt4_hwd_vkCmdCopyImageToBuffer(s.cmd, source.image, VK_IMAGE_LAYOUT_GENERAL, s.readback.buffer, 1, &region);
    s.full_barrier();
    if (!s.submit(true, error)) return false;
    std::memcpy(pixels.data() + static_cast<std::size_t>(top) * width, s.readback.mapped,
                static_cast<std::size_t>(rowBytes * rows));
  }
  return true;
}

bool Gpu::ReadImage(ImageId id, std::uint32_t width, std::uint32_t height, std::vector<std::uint32_t> &words,
                    std::string &error) {
  Impl &s = *impl_;
  const Image *image = s.image_of(id);
  if (!image) {
    error = "CORE2 reads an image that does not exist";
    return false;
  }
  width = std::min(width, image->width);
  height = std::min(height, image->height);
  const VkDeviceSize bytes = static_cast<VkDeviceSize>(width) * height * 4u;
  if (bytes == 0 || bytes > kReadbackBytes) {
    error = "CORE2 image read outside the readback buffer";
    return false;
  }
  if (!s.begin_recording(error)) return false;
  s.flush_draws();
  s.end_render_pass();
  s.full_barrier();  // a host read or a rare pass: ordered at once
  VkBufferImageCopy region{};
  region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
  region.imageExtent = {width, height, 1};
  gt4_hwd_vkCmdCopyImageToBuffer(s.cmd, image->image, VK_IMAGE_LAYOUT_GENERAL, s.readback.buffer, 1, &region);
  s.full_barrier();
  if (!s.submit(true, error)) return false;
  words.resize(static_cast<std::size_t>(width) * height);
  std::memcpy(words.data(), s.readback.mapped, static_cast<std::size_t>(bytes));
  return true;
}

bool Gpu::Submit(bool wait, std::string &error) { return impl_->submit(wait, error); }

}  // namespace hwgs::core2
