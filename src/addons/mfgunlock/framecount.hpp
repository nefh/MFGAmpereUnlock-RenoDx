/*
 * DLSS-G frame-count policy.
 * SPDX-License-Identifier: MIT
 *
 * Capability advertising and request forcing are separate. Hosts with a native
 * multiplier selector only need the verified maximum exposed through GetState;
 * hosts with an on/off control can optionally have numFramesToGenerate raised
 * at slDLSSGSetOptions. Modified requests use addon-owned storage; the
 * caller-owned options structure is never written.
 */

#pragma once

#include "./runtime_controls.hpp"

#include <vector>
#include <windows.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstring>
#include <limits>
#include <mutex>
#include <sstream>
#include <type_traits>
#include <utility>

#include <sl.h>
#include <sl_dlss_g.h>
#include <sl_pcl.h>

#include "./reshade_compat.hpp"

#include "./architecture.hpp"
#include "./provider_profile.hpp"
#include "./feature_capabilities.hpp"
#include "./ngx_hook.hpp"
#include "./count_observer.hpp"
#include "./diagnostic_bridge.hpp"
#include "./integration_contract.hpp"
#include "./presentation_observer.hpp"
#include "./output_state.hpp"
#include <optional>
#include "./quality_guard.hpp"
#include "./ui_candidate.hpp"
#include "./ui_alpha.hpp"
#include "./ui_submission.hpp"

