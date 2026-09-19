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

#include "./reshade_compat.hpp"

#include "./architecture.hpp"
#include "./ngx_hook.hpp"
#include "./count_observer.hpp"
#include "./diagnostic_bridge.hpp"
#include <optional>
#include "./quality_guard.hpp"
#include "./ui_candidate.hpp"

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

// 0 = leave the game's request alone. 2..6 = force that total multiplier.
inline std::atomic<unsigned int> g_force_multiplier{0};
inline std::atomic_bool g_feature_function_hooked{false};
inline std::atomic<uint64_t> g_options_revision{1};
inline std::atomic<uint64_t> g_options_cache_epoch{1};
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

// Zero means no Streamline plugin is active or its structural ceiling is not
// known yet. Direct NGX intentionally leaves this at zero.
inline std::atomic_bool g_streamline_plugin_seen{false};
inline std::atomic<unsigned int> g_streamline_max_generated{0};
inline std::atomic_bool g_capacity_advertised{false};
inline std::atomic<unsigned int> g_runtime_max_generated{0};

// NVIDIA Dynamic MFG is opt-in and only submitted after the current D3D12
// runtime reports the v4 capability on the validated 2.14.1 / 310.9.1 stack.
// The game-owned options structure is never extended or modified in place.
inline std::atomic_bool g_dynamic_mfg_enabled{false};
inline std::atomic<unsigned int> g_dynamic_target_fps{0};
inline std::atomic_bool g_dynamic_d3d12{false};
inline std::atomic_bool g_dynamic_support_seen{false};
inline std::atomic_bool g_dynamic_supported{false};
inline std::atomic_bool g_dynamic_probe_attempted{false};
inline std::atomic_bool g_dynamic_applied{false};
inline std::atomic_bool g_dynamic_fell_back{false};
inline std::atomic_bool g_dynamic_runtime_declined{false};
inline std::atomic<unsigned int> g_dynamic_result{0};
inline bool (*g_dynamic_stack_ready)() = nullptr;

// UI recomposition is independent of the selected Frame Generation preset.
// Automatic mode requests Streamline's UI-capable path only after ReShade has
// positively identified an SDR output; optional HUD/UI resources still pass
// through a fail-closed metadata guard before Streamline can consume them.
inline std::atomic_bool g_ui_composition_enabled{true};
inline std::atomic_bool g_hdr_state_seen{false};
inline std::atomic_bool g_hdr_active{false};
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
  uint32_t state = 0;
  uint32_t stable_frames = 0;
  uint32_t rtv_binds_after_clear = 0;
  uint32_t reject_reasons = uicandidate::kStale;
  uint32_t late_writes = 0;
  uint64_t clear_serial = 0;
  bool state_known = false;
  bool recent = false;
};

inline void (*g_observe_streamline_ui_tags)(const sl::ResourceTag*, uint32_t) = nullptr;
inline bool (*g_observe_ui_candidate)(UiCandidateSnapshot*) = nullptr;
inline bool (*g_acquire_ui_candidate)(const UiCandidateSnapshot*) = nullptr;
inline void (*g_release_ui_candidate)(const UiCandidateSnapshot*, bool, sl::Result) = nullptr;

// The game owns Streamline runtime selection by default. Explicit overrides
// only change NVIDIA's documented OTA flags for the slInit call.
enum class RuntimeSelectionMode : unsigned int {
  kGameDefault = 0,
  kPreferLocal = 1,
  kForceOta = 2,
};
inline std::atomic<unsigned int> g_runtime_selection_mode{
    static_cast<unsigned int>(RuntimeSelectionMode::kGameDefault)};

// More than one generated frame requires software pacing on backported
// providers. slDLSSGSetOptions is the first point where the plugin is guaranteed
// to be loaded, so verify pacing there before raising the request.
inline void (*g_ensure_pacing)() = nullptr;
inline bool (*g_pacing_ready)() = nullptr;
// Readiness of provider retargeting, the temporal program and MFG arch gates.
// Pacing remains owned by g_ensure_pacing below.
inline bool (*g_multi_frame_ready)() = nullptr;
inline std::atomic_bool g_declined_backend{false};
inline sl::Result (*g_on_init)(const sl::Preferences&, uint64_t,
    sl::Result (*)(const sl::Preferences&, uint64_t)) = nullptr;
inline bool (*g_init_compatible)(HMODULE) = nullptr;
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

inline SetOptionsFn g_real_set_options = nullptr;
inline GetStateFn g_real_get_state = nullptr;
inline hook::AddressHook g_set_options_entry;
inline hook::AddressHook g_get_state_entry;

inline SetOptionsFn RealSetOptions() {
  const auto entry = g_set_options_entry.Original<SetOptionsFn>();
  return entry ? entry : g_real_set_options;
}

