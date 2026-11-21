#include "vulkan_extensions.hpp"

#include <algorithm>
#include <cstring>
#include <stdexcept>

namespace gt4::vulkan {
namespace {

bool has(const std::vector<std::string> &available, const char *name) {
  return std::find(available.begin(), available.end(), name) != available.end();
}

// The swapchain-maintenance instance prerequisites, shared by the Win32 and the Linux selection.
void add_surface_maintenance(InstanceChoice &choice, const std::vector<std::string> &available, bool force_baseline) {
  if (force_baseline || !has(available, "VK_KHR_get_surface_capabilities2"))
    return;
  // Prefer the KHR family (promoted 2025); the EXT family has the same structures and values.
  if (has(available, "VK_KHR_surface_maintenance1")) {
    choice.extensions.emplace_back("VK_KHR_get_surface_capabilities2");
    choice.extensions.emplace_back("VK_KHR_surface_maintenance1");
    choice.family = MaintenanceFamily::khr;
  } else if (has(available, "VK_EXT_surface_maintenance1")) {
    choice.extensions.emplace_back("VK_KHR_get_surface_capabilities2");
    choice.extensions.emplace_back("VK_EXT_surface_maintenance1");
    choice.family = MaintenanceFamily::ext;
  }
}

} // namespace

const char *maintenance_family_name(MaintenanceFamily family) {
  switch (family) {
  case MaintenanceFamily::khr: return "khr";
  case MaintenanceFamily::ext: return "ext";
  case MaintenanceFamily::none: return "none";
  }
  return "none";
}

bool parse_force_baseline(const char *value) {
  if (!value || !*value || std::strcmp(value, "0") == 0)
    return false;
  if (std::strcmp(value, "1") == 0)
    return true;
  throw std::invalid_argument("GT4_VULKAN_FORCE_BASELINE must be 0 or 1");
}

InstanceChoice select_instance_extensions(const std::vector<std::string> &available, bool force_baseline) {
  InstanceChoice choice;
  for (const char *required : {"VK_KHR_surface", "VK_KHR_win32_surface"}) {
    if (!has(available, required))
      throw std::runtime_error(std::string("Vulkan driver lacks the required instance extension ") + required +
                               "; update the graphics driver");
    choice.extensions.emplace_back(required);
  }
  add_surface_maintenance(choice, available, force_baseline);
  return choice;
}

InstanceChoice select_linux_instance_extensions(const std::vector<std::string> &available, bool force_baseline) {
  InstanceChoice choice;
  if (!has(available, "VK_KHR_surface"))
    return choice;
  std::vector<std::string> platforms;
  for (const char *platform : {"VK_KHR_xlib_surface", "VK_KHR_wayland_surface"})
    if (has(available, platform))
      platforms.emplace_back(platform);
  if (platforms.empty())
    return choice;
  choice.extensions.emplace_back("VK_KHR_surface");
  choice.extensions.insert(choice.extensions.end(), platforms.begin(), platforms.end());
  add_surface_maintenance(choice, available, force_baseline);
  return choice;
}

const char *swapchain_maintenance_extension(MaintenanceFamily family) {
  switch (family) {
  case MaintenanceFamily::khr: return "VK_KHR_swapchain_maintenance1";
  case MaintenanceFamily::ext: return "VK_EXT_swapchain_maintenance1";
  case MaintenanceFamily::none: return nullptr;
  }
  return nullptr;
}

DeviceChoice select_device_extensions(const std::vector<std::string> &available, MaintenanceFamily instance_family,
                                      bool swapchain_maintenance_feature, bool force_baseline) {
  if (!has(available, "VK_KHR_swapchain"))
    throw std::runtime_error("Vulkan GPU lacks VK_KHR_swapchain; update the graphics driver");
  DeviceChoice choice;
  choice.extensions.emplace_back("VK_KHR_swapchain");
  const char *maintenance = swapchain_maintenance_extension(instance_family);
  if (force_baseline || !maintenance || !has(available, maintenance) || !swapchain_maintenance_feature)
    return choice;
  choice.extensions.emplace_back(maintenance);
  choice.present_fences = true;
  choice.path = instance_family == MaintenanceFamily::khr ? "khr-maintenance1" : "ext-maintenance1";
  return choice;
}

} // namespace gt4::vulkan
