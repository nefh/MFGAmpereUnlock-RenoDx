// SPDX-License-Identifier: MIT
// API doubles ONLY. Production compiles against Khronos Vulkan-Headers.
#pragma once
#include <cstdint>
#define VK_TRUE 1
#define VK_LUID_SIZE 8
#define VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES 1000071004
#define VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2 1000059001
using VkInstance = void*;
using VkPhysicalDevice = void*;
using VkImage = uint64_t; using VkImageView = uint64_t; using VkBuffer = uint64_t;
using VkFormat = uint32_t;
struct VkImageSubresourceRange { uint32_t aspectMask, baseMipLevel, levelCount, baseArrayLayer, layerCount; };
struct VkPhysicalDeviceIDProperties { uint32_t sType; void* pNext; uint8_t deviceUUID[16],driverUUID[16],deviceLUID[8]; uint32_t deviceNodeMask, deviceLUIDValid; };
struct VkPhysicalDeviceProperties { uint32_t vendorID; };
struct VkPhysicalDeviceProperties2 { uint32_t sType; void* pNext; VkPhysicalDeviceProperties properties; };
using PFN_vkVoidFunction = void(*)();
using PFN_vkGetInstanceProcAddr = PFN_vkVoidFunction(*)(VkInstance,const char*);
using PFN_vkGetPhysicalDeviceProperties2 = void(*)(VkPhysicalDevice,VkPhysicalDeviceProperties2*);
