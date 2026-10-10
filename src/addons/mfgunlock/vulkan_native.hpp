// SPDX-License-Identifier: MIT
#pragma once
#include <windows.h>
#include <vulkan/vulkan_core.h>
#include <nvsdk_ngx_defs_vk.h>
#include <cstddef>
#include <cstring>
#include <mutex>
#include <type_traits>
#include "./vulkan_contract.hpp"

namespace mfgunlock::vulkannative {
struct PhysicalIdentity {
  void* instance = nullptr;
  void* physical = nullptr;
  uint32_t vendor = 0;
  LUID luid{};
  bool valid = false;
};
inline std::mutex g_identity_lock;
inline PhysicalIdentity g_last_identity{};
inline void ResetIdentity() {
  std::lock_guard lock(g_identity_lock);
  g_last_identity = {};
}
// Only use an instance-dispatched command. Resolving the function from the
// caller's VkInstance avoids mixing dispatch tables. A returned pointer alone is
// not an extension-enablement proof: unknown ID data must remain unbound.
inline PhysicalIdentity Identify(void* instance, void* physical) {
  PhysicalIdentity result{};
  ResetIdentity();
  if (!instance || !physical) return result;
  const auto loader = GetModuleHandleW(L"vulkan-1.dll");
  if (!loader) return result;
  const auto get = reinterpret_cast<PFN_vkGetInstanceProcAddr>(
      GetProcAddress(loader, "vkGetInstanceProcAddr"));
  if (!get) return result;
  auto query = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties2>(
      get(reinterpret_cast<VkInstance>(instance), "vkGetPhysicalDeviceProperties2"));
  // No KHR alias fallback: its instance-extension enablement is not observed.
  if (!query) return result;
  VkPhysicalDeviceIDProperties id{};
  id.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES;
  VkPhysicalDeviceProperties2 properties{};
  properties.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
  properties.pNext = &id;
  query(reinterpret_cast<VkPhysicalDevice>(physical), &properties);
  result.instance = instance;
  result.physical = physical;
  result.vendor = properties.properties.vendorID;
  if (id.deviceLUIDValid == VK_TRUE && result.vendor == 0x10de) {
    static_assert(sizeof(result.luid) == VK_LUID_SIZE);
    std::memcpy(&result.luid, id.deviceLUID, sizeof(result.luid));
    result.valid = result.luid.LowPart != 0 || result.luid.HighPart != 0;
  }
  // Replace (including failed/non-NVIDIA observations), not a permanent
  // physical-handle allowlist. Re-check before each requirements attempt.
  { std::lock_guard lock(g_identity_lock); g_last_identity = result; }
  return result;
}
inline PhysicalIdentity KnownPhysical(void* physical) {
  std::lock_guard lock(g_identity_lock);
  return physical && physical == g_last_identity.physical ? g_last_identity : PhysicalIdentity{};
}
inline bool Readable(const void* pointer, size_t size) {
  if (!pointer || !size) return false;
  uintptr_t cursor = reinterpret_cast<uintptr_t>(pointer);
  if (size > UINTPTR_MAX - cursor) return false;
  const auto end = cursor + size;
  while (cursor < end) {
    MEMORY_BASIC_INFORMATION region{};
    if (!VirtualQuery(reinterpret_cast<const void*>(cursor), &region, sizeof(region)) ||
        region.State != MEM_COMMIT || (region.Protect & (PAGE_GUARD | PAGE_NOACCESS))) return false;
    const DWORD access = region.Protect & 0xff;
    if (access != PAGE_READONLY && access != PAGE_READWRITE && access != PAGE_WRITECOPY &&
        access != PAGE_EXECUTE_READ && access != PAGE_EXECUTE_READWRITE &&
        access != PAGE_EXECUTE_WRITECOPY) return false;
    const auto start = reinterpret_cast<uintptr_t>(region.BaseAddress);
    if (region.RegionSize > UINTPTR_MAX - start) return false;
    const auto next = start + region.RegionSize;
    if (next <= cursor) return false;
    cursor = next < end ? next : end;
  }
  return true;
}
inline bool DecodeImage(const void* pointer, vulkancontract::Image& output) {
  output = {};
  // The NGX parameter is a pointer to a Resource_VK wrapper, NOT a VkImage and
  // certainly not a COM interface. Inspect only the public descriptor bytes.
  if (!Readable(pointer, sizeof(NVSDK_NGX_Resource_VK))) return false;
  const auto* bytes = static_cast<const unsigned char*>(pointer);
  NVSDK_NGX_Resource_VK_Type type{};
  unsigned char writable = 0;
  std::memcpy(&type, bytes + offsetof(NVSDK_NGX_Resource_VK, Type), sizeof(type));
  std::memcpy(&writable, bytes + offsetof(NVSDK_NGX_Resource_VK, ReadWrite), 1);
  if (type != NVSDK_NGX_RESOURCE_VK_TYPE_VK_IMAGEVIEW || writable > 1) return false;
  NVSDK_NGX_ImageViewInfo_VK image{};
  std::memcpy(&image, bytes + offsetof(NVSDK_NGX_Resource_VK, Resource), sizeof(image));
  const auto& range = image.SubresourceRange;
  output = {vulkancontract::HandleBits(image.Image), vulkancontract::HandleBits(image.ImageView),
      static_cast<uint32_t>(image.Format), image.Width, image.Height,
      range.aspectMask, range.baseMipLevel, range.levelCount, range.baseArrayLayer, range.layerCount,
      writable != 0};
  if (!vulkancontract::ImageMetadataValid(output)) { output = {}; return false; }
  return true;
}
static_assert(std::is_standard_layout_v<NVSDK_NGX_Resource_VK>);
} // namespace mfgunlock::vulkannative
