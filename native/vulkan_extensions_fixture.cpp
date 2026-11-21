// CTest native_vulkan_extensions (TASK-081): the pure extension selection of vulkan_extensions.cpp.

#include "vulkan_extensions.hpp"

#include <cstdio>
#include <stdexcept>
#include <string>
#include <vector>

using gt4::vulkan::DeviceChoice;
using gt4::vulkan::InstanceChoice;
using gt4::vulkan::MaintenanceFamily;
using gt4::vulkan::parse_force_baseline;
using gt4::vulkan::select_device_extensions;
using gt4::vulkan::select_instance_extensions;
using gt4::vulkan::select_linux_instance_extensions;
using List = std::vector<std::string>;

namespace {

int failures = 0;

void expect(bool condition, const char *what) {
  if (!condition) {
    std::fprintf(stderr, "[vulkan-extensions] FAIL: %s\n", what);
    ++failures;
  }
}

template <class F> bool throws(F &&body) {
  try {
    body();
  } catch (const std::exception &) {
    return true;
  }
  return false;
}

const List kBase = {"VK_KHR_surface", "VK_KHR_win32_surface"};

List with(List list, std::initializer_list<const char *> more) {
  for (const char *name : more)
    list.emplace_back(name);
  return list;
}

} // namespace

int main() {
  // The switch.
  expect(!parse_force_baseline(nullptr) && !parse_force_baseline("") && !parse_force_baseline("0"), "unset/0 = auto");
  expect(parse_force_baseline("1"), "1 forces the baseline");
  expect(throws([] { parse_force_baseline("yes"); }), "other values are refused");

  // Instance: required pair only on an old driver.
  {
    const InstanceChoice old = select_instance_extensions(with(kBase, {"VK_EXT_debug_utils"}), false);
    expect(old.extensions == kBase && old.family == MaintenanceFamily::none, "old driver: surface + win32 only");
  }
  // Instance: KHR family (this machine, 2025+ drivers).
  const List khr_instance = with(kBase, {"VK_KHR_get_surface_capabilities2", "VK_KHR_surface_maintenance1",
                                         "VK_EXT_surface_maintenance1"});
  {
    const InstanceChoice khr = select_instance_extensions(khr_instance, false);
    expect(khr.family == MaintenanceFamily::khr, "KHR family preferred");
    expect(khr.extensions == with(kBase, {"VK_KHR_get_surface_capabilities2", "VK_KHR_surface_maintenance1"}),
           "KHR instance list");
    const InstanceChoice forced = select_instance_extensions(khr_instance, true);
    expect(forced.extensions == kBase && forced.family == MaintenanceFamily::none, "forced baseline drops both");
  }
  // Instance: EXT family (2023-2024 drivers).
  {
    const InstanceChoice ext = select_instance_extensions(
        with(kBase, {"VK_KHR_get_surface_capabilities2", "VK_EXT_surface_maintenance1"}), false);
    expect(ext.family == MaintenanceFamily::ext, "EXT family");
    expect(ext.extensions == with(kBase, {"VK_KHR_get_surface_capabilities2", "VK_EXT_surface_maintenance1"}),
           "EXT instance list");
  }
  // Instance: surface_maintenance1 without its prerequisite is not used.
  {
    const InstanceChoice lone = select_instance_extensions(with(kBase, {"VK_KHR_surface_maintenance1"}), false);
    expect(lone.family == MaintenanceFamily::none && lone.extensions == kBase, "no capabilities2: baseline");
  }
  // Instance: missing required extensions refuse.
  expect(throws([] { select_instance_extensions({"VK_KHR_surface"}, false); }), "no win32 surface refuses");
  expect(throws([] { select_instance_extensions({"VK_KHR_win32_surface"}, true); }), "no surface refuses");

  // Device.
  const List swapchain = {"VK_KHR_swapchain"};
  const List both_device = {"VK_KHR_swapchain", "VK_KHR_swapchain_maintenance1", "VK_EXT_swapchain_maintenance1"};
  {
    const DeviceChoice fast = select_device_extensions(both_device, MaintenanceFamily::khr, true, false);
    expect(fast.present_fences && fast.extensions == List({"VK_KHR_swapchain", "VK_KHR_swapchain_maintenance1"}) &&
               std::string(fast.path) == "khr-maintenance1",
           "KHR device path");
    const DeviceChoice ext = select_device_extensions(both_device, MaintenanceFamily::ext, true, false);
    expect(ext.present_fences && ext.extensions == List({"VK_KHR_swapchain", "VK_EXT_swapchain_maintenance1"}) &&
               std::string(ext.path) == "ext-maintenance1",
           "EXT device path matches the instance family");
    const DeviceChoice no_feature = select_device_extensions(both_device, MaintenanceFamily::khr, false, false);
    expect(!no_feature.present_fences && no_feature.extensions == swapchain, "feature off: baseline");
    const DeviceChoice no_instance = select_device_extensions(both_device, MaintenanceFamily::none, true, false);
    expect(!no_instance.present_fences && no_instance.extensions == swapchain, "instance baseline: device baseline");
    const DeviceChoice old = select_device_extensions(swapchain, MaintenanceFamily::khr, false, false);
    expect(!old.present_fences && std::string(old.path) == "baseline", "old device: baseline");
    const DeviceChoice mismatch =
        select_device_extensions({"VK_KHR_swapchain", "VK_EXT_swapchain_maintenance1"}, MaintenanceFamily::khr, true,
                                 false);
    expect(!mismatch.present_fences, "KHR instance with an EXT-only device: baseline");
    const DeviceChoice forced = select_device_extensions(both_device, MaintenanceFamily::khr, true, true);
    expect(!forced.present_fences && forced.extensions == swapchain, "forced baseline on the device");
  }
  expect(throws([] { select_device_extensions({}, MaintenanceFamily::none, false, false); }),
         "no VK_KHR_swapchain refuses");

  // TASK-074 L2: the Linux selection (pure, so it is checked on every host).
  {
    const List x11_wayland = {"VK_KHR_surface", "VK_KHR_xlib_surface", "VK_KHR_wayland_surface", "VK_KHR_xcb_surface"};
    const InstanceChoice both = select_linux_instance_extensions(x11_wayland, false);
    expect(both.extensions == List({"VK_KHR_surface", "VK_KHR_xlib_surface", "VK_KHR_wayland_surface"}) &&
               both.family == MaintenanceFamily::none,
           "Linux: surface plus every window-system extension, xcb not used");
    const InstanceChoice wayland_only = select_linux_instance_extensions({"VK_KHR_surface", "VK_KHR_wayland_surface"}, false);
    expect(wayland_only.extensions == List({"VK_KHR_surface", "VK_KHR_wayland_surface"}), "Linux: Wayland only");
    const List maintained = with(x11_wayland, {"VK_KHR_get_surface_capabilities2", "VK_KHR_surface_maintenance1",
                                               "VK_EXT_surface_maintenance1"});
    const InstanceChoice khr = select_linux_instance_extensions(maintained, false);
    expect(khr.family == MaintenanceFamily::khr && khr.extensions.size() == 5 &&
               khr.extensions[3] == "VK_KHR_get_surface_capabilities2" && khr.extensions[4] == "VK_KHR_surface_maintenance1",
           "Linux: the KHR maintenance family, as on Windows");
    expect(select_linux_instance_extensions(maintained, true).family == MaintenanceFamily::none,
           "Linux: forced baseline");
    expect(select_linux_instance_extensions({"VK_KHR_surface"}, false).extensions.empty(),
           "Linux: no window-system extension gives a headless instance");
    expect(select_linux_instance_extensions({}, false).extensions.empty(), "Linux: no surface gives a headless instance");
    expect(throws([] { select_instance_extensions({"VK_KHR_surface", "VK_KHR_xlib_surface"}, false); }),
           "the Win32 selection still refuses a driver without VK_KHR_win32_surface");
  }

  if (failures) {
    std::fprintf(stderr, "[vulkan-extensions] %d failure(s)\n", failures);
    return 1;
  }
  std::fprintf(stderr, "[vulkan-extensions] PASS\n");
  return 0;
}
