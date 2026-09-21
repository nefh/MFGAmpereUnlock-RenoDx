// SPDX-License-Identifier: MIT
#pragma once
#include "./observer_scope.hpp"

#include <array>
#include <atomic>
#include <cstdint>
#include <cstddef>
#include <mutex>
#include <shared_mutex>

namespace mfgunlock::diagnostic {
inline std::atomic_bool g_verbose{false};
inline constexpr uint32_t kEvaluateVersion = 1;
inline constexpr uint32_t kStateVersion1 = 1;
inline constexpr uint32_t kStateVersion2 = 2;
inline constexpr uint32_t kStateVersion = 3;
inline constexpr uint32_t kUnknown32 = UINT32_MAX;
inline constexpr uint64_t kUnknown64 = UINT64_MAX;

enum class Backend : uint32_t { kNgxD3D12 = 0 };
enum class EvaluatePhase : uint32_t { kBegin = 0, kEnd = 1 };
enum class ResourceKey : uint32_t {
  kBackbuffer = 0,
  kDepth,
  kMotionVectors,
  kHudLess,
  kUi,
  kUiAlpha,
  kBidirectionalDistortionField,
  kOutputInterpolated,
  kOutputReal,
  kOutputDisableInterpolation,
  kCount,
};

enum class TemporalBackend : uint32_t { kNone = 0, kMidpoint = 1, kBlackwell = 2 };

enum UiAutomaticRejectReason : uint32_t {
  kUiAutoAccept = 0,
  kUiAutoNoCandidate = 1u << 16,
  kUiAutoHdrOutput = 1u << 17,
  kUiAutoOptionsUnsynced = 1u << 18,
  kUiAutoRuntimeDeclined = 1u << 19,
  kUiAutoDisabled = 1u << 20,
};

struct ResourceSnapshot {
  uint32_t key = 0;
  uint32_t known = 0;
  uint64_t object = 0;
  uint64_t width = 0;
  uint32_t height = 0;
  uint16_t depth_or_array = 0;
  uint16_t mip_levels = 0;
  uint32_t format = 0;
  uint32_t dimension = 0;
  uint32_t flags = 0;
  uint32_t sample_count = 0;
};

// POD observer boundary. It describes one provider Evaluate call. Resource
// pointers are identity tokens plus GetDesc snapshots, never retained objects.
struct EvaluateEvent {
  uint32_t bytes = sizeof(EvaluateEvent);
  uint32_t version = kEvaluateVersion;
  Backend backend = Backend::kNgxD3D12;
  EvaluatePhase phase = EvaluatePhase::kBegin;
  uint32_t result = kUnknown32;
  uint32_t thread = 0;
  uint64_t evaluation_id = 0;
  uint64_t caller = 0;
  uint64_t commands = 0;
  uint64_t handle = 0;
  uint64_t parameters = 0;
  uint32_t generated_count_known = 0;
  uint32_t generated_count = 0;
  uint32_t generated_index_known = 0;
  uint32_t generated_index = 0;
  uint32_t reset_known = 0;
  uint32_t reset = 0;
  uint32_t automode_reset_known = 0;
  uint32_t automode_reset = 0;
  uint32_t backbuffer_frame_id_known = 0;
  uint32_t reserved = 0;
  uint64_t backbuffer_frame_id = 0;
  std::array<ResourceSnapshot, static_cast<size_t>(ResourceKey::kCount)> resources{};
};
using EvaluateCallback = void (*)(const EvaluateEvent*) noexcept;
using RegisterEvaluate = bool (*)(uint32_t, EvaluateCallback);
inline std::atomic<EvaluateCallback> g_evaluate_callback{nullptr};
inline std::shared_mutex g_evaluate_callback_mutex;
inline std::atomic<uint64_t> g_evaluation_sequence{0};

inline bool SetEvaluateCallback(EvaluateCallback callback) noexcept {
  if (observers::g_callback_depth != 0) {
    if (callback != nullptr) return false;
    g_evaluate_callback.store(nullptr, std::memory_order_release);
    return true;
  }
  std::unique_lock lock(g_evaluate_callback_mutex);
  g_evaluate_callback.store(callback, std::memory_order_release);
  return true;
}

inline void Emit(const EvaluateEvent& event) noexcept {
  if (observers::g_callback_depth != 0) return;  // Diagnostic re-entry must not recurse into the shared mutex.
  std::shared_lock lock(g_evaluate_callback_mutex);
  const auto callback = g_evaluate_callback.load(std::memory_order_acquire);
  if (!callback) return;
  ++observers::g_callback_depth;
  callback(&event);
  --observers::g_callback_depth;
}

inline constexpr uint32_t kLifecycleVersion = 1;
enum class LifecycleKind : uint32_t {
  kCreated, kReleased, kZeroHandles, kTrackingLost, kModeOff, kModeOn,
  kQualityRequested, kQualitySuperseded, kQualityFrozen, kQualityRestartRequired,
  kLiveReapply,
};
inline const char* LifecycleName(LifecycleKind kind) {
  switch (kind) {
    case LifecycleKind::kCreated: return "ngx_fg_feature_created";
    case LifecycleKind::kReleased: return "ngx_fg_feature_released";
    case LifecycleKind::kZeroHandles: return "fg_zero_handle_boundary";
    case LifecycleKind::kTrackingLost: return "fg_tracking_lost";
    case LifecycleKind::kModeOff: return "fg_mode_off_observed";
    case LifecycleKind::kModeOn: return "fg_mode_on_observed";
    case LifecycleKind::kQualityRequested: return "quality_change_requested";
    case LifecycleKind::kQualitySuperseded: return "quality_change_superseded";
    case LifecycleKind::kQualityFrozen: return "quality_configuration_prepared";
    case LifecycleKind::kQualityRestartRequired: return "quality_restart_required";
    case LifecycleKind::kLiveReapply: return "live_options_reapplied";
  }
  return "unknown";
}
struct LifecycleEvent {
  uint32_t bytes = sizeof(LifecycleEvent);
  uint32_t version = kLifecycleVersion;
  LifecycleKind kind = LifecycleKind::kCreated;
  uint32_t thread = 0;
  uint64_t epoch = kUnknown64;
  uint64_t handle = 0;
  uint32_t handles = kUnknown32;
  uint32_t evaluates = kUnknown32;
  uint32_t creates = kUnknown32;
  uint32_t releases = kUnknown32;
  uint32_t tracking_uncertain = kUnknown32;
  uint32_t requested_quality = kUnknown32;
  uint32_t prepared_quality = kUnknown32;
  uint32_t viewport = kUnknown32;
  uint32_t result = kUnknown32;
  uint32_t mode = kUnknown32;
};
using LifecycleCallback = void (*)(const LifecycleEvent*) noexcept;
using RegisterLifecycle = bool (*)(uint32_t, LifecycleCallback);
inline std::atomic<LifecycleCallback> g_lifecycle_callback{nullptr};
inline std::shared_mutex g_lifecycle_callback_mutex;

inline bool SetLifecycleCallback(LifecycleCallback callback) noexcept {
  if (observers::g_callback_depth != 0) {
    if (callback != nullptr) return false;
    g_lifecycle_callback.store(nullptr, std::memory_order_release);
    return true;
  }
  std::unique_lock lock(g_lifecycle_callback_mutex);
  g_lifecycle_callback.store(callback, std::memory_order_release);
  return true;
}

inline void Emit(const LifecycleEvent& event) noexcept {
  if (observers::g_callback_depth != 0) return;  // Diagnostic re-entry must not recurse into the shared mutex.
  std::shared_lock lock(g_lifecycle_callback_mutex);
  const auto callback = g_lifecycle_callback.load(std::memory_order_acquire);
  if (!callback) return;
  ++observers::g_callback_depth;
  callback(&event);
  --observers::g_callback_depth;
}

// Version 1 is kept byte-for-byte for already deployed diagnostics add-ons.
struct DiagnosticStateV1 {
  uint32_t bytes = sizeof(DiagnosticStateV1);
  uint32_t version = kStateVersion1;
  uint32_t architecture = kUnknown32;
  uint32_t target_sm = 0;
  TemporalBackend temporal_backend = TemporalBackend::kNone;
  uint32_t temporal_target_sm = 0;
  uint32_t boundary_mode = kUnknown32;
  uint32_t warp_applied = 0;
  uint32_t warp_target_sm = 0;
  uint32_t dynamic_requested = 0;
  uint32_t dynamic_applied = 0;
  uint32_t fixed_multiplier = 0;
  uint32_t effective_generated = 0;
  int32_t preset_requested = -1;
  int32_t preset_supplied = -1;
  int32_t preset_applied = -1;
  uint32_t ui_composition_requested = 0;
  uint32_t ui_composition_applied = 0;
  uint32_t ui_composition_fallback = 0;
  uint32_t prepared_provider_count = 0;
};

// Read-only point-in-time runtime state for diagnostic capture manifests.
// Known flags are explicit so a missing observation never becomes false/zero.
struct DiagnosticStateV2 {
  uint32_t bytes = sizeof(DiagnosticStateV2);
  uint32_t version = kStateVersion2;
  uint32_t architecture = kUnknown32;
  uint32_t target_sm = 0;
  TemporalBackend temporal_backend = TemporalBackend::kNone;
  uint32_t temporal_target_sm = 0;
  uint32_t boundary_mode = kUnknown32;
  uint32_t warp_applied = 0;
  uint32_t warp_target_sm = 0;
  uint32_t dynamic_requested = 0;
  uint32_t dynamic_applied = 0;
  uint32_t fixed_multiplier = 0;
  uint32_t effective_generated = 0;
  int32_t preset_requested = -1;
  int32_t preset_supplied = -1;
  int32_t preset_applied = -1;
  uint32_t ui_composition_requested = 0;
  uint32_t ui_composition_applied = 0;
  uint32_t ui_composition_fallback = 0;
  uint32_t prepared_provider_count = 0;