inline GetStateFn RealGetState() {
  const auto entry = g_get_state_entry.Original<GetStateFn>();
  return entry ? entry : g_real_get_state;
}
inline std::atomic_bool g_entry_shutting_down{false};
inline GetFeatureFunctionFn g_real_get_feature_function = nullptr;
inline InitFn g_real_init = nullptr;
inline SetTagFn g_real_set_tag = nullptr;
inline SetTagForFrameFn g_real_set_tag_for_frame = nullptr;
inline SetConstantsFn g_real_set_constants = nullptr;
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
  const auto* profile = architecture::ActiveProfile();
  return profile && architecture::SupportsDynamicMfg(profile->architecture) &&
         g_dynamic_d3d12.load(std::memory_order_relaxed) &&
         g_streamline_plugin_seen.load(std::memory_order_acquire) &&
         g_streamline_max_generated.load(std::memory_order_acquire) != 0 &&
         g_dynamic_stack_ready != nullptr && g_dynamic_stack_ready();
}

inline bool ObserveDynamicSupport(const sl::DLSSGState& state, sl::Result result) {
  if (result != sl::Result::eOk || !DynamicStackReady() ||
      state.structVersion < sl::kStructVersion4) {
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
    if (g_dynamic_mfg_enabled.load(std::memory_order_relaxed)) NotifyLiveOptionsChanged();
    reshade::log::message(
        reshade::log::level::info,
        supported
            ? "mfgunlock: slDLSSGGetState confirms NVIDIA Dynamic MFG support on the validated runtime stack."
            : "mfgunlock: slDLSSGGetState reports NVIDIA Dynamic MFG unsupported; fixed MFG remains active.");
  }
  return true;
}

constexpr uint32_t kUnusedViewport = (std::numeric_limits<uint32_t>::max)();

struct UiViewportState {
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
    state.ui_recomposition_invalid.store(false, std::memory_order_relaxed);
    state.ui_recomposition_eligible.store(false, std::memory_order_relaxed);
    ReleaseSRWLockExclusive(&state.lock);
  }
  g_ui_pair_eligible.store(false, std::memory_order_relaxed);
  g_ui_native_hudless_fallback.store(false, std::memory_order_relaxed);
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

inline void UpdateUiCandidateFormat(uint32_t format) {
  if (format == 0) return;
  const bool was_ready = g_ui_candidate_ready.exchange(true, std::memory_order_acq_rel);
  const uint32_t previous = g_ui_candidate_format.exchange(
      format, std::memory_order_acq_rel);
  if (!was_ready || previous != format) {
    g_ui_candidate_options_synced.store(false, std::memory_order_release);
    NotifyLiveOptionsChanged();
  }
}

inline void NotifyUiCandidateUnavailable() {
  g_ui_candidate_ready.store(false, std::memory_order_release);
  g_ui_candidate_format.store(0, std::memory_order_release);
  g_ui_candidate_active.store(false, std::memory_order_relaxed);
  g_ui_candidate_options_synced.store(false, std::memory_order_release);
}

inline bool CandidateInjectionRequested() {
  return g_ui_candidate_injection_enabled.load(std::memory_order_relaxed) &&
         !g_ui_candidate_runtime_declined.load(std::memory_order_acquire);
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
         qualityguard::ShouldRequestAutomaticUiPath(
             g_hdr_state_seen.load(std::memory_order_acquire),
             g_hdr_active.load(std::memory_order_relaxed));
}

using ModeObserver = void (*)(uint32_t, uint32_t, uint32_t);
inline std::atomic<ModeObserver> g_mode_observer{nullptr};

