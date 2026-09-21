// SPDX-License-Identifier: MIT
#include "framecount.hpp"
#include "ngx_bridge.hpp"
#include "quality_config.hpp"

#include <cstdio>
#include <stdexcept>
#include <thread>
#include <vector>

namespace fc = mfgunlock::framecount;
namespace ngx = mfgunlock::ngx;
namespace qc = mfgunlock::qualityconfig;
namespace dg = mfgunlock::diagnostic;
namespace {
unsigned g_checks = 0;
std::vector<unsigned> g_counts;
sl::DLSSGMode g_mode{};
float g_target = 0;
uint32_t g_ui_format = 0;
sl::Boolean g_ui_recomposition = sl::Boolean::eFalse;
unsigned g_constants = 0;
bool g_throw = false;
bool g_reenter = false;
sl::Result g_result = sl::Result::eOk;
std::array<NVSDK_NGX_Handle, 70> g_handles{};
unsigned g_next_handle = 0;
unsigned g_creates = 0;
NVSDK_NGX_Result g_ngx_result = NVSDK_NGX_Result_Success;
std::vector<dg::LifecycleEvent> g_events;
void Check(bool value, const char* reason) {
  ++g_checks;
  if (!value) throw std::runtime_error(reason);
}
void Observe(const dg::LifecycleEvent* event) noexcept {
  // This would deadlock if the producer called observers under its inventory lock.
  const auto snapshot = ngx::internal::LifecycleSnapshot();
  (void)snapshot;
  g_events.push_back(*event);
}
sl::Result SetOptions(const sl::ViewportHandle& viewport, const sl::DLSSGOptions& options) {
  g_counts.push_back(options.numFramesToGenerate);
  g_mode = options.mode;
  g_target = options.dynamicTargetFrameRate;
  g_ui_format = options.uiBufferFormat;
  g_ui_recomposition = options.enableUserInterfaceRecomposition;
  if (g_throw) throw std::runtime_error("injected API exception");
  if (g_reenter) {
    g_reenter = false;
    fc::internal::HookedSetOptions(viewport, options);
  }
  return g_result;
}
sl::Result SetConstants(const sl::Constants&, const sl::FrameToken&, const sl::ViewportHandle&) {
  ++g_constants;
  return sl::Result::eOk;
}
NVSDK_NGX_Result NVSDK_CONV Create(ID3D12GraphicsCommandList*, NVSDK_NGX_Feature,
                                    NVSDK_NGX_Parameter*, NVSDK_NGX_Handle** output) {
  ++g_creates;
  Check(ngx::internal::LifecycleSnapshot().creates != 0, "Create is counted during original API");
  if (g_throw) throw std::runtime_error("injected Create exception");
  *output = &g_handles[g_next_handle++];
  return g_ngx_result;
}
NVSDK_NGX_Result NVSDK_CONV Evaluate(ID3D12GraphicsCommandList*, const NVSDK_NGX_Handle*,
                                      const NVSDK_NGX_Parameter*, PFN_NVSDK_NGX_ProgressCallback) {
  Check(ngx::internal::LifecycleSnapshot().evaluates != 0, "Evaluate is counted during original API");
  if (g_throw) throw std::runtime_error("injected Evaluate exception");
  return g_ngx_result;
}
NVSDK_NGX_Result NVSDK_CONV Release(NVSDK_NGX_Handle*) {
  Check(ngx::internal::LifecycleSnapshot().releases != 0, "Release is counted during original API");
  if (g_throw) throw std::runtime_error("injected Release exception");
  return g_ngx_result;
}
void Reset() {
  mfgunlock::architecture::Configure(mfgunlock::Architecture::kTuring);
  mfgunlock::g_enabled = true;
  fc::g_force_multiplier = 0;
  fc::g_options_revision = 1;
  fc::g_options_cache_epoch = 1;
  fc::g_present_thread = 7;
  fc::g_dynamic_mfg_enabled = false;
  fc::g_dynamic_applied = false;
  fc::g_dynamic_support_seen = true;
  fc::g_dynamic_supported = true;
  fc::g_dynamic_d3d12 = true;
  fc::g_dynamic_runtime_declined = false;
  fc::g_dynamic_target_fps = 0;
  fc::g_streamline_plugin_seen = true;
  fc::g_streamline_max_generated = 5;
  fc::g_multi_frame_ready = [] { return true; };
  fc::g_dynamic_stack_ready = [] { return true; };
  fc::g_pacing_ready = [] { return true; };
  fc::g_ensure_pacing = nullptr;
  fc::g_ui_composition_enabled = false;
  fc::g_hdr_state_seen = false;
  fc::g_hdr_active = false;
  fc::g_output_color_space_seen = false;
  fc::g_output_color_space = dg::kUnknown32;
  fc::g_ui_candidate_injection_enabled = true;
  fc::g_ui_candidate_ready = false;
  fc::g_ui_candidate_format = 0;
  fc::g_ui_candidate_options_synced = false;
  fc::g_ui_candidate_runtime_declined = false;
  fc::internal::g_entry_shutting_down = false;
  fc::internal::g_real_set_options = SetOptions;
  fc::internal::g_real_set_constants = SetConstants;
  for (auto& state : fc::internal::g_ui_viewports) {
    state.key = fc::internal::kUnusedViewport;
    state.native_options_valid = false;
    state.options_in_call = false;
    state.options_seen = false;
    state.attempted_revision = 0;
  }
  ngx::g_shutting_down = false;
  ngx::g_before_fg_create = nullptr;
  ngx::g_create_seen = false;
  ngx::internal::g_lifecycle = {};
  for (auto& slot : ngx::internal::g_slots) {
    slot.fg_handles.fill(nullptr);
    slot.fg_active.fill(false);
  }
  ngx::internal::UpdateFeatureStatus();
  auto& slot = ngx::internal::g_slots[0];
  slot.entry_create = Create;
  slot.entry_evaluate = Evaluate;
  slot.entry_release = Release;
  g_next_handle = g_creates = g_constants = 0;
  g_ui_format = 0;
  g_ui_recomposition = sl::Boolean::eFalse;
  g_throw = g_reenter = false;
  g_result = sl::Result::eOk;
  g_ngx_result = NVSDK_NGX_Result_Success;
  g_counts.clear();
  g_events.clear();
  dg::g_lifecycle_callback = Observe;
  g_mock_thread_id = 7;
}
void Boundary(uint32_t viewport = 0) {
  sl::Constants constants{};
  sl::FrameToken frame{};
  fc::internal::HookedSetConstants(constants, frame, sl::ViewportHandle(viewport));
}
void Request(unsigned count) {
  fc::g_force_multiplier = count;
  fc::NotifyFixedMultiplierChanged();
}
void LiveOptions() {
  for (auto profile : {mfgunlock::Architecture::kAmpere, mfgunlock::Architecture::kTuring}) {
    Reset();
    mfgunlock::architecture::Configure(profile);
    sl::DLSSGOptions native{};
    native.numFramesToGenerate = 1;
    fc::internal::HookedSetOptions({}, native);
    Request(4);
    Check(g_counts.size() == 1, "UI edit never invokes the vendor on the UI thread");
    Boundary();
    Check(g_counts.size() == 2 && g_counts.back() == 3, "next native boundary reapplies fixed count");
    Check(native.numFramesToGenerate == 1, "native caller storage unchanged");
    Boundary();
    Check(g_counts.size() == 2, "no vendor call without another edit");
    Request(6); Request(2); Request(4);
    Boundary();
    Check(g_counts.size() == 3 && g_counts.back() == 3, "edits coalesce to latest request");
    fc::g_dynamic_mfg_enabled = true;
    fc::NotifyDynamicModeChanged();
    Boundary();
    Check(g_mode == sl::DLSSGMode::eDynamic && fc::g_dynamic_applied, "Dynamic can apply without FG cycle");
    fc::g_dynamic_target_fps = 144;
    fc::NotifyDynamicModeChanged();
    Check(fc::g_dynamic_applied, "edit does not erase previous acceptance evidence");
    Boundary();
    Check(g_target == 144.0f && g_mode == sl::DLSSGMode::eDynamic, "live Dynamic target update");
    fc::g_dynamic_mfg_enabled = false;
    fc::NotifyDynamicModeChanged();
    Boundary();
    Check(g_mode == sl::DLSSGMode::eOn && g_counts.back() == 3, "Dynamic off uses original fixed policy");
    mfgunlock::g_enabled = false;
    fc::NotifyLiveOptionsChanged();
    Boundary();
    Check(g_counts.back() == 1, "disable restores native request, not previously forced copy");
    mfgunlock::g_enabled = true;
    native.mode = sl::DLSSGMode::eOff;
    fc::internal::HookedSetOptions({}, native);
    const auto before = g_counts.size();
    Request(6);
    Boundary();
    Check(g_counts.size() == before, "never reactivate game-disabled FG");
    native.mode = sl::DLSSGMode::eOn;
    fc::internal::HookedSetOptions({}, native);
    Boundary();
    Check(g_counts.size() == before + 1 && g_counts.back() == 5, "game re-enable applies pending once");
    native.numFramesToGenerate = 2;
    fc::internal::HookedSetOptions({}, native);
    Request(0);
    Boundary();
    Check(g_counts.back() == 2, "fresh native options supersede old cached options");
  }
}
void LiveGuards() {
  for (int kind = 0; kind < 4; ++kind) {
    Reset();
    sl::DLSSGOptions native{};
    if (kind == 0) native.next = reinterpret_cast<decltype(native.next)>(uintptr_t{1});
    if (kind == 1) native.onErrorCallback = reinterpret_cast<void*>(uintptr_t{1});
    if (kind == 2) native.structVersion = 99;
    if (kind == 3) g_result = sl::Result::eErrorFeatureNotSupported;
    fc::internal::HookedSetOptions({}, native);
    const auto before = g_counts.size();
    Request(4); Boundary();
    Check(g_counts.size() == before, "unsafe snapshot or failed native call is not replayed");
  }
  Reset();
  sl::DLSSGOptions native{};
  fc::internal::HookedSetOptions({}, native);
  Request(4);
  g_mock_thread_id = 8;
  Boundary();
  Check(g_counts.size() == 1, "wrong thread cannot replay options");
  g_mock_thread_id = 7;
  fc::g_present_thread = 8;
  Boundary();
  Check(g_counts.size() == 1, "native-options thread alone does not prove Present ordering");
  fc::g_present_thread = 7;
  Boundary();
  Check(g_counts.size() == 2, "observed native options thread can replay");
  fc::internal::InvalidateLiveOptions();
  Request(3); Boundary();
  Check(g_counts.size() == 2, "output destruction invalidates prior snapshot");
  fc::g_present_thread = 7;
  fc::internal::HookedSetOptions({}, native);
  Request(4);
  g_result = sl::Result::eErrorFeatureNotSupported;
  Boundary();
  const auto failed = g_counts.size();
  Boundary();
  Check(g_counts.size() == failed, "failed automatic call is not retried every frame");
  g_result = sl::Result::eOk;
  fc::internal::HookedSetOptions({}, native);
  Request(2);
  g_throw = true;
  Boundary();
  Check(!fc::internal::GetUiState({})->options_in_call, "automatic API exception clears reentry guard");
  Check(!fc::internal::GetUiState({})->native_options_valid, "automatic exception invalidates snapshot");
  g_throw = false;
  g_reenter = true;
  fc::internal::HookedSetOptions({}, native);
  Request(6);
  const auto reentrant = g_counts.size();
  Boundary();
  Check(g_counts.size() == reentrant, "reentrant native update disqualifies replay");

  Reset();
  fc::internal::HookedSetOptions(sl::ViewportHandle(0), native);
  fc::internal::HookedSetOptions(sl::ViewportHandle(1), native);
  Request(4);
  fc::ApplyLiveOptionsOnPresent();
  Check(g_counts.size() == 2, "Present never guesses among multiple viewports");
  Boundary(0); Boundary(1);
  Check(g_counts.size() == 4, "each viewport gets one independent runtime update");

  Reset();
  fc::internal::HookedSetOptions(sl::ViewportHandle(0), native);
  fc::internal::HookedSetOptions(sl::ViewportHandle(1), native);
  native.mode = sl::DLSSGMode::eOff;
  fc::internal::HookedSetOptions(sl::ViewportHandle(1), native);
  Request(4);
  const auto before_inactive_viewport = g_counts.size();
  fc::ApplyLiveOptionsOnPresent();
  Check(g_counts.size() == before_inactive_viewport + 1 && g_counts.back() == 3,
        "observed inactive transient viewport does not poison Present recovery");
  native.mode = sl::DLSSGMode::eOn;

  Reset();
  fc::internal::HookedSetOptions(sl::ViewportHandle(0), native);
  fc::internal::HookedSetOptions(sl::ViewportHandle(1), native);
  Request(4);
  const auto before_ambiguous_present = g_counts.size();
  fc::ApplyLiveOptionsOnPresent();
  Check(g_counts.size() == before_ambiguous_present,
        "Present still never guesses among two active replayable viewports");
  auto* state = fc::internal::GetUiState(sl::ViewportHandle(0));
  std::atomic_bool locked{false}, release{false};
  std::thread owner([&] {
    std::lock_guard lock(state->options_lock);
    locked = true;
    while (!release.load()) std::this_thread::yield();
  });
  while (!locked.load()) std::this_thread::yield();
  Request(2);
  const auto before_busy = g_counts.size();
  Boundary(0);
  Check(g_counts.size() == before_busy,
        "busy options call defers nonblocking automatic update");
  release = true;
  owner.join();
  Boundary(0);
  Check(g_counts.size() == before_busy + 1 && g_counts.back() == 1,
        "deferred update remains pending");
}
NVSDK_NGX_Handle* MakeFeature() {
  NVSDK_NGX_Handle* handle = nullptr;
  Check(ngx::internal::Create<0>(nullptr, NVSDK_NGX_Feature_FrameGeneration, nullptr, &handle) ==
        NVSDK_NGX_Result_Success, "Create forwarded");
  return handle;
}
void FeatureLifecycle() {
  Reset();
  auto* first = MakeFeature();
  auto* second = MakeFeature();
  Check(ngx::internal::LifecycleSnapshot().handles == 2, "multiple handles tracked");
  Check(ngx::internal::LifecycleSnapshot().epoch == 1, "epoch changes only at zero to nonzero");
  ngx::internal::Evaluate<0>(nullptr, first, nullptr, nullptr);
  Check(ngx::g_status.feature_active, "successful Evaluate marks a live feature active");
  ngx::internal::Release<0>(first);
  Check(ngx::internal::LifecycleSnapshot().handles == 1 && !ngx::internal::LifecycleSnapshot().zero_boundary,
        "one release cannot claim zero-handle boundary");
  Check(!ngx::g_status.feature_active, "releasing the only evaluated handle clears active status");
  g_ngx_result = NVSDK_NGX_Result_FAIL_InvalidParameter;
  ngx::internal::Release<0>(second);
  Check(ngx::internal::LifecycleSnapshot().handles == 1, "failed release retains handle");
  g_ngx_result = NVSDK_NGX_Result_Success;
  ngx::internal::Release<0>(second);
  Check(ngx::internal::LifecycleSnapshot().handles == 0 && ngx::internal::LifecycleSnapshot().zero_boundary,
        "last successful release records observed zero boundary");
  Check(!ngx::g_status.feature_created && !ngx::g_status.feature_active, "feature flags are no longer ever-seen flags");
  first = MakeFeature();
  Check(ngx::internal::LifecycleSnapshot().epoch == 2, "recreated feature gets another epoch");
  for (int kind = 0; kind < 3; ++kind) {
    g_throw = true;
    try {
      if (kind == 0) MakeFeature();
      if (kind == 1) ngx::internal::Evaluate<0>(nullptr, first, nullptr, nullptr);
      if (kind == 2) ngx::internal::Release<0>(first);
      Check(false, "expected injected exception");
    } catch (const std::runtime_error&) {}
    g_throw = false;
    const auto status = ngx::internal::LifecycleSnapshot();
    Check(status.creates == 0 && status.evaluates == 0 && status.releases == 0,
          "RAII clears counters on all exceptional exits");
    Check(status.uncertain, "exception cannot fabricate safe lifecycle");
  }
  Reset();
  for (unsigned i = 0; i < 17; ++i) MakeFeature();
  Check(ngx::internal::LifecycleSnapshot().handles == 16 && ngx::internal::LifecycleSnapshot().uncertain,
        "capacity exhaustion is explicit, never silent absence");
  for (unsigned i = 0; i < 16; ++i) ngx::internal::Release<0>(&g_handles[i]);
  Check(!ngx::internal::LifecycleSnapshot().zero_boundary, "overflow forbids proven zero boundary");
  Reset();
  MakeFeature();
  g_next_handle = 0;
  MakeFeature();
  Check(ngx::internal::LifecycleSnapshot().handles == 1 && ngx::internal::LifecycleSnapshot().uncertain,
        "duplicate handle is not counted twice");
  Reset();
  ngx::g_before_fg_create = [] { return false; };
  NVSDK_NGX_Handle* blocked = &g_handles[0];
  Check(ngx::internal::Create<0>(nullptr, NVSDK_NGX_Feature_FrameGeneration, nullptr, &blocked) !=
      NVSDK_NGX_Result_Success && !blocked && g_creates == 0, "busy preparation barrier fails before vendor Create");
}
void PacingAndCeiling() {
  Reset();
  sl::DLSSGOptions native{};
  fc::internal::HookedSetOptions({}, native);
  fc::g_pacing_ready = [] { return false; };
  fc::g_ensure_pacing = [] { throw std::runtime_error("automatic update must not install pacing patches"); };
  Request(4); Boundary();
  Check(fc::g_fixed_override_status == fc::FixedOverrideStatus::kBlockedPacing,
        "automatic setter never hot-patches legacy pacing");
  Check(g_counts.back() == 1, "pacing block preserves native request");
  Reset();
  fc::internal::HookedSetOptions({}, native);
  fc::g_streamline_max_generated = 3;
  Request(6); Boundary();
  Check(g_counts.back() == 1 && fc::g_fixed_override_status == fc::FixedOverrideStatus::kBlockedStructuralCeiling,
        "automatic requests obey structural ceiling without silent clamp");
}
void HdrTelemetry() {
  Reset();
  fc::NotifyHdrState(true, 3);
  Check(fc::g_hdr_state_seen.load() && fc::g_hdr_active.load() &&
            fc::g_output_color_space_seen.load() && fc::g_output_color_space.load() == 3,
        "HDR10/PQ observation preserves the raw swapchain color-space value");
  fc::NotifyHdrState(true, 2);
  Check(fc::g_hdr_active.load() && fc::g_output_color_space.load() == 2,
        "scRGB/FP16 observation stays distinct from HDR10/PQ");
  fc::NotifyHdrState(false, 0);
  Check(!fc::g_hdr_active.load() && fc::g_output_color_space.load() == 0,
        "SDR observation remains a known non-HDR color space");
  fc::NotifyOutputUnknown();
  Check(!fc::g_hdr_state_seen.load() && !fc::g_output_color_space_seen.load() &&
            fc::g_output_color_space.load() == dg::kUnknown32,
        "swapchain destruction/unknown output clears color-space evidence");
}

void LiveUiOptions() {
  Reset();
  sl::DLSSGOptions native{};
  fc::g_ui_composition_applied = false;
  fc::g_hdr_state_seen = true;
  fc::g_hdr_active = false;
  fc::internal::HookedSetOptions({}, native);
  fc::g_ui_composition_enabled = true;
  fc::NotifyUiCompositionChanged();
  Boundary();
  Check(fc::g_ui_composition_applied, "UI composition is resubmitted through the same live options path");
  fc::g_ui_composition_enabled = false;
  fc::NotifyUiCompositionChanged();
  Check(fc::g_ui_composition_applied, "UI edit alone does not erase last accepted state");
  Boundary();
  Check(!fc::g_ui_composition_applied, "accepted native options retire the override evidence");
}
void UiCandidateRecovery() {
  Reset();
  fc::g_ui_composition_enabled = true;
  fc::g_hdr_state_seen = true;
  fc::g_hdr_active = false;
  sl::DLSSGOptions native{};

  fc::internal::UpdateUiCandidateFormat(29);
  fc::internal::HookedSetOptions({}, native);
  Check(g_ui_format == 29 && g_ui_recomposition == sl::Boolean::eTrue &&
            fc::g_ui_candidate_options_synced.load(),
        "active candidate is synchronized in native UI Composition submission");

  native.mode = sl::DLSSGMode::eOff;
  fc::internal::HookedSetOptions({}, native);
  fc::internal::NotifyUiCandidateUnavailable();
  const auto revision_before_return = fc::g_options_revision.load();
  fc::internal::UpdateUiCandidateFormat(29);
  Check(fc::g_options_revision.load() == revision_before_return + 1,
        "same-format candidate return is a real lifecycle edge");
  const auto disabled_calls = g_counts.size();
  Boundary();
  Check(g_counts.size() == disabled_calls,
        "candidate return never reactivates game-disabled Frame Generation");

  Reset();
  fc::g_ui_composition_enabled = true;
  fc::g_hdr_state_seen = true;
  fc::g_hdr_active = false;
  native = {};
  fc::internal::UpdateUiCandidateFormat(29);
  fc::internal::HookedSetOptions({}, native);
  native.mode = sl::DLSSGMode::eOff;
  fc::internal::HookedSetOptions({}, native);
  fc::internal::NotifyUiCandidateUnavailable();
  native.mode = sl::DLSSGMode::eOn;
  fc::internal::HookedSetOptions({}, native);
  Check(g_ui_format == 0 && !fc::g_ui_candidate_options_synced.load(),
        "fresh active options do not reuse an unavailable candidate format");
  const auto before_recovery = g_counts.size();
  fc::internal::UpdateUiCandidateFormat(29);
  Boundary();
  Check(g_counts.size() == before_recovery + 1 && g_ui_format == 29 &&
            g_ui_recomposition == sl::Boolean::eTrue &&
            fc::g_ui_candidate_options_synced.load(),
        "candidate ready after game re-enable submits one synchronized recovery");
  Boundary();
  Check(g_counts.size() == before_recovery + 1,
        "recovered candidate does not resubmit unchanged options every frame");

  fc::internal::InvalidateLiveOptions();
  fc::internal::NotifyUiCandidateUnavailable();
  const auto stale_calls = g_counts.size();
  fc::internal::UpdateUiCandidateFormat(29);
  Boundary();
  Check(g_counts.size() == stale_calls,
        "output invalidation prevents replay of stale active options");
  fc::internal::NotifyUiCandidateUnavailable();
  fc::g_present_thread = 7;
  fc::internal::HookedSetOptions({}, native);
  const auto fresh_calls = g_counts.size();
  fc::internal::UpdateUiCandidateFormat(29);
  Boundary();
  Check(g_counts.size() == fresh_calls + 1 && fc::g_ui_candidate_options_synced.load(),
        "fresh active options recover once after output invalidation");
}
void RestoreOwnership() {
  struct Owner { bool fail; bool allocation = true; bool metadata = true; };
  std::vector<Owner> owners{{false}, {true}};
  const auto restore = [](Owner& owner) {
    if (owner.fail) return false;
    owner.allocation = false; owner.metadata = false;
    return true;
  };
  Check(!qc::RestoreRecords(owners, restore) && owners.size() == 1,
        "failed restore owner remains in inventory");
  Check(owners[0].allocation && owners[0].metadata, "failed owner retains allocation and rollback metadata");
  owners[0].fail = false;
  Check(qc::RestoreRecords(owners, restore) && owners.empty(), "retry removes only a fully restored owner");
}
void QualityState() {
  qc::Config a;
  a.scatter = true;
  a.boundary = 1;
  const auto original = qc::Encode(a);
  qc::g_requested = original;
  qc::g_prepared = qc::kUnknown;
  Check(qc::Freeze() == original, "first preparation consumes a coherent config");
  a.boundary = 2;
  qc::g_requested = qc::Encode(a);
  Check(qc::Pending(qc::g_requested, qc::g_prepared), "requested and prepared remain separate");
  Check(qc::Freeze() == original, "ordinary maintenance cannot adopt a pending kernel edit");
  a.warp = true;
  qc::g_requested = qc::Encode(a);
  Check(qc::Decode(qc::g_requested).warp, "latest pending edit wins");
  qc::g_requested = original;
  Check(!qc::Pending(qc::g_requested, qc::g_prepared), "return to prepared choice cancels pending");
  a.scatter = false;
  const auto disabled = qc::Encode(a);
  a.boundary = 0;
  Check(!qc::Pending(qc::Encode(a), disabled), "inactive child setting does not require reload");
  Check(qc::ActionText(true, true, true, false, false) != nullptr,
        "feature recreation alone never fabricates a kernel reconfiguration success");
  Check(qc::ActionText(false, true, true, false, false) == nullptr, "no sticky success/pending message");
  Check(qc::PresetAction(2, 2, 1) != nullptr, "supplied B is not provider-applied B");
  Check(qc::PresetAction(2, 2, 2) == nullptr, "observed B clears matching pending preset");
  Check(qc::PresetAction(0, 2, 2) != nullptr, "removing override does not erase cached model evidence");
}
}  // namespace
int main() {
  try {
    LiveOptions();
    LiveGuards();
    FeatureLifecycle();
    PacingAndCeiling();
    HdrTelemetry();
    LiveUiOptions();
    UiCandidateRecovery();
    RestoreOwnership();
    QualityState();
    std::printf("PASS runtime_settings checks=%u\n", g_checks);
    return 0;
  } catch (const std::exception& error) {
    std::fprintf(stderr, "FAIL runtime_settings: %s\n", error.what());
    return 1;
  }
}