  uint32_t requested_generated_known = 0;
  uint32_t requested_generated = 0;
  uint32_t forwarded_generated_known = 0;
  uint32_t forwarded_generated = 0;
  uint32_t accepted_generated_known = 0;
  uint32_t accepted_generated = 0;
  uint32_t provider_applied_generated_known = 0;
  uint32_t provider_applied_generated = 0;
  uint32_t structural_max_known = 0;
  uint32_t structural_max_generated = 0;
  uint32_t runtime_max_known = 0;
  uint32_t runtime_max_generated = 0;
  uint32_t effective_max_known = 0;
  uint32_t effective_max_generated = 0;
  uint32_t game_ui_max_known = 0;
  uint32_t game_ui_max_generated = 0;

  uint32_t requested_mode_known = 0;
  uint32_t requested_mode = kUnknown32;
  uint32_t forwarded_mode_known = 0;
  uint32_t forwarded_mode = kUnknown32;
  uint32_t accepted_mode_known = 0;
  uint32_t accepted_mode = kUnknown32;
  uint32_t observed_mode_known = 0;
  uint32_t observed_mode = kUnknown32;
  uint32_t dynamic_support_seen = 0;
  uint32_t dynamic_supported = 0;

  uint32_t hdr_state_seen = 0;
  uint32_t hdr_active = 0;
  uint32_t swapchain_color_space_known = 0;
  uint32_t swapchain_color_space = kUnknown32;
  uint32_t dlssg_color_space_known = 0;
  uint32_t dlssg_color_space = kUnknown32;