namespace mfgunlock::framecount {

enum class FixedOverrideStatus : unsigned int {
  kNative = 0,
  kPending,
  kApplied,
  kAppliedAfterRetry,
  kAlreadyMatched,
  kRejectedNativeFallback,
  kBlockedBackend,
  kBlockedPacing,
  kBlockedStructuralCeiling,
  kUnknownStructuralCeiling,
  kUnknownOptionsAbi,
  kNativeRejected,
  kFallbackRejected,
};

// 0 = leave the game's request alone. Any value >= 2 requests that total multiplier.
inline std::atomic<unsigned int> g_force_multiplier{0};
inline std::atomic_bool g_feature_function_hooked{false};
inline std::atomic<uint64_t> g_options_revision{1};
inline std::atomic<uint64_t> g_options_cache_epoch{1};
// A new setting or provider lifetime starts a new empirical attempt. Ordinary
// SetOptions/GetState polling never rearms a request rejected by this runtime.
inline std::atomic<uint64_t> g_request_retry_revision{1};
inline std::atomic<uint32_t> g_present_thread{0};
inline void NotifyLiveOptionsChanged() {
  g_options_revision.fetch_add(1, std::memory_order_release);
}
inline std::atomic_bool g_init_hooked{false};
inline std::atomic<FixedOverrideStatus> g_fixed_override_status{FixedOverrideStatus::kNative};
inline std::atomic<unsigned int> g_last_requested{0};
inline std::atomic<unsigned int> g_last_effective_generated{0};
inline std::atomic_bool g_declined_no_pacing{false};
inline std::atomic<unsigned int> g_force_failed_for{0};
inline std::atomic<unsigned int> g_ceiling_blocked_for{0};
inline std::atomic_bool g_unknown_ceiling_logged{false};
inline std::atomic_bool g_unknown_fixed_abi_logged{false};
inline std::atomic_bool g_state_seen{false};
inline std::atomic<unsigned int> g_dlssg_status{0};
inline std::atomic_bool g_failure_status_logged{false};

// Last game request, actual forwarded request, and the last request accepted by
// Streamline. UINT32_MAX means no trustworthy observation is available.
inline std::atomic<uint32_t> g_last_requested_mode{diagnostic::kUnknown32};
inline std::atomic<uint32_t> g_last_requested_generated{diagnostic::kUnknown32};
inline std::atomic<uint32_t> g_last_forwarded_mode{diagnostic::kUnknown32};
inline std::atomic<uint32_t> g_last_forwarded_generated{diagnostic::kUnknown32};
inline std::atomic<uint32_t> g_last_accepted_mode{diagnostic::kUnknown32};
inline std::atomic<uint32_t> g_last_accepted_generated{diagnostic::kUnknown32};
inline std::atomic_bool g_effective_max_seen{false};
inline std::atomic<unsigned int> g_effective_max_generated{0};

// Turing diagnostic v2: bounded call tracing for the first active DLSS-G setup.
// This is observational only; results and caller-owned structures are unchanged.
inline std::atomic_uint32_t g_turing_diag_set_options_calls{0};
inline std::atomic_uint32_t g_turing_diag_get_state_calls{0};
inline constexpr uint32_t kTuringDiagCallTraceLimit = 16;

inline void TuringDiagLog(const std::string& message) {
  const DWORD last_error = GetLastError();
  reshade::log::message(reshade::log::level::info, message.c_str());
  SetLastError(last_error);
}

// Zero means no Streamline plugin is active or its structural ceiling is not
// known yet. Direct NGX intentionally leaves this at zero.
inline std::atomic_bool g_streamline_plugin_seen{false};
inline std::atomic<unsigned int> g_streamline_max_generated{0};
inline std::atomic_bool g_capacity_advertised{false};
inline std::atomic<unsigned int> g_runtime_max_generated{0};
// Original GetState response before our capability rewrite, not a scan estimate.
inline std::atomic<uint64_t> g_native_maximum{runtimecontrols::kUnknownMaximum};
inline std::atomic<uint64_t> g_vulkan_native_state_epoch{0};
inline std::atomic<uint32_t> g_vulkan_native_state_viewport{UINT32_MAX};

// NVIDIA Dynamic MFG is opt-in. The live typed setter decides whether the
// request works; API, GPU profile and capability flags are observations only.
// The game-owned options structure is never extended or modified in place.
inline std::atomic_bool g_dynamic_mfg_enabled{false};
inline std::atomic<float> g_dynamic_target_fps{0.0f};
inline std::atomic_bool g_dynamic_d3d12{false};
inline std::atomic_bool g_dynamic_support_seen{false};
inline std::atomic_bool g_dynamic_supported{false};
inline std::atomic_bool g_dynamic_probe_attempted{false};
inline std::atomic_bool g_dynamic_applied{false};
inline std::atomic_bool g_dynamic_fell_back{false};
inline std::atomic_bool g_dynamic_runtime_declined{false};
inline std::atomic<unsigned int> g_dynamic_result{0};
inline std::atomic<unsigned int> g_dynamic_probe_result{0};
inline std::atomic<uint64_t> g_dynamic_probe_calls{0};
inline std::atomic<uint64_t> g_dynamic_request_calls{0};
inline std::atomic<uint64_t> g_dynamic_suppressed_repeats{0};
inline std::atomic<uint64_t> g_fixed_suppressed_repeats{0};
inline bool (*g_dynamic_stack_ready)() = nullptr;

// UI recomposition is independent of the selected Frame Generation preset.
// Output encoding is owned by output_state.hpp; optional HUD/UI resources still
// pass through a fail-closed metadata guard before Streamline can consume them.
inline std::atomic_bool g_ui_composition_enabled{true};
inline std::atomic_bool g_ui_composition_applied{false};
inline std::atomic_bool g_ui_composition_fell_back{false};
inline std::atomic<unsigned int> g_ui_composition_result{0};
inline std::atomic<unsigned int> g_ui_composition_source_version{0};
inline std::atomic_bool g_ui_pair_eligible{false};
inline std::atomic_bool g_hud_inputs_suppressed{false};
inline std::atomic_bool g_ui_native_hudless_fallback{false};
inline std::atomic<uint32_t> g_ui_issue_mask{0};
inline std::atomic<unsigned long long> g_ui_resets_requested{0};
inline std::atomic<unsigned long long> g_ui_resets_injected{0};
// Common SL constants are renderer-owned and shared by SR/RR/FG. UI state
// changes may invalidate our bookkeeping, but must not reset those histories.
inline std::atomic<unsigned long long> g_ui_constants_calls{0};
inline std::atomic<unsigned long long> g_ui_native_resets_seen{0};
inline std::atomic<unsigned long long> g_ui_reset_requests_retired{0};
inline std::atomic<unsigned long long> g_ui_hudless_tags_seen{0};
inline std::atomic<unsigned long long> g_ui_color_alpha_tags_seen{0};
inline std::atomic<unsigned long long> g_ui_alpha_tags_seen{0};
inline std::atomic<unsigned long long> g_ui_optional_tags_suppressed{0};
inline std::atomic_bool g_ui_tag_batch_too_large{false};
inline std::atomic_bool g_ui_viewport_capacity_exhausted{false};

// UI resource discovery is observational. Automatic injection is enabled by
// default, but still fails closed unless the candidate passes every metadata gate.
inline std::atomic_bool g_ui_candidate_injection_enabled{true};
inline std::atomic_bool g_ui_candidate_active{false};
inline std::atomic_bool g_ui_candidate_ready{false};
inline std::atomic<uint32_t> g_ui_candidate_format{0};
inline std::atomic<uint32_t> g_ui_candidate_path{
    static_cast<uint32_t>(uisubmission::Path::kUnavailable)};
inline std::atomic_bool g_ui_candidate_options_synced{false};
inline std::atomic_bool g_ui_candidate_runtime_declined{false};
inline std::atomic<uint32_t> g_ui_candidate_retry_frames{0};
inline std::atomic<uint32_t> g_ui_candidate_consecutive_failures{0};
inline std::atomic<uint64_t> g_ui_candidate_last_failed_resource{0};
inline std::atomic<unsigned long long> g_ui_candidate_tags_injected{0};
inline std::atomic<unsigned long long> g_ui_candidate_injection_failures{0};
inline std::atomic<unsigned long long> g_ui_candidate_recoveries{0};
inline std::atomic<unsigned long long> g_ui_candidate_native_fallbacks{0};
inline std::atomic<unsigned long long> g_ui_candidate_missing_command_buffer{0};
inline std::atomic<unsigned long long> g_ui_candidate_full_batches{0};
inline std::atomic<unsigned int> g_ui_candidate_last_result{0};

struct UiCandidateSnapshot {
  uint64_t native_resource = 0;
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t format = 0;
  uint32_t ui_encoding = static_cast<uint32_t>(outputstate::UiEncoding::kUnknown);
  uint32_t state = 0;
  uint32_t stable_frames = 0;
  uint32_t rtv_binds_after_clear = 0;
  uint32_t reject_reasons = uicandidate::kStale;
  uint32_t late_writes = 0;
  uint32_t alpha_convention = 0;
  uint32_t alpha_draws = 0;
  uint32_t alpha_unproven_draws = 0;
  uint64_t clear_serial = 0;
  uint64_t output_revision = 0;
  uint64_t submission_queue = 0;
  uint32_t submission_thread = 0;
  bool state_known = false;
  bool source_submitted = false;
  bool recent = false;
};

struct UiAlphaCandidateSnapshot {
  // Native D3D12 command list on which extraction is already recorded.
  // Only the optional volatile alpha tag may be submitted on this list.
  sl::CommandBuffer* copy_commands = nullptr;
  uint64_t ticket = 0;
  uint32_t slot = UINT32_MAX;
  uint64_t native_resource = 0;
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t format = 0;
  uint32_t state = 0;
};

inline void (*g_observe_streamline_ui_tags)(const sl::ResourceTag*, uint32_t) = nullptr;
inline bool (*g_observe_ui_candidate)(UiCandidateSnapshot*) = nullptr;
inline bool (*g_acquire_ui_candidate)(const UiCandidateSnapshot*) = nullptr;
inline void (*g_release_ui_candidate)(const UiCandidateSnapshot*, bool, sl::Result) = nullptr;
inline bool (*g_prepare_ui_alpha_candidate)(const UiCandidateSnapshot*, sl::CommandBuffer*,
                                             UiAlphaCandidateSnapshot*) = nullptr;
inline bool (*g_finish_ui_alpha_candidate)(const UiCandidateSnapshot*,
    const UiAlphaCandidateSnapshot*, bool, sl::Result) = nullptr;

// The game owns Streamline runtime selection by default. Explicit overrides
// only change NVIDIA's documented OTA flags for the slInit call.
enum class RuntimeSelectionMode : unsigned int {
  kGameDefault = 0,
  kPreferLocal = 1,
  kForceOta = 2,
};
inline std::atomic<unsigned int> g_runtime_selection_mode{
    static_cast<unsigned int>(RuntimeSelectionMode::kGameDefault)};

// Prepare the software-pacing workaround when available. Its readiness is not
// proof that a native scalar request will fail, and never vetoes that request.
inline void (*g_ensure_pacing)() = nullptr;
inline bool (*g_pacing_ready)() = nullptr;
// Diagnostic preparation status only. Neither menu advertisement nor the
// native scalar request requires positive readiness here.
inline bool (*g_multi_frame_ready)() = nullptr;
inline void (*g_on_dlssg_entry_observed)(void*) = nullptr;
// Historical callback name: the value is the configured advertisement request,
// not a discovered provider limit. Zero preserves the native maximum.
inline unsigned int (*g_provider_mfg_ceiling)() = nullptr;
// Legacy default when no configuration callback is bound, never an upper cap.
inline constexpr unsigned int kPolicyGeneratedCeiling = 5;
inline unsigned int ProviderMfgCeiling() {
  return g_provider_mfg_ceiling ? g_provider_mfg_ceiling() : kPolicyGeneratedCeiling;
}
inline std::atomic_bool g_declined_backend{false};
inline sl::Result (*g_on_init)(const sl::Preferences&, uint64_t,
    sl::Result (*)(const sl::Preferences&, uint64_t)) = nullptr;
inline bool (*g_init_compatible)(HMODULE) = nullptr;
inline bool (*g_interposer_contract_ready)(HMODULE) = nullptr;
inline HMODULE (*g_find_interposer)() = nullptr;
inline hook::ModuleIdentity g_interposer_hook_identity{};
namespace internal {

using SetOptionsFn = sl::Result (*)(const sl::ViewportHandle&, const sl::DLSSGOptions&);
using GetStateFn =
    sl::Result (*)(const sl::ViewportHandle&, sl::DLSSGState&, const sl::DLSSGOptions*);
using GetFeatureFunctionFn = sl::Result (*)(sl::Feature, const char*, void*&);
using InitFn = sl::Result (*)(const sl::Preferences&, uint64_t);
using SetTagFn = sl::Result (*)(const sl::ViewportHandle&, const sl::ResourceTag*, uint32_t,
                               sl::CommandBuffer*);
using SetTagForFrameFn = PFun_slSetTagForFrame*;
using SetConstantsFn = PFun_slSetConstants*;
using PclSetMarkerFn = PFun_slPCLSetMarker*;

inline SetOptionsFn g_real_set_options = nullptr;
inline GetStateFn g_real_get_state = nullptr;
inline hook::AddressHook g_set_options_entry;
inline hook::AddressHook g_get_state_entry;

inline SetOptionsFn RealSetOptions() {
  const auto entry = g_set_options_entry.Original<SetOptionsFn>();
  if (entry) {
    if (!hook::IsCurrent(g_set_options_entry.identity)) return nullptr;
    return entry;
  }
  return g_real_set_options;
}

inline GetStateFn RealGetState() {
  const auto entry = g_get_state_entry.Original<GetStateFn>();
  if (entry) {
    if (!hook::IsCurrent(g_get_state_entry.identity)) return nullptr;
    return entry;
  }
  return g_real_get_state;
}
inline std::atomic_bool g_entry_shutting_down{false};
inline thread_local uint32_t g_options_call_depth = 0;
inline GetFeatureFunctionFn g_real_get_feature_function = nullptr;
inline InitFn g_real_init = nullptr;
inline SetTagFn g_real_set_tag = nullptr;
inline SetTagForFrameFn g_real_set_tag_for_frame = nullptr;
inline SetConstantsFn g_real_set_constants = nullptr;
inline PclSetMarkerFn g_real_pcl_set_marker = nullptr;
inline std::vector<hook::HookItem> g_ui_hooks;
inline std::atomic_bool g_ui_hooks_installed{false};

inline bool KnownOptionsAbi(const sl::DLSSGOptions& options) {
  return options.structType == sl::DLSSGOptions::s_structType &&
         options.structVersion >= sl::kStructVersion1 &&
         options.structVersion <= sl::kStructVersion5;
}

inline bool SetFixedOverrideState(FixedOverrideStatus status, unsigned int requested,
                                  unsigned int effective) {
  const auto previous_status = g_fixed_override_status.exchange(status, std::memory_order_relaxed);
  const unsigned int previous_requested =
      g_last_requested.exchange(requested, std::memory_order_relaxed);
  const unsigned int previous_effective =
      g_last_effective_generated.exchange(effective, std::memory_order_relaxed);
  return previous_status != status || previous_requested != requested ||
         previous_effective != effective;
}

inline bool DynamicStackReady() {
  return !g_entry_shutting_down.load(std::memory_order_acquire) &&
         RealSetOptions() != nullptr;
}

inline bool ObserveDynamicSupport(const sl::DLSSGState& state, sl::Result result) {
  if (result != sl::Result::eOk ||
      state.structType != sl::DLSSGState::s_structType ||
      state.structVersion != sl::kStructVersion4) {
    return false;
  }
  if (state.bIsDynamicMFGSupported != sl::Boolean::eTrue &&
      state.bIsDynamicMFGSupported != sl::Boolean::eFalse) {
    return false;
  }
  const bool supported = state.bIsDynamicMFGSupported == sl::Boolean::eTrue;
  const bool previous = g_dynamic_supported.exchange(supported, std::memory_order_acq_rel);
  const bool seen = g_dynamic_support_seen.exchange(true, std::memory_order_release);
  if (!seen || previous != supported) {
    reshade::log::message(
        reshade::log::level::info,
        supported
            ? "mfgunlock: slDLSSGGetState reports NVIDIA Dynamic MFG capability=true; actual SetOptions result decides activation."
            : "mfgunlock: slDLSSGGetState reports NVIDIA Dynamic MFG capability=false; an explicitly requested Dynamic mode will still be tried.");
  }
  return true;
}

constexpr uint32_t kUnusedViewport = (std::numeric_limits<uint32_t>::max)();

// These snapshots are compared, never replayed. Pointer fields identify a
// synchronous caller context and do not extend any caller-owned lifetime.
struct RuntimeAttempt {
  sl::DLSSGOptions options{};
  const void* entry = nullptr;
  uint64_t provider_epoch = 0;
  uint64_t setting_revision = 0;
  uint32_t api = 0;
  uint32_t forced_multiplier = 0;
  float dynamic_target = 0.0f;
  bool ui_composition = false;
  bool valid = false;
  bool rejected = false;
  uint32_t transient_retries = 0;
  uint32_t transient_wait = 0;
  sl::Result result = sl::Result::eOk;
};

struct DynamicProbeAttempt {
  std::atomic<const void*> entry{nullptr};
  std::atomic<uint64_t> provider_epoch{0};
  std::atomic<uint64_t> setting_revision{0};
  std::atomic_bool attempted{false};
  std::atomic<sl::Result> result{sl::Result::eOk};
};

struct UiViewportState {
  // Guarded by lock; unknown Vulkan UI metadata survives split tag batches.
  uint32_t vulkan_metadata_unknown = 0;
  // Only SetOptions calls are serialized here, never provider maintenance or
  // Evaluate. Automatic submission uses try_lock on the observed native thread.
  std::recursive_mutex options_lock;
  sl::DLSSGOptions native_options{};
  bool native_options_valid = false;
  bool options_in_call = false;
  uint32_t options_thread = 0;
  uint64_t attempted_revision = 0;
  uint64_t cache_epoch = 0;
  const void* options_entry = nullptr;
  RuntimeAttempt fixed_attempt;
  RuntimeAttempt dynamic_attempt;
  RuntimeAttempt ui_attempt;
  RuntimeAttempt pacing_attempt;
  DynamicProbeAttempt dynamic_probe;
  std::atomic<uint64_t> probe_presented_pending{0};
  uint64_t probe_presented_epoch = 0;
  std::atomic<uint64_t> runtime_health_epoch{0};
  std::atomic_bool runtime_state_ready{false};
  SRWLOCK lock = SRWLOCK_INIT;
  std::atomic<uint32_t> key{kUnusedViewport};
  std::atomic_bool options_seen{false};
  std::atomic<uint32_t> mode{0};
  std::atomic<uint32_t> generated_frames{0};
  std::atomic<uint32_t> flags{0};
  std::atomic<uint32_t> color_width{0};
  std::atomic<uint32_t> color_height{0};
  std::atomic<uint32_t> color_format{0};
  std::atomic<uint32_t> mvec_width{0};
  std::atomic<uint32_t> mvec_height{0};
  std::atomic<uint32_t> backbuffer_width{0};
  std::atomic<uint32_t> backbuffer_height{0};
  std::atomic<uint32_t> backbuffer_format{0};
  std::atomic_bool hud_separation_seen{false};
  std::atomic_bool hud_separation_suppressed{false};
  std::atomic_bool hudless_color_seen{false};
  std::atomic_bool ui_color_or_alpha_seen{false};
  std::atomic_bool ui_color_alpha_seen{false};
  std::atomic_bool ui_alpha_seen{false};
  // Native intent is independent from helper-produced tags, including an
  // explicit null UI tag. Frame-based intent expires with its source token.
  std::atomic_bool native_ui_owner{false};
  uint64_t native_tag_frame = UINT64_MAX;
  bool native_tag_frame_based = false;
  std::atomic<uint32_t> automatic_ui_tag_type{0};
  std::atomic<uint32_t> automatic_ui_frame_type{0};
  std::atomic_bool automatic_ui_tag_global{false};
  std::atomic_bool automatic_ui_clear_pending{false};
  std::atomic_bool ui_recomposition_invalid{false};
  std::atomic_bool ui_recomposition_eligible{false};
  std::atomic<uint64_t> reset_requested{0};
  std::atomic<uint64_t> reset_applied{0};
};

inline std::array<UiViewportState, 8> g_ui_viewports{};

inline UiViewportState* GetUiState(const sl::ViewportHandle& viewport) {
  const uint32_t key = static_cast<uint32_t>(viewport);
  for (auto& state : g_ui_viewports) {
    if (state.key.load(std::memory_order_acquire) == key) return &state;
  }
  for (auto& state : g_ui_viewports) {
    uint32_t unused = kUnusedViewport;
    if (state.key.compare_exchange_strong(unused, key, std::memory_order_acq_rel))
      return &state;
    if (unused == key) return &state;
  }
  if (!g_ui_viewport_capacity_exhausted.exchange(true, std::memory_order_relaxed)) {
    reshade::log::message(
        reshade::log::level::warning,
        "mfgunlock: UI Composition observed more than eight Streamline viewports; "
        "additional viewports keep the native final-color path.");
  }
  return nullptr;
}

inline void RequestUiReset(UiViewportState* state) {
  if (state == nullptr) return;
  state->reset_requested.fetch_add(1, std::memory_order_release);
  g_ui_resets_requested.fetch_add(1, std::memory_order_relaxed);
}

inline void RequestAllUiResets() {
  for (auto& state : g_ui_viewports) {
    if (state.key.load(std::memory_order_acquire) != kUnusedViewport)
      RequestUiReset(&state);
  }
}

inline void ForgetUiOutputs() {
  for (auto& state : g_ui_viewports) {
    AcquireSRWLockExclusive(&state.lock);
    state.backbuffer_width.store(0, std::memory_order_relaxed);
    state.backbuffer_height.store(0, std::memory_order_relaxed);
    state.backbuffer_format.store(0, std::memory_order_relaxed);
    state.hud_separation_seen.store(false, std::memory_order_relaxed);
    state.hud_separation_suppressed.store(false, std::memory_order_relaxed);
    state.hudless_color_seen.store(false, std::memory_order_relaxed);
    state.ui_color_or_alpha_seen.store(false, std::memory_order_relaxed);
    state.ui_color_alpha_seen.store(false, std::memory_order_relaxed);
    state.ui_alpha_seen.store(false, std::memory_order_relaxed);
    state.vulkan_metadata_unknown = 0;
    state.native_ui_owner.store(false, std::memory_order_relaxed);
    state.native_tag_frame = UINT64_MAX;
    state.native_tag_frame_based = false;
    state.automatic_ui_frame_type.store(0, std::memory_order_relaxed);
    state.ui_recomposition_invalid.store(false, std::memory_order_relaxed);
    state.ui_recomposition_eligible.store(false, std::memory_order_relaxed);
    ReleaseSRWLockExclusive(&state.lock);
  }
  g_ui_pair_eligible.store(false, std::memory_order_relaxed);
  g_ui_native_hudless_fallback.store(false, std::memory_order_relaxed);
}

inline void ClearAutomaticUiTagState(UiViewportState* state) {
  if (state == nullptr) return;
  state->automatic_ui_tag_type.store(0, std::memory_order_release);
  state->automatic_ui_frame_type.store(0, std::memory_order_release);
  state->automatic_ui_tag_global.store(false, std::memory_order_release);
  state->automatic_ui_clear_pending.store(false, std::memory_order_release);
}

inline void MarkAutomaticUiTag(UiViewportState* state, sl::BufferType type,
                               bool frame_based) {
  if (state == nullptr) return;
  if (frame_based) {
    ClearAutomaticUiTagState(state);
    // A later native ColorAlpha tag in the SAME source frame must not be
    // shadowed by our earlier preferred UIAlpha tag. No global ownership.
    state->automatic_ui_frame_type.store(static_cast<uint32_t>(type), std::memory_order_release);
    return;
  }
  state->automatic_ui_tag_type.store(static_cast<uint32_t>(type),
                                     std::memory_order_release);
  state->automatic_ui_tag_global.store(true, std::memory_order_release);
  state->automatic_ui_clear_pending.store(false, std::memory_order_release);
}

inline void RequestGlobalAutomaticUiTagRelease() {
  for (auto& state : g_ui_viewports) {
    if (state.key.load(std::memory_order_acquire) == kUnusedViewport ||
        !state.automatic_ui_tag_global.load(std::memory_order_acquire) ||
        state.automatic_ui_tag_type.load(std::memory_order_acquire) == 0) {
      continue;
    }
    state.automatic_ui_clear_pending.store(true, std::memory_order_release);
  }
}

inline bool ReleaseGlobalAutomaticUiTag(UiViewportState* state) {
  if (state == nullptr ||
      !state->automatic_ui_tag_global.load(std::memory_order_acquire)) {
    return true;
  }
  const uint32_t key = state->key.load(std::memory_order_acquire);
  const auto type = static_cast<sl::BufferType>(
      state->automatic_ui_tag_type.load(std::memory_order_acquire));
  if (key == kUnusedViewport || type == 0 || g_real_set_tag == nullptr) return false;

  const sl::ResourceTag clear_tag(
      nullptr, type, sl::ResourceLifecycle::eValidUntilPresent);
  const sl::Result result = presentation::passive::CallNative([&] {
    return g_real_set_tag(sl::ViewportHandle(key), &clear_tag, 1, nullptr);
  });
  if (result != sl::Result::eOk) return false;
  ClearAutomaticUiTagState(state);
  return true;
}

inline bool BatchContainsTagType(const sl::ResourceTag* tags, uint32_t count,
                                 sl::BufferType type) {
  if (tags == nullptr) return false;
  for (uint32_t index = 0; index < count; ++index) {
    if (tags[index].type == type) return true;
  }
  return false;
}

template <typename Forward>
inline sl::Result ForwardWithAutomaticUiRelease(
    UiViewportState* state, const sl::ResourceTag* tags, uint32_t count,
    bool frame_based, Forward&& forward) {
  if (state == nullptr) return forward(tags, count);
  if (frame_based) {
    if (state->automatic_ui_tag_global.load(std::memory_order_acquire))
      ReleaseGlobalAutomaticUiTag(state);
    const auto owned = static_cast<sl::BufferType>(state->automatic_ui_frame_type.load());
    bool native_ui = false;
    if (tags && count <= 64) {
      for (uint32_t index = 0; index < count; ++index) {
        if (tags[index].structType != sl::ResourceTag::s_structType ||
            tags[index].structVersion != sl::kStructVersion1) return forward(tags, count);
        native_ui |= qualityguard::IsUiColorOrAlphaType(tags[index].type);
      }
    }
    if (!owned || !native_ui) return forward(tags, count);
    if (BatchContainsTagType(tags, count, owned)) {
      const auto result = forward(tags, count);
      if (result == sl::Result::eOk) state->automatic_ui_frame_type.store(0);
      return result;
    }
    alignas(sl::ResourceTag) std::array<std::byte, sizeof(sl::ResourceTag) * 65> storage{};
    std::memcpy(storage.data(), tags, sizeof(sl::ResourceTag) * count);
    const sl::ResourceTag clear(nullptr, owned, sl::ResourceLifecycle::eOnlyValidNow);
    std::memcpy(storage.data() + sizeof(sl::ResourceTag) * count, &clear, sizeof(clear));
    const auto result = forward(reinterpret_cast<const sl::ResourceTag*>(storage.data()), count + 1);
    if (result == sl::Result::eOk) state->automatic_ui_frame_type.store(0);
    return result;
  }
  if (!state->automatic_ui_tag_global.load(std::memory_order_acquire) ||
      !state->automatic_ui_clear_pending.load(std::memory_order_acquire)) {
    return forward(tags, count);
  }

  const auto type = static_cast<sl::BufferType>(
      state->automatic_ui_tag_type.load(std::memory_order_acquire));
  if (type == 0) {
    ClearAutomaticUiTagState(state);
    return forward(tags, count);
  }
  if (count >= 65) {
    ReleaseGlobalAutomaticUiTag(state);
    return forward(tags, count);
  }
  if (BatchContainsTagType(tags, count, type)) {
    const sl::Result result = forward(tags, count);
    if (result == sl::Result::eOk) ClearAutomaticUiTagState(state);
    return result;
  }

  static_assert(std::is_trivially_copyable_v<sl::ResourceTag>);
  alignas(sl::ResourceTag)
      std::array<std::byte, sizeof(sl::ResourceTag) * 65> storage{};
  if (tags != nullptr && count != 0)
    std::memcpy(storage.data(), tags, sizeof(sl::ResourceTag) * count);
  const sl::ResourceTag clear_tag(
      nullptr, type, sl::ResourceLifecycle::eValidUntilPresent);
  std::memcpy(storage.data() + sizeof(sl::ResourceTag) * count,
              &clear_tag, sizeof(clear_tag));
  const auto* forwarded = reinterpret_cast<const sl::ResourceTag*>(storage.data());
  const sl::Result result = forward(forwarded, count + 1);
  if (result == sl::Result::eOk) ClearAutomaticUiTagState(state);
  return result;
}

inline void ReleaseGlobalAutomaticUiTags() {
  for (auto& state : g_ui_viewports) ReleaseGlobalAutomaticUiTag(&state);
}

inline void ObserveUiOptionsTransition(const sl::ViewportHandle& viewport,
                                       const sl::DLSSGOptions& options) {
  UiViewportState* state = GetUiState(viewport);
  if (state == nullptr) return;

  AcquireSRWLockExclusive(&state->lock);
  const uint32_t mode = static_cast<uint32_t>(options.mode);
  const uint32_t flags = static_cast<uint32_t>(options.flags);
  const bool changed = state->options_seen.load(std::memory_order_acquire) &&
      (state->mode.load(std::memory_order_relaxed) != mode ||
       state->generated_frames.load(std::memory_order_relaxed) != options.numFramesToGenerate ||
       state->flags.load(std::memory_order_relaxed) != flags ||
       state->color_width.load(std::memory_order_relaxed) != options.colorWidth ||
       state->color_height.load(std::memory_order_relaxed) != options.colorHeight ||
       state->color_format.load(std::memory_order_relaxed) != options.colorBufferFormat ||
       state->mvec_width.load(std::memory_order_relaxed) != options.mvecDepthWidth ||
       state->mvec_height.load(std::memory_order_relaxed) != options.mvecDepthHeight);
  state->mode.store(mode, std::memory_order_relaxed);
  state->generated_frames.store(options.numFramesToGenerate, std::memory_order_relaxed);
  state->flags.store(flags, std::memory_order_relaxed);
  state->color_width.store(options.colorWidth, std::memory_order_relaxed);
  state->color_height.store(options.colorHeight, std::memory_order_relaxed);
  state->color_format.store(options.colorBufferFormat, std::memory_order_relaxed);
  state->mvec_width.store(options.mvecDepthWidth, std::memory_order_relaxed);
  state->mvec_height.store(options.mvecDepthHeight, std::memory_order_relaxed);
  state->options_seen.store(true, std::memory_order_release);
  ReleaseSRWLockExclusive(&state->lock);

  if (changed && g_ui_composition_enabled.load(std::memory_order_relaxed))
    RequestUiReset(state);
}

inline qualityguard::OutputDescription ExpectedUiOutput(const UiViewportState* state) {
  if (state == nullptr) return {};
  qualityguard::OutputDescription result{
      state->backbuffer_width.load(std::memory_order_relaxed),
      state->backbuffer_height.load(std::memory_order_relaxed),
      state->backbuffer_format.load(std::memory_order_relaxed)};
  if (!result.HasDimensions()) {
    result.width = state->color_width.load(std::memory_order_relaxed);
    result.height = state->color_height.load(std::memory_order_relaxed);
  }
  if (!result.HasFormat())
    result.format = state->color_format.load(std::memory_order_relaxed);
  return result;
}

inline void UpdateUiCandidate(uisubmission::Path path, uint32_t format = 0) {
  if (path == uisubmission::Path::kUnavailable) return;
  if (path == uisubmission::Path::kColorAndAlpha && format == 0) return;
  const bool was_ready = g_ui_candidate_ready.exchange(true, std::memory_order_acq_rel);
  const uint32_t options_format = path == uisubmission::Path::kColorAndAlpha ? format : 0;
  const uint32_t previous_format = g_ui_candidate_format.exchange(
      options_format, std::memory_order_acq_rel);
  const uint32_t previous_path = g_ui_candidate_path.exchange(
      static_cast<uint32_t>(path), std::memory_order_acq_rel);
  if (!was_ready || previous_format != options_format ||
      previous_path != static_cast<uint32_t>(path)) {
    g_ui_candidate_options_synced.store(false, std::memory_order_release);
    NotifyLiveOptionsChanged();
  }
}

inline void UpdateUiCandidateFormat(uint32_t format) {
  UpdateUiCandidate(uisubmission::Path::kColorAndAlpha, format);
}

inline void NotifyUiCandidateUnavailable() {
  RequestGlobalAutomaticUiTagRelease();
  g_ui_candidate_ready.store(false, std::memory_order_release);
  g_ui_candidate_format.store(0, std::memory_order_release);
  g_ui_candidate_path.store(static_cast<uint32_t>(uisubmission::Path::kUnavailable),
                            std::memory_order_release);
  g_ui_candidate_active.store(false, std::memory_order_relaxed);
  g_ui_candidate_options_synced.store(false, std::memory_order_release);
}

inline bool CandidateInjectionRequested() {
  return g_ui_candidate_injection_enabled.load(std::memory_order_relaxed) &&
         !g_ui_candidate_runtime_declined.load(std::memory_order_acquire);
}

inline bool CandidateColorAlphaProven(const UiCandidateSnapshot& candidate) {
  return static_cast<uialpha::AlphaConvention>(candidate.alpha_convention) ==
      uialpha::AlphaConvention::kPremultiplied;
}

inline uisubmission::Path CandidateSubmissionPath(
    const UiCandidateSnapshot& candidate) {
  return uisubmission::Select(
      outputstate::Read(),
      {static_cast<outputstate::UiEncoding>(candidate.ui_encoding),
       static_cast<uialpha::AlphaConvention>(candidate.alpha_convention),
       candidate.format},
      g_prepare_ui_alpha_candidate != nullptr && g_finish_ui_alpha_candidate != nullptr);
}

inline bool ConsumeCandidateRetryFrame() {
  uint32_t remaining = g_ui_candidate_retry_frames.load(std::memory_order_acquire);
  while (remaining != 0) {
    if (g_ui_candidate_retry_frames.compare_exchange_weak(
            remaining, remaining - 1, std::memory_order_acq_rel,
            std::memory_order_acquire)) {
      return true;
    }
  }
  return false;
}

inline bool CopyOptions(const sl::DLSSGOptions& source, sl::DLSSGOptions& destination) {
  if (!KnownOptionsAbi(source)) return false;
  const size_t version = source.structVersion;
  destination = sl::DLSSGOptions{};
  destination.next = source.next;
  destination.structVersion = version;
  destination.mode = source.mode;
  destination.numFramesToGenerate = source.numFramesToGenerate;
  destination.flags = source.flags;
  destination.dynamicResWidth = source.dynamicResWidth;
  destination.dynamicResHeight = source.dynamicResHeight;
  destination.numBackBuffers = source.numBackBuffers;
  destination.mvecDepthWidth = source.mvecDepthWidth;
  destination.mvecDepthHeight = source.mvecDepthHeight;
  destination.colorWidth = source.colorWidth;
  destination.colorHeight = source.colorHeight;
  destination.colorBufferFormat = source.colorBufferFormat;
  destination.mvecBufferFormat = source.mvecBufferFormat;
  destination.depthBufferFormat = source.depthBufferFormat;
  destination.hudLessBufferFormat = source.hudLessBufferFormat;
  destination.uiBufferFormat = source.uiBufferFormat;
  destination.onErrorCallback = source.onErrorCallback;
  if (version >= sl::kStructVersion2) destination.bReserved15 = source.bReserved15;
  if (version >= sl::kStructVersion3)
    destination.queueParallelismMode = source.queueParallelismMode;
  if (version >= sl::kStructVersion4)
    destination.enableUserInterfaceRecomposition = source.enableUserInterfaceRecomposition;
  if (version >= sl::kStructVersion5)
    destination.dynamicTargetFrameRate = source.dynamicTargetFrameRate;
  return true;
}

inline bool SameRuntimeOptions(const sl::DLSSGOptions& a, const sl::DLSSGOptions& b) {
  // Both objects have already been copied through the bounded public ABI.
  // Compare fields, not padding bytes or caller-owned pointed-to storage.
  return a.structVersion == b.structVersion && a.next == b.next &&
         a.mode == b.mode && a.numFramesToGenerate == b.numFramesToGenerate &&
         a.flags == b.flags && a.dynamicResWidth == b.dynamicResWidth &&
         a.dynamicResHeight == b.dynamicResHeight && a.numBackBuffers == b.numBackBuffers &&
         a.mvecDepthWidth == b.mvecDepthWidth && a.mvecDepthHeight == b.mvecDepthHeight &&
         a.colorWidth == b.colorWidth && a.colorHeight == b.colorHeight &&
         a.colorBufferFormat == b.colorBufferFormat && a.mvecBufferFormat == b.mvecBufferFormat &&
         a.depthBufferFormat == b.depthBufferFormat && a.hudLessBufferFormat == b.hudLessBufferFormat &&
         a.uiBufferFormat == b.uiBufferFormat && a.onErrorCallback == b.onErrorCallback &&
         a.bReserved15 == b.bReserved15 && a.queueParallelismMode == b.queueParallelismMode &&
         a.enableUserInterfaceRecomposition == b.enableUserInterfaceRecomposition &&
         std::memcmp(&a.dynamicTargetFrameRate, &b.dynamicTargetFrameRate,
                     sizeof(a.dynamicTargetFrameRate)) == 0;
}

inline bool PrepareRuntimeAttempt(RuntimeAttempt& attempt, const sl::DLSSGOptions& options) {
  sl::DLSSGOptions copied{};
  if (!CopyOptions(options, copied)) return false;
  const auto* entry = reinterpret_cast<const void*>(RealSetOptions());
  const auto epoch = g_options_cache_epoch.load(std::memory_order_acquire);
  const auto revision = g_request_retry_revision.load(std::memory_order_acquire);
  const auto api = static_cast<uint32_t>(outputstate::GetResourceApi());
  const auto multiplier = g_force_multiplier.load(std::memory_order_relaxed);
  const auto target = g_dynamic_target_fps.load(std::memory_order_relaxed);
  const auto ui = g_ui_composition_enabled.load(std::memory_order_relaxed);
  const bool same = attempt.valid && attempt.entry == entry &&
      attempt.provider_epoch == epoch && attempt.setting_revision == revision &&
      attempt.api == api && attempt.forced_multiplier == multiplier &&
      attempt.dynamic_target == target && attempt.ui_composition == ui &&
      SameRuntimeOptions(attempt.options, copied);
  if (same) return true;
  attempt = RuntimeAttempt{};
  attempt.options = copied;
  attempt.entry = entry;
  attempt.provider_epoch = epoch;
  attempt.setting_revision = revision;
  attempt.api = api;
  attempt.forced_multiplier = multiplier;
  attempt.dynamic_target = target;
  attempt.ui_composition = ui;
  attempt.valid = true;
  return false;
}

inline bool SuppressRejectedAttempt(RuntimeAttempt& attempt, bool same) {
  if (!same || !attempt.rejected) return false;
  if (attempt.result != sl::Result::eErrorNotInitialized || attempt.transient_retries >= 3)
    return true;
  if (attempt.transient_wait != 0) {
    --attempt.transient_wait;
    return true;
  }
  ++attempt.transient_retries;
  return false;
}

inline void RecordRuntimeResult(RuntimeAttempt& attempt, sl::Result result) {
  attempt.result = result;
  attempt.rejected = result != sl::Result::eOk;
  if (result == sl::Result::eErrorNotInitialized)
    attempt.transient_wait = 8u << (attempt.transient_retries * 2);
  else if (result == sl::Result::eOk)
    attempt.transient_retries = attempt.transient_wait = 0;
}

inline void ObserveRuntimeHealth(const sl::ViewportHandle& viewport, bool ready) {
  auto* context = GetUiState(viewport);
  if (!context) return;
  const auto epoch = g_options_cache_epoch.load(std::memory_order_acquire);
  if (context->runtime_health_epoch.load(std::memory_order_acquire) == epoch &&
      context->runtime_state_ready.load(std::memory_order_relaxed) == ready) return;
  std::unique_lock lock(context->options_lock, std::try_to_lock);
  if (!lock) return;
  if (context->runtime_health_epoch != epoch) {
    context->runtime_health_epoch = epoch;
    context->runtime_state_ready = false;
  }
  if (ready && !context->runtime_state_ready) {
    bool retry = false;
    for (auto* attempt : {&context->fixed_attempt, &context->dynamic_attempt, &context->ui_attempt}) {
      if (attempt->valid && attempt->rejected && attempt->result == sl::Result::eErrorNotInitialized) {
        attempt->valid = false;
        retry = true;
      }
    }
    if (context->dynamic_probe.result == sl::Result::eErrorNotInitialized)
      context->dynamic_probe.attempted = false;
    if (retry) NotifyLiveOptionsChanged();
  }
  context->runtime_state_ready = ready;
}

inline bool ProbeStampCurrent(const DynamicProbeAttempt& probe, GetStateFn real) {
  return probe.attempted.load(std::memory_order_acquire) &&
      probe.entry.load(std::memory_order_relaxed) == reinterpret_cast<const void*>(real) &&
      probe.provider_epoch.load(std::memory_order_relaxed) == g_options_cache_epoch.load(std::memory_order_acquire) &&
      probe.setting_revision.load(std::memory_order_relaxed) == g_request_retry_revision.load(std::memory_order_acquire);
}

inline void MarkProbeStamp(DynamicProbeAttempt& probe, GetStateFn real) {
  probe.attempted.store(false, std::memory_order_release);
  probe.entry.store(reinterpret_cast<const void*>(real), std::memory_order_relaxed);
  probe.provider_epoch.store(g_options_cache_epoch.load(std::memory_order_acquire), std::memory_order_relaxed);
  probe.setting_revision.store(g_request_retry_revision.load(std::memory_order_acquire), std::memory_order_relaxed);
  probe.attempted.store(true, std::memory_order_release);
}

inline void RememberNativeDynamicState(const sl::ViewportHandle& viewport) {
  auto* context = GetUiState(viewport);
  if (!context) return;
  if (ProbeStampCurrent(context->dynamic_probe, RealGetState())) return;
  std::unique_lock lock(context->options_lock, std::try_to_lock);
  if (lock) {
    MarkProbeStamp(context->dynamic_probe, RealGetState());
    context->dynamic_probe.result.store(sl::Result::eOk, std::memory_order_relaxed);
  }
}

inline void ProbeDynamicSupport(const sl::ViewportHandle& viewport,
                                 const sl::DLSSGOptions* options) {
  const auto real = RealGetState();
  if (!real || g_entry_shutting_down.load(std::memory_order_acquire)) return;
  auto* context = GetUiState(viewport);
  if (!context) return;
  if (ProbeStampCurrent(context->dynamic_probe, real)) return;
  std::unique_lock lock(context->options_lock, std::try_to_lock);
  if (!lock || ProbeStampCurrent(context->dynamic_probe, real)) return;
  MarkProbeStamp(context->dynamic_probe, real);
  const auto probe_epoch = context->dynamic_probe.provider_epoch.load(std::memory_order_relaxed);
  const auto probe_revision = context->dynamic_probe.setting_revision.load(std::memory_order_relaxed);
  g_dynamic_probe_attempted.store(true, std::memory_order_release);
  g_dynamic_probe_calls.fetch_add(1, std::memory_order_relaxed);

  // The game may have allocated only State v1-v3. All extension fields belong
  // to this buffer, never to that allocation or its extension chains.
  sl::DLSSGState extended{};
  extended.next = nullptr;
  extended.structVersion = sl::kStructVersion4;
  extended.bIsDynamicMFGSupported = static_cast<sl::Boolean>(UINT32_MAX);
  sl::DLSSGOptions copied{};
  const sl::DLSSGOptions* query_options = nullptr;
  if (options && CopyOptions(*options, copied)) {
    copied.next = nullptr;
    copied.onErrorCallback = nullptr;
    query_options = &copied;
  }
  {
    std::stringstream message;
    message << "mfgunlock: Dynamic GetState v4 BEGIN api="
            << static_cast<uint32_t>(outputstate::GetResourceApi())
            << " viewport=" << static_cast<uint32_t>(viewport);
    reshade::log::message(reshade::log::level::info, message.str().c_str());
  }
  const sl::Result result = presentation::passive::CallNative(
      [&] { return real(viewport, extended, query_options); });
  const bool current = probe_epoch == g_options_cache_epoch.load(std::memory_order_acquire) &&
                       real == RealGetState();
  const bool current_setting = current &&
      probe_revision == g_request_retry_revision.load(std::memory_order_acquire);
  if (current) context->dynamic_probe.result = result;
  if (current_setting)
    g_dynamic_probe_result.store(static_cast<unsigned int>(result), std::memory_order_relaxed);
  const bool observed = current_setting && ObserveDynamicSupport(extended, result);
  if (current && result == sl::Result::eOk && extended.structType == sl::DLSSGState::s_structType &&
      extended.structVersion >= sl::kStructVersion1 && extended.structVersion <= sl::kStructVersion4) {
    // GetState consumes a counter. A probe made before SetOptions must carry
    // those presentations into the next successful game-owned state query.
    if (context->probe_presented_epoch != probe_epoch) context->probe_presented_pending = 0;
    context->probe_presented_epoch = probe_epoch;
    context->probe_presented_pending = (std::min)(uint64_t{UINT32_MAX},
        context->probe_presented_pending.load(std::memory_order_relaxed) + extended.numFramesActuallyPresented);
  }
  std::stringstream message;
  message << "mfgunlock: Dynamic GetState v4 attempt api="
          << static_cast<uint32_t>(outputstate::GetResourceApi())
          << " viewport=" << static_cast<uint32_t>(viewport)
          << " result=0x" << std::hex << static_cast<uint32_t>(result) << std::dec
          << " returnedAbi=" << extended.structVersion << " capability="
          << (observed ? (g_dynamic_supported.load(std::memory_order_relaxed) ? "true" : "false")
                       : "unobserved");
  if (!current_setting) message << "; diagnostic context changed during native query";
  reshade::log::message(result == sl::Result::eOk ? reshade::log::level::info
                                                  : reshade::log::level::warning,
                        message.str().c_str());
}

inline void DrainProbePresentations(const sl::ViewportHandle& viewport, sl::DLSSGState& state) {
  auto* context = GetUiState(viewport);
  if (!context) return;
  if (context->probe_presented_pending.load(std::memory_order_relaxed) == 0) return;
  std::unique_lock lock(context->options_lock, std::try_to_lock);
  if (!lock) return;
  if (context->probe_presented_epoch == g_options_cache_epoch.load(std::memory_order_acquire)) {
    state.numFramesActuallyPresented = static_cast<uint32_t>((std::min)(uint64_t{UINT32_MAX},
        uint64_t{state.numFramesActuallyPresented} + context->probe_presented_pending.load(std::memory_order_relaxed)));
  }
  context->probe_presented_pending = 0;
}

inline bool BuildUiCompositionOptions(const sl::DLSSGOptions& source,
                                      sl::DLSSGOptions& destination) {
  if (!CopyOptions(source, destination)) return false;
  if (destination.structVersion < sl::kStructVersion4)
    destination.structVersion = sl::kStructVersion4;
  const uint32_t candidate_format = g_ui_candidate_format.load(std::memory_order_acquire);
  if (CandidateInjectionRequested() &&
      g_ui_candidate_ready.load(std::memory_order_acquire) && candidate_format != 0)
    destination.uiBufferFormat = candidate_format;
  destination.enableUserInterfaceRecomposition = sl::Boolean::eTrue;
  return true;
}

inline bool CandidateOptionsSynchronized(const sl::DLSSGOptions& forwarded) {
  if (!CandidateInjectionRequested() ||
      !g_ui_candidate_ready.load(std::memory_order_acquire)) {
    return false;
  }

  const auto path = static_cast<uisubmission::Path>(
      g_ui_candidate_path.load(std::memory_order_acquire));
  if (path == uisubmission::Path::kAlphaOnly) {
    // UI Alpha carries coverage only. It does not use DLSSGOptions::uiBufferFormat,
    // so an accepted UIR options submission is sufficient to synchronize it.
    return true;
  }
  if (path != uisubmission::Path::kColorAndAlpha) return false;

  const uint32_t candidate_format =
      g_ui_candidate_format.load(std::memory_order_acquire);
  return candidate_format != 0 && forwarded.uiBufferFormat == candidate_format;
}

inline uint32_t SuppressHudSeparationResources(sl::ResourceTag* tags, uint32_t count) {
  if (tags == nullptr) return 0;
  uint32_t suppressed = 0;
  for (uint32_t index = 0; index < count; ++index) {
    if (!qualityguard::IsHudSeparationType(tags[index].type)) continue;
    tags[index].resource = nullptr;
    ++suppressed;
  }
  return suppressed;
}

inline uint32_t SuppressUiResources(sl::ResourceTag* tags, uint32_t count) {
  if (tags == nullptr) return 0;
  uint32_t suppressed = 0;
  for (uint32_t index = 0; index < count; ++index) {
    if (!qualityguard::IsUiColorOrAlphaType(tags[index].type)) continue;
    tags[index].resource = nullptr;
    ++suppressed;
  }
  return suppressed;
}

inline bool ShouldRequestUiComposition(const sl::DLSSGOptions& options) {
  return g_ui_composition_enabled.load(std::memory_order_relaxed) &&
         options.mode != sl::DLSSGMode::eOff &&
         qualityguard::ShouldRequestAutomaticUiPath(outputstate::Read());
}

using ModeObserver = void (*)(uint32_t, uint32_t, uint32_t);
inline std::atomic<ModeObserver> g_mode_observer{nullptr};

inline presentation::ProviderMode PresentationMode(sl::DLSSGMode mode) {
  switch (mode) {
    case sl::DLSSGMode::eOff: return presentation::ProviderMode::kOff;
    case sl::DLSSGMode::eOn: return presentation::ProviderMode::kFixed;
    case sl::DLSSGMode::eAuto: return presentation::ProviderMode::kAuto;
    case sl::DLSSGMode::eDynamic: return presentation::ProviderMode::kDynamic;
    default: return presentation::ProviderMode::kUnknown;
  }
}

inline sl::Result CallSetOptions(const sl::ViewportHandle& viewport,
                                 const sl::DLSSGOptions& options,
                                 countobserver::Origin origin = countobserver::Origin::kNative,
                                 bool ui_fallback = false) {
  const auto real = RealSetOptions();
  if (!real) return sl::Result::eErrorNotInitialized;
  if (origin == countobserver::Origin::kDynamic)
    g_dynamic_request_calls.fetch_add(1, std::memory_order_relaxed);
  const uint32_t diag_call =
      g_turing_diag_set_options_calls.fetch_add(1, std::memory_order_relaxed) + 1;
  const bool diag_trace = diagnostic::g_verbose.load(std::memory_order_relaxed) &&
                          diag_call <= kTuringDiagCallTraceLimit;
  if (diag_trace) {
    std::stringstream message;
    message << "mfgunlock: TuringDiagV2 SetOptions#" << diag_call
            << " BEGIN thread=" << GetCurrentThreadId()
            << " viewport=" << static_cast<uint32_t>(viewport)
            << " origin=" << static_cast<uint32_t>(origin)
            << " abi=" << options.structVersion;
    if (KnownOptionsAbi(options)) {
      message << " mode=" << static_cast<uint32_t>(options.mode)
              << " generated=" << options.numFramesToGenerate
              << " flags=0x" << std::hex << static_cast<uint32_t>(options.flags) << std::dec;
      if (options.structVersion >= sl::kStructVersion5)
        message << " dynamicTargetFPS=" << options.dynamicTargetFrameRate;
    } else {
      message << " unknown-options-abi";
    }
    TuringDiagLog(message.str());
  }
  if (KnownOptionsAbi(options)) {
    g_last_forwarded_mode.store(static_cast<uint32_t>(options.mode),
                                std::memory_order_relaxed);
    g_last_forwarded_generated.store(
        options.mode == sl::DLSSGMode::eOff ? 0u : options.numFramesToGenerate,
        std::memory_order_relaxed);
  } else {
    g_last_forwarded_mode.store(diagnostic::kUnknown32, std::memory_order_relaxed);
    g_last_forwarded_generated.store(diagnostic::kUnknown32,
                                     std::memory_order_relaxed);
  }
  if (KnownOptionsAbi(options)) {
    integration::ObserveSetOptions(
        static_cast<uint32_t>(viewport), static_cast<uint32_t>(options.flags),
        options.dynamicResWidth, options.dynamicResHeight,
        options.mvecDepthWidth, options.mvecDepthHeight,
        options.colorWidth, options.colorHeight, options.numBackBuffers,
        options.structVersion >= sl::kStructVersion3,
        options.structVersion >= sl::kStructVersion3
            ? static_cast<uint32_t>(options.queueParallelismMode)
            : integration::kUnknown32,
        GetCurrentThreadId());
  }
  const auto result = presentation::passive::CallNative([&] { return real(viewport, options); });
  if (result == sl::Result::eOk && KnownOptionsAbi(options)) {
    g_last_accepted_mode.store(static_cast<uint32_t>(options.mode),
                               std::memory_order_relaxed);
    g_last_accepted_generated.store(
        options.mode == sl::DLSSGMode::eOff ? 0u : options.numFramesToGenerate,
        std::memory_order_relaxed);
    const auto presentation_mode = PresentationMode(options.mode);
    presentation::ObserveProviderOptions(
        presentation_mode,
        presentation_mode == presentation::ProviderMode::kFixed
            ? options.numFramesToGenerate + 1u : 0u, static_cast<uint32_t>(viewport));
  } else {
    presentation::ObserveProviderUnavailable();
    g_last_accepted_mode.store(diagnostic::kUnknown32, std::memory_order_relaxed);
    g_last_accepted_generated.store(diagnostic::kUnknown32,
                                    std::memory_order_relaxed);
  }
  if (diag_trace) {
    std::stringstream message;
    message << "mfgunlock: TuringDiagV2 SetOptions#" << diag_call
            << " END result=0x" << std::hex << static_cast<uint32_t>(result);
    TuringDiagLog(message.str());
  }
  const auto observer = g_mode_observer.load(std::memory_order_acquire);
  if (KnownOptionsAbi(options) && observer) {
    const DWORD last_error = GetLastError();
    observer(static_cast<uint32_t>(viewport), static_cast<uint32_t>(options.mode),
             static_cast<uint32_t>(result));
    SetLastError(last_error);
  }
  if (!countobserver::g_callback.load(std::memory_order_relaxed)) return result;
  countobserver::Event event;
  event.operation = countobserver::Operation::kForward;
  event.origin = origin;
  event.viewport = static_cast<uint32_t>(viewport);
  event.requested = countobserver::g_context.requested;
  event.caller = countobserver::g_context.caller;
  const void* target = g_set_options_entry.target.load(std::memory_order_acquire);
  event.callee = reinterpret_cast<uint64_t>(target ? target : reinterpret_cast<const void*>(real));
  event.ui_fallback = ui_fallback;
  if (KnownOptionsAbi(options)) {
    event.value_known = 1;
    event.value = options.numFramesToGenerate;
    event.mode = static_cast<uint32_t>(options.mode);
  }
  event.result = static_cast<uint32_t>(result);
  countobserver::Emit(event);
  return result;
}

inline sl::Result ForwardSetOptions(const sl::ViewportHandle& viewport,
                                    const sl::DLSSGOptions& options,
                                    countobserver::Origin origin = countobserver::Origin::kNative) {
  if (RealSetOptions() == nullptr) return sl::Result::eErrorNotInitialized;

  const bool recompose = ShouldRequestUiComposition(options);
  if (!recompose) {
    if (g_options_call_depth <= 1) {
      if (auto* context = GetUiState(viewport)) context->ui_attempt.valid = false;
    }
    ObserveUiOptionsTransition(viewport, options);
    const sl::Result result = CallSetOptions(viewport, options, origin);
    if (result == sl::Result::eOk) {
      g_ui_composition_applied.store(false, std::memory_order_relaxed);
      g_ui_candidate_options_synced.store(false, std::memory_order_release);
    }
    return result;
  }

  sl::DLSSGOptions forwarded{};
  if (!BuildUiCompositionOptions(options, forwarded)) {
    g_ui_composition_applied.store(false, std::memory_order_relaxed);
    g_ui_candidate_options_synced.store(false, std::memory_order_release);
    g_ui_composition_result.store(
        static_cast<unsigned int>(sl::Result::eErrorUnsupportedInterface),
        std::memory_order_relaxed);
    if (!g_ui_composition_fell_back.exchange(true, std::memory_order_relaxed)) {
      reshade::log::message(
          reshade::log::level::warning,
          "mfgunlock: UI Composition was not submitted because the game supplied an "
          "unknown DLSSGOptions ABI; native options were preserved.");
    }
    ObserveUiOptionsTransition(viewport, options);
    return CallSetOptions(viewport, options, origin);
  }

  g_ui_composition_source_version.store(
      static_cast<unsigned int>(options.structVersion), std::memory_order_relaxed);
  RuntimeAttempt uncached;
  auto* context = g_options_call_depth <= 1 ? GetUiState(viewport) : nullptr;
  auto& attempt = context ? context->ui_attempt : uncached;
  const bool same = PrepareRuntimeAttempt(attempt, forwarded);
  if (SuppressRejectedAttempt(attempt, same)) {
    ObserveUiOptionsTransition(viewport, options);
    const auto fallback = CallSetOptions(viewport, options, origin, true);
    g_ui_candidate_options_synced.store(false, std::memory_order_release);
    g_ui_composition_applied.store(false, std::memory_order_relaxed);
    return fallback;
  }
  const sl::Result result = CallSetOptions(viewport, forwarded, origin);
  RecordRuntimeResult(attempt, result);
  g_ui_composition_result.store(static_cast<unsigned int>(result),
                                std::memory_order_relaxed);
  if (result == sl::Result::eOk) {
    ObserveUiOptionsTransition(viewport, forwarded);
    g_ui_candidate_options_synced.store(
        CandidateOptionsSynchronized(forwarded), std::memory_order_release);
    g_ui_composition_fell_back.store(false, std::memory_order_relaxed);
    if (!g_ui_composition_applied.exchange(true, std::memory_order_relaxed)) {
      reshade::log::message(
          reshade::log::level::info,
          "mfgunlock: Streamline accepted the guarded UI Composition path; optional HUD/UI "
          "resources remain gated by metadata validation.");
    }
    return result;
  }

  g_ui_candidate_options_synced.store(false, std::memory_order_release);
  if (!g_ui_composition_fell_back.exchange(true, std::memory_order_relaxed)) {
    std::stringstream message;
    message << "mfgunlock: UI Composition was rejected with sl::Result "
            << static_cast<unsigned int>(result)
            << "; restoring the game's native final-color path.";
    reshade::log::message(reshade::log::level::warning, message.str().c_str());
  }
  ObserveUiOptionsTransition(viewport, options);
  const auto fallback = CallSetOptions(viewport, options, origin, true);
  if (fallback == sl::Result::eOk)
    g_ui_composition_applied.store(false, std::memory_order_relaxed);
  return fallback;
}

inline sl::Result ForwardFixedNative(const sl::ViewportHandle& viewport,
                                     const sl::DLSSGOptions& options,
                                     FixedOverrideStatus status) {
  const unsigned int requested = options.numFramesToGenerate;
  const sl::Result result = ForwardSetOptions(viewport, options,
      status == FixedOverrideStatus::kRejectedNativeFallback
          ? countobserver::Origin::kNativeFallback : countobserver::Origin::kNative);
  if (result == sl::Result::eOk)
    g_dynamic_applied.store(options.mode == sl::DLSSGMode::eDynamic, std::memory_order_relaxed);
  SetFixedOverrideState(result != sl::Result::eOk
                            ? (status == FixedOverrideStatus::kRejectedNativeFallback
                                   ? FixedOverrideStatus::kFallbackRejected
                                   : FixedOverrideStatus::kNativeRejected)
                            : status,
                       requested, result == sl::Result::eOk ? requested : 0);
  return result;
}

inline sl::Result ForwardFixedCount(const sl::ViewportHandle& viewport,
                                    const sl::DLSSGOptions& options, uint32_t count,
                                    countobserver::Origin origin = countobserver::Origin::kFixed) {
  sl::DLSSGOptions forwarded{};
  if (!CopyOptions(options, forwarded)) return CallSetOptions(viewport, options, origin);
  forwarded.numFramesToGenerate = count;
  const sl::Result result = ForwardSetOptions(viewport, forwarded, origin);
  if (result == sl::Result::eOk)
    g_dynamic_applied.store(options.mode == sl::DLSSGMode::eDynamic, std::memory_order_relaxed);
  return result;
}

inline bool BuildDynamicOptions(const sl::DLSSGOptions& source,
                                sl::DLSSGOptions& destination) {
  const float target = g_dynamic_target_fps.load(std::memory_order_relaxed);
  if (!runtimecontrols::ValidDynamicTarget(target) || !CopyOptions(source, destination)) return false;
  destination.structVersion = sl::kStructVersion5;
  destination.mode = source.mode == sl::DLSSGMode::eOff
                         ? sl::DLSSGMode::eOff : sl::DLSSGMode::eDynamic;
  destination.dynamicTargetFrameRate = target;
  return true;
}

inline sl::Result InitWithPreferences(const sl::Preferences& pref, uint64_t sdk_version) {
  if (g_real_init == nullptr) return sl::Result::eErrorNotInitialized;
  if (!g_enabled.load()) return g_real_init(pref, sdk_version);

  const auto mode = static_cast<RuntimeSelectionMode>(
      g_runtime_selection_mode.load(std::memory_order_relaxed));
  if (mode == RuntimeSelectionMode::kGameDefault) return g_real_init(pref, sdk_version);

  if (pref.structType != sl::Preferences::s_structType ||
      pref.structVersion != sl::kStructVersion1) {
    reshade::log::message(
        reshade::log::level::warning,
        "mfgunlock: runtime selection ignored because sl::Preferences uses an unknown ABI.");
    return g_real_init(pref, sdk_version);
  }

  constexpr uint64_t kOta = static_cast<uint64_t>(sl::PreferenceFlags::eAllowOTA) |
                            static_cast<uint64_t>(sl::PreferenceFlags::eLoadDownloadedPlugins);
  const uint64_t before = static_cast<uint64_t>(pref.flags);
  const uint64_t after = mode == RuntimeSelectionMode::kPreferLocal
                             ? (before & ~kOta)
                             : (before | kOta);

  sl::Preferences forwarded = pref;
  forwarded.flags = static_cast<sl::PreferenceFlags>(after);
  const sl::Result result = g_real_init(forwarded, sdk_version);

  std::stringstream message;
  message << "mfgunlock: Streamline runtime policy "
          << (mode == RuntimeSelectionMode::kPreferLocal ? "prefer-local" : "force-OTA")
          << " changed flags 0x" << std::hex << before << " -> 0x" << after;
  reshade::log::message(result == sl::Result::eOk ? reshade::log::level::info
                                                  : reshade::log::level::warning,
                        message.str().c_str());
  return result;
}

inline sl::Result HookedInit(const sl::Preferences& pref, uint64_t sdk_version) {
  const auto previous = outputstate::GetResourceApi();
  if (pref.structType == sl::Preferences::s_structType && pref.structVersion == sl::kStructVersion1) {
    if (pref.renderAPI == sl::RenderAPI::eVulkan)
      outputstate::SetResourceApi(outputstate::ResourceApi::kVulkan);
    else if (pref.renderAPI == sl::RenderAPI::eD3D12)
      outputstate::SetResourceApi(outputstate::ResourceApi::kD3D12);
  }
  const auto result = g_on_init ? g_on_init(pref, sdk_version, InitWithPreferences)
                               : InitWithPreferences(pref, sdk_version);
  if (result != sl::Result::eOk) outputstate::SetResourceApi(previous);
  return result;
}

inline sl::Result SetOptionsImpl(const sl::ViewportHandle& viewport,
                                  const sl::DLSSGOptions& options, uint64_t caller,
                                  bool automatic = false) {
  presentation::passive::CpuScope timing(presentation::passive::CpuKind::kSetOptions);
  if (RealSetOptions() == nullptr) return sl::Result::eErrorNotInitialized;
  struct CallDepth {
    CallDepth() { ++g_options_call_depth; }
    ~CallDepth() { --g_options_call_depth; }
  } call_depth;
  const bool known = KnownOptionsAbi(options);
  const bool vulkan = outputstate::GetResourceApi() == outputstate::ResourceApi::kVulkan;
  if (known) {
    g_last_requested_mode.store(static_cast<uint32_t>(options.mode),
                                std::memory_order_relaxed);
    g_last_requested_generated.store(
        options.mode == sl::DLSSGMode::eOff ? 0u : options.numFramesToGenerate,
        std::memory_order_relaxed);
  } else {
    g_last_requested_mode.store(diagnostic::kUnknown32, std::memory_order_relaxed);
    g_last_requested_generated.store(diagnostic::kUnknown32,
                                     std::memory_order_relaxed);
  }
  const countobserver::Scope scope(caller,
      known ? options.numFramesToGenerate : countobserver::kUnknown);
  if (countobserver::g_callback.load(std::memory_order_relaxed)) {
    countobserver::Event event;
    event.caller = countobserver::g_context.caller;
    event.viewport = static_cast<uint32_t>(viewport);
    event.origin = countobserver::Origin::kNative;
    event.value_known = known;
    event.requested = countobserver::g_context.requested;
    if (known) {
      event.value = options.numFramesToGenerate;
      event.mode = static_cast<uint32_t>(options.mode);
    }
    countobserver::Emit(event);
  }
  const auto* profile = architecture::ActiveProfile();
  if (!g_enabled.load()) {
    if (g_options_call_depth == 1) {
      if (auto* context = GetUiState(viewport)) {
        context->fixed_attempt.valid = false;
        context->dynamic_attempt.valid = false;
        context->ui_attempt.valid = false;
        context->pacing_attempt.valid = false;
      }
    }
    const auto result = CallSetOptions(viewport, options);
    if (known && result == sl::Result::eOk) {
      g_dynamic_applied.store(false, std::memory_order_relaxed);
      SetFixedOverrideState(FixedOverrideStatus::kNative,
                            options.numFramesToGenerate, options.numFramesToGenerate);
    }
    return result;
  }

  if (!KnownOptionsAbi(options)) {
    SetFixedOverrideState(FixedOverrideStatus::kUnknownOptionsAbi, 0, 0);
    g_dynamic_applied.store(false, std::memory_order_relaxed);
    return CallSetOptions(viewport, options);
  }

  // HookedSetOptions/ApplyLiveOptions already own this viewport's recursive
  // options lock. No global hot-path mutex, scanner, or provider sweep is added.
  auto* context = g_options_call_depth == 1 ? GetUiState(viewport) : nullptr;
  if (options.mode == sl::DLSSGMode::eOff) {
    if (context) {
      context->fixed_attempt.valid = false;
      context->dynamic_attempt.valid = false;
      context->ui_attempt.valid = false;
      context->pacing_attempt.valid = false;
      context->dynamic_probe.attempted = false;
      context->runtime_state_ready = false;
    }
    g_dynamic_applied.store(false, std::memory_order_relaxed);
    g_dynamic_runtime_declined.store(false, std::memory_order_relaxed);
    const sl::Result result = ForwardSetOptions(viewport, options);
    SetFixedOverrideState(result == sl::Result::eOk ? FixedOverrideStatus::kNative
                                                   : FixedOverrideStatus::kNativeRejected, 0, 0);
    return result;
  }

  if (g_dynamic_mfg_enabled.load(std::memory_order_relaxed)) {
    // Observation is independent of admission: neither Boolean controls the
    // request, retry cache or readiness of unrelated hooks.
    ProbeDynamicSupport(viewport, &options);
    RuntimeAttempt uncached;
    auto& attempt = context ? context->dynamic_attempt : uncached;
    const bool same = PrepareRuntimeAttempt(attempt, options);
    if (SuppressRejectedAttempt(attempt, same)) {
      // Only a real previous setter failure reaches this path. A different
      // viewport, setting, native context, entrypoint or provider epoch retries.
      g_dynamic_suppressed_repeats.fetch_add(1, std::memory_order_relaxed);
      g_dynamic_runtime_declined.store(true, std::memory_order_release);
      g_dynamic_result.store(static_cast<unsigned int>(attempt.result), std::memory_order_relaxed);
    } else {
      sl::DLSSGOptions dynamic_options{};
      if (BuildDynamicOptions(options, dynamic_options)) {
        if (!same || attempt.rejected) {
          std::stringstream message;
          message << "mfgunlock: NVIDIA Dynamic MFG SetOptions BEGIN api="
                  << static_cast<uint32_t>(outputstate::GetResourceApi())
                  << " viewport=" << static_cast<uint32_t>(viewport)
                  << " abi=" << dynamic_options.structVersion
                  << " mode=" << static_cast<uint32_t>(dynamic_options.mode)
                  << " targetFPS=" << dynamic_options.dynamicTargetFrameRate;
          reshade::log::message(reshade::log::level::info, message.str().c_str());
        }
        const sl::Result first = ForwardSetOptions(viewport, dynamic_options,
                                                   countobserver::Origin::kDynamic);
        const sl::Result result = first == sl::Result::eOk ? first
            : ForwardSetOptions(viewport, dynamic_options, countobserver::Origin::kDynamic);
        const bool changed = !same || attempt.result != result ||
                             attempt.rejected != (result != sl::Result::eOk);
        RecordRuntimeResult(attempt, result);
        g_dynamic_result.store(static_cast<unsigned int>(result), std::memory_order_relaxed);
        g_dynamic_runtime_declined.store(attempt.rejected, std::memory_order_release);
        g_dynamic_applied.store(!attempt.rejected, std::memory_order_relaxed);
        g_dynamic_fell_back.store(attempt.rejected, std::memory_order_relaxed);
        if (changed) {
          std::stringstream message;
          message << "mfgunlock: NVIDIA Dynamic MFG SetOptions attempt api="
                  << static_cast<uint32_t>(outputstate::GetResourceApi())
                  << " viewport=" << static_cast<uint32_t>(viewport)
                  << " abi=" << dynamic_options.structVersion
                  << " mode=" << static_cast<uint32_t>(dynamic_options.mode)
                  << " targetFPS=" << dynamic_options.dynamicTargetFrameRate
                  << " capability="
                  << (!g_dynamic_support_seen.load(std::memory_order_acquire) ? "unobserved"
                      : g_dynamic_supported.load(std::memory_order_relaxed) ? "true" : "false")
                  << " firstResult=0x" << std::hex << static_cast<uint32_t>(first)
                  << " finalResult=0x" << static_cast<uint32_t>(result)
                  << (attempt.rejected
                          ? "; native rejection, restoring fixed/native request until context changes."
                          : "; accepted by the native setter (presentation remains separately observed).");
          reshade::log::message(attempt.rejected ? reshade::log::level::warning
                                                : reshade::log::level::info,
                                message.str().c_str());
        }
        if (result == sl::Result::eOk) {
          SetFixedOverrideState(FixedOverrideStatus::kNative, options.numFramesToGenerate, 0);
          return result;
        }
      }
    }
  } else {
    if (context) context->dynamic_attempt.valid = false;
    g_dynamic_runtime_declined.store(false, std::memory_order_release);
  }

  if (options.mode == sl::DLSSGMode::eDynamic) {
    const auto result = ForwardSetOptions(viewport, options);
    g_dynamic_applied.store(result == sl::Result::eOk, std::memory_order_relaxed);
    SetFixedOverrideState(result == sl::Result::eOk ? FixedOverrideStatus::kNative
                                                   : FixedOverrideStatus::kNativeRejected,
                          options.numFramesToGenerate, 0);
    return result;
  }

  const unsigned int multiplier = g_force_multiplier.load(std::memory_order_relaxed);
  const uint32_t desired = multiplier >= 2 ? multiplier - 1 : options.numFramesToGenerate;
  const uint32_t requested = options.numFramesToGenerate;
  // Keep the existing D3D pacing preparation as best effort. Neither missing
  // readiness nor a scanner estimate authorizes rewriting or refusing a native
  // request before the runtime sees it. Dynamic leaves NVIDIA pacing untouched.
  if (!vulkan && !automatic && g_ensure_pacing &&
      ((profile && profile->RequiresProviderRetarget() && requested > 1) ||
       (multiplier >= 2 && desired > 1)) &&
      (!g_pacing_ready || !g_pacing_ready())) {
    // Some optional recipes cannot locate a marker. Repeating their full scan
    // on every game call is unnecessary; provider/settings/context changes
    // rearm this best-effort preparation independently from native admission.
    RuntimeAttempt uncached;
    auto& preparation = context ? context->pacing_attempt : uncached;
    if (!PrepareRuntimeAttempt(preparation, options)) g_ensure_pacing();
  }
  if (multiplier < 2) {
    if (context) context->fixed_attempt.valid = false;
    return ForwardFixedNative(viewport, options, FixedOverrideStatus::kNative);
  }
  if (requested == desired) {
    if (context) context->fixed_attempt.valid = false;
    return ForwardFixedNative(viewport, options, FixedOverrideStatus::kAlreadyMatched);
  }

  RuntimeAttempt uncached;
  auto& attempt = context ? context->fixed_attempt : uncached;
  const bool same = PrepareRuntimeAttempt(attempt, options);
  if (SuppressRejectedAttempt(attempt, same)) {
    g_fixed_suppressed_repeats.fetch_add(1, std::memory_order_relaxed);
    return ForwardFixedNative(viewport, options, FixedOverrideStatus::kRejectedNativeFallback);
  }

  // Keep the previous terminal UI status while this synchronous call executes.
  // Pending -> Applied on every frame previously produced thousands of logs.
  if (!same || attempt.rejected) {
    std::stringstream message;
    message << "mfgunlock: fixed MFG SetOptions BEGIN viewport=" << static_cast<uint32_t>(viewport)
            << " requested=" << requested << " forced=" << desired;
    reshade::log::message(reshade::log::level::info, message.str().c_str());
  }
  const sl::Result first = ForwardFixedCount(viewport, options, desired);
  const sl::Result result = first == sl::Result::eOk ? first
      : ForwardFixedCount(viewport, options, desired, countobserver::Origin::kRetry);
  const bool changed = !same || attempt.result != result ||
                       attempt.rejected != (result != sl::Result::eOk);
  RecordRuntimeResult(attempt, result);
  if (attempt.rejected) {
    g_force_failed_for.store(desired, std::memory_order_relaxed);
    if (changed) {
      std::stringstream message;
      message << "mfgunlock: slDLSSGSetOptions refused numFramesToGenerate=" << desired
              << " twice (sl::Result " << static_cast<unsigned int>(first) << " then "
              << static_cast<unsigned int>(result) << "); falling back to the game's request of "
              << requested << ". The same rejected override will wait for a context change.";
      reshade::log::message(reshade::log::level::warning, message.str().c_str());
    }
    return ForwardFixedNative(viewport, options, FixedOverrideStatus::kRejectedNativeFallback);
  }

  g_force_failed_for.store(0, std::memory_order_relaxed);
  SetFixedOverrideState(first == sl::Result::eOk ? FixedOverrideStatus::kApplied
                                               : FixedOverrideStatus::kAppliedAfterRetry,
                        requested, desired);
  if (changed) {
    std::stringstream message;
    message << "mfgunlock: forcing DLSS-G numFramesToGenerate from " << requested << " to " << desired
            << " (" << multiplier << "x). slDLSSGSetOptions accepted it";
    if (first != sl::Result::eOk)
      message << " on retry after result " << static_cast<unsigned int>(first);
    message << ".";
    reshade::log::message(reshade::log::level::info, message.str().c_str());
  }
  return result;
}

inline sl::Result HookedSetOptions(const sl::ViewportHandle& viewport,
                                   const sl::DLSSGOptions& options) {
  const uint64_t caller = countobserver::CallerAddress();
  auto* state = GetUiState(viewport);
  if (!state) return SetOptionsImpl(viewport, options, caller);
  std::lock_guard lock(state->options_lock);
  // Reentrant callbacks are forwarded, but disqualify this snapshot for replay.
  if (state->options_in_call) {
    state->native_options_valid = false;
    return SetOptionsImpl(viewport, options, caller);
  }
  state->options_in_call = true;
  struct Finish {
    UiViewportState& state;
    ~Finish() { state.options_in_call = false; }
  } finish{*state};
  state->native_options_valid = false;
  state->cache_epoch = g_options_cache_epoch.load(std::memory_order_acquire);
  state->options_thread = GetCurrentThreadId();
  state->options_entry = reinterpret_cast<const void*>(RealSetOptions());
  state->attempted_revision = g_options_revision.load(std::memory_order_acquire);
  // next and error callbacks belong to the game. Their lifetime is not extended
  // by copying DLSSGOptions, so such options are forwarded only synchronously.
  if (KnownOptionsAbi(options) && options.next == nullptr && options.onErrorCallback == nullptr)
    state->native_options_valid = CopyOptions(options, state->native_options);
  const auto result = SetOptionsImpl(viewport, options, caller);
  if (result != sl::Result::eOk) state->native_options_valid = false;
  return result;
}

inline void ApplyLiveOptions(const sl::ViewportHandle& viewport) {
  auto* state = GetUiState(viewport);
  if (!state || g_entry_shutting_down.load(std::memory_order_acquire)) return;
  std::unique_lock lock(state->options_lock, std::try_to_lock);
  if (!lock || state->options_in_call || !state->native_options_valid ||
      state->cache_epoch != g_options_cache_epoch.load(std::memory_order_acquire) ||
      state->options_thread != GetCurrentThreadId() ||
      g_present_thread.load(std::memory_order_acquire) != GetCurrentThreadId() ||
      state->options_entry != reinterpret_cast<const void*>(RealSetOptions())) return;
  const auto revision = g_options_revision.load(std::memory_order_acquire);
  if (state->attempted_revision == revision) return;
  // Do not re-enable a game-disabled feature, including pauses and resizes.
  if (state->native_options.mode == sl::DLSSGMode::eOff) return;
  state->attempted_revision = revision;
  state->options_in_call = true;
  struct Finish {
    UiViewportState& state;
    ~Finish() { state.options_in_call = false; }
  } finish{*state};
  sl::DLSSGOptions options{};
  CopyOptions(state->native_options, options);
  diagnostic::LifecycleEvent event;
  event.kind = diagnostic::LifecycleKind::kLiveReapply;
  event.thread = GetCurrentThreadId();
  event.viewport = static_cast<uint32_t>(viewport);
  event.mode = static_cast<uint32_t>(options.mode);
  // Replay only on the observed presenting AND native-options thread. Never
  // impersonate the game caller; tracing retains override/retry/fallback.
  try {
    event.result = static_cast<uint32_t>(SetOptionsImpl(viewport, options, 0, true));
  } catch (...) {
    state->native_options_valid = false;
    diagnostic::Emit(event);
    return;
  }
  if (event.result != static_cast<uint32_t>(sl::Result::eOk))
    state->native_options_valid = false;
  diagnostic::Emit(event);
}

inline void InvalidateLiveOptions() {
  g_native_maximum.store(runtimecontrols::kUnknownMaximum, std::memory_order_release);
  g_options_cache_epoch.fetch_add(1, std::memory_order_acq_rel);
  g_present_thread.store(0, std::memory_order_release);
}

inline void MarkInjectedUiPair(UiViewportState* state, bool alpha_only,
                               bool frame_based) {
  if (state == nullptr) return;
  AcquireSRWLockExclusive(&state->lock);
  const bool was_eligible =
      state->ui_recomposition_eligible.exchange(true, std::memory_order_acq_rel);
  state->hudless_color_seen.store(true, std::memory_order_release);
  state->ui_color_or_alpha_seen.store(true, std::memory_order_release);
  if (alpha_only)
    state->ui_alpha_seen.store(true, std::memory_order_release);
  else
    state->ui_color_alpha_seen.store(true, std::memory_order_release);
  state->ui_recomposition_invalid.store(false, std::memory_order_release);
  state->hud_separation_suppressed.store(false, std::memory_order_release);
  if (!was_eligible) RequestUiReset(state);
  ReleaseSRWLockExclusive(&state->lock);

  if (alpha_only) {
    MarkAutomaticUiTag(state, sl::kBufferTypeUIAlpha, frame_based);
  } else if (frame_based) {
    MarkAutomaticUiTag(state, sl::kBufferTypeUIColorAndAlpha, true);
  } else if (state != nullptr) {
    // Color+Alpha keeps the pre-Phase-6 eOnlyValidNow semantics. SL owns any
    // copy it needs after slSetTag returns, so do not create persistent legacy
    // ownership that would later be null-tagged between frames.
    ClearAutomaticUiTagState(state);
  }

  g_ui_pair_eligible.store(true, std::memory_order_relaxed);
  g_ui_native_hudless_fallback.store(false, std::memory_order_relaxed);
}

template <typename Forward>
inline sl::Result TryInjectUiCandidate(UiViewportState* state,
                                      const UiCandidateSnapshot& candidate,
                                      const sl::ResourceTag* tags, uint32_t count,
                                      sl::CommandBuffer* command_buffer,
                                      bool frame_based,
                                      Forward&& forward) {
  g_ui_candidate_active.store(false, std::memory_order_relaxed);
  if (frame_based && state != nullptr &&
      state->automatic_ui_tag_global.load(std::memory_order_acquire) &&
      !ReleaseGlobalAutomaticUiTag(state)) {
    return forward(tags, count);
  }
  const bool injection_requested = CandidateInjectionRequested();
  const bool options_synced =
      g_ui_candidate_options_synced.load(std::memory_order_acquire);
  const auto path = CandidateSubmissionPath(candidate);
  const bool viewport_enabled = state == nullptr ||
      (state->options_seen.load(std::memory_order_acquire) &&
       state->mode.load(std::memory_order_relaxed) != static_cast<uint32_t>(sl::DLSSGMode::eOff));
  const bool can_inject = viewport_enabled && path != uisubmission::Path::kUnavailable &&
      uicandidate::CanInject(candidate.reject_reasons, candidate.stable_frames,
                            command_buffer != nullptr, options_synced,
                            injection_requested);
  if (can_inject && count >= 64) {
    if (state && state->automatic_ui_tag_global.load()) state->automatic_ui_clear_pending.store(true);
    g_ui_candidate_full_batches.fetch_add(1, std::memory_order_relaxed);
    return ForwardWithAutomaticUiRelease(
        state, tags, count, frame_based, std::forward<Forward>(forward));
  }
  if (!can_inject) {
    if (state && state->automatic_ui_tag_global.load(std::memory_order_acquire))
      state->automatic_ui_clear_pending.store(true, std::memory_order_release);
    if (injection_requested && options_synced &&
        candidate.reject_reasons == uicandidate::kAccept &&
        uicandidate::IsStable(candidate.stable_frames) && command_buffer == nullptr) {
      g_ui_candidate_missing_command_buffer.fetch_add(1, std::memory_order_relaxed);
    }
    return ForwardWithAutomaticUiRelease(
        state, tags, count, frame_based, std::forward<Forward>(forward));
  }
  if (ConsumeCandidateRetryFrame()) {
    if (state && state->automatic_ui_tag_global.load()) state->automatic_ui_clear_pending.store(true);
    return ForwardWithAutomaticUiRelease(
        state, tags, count, frame_based, std::forward<Forward>(forward));
  }
  const bool alpha_only = path == uisubmission::Path::kAlphaOnly;
  UiAlphaCandidateSnapshot alpha_candidate{};
  if (alpha_only) {
    // Forward the game's complete batch ONCE, on its original command list.
    // Moving volatile host tags to our list would read them before host work.
    const sl::Result host_result = forward(tags, count);
    if (host_result != sl::Result::eOk) return host_result;
    const auto clear_owned = [&]() {
      if (state == nullptr || !state->automatic_ui_tag_global.load()) return;
      state->automatic_ui_clear_pending.store(true, std::memory_order_release);
      ForwardWithAutomaticUiRelease(state, nullptr, 0, frame_based, forward);
    };
    if constexpr (!std::is_invocable_r_v<sl::Result, Forward,
        const sl::ResourceTag*, uint32_t, sl::CommandBuffer*>) {
      // An alpha producer without an explicit copy-list contract cannot be used.
      clear_owned();
      return host_result;
    } else {
      if (g_prepare_ui_alpha_candidate == nullptr || g_finish_ui_alpha_candidate == nullptr ||
          !g_prepare_ui_alpha_candidate(&candidate, command_buffer, &alpha_candidate)) {
        clear_owned();
        return host_result;
      }
      if (!alpha_candidate.copy_commands || alpha_candidate.native_resource == 0 ||
          alpha_candidate.width != candidate.width || alpha_candidate.height != candidate.height ||
          alpha_candidate.format == 0) {
        g_finish_ui_alpha_candidate(&candidate, &alpha_candidate, false, sl::Result::eOk);
        clear_owned();
        return host_result;
      }
      sl::Resource alpha_resource(sl::ResourceType::eTex2d,
          reinterpret_cast<void*>(static_cast<uintptr_t>(alpha_candidate.native_resource)),
          alpha_candidate.state);
      alpha_resource.width = candidate.width;
      alpha_resource.height = candidate.height;
      alpha_resource.nativeFormat = alpha_candidate.format;
      sl::Extent extent{}; extent.width = candidate.width; extent.height = candidate.height;
      // SL owns the Present-time clone. The helper fence only protects our
      // extraction/copy source. CPU Present must NEVER release a live slot.
      const sl::ResourceTag alpha_tag(&alpha_resource, sl::kBufferTypeUIAlpha,
          sl::ResourceLifecycle::eOnlyValidNow, &extent);
      const sl::Result tag_result = forward(&alpha_tag, 1, alpha_candidate.copy_commands);
      const bool accepted = g_finish_ui_alpha_candidate(
          &candidate, &alpha_candidate, tag_result == sl::Result::eOk, tag_result);
      g_ui_candidate_last_result.store(static_cast<uint32_t>(tag_result));
      if (accepted) {
        g_ui_candidate_consecutive_failures.store(0);
        g_ui_candidate_retry_frames.store(0);
        g_ui_candidate_last_failed_resource.store(0);
        g_ui_candidate_tags_injected.fetch_add(1);
        g_ui_candidate_active.store(true);
        MarkInjectedUiPair(state, true, frame_based);
      } else {
        // Even a rejected call can have partially installed a tag or recorded
        // GPU commands. Finish sealed that work; now retire our optional tag.
        if (state != nullptr) MarkAutomaticUiTag(state, sl::kBufferTypeUIAlpha, frame_based);
        if (frame_based) {
          const sl::ResourceTag clear(nullptr, sl::kBufferTypeUIAlpha,
                                      sl::ResourceLifecycle::eOnlyValidNow);
          if (forward(&clear, 1) == sl::Result::eOk && state != nullptr)
            state->automatic_ui_frame_type.store(0, std::memory_order_release);
        } else {
          clear_owned();
        }
        g_ui_candidate_injection_failures.fetch_add(1);
        g_ui_candidate_native_fallbacks.fetch_add(1);
        g_ui_candidate_retry_frames.store(uicandidate::kRetryCooldownFrames);
      }
      // Optional helper failure must not change an accepted native submission
      // into an application error, nor re-submit the game's volatile batch.
      return host_result;
    }
  }
  if (g_acquire_ui_candidate == nullptr || g_release_ui_candidate == nullptr ||
      !g_acquire_ui_candidate(&candidate)) {
    return ForwardWithAutomaticUiRelease(
        state, tags, count, frame_based, std::forward<Forward>(forward));
  }

  // Preserve the SDR ColorAlpha resource/encoding policy, but isolate its
  // optional call too: SL batches are not transactional. Replaying the game's
  // volatile HUDless tags after an optional rejection can enqueue a second copy.
  if (state && state->automatic_ui_tag_global.load(std::memory_order_acquire))
    state->automatic_ui_clear_pending.store(true, std::memory_order_release);
  const auto host_result = ForwardWithAutomaticUiRelease(
      state, tags, count, frame_based, forward);
  if (host_result != sl::Result::eOk) {
    g_release_ui_candidate(&candidate, false, host_result);
    return host_result;
  }
  sl::Resource ui_resource(sl::ResourceType::eTex2d,
      reinterpret_cast<void*>(static_cast<uintptr_t>(candidate.native_resource)),
      candidate.state);
  ui_resource.width = candidate.width;
  ui_resource.height = candidate.height;
  ui_resource.nativeFormat = candidate.format;
  sl::Extent ui_extent{};
  ui_extent.width = candidate.width;
  ui_extent.height = candidate.height;
  const sl::ResourceTag ui_tag(&ui_resource, sl::kBufferTypeUIColorAndAlpha,
      sl::ResourceLifecycle::eOnlyValidNow, &ui_extent);
  const sl::Result result = forward(&ui_tag, 1);
  g_ui_candidate_last_result.store(static_cast<unsigned int>(result),
                                   std::memory_order_relaxed);
  g_release_ui_candidate(&candidate, true, result);

  if (result == sl::Result::eOk) {
    const uint32_t previous_failures =
        g_ui_candidate_consecutive_failures.exchange(0, std::memory_order_acq_rel);
    g_ui_candidate_last_failed_resource.store(0, std::memory_order_release);
    g_ui_candidate_retry_frames.store(0, std::memory_order_release);
    if (previous_failures != 0)
      g_ui_candidate_recoveries.fetch_add(1, std::memory_order_relaxed);
    g_ui_candidate_tags_injected.fetch_add(1, std::memory_order_relaxed);
    g_ui_candidate_active.store(true, std::memory_order_relaxed);
    MarkInjectedUiPair(state, alpha_only, frame_based);
    return result;
  }

  g_ui_candidate_injection_failures.fetch_add(1, std::memory_order_relaxed);
  g_ui_candidate_native_fallbacks.fetch_add(1, std::memory_order_relaxed);
  const uint64_t previous_resource =
      g_ui_candidate_last_failed_resource.exchange(candidate.native_resource,
                                                   std::memory_order_acq_rel);
  const uint32_t previous_streak =
      g_ui_candidate_consecutive_failures.load(std::memory_order_acquire);
  const uint32_t failure_streak = uicandidate::NextFailureStreak(
      previous_resource == candidate.native_resource, previous_streak);
  g_ui_candidate_consecutive_failures.store(failure_streak,
                                            std::memory_order_release);
  g_ui_candidate_retry_frames.store(uicandidate::kRetryCooldownFrames,
                                    std::memory_order_release);

  if (uicandidate::ShouldHardDecline(failure_streak)) {
    g_ui_candidate_runtime_declined.store(true, std::memory_order_release);
    g_ui_candidate_options_synced.store(false, std::memory_order_release);
    std::stringstream message;
    message << "mfgunlock: detected " << uisubmission::PathName(path)
            << " tag failed " << failure_streak
            << " consecutive times on resource 0x" << std::hex
            << candidate.native_resource << std::dec << " (sl::Result "
            << static_cast<unsigned int>(result)
            << "); disabling automatic UI injection until the option is toggled.";
    reshade::log::message(reshade::log::level::warning, message.str().c_str());
  } else if (failure_streak == 1) {
    std::stringstream message;
    message << "mfgunlock: detected " << uisubmission::PathName(path)
            << " tag was transiently rejected with "
            << "sl::Result " << static_cast<unsigned int>(result)
            << "; preserving native HUD-less and retrying after candidate revalidation.";
    reshade::log::message(reshade::log::level::warning, message.str().c_str());
  }
  // Retire only the optional UI we attempted; never replay native inputs.
  MarkAutomaticUiTag(state, sl::kBufferTypeUIColorAndAlpha, frame_based);
  if (frame_based) {
    const sl::ResourceTag clear(nullptr, sl::kBufferTypeUIColorAndAlpha,
                                sl::ResourceLifecycle::eOnlyValidNow);
    if (forward(&clear, 1) == sl::Result::eOk && state)
      state->automatic_ui_frame_type.store(0);
  } else if (state) {
    state->automatic_ui_clear_pending.store(true, std::memory_order_release);
    ForwardWithAutomaticUiRelease(state, nullptr, 0, false, forward);
  } else {
    const sl::ResourceTag clear(nullptr, sl::kBufferTypeUIColorAndAlpha,
                                sl::ResourceLifecycle::eOnlyValidNow);
    forward(&clear, 1);
  }
  return host_result;
}

template <typename Forward>
inline sl::Result FilterUiTags(const sl::ViewportHandle& viewport,
                               const sl::ResourceTag* tags, uint32_t count,
                               sl::CommandBuffer* command_buffer,
                               bool frame_based,
                               Forward&& forward, uint64_t source_frame = UINT64_MAX) {
  UiViewportState* state = GetUiState(viewport);
  if (tags == nullptr || count == 0)
    return forward(tags, count);
  if (!g_enabled.load(std::memory_order_relaxed) ||
      !g_ui_composition_enabled.load(std::memory_order_relaxed)) {
    return ForwardWithAutomaticUiRelease(
        state, tags, count, frame_based, std::forward<Forward>(forward));
  }

  if (count > 64) {
    if (!g_ui_tag_batch_too_large.exchange(true, std::memory_order_relaxed)) {
      reshade::log::message(
          reshade::log::level::warning,
          "mfgunlock: UI Composition received more than 64 Streamline tags in one call; "
          "the oversized batch is forwarded unchanged.");
    }
    return ForwardWithAutomaticUiRelease(
        state, tags, count, frame_based, std::forward<Forward>(forward));
  }

  for (uint32_t index = 0; index < count; ++index) {
    const auto& tag = tags[index];
    if (tag.structType != sl::ResourceTag::s_structType || tag.structVersion != sl::kStructVersion1 ||
        (tag.resource && (tag.resource->structType != sl::Resource::s_structType ||
                          tag.resource->structVersion != sl::kStructVersion1))) {
      if (state) {
        state->native_ui_owner.store(true, std::memory_order_release);
        ReleaseGlobalAutomaticUiTag(state);
        const auto owned = static_cast<sl::BufferType>(state->automatic_ui_frame_type.load());
        if (frame_based && owned) {
          const sl::ResourceTag clear(nullptr, owned, sl::ResourceLifecycle::eOnlyValidNow);
          if (forward(&clear, 1) == sl::Result::eOk) state->automatic_ui_frame_type.store(0);
        }
      }
      return forward(tags, count);
    }
  }
  for (uint32_t index = 0; index < count; ++index) {
    if (tags[index].resource == nullptr) continue;
    if (tags[index].type == sl::kBufferTypeHUDLessColor) {
      g_ui_hudless_tags_seen.fetch_add(1, std::memory_order_relaxed);
    } else if (tags[index].type == sl::kBufferTypeUIColorAndAlpha) {
      g_ui_color_alpha_tags_seen.fetch_add(1, std::memory_order_relaxed);
    } else if (tags[index].type == sl::kBufferTypeUIAlpha) {
      g_ui_alpha_tags_seen.fetch_add(1, std::memory_order_relaxed);
    }
  }

  if (g_observe_streamline_ui_tags != nullptr)
    g_observe_streamline_ui_tags(tags, count);

  const auto output_state = outputstate::Read();
  qualityguard::OutputDescription expected{};
  if (state != nullptr) {
    AcquireSRWLockShared(&state->lock);
    expected = ExpectedUiOutput(state);
    ReleaseSRWLockShared(&state->lock);
  }

  bool native_ui_in_batch = false;
  for (uint32_t index = 0; index < count; ++index)
    native_ui_in_batch |= qualityguard::IsUiColorOrAlphaType(tags[index].type);
  if (state != nullptr) {
    AcquireSRWLockExclusive(&state->lock);
    if (frame_based && (!state->native_tag_frame_based || state->native_tag_frame != source_frame)) {
      state->native_ui_owner.store(false, std::memory_order_relaxed);
      state->automatic_ui_frame_type.store(0, std::memory_order_relaxed);
      state->hudless_color_seen.store(false, std::memory_order_relaxed);
      state->ui_color_or_alpha_seen.store(false, std::memory_order_relaxed);
      state->ui_color_alpha_seen.store(false, std::memory_order_relaxed);
      state->ui_alpha_seen.store(false, std::memory_order_relaxed);
      state->vulkan_metadata_unknown = 0;
      state->ui_recomposition_invalid.store(false, std::memory_order_relaxed);
      state->ui_recomposition_eligible.store(false, std::memory_order_relaxed);
    }
    state->native_tag_frame_based = frame_based;
    state->native_tag_frame = frame_based ? source_frame : UINT64_MAX;
    if (native_ui_in_batch) {
      g_ui_candidate_active.store(false, std::memory_order_relaxed);
      const auto helper_type = frame_based
          ? state->automatic_ui_frame_type.load(std::memory_order_relaxed)
          : (state->automatic_ui_tag_global.load(std::memory_order_relaxed)
              ? state->automatic_ui_tag_type.load(std::memory_order_relaxed) : 0);
      if (helper_type == sl::kBufferTypeUIAlpha &&
          !BatchContainsTagType(tags, count, sl::kBufferTypeUIAlpha))
        state->ui_alpha_seen.store(false, std::memory_order_relaxed);
      if (helper_type == sl::kBufferTypeUIColorAndAlpha &&
          !BatchContainsTagType(tags, count, sl::kBufferTypeUIColorAndAlpha))
        state->ui_color_alpha_seen.store(false, std::memory_order_relaxed);
      state->native_ui_owner.store(true, std::memory_order_release);
      if (state->automatic_ui_tag_global.load(std::memory_order_acquire))
        state->automatic_ui_clear_pending.store(true, std::memory_order_release);
    }
    ReleaseSRWLockExclusive(&state->lock);
  }
  const auto assessment =
      qualityguard::AssessTags(tags, count, output_state, expected);
  bool eligible = false;
  auto suppression = assessment.suppress_hud_separation
                         ? qualityguard::SuppressionPolicy::kAllHudSeparation
                         : qualityguard::SuppressionPolicy::kNone;

  if (state != nullptr) {
    AcquireSRWLockExclusive(&state->lock);
    bool output_changed = false;
    if (assessment.observed_backbuffer.HasDimensions()) {
      const uint32_t old_width = state->backbuffer_width.load(std::memory_order_relaxed);
      const uint32_t old_height = state->backbuffer_height.load(std::memory_order_relaxed);
      output_changed = (old_width != 0 && old_height != 0) &&
                       (old_width != assessment.observed_backbuffer.width ||
                        old_height != assessment.observed_backbuffer.height);
      state->backbuffer_width.store(assessment.observed_backbuffer.width,
                                    std::memory_order_relaxed);
      state->backbuffer_height.store(assessment.observed_backbuffer.height,
                                     std::memory_order_relaxed);
    }
    if (assessment.observed_backbuffer.HasFormat()) {
      const uint32_t old_format = state->backbuffer_format.load(std::memory_order_relaxed);
      output_changed = output_changed ||
                       (old_format != 0 && old_format != assessment.observed_backbuffer.format);
      state->backbuffer_format.store(assessment.observed_backbuffer.format,
                                     std::memory_order_relaxed);
    }

    if (output_changed) {
      state->native_ui_owner.store(native_ui_in_batch, std::memory_order_relaxed);
      state->hudless_color_seen.store(false, std::memory_order_relaxed);
      state->ui_color_or_alpha_seen.store(false, std::memory_order_relaxed);
      state->ui_color_alpha_seen.store(false, std::memory_order_relaxed);
      state->ui_alpha_seen.store(false, std::memory_order_relaxed);
      state->vulkan_metadata_unknown = 0;
      state->ui_recomposition_invalid.store(false, std::memory_order_relaxed);
      state->ui_recomposition_eligible.store(false, std::memory_order_relaxed);
      RequestUiReset(state);
    }

    if (assessment.has_hud_separation) {
      const bool was_eligible =
          state->ui_recomposition_eligible.load(std::memory_order_relaxed);
      if (assessment.clears_hudless_color)
        state->hudless_color_seen.store(false, std::memory_order_release);
      if (assessment.clears_ui_color_alpha)
        state->ui_color_alpha_seen.store(false, std::memory_order_release);
      if (assessment.clears_ui_alpha)
        state->ui_alpha_seen.store(false, std::memory_order_release);
      if (assessment.clears_ui_color_or_alpha) {
        const bool ui_seen =
            state->ui_color_alpha_seen.load(std::memory_order_acquire) ||
            state->ui_alpha_seen.load(std::memory_order_acquire);
        state->ui_color_or_alpha_seen.store(ui_seen, std::memory_order_release);
      }
      if (assessment.clears_hudless_color || assessment.clears_ui_color_or_alpha)
        state->ui_recomposition_invalid.store(false, std::memory_order_release);
      if (assessment.has_hudless_color)
        state->hudless_color_seen.store(true, std::memory_order_release);
      if (assessment.has_ui_color_alpha)
        state->ui_color_alpha_seen.store(true, std::memory_order_release);
      if (assessment.has_ui_alpha)
        state->ui_alpha_seen.store(true, std::memory_order_release);
      if (assessment.has_ui_color_or_alpha)
        state->ui_color_or_alpha_seen.store(true, std::memory_order_release);

      const bool complete_batch =
          assessment.has_hudless_color && assessment.has_ui_color_or_alpha;
      if (complete_batch && !qualityguard::HasStructuralIssues(assessment)) {
        state->ui_recomposition_invalid.store(false, std::memory_order_release);
      } else if (qualityguard::HasStructuralIssues(assessment)) {
        state->ui_recomposition_invalid.store(true, std::memory_order_release);
      }

      state->vulkan_metadata_unknown =
          (state->vulkan_metadata_unknown & ~assessment.vulkan_metadata_touched) |
          assessment.vulkan_metadata_unknown;
      auto accumulated = assessment;
      if (state->vulkan_metadata_unknown)
        accumulated.issues |= qualityguard::kVulkanResourceMetadataUnknown;
      accumulated.has_hudless_color =
          state->hudless_color_seen.load(std::memory_order_acquire);
      accumulated.has_ui_color_or_alpha =
          state->ui_color_or_alpha_seen.load(std::memory_order_acquire);
      accumulated.has_ui_color_alpha =
          state->ui_color_alpha_seen.load(std::memory_order_acquire);
      accumulated.has_ui_alpha =
          state->ui_alpha_seen.load(std::memory_order_acquire);
      accumulated.ui_encoding = qualityguard::NativeUiEncoding(accumulated);
      if (state->ui_recomposition_invalid.load(std::memory_order_acquire))
        accumulated.issues |= qualityguard::kInvalidOptionalResource;

      eligible = qualityguard::CanAutomaticallyUseUiRecomposition(
          accumulated, output_state);
      suppression = qualityguard::ResolveSuppression(
          assessment, accumulated, output_state, was_eligible);
      const bool suppress = suppression != qualityguard::SuppressionPolicy::kNone;
      const bool native_hudless_fallback =
          accumulated.has_hudless_color && !accumulated.has_ui_color_or_alpha &&
          !qualityguard::HasStructuralIssues(accumulated);
      g_ui_native_hudless_fallback.store(native_hudless_fallback,
                                         std::memory_order_relaxed);

      const bool was_seen =
          state->hud_separation_seen.exchange(true, std::memory_order_acq_rel);
      const bool was_suppressed = state->hud_separation_suppressed.exchange(
          suppress, std::memory_order_acq_rel);
      const bool previous_eligible = state->ui_recomposition_eligible.exchange(
          eligible, std::memory_order_acq_rel);
      if ((!was_seen && suppress) ||
          (was_seen && (was_suppressed != suppress || previous_eligible != eligible))) {
        RequestUiReset(state);
      }
    }
    ReleaseSRWLockExclusive(&state->lock);
  } else if (assessment.has_hud_separation) {
    suppression = qualityguard::SuppressionPolicy::kAllHudSeparation;
    g_ui_native_hudless_fallback.store(false, std::memory_order_relaxed);
  }

  if (assessment.has_hud_separation)
    g_ui_pair_eligible.store(eligible, std::memory_order_relaxed);

  const bool native_hudless_only = state != nullptr &&
      assessment.has_hudless_color && !assessment.has_ui_color_or_alpha &&
      !state->native_ui_owner.load(std::memory_order_acquire) &&
      !qualityguard::HasStructuralIssues(assessment) &&
      suppression == qualityguard::SuppressionPolicy::kNone;
  if (native_hudless_only &&
      g_ui_candidate_injection_enabled.load(std::memory_order_relaxed)) {
    g_ui_candidate_active.store(false, std::memory_order_relaxed);
    g_ui_pair_eligible.store(false, std::memory_order_relaxed);
    g_ui_native_hudless_fallback.store(true, std::memory_order_relaxed);
  }
  if (native_hudless_only && g_observe_ui_candidate != nullptr) {
    UiCandidateSnapshot candidate{};
    if (g_observe_ui_candidate(&candidate)) {
      bool paired = false;
      for (uint32_t index = 0; index < count; ++index) {
        if (tags[index].type != sl::kBufferTypeHUDLessColor || !tags[index].resource) continue;
        bool valid = false;
        const auto extent = qualityguard::Describe(tags[index], &valid);
        paired = valid && extent.width == candidate.width && extent.height == candidate.height &&
                 tags[index].extent.left == 0 && tags[index].extent.top == 0;
      }
      // One globally discovered candidate has no proven association with a
      // second active viewport. Native per-viewport tags remain untouched.
      unsigned int active_viewports = 0;
      for (const auto& other : g_ui_viewports)
        if (other.options_seen.load(std::memory_order_acquire) &&
            other.mode.load(std::memory_order_relaxed) != static_cast<uint32_t>(sl::DLSSGMode::eOff))
          ++active_viewports;
      if (!paired || active_viewports > 1) {
        if (state->automatic_ui_tag_global.load(std::memory_order_acquire))
          state->automatic_ui_clear_pending.store(true, std::memory_order_release);
        return ForwardWithAutomaticUiRelease(state, tags, count, frame_based, forward);
      }
      const auto path = CandidateSubmissionPath(candidate);
      if (path == uisubmission::Path::kColorAndAlpha)
        UpdateUiCandidate(path, candidate.format);
      else if (path == uisubmission::Path::kAlphaOnly)
        UpdateUiCandidate(path);
      else
        NotifyUiCandidateUnavailable();
      return TryInjectUiCandidate(
          state, candidate, tags, count, command_buffer, frame_based,
          std::forward<Forward>(forward));
    }
  }

  if (native_hudless_only && state->automatic_ui_tag_global.load(std::memory_order_acquire))
    state->automatic_ui_clear_pending.store(true, std::memory_order_release);
  if (suppression == qualityguard::SuppressionPolicy::kNone) {
    return ForwardWithAutomaticUiRelease(
        state, tags, count, frame_based, std::forward<Forward>(forward));
  }

  static_assert(std::is_trivially_copyable_v<sl::ResourceTag>);
  alignas(sl::ResourceTag)
      std::array<std::byte, sizeof(sl::ResourceTag) * 64> storage{};
  std::memcpy(storage.data(), tags, sizeof(sl::ResourceTag) * count);
  auto* forwarded = reinterpret_cast<sl::ResourceTag*>(storage.data());
  const uint32_t suppressed =
      suppression == qualityguard::SuppressionPolicy::kAllHudSeparation
          ? SuppressHudSeparationResources(forwarded, count)
          : SuppressUiResources(forwarded, count);
  if (suppressed == 0) {
    return ForwardWithAutomaticUiRelease(
        state, tags, count, frame_based, std::forward<Forward>(forward));
  }

  g_ui_optional_tags_suppressed.fetch_add(suppressed, std::memory_order_relaxed);
  const uint32_t previous_issues =
      g_ui_issue_mask.fetch_or(assessment.issues, std::memory_order_relaxed);
  if (!g_hud_inputs_suppressed.exchange(true, std::memory_order_relaxed)) {
    reshade::log::message(
        reshade::log::level::info,
        "mfgunlock: UI Composition guard is active; unsafe or transition-only UI tags are "
        "withheld while a valid native HUD-less input is preserved when no UI partner exists.");
  }
  if ((assessment.issues & ~previous_issues) != 0) {
    std::stringstream message;
    message << "mfgunlock: UI Composition guard observed optional-input issue mask 0x"
            << std::hex << assessment.issues << std::dec
            << "; using the safest available HUD-less/final-color fallback for this submission.";
    reshade::log::message(reshade::log::level::info, message.str().c_str());
  }
  return ForwardWithAutomaticUiRelease(
      state, forwarded, count, frame_based, std::forward<Forward>(forward));
}

inline bool ContractResourceRole(sl::BufferType type, integration::ResourceRole& role) {
  if (type == sl::kBufferTypeBackbuffer) role = integration::ResourceRole::kBackbuffer;
  else if (type == sl::kBufferTypeDepth) role = integration::ResourceRole::kDepth;
  else if (type == sl::kBufferTypeMotionVectors) role = integration::ResourceRole::kMotionVectors;
  else if (type == sl::kBufferTypeScalingInputColor) role = integration::ResourceRole::kColorInput;
  else if (type == sl::kBufferTypeScalingOutputColor) role = integration::ResourceRole::kColorOutput;
  else if (type == sl::kBufferTypeExposure) role = integration::ResourceRole::kExposure;
  else if (type == sl::kBufferTypeHUDLessColor) role = integration::ResourceRole::kHudLess;
  else if (type == sl::kBufferTypeUIColorAndAlpha) role = integration::ResourceRole::kUiColorAndAlpha;
  else if (type == sl::kBufferTypeUIAlpha) role = integration::ResourceRole::kUiAlpha;
  else if (type == sl::kBufferTypeBidirectionalDistortionField)
    role = integration::ResourceRole::kBidirectionalDistortionField;
  else return false;
  return true;
}

inline void ObserveContractTags(const sl::ViewportHandle& viewport,
                                const sl::ResourceTag* tags, uint32_t count,
                                bool frame_known = false, uint64_t frame = 0) {
  if (tags == nullptr || count > 65) {
    integration::InvalidateFrameEvidence();
    return;
  }
  for (uint32_t index = 0; index < count; ++index) {
    if (tags[index].structType != sl::ResourceTag::s_structType ||
        tags[index].structVersion != sl::kStructVersion1) {
      integration::InvalidateFrameEvidence();
      return;
    }
    integration::ResourceRole role{};
    if (!ContractResourceRole(tags[index].type, role)) continue;
    const sl::Resource* resource = tags[index].resource;
    if (resource != nullptr &&
        (resource->structType != sl::Resource::s_structType ||
         resource->structVersion != sl::kStructVersion1)) {
      integration::InvalidateFrameEvidence();
      return;
    }
    const bool present = resource != nullptr && resource->native != nullptr;
    const bool extent_known = tags[index].extent.width != 0 && tags[index].extent.height != 0;
    integration::ObserveTag(
        static_cast<uint32_t>(viewport), role, present, true,
        static_cast<uint32_t>(tags[index].lifecycle), extent_known,
        tags[index].extent.left, tags[index].extent.top,
        tags[index].extent.width, tags[index].extent.height,
        present && resource->state != UINT32_MAX,
        present ? resource->state : 0,
        present && resource->nativeFormat != 0,
        present ? resource->nativeFormat : 0,
        present ? reinterpret_cast<uint64_t>(resource->native) : 0,
        frame_known, frame);
  }
}

inline sl::Result HookedPclSetMarker(sl::PCLMarker marker, const sl::FrameToken& frame) {
  if (g_real_pcl_set_marker == nullptr) return sl::Result::eErrorNotInitialized;
  const auto result = g_real_pcl_set_marker(marker, frame);
  if (result == sl::Result::eOk) {
    if (marker == sl::PCLMarker::ePresentStart)
      integration::ObservePresentMarker(true, static_cast<uint32_t>(frame));
    else if (marker == sl::PCLMarker::ePresentEnd)
      integration::ObservePresentMarker(false, static_cast<uint32_t>(frame));
  }
  return result;
}

inline sl::Result HookedSetTag(const sl::ViewportHandle& viewport,
                               const sl::ResourceTag* tags, uint32_t count,
                               sl::CommandBuffer* command_buffer) {
  presentation::passive::CpuScope timing(presentation::passive::CpuKind::kTag);
  if (g_real_set_tag == nullptr) return sl::Result::eErrorNotInitialized;
  return FilterUiTags(
      viewport, tags, count, command_buffer, false,
      [&](const sl::ResourceTag* forwarded, uint32_t forwarded_count,
          sl::CommandBuffer* copy_commands = nullptr) {
        const auto result = presentation::passive::CallNative([&] {
          return g_real_set_tag(viewport, forwarded, forwarded_count,
                                copy_commands ? copy_commands : command_buffer);
        });
        if (result == sl::Result::eOk)
          ObserveContractTags(viewport, forwarded, forwarded_count);
        else {
          integration::InvalidateFrameEvidence();
          ForgetUiOutputs();
          g_ui_candidate_options_synced.store(false, std::memory_order_release);
        }
        return result;
      });
}

inline sl::Result HookedSetTagForFrame(const sl::FrameToken& frame,
                                       const sl::ViewportHandle& viewport,
                                       const sl::ResourceTag* tags, uint32_t count,
                                       sl::CommandBuffer* command_buffer) {
  presentation::passive::CpuScope timing(presentation::passive::CpuKind::kTag);
  if (g_real_set_tag_for_frame == nullptr) return sl::Result::eErrorNotInitialized;
  return FilterUiTags(
      viewport, tags, count, command_buffer, true,
      [&](const sl::ResourceTag* forwarded, uint32_t forwarded_count,
          sl::CommandBuffer* copy_commands = nullptr) {
        const auto result = presentation::passive::CallNative([&] {
          return g_real_set_tag_for_frame(frame, viewport, forwarded, forwarded_count,
                                         copy_commands ? copy_commands : command_buffer);
        });
        if (result == sl::Result::eOk)
          ObserveContractTags(viewport, forwarded, forwarded_count, true,
                              static_cast<uint64_t>(frame));
        else {
          integration::InvalidateFrameEvidence();
          ForgetUiOutputs();
          g_ui_candidate_options_synced.store(false, std::memory_order_release);
        }
        return result;
      }, static_cast<uint64_t>(frame));
}

template <typename ConstantsT>
inline void ObserveStreamlineHistory(const ConstantsT& values) {
  const bool reset_known =
      values.reset == sl::Boolean::eTrue || values.reset == sl::Boolean::eFalse;
  bool rendering_known = false;
  bool rendering = false;

  // The guide and SDK revisions do not always expose the same optional fields.
  // Only inspect a member declared by these headers, and require at least the
  // header's default layout version rather than inventing an ABI offset/version.
  if constexpr (requires(const ConstantsT& constants) { constants.renderingGameFrames; }) {
    const ConstantsT declared_layout{};
    if (values.structVersion >= declared_layout.structVersion) {
      rendering_known = values.renderingGameFrames == sl::Boolean::eTrue ||
          values.renderingGameFrames == sl::Boolean::eFalse;
      rendering = values.renderingGameFrames == sl::Boolean::eTrue;
    }
  }

  integration::ObserveHistory(
      reset_known, values.reset == sl::Boolean::eTrue, rendering_known, rendering);
}

inline sl::Result ForwardObservedConstants(const sl::Constants& values,
                                           const sl::FrameToken& frame,
                                           const sl::ViewportHandle& viewport) {
  const auto result = g_real_set_constants(values, frame, viewport);
  if (result == sl::Result::eOk && values.structType == sl::Constants::s_structType &&
      (values.structVersion == sl::kStructVersion1 ||
       values.structVersion == sl::kStructVersion2)) {
    integration::ObserveConstants(static_cast<uint32_t>(frame), static_cast<uint32_t>(viewport));
    ObserveStreamlineHistory(values);
  } else {
    integration::InvalidateFrameEvidence();
  }
  return result;
}

inline sl::Result HookedSetConstants(const sl::Constants& values,
                                     const sl::FrameToken& frame,
                                     const sl::ViewportHandle& viewport) {
  if (g_real_set_constants == nullptr) return sl::Result::eErrorNotInitialized;
  if (values.structType != sl::Constants::s_structType ||
      (values.structVersion != sl::kStructVersion1 && values.structVersion != sl::kStructVersion2)) {
    integration::InvalidateFrameEvidence();
    return g_real_set_constants(values, frame, viewport);
  }
  ApplyLiveOptions(viewport);

  // slSetConstants is shared with DLSS SR and Ray Reconstruction. In
  // particular, receiving a fresh HUDless/UI pair is NOT a camera cut.
  // Keep all caller fields and the original reference, even for UI toggles,
  // format changes or pending helper invalidations. The renderer remains the
  // sole owner of the native reset flag (including true/invalid values).
  g_ui_constants_calls.fetch_add(1, std::memory_order_relaxed);
  if (values.reset == sl::Boolean::eTrue)
    g_ui_native_resets_seen.fetch_add(1, std::memory_order_relaxed);

  UiViewportState* state = g_enabled.load(std::memory_order_relaxed) &&
      g_ui_composition_enabled.load(std::memory_order_relaxed)
      ? GetUiState(viewport) : nullptr;
  const uint64_t requested = state
      ? state->reset_requested.load(std::memory_order_acquire) : 0;
  const sl::Result result = ForwardObservedConstants(values, frame, viewport);
  if (result == sl::Result::eOk && state != nullptr) {
    // Retire local invalidations for diagnostics only. No synthetic reset is
    // submitted to SL. A failed native call leaves the request unacknowledged.
    uint64_t retired = state->reset_applied.load(std::memory_order_acquire);
    while (retired < requested) {
      if (state->reset_applied.compare_exchange_weak(
              retired, requested, std::memory_order_acq_rel,
              std::memory_order_acquire)) {
        g_ui_reset_requests_retired.fetch_add(requested - retired,
                                               std::memory_order_relaxed);
        break;
      }
    }
  }
  return result;
}

inline sl::Result HookedGetState(const sl::ViewportHandle& viewport, sl::DLSSGState& state,
                                 const sl::DLSSGOptions* options) {
  presentation::passive::CpuScope timing(presentation::passive::CpuKind::kGetState);
  bool passive_valid = false;
  struct PassiveStateTap {
    const sl::DLSSGState& state; const sl::ViewportHandle& viewport; const bool& valid;
    uint64_t epoch = presentation::passive::g_generation.load(std::memory_order_acquire);
    ~PassiveStateTap() {
      if (presentation::passive::g_enabled.load(std::memory_order_relaxed))
        presentation::passive::NativeReport(presentation::passive::NowNs(),
            static_cast<uint32_t>(viewport), valid ? state.numFramesActuallyPresented : 0, valid, epoch);
    }
  } passive_tap{state, viewport, passive_valid};
  if (RealGetState() == nullptr) return sl::Result::eErrorNotInitialized;
  if (!g_enabled.load())
    return presentation::passive::CallNative([&] { return RealGetState()(viewport, state, options); });

  const uint32_t diag_call =
      g_turing_diag_get_state_calls.fetch_add(1, std::memory_order_relaxed) + 1;
  const bool diag_trace = diagnostic::g_verbose.load(std::memory_order_relaxed) &&
                          diag_call <= kTuringDiagCallTraceLimit;
  if (diag_trace) {
    std::stringstream message;
    message << "mfgunlock: TuringDiagV2 GetState#" << diag_call
            << " BEGIN thread=" << GetCurrentThreadId()
            << " viewport=" << static_cast<uint32_t>(viewport)
            << " requested-abi=" << state.structVersion;
    TuringDiagLog(message.str());
  }
  const uint64_t native_epoch = g_options_cache_epoch.load(std::memory_order_acquire);
  const sl::Result result = presentation::passive::CallNative([&] { return RealGetState()(viewport, state, options); });
  const bool maximum_known = result == sl::Result::eOk &&
      state.structType == sl::DLSSGState::s_structType &&
      state.structVersion >= sl::kStructVersion2 && state.structVersion <= sl::kStructVersion4 &&
      native_epoch == g_options_cache_epoch.load(std::memory_order_acquire);
  g_native_maximum.store(maximum_known
      ? runtimecontrols::EncodeMaximum(state.numFramesToGenerateMax)
      : runtimecontrols::kUnknownMaximum, std::memory_order_release);
  if (outputstate::GetResourceApi() == outputstate::ResourceApi::kVulkan) {
    g_vulkan_native_state_epoch.store(0, std::memory_order_release);
    if (result == sl::Result::eOk && state.structType == sl::DLSSGState::s_structType &&
        state.structVersion >= sl::kStructVersion2 && state.structVersion <= sl::kStructVersion4 &&
        static_cast<uint32_t>(state.status) == 0 &&
        native_epoch == g_options_cache_epoch.load(std::memory_order_acquire)) {
      g_vulkan_native_state_viewport.store(static_cast<uint32_t>(viewport), std::memory_order_relaxed);
      g_vulkan_native_state_epoch.store(native_epoch, std::memory_order_release);
    }
  }
  if (result == sl::Result::eOk &&
      state.structType == sl::DLSSGState::s_structType &&
      state.structVersion >= sl::kStructVersion1 &&
      state.structVersion <= sl::kStructVersion4) {
    const bool extended_contract = state.structVersion >= sl::kStructVersion4;
    integration::ObserveState(
        static_cast<uint32_t>(viewport), static_cast<uint32_t>(state.status),
        extended_contract && (state.bIsVsyncSupportAvailable == sl::Boolean::eTrue ||
                              state.bIsVsyncSupportAvailable == sl::Boolean::eFalse),
        extended_contract && state.bIsVsyncSupportAvailable == sl::Boolean::eTrue,
        extended_contract, extended_contract
            ? reinterpret_cast<uint64_t>(state.inputsProcessingCompletionFence) : 0,
        extended_contract ? state.lastPresentInputsProcessingCompletionFenceValue : 0);
  }
  if (diag_trace) {
    std::stringstream message;
    message << "mfgunlock: TuringDiagV2 GetState#" << diag_call
            << " NATIVE-END result=0x" << std::hex << static_cast<uint32_t>(result)
            << std::dec << " abi=" << state.structVersion;
    if (result == sl::Result::eOk &&
        state.structType == sl::DLSSGState::s_structType &&
        state.structVersion >= sl::kStructVersion1 &&
        state.structVersion <= sl::kStructVersion4) {
      message << " status=0x" << std::hex << static_cast<uint32_t>(state.status) << std::dec
              << " presented=" << state.numFramesActuallyPresented;
      if (state.structVersion >= sl::kStructVersion2)
        message << " maxGenerated=" << state.numFramesToGenerateMax;
      if (state.structVersion >= sl::kStructVersion4)
        message << " dynamicSupported="
                << (state.bIsDynamicMFGSupported == sl::Boolean::eTrue ? "yes" : "no");
    }
    TuringDiagLog(message.str());
  }
  struct StateObservation {
    const sl::DLSSGState& state;
    countobserver::Event event;
    explicit StateObservation(const sl::DLSSGState& value) : state(value) {}
    ~StateObservation() {
      event.operation = countobserver::Operation::kAdvertise;
      if (event.value_known) event.value = state.numFramesToGenerateMax;
      countobserver::Emit(event);
    }
  };
  std::optional<StateObservation> observation;
  if (countobserver::g_callback.load(std::memory_order_relaxed)) {
    observation.emplace(state);
    auto& event = observation->event;
    event.operation = countobserver::Operation::kRead;
    event.key = countobserver::Key::kMaximum;
    event.origin = countobserver::Origin::kCapabilityPolicy;
    event.viewport = static_cast<uint32_t>(viewport);
    event.caller = countobserver::CallerAddress();
    const void* target = g_get_state_entry.target.load(std::memory_order_acquire);
    event.callee = reinterpret_cast<uint64_t>(target ? target : reinterpret_cast<const void*>(RealGetState()));
    event.result = static_cast<uint32_t>(result);
    event.value_known = result == sl::Result::eOk &&
        state.structType == sl::DLSSGState::s_structType &&
        state.structVersion >= sl::kStructVersion2 && state.structVersion <= sl::kStructVersion4;
    if (event.value_known) event.value = state.numFramesToGenerateMax;
    countobserver::Emit(event);
  }
  if (state.structType != sl::DLSSGState::s_structType ||
      state.structVersion < sl::kStructVersion1 || state.structVersion > sl::kStructVersion4) {
    presentation::ClearCorrelation();
    return result;
  }
  ObserveRuntimeHealth(viewport, result == sl::Result::eOk && static_cast<uint32_t>(state.status) == 0);
  if (result == sl::Result::eOk) {
    if (ObserveDynamicSupport(state, result)) {
      RememberNativeDynamicState(viewport);
    } else if (state.structVersion < sl::kStructVersion4) {
      ProbeDynamicSupport(viewport, options);
    }
    DrainProbePresentations(viewport, state);

    if (static_cast<uint32_t>(state.status) == 0) {
      passive_valid = true;
      presentation::ObserveProviderPresentation(
          state.numFramesActuallyPresented, static_cast<uint32_t>(viewport));
    } else {
      presentation::ClearCorrelation();
    }

    const unsigned int status = static_cast<unsigned int>(state.status);
    g_dlssg_status.store(status, std::memory_order_relaxed);
    g_state_seen.store(true, std::memory_order_relaxed);
    if (status != 0 && !g_failure_status_logged.exchange(true, std::memory_order_relaxed)) {
      std::stringstream message;
      message << "mfgunlock: DLSS-G runtime status 0x" << std::hex << status;
      reshade::log::message(reshade::log::level::warning, message.str().c_str());
    }
  }
  if (result != sl::Result::eOk) {
    presentation::ClearCorrelation();
    return result;
  }
  if (state.structVersion < sl::kStructVersion2) return result;

  const unsigned int reported = state.numFramesToGenerateMax;
  g_runtime_max_generated.store(reported, std::memory_order_relaxed);
  const unsigned int wanted = ProviderMfgCeiling();
  // A configured menu request is independent of scanner/gate observations on
  // both APIs. Keep native and advertised values separately; only an actual
  // native options/create/state response confirms use.

  if (wanted == 0 || reported >= wanted) {
    g_effective_max_generated.store(state.numFramesToGenerateMax,
                                    std::memory_order_relaxed);
    g_effective_max_seen.store(true, std::memory_order_release);
    return result;
  }

  state.numFramesToGenerateMax = wanted;
  g_effective_max_generated.store(state.numFramesToGenerateMax,
                                  std::memory_order_relaxed);
  g_effective_max_seen.store(true, std::memory_order_release);
  if (!g_capacity_advertised.exchange(true, std::memory_order_relaxed)) {
    std::stringstream s;
    s << "mfgunlock: slDLSSGGetState reported a maximum of " << reported
      << " generated frame(s); advertising the configured trial maximum of " << wanted
      << " (" << (uint64_t{wanted} + 1) << "x); native acceptance is not yet proven.";
    reshade::log::message(reshade::log::level::info, s.str().c_str());
  }
  return result;
}

inline sl::Result HookedGetFeatureFunction(sl::Feature feature, const char* function_name,
                                                void*& function) {
  const sl::Result result = g_real_get_feature_function(feature, function_name, function);
  if (result != sl::Result::eOk || function_name == nullptr || function == nullptr) return result;
  if (feature != sl::kFeatureDLSS_G || g_entry_shutting_down.load(std::memory_order_acquire)) return result;

  if ((std::strcmp(function_name, "slDLSSGSetOptions") == 0 ||
       std::strcmp(function_name, "slDLSSGGetState") == 0) && g_on_dlssg_entry_observed)
    g_on_dlssg_entry_observed(function);

  if (std::strcmp(function_name, "slDLSSGSetOptions") == 0) {
    if (!g_set_options_entry.installed.load(std::memory_order_acquire) ||
        g_set_options_entry.target.load(std::memory_order_acquire) != function)
      InvalidateLiveOptions();
    if (hook::InstallAddress(g_set_options_entry, function,
                             reinterpret_cast<void*>(&HookedSetOptions),
                             "slDLSSGSetOptions")) {
      g_real_set_options = g_set_options_entry.Original<SetOptionsFn>();
      reshade::log::message(
          reshade::log::level::info,
          "mfgunlock: slDLSSGSetOptions hook installed.");
    } else {
      reshade::log::message(
          reshade::log::level::warning,
          "mfgunlock: slDLSSGSetOptions hook failed.");
    }
  } else if (std::strcmp(function_name, "slDLSSGGetState") == 0) {
    if (hook::InstallAddress(g_get_state_entry, function,
                             reinterpret_cast<void*>(&HookedGetState),
                             "slDLSSGGetState")) {
      g_real_get_state = g_get_state_entry.Original<GetStateFn>();
      reshade::log::message(
          reshade::log::level::info,
          "mfgunlock: slDLSSGGetState hook installed.");
    } else {
      reshade::log::message(
          reshade::log::level::warning,
          "mfgunlock: slDLSSGGetState hook failed.");
    }
  }
  return result;
}

inline void TryInstallUiHooks(HMODULE interposer) {
  if (g_ui_hooks_installed.load(std::memory_order_acquire) || interposer == nullptr) return;

  std::vector<hook::HookItem> hooks;
  if (GetProcAddress(interposer, "slSetTag") != nullptr) {
    hooks.push_back({"slSetTag", reinterpret_cast<void**>(&g_real_set_tag),
                     reinterpret_cast<void*>(&HookedSetTag)});
  }
  if (GetProcAddress(interposer, "slSetTagForFrame") != nullptr) {
    hooks.push_back({"slSetTagForFrame", reinterpret_cast<void**>(&g_real_set_tag_for_frame),
                     reinterpret_cast<void*>(&HookedSetTagForFrame)});
  }
  if (GetProcAddress(interposer, "slSetConstants") != nullptr) {
    hooks.push_back({"slSetConstants", reinterpret_cast<void**>(&g_real_set_constants),
                     reinterpret_cast<void*>(&HookedSetConstants)});
  }
  if (GetProcAddress(interposer, "slPCLSetMarker") != nullptr) {
    hooks.push_back({"slPCLSetMarker", reinterpret_cast<void**>(&g_real_pcl_set_marker),
                     reinterpret_cast<void*>(&HookedPclSetMarker)});
  }
  if (hooks.empty()) return;
  if (!hook::Install(interposer, hooks, "Streamline UI Composition guard")) return;

  g_ui_hooks = std::move(hooks);
  g_ui_hooks_installed.store(true, std::memory_order_release);
  reshade::log::message(
      reshade::log::level::info,
      "mfgunlock: Streamline UI Composition guard hooks installed.");
}

inline const std::vector<hook::HookItem> kFeatureFunctionHook = {
    {"slGetFeatureFunction", reinterpret_cast<void**>(&g_real_get_feature_function),
     reinterpret_cast<void*>(&HookedGetFeatureFunction)},
};

// This is the SL 2.x signature. Legacy startup is handled by streamline_bridge.hpp.
inline const std::vector<hook::HookItem> kInitHook = {
    {"slInit", reinterpret_cast<void**>(&g_real_init), reinterpret_cast<void*>(&HookedInit)},
};

}  // namespace internal

inline bool DynamicStackReady() { return internal::DynamicStackReady(); }

struct UiDebugSnapshot {
  uint32_t viewport_count = 0;
  bool options_seen = false;
  bool hudless_seen = false;
  bool ui_color_or_alpha_seen = false;
  bool ui_color_alpha_seen = false;
  bool ui_alpha_seen = false;
  bool pair_eligible = false;
  bool invalid = false;
  bool suppression_active = false;
};

inline UiDebugSnapshot ReadUiDebugSnapshot() {
  UiDebugSnapshot snapshot{};
  for (const auto& state : internal::g_ui_viewports) {
    if (state.key.load(std::memory_order_acquire) == internal::kUnusedViewport) continue;
    ++snapshot.viewport_count;
    snapshot.options_seen |= state.options_seen.load(std::memory_order_acquire);
    snapshot.hudless_seen |= state.hudless_color_seen.load(std::memory_order_acquire);
    snapshot.ui_color_or_alpha_seen |=
        state.ui_color_or_alpha_seen.load(std::memory_order_acquire);
    snapshot.ui_color_alpha_seen |=
        state.ui_color_alpha_seen.load(std::memory_order_acquire);
    snapshot.ui_alpha_seen |= state.ui_alpha_seen.load(std::memory_order_acquire);
    snapshot.pair_eligible |=
        state.ui_recomposition_eligible.load(std::memory_order_acquire);
    snapshot.invalid |= state.ui_recomposition_invalid.load(std::memory_order_acquire);
    snapshot.suppression_active |=
        state.hud_separation_suppressed.load(std::memory_order_acquire);
  }
  return snapshot;
}

inline void RequestGlobalAutomaticUiTagRelease() {
  internal::RequestGlobalAutomaticUiTagRelease();
}

inline void ReleaseGlobalAutomaticUiTags() {
  internal::ReleaseGlobalAutomaticUiTags();
}

inline void NotifyOutputStateChanged() {
  g_request_retry_revision.fetch_add(1, std::memory_order_acq_rel);
  internal::ForgetUiOutputs();
  internal::RequestAllUiResets();
  g_ui_composition_applied.store(false, std::memory_order_relaxed);
  g_ui_candidate_options_synced.store(false, std::memory_order_release);
  NotifyLiveOptionsChanged();
}

inline void NotifyOutputUnknown() {
  g_request_retry_revision.fetch_add(1, std::memory_order_acq_rel);
  outputstate::Reset();
  g_ui_composition_applied.store(false, std::memory_order_relaxed);
  g_ui_candidate_options_synced.store(false, std::memory_order_release);
  internal::NotifyUiCandidateUnavailable();
  internal::ForgetUiOutputs();
  internal::RequestAllUiResets();
  NotifyLiveOptionsChanged();
}

inline const char* LiveOptionsActionText() {
  const auto revision = g_options_revision.load(std::memory_order_acquire);
  bool seen = false;
  for (auto& state : internal::g_ui_viewports) {
    if (state.key.load(std::memory_order_acquire) == internal::kUnusedViewport) continue;
    seen = true;
    std::unique_lock lock(state.options_lock, std::try_to_lock);
    if (!lock) return "Runtime options update in progress.";
    if (state.attempted_revision == revision) continue;
    if (!state.native_options_valid || state.cache_epoch != g_options_cache_epoch.load())
      return "Runtime options pending. Turn Frame Generation off and on in the game menu to apply.";
    if (state.native_options.mode == sl::DLSSGMode::eOff)
      return "Runtime options saved. Enable Frame Generation to apply.";
    return "Runtime options pending automatic submission. If unchanged, turn Frame Generation off and on in the game menu.";
  }
  return seen ? nullptr : "Runtime options will apply when the game next configures Frame Generation.";
}

inline void ApplyLiveOptionsOnPresent() {
  const uint32_t thread = GetCurrentThreadId();
  const uint64_t revision = g_options_revision.load(std::memory_order_acquire);
  const uint64_t cache_epoch = g_options_cache_epoch.load(std::memory_order_acquire);
  const void* options_entry = reinterpret_cast<const void*>(internal::RealSetOptions());
  g_present_thread.store(thread, std::memory_order_release);
  uint32_t key = internal::kUnusedViewport;
  for (auto& state : internal::g_ui_viewports) {
    const auto candidate = state.key.load(std::memory_order_acquire);
    if (candidate == internal::kUnusedViewport) continue;
    std::unique_lock lock(state.options_lock, std::try_to_lock);
    if (!lock) return;
    if (!state.native_options_valid || state.cache_epoch != cache_epoch ||
        state.options_thread != thread || state.options_entry != options_entry ||
        state.native_options.mode == sl::DLSSGMode::eOff ||
        state.attempted_revision == revision) {
      continue;
    }
    if (key != internal::kUnusedViewport) return;
    key = candidate;
  }
  if (key != internal::kUnusedViewport) internal::ApplyLiveOptions(sl::ViewportHandle(key));
}

inline void NotifyUiCompositionChanged() {
  g_request_retry_revision.fetch_add(1, std::memory_order_acq_rel);
  internal::RequestGlobalAutomaticUiTagRelease();
  // Keep the last accepted options evidence while the new request is pending.
  g_ui_composition_fell_back.store(false, std::memory_order_relaxed);
  g_ui_composition_result.store(0, std::memory_order_relaxed);
  g_hud_inputs_suppressed.store(false, std::memory_order_relaxed);
  g_ui_native_hudless_fallback.store(false, std::memory_order_relaxed);
  g_ui_issue_mask.store(0, std::memory_order_relaxed);
  g_ui_pair_eligible.store(false, std::memory_order_relaxed);
  g_ui_hudless_tags_seen.store(0, std::memory_order_relaxed);
  g_ui_color_alpha_tags_seen.store(0, std::memory_order_relaxed);
  g_ui_alpha_tags_seen.store(0, std::memory_order_relaxed);
  g_ui_optional_tags_suppressed.store(0, std::memory_order_relaxed);
  g_ui_candidate_active.store(false, std::memory_order_relaxed);
  g_ui_candidate_options_synced.store(false, std::memory_order_release);
  internal::ForgetUiOutputs();
  internal::RequestAllUiResets();
  NotifyLiveOptionsChanged();
}

inline void NotifyUiCandidateInjectionChanged() {
  g_request_retry_revision.fetch_add(1, std::memory_order_acq_rel);
  internal::RequestGlobalAutomaticUiTagRelease();
  g_ui_candidate_active.store(false, std::memory_order_relaxed);
  g_ui_candidate_options_synced.store(false, std::memory_order_release);
  g_ui_candidate_runtime_declined.store(false, std::memory_order_release);
  g_ui_candidate_retry_frames.store(0, std::memory_order_release);
  g_ui_candidate_consecutive_failures.store(0, std::memory_order_release);
  g_ui_candidate_last_failed_resource.store(0, std::memory_order_release);
  g_ui_candidate_last_result.store(0, std::memory_order_relaxed);
  g_ui_candidate_injection_failures.store(0, std::memory_order_relaxed);
  g_ui_candidate_recoveries.store(0, std::memory_order_relaxed);
  g_ui_candidate_native_fallbacks.store(0, std::memory_order_relaxed);
  g_ui_candidate_missing_command_buffer.store(0, std::memory_order_relaxed);
  g_ui_candidate_full_batches.store(0, std::memory_order_relaxed);
  g_ui_candidate_tags_injected.store(0, std::memory_order_relaxed);
  internal::RequestAllUiResets();
  NotifyLiveOptionsChanged();
}

inline void NotifyDynamicD3D12(bool d3d12) {
  g_dynamic_d3d12.store(d3d12, std::memory_order_relaxed);
}

inline void NotifyFixedMultiplierChanged() {
  g_request_retry_revision.fetch_add(1, std::memory_order_acq_rel);
  const bool enabled = g_force_multiplier.load(std::memory_order_relaxed) >= 2;
  g_fixed_override_status.store(
      enabled ? FixedOverrideStatus::kPending : FixedOverrideStatus::kNative,
      std::memory_order_relaxed);
  g_declined_no_pacing.store(false, std::memory_order_relaxed);
  g_force_failed_for.store(0, std::memory_order_relaxed);
  g_ceiling_blocked_for.store(0, std::memory_order_relaxed);
  g_unknown_ceiling_logged.store(false, std::memory_order_relaxed);
  g_unknown_fixed_abi_logged.store(false, std::memory_order_relaxed);
  NotifyLiveOptionsChanged();
}

inline void NotifyDynamicModeChanged() {
  g_request_retry_revision.fetch_add(1, std::memory_order_acq_rel);
  g_dynamic_fell_back.store(false, std::memory_order_relaxed);
  g_dynamic_runtime_declined.store(false, std::memory_order_release);
  g_dynamic_result.store(0, std::memory_order_relaxed);
  g_dynamic_probe_attempted.store(false, std::memory_order_relaxed);
  g_dynamic_probe_result.store(0, std::memory_order_relaxed);
  NotifyLiveOptionsChanged();
}

inline void NotifyDlssgPluginUnloaded(HMODULE module) {
  if (!module) return;
  const auto options_owner = internal::g_set_options_entry.identity.module;
  const auto state_owner = internal::g_get_state_entry.identity.module;
  if ((options_owner || state_owner) && options_owner != module && state_owner != module) return;
  internal::InvalidateLiveOptions();
  if (internal::g_set_options_entry.identity.module == module) {
    hook::UninstallAddress(internal::g_set_options_entry);
    internal::g_real_set_options = nullptr;
  }
  if (internal::g_get_state_entry.identity.module == module) {
    hook::UninstallAddress(internal::g_get_state_entry);
    internal::g_real_get_state = nullptr;
  }
  g_streamline_plugin_seen.store(false, std::memory_order_release);
  g_streamline_max_generated.store(0, std::memory_order_release);
  g_capacity_advertised.store(false, std::memory_order_relaxed);
  g_runtime_max_generated.store(0, std::memory_order_relaxed);
  g_native_maximum.store(runtimecontrols::kUnknownMaximum, std::memory_order_release);
  g_effective_max_generated.store(0, std::memory_order_relaxed);
  g_effective_max_seen.store(false, std::memory_order_release);
  g_last_forwarded_mode.store(diagnostic::kUnknown32, std::memory_order_relaxed);
  g_last_forwarded_generated.store(diagnostic::kUnknown32, std::memory_order_relaxed);
  g_last_accepted_mode.store(diagnostic::kUnknown32, std::memory_order_relaxed);
  g_last_accepted_generated.store(diagnostic::kUnknown32, std::memory_order_relaxed);
  presentation::ObserveProviderUnavailable();
  g_fixed_override_status.store(g_force_multiplier.load(std::memory_order_relaxed) >= 2
                                   ? FixedOverrideStatus::kPending : FixedOverrideStatus::kNative,
                               std::memory_order_relaxed);
  g_last_effective_generated.store(0, std::memory_order_relaxed);
  g_force_failed_for.store(0, std::memory_order_relaxed);
  g_dynamic_probe_result.store(0, std::memory_order_relaxed);
  g_dynamic_support_seen.store(false, std::memory_order_release);
  g_dynamic_supported.store(false, std::memory_order_release);
  g_dynamic_probe_attempted.store(false, std::memory_order_relaxed);
  g_dynamic_applied.store(false, std::memory_order_relaxed);
  g_dynamic_fell_back.store(false, std::memory_order_relaxed);
  g_dynamic_runtime_declined.store(false, std::memory_order_release);
  g_dynamic_result.store(0, std::memory_order_relaxed);
}

// Install before the host caches DLSS-G function pointers.
inline void TryInstall() {
  static thread_local bool installing = false;
  if (installing) return;
  installing = true;
  struct Guard {
    ~Guard() { installing = false; }
  } guard;
  HMODULE interposer = g_find_interposer ? g_find_interposer() : GetModuleHandleW(L"sl.interposer.dll");

  // An entry detour disappeared with the old image. Clear every borrowed
  // trampoline before attempting to bind a replacement image. Do not detach
  // from an unmapped module, even if its base address is reused.
  if (g_interposer_hook_identity.module &&
      !hook::IsCurrent(g_interposer_hook_identity)) {
    g_feature_function_hooked.store(false, std::memory_order_release);
    g_init_hooked.store(false, std::memory_order_release);
    internal::g_ui_hooks_installed.store(false, std::memory_order_release);
    internal::g_ui_hooks.clear();
    internal::g_real_get_feature_function = nullptr;
    internal::g_real_init = nullptr;
    internal::g_real_set_tag = nullptr;
    internal::g_real_set_tag_for_frame = nullptr;
    internal::g_real_set_constants = nullptr;
    internal::g_real_pcl_set_marker = nullptr;
    g_interposer_hook_identity = {};
    internal::InvalidateLiveOptions();
  }
  if (interposer == nullptr) return;

  // slInit must be observed before the game calls it. A current, own-code
  // supported typed slInit is enough to install that entry hook; the complete runtime
  // contract can become available later, after sl.common is loaded.
  const bool compatible_init = g_init_compatible && g_init_compatible(interposer);
  const bool compatible_runtime =
      g_interposer_contract_ready && g_interposer_contract_ready(interposer);
  if (!compatible_init && !compatible_runtime) return;
  if (!g_interposer_hook_identity.module &&
      !hook::CaptureModule(interposer, g_interposer_hook_identity)) return;

  if (compatible_runtime) {
    if (!g_feature_function_hooked.load(std::memory_order_acquire) &&
        GetProcAddress(interposer, "slGetFeatureFunction") != nullptr &&
        hook::Install(interposer, internal::kFeatureFunctionHook, "sl.interposer.dll")) {
      g_feature_function_hooked.store(true, std::memory_order_release);
    }
    internal::TryInstallUiHooks(interposer);
  }

  const bool observe_init = g_on_init && compatible_init;
  const bool alter_init =
      g_runtime_selection_mode.load(std::memory_order_relaxed) !=
          static_cast<unsigned int>(RuntimeSelectionMode::kGameDefault) &&
      compatible_init;
  if (!g_init_hooked.load(std::memory_order_acquire) && (observe_init || alter_init) &&
      GetProcAddress(interposer, "slInit") != nullptr &&
      hook::Install(interposer, internal::kInitHook, "sl.interposer.dll")) {
    g_init_hooked.store(true, std::memory_order_release);
  }
}

inline void Uninstall() {
  internal::ReleaseGlobalAutomaticUiTags();
  internal::InvalidateLiveOptions();
  internal::g_entry_shutting_down.store(true, std::memory_order_release);
  const bool interposer_current = hook::IsCurrent(g_interposer_hook_identity);
  if (interposer_current && internal::g_ui_hooks_installed.load(std::memory_order_acquire) &&
      !internal::g_ui_hooks.empty()) {
    hook::Uninstall(internal::g_ui_hooks);
    internal::g_ui_hooks.clear();
    internal::g_ui_hooks_installed.store(false, std::memory_order_release);
  }
  if (interposer_current && g_init_hooked.load(std::memory_order_acquire)) hook::Uninstall(internal::kInitHook);
  if (interposer_current && g_feature_function_hooked.load(std::memory_order_acquire))
    hook::Uninstall(internal::kFeatureFunctionHook);
  g_init_hooked.store(false, std::memory_order_release);
  g_feature_function_hooked.store(false, std::memory_order_release);
  internal::g_ui_hooks.clear();
  internal::g_ui_hooks_installed.store(false, std::memory_order_release);
  g_interposer_hook_identity = {};
  internal::g_real_get_feature_function = nullptr;
  internal::g_real_init = nullptr;
  internal::g_real_pcl_set_marker = nullptr;

  hook::UninstallAddress(internal::g_get_state_entry);
  hook::UninstallAddress(internal::g_set_options_entry);
  internal::g_real_get_state = nullptr;
  internal::g_real_set_options = nullptr;
  internal::g_real_set_tag = nullptr;
  internal::g_real_set_tag_for_frame = nullptr;
  internal::g_real_set_constants = nullptr;
}

}  // namespace mfgunlock::framecount
