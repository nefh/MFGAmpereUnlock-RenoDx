// SPDX-License-Identifier: MIT
// Test descriptor double. SDK ABI qualification is a separate native compile target.
#pragma once
#include <vulkan/vulkan_core.h>
enum NVSDK_NGX_Resource_VK_Type { NVSDK_NGX_RESOURCE_VK_TYPE_VK_IMAGEVIEW, NVSDK_NGX_RESOURCE_VK_TYPE_VK_BUFFER };
struct NVSDK_NGX_ImageViewInfo_VK { VkImageView ImageView; VkImage Image; VkImageSubresourceRange SubresourceRange; VkFormat Format; unsigned Width,Height; };
struct NVSDK_NGX_BufferInfo_VK { VkBuffer Buffer; unsigned SizeInBytes; };
struct NVSDK_NGX_Resource_VK { union { NVSDK_NGX_ImageViewInfo_VK ImageViewInfo; NVSDK_NGX_BufferInfo_VK BufferInfo; } Resource; NVSDK_NGX_Resource_VK_Type Type; bool ReadWrite; };