  uint32_t native_uir_observed = 0;
  uint32_t automatic_uir_eligible = 0;
  uint32_t automatic_uir_applied = 0;
  uint32_t automatic_uir_rejected = 0;
  uint32_t automatic_uir_reject_reasons = 0;
};

struct DiagnosticState {
  uint32_t bytes = sizeof(DiagnosticState);
  uint32_t version = kStateVersion;
  uint32_t architecture = kUnknown32;
  uint32_t target_sm = 0;
  TemporalBackend temporal_backend = TemporalBackend::kNone;
  uint32_t temporal_target_sm = 0;
  uint32_t boundary_mode = kUnknown32;
  uint32_t warp_applied = 0;
  uint32_t warp_target_sm = 0;
  uint32_t dynamic_requested = 0;
  uint32_t dynamic_applied = 0;
  uint32_t fixed_multiplier = 0;
  uint32_t effective_generated = 0;
  int32_t preset_requested = -1;
  int32_t preset_supplied = -1;
  int32_t preset_applied = -1;
  uint32_t ui_composition_requested = 0;
  uint32_t ui_composition_applied = 0;
  uint32_t ui_composition_fallback = 0;
  uint32_t prepared_provider_count = 0;

  uint32_t requested_generated_known = 0;
  uint32_t requested_generated = 0;
  uint32_t forwarded_generated_known = 0;
  uint32_t forwarded_generated = 0;
  uint32_t accepted_generated_known = 0;
  uint32_t accepted_generated = 0;
  uint32_t provider_applied_generated_known = 0;
  uint32_t provider_applied_generated = 0;
  uint32_t structural_max_known = 0;
  uint32_t structural_max_generated = 0;
  uint32_t runtime_max_known = 0;
  uint32_t runtime_max_generated = 0;
  uint32_t effective_max_known = 0;
  uint32_t effective_max_generated = 0;
  uint32_t game_ui_max_known = 0;
  uint32_t game_ui_max_generated = 0;

