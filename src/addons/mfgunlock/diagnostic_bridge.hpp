// SPDX-License-Identifier: MIT
#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <cstddef>

namespace mfgunlock::diagnostic {
inline constexpr uint32_t kEvaluateVersion = 1;
inline constexpr uint32_t kStateVersion = 1;
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
inline std::atomic<uint64_t> g_evaluation_sequence{0};

inline void Emit(const EvaluateEvent& event) noexcept {
  if (const auto callback = g_evaluate_callback.load(std::memory_order_acquire)) callback(&event);
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
inline void Emit(const LifecycleEvent& event) noexcept {
  if (const auto callback = g_lifecycle_callback.load(std::memory_order_acquire)) callback(&event);
}

// Read-only point-in-time runtime state for diagnostic capture manifests.
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
};
using QueryState = bool (*)(uint32_t, DiagnosticState*);
}  // namespace mfgunlock::diagnostic
