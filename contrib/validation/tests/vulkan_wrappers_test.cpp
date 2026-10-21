// SPDX-License-Identifier: MIT
// Real wrappers, deterministic API doubles. Not a Vulkan driver/NGX GPU test.
#include "ngx_bridge.hpp"
#include <cstdio>
#include <cstdlib>
#include <vector>

namespace n = mfgunlock::ngx;
namespace ni = n::internal;
namespace d = mfgunlock::diagnostic;
namespace ic = mfgunlock::integration;

unsigned int g_checks = 0;
unsigned int g_creates = 0, g_evaluates = 0, g_releases = 0, g_barriers = 0;
NVSDK_NGX_Handle g_handle{10};
NVSDK_NGX_Result g_result = NVSDK_NGX_Result_Success;
VkCommandBuffer_T* g_commands = reinterpret_cast<VkCommandBuffer_T*>(0x100);
VkDevice_T* g_device = reinterpret_cast<VkDevice_T*>(0x200);
PFN_NVSDK_NGX_ProgressCallback g_callback = nullptr;
std::vector<d::EvaluateEvent> g_events;

void Check(bool condition, const char* why) {
  ++g_checks;
  if (condition) return;
  std::fprintf(stderr, "FAIL: %s\n", why);
  std::exit(1);
}
bool AllowCreate() { ++g_barriers; return true; }
bool RejectCreate() { return false; }
void Observe(const d::EvaluateEvent* event) noexcept { g_events.push_back(*event); }
NVSDK_NGX_Result RealCreate(VkCommandBuffer_T* commands, NVSDK_NGX_Feature,
                           NVSDK_NGX_Parameter*, NVSDK_NGX_Handle** handle) {
  Check(commands == g_commands, "command buffer is forwarded unchanged");
  ++g_creates;
  if (handle) *handle = g_result == NVSDK_NGX_Result_Success ? &g_handle : nullptr;
  return g_result;
}
NVSDK_NGX_Result RealCreate1(VkDevice_T* device, VkCommandBuffer_T* commands,
    NVSDK_NGX_Feature feature, NVSDK_NGX_Parameter* parameters, NVSDK_NGX_Handle** handle) {
  Check(device == g_device, "explicit Vulkan device forwarded unchanged");
  return ni::VulkanCreate<0>(commands, feature, parameters, handle);
}
NVSDK_NGX_Result RealEvaluate(VkCommandBuffer_T* commands, const NVSDK_NGX_Handle* handle,
    const NVSDK_NGX_Parameter*, PFN_NVSDK_NGX_ProgressCallback callback) {
  Check(commands == g_commands && handle == &g_handle && callback == g_callback,
        "evaluate forwards command buffer, handle and callback unchanged");
  ++g_evaluates;
  return g_result;
}
NVSDK_NGX_Result RealRelease(NVSDK_NGX_Handle* handle) {
  Check(handle == &g_handle, "release forwards original handle");
  ++g_releases;
  return g_result;
}