  uint32_t requested_mode_known = 0;
  uint32_t requested_mode = kUnknown32;
  uint32_t forwarded_mode_known = 0;
  uint32_t forwarded_mode = kUnknown32;
  uint32_t accepted_mode_known = 0;
  uint32_t accepted_mode = kUnknown32;
  uint32_t observed_mode_known = 0;
  uint32_t observed_mode = kUnknown32;
  uint32_t dynamic_support_seen = 0;
  uint32_t dynamic_supported = 0;

  uint32_t hdr_state_seen = 0;
  uint32_t hdr_active = 0;
  uint32_t swapchain_color_space_known = 0;
  uint32_t swapchain_color_space = kUnknown32;
  uint32_t dlssg_color_space_known = 0;
  uint32_t dlssg_color_space = kUnknown32;

  uint32_t native_uir_observed = 0;
  uint32_t automatic_uir_eligible = 0;
  uint32_t automatic_uir_applied = 0;
  uint32_t automatic_uir_rejected = 0;
  uint32_t automatic_uir_reject_reasons = 0;


  // Stage 4 integration-contract evidence. Verdict values are
  // integration::Verdict numeric values; UNKNOWN is never folded into false.
  uint32_t frame_contract = 0;
  uint32_t resource_contract = 0;
  uint32_t dynamic_resolution_contract = 0;
  uint32_t queue_contract = 0;
  uint32_t swapchain_contract = 0;
  uint32_t viewport_contract = 0;

  uint32_t constants_frame_known = 0;
  uint32_t constants_frame = 0;
  uint32_t present_start_known = 0;
  uint32_t present_start_frame = 0;
  uint32_t present_end_known = 0;
  uint32_t present_end_frame = 0;
  uint32_t present_marker_mismatches = 0;
  uint32_t constants_marker_mismatches = 0;

  uint32_t resource_seen_mask = 0;
  uint32_t resource_active_mask = 0;
  uint32_t resource_clear_mask = 0;
  uint32_t resource_lifecycle_known_mask = 0;
  uint32_t resource_extent_known_mask = 0;
  uint32_t resource_state_known_mask = 0;
  uint32_t resource_format_known_mask = 0;

  uint32_t dynamic_resolution_enabled = 0;
  uint32_t dynamic_res_width = 0;
  uint32_t dynamic_res_height = 0;
  uint32_t mvec_depth_width = 0;
  uint32_t mvec_depth_height = 0;
  uint32_t color_width = 0;
  uint32_t color_height = 0;
  uint32_t num_back_buffers = 0;

  uint32_t queue_parallelism_known = 0;
  uint32_t queue_parallelism_mode = kUnknown32;
  uint32_t completion_fence_known = 0;
  uint64_t completion_fence = 0;
  uint64_t completion_fence_value = 0;
  uint32_t presenting_queue_seen = 0;
  uint64_t presenting_queue = 0;
  uint32_t queue_destroy_seen = 0;

  uint32_t swapchain_init_count = 0;
  uint32_t swapchain_destroy_count = 0;
  uint32_t swapchain_present_count = 0;
  uint32_t swapchain_recreation_count = 0;
  uint32_t swapchain_resize_count = 0;
  uint32_t swapchain_buffer_count_known = 0;
  uint32_t swapchain_buffer_count = 0;
  uint32_t fullscreen_transition_known = 0;
  uint32_t waitable_object_ownership_known = 0;
  uint32_t iflip_known = 0;

  uint32_t dlssg_status_known = 0;
  uint32_t dlssg_status_raw = 0;
  uint32_t dlssg_status_unknown_bits = 0;
  uint32_t fail_resolution_too_low = 0;
  uint32_t fail_reflex_missing = 0;
  uint32_t fail_hdr_unsupported = 0;
  uint32_t fail_constants_invalid = 0;
  uint32_t fail_backbuffer_index_missing = 0;

  uint32_t options_viewport_known = 0;
  uint32_t options_viewport = kUnknown32;
  uint32_t constants_viewport_known = 0;
  uint32_t constants_viewport = kUnknown32;
  uint32_t tag_viewport_known = 0;
  uint32_t tag_viewport = kUnknown32;
  uint32_t state_viewport_known = 0;
  uint32_t state_viewport = kUnknown32;
  uint32_t viewport_mismatches = 0;

  uint32_t set_options_thread_known = 0;
  uint32_t set_options_thread = 0;
  uint32_t present_thread_known = 0;
  uint32_t present_thread = 0;
  uint32_t set_options_on_present_thread = 0;
  uint32_t native_vsync_support_known = 0;
  uint32_t native_vsync_support = 0;
};
using QueryState = bool (*)(uint32_t, void*);
}  // namespace mfgunlock::diagnostic
