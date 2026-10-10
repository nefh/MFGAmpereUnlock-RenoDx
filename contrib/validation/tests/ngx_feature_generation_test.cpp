// SPDX-License-Identifier: MIT
// Production NGX wrappers, deterministic re-entry and API doubles only.
// No vendor code, live module unloading, CUDA or graphics execution.
#include "ngx_bridge.hpp"
#include <cstdio>
#include <cstdlib>

namespace n = mfgunlock::ngx;
namespace ni = n::internal;
namespace ic = mfgunlock::integration;
namespace d = mfgunlock::diagnostic;
unsigned checks = 0;
void Check(bool ok, const char* why) {
  ++checks;
  if (!ok) { std::fprintf(stderr, "FAIL: %s\n", why); std::exit(1); }
}
NVSDK_NGX_Handle handle{17};
NVSDK_NGX_Parameter parameters;
bool replace_during_evaluate = false;
unsigned forwarded = 0;
NvPhysicalGpuHandle observed_scope = nullptr;
NVSDK_NGX_Result RealCreate(ID3D12GraphicsCommandList*, NVSDK_NGX_Feature,
    NVSDK_NGX_Parameter*, NVSDK_NGX_Handle** out) {
  ++forwarded; observed_scope = ni::g_arch_scope;
  if (out) *out = &handle;
  return NVSDK_NGX_Result_Success;
}
NVSDK_NGX_Result RealRelease(NVSDK_NGX_Handle*) { ++forwarded; return NVSDK_NGX_Result_Success; }
NVSDK_NGX_Result RealEvaluate(ID3D12GraphicsCommandList*, const NVSDK_NGX_Handle*,
    const NVSDK_NGX_Parameter*, PFN_NVSDK_NGX_ProgressCallback) {
  ++forwarded;
  if (replace_during_evaluate) {
    replace_during_evaluate = false;
    ni::Release<0>(&handle);
    NVSDK_NGX_Handle* next = nullptr;
    ni::Create<0>(nullptr, NVSDK_NGX_Feature_FrameGeneration, &parameters, &next);
  }
  return NVSDK_NGX_Result_Success;
}
void Reset() {
  n::g_shutting_down = false; n::g_before_fg_create = nullptr;
  Check(n::ResetFeatureTrackingForDeviceRecreation(), "reset without in-flight calls");
  ni::g_slots[0].entry_create = RealCreate;
  ni::g_slots[0].entry_evaluate = RealEvaluate;
  ni::g_slots[0].entry_release = RealRelease;
  parameters.values["DLSSG.Reset"] = 0;
  parameters.values["DLSSG.AutomodeOverrideReset"] = 0;
}
void Create() {
  NVSDK_NGX_Handle* out = nullptr;
  Check(ni::Create<0>(nullptr, NVSDK_NGX_Feature_FrameGeneration, &parameters, &out) ==
            NVSDK_NGX_Result_Success && out == &handle, "native Create forwarded");
}
int main(int argc, char** argv) {
  const int scenario = argc > 1 ? std::atoi(argv[1]) : 0;
  Reset();
  if (!scenario || scenario == 1) {
    Create();
    ni::Evaluate<0>(nullptr, &handle, &parameters, nullptr);
    ic::ObserveNgxResource(ic::ResourceRole::kBackbuffer, true, 0x100, 1920, 1080, 28);
    ic::ObserveTag(0, ic::ResourceRole::kDepth, true, true, 0, true, 0, 0, 1920, 1080,
                   true, 1, true, 28, 0x200, true, 9);
    Check(ni::Release<0>(&handle) == NVSDK_NGX_Result_Success, "native Release forwarded");
    const auto s = ic::Read();
    Check(!s.history_reset_known && !s.resource[0].active,
          "successful D3D12 Release invalidates previous NGX history/resources");
    Check(s.resource[1].active, "independent Streamline tag is not erased by NGX Release");
  }
  Reset();
  if (!scenario || scenario == 2) {
    Create();
    replace_during_evaluate = true;
    Check(ni::Evaluate<0>(nullptr, &handle, &parameters, nullptr) == NVSDK_NGX_Result_Success,
          "re-entry does not alter vendor return value");
    Check(ni::LifecycleSnapshot().handles == 1 && !n::g_status.feature_active.load(),
          "old Evaluate completion must not activate recreated same-address feature");
    Check(!ic::Read().history_reset_known, "old Evaluate cannot publish history for new feature");
    Check(ni::LifecycleSnapshot().uncertain, "overlapping lifetime is explicitly uncertain");
  }
  Reset();
  if (!scenario || scenario == 3) {
    Create();
    ni::FgCall old_release(ni::g_slots[0], ni::FgCallKind::kRelease, &handle);
    ni::Release<0>(&handle); Create();
    old_release.Finish(NVSDK_NGX_Result_Success);
    Check(ni::IsTracked(ni::g_slots[0], &handle), "old Release cannot delete new same-address feature");
    old_release.Finish(NVSDK_NGX_Result_Success);
    Check(ni::LifecycleSnapshot().releases == 0, "Finish is idempotent; in-flight counter cannot underflow");
  }
  Reset();
  if (!scenario || scenario == 4) {
    ni::FgCall old_create(ni::g_slots[0], ni::FgCallKind::kCreate);
    ni::ClearRuntime(0);
    old_create.Finish(NVSDK_NGX_Result_Success, &handle);
    Check(ni::LifecycleSnapshot().handles == 0, "old module Create completion cannot register into reused runtime slot");
  }
  Reset();
  if (!scenario || scenario == 5) {
    const auto parent = reinterpret_cast<NvPhysicalGpuHandle>(0x123);
    ni::ScopedArchQuery fg_scope(parent);
    for (unsigned feature : {1u, 2u, 11u, 42u, 0xffffu}) {
      if (feature == static_cast<unsigned>(NVSDK_NGX_Feature_FrameGeneration)) continue;
      NVSDK_NGX_Handle* out = nullptr;
      Check(ni::Create<0>(nullptr, static_cast<NVSDK_NGX_Feature>(feature), &parameters, &out) ==
                NVSDK_NGX_Result_Success && out == &handle, "sibling feature forwarded unchanged");
      Check(!observed_scope, "non-FG Create cannot inherit parent FG architecture spoof scope");
      Check(ni::g_arch_scope == parent, "nested native scope restores parent on return");
    }
    Check(ni::LifecycleSnapshot().handles == 0, "SR/RR/NR/unknown sibling creates never tracked as FG");
  }
  Reset();
  if (!scenario || scenario == 6) {
    for (unsigned i = 0; i < 64; ++i) {
      Create();
      Check(!n::g_status.feature_active.load(), "recreated FG starts inactive");
      parameters.values["DLSSG.BackbufferFrameID"] = i;
      ni::Evaluate<0>(nullptr, &handle, &parameters, nullptr);
      Check(n::g_status.feature_active.load(), "current generation Evaluate activates FG");
      ni::Release<0>(&handle);
      Check(!n::g_status.feature_created.load() && !n::g_status.feature_active.load(), "off/on lifetime ends cleanly");
    }
    Check(!ni::LifecycleSnapshot().uncertain, "serial repeated lifecycle stays certain");
  }
  Reset();
  if (!scenario || scenario == 7) {
    Create();
    ni::FgCall old_eval(ni::g_slots[0], ni::FgCallKind::kEvaluate, &handle);
    ni::ClearRuntime(0);
    ni::g_slots[0].entry_create = RealCreate;
    Create();
    old_eval.Finish(NVSDK_NGX_Result_Success);
    Check(ni::IsTracked(ni::g_slots[0], &handle) && !n::g_status.feature_active,
          "reused module/handle address does not inherit old Evaluate completion");
  }
  Reset();
  if (!scenario || scenario == 8) {
    const auto parent = reinterpret_cast<NvPhysicalGpuHandle>(0x123);
    ni::ScopedArchQuery fg_scope(parent);
    NVSDK_NGX_Handle* out = nullptr;
    const auto result = ni::CreateVulkanFeature<0>(static_cast<NVSDK_NGX_Feature>(1), &out, [&] {
      Check(!ni::g_arch_scope, "non-FG Vulkan Create cannot inherit parent architecture scope");
      out = &handle;
      return NVSDK_NGX_Result_Success;
    });
    Check(result == NVSDK_NGX_Result_Success && out == &handle && ni::g_arch_scope == parent,
          "Vulkan sibling forwards unchanged and restores parent scope");
  }
  Reset();
  if (!scenario || scenario == 9) {
    Create();
    ni::Evaluate<0>(nullptr, &handle, &parameters, nullptr);
    ni::FgCall failed_release(ni::g_slots[0], ni::FgCallKind::kRelease, &handle);
    failed_release.Finish(NVSDK_NGX_Result_FAIL_InvalidParameter);
    Check(ni::IsTracked(ni::g_slots[0], &handle) && n::g_status.feature_active,
          "failed Release does not retire the current feature");
    Check(ic::Read().history_reset_known, "failed Release leaves current history observation intact");
  }
  std::printf("PASS: %u feature generation checks; vendor/GPU execution NOT_RUN\n", checks);
}
