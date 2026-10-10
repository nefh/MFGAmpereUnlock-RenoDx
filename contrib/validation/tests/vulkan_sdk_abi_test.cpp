// SPDX-License-Identifier: MIT
// Actual SDK C++ signature/layout check. Never creates a Vulkan device or loads NGX.
#include <vulkan/vulkan_core.h>
#include <nvsdk_ngx_vk.h>
#include <cstddef>
#include <type_traits>
#include <cstdio>
using Requirements = NVSDK_NGX_Result(NVSDK_CONV*)(VkInstance,VkPhysicalDevice,
    const NVSDK_NGX_FeatureDiscoveryInfo*,NVSDK_NGX_FeatureRequirement*);
using Create = NVSDK_NGX_Result(NVSDK_CONV*)(VkCommandBuffer,NVSDK_NGX_Feature,
    NVSDK_NGX_Parameter*,NVSDK_NGX_Handle**);
using Create1 = NVSDK_NGX_Result(NVSDK_CONV*)(VkDevice,VkCommandBuffer,NVSDK_NGX_Feature,
    NVSDK_NGX_Parameter*,NVSDK_NGX_Handle**);
using Evaluate = NVSDK_NGX_Result(NVSDK_CONV*)(VkCommandBuffer,const NVSDK_NGX_Handle*,
    const NVSDK_NGX_Parameter*,PFN_NVSDK_NGX_ProgressCallback);
static_assert(std::is_same_v<Requirements,decltype(&NVSDK_NGX_VULKAN_GetFeatureRequirements)>);
static_assert(std::is_same_v<Create,decltype(&NVSDK_NGX_VULKAN_CreateFeature)>);
static_assert(std::is_same_v<Create1,decltype(&NVSDK_NGX_VULKAN_CreateFeature1)>);
static_assert(std::is_same_v<Evaluate,decltype(&NVSDK_NGX_VULKAN_EvaluateFeature)>);
static_assert(std::is_standard_layout_v<NVSDK_NGX_Resource_VK>);
static_assert(sizeof(VkImage)==8&&sizeof(VkImageView)==8&&VK_LUID_SIZE==8);
static_assert(offsetof(NVSDK_NGX_Resource_VK,Resource)==0);
int main(){printf("Vulkan SDK ABI PASS; VK_HEADER_VERSION=%u NGX_Resource_VK=%zu; GPU NOT_RUN\n",VK_HEADER_VERSION,sizeof(NVSDK_NGX_Resource_VK));}
