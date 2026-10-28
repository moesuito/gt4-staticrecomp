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
#include <chrono>
#include <cmath>
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
  VkDeviceSize used = 0;  // ring cursor for this recording, inside the recording's half
  VkDeviceSize base = 0;  // the recording's half: [base, base + size / 2) (two recordings alternate)
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
};

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
  // command buffer, fence, descriptor pool and half of every ring.
  struct Slot {
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    VkDescriptorPool pool = VK_NULL_HANDLE;
    bool inFlight = false;
    std::vector<Image> doomed;  // released once this slot's submission is complete
  };
  Slot slots[2];
  std::uint32_t slot = 0;           // the slot being recorded
  std::uint32_t lastSubmitted = 1;  // the slot submitted last
  VkCommandBuffer cmd = VK_NULL_HANDLE;  // slots[slot].cmd
  VkFence fence = VK_NULL_HANDLE;
  bool recording = false;
  // IN-410: the recording is submitted every `submitEvery` draw batches, so the GPU draws while the CPU records the
  // rest of the frame and a readback in mid-frame (GT4's flare reads) waits only for the work after the last
  // submission. GT4_CORE2_SUBMIT_EVERY=<batches>, 0 = only when the CPU needs a result or at the end of the frame.
  std::uint32_t submitEvery = [] {
    const char *text = std::getenv("GT4_CORE2_SUBMIT_EVERY");
    return text ? static_cast<std::uint32_t>(std::strtoul(text, nullptr, 10)) : 96u;
  }();
  std::uint32_t batchesSinceSubmit = 0;
  // Draw descriptor sets of this recording by their four images (a batch with the same targets reuses its set).
  std::unordered_map<std::uint64_t, VkDescriptorSet> drawSets;
  bool renderPassOpen = false;
  bool drewSinceBarrier = false;

  Buffer staging, vertices, uniforms, readback;

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

  VkDescriptorSetLayout computeSetLayout = VK_NULL_HANDLE;
  VkPipelineLayout computeLayout = VK_NULL_HANDLE;
  VkShaderModule patchModule = VK_NULL_HANDLE, reduceModule = VK_NULL_HANDLE, toneModule = VK_NULL_HANDLE,
                 composeModule = VK_NULL_HANDLE;
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
  bool ring(Buffer &buffer, VkDeviceSize bytes, VkDeviceSize alignment, VkDeviceSize &offset, std::string &error) {
    if (bytes > buffer.size) {
      error = "CORE2 ring request larger than the ring";
      return false;
    }
    const VkDeviceSize half = buffer.size / 2;
    if (bytes > half) {
      error = "CORE2 ring request larger than half the ring";
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
    if (!check(gt4_hwd_vkAllocateMemory(device, &allocate, nullptr, &out.memory), "vkAllocateMemory", error)) return false;
    if (!check(gt4_hwd_vkBindImageMemory(device, out.image, out.memory, 0), "vkBindImageMemory", error)) return false;
    VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    view.image = out.image;
    view.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view.format = VK_FORMAT_R32_UINT;
    view.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    if (!check(gt4_hwd_vkCreateImageView(device, &view, nullptr, &out.view), "vkCreateImageView", error)) return false;
    out.bytes = requirements.size;
    out.alive = true;
    drawSets.clear();
    stats.imageBytes += out.bytes;
    // UNDEFINED -> GENERAL and zero, in this recording.
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

  void release(Image &image) {
    if (!image.alive) return;
    if (image.view) gt4_hwd_vkDestroyImageView(device, image.view, nullptr);
    if (image.image) gt4_hwd_vkDestroyImage(device, image.image, nullptr);
    if (image.memory) gt4_hwd_vkFreeMemory(device, image.memory, nullptr);
    stats.imageBytes -= std::min(stats.imageBytes, image.bytes);
    image = Image{};
  }

  void doom(Image &image) {
    if (!image.alive) return;
    drawSets.clear();  // a cached set may name this image (an id can be reused or grown)
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

  VkPipeline make_graphics(VkPrimitiveTopology topology, VkShaderModule fragment, std::string &error) {
    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vertexModule;
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fragment;
    stages[1].pName = "main";
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
    drewSinceBarrier = false;
  }

  // Inside the render pass (the subpass self-dependency): later fragments see earlier fragment stores.
  void fragment_barrier() {
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

  // Leaves the render pass and orders everything before against everything after (compute, transfer).
  void outside_pass() {
    end_render_pass();
    full_barrier();
  }

  bool wait_slot(std::uint32_t index, std::string &error) {
    Slot &s = slots[index];
    if (!s.inFlight) return true;
    const auto waitStart = std::chrono::steady_clock::now();
    if (!check(gt4_hw_vkWaitForFences(device, 1, &s.fence, VK_TRUE, UINT64_MAX), "vkWaitForFences", error)) return false;
    stats.waitNs += static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - waitStart).count());
    stats.waits++;
    for (Image &image : s.doomed) release(image);
    s.doomed.clear();
    s.inFlight = false;
    return true;
  }

  // Waits for everything submitted (a fence signals after every earlier submission on the queue too).
  bool wait_all(std::string &error) { return wait_slot(lastSubmitted, error) && wait_slot(lastSubmitted ^ 1u, error); }

  bool begin_recording(std::string &error) {
    if (recording) return true;
    slot = lastSubmitted ^ 1u;
    if (!wait_slot(slot, error)) return false;  // the submission before the last one: usually long done
    cmd = slots[slot].cmd;
    fence = slots[slot].fence;
    transientPool = slots[slot].pool;
    if (!check(gt4_hw_vkResetCommandBuffer(cmd, 0), "vkResetCommandBuffer", error)) return false;
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (!check(gt4_hw_vkBeginCommandBuffer(cmd, &begin), "vkBeginCommandBuffer", error)) return false;
    if (!check(gt4_hwd_vkResetDescriptorPool(device, transientPool, 0), "vkResetDescriptorPool", error)) return false;
    transientSetsUsed = 0;
    drawSets.clear();
    for (Buffer *buffer : {&staging, &vertices, &uniforms}) {
      buffer->used = 0;
      buffer->base = slot * (buffer->size / 2);
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
    end_render_pass();
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
    full_barrier();
  }
};

Gpu::Gpu() : impl_(std::make_unique<Impl>()) {}

Gpu::~Gpu() {
  Impl &s = *impl_;
  if (!s.device) return;
  std::string ignored;
  s.submit(true, ignored);
  for (Image &image : s.images) s.release(image);
  for (Image &image : s.doomed) s.release(image);
  s.release(s.atlas);
  s.release(s.display);
  for (Image &image : s.deinterlaceHistory) s.release(image);
  s.release(s.deinterlaceWoven);
  s.release(s.deinterlaceOutput);
  s.release(s.fxaa);
  // The remaining objects live as long as the device (the renderer is destroyed with the backend).
}

bool Gpu::Ready() const { return impl_->ready; }
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

  const VkBufferUsageFlags stagingUsage =
      VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
  if (!s.make_buffer(s.staging, kStagingBytes, stagingUsage, 0, error)) return false;
  if (!s.make_buffer(s.vertices, kVertexBytes, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, 0, error)) return false;
  if (!s.make_buffer(s.uniforms, kUniformBytes, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, 0, error)) return false;
  if (!s.make_buffer(s.readback, kReadbackBytes, stagingUsage, VK_MEMORY_PROPERTY_HOST_CACHED_BIT, error)) return false;

  // Draw set (hw_pixel_uber.frag): 0 pixel state, 1 frame, 2 depth, 3 1x texel atlas, 4 a target sampled at scale.
  {
    VkDescriptorSetLayoutBinding bindings[5]{};
    bindings[0] = {0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
    for (std::uint32_t i = 1; i < 5; ++i) {
      bindings[i] = {i, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
    }
    VkDescriptorSetLayoutCreateInfo info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    info.bindingCount = 5;
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
                                     {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 4 * kTransientSets + 64},
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
      s.make_module(hwgs::shaders::hw_pixel_uber_smooth_words, hwgs::shaders::hw_pixel_uber_smooth_word_count, error);
  if (!s.vertexModule || !s.fragmentModule) return false;
  s.pipelines[0] = s.make_graphics(VK_PRIMITIVE_TOPOLOGY_POINT_LIST, s.fragmentModule, error);
  s.pipelines[1] = s.make_graphics(VK_PRIMITIVE_TOPOLOGY_LINE_LIST, s.fragmentModule, error);
  s.pipelines[2] = s.make_graphics(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, s.fragmentModule, error);
  if (!s.pipelines[0] || !s.pipelines[1] || !s.pipelines[2]) return false;
  if (handles.pixelInterlock) {
    s.interlockModule = s.make_module(hwgs::shaders::hw_pixel_uber_smooth_interlock_words,
                                      hwgs::shaders::hw_pixel_uber_smooth_interlock_word_count, error);
    if (!s.interlockModule) return false;
    s.interlockTriangles = s.make_graphics(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, s.interlockModule, error);
    if (!s.interlockTriangles) return false;
    s.interlock = true;
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
  VkImageCopy region{};
  region.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
  region.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
  region.srcOffset = {left * static_cast<std::int32_t>(sc), top * static_cast<std::int32_t>(sc), 0};
  region.dstOffset = region.srcOffset;
  region.extent = {static_cast<std::uint32_t>(right - left) * sc, static_cast<std::uint32_t>(bottom - top) * sc, 1};
  gt4_hwd_vkCmdCopyImage(s.cmd, source->image, VK_IMAGE_LAYOUT_GENERAL, destination->image, VK_IMAGE_LAYOUT_GENERAL,
                         1, &region);
  s.full_barrier();
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
  VkBufferImageCopy region{};
  region.bufferOffset = offset;
  region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
  region.imageOffset = {static_cast<std::int32_t>(x), static_cast<std::int32_t>(y), 0};
  region.imageExtent = {width, height, 1};
  gt4_hwd_vkCmdCopyBufferToImage(s.cmd, s.staging.buffer, s.atlas.image, VK_IMAGE_LAYOUT_GENERAL, 1, &region);
  s.full_barrier();
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
  VkBufferImageCopy region{};
  region.bufferOffset = offset;
  region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
  region.imageExtent = {width, height, 1};
  gt4_hwd_vkCmdCopyBufferToImage(s.cmd, s.staging.buffer, image->image, VK_IMAGE_LAYOUT_GENERAL, 1, &region);
  s.full_barrier();
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
  state.fields[gpu::gpufield::TexOriginX] = 0u;
  state.fields[gpu::gpufield::TexOriginY] = paletted ? static_cast<std::uint32_t>(batch.paletteRow) : 0u;
  if (paletted) state.fields[gpu::gpufield::TexelKind] = 3u;  // HW_TEXEL_INDEXED
  state.fields[gpu::gpufield::KeepTopByte] = 1u;
  std::memcpy(static_cast<std::uint8_t *>(s.uniforms.mapped) + uniformOffset, state.fields, sizeof state.fields);

  VkDescriptorSet set = VK_NULL_HANDLE;
  const std::uint64_t setKey = (static_cast<std::uint64_t>(batch.frame + 1) << 48) ^
                               (static_cast<std::uint64_t>(batch.depth + 1) << 32) ^
                               (static_cast<std::uint64_t>(batch.memoryTexture + 1) << 16) ^
                               static_cast<std::uint64_t>(paletted ? batch.palettes + 0x8001 : batch.texture + 1);
  if (auto cached = s.drawSets.find(setKey); cached != s.drawSets.end()) {
    set = cached->second;
  } else {
    if (!s.allocate_set(s.drawSetLayout, set, error)) return false;
    s.drawSets[setKey] = set;
    VkDescriptorBufferInfo uniformInfo{s.uniforms.buffer, 0, kPixelStateBytes};
    VkDescriptorImageInfo imageInfo[4] = {{VK_NULL_HANDLE, frame->view, VK_IMAGE_LAYOUT_GENERAL},
                                          {VK_NULL_HANDLE, depth->view, VK_IMAGE_LAYOUT_GENERAL},
                                          {VK_NULL_HANDLE, texels->view, VK_IMAGE_LAYOUT_GENERAL},
                                          {VK_NULL_HANDLE, texture->view, VK_IMAGE_LAYOUT_GENERAL}};
    VkWriteDescriptorSet writes[5]{};
    for (std::uint32_t i = 0; i < 5; ++i) {
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
    gt4_hwd_vkUpdateDescriptorSets(s.device, 5, writes, 0, nullptr);
  }

  if (!s.renderPassOpen) {
    VkRenderPassBeginInfo begin{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    begin.renderPass = s.renderPass;
    begin.framebuffer = s.framebuffer;
    begin.renderArea = {{0, 0}, {s.framebufferWidth, s.framebufferHeight}};
    gt4_hwd_vkCmdBeginRenderPass(s.cmd, &begin, VK_SUBPASS_CONTENTS_INLINE);
    s.renderPassOpen = true;
  } else if (s.drewSinceBarrier && (!s.interlock || batch.texture != kNoImage)) {
    // A texture taken from a target reads pixels other than its own (no interlock order); without pixel interlock
    // every batch waits for the one before.
    s.fragment_barrier();
  }
  const std::uint32_t sc = s.scale;
  const std::uint32_t uniformDynamic = static_cast<std::uint32_t>(uniformOffset);
  gt4_hwd_vkCmdBindDescriptorSets(s.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, s.drawLayout, 0, 1, &set, 1, &uniformDynamic);
  VkDeviceSize bindOffset = vertexOffset;
  gt4_hwd_vkCmdBindVertexBuffers(s.cmd, 0, 1, &s.vertices.buffer, &bindOffset);
  const VkViewport viewport{0.0f, 0.0f, static_cast<float>(frame->width), static_cast<float>(frame->height), 0.0f, 1.0f};
  gt4_hwd_vkCmdSetViewport(s.cmd, 0, 1, &viewport);
  std::uint32_t push[8];
  push[0] = FloatBits(static_cast<float>(frame->width));
  push[1] = FloatBits(static_cast<float>(frame->height));
  push[2] = FloatBits(static_cast<float>(sc));
  push[3] = FloatBits(s.shift);
  push[4] = 0;  // the batch vertices already carry XYOFFSET
  push[5] = 0;
  push[6] = batch.zMax;
  // tex_scale: s for a texture sampled from a target (binding 4), 1 for the atlas; at scale the fine dither (8.6)
  // and the floored rasteriser depth (1.6) are on.
  push[7] = (batch.texture != kNoImage ? sc : 1u) | (sc > 1 ? (0x40000000u | 0x10000000u) : 0u);
  gt4_hwd_vkCmdPushConstants(s.cmd, s.drawLayout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                             sizeof push, push);
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
    VkRect2D scissor{};
    if (end > first && scissorOf(x0, y0, x1, y1, scissor)) {
      gt4_hwd_vkCmdSetScissor(s.cmd, 0, 1, &scissor);
      gt4_hwd_vkCmdBindPipeline(s.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, s.interlockTriangles);
      gt4_hwd_vkCmdDraw(s.cmd, end - first, 1, first, 0);
      s.stats.draws++;
      s.drewSinceBarrier = true;
    }
  } else {
    for (const gpu::GpuDraw &draw : *batch.draws) {
      if (draw.count == 0 || draw.first + draw.count > vertexCount) continue;
      std::int32_t a, b, c, d;
      boundsOf(draw, a, b, c, d);
      VkRect2D scissor{};
      if (!scissorOf(a, b, c, d, scissor)) continue;
      if (s.drewSinceBarrier) s.fragment_barrier();
      gt4_hwd_vkCmdSetScissor(s.cmd, 0, 1, &scissor);
      const std::size_t pipeline = draw.topology == gpu::GpuTopology::Points  ? 0
                                   : draw.topology == gpu::GpuTopology::Lines ? 1
                                                                              : 2;
      gt4_hwd_vkCmdBindPipeline(s.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, s.pipelines[pipeline]);
      gt4_hwd_vkCmdDraw(s.cmd, draw.count, 1, draw.first, 0);
      s.stats.draws++;
      s.drewSinceBarrier = true;
    }
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
  if (!copies.empty()) {
    gt4_hwd_vkCmdCopyBuffer(s.cmd, s.staging.buffer, s.glm, static_cast<std::uint32_t>(copies.size()), copies.data());
  }
  s.full_barrier();
  return true;
}

bool Gpu::DecodeTexture(ImageId id, std::uint32_t bp, std::uint32_t bw, std::uint32_t psm, std::uint32_t width,
                        std::uint32_t height, const std::uint32_t *palette, std::string &error) {
  Impl &s = *impl_;
  const Image *image = s.image_of(id);
  if (!image || width > image->width || height > image->height) {
    error = "CORE2 texture decode outside its image";
    return false;
  }
  if (!s.begin_recording(error)) return false;
  VkDeviceSize paletteOffset = 0;
  const VkDeviceSize align = std::max<VkDeviceSize>(s.limits.minStorageBufferOffsetAlignment, 16);
  if (!s.ring(s.staging, 256 * 4, align, paletteOffset, error)) return false;
  if (palette) std::memcpy(static_cast<std::uint8_t *>(s.staging.mapped) + paletteOffset, palette, 256 * 4);
  s.outside_pass();
  VkDescriptorSet set = VK_NULL_HANDLE;
  if (!s.allocate_set(s.decodeSetLayout, set, error)) return false;
  VkDescriptorBufferInfo buffers[4] = {{s.glm, 0, 4u << 20},
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
  std::uint32_t push[20] = {};
  push[0] = bp;
  push[1] = bw;
  push[2] = psm;
  push[3] = width;
  push[4] = height;
  push[5] = palette ? 1u : 0u;
  gt4_hwd_vkCmdBindPipeline(s.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, s.decodePipeline);
  gt4_hwd_vkCmdBindDescriptorSets(s.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, s.decodeLayout, 0, 1, &set, 0, nullptr);
  gt4_hwd_vkCmdPushConstants(s.cmd, s.decodeLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof push, push);
  gt4_hwd_vkCmdDispatch(s.cmd, (width + 7u) / 8u, (height + 7u) / 8u, 1);
  s.full_barrier();
  s.stats.atlasUploads++;
  return true;
}

bool Gpu::DecodeTextureGpuClut(ImageId id, std::uint32_t bp, std::uint32_t bw, std::uint32_t psm, std::uint32_t width,
                               std::uint32_t height, const GpuPalette &palette, std::string &error) {
  Impl &s = *impl_;
  const Image *image = s.image_of(id);
  if (!image || width > image->width || height > image->height) {
    error = "CORE2 texture decode outside its image";
    return false;
  }
  if (!s.begin_recording(error)) return false;
  s.outside_pass();
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
  std::uint32_t push[20] = {};
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
  gt4_hwd_vkCmdBindPipeline(s.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, s.decodePipeline);
  gt4_hwd_vkCmdBindDescriptorSets(s.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, s.decodeLayout, 0, 1, &set, 0, nullptr);
  gt4_hwd_vkCmdPushConstants(s.cmd, s.decodeLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof push, push);
  gt4_hwd_vkCmdDispatch(s.cmd, (width + 7u) / 8u, (height + 7u) / 8u, 1);
  s.full_barrier();
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
  std::uint32_t push[20] = {};
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
  s.full_barrier();
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
  s.full_barrier();
  s.stats.clutTargetLoads++;
  return true;
}

bool Gpu::MaterializeTarget(ImageId id, std::uint32_t tbp, std::uint32_t tbw, std::int32_t x0, std::int32_t y0,
                            std::int32_t x1, std::int32_t y1, bool fullWord, std::string &error) {
  Impl &s = *impl_;
  const Image *image = s.image_of(id);
  if (!image || x0 < 0 || y0 < 0 || x1 < x0 || y1 < y0) {
    error = "CORE2 GLM write of an image or rectangle that does not exist";
    return false;
  }
  if (!s.begin_recording(error)) return false;
  s.outside_pass();
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
  const std::uint32_t push[8] = {tbp,   tbw, static_cast<std::uint32_t>(x0), static_cast<std::uint32_t>(y0), width,
                                 height, fullWord ? 1u : 0u, FloatBits(static_cast<float>(image->scale))};
  gt4_hwd_vkCmdBindPipeline(s.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, s.materializePipeline);
  gt4_hwd_vkCmdBindDescriptorSets(s.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, s.decodeLayout, 0, 1, &set, 0, nullptr);
  gt4_hwd_vkCmdPushConstants(s.cmd, s.decodeLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof push, push);
  gt4_hwd_vkCmdDispatch(s.cmd, (width + 7u) / 8u, (height + 7u) / 8u, 1);
  s.full_barrier();
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
  VkBufferCopy copy{offset, first * 4u, count * 4u};
  gt4_hwd_vkCmdCopyBuffer(s.cmd, s.staging.buffer, s.clut, 1, &copy);
  s.full_barrier();
  s.stats.clutWrites++;
  return true;
}

bool Gpu::ReadClut(std::uint16_t halfwords[512], std::string &error) {
  Impl &s = *impl_;
  if (!s.begin_recording(error)) return false;
  s.outside_pass();
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
  s.full_barrier();
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
  s.outside_pass();
  VkDescriptorSet set = VK_NULL_HANDLE;
  if (!s.compute_set(s.readback.buffer, 0, bytes, image->view, image->view, image->view, set, error)) return false;
  const std::uint32_t push[6] = {0u, static_cast<std::uint32_t>(x0), static_cast<std::uint32_t>(y0), width, height,
                                 FloatBits(static_cast<float>(image->scale))};
  s.dispatch(s.reducePipeline, set, push, 6, width, height);
  if (!s.submit(true, error)) return false;
  words.resize(static_cast<std::size_t>(width) * height);
  std::memcpy(words.data(), s.readback.mapped, static_cast<std::size_t>(bytes));
  s.stats.reductions++;
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
  s.outside_pass();
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
  s.outside_pass();
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

bool Gpu::ReadDisplay(std::uint32_t width, std::uint32_t height, std::vector<std::uint32_t> &pixels,
                      std::string &error, bool fxaa) {
  Impl &s = *impl_;
  const Image &source = fxaa ? s.fxaa : s.display;
  width = std::min(width, source.width);
  height = std::min(height, source.height);
  const VkDeviceSize bytes = static_cast<VkDeviceSize>(width) * height * 4u;
  if (bytes == 0 || bytes > kReadbackBytes) {
    error = "CORE2 display read outside the readback buffer";
    return false;
  }
  if (!s.begin_recording(error)) return false;
  s.outside_pass();
  VkBufferImageCopy region{};
  region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
  region.imageExtent = {width, height, 1};
  gt4_hwd_vkCmdCopyImageToBuffer(s.cmd, source.image, VK_IMAGE_LAYOUT_GENERAL, s.readback.buffer, 1, &region);
  s.full_barrier();
  if (!s.submit(true, error)) return false;
  pixels.resize(static_cast<std::size_t>(width) * height);
  std::memcpy(pixels.data(), s.readback.mapped, static_cast<std::size_t>(bytes));
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
  s.outside_pass();
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