inline sl::Result CallSetOptions(const sl::ViewportHandle& viewport,
                                 const sl::DLSSGOptions& options,
                                 countobserver::Origin origin = countobserver::Origin::kNative,
                                 bool ui_fallback = false) {
  const auto real = RealSetOptions();
  if (!real) return sl::Result::eErrorNotInitialized;
  const auto result = real(viewport, options);
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
    ObserveUiOptionsTransition(viewport, options);
    const sl::Result result = CallSetOptions(viewport, options, origin);
    if (result == sl::Result::eOk && (options.mode == sl::DLSSGMode::eOff ||
        !g_ui_composition_enabled.load(std::memory_order_relaxed) ||
        g_hdr_active.load(std::memory_order_relaxed))) {
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
  const sl::Result result = CallSetOptions(viewport, forwarded, origin);
  g_ui_composition_result.store(static_cast<unsigned int>(result),
                                std::memory_order_relaxed);
  if (result == sl::Result::eOk) {
    ObserveUiOptionsTransition(viewport, forwarded);
    const uint32_t candidate_format =
        g_ui_candidate_format.load(std::memory_order_acquire);
    const bool candidate_synced = CandidateInjectionRequested() &&
        g_ui_candidate_ready.load(std::memory_order_acquire) &&
        candidate_format != 0 && forwarded.uiBufferFormat == candidate_format;
    g_ui_candidate_options_synced.store(candidate_synced, std::memory_order_release);
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
  SetFixedOverrideState(result != sl::Result::eOk && status == FixedOverrideStatus::kAlreadyMatched
                            ? FixedOverrideStatus::kNativeRejected : status,
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
  if (!CopyOptions(source, destination)) return false;
  destination.structVersion = sl::kStructVersion5;
  destination.mode = source.mode == sl::DLSSGMode::eOff
                         ? sl::DLSSGMode::eOff : sl::DLSSGMode::eDynamic;
  destination.dynamicTargetFrameRate = static_cast<float>(
      g_dynamic_target_fps.load(std::memory_order_relaxed));
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
  return g_on_init ? g_on_init(pref, sdk_version, InitWithPreferences)
                   : InitWithPreferences(pref, sdk_version);
}

inline sl::Result SetOptionsImpl(const sl::ViewportHandle& viewport,
                                  const sl::DLSSGOptions& options, uint64_t caller,
                                  bool automatic = false) {
  if (RealSetOptions() == nullptr) return sl::Result::eErrorNotInitialized;
  const bool known = KnownOptionsAbi(options);
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
  if (!g_enabled.load() || !profile) {
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

  if (options.mode == sl::DLSSGMode::eOff) {
    g_dynamic_applied.store(false, std::memory_order_relaxed);
    g_dynamic_runtime_declined.store(false, std::memory_order_relaxed);
    const sl::Result result = ForwardSetOptions(viewport, options);
    SetFixedOverrideState(FixedOverrideStatus::kNative, 0, 0);
    return result;
  }

  const bool dynamic_requested =
      g_dynamic_mfg_enabled.load(std::memory_order_relaxed) &&
      !g_dynamic_runtime_declined.load(std::memory_order_relaxed);
  if (dynamic_requested && DynamicStackReady() &&
      g_dynamic_support_seen.load(std::memory_order_acquire) &&
      g_dynamic_supported.load(std::memory_order_relaxed)) {
    const bool backend_ready = !profile->NeedsRetarget() ||
        (g_multi_frame_ready != nullptr && g_multi_frame_ready());
    if (backend_ready) {
      sl::DLSSGOptions dynamic_options{};
      if (BuildDynamicOptions(options, dynamic_options)) {
        sl::Result result = ForwardSetOptions(viewport, dynamic_options, countobserver::Origin::kDynamic);
        if (result != sl::Result::eOk) {
          // Feature-manager startup can be transient. Retry the same validated
          // request once, then preserve the game's fixed request as fallback.
          result = ForwardSetOptions(viewport, dynamic_options, countobserver::Origin::kDynamic);
        }
        g_dynamic_result.store(static_cast<unsigned int>(result),
                               std::memory_order_relaxed);
        if (result == sl::Result::eOk) {
          SetFixedOverrideState(FixedOverrideStatus::kNative,
                                options.numFramesToGenerate, 0);
          if (!g_dynamic_applied.exchange(true, std::memory_order_relaxed)) {
            std::stringstream message;
            const unsigned int target =
                g_dynamic_target_fps.load(std::memory_order_relaxed);
            message << "mfgunlock: NVIDIA Dynamic MFG accepted (target "
                    << (target == 0 ? "active-display refresh"
                                    : std::to_string(target) + " FPS")
                    << ").";
            reshade::log::message(reshade::log::level::info,
                                  message.str().c_str());
          }
          g_dynamic_fell_back.store(false, std::memory_order_relaxed);
          return result;
        }
        g_dynamic_runtime_declined.store(true, std::memory_order_release);
        g_dynamic_applied.store(false, std::memory_order_relaxed);
        if (!g_dynamic_fell_back.exchange(true, std::memory_order_relaxed)) {
          std::stringstream message;
          message << "mfgunlock: NVIDIA Dynamic MFG was rejected with sl::Result "
                  << static_cast<unsigned int>(result)
                  << "; restoring the game's fixed mode.";
          reshade::log::message(reshade::log::level::warning,
                                message.str().c_str());
        }
      } else {
        g_dynamic_runtime_declined.store(true, std::memory_order_release);
        g_dynamic_result.store(
            static_cast<unsigned int>(sl::Result::eErrorUnsupportedInterface),
            std::memory_order_relaxed);
        if (!g_dynamic_fell_back.exchange(true, std::memory_order_relaxed)) {
          reshade::log::message(
              reshade::log::level::warning,
              "mfgunlock: Dynamic MFG was not submitted because the game supplied "
              "an unknown DLSSGOptions ABI; fixed mode remains active.");
        }
      }
    }
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
  // Backported providers need temporal correction before a native 3x/4x request.
  if (profile->NeedsRetarget() && g_multi_frame_ready != nullptr && options.mode != sl::DLSSGMode::eOff &&
      options.numFramesToGenerate > 1) {
    bool ready = g_multi_frame_ready();
    if (ready && g_pacing_ready != nullptr && !g_pacing_ready() &&
        g_ensure_pacing != nullptr && !automatic) {
      g_ensure_pacing();
    }
    if (ready && g_pacing_ready != nullptr) ready = g_pacing_ready();
    if (!ready) {
      if (!g_declined_backend.exchange(true, std::memory_order_relaxed)) {
        reshade::log::message(
            reshade::log::level::warning,
            "mfgunlock: provider not ready for MFG; using 2x.");
      }
      const uint32_t requested = options.numFramesToGenerate;
      const sl::Result result = ForwardFixedCount(viewport, options, 1, countobserver::Origin::kBackendFallback);
      SetFixedOverrideState(multiplier >= 2 ? FixedOverrideStatus::kBlockedBackend
                                            : FixedOverrideStatus::kNative,
                            requested, result == sl::Result::eOk ? 1 : 0);
      return result;
    }
  }
  if (multiplier < 2)
    return ForwardFixedNative(viewport, options, FixedOverrideStatus::kNative);

  const uint32_t desired = multiplier - 1;  // generated frames, not total
  const uint32_t requested = options.numFramesToGenerate;
  if (desired > 1 && g_multi_frame_ready != nullptr && !g_multi_frame_ready()) {
    if (!g_declined_backend.exchange(true, std::memory_order_relaxed)) {
      reshade::log::message(
          reshade::log::level::warning,
          "mfgunlock: provider not ready for MFG; multiplier unchanged.");
    }
    return ForwardFixedNative(viewport, options, FixedOverrideStatus::kBlockedBackend);
  }

  // Fixed multi-frame compatibility can use the legacy software-pacing path.
  // Dynamic MFG returns above and leaves pacing to the current NVIDIA runtime.
  if (desired > 1) {
    unsigned int ceiling = g_streamline_max_generated.load(std::memory_order_acquire);
    if (ceiling != 0 && desired > ceiling) {
      if (g_ceiling_blocked_for.exchange(desired, std::memory_order_relaxed) != desired) {
        std::stringstream message;
        message << "mfgunlock: fixed " << multiplier
                << "x not submitted: Streamline structural maximum is " << (ceiling + 1)
                << "x; preserving the game's request.";
        reshade::log::message(reshade::log::level::warning, message.str().c_str());
      }
      return ForwardFixedNative(
          viewport, options, FixedOverrideStatus::kBlockedStructuralCeiling);
    }

    if (g_pacing_ready != nullptr && !g_pacing_ready() && g_ensure_pacing != nullptr && !automatic) {
      g_ensure_pacing();
    }
    ceiling = g_streamline_max_generated.load(std::memory_order_acquire);
    if (ceiling != 0 && desired > ceiling) {
      if (g_ceiling_blocked_for.exchange(desired, std::memory_order_relaxed) != desired) {
        std::stringstream message;
        message << "mfgunlock: fixed " << multiplier
                << "x not submitted: Streamline structural maximum is " << (ceiling + 1)
                << "x; preserving the game's request.";
        reshade::log::message(reshade::log::level::warning, message.str().c_str());
      }
      return ForwardFixedNative(
          viewport, options, FixedOverrideStatus::kBlockedStructuralCeiling);
    }
    if (g_streamline_plugin_seen.load(std::memory_order_acquire) && ceiling == 0) {
      if (!g_unknown_ceiling_logged.exchange(true, std::memory_order_relaxed)) {
        reshade::log::message(
            reshade::log::level::warning,
            "mfgunlock: fixed multiplier not submitted because the active Streamline plugin's "
            "structural frame ceiling is unknown; preserving the game's request.");
      }
      return ForwardFixedNative(
          viewport, options, FixedOverrideStatus::kUnknownStructuralCeiling);
    }
    if (g_pacing_ready != nullptr && !g_pacing_ready()) {
      if (!g_declined_no_pacing.exchange(true, std::memory_order_relaxed)) {
        reshade::log::message(
            reshade::log::level::warning,
            "mfgunlock: NOT forcing the multiplier -- flip metering is still enabled, and "
            "asking for more than one generated frame without software pacing freezes "
            "presentation. Leaving the game's own request alone.");
      }
      return ForwardFixedNative(viewport, options, FixedOverrideStatus::kBlockedPacing);
    }
  }

  if (requested == desired)
    return ForwardFixedNative(viewport, options, FixedOverrideStatus::kAlreadyMatched);

  SetFixedOverrideState(FixedOverrideStatus::kPending, requested, 0);
  const sl::Result result = ForwardFixedCount(viewport, options, desired);

  // Fall back to the original request if the override is rejected.
  if (result != sl::Result::eOk) {
    // The feature manager can reject the first call while still initializing.
    // Retry the same request once before treating the count as unsupported.
    const sl::Result retry = ForwardFixedCount(viewport, options, desired, countobserver::Origin::kRetry);

    if (retry == sl::Result::eOk) {
      g_force_failed_for.store(0, std::memory_order_relaxed);
      if (SetFixedOverrideState(
              FixedOverrideStatus::kAppliedAfterRetry, requested, desired)) {
        std::stringstream s;
        s << "mfgunlock: slDLSSGSetOptions returned " << static_cast<unsigned int>(result)
          << " on the first attempt but accepted numFramesToGenerate=" << desired
          << " on retry.";
        reshade::log::message(reshade::log::level::info, s.str().c_str());
      }
      return retry;
    }

    if (g_force_failed_for.exchange(desired, std::memory_order_relaxed) != desired) {
      std::stringstream s;
      s << "mfgunlock: slDLSSGSetOptions refused numFramesToGenerate=" << desired
        << " twice (sl::Result " << static_cast<unsigned int>(result) << " then "
        << static_cast<unsigned int>(retry) << "); falling back to the game's own request of "
        << requested << ".";
      reshade::log::message(reshade::log::level::warning, s.str().c_str());
    }
    const sl::Result fallback = ForwardSetOptions(viewport, options, countobserver::Origin::kNativeFallback);
    g_dynamic_applied.store(fallback == sl::Result::eOk && options.mode == sl::DLSSGMode::eDynamic,
                            std::memory_order_relaxed);
    SetFixedOverrideState(fallback == sl::Result::eOk ? FixedOverrideStatus::kRejectedNativeFallback
                                                     : FixedOverrideStatus::kFallbackRejected,
                          requested, fallback == sl::Result::eOk ? requested : 0);
    return fallback;
  }

  g_force_failed_for.store(0, std::memory_order_relaxed);
  if (SetFixedOverrideState(FixedOverrideStatus::kApplied, requested, desired)) {
    std::stringstream s;
    s << "mfgunlock: forcing DLSS-G numFramesToGenerate from " << requested << " to " << desired
      << " (" << multiplier << "x). slDLSSGSetOptions accepted it.";
    reshade::log::message(reshade::log::level::info, s.str().c_str());
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
  g_options_cache_epoch.fetch_add(1, std::memory_order_acq_rel);
  g_present_thread.store(0, std::memory_order_release);
}

inline void MarkInjectedUiPair(UiViewportState* state) {
  if (state == nullptr) return;
  AcquireSRWLockExclusive(&state->lock);
  const bool was_eligible =
      state->ui_recomposition_eligible.exchange(true, std::memory_order_acq_rel);
  state->hudless_color_seen.store(true, std::memory_order_release);
  state->ui_color_or_alpha_seen.store(true, std::memory_order_release);
  state->ui_color_alpha_seen.store(true, std::memory_order_release);
  state->ui_recomposition_invalid.store(false, std::memory_order_release);
  state->hud_separation_suppressed.store(false, std::memory_order_release);
  if (!was_eligible) RequestUiReset(state);
  ReleaseSRWLockExclusive(&state->lock);

  g_ui_pair_eligible.store(true, std::memory_order_relaxed);
  g_ui_native_hudless_fallback.store(false, std::memory_order_relaxed);
}

template <typename Forward>
inline sl::Result TryInjectUiCandidate(UiViewportState* state,
                                      const UiCandidateSnapshot& candidate,
                                      const sl::ResourceTag* tags, uint32_t count,
                                      sl::CommandBuffer* command_buffer,
                                      Forward&& forward) {
  g_ui_candidate_active.store(false, std::memory_order_relaxed);
  const bool injection_requested = CandidateInjectionRequested();
  const bool options_synced =
      g_ui_candidate_options_synced.load(std::memory_order_acquire);
  const bool can_inject = uicandidate::CanInject(
      candidate.reject_reasons, candidate.stable_frames, command_buffer != nullptr,
      options_synced, injection_requested);
  if (can_inject && count >= 64) {
    g_ui_candidate_full_batches.fetch_add(1, std::memory_order_relaxed);
    return forward(tags, count);
  }
  if (!can_inject) {
    if (injection_requested && options_synced &&
        candidate.reject_reasons == uicandidate::kAccept &&
        uicandidate::IsStable(candidate.stable_frames) && command_buffer == nullptr) {
      g_ui_candidate_missing_command_buffer.fetch_add(1, std::memory_order_relaxed);
    }
    return forward(tags, count);
  }
  if (ConsumeCandidateRetryFrame()) return forward(tags, count);
  if (g_acquire_ui_candidate == nullptr || g_release_ui_candidate == nullptr ||
      !g_acquire_ui_candidate(&candidate)) {
    return forward(tags, count);
  }

  sl::Resource ui_resource(
      sl::ResourceType::eTex2d,
      reinterpret_cast<void*>(static_cast<uintptr_t>(candidate.native_resource)),
      candidate.state);
  ui_resource.width = candidate.width;
  ui_resource.height = candidate.height;
  ui_resource.nativeFormat = candidate.format;
  sl::Extent ui_extent{};
  ui_extent.width = candidate.width;
  ui_extent.height = candidate.height;
  sl::ResourceTag ui_tag(&ui_resource, sl::kBufferTypeUIColorAndAlpha,
                         sl::ResourceLifecycle::eOnlyValidNow, &ui_extent);

  static_assert(std::is_trivially_copyable_v<sl::ResourceTag>);
  alignas(sl::ResourceTag)
      std::array<std::byte, sizeof(sl::ResourceTag) * 65> storage{};
  std::memcpy(storage.data(), tags, sizeof(sl::ResourceTag) * count);
  std::memcpy(storage.data() + sizeof(sl::ResourceTag) * count,
              &ui_tag, sizeof(ui_tag));
  const auto* forwarded = reinterpret_cast<const sl::ResourceTag*>(storage.data());
  const sl::Result result = forward(forwarded, count + 1);
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
    MarkInjectedUiPair(state);
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
    message << "mfgunlock: detected UI Color+Alpha tag failed " << failure_streak
            << " consecutive times on resource 0x" << std::hex
            << candidate.native_resource << std::dec << " (sl::Result "
            << static_cast<unsigned int>(result)
            << "); disabling automatic UI injection until the option is toggled.";
    reshade::log::message(reshade::log::level::warning, message.str().c_str());
  } else if (failure_streak == 1) {
    std::stringstream message;
    message << "mfgunlock: detected UI Color+Alpha tag was transiently rejected with "
            << "sl::Result " << static_cast<unsigned int>(result)
            << "; preserving native HUD-less and retrying after candidate revalidation.";
    reshade::log::message(reshade::log::level::warning, message.str().c_str());
  }
  return forward(tags, count);
}

template <typename Forward>
inline sl::Result FilterUiTags(const sl::ViewportHandle& viewport,
                               const sl::ResourceTag* tags, uint32_t count,
                               sl::CommandBuffer* command_buffer,
                               Forward&& forward) {
  if (!g_enabled.load(std::memory_order_relaxed) ||
      !g_ui_composition_enabled.load(std::memory_order_relaxed) ||
      tags == nullptr || count == 0) {
    return forward(tags, count);
  }

  if (count > 64) {
    if (!g_ui_tag_batch_too_large.exchange(true, std::memory_order_relaxed)) {
      reshade::log::message(
          reshade::log::level::warning,
          "mfgunlock: UI Composition received more than 64 Streamline tags in one call; "
          "the oversized batch is forwarded unchanged.");
    }
    return forward(tags, count);
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

  UiViewportState* state = GetUiState(viewport);
  const bool hdr = g_hdr_active.load(std::memory_order_relaxed);
  qualityguard::OutputDescription expected{};
  if (state != nullptr) {
    AcquireSRWLockShared(&state->lock);
    expected = ExpectedUiOutput(state);
    ReleaseSRWLockShared(&state->lock);
  }

  const auto assessment = qualityguard::AssessTags(tags, count, hdr, expected);
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
      state->hudless_color_seen.store(false, std::memory_order_relaxed);
      state->ui_color_or_alpha_seen.store(false, std::memory_order_relaxed);
      state->ui_color_alpha_seen.store(false, std::memory_order_relaxed);
      state->ui_alpha_seen.store(false, std::memory_order_relaxed);
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

      auto accumulated = assessment;
      accumulated.has_hudless_color =
          state->hudless_color_seen.load(std::memory_order_acquire);
      accumulated.has_ui_color_or_alpha =
          state->ui_color_or_alpha_seen.load(std::memory_order_acquire);
      if (state->ui_recomposition_invalid.load(std::memory_order_acquire))
        accumulated.issues |= qualityguard::kInvalidOptionalResource;

      eligible = qualityguard::CanAutomaticallyUseUiRecomposition(accumulated, hdr);
      suppression = qualityguard::ResolveSuppression(
          assessment, accumulated, hdr, was_eligible);
      const bool suppress = suppression != qualityguard::SuppressionPolicy::kNone;
      const bool native_hudless_fallback =
          !hdr && accumulated.has_hudless_color &&
          !accumulated.has_ui_color_or_alpha &&
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

  const bool native_hudless_only = state != nullptr && !hdr &&
      assessment.has_hudless_color && !assessment.has_ui_color_or_alpha &&
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
      UpdateUiCandidateFormat(candidate.format);
      return TryInjectUiCandidate(
          state, candidate, tags, count, command_buffer, std::forward<Forward>(forward));
    }
  }

  if (suppression == qualityguard::SuppressionPolicy::kNone) return forward(tags, count);

  static_assert(std::is_trivially_copyable_v<sl::ResourceTag>);
  alignas(sl::ResourceTag)
      std::array<std::byte, sizeof(sl::ResourceTag) * 64> storage{};
  std::memcpy(storage.data(), tags, sizeof(sl::ResourceTag) * count);
  auto* forwarded = reinterpret_cast<sl::ResourceTag*>(storage.data());
  const uint32_t suppressed =
      suppression == qualityguard::SuppressionPolicy::kAllHudSeparation
          ? SuppressHudSeparationResources(forwarded, count)
          : SuppressUiResources(forwarded, count);
  if (suppressed == 0) return forward(tags, count);

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
  return forward(forwarded, count);
}

inline sl::Result HookedSetTag(const sl::ViewportHandle& viewport,
                               const sl::ResourceTag* tags, uint32_t count,
                               sl::CommandBuffer* command_buffer) {
  if (g_real_set_tag == nullptr) return sl::Result::eErrorNotInitialized;
  return FilterUiTags(
      viewport, tags, count, command_buffer,
      [&](const sl::ResourceTag* forwarded, uint32_t forwarded_count) {
        return g_real_set_tag(viewport, forwarded, forwarded_count, command_buffer);
      });
}

inline sl::Result HookedSetTagForFrame(const sl::FrameToken& frame,
                                       const sl::ViewportHandle& viewport,
                                       const sl::ResourceTag* tags, uint32_t count,
                                       sl::CommandBuffer* command_buffer) {
  if (g_real_set_tag_for_frame == nullptr) return sl::Result::eErrorNotInitialized;
  return FilterUiTags(
      viewport, tags, count, command_buffer,
      [&](const sl::ResourceTag* forwarded, uint32_t forwarded_count) {
        return g_real_set_tag_for_frame(
            frame, viewport, forwarded, forwarded_count, command_buffer);
      });
}

inline sl::Result HookedSetConstants(const sl::Constants& values,
                                     const sl::FrameToken& frame,
                                     const sl::ViewportHandle& viewport) {
  if (g_real_set_constants == nullptr) return sl::Result::eErrorNotInitialized;
  ApplyLiveOptions(viewport);
  if (!g_enabled.load(std::memory_order_relaxed) ||
      !g_ui_composition_enabled.load(std::memory_order_relaxed)) {
    return g_real_set_constants(values, frame, viewport);
  }

  UiViewportState* state = GetUiState(viewport);
  if (state == nullptr || !state->options_seen.load(std::memory_order_acquire) ||
      state->mode.load(std::memory_order_relaxed) ==
          static_cast<uint32_t>(sl::DLSSGMode::eOff)) {
    return g_real_set_constants(values, frame, viewport);
  }

  const uint64_t requested = state->reset_requested.load(std::memory_order_acquire);
  if (requested == state->reset_applied.load(std::memory_order_relaxed))
    return g_real_set_constants(values, frame, viewport);

  if (values.reset == sl::Boolean::eTrue) {
    const sl::Result result = g_real_set_constants(values, frame, viewport);
    if (result == sl::Result::eOk)
      state->reset_applied.store(requested, std::memory_order_release);
    return result;
  }

  alignas(sl::Constants) std::array<std::byte, sizeof(sl::Constants)> storage{};
  if (!qualityguard::CopyConstantsWithReset(values, storage.data(), storage.size()))
    return g_real_set_constants(values, frame, viewport);

  const sl::Result result = g_real_set_constants(
      *reinterpret_cast<const sl::Constants*>(storage.data()), frame, viewport);
  if (result == sl::Result::eOk) {
    state->reset_applied.store(requested, std::memory_order_release);
    g_ui_resets_injected.fetch_add(1, std::memory_order_relaxed);
  }
  return result;
}

inline sl::Result HookedGetState(const sl::ViewportHandle& viewport, sl::DLSSGState& state,
                                 const sl::DLSSGOptions* options) {
  if (RealGetState() == nullptr) return sl::Result::eErrorNotInitialized;
  if (!g_enabled.load() || !architecture::ActiveProfile())
    return RealGetState()(viewport, state, options);

  const sl::Result result = RealGetState()(viewport, state, options);
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
      state.structVersion < sl::kStructVersion1 || state.structVersion > sl::kStructVersion4)
    return result;
  if (result == sl::Result::eOk) {
    ObserveDynamicSupport(state, result);
    if (!g_dynamic_support_seen.load(std::memory_order_acquire) &&
        DynamicStackReady() && state.structVersion < sl::kStructVersion4 &&
        !g_dynamic_probe_attempted.exchange(true, std::memory_order_acq_rel)) {
      // Older games allocate the state ABI they were built against. Probe v4
      // into addon-owned storage so no field beyond the caller's allocation is
      // ever read or written. The original call/result above remains native.
      sl::DLSSGState extended{};
      extended.next = state.next;
      extended.structVersion = sl::kStructVersion4;
      const sl::Result probe = RealGetState()(viewport, extended, options);
      ObserveDynamicSupport(extended, probe);
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
  if (result != sl::Result::eOk || state.structVersion < sl::kStructVersion2) return result;

  const unsigned int reported = state.numFramesToGenerateMax;
  g_runtime_max_generated.store(reported, std::memory_order_relaxed);
  if (g_multi_frame_ready != nullptr && !g_multi_frame_ready()) {
    if (state.numFramesToGenerateMax > 1) state.numFramesToGenerateMax = 1;
    return result;
  }

  const unsigned int wanted = g_streamline_max_generated.load(std::memory_order_relaxed);
  if (wanted == 0 || reported == wanted) return result;

  state.numFramesToGenerateMax = wanted;
  if (!g_capacity_advertised.exchange(true, std::memory_order_relaxed)) {
    std::stringstream s;
    s << "mfgunlock: slDLSSGGetState reported a maximum of " << reported
      << " generated frame(s); using the Streamline structural ceiling of " << wanted
      << " (" << (wanted + 1) << "x).";
    reshade::log::message(reshade::log::level::info, s.str().c_str());
  }
  return result;
}

inline sl::Result HookedGetFeatureFunction(sl::Feature feature, const char* function_name,
                                                void*& function) {
  const sl::Result result = g_real_get_feature_function(feature, function_name, function);
  if (result != sl::Result::eOk || function_name == nullptr || function == nullptr) return result;
  if (feature != sl::kFeatureDLSS_G || g_entry_shutting_down.load(std::memory_order_acquire)) return result;

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

inline void NotifyHdrState(bool hdr) {
  const bool previous = g_hdr_active.exchange(hdr, std::memory_order_relaxed);
  const bool seen = g_hdr_state_seen.exchange(true, std::memory_order_acq_rel);
  if (!seen || previous != hdr) {
    internal::ForgetUiOutputs();
    internal::RequestAllUiResets();
    g_ui_composition_applied.store(false, std::memory_order_relaxed);
    NotifyLiveOptionsChanged();
  }
}

inline void NotifyOutputUnknown() {
  g_hdr_state_seen.store(false, std::memory_order_release);
  g_hdr_active.store(false, std::memory_order_relaxed);
  g_ui_composition_applied.store(false, std::memory_order_relaxed);
  internal::NotifyUiCandidateUnavailable();
  internal::ForgetUiOutputs();
  internal::RequestAllUiResets();
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
  g_dynamic_fell_back.store(false, std::memory_order_relaxed);
  g_dynamic_runtime_declined.store(false, std::memory_order_release);
  g_dynamic_result.store(0, std::memory_order_relaxed);
  g_dynamic_probe_attempted.store(false, std::memory_order_relaxed);
  NotifyLiveOptionsChanged();
}

// Install before the host caches DLSS-G function pointers.
inline void TryInstall() {
  static thread_local bool installing = false;
  if (installing) return;
  installing = true;
  struct Guard {
    ~Guard() { installing = false; }
  } guard;
  HMODULE interposer = GetModuleHandleW(L"sl.interposer.dll");
  if (interposer == nullptr) return;

  if (!g_feature_function_hooked.load(std::memory_order_acquire) &&
      GetProcAddress(interposer, "slGetFeatureFunction") != nullptr &&
      hook::Install(interposer, internal::kFeatureFunctionHook, "sl.interposer.dll")) {
    g_feature_function_hooked.store(true, std::memory_order_release);
  }

  internal::TryInstallUiHooks(interposer);

  const bool compatible_init = g_init_compatible ? g_init_compatible(interposer)
      : GetProcAddress(interposer, "slGetFeatureFunction") != nullptr;
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
  internal::InvalidateLiveOptions();
  internal::g_entry_shutting_down.store(true, std::memory_order_release);
  if (internal::g_ui_hooks_installed.load(std::memory_order_acquire) &&
      !internal::g_ui_hooks.empty()) {
    hook::Uninstall(internal::g_ui_hooks);
    internal::g_ui_hooks.clear();
    internal::g_ui_hooks_installed.store(false, std::memory_order_release);
  }
  if (g_init_hooked.load(std::memory_order_acquire)) hook::Uninstall(internal::kInitHook);
  if (g_feature_function_hooked.load(std::memory_order_acquire))
    hook::Uninstall(internal::kFeatureFunctionHook);
  g_init_hooked.store(false, std::memory_order_release);
  g_feature_function_hooked.store(false, std::memory_order_release);

  hook::UninstallAddress(internal::g_get_state_entry);
  hook::UninstallAddress(internal::g_set_options_entry);
  internal::g_real_get_state = nullptr;
  internal::g_real_set_options = nullptr;
  internal::g_real_set_tag = nullptr;
  internal::g_real_set_tag_for_frame = nullptr;
  internal::g_real_set_constants = nullptr;
}

}  // namespace mfgunlock::framecount
