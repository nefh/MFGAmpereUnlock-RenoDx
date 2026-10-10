// SPDX-License-Identifier: MIT
// Production NGX bridge against explicit API doubles, not a Vulkan driver.
#include "ngx_bridge.hpp"
#include "streamline_bridge.hpp"
#include <cassert>
#include <cstdio>
namespace n=mfgunlock::ngx::internal;
namespace v=mfgunlock::vulkannative;
namespace d=mfgunlock::diagnostic;
void* instance=reinterpret_cast<void*>(0x3330);
void* physical=reinterpret_cast<void*>(0x4440);
bool valid_luid=true;uint32_t vendor=0x10de;unsigned calls=0;
void Query(VkPhysicalDevice p,VkPhysicalDeviceProperties2* properties) {
  assert(p==physical);++calls; properties->properties.vendorID=vendor;
  auto* id=static_cast<VkPhysicalDeviceIDProperties*>(properties->pNext);
  id->deviceLUIDValid=valid_luid;
  const LUID luid{17,23}; std::memcpy(id->deviceLUID,&luid,8);
}
PFN_vkVoidFunction Get(VkInstance i,const char* name) {
  assert(i==instance);
  return std::strcmp(name,"vkGetPhysicalDeviceProperties2")==0
      ? reinterpret_cast<PFN_vkVoidFunction>(Query) : nullptr;
}
int main() {
  const auto loader=reinterpret_cast<HMODULE>(0x5550);
  mock::modules[L"vulkan-1.dll"]=loader;
  mock::exports[{loader,"vkGetInstanceProcAddr"}]=reinterpret_cast<FARPROC>(Get);
  auto id=v::Identify(instance,physical);
  assert(id.valid&&id.luid.LowPart==17&&id.luid.HighPart==23&&calls==1);
  assert(!v::KnownPhysical(reinterpret_cast<void*>(0x9990)).valid);
  n::g_luid_low=17;n::g_luid_high=23;
  sl::AdapterInfo adapter{};adapter.vkPhysicalDevice=physical;
  LUID wrong{99,1};adapter.deviceLUID=&wrong;adapter.deviceLUIDSizeInBytes=8;
  assert(mfgunlock::streamline::internal::SupportAdapterMatchesBoundGpu(adapter));
  adapter.vkPhysicalDevice=reinterpret_cast<void*>(0x9990);
  LUID right{17,23};adapter.deviceLUID=&right;
  assert(!mfgunlock::streamline::internal::SupportAdapterMatchesBoundGpu(adapter));
  vendor=0x8086;id=v::Identify(instance,physical);assert(!id.valid);
  assert(!mfgunlock::streamline::internal::SupportAdapterMatchesBoundGpu(adapter));
  vendor=0x10de;valid_luid=false;id=v::Identify(instance,physical);assert(!id.valid);
  v::ResetIdentity();assert(!v::KnownPhysical(physical).valid);

  NVSDK_NGX_Resource_VK resource{};
  resource.Type=NVSDK_NGX_RESOURCE_VK_TYPE_VK_IMAGEVIEW;
  resource.Resource.ImageViewInfo={2,1,{1,0,1,0,1},37,128,72};
  mock::readable_regions[reinterpret_cast<uintptr_t>(&resource)]=sizeof(resource);
  NVSDK_NGX_Parameter params;params.vk_resources["DLSSG.Backbuffer"]=&resource;
  params.vk_resources["DLSSG.UI"]=nullptr;
  n::VulkanImages images{};
  d::EvaluateEvent event{};n::SnapshotVulkanEvaluate(&params,event,&images);
  assert(images[0].image==1&&images[0].view==2&&images[0].mip_count==1&&images[0].layer_count==1);
  assert(event.resources[0].known&&event.resources[0].object==1&&event.resources[0].format==37);
  assert(event.resources[4].known&&!event.resources[4].object);
  assert(!event.resources[0].sample_count&&!event.resources[0].dimension);
  // Validate the bool representation before a typed read (UBSan must not trip).
  auto* raw = reinterpret_cast<unsigned char*>(&resource);
  raw[offsetof(NVSDK_NGX_Resource_VK, ReadWrite)] = 2;
  event={};n::SnapshotVulkanEvaluate(&params,event);assert(!event.resources[0].known);
  raw[offsetof(NVSDK_NGX_Resource_VK, ReadWrite)] = 0;
  resource.Resource.ImageViewInfo.SubresourceRange.levelCount=0;
  event={};n::SnapshotVulkanEvaluate(&params,event);assert(!event.resources[0].known);
  resource.Type=NVSDK_NGX_RESOURCE_VK_TYPE_VK_BUFFER;
  event={};n::SnapshotVulkanEvaluate(&params,event);assert(!event.resources[0].known);
  params.vk_resources["DLSSG.Backbuffer"]=reinterpret_cast<void*>(1);
  event={};n::SnapshotVulkanEvaluate(&params,event);assert(!event.resources[0].known);
  assert(!params.writes);
  puts("vulkan_native_test PASS (physical identity + public resource decode; API doubles)");
}