int main() {
  n::g_shutting_down = false;
  n::ResetFeatureTrackingForDeviceRecreation();
  auto& slot = ni::g_slots[0];
  slot.entry_vulkan_create = RealCreate;
  slot.entry_vulkan_create1 = RealCreate1;
  slot.entry_vulkan_evaluate = RealEvaluate;
  slot.entry_vulkan_release = RealRelease;
  n::g_before_fg_create = AllowCreate;
  d::SetEvaluateCallback(Observe);
  NVSDK_NGX_Parameter parameters;
  parameters.values["DLSSG.Reset"] = 0;
  parameters.values["DLSSG.AutomodeOverrideReset"] = 0;
  parameters.values["DLSSG.BackbufferFrameID"] = 77;
  // A Vulkan parameter block is not a bag of COM resources. Dereferencing this
  // through the D3D12 GetDesc path would crash (including under sanitizers).
  parameters.resources["DLSSG.Backbuffer"] = reinterpret_cast<ID3D12Resource*>(1);
  NVSDK_NGX_Handle* output = nullptr;
  Check(ni::VulkanCreate1<0>(g_device, g_commands, NVSDK_NGX_Feature_FrameGeneration,
        &parameters, &output) == NVSDK_NGX_Result_Success && output == &g_handle,
        "Vulkan create1 succeeds through native create");
  Check(g_creates == 1 && g_barriers == 1 && ni::LifecycleSnapshot().handles == 1 &&
        !ni::LifecycleSnapshot().uncertain, "nested CreateFeature1 is tracked once");
  Check(ni::IsTracked(slot, &g_handle, d::Backend::kNgxVulkan) &&
        !ni::IsTracked(slot, &g_handle), "Vulkan identity is not a D3D12 handle");
  Check(ni::VulkanEvaluate<0>(g_commands, &g_handle, &parameters, g_callback) ==
        NVSDK_NGX_Result_Success && g_evaluates == 1, "Vulkan evaluate calls provider once");
  Check(g_events.size() == 2 && g_events.front().backend == d::Backend::kNgxVulkan &&
        g_events.back().phase == d::EvaluatePhase::kEnd &&
        g_events.back().backbuffer_frame_id_known && g_events.back().backbuffer_frame_id == 77,
        "Vulkan scalar frame evidence is retained without resource reinterpretation");
  for (const auto& resource : g_events.back().resources)
    Check(!resource.known, "unverified Vulkan resource layout remains unknown");
  Check(ni::ReadVulkanObservation().snapshot_current, "current Vulkan evaluation snapshot published");
  Check(parameters.writes == 0 && ic::Read().history_reset_known &&
        !ic::Read().rendering_game_frames_known, "no parameter writes or invented gameplay evidence");

  ic::ObserveTag(0, ic::ResourceRole::kDepth, true, true, 0, true, 0, 0, 1, 1,
                 true, 1, true, 1, 0x300, true, 77);
  ic::ObserveNgxResource(ic::ResourceRole::kBackbuffer, true, 0x400, 1, 1, 28);
  ni::VulkanEvaluate<0>(g_commands, &g_handle, &parameters, g_callback);
  Check(ic::Read().resource[static_cast<size_t>(ic::ResourceRole::kDepth)].active &&
        !ic::Read().resource[0].active,
        "Vulkan drops stale NGX/COM metadata but preserves independent Streamline tags");
  g_result = NVSDK_NGX_Result_FAIL_InvalidParameter;
  ni::VulkanEvaluate<0>(g_commands, &g_handle, &parameters, g_callback);
  Check(!ic::Read().history_reset_known && !ic::Read().resource[1].active,
        "failed Vulkan evaluate cannot keep stale frame/history evidence");
  g_result = NVSDK_NGX_Result_Success;
  // An identically-valued D3D12 handle is a separate identity domain.
  ni::FgCall d3d(slot, ni::FgCallKind::kCreate);
  d3d.Finish(NVSDK_NGX_Result_Success, &g_handle);
  Check(ni::LifecycleSnapshot().handles == 2 && !ni::LifecycleSnapshot().uncertain,
        "equal handle pointers across APIs are not duplicate registration");
  Check(ni::VulkanRelease<0>(&g_handle) == NVSDK_NGX_Result_Success &&
        ni::LifecycleSnapshot().handles == 1 && ni::IsTracked(slot, &g_handle),
        "Vulkan release cannot remove the D3D12 handle");
  Check(!ni::ReadVulkanObservation().snapshot_current,
        "release invalidates Vulkan descriptor snapshot without erasing historical counters");
  ni::FgCall d3d_release(slot, ni::FgCallKind::kRelease, &g_handle);
  d3d_release.Finish(NVSDK_NGX_Result_Success);
  Check(ni::LifecycleSnapshot().handles == 0, "both handles released independently");

  n::g_before_fg_create = RejectCreate;
  const auto before = g_creates;
  output = &g_handle;
  Check(ni::VulkanCreate<0>(g_commands, NVSDK_NGX_Feature_FrameGeneration, &parameters, &output) ==
        NVSDK_NGX_Result_FAIL_FeatureNotSupported && !output && g_creates == before,
        "provider preflight fails closed without native create");
  Check(ni::VulkanCreate<0>(g_commands, static_cast<NVSDK_NGX_Feature>(1), &parameters, &output) ==
        NVSDK_NGX_Result_Success && ni::LifecycleSnapshot().handles == 0,
        "unrelated features stay native and untracked");
  n::g_before_fg_create = AllowCreate;
  g_result = NVSDK_NGX_Result_FAIL_InvalidParameter;
  Check(ni::VulkanCreate<0>(g_commands, NVSDK_NGX_Feature_FrameGeneration, &parameters, &output) ==
        g_result && ni::LifecycleSnapshot().handles == 0, "failed create is not a live handle");
  g_result = NVSDK_NGX_Result_Success;
  ni::VulkanCreate<0>(g_commands, NVSDK_NGX_Feature_FrameGeneration, &parameters, &output);
  n::ResetFeatureTrackingForDeviceRecreation();
  Check(!ni::IsTracked(slot, &g_handle, d::Backend::kNgxVulkan), "device reset invalidates Vulkan handles");
  const auto events = g_events.size();
  ni::VulkanEvaluate<0>(g_commands, &g_handle, &parameters, g_callback);
  Check(g_events.size() == events, "stale/untracked feature forwards without false FG evidence");
  n::g_shutting_down = true;
  ni::VulkanCreate<0>(g_commands, NVSDK_NGX_Feature_FrameGeneration, &parameters, &output);
  Check(ni::LifecycleSnapshot().handles == 0, "shutdown does not register new handles");
  Check(ni::IsTargetName("NVSDK_NGX_VULKAN_CreateFeature1") &&
        ni::IsTargetName("NVSDK_NGX_VULKAN_EvaluateFeature") &&
        !ni::IsTargetName("NVSDK_NGX_VULKAN_EvaluateFeature_C"),
        "only the verified C++ callback ABI is intercepted");
  Check(ni::ShouldHookEntry("NVSDK_NGX_D3D12_GetFeatureRequirements", false) &&
        ni::ShouldHookEntry("NVSDK_NGX_VULKAN_GetFeatureRequirements", false) &&
        ni::ShouldHookEntry("NVSDK_NGX_D3D12_CreateFeature", false) &&
        ni::ShouldHookEntry("NVSDK_NGX_VULKAN_EvaluateFeature", false),
        "typed admission and lifecycle observers remain available without a retarget profile");
  Check(ni::ShouldHookEntry("NVSDK_NGX_VULKAN_GetFeatureRequirements", true),
        "retarget architectures retain their requirements bridge");
  d::SetEvaluateCallback(nullptr);
  std::printf("vulkan_wrappers_test PASS (%u checks; API doubles, not driver execution)\n", g_checks);
}
