// SPDX-License-Identifier: MIT
#pragma once

#include <atomic>
#include <cstdint>

#include "./architecture.hpp"
#include "./feature_capabilities.hpp"
#include "./provider_profile.hpp"

namespace mfgunlock::runtime {

inline constexpr uint32_t kVersion = 1;
inline constexpr uint32_t kUnknown32 = UINT32_MAX;

enum class Phase : uint32_t {
  kDetached = 0,
  kAttached,
  kDeviceReady,
  kSwapchainReady,
  kFeatureReady,
  kDegraded,
  kShuttingDown,
};

enum class Event : uint32_t {
  kNone = 0,
  kAttach,
  kDeviceObserved,
  kDeviceRecreated,
  kSwapchainCreated,
  kSwapchainDestroyed,
  kProviderQualified,
  kProviderUnknown,
  kProviderAmbiguous,
  kProviderChanged,
  kProviderLost,
  kNgxObserved,
  kNgxTrackingLost,
  kStreamlineObserved,
  kShutdown,
  kDeviceDestroyed,
};

enum class FallbackReason : uint32_t {
  kNone = 0,
  kNoArchitecture,
  kProviderMissing,
  kProviderUnknown,
  kProviderAmbiguous,
  kProviderChanged,
  kProviderBlocked,
  kNgxTrackingLost,
  kStreamlineStartupFailed,
  kRestartRequired,
  kShuttingDown,
};

struct Snapshot {
  uint32_t bytes = sizeof(Snapshot);
  uint32_t version = kVersion;
  Phase phase = Phase::kDetached;
  Event last_event = Event::kNone;
  FallbackReason fallback_reason = FallbackReason::kNone;
  uint64_t revision = 0;

  uint32_t architecture = static_cast<uint32_t>(Architecture::kUnknown);
  uint32_t provider_backend = static_cast<uint32_t>(ProviderBackend::kUnknown);
  uint32_t target_sm = 0;
  uint32_t backend_capability_bits = 0;
  uint32_t allowed_capability_bits = 0;

  uint32_t device_known = 0;
  uint64_t device = 0;
  uint32_t device_generation = 0;
  uint32_t swapchain_alive = 0;
  uint64_t swapchain = 0;
  uint32_t swapchain_generation = 0;

  uint32_t provider_known = 0;
  uint64_t provider_module = 0;
  uint64_t provider_identity = 0;
  uint32_t provider_generation = 0;
  uint32_t provider_profile_index = 0;
  uint32_t provider_exact_qualified = 0;
  uint32_t provider_qualification = static_cast<uint32_t>(profiles::Qualification::kUnknown);

  uint64_t ngx_epoch = 0;
  uint32_t ngx_handles = 0;
  uint32_t ngx_tracking_uncertain = 0;
  uint32_t streamline_state = kUnknown32;

  uint32_t restart_required = 0;
  // Cleanup still has Detours/provider-owned rollback work under DllMain's
  // loader lock. Do not advertise live unload until that ownership is moved to
  // a safe pre-unload boundary.
  uint32_t live_unload_safe = 0;
};

inline std::atomic_uint32_t g_phase{static_cast<uint32_t>(Phase::kDetached)};
inline std::atomic_uint32_t g_last_event{static_cast<uint32_t>(Event::kNone)};
inline std::atomic_uint32_t g_fallback_reason{static_cast<uint32_t>(FallbackReason::kNone)};
inline std::atomic_uint64_t g_revision{0};
inline std::atomic_uint32_t g_architecture{static_cast<uint32_t>(Architecture::kUnknown)};
inline std::atomic_uint32_t g_provider_backend{static_cast<uint32_t>(ProviderBackend::kUnknown)};
inline std::atomic_uint32_t g_target_sm{0};
inline std::atomic_uint32_t g_backend_capabilities{0};
inline std::atomic_uint32_t g_allowed_capabilities{0};
inline std::atomic_uint64_t g_device{0};
inline std::atomic_uint32_t g_device_generation{0};
inline std::atomic_uint64_t g_swapchain{0};
inline std::atomic_uint32_t g_swapchain_alive{0};
inline std::atomic_uint32_t g_swapchain_generation{0};
inline std::atomic_uint64_t g_provider_module{0};
inline std::atomic_uint64_t g_provider_identity{0};
inline std::atomic_uint32_t g_provider_generation{0};
inline std::atomic_uint32_t g_provider_profile_index{0};
inline std::atomic_uint32_t g_provider_exact{0};
inline std::atomic_uint32_t g_provider_qualification{
    static_cast<uint32_t>(profiles::Qualification::kUnknown)};
inline std::atomic_uint64_t g_ngx_epoch{0};
inline std::atomic_uint32_t g_ngx_handles{0};
inline std::atomic_uint32_t g_ngx_uncertain{0};
inline std::atomic_uint32_t g_streamline_state{kUnknown32};
inline std::atomic_uint32_t g_restart_required{0};

inline void Touch(Event event) {
  g_last_event.store(static_cast<uint32_t>(event), std::memory_order_relaxed);
  g_revision.fetch_add(1, std::memory_order_relaxed);
}

inline void SetFallback(FallbackReason reason) {
  g_fallback_reason.store(static_cast<uint32_t>(reason), std::memory_order_relaxed);
  if (reason != FallbackReason::kNone && reason != FallbackReason::kProviderMissing)
    g_phase.store(static_cast<uint32_t>(Phase::kDegraded), std::memory_order_release);
}

inline void Reset() {
  g_phase.store(static_cast<uint32_t>(Phase::kDetached), std::memory_order_relaxed);
  g_last_event.store(static_cast<uint32_t>(Event::kNone), std::memory_order_relaxed);
  g_fallback_reason.store(static_cast<uint32_t>(FallbackReason::kNone), std::memory_order_relaxed);
  g_revision.store(0, std::memory_order_relaxed);
  g_architecture.store(static_cast<uint32_t>(Architecture::kUnknown), std::memory_order_relaxed);
  g_provider_backend.store(static_cast<uint32_t>(ProviderBackend::kUnknown), std::memory_order_relaxed);
  g_target_sm.store(0, std::memory_order_relaxed);
  g_backend_capabilities.store(0, std::memory_order_relaxed);
  g_allowed_capabilities.store(0, std::memory_order_relaxed);
  g_device.store(0, std::memory_order_relaxed);
  g_device_generation.store(0, std::memory_order_relaxed);
  g_swapchain.store(0, std::memory_order_relaxed);
  g_swapchain_alive.store(0, std::memory_order_relaxed);
  g_swapchain_generation.store(0, std::memory_order_relaxed);
  g_provider_module.store(0, std::memory_order_relaxed);
  g_provider_identity.store(0, std::memory_order_relaxed);
  g_provider_generation.store(0, std::memory_order_relaxed);
  g_provider_profile_index.store(0, std::memory_order_relaxed);
  g_provider_exact.store(0, std::memory_order_relaxed);
  g_provider_qualification.store(static_cast<uint32_t>(profiles::Qualification::kUnknown),
                                 std::memory_order_relaxed);
  g_ngx_epoch.store(0, std::memory_order_relaxed);
  g_ngx_handles.store(0, std::memory_order_relaxed);
  g_ngx_uncertain.store(0, std::memory_order_relaxed);
  g_streamline_state.store(kUnknown32, std::memory_order_relaxed);
  g_restart_required.store(0, std::memory_order_relaxed);
}

inline void Attach(const ArchitectureProfile* profile) {
  Reset();
  if (profile) {
    g_architecture.store(static_cast<uint32_t>(profile->architecture), std::memory_order_relaxed);
    g_provider_backend.store(static_cast<uint32_t>(profile->provider_backend), std::memory_order_relaxed);
    g_target_sm.store(profile->target_sm, std::memory_order_relaxed);
    g_backend_capabilities.store(features::BackendCapabilities(profile).bits,
                                 std::memory_order_relaxed);
    g_phase.store(static_cast<uint32_t>(Phase::kAttached), std::memory_order_release);
    SetFallback(FallbackReason::kProviderMissing);
  } else {
    g_phase.store(static_cast<uint32_t>(Phase::kDegraded), std::memory_order_release);
    SetFallback(FallbackReason::kNoArchitecture);
  }
  Touch(Event::kAttach);
}

// Returns true only when an already-selected primary device was replaced.
inline bool ObserveDevice(uint64_t device) {
  if (g_phase.load(std::memory_order_acquire) == static_cast<uint32_t>(Phase::kShuttingDown)) return false;
  if (device == 0) return false;
  const uint64_t previous = g_device.exchange(device, std::memory_order_acq_rel);
  if (previous == device) return false;
  g_device_generation.fetch_add(1, std::memory_order_relaxed);
  const bool replaced = previous != 0 || g_device_generation.load(std::memory_order_relaxed) > 1;
  if (!g_restart_required.load(std::memory_order_acquire)) {
    g_phase.store(static_cast<uint32_t>(Phase::kDeviceReady), std::memory_order_release);
    if (g_allowed_capabilities.load(std::memory_order_acquire) != 0)
      SetFallback(FallbackReason::kNone);
  }
  Touch(replaced ? Event::kDeviceRecreated : Event::kDeviceObserved);
  return replaced;
}

inline bool ObserveDeviceDestroy(uint64_t device) {
  if (g_phase.load(std::memory_order_acquire) == static_cast<uint32_t>(Phase::kShuttingDown)) return false;
  if (device == 0) return false;
  uint64_t expected = device;
  if (!g_device.compare_exchange_strong(expected, 0, std::memory_order_acq_rel)) return false;
  g_swapchain.store(0, std::memory_order_relaxed);
  g_swapchain_alive.store(0, std::memory_order_release);
  g_ngx_handles.store(0, std::memory_order_relaxed);
  g_streamline_state.store(kUnknown32, std::memory_order_relaxed);
  g_phase.store(static_cast<uint32_t>(Phase::kAttached), std::memory_order_release);
  Touch(Event::kDeviceDestroyed);
  return true;
}

inline void ObserveSwapchainInit(uint64_t swapchain) {
  if (g_phase.load(std::memory_order_acquire) == static_cast<uint32_t>(Phase::kShuttingDown)) return;
  if (swapchain == 0) return;
  const uint64_t previous = g_swapchain.exchange(swapchain, std::memory_order_acq_rel);
  const bool was_alive = g_swapchain_alive.exchange(1, std::memory_order_acq_rel) != 0;
  if (previous != swapchain || !was_alive)
    g_swapchain_generation.fetch_add(1, std::memory_order_relaxed);
  if (!g_restart_required.load(std::memory_order_acquire))
    g_phase.store(static_cast<uint32_t>(Phase::kSwapchainReady), std::memory_order_release);
  Touch(Event::kSwapchainCreated);
}

inline void ObserveSwapchainDestroy(uint64_t swapchain) {
  if (g_phase.load(std::memory_order_acquire) == static_cast<uint32_t>(Phase::kShuttingDown)) return;
  if (swapchain == 0 || g_swapchain.load(std::memory_order_relaxed) != swapchain) return;
  g_swapchain_alive.store(0, std::memory_order_release);
  if (!g_restart_required.load(std::memory_order_acquire))
    g_phase.store(static_cast<uint32_t>(Phase::kDeviceReady), std::memory_order_release);
  Touch(Event::kSwapchainDestroyed);
}

inline void ObserveProvider(uint64_t module, uint64_t identity, uint32_t profile_index,
                            profiles::Qualification qualification, bool exact,
                            const ArchitectureProfile* profile) {
  if (module == 0 || g_phase.load(std::memory_order_acquire) ==
                         static_cast<uint32_t>(Phase::kShuttingDown)) return;
  const uint64_t previous_module = g_provider_module.load(std::memory_order_acquire);
  const uint64_t previous_identity = g_provider_identity.load(std::memory_order_acquire);
  if (previous_module != 0 && (previous_module != module || previous_identity != identity)) {
    g_provider_generation.fetch_add(1, std::memory_order_relaxed);
    g_provider_module.store(module, std::memory_order_relaxed);
    g_provider_identity.store(identity, std::memory_order_relaxed);
    g_provider_profile_index.store(profile_index, std::memory_order_relaxed);
    g_provider_exact.store(0, std::memory_order_relaxed);
    g_provider_qualification.store(static_cast<uint32_t>(qualification), std::memory_order_relaxed);
    g_allowed_capabilities.store(0, std::memory_order_relaxed);
    g_restart_required.store(1, std::memory_order_release);
    SetFallback(FallbackReason::kProviderChanged);
    Touch(Event::kProviderChanged);
    return;
  }
  if (previous_module == 0) g_provider_generation.fetch_add(1, std::memory_order_relaxed);
  g_provider_module.store(module, std::memory_order_relaxed);
  g_provider_identity.store(identity, std::memory_order_relaxed);
  g_provider_profile_index.store(profile_index, std::memory_order_relaxed);
  g_provider_qualification.store(static_cast<uint32_t>(qualification), std::memory_order_relaxed);
  g_provider_exact.store(exact, std::memory_order_release);
  const bool qualification_ready = profile && profile->RequiresProviderRetarget()
      ? qualification == profiles::Qualification::kPatchable
      : profile != nullptr;  // A native provider needs no addon retarget transaction.
  const bool admitted = qualification_ready && profile &&
      !g_restart_required.load(std::memory_order_acquire) &&
      !g_ngx_uncertain.load(std::memory_order_acquire) &&
      g_phase.load(std::memory_order_acquire) != static_cast<uint32_t>(Phase::kShuttingDown);
  g_allowed_capabilities.store(admitted ? features::BackendCapabilities(profile).bits : 0,
                               std::memory_order_relaxed);
  if (!g_restart_required.load(std::memory_order_acquire)) {
    SetFallback(admitted ? FallbackReason::kNone : FallbackReason::kProviderUnknown);
    if (admitted && g_ngx_handles.load(std::memory_order_acquire) != 0)
      g_phase.store(static_cast<uint32_t>(Phase::kFeatureReady), std::memory_order_release);
  }
  Touch(admitted ? Event::kProviderQualified : Event::kProviderUnknown);
}

// Caller may permit recovery only after proving there are no provider-owned
// patch records, no live NGX handles, and no uncertain ownership. Otherwise a
// disappearing image requires a restart; module-handle reuse is never enough.
inline void ObserveProviderMissing(bool clean_unload = false) {
  if (g_provider_module.load(std::memory_order_acquire) != 0) {
    g_provider_generation.fetch_add(1, std::memory_order_relaxed);
    g_provider_exact.store(0, std::memory_order_relaxed);
    g_allowed_capabilities.store(0, std::memory_order_relaxed);
    if (clean_unload && g_ngx_handles.load(std::memory_order_acquire) == 0 &&
        !g_ngx_uncertain.load(std::memory_order_acquire)) {
      g_provider_module.store(0, std::memory_order_relaxed);
      g_provider_identity.store(0, std::memory_order_relaxed);
      g_provider_profile_index.store(0, std::memory_order_relaxed);
      g_provider_qualification.store(static_cast<uint32_t>(profiles::Qualification::kUnknown),
                                     std::memory_order_relaxed);
      g_restart_required.store(0, std::memory_order_release);
      SetFallback(FallbackReason::kProviderMissing);
      Touch(Event::kProviderLost);
      return;
    }
    g_restart_required.store(1, std::memory_order_release);
    SetFallback(FallbackReason::kRestartRequired);
    Touch(Event::kProviderLost);
    return;
  }
  if (!g_restart_required.load(std::memory_order_acquire))
    SetFallback(FallbackReason::kProviderMissing);
}

inline void ObserveProviderAmbiguous() {
  g_provider_exact.store(0, std::memory_order_relaxed);
  g_allowed_capabilities.store(0, std::memory_order_relaxed);
  SetFallback(FallbackReason::kProviderAmbiguous);
  Touch(Event::kProviderAmbiguous);
}

inline void ObserveProviderBlocked() {
  g_provider_exact.store(0, std::memory_order_relaxed);
  g_allowed_capabilities.store(0, std::memory_order_relaxed);
  SetFallback(FallbackReason::kProviderBlocked);
  Touch(Event::kProviderUnknown);
}

inline void ObserveNgx(uint64_t epoch, uint32_t handles, bool uncertain) {
  g_ngx_epoch.store(epoch, std::memory_order_relaxed);
  g_ngx_handles.store(handles, std::memory_order_relaxed);
  g_ngx_uncertain.store(uncertain, std::memory_order_release);
  if (uncertain) {
    g_allowed_capabilities.store(0, std::memory_order_relaxed);
    SetFallback(FallbackReason::kNgxTrackingLost);
    Touch(Event::kNgxTrackingLost);
    return;
  }
  if (!g_restart_required.load(std::memory_order_acquire) &&
      !g_ngx_uncertain.load(std::memory_order_acquire) &&
      g_phase.load(std::memory_order_acquire) != static_cast<uint32_t>(Phase::kShuttingDown) &&
      g_allowed_capabilities.load(std::memory_order_acquire) != 0 && handles != 0 &&
      g_phase.load(std::memory_order_acquire) != static_cast<uint32_t>(Phase::kShuttingDown)) {
    g_phase.store(static_cast<uint32_t>(Phase::kFeatureReady), std::memory_order_release);
  }
  if (handles == 0 && g_phase.load(std::memory_order_acquire) ==
                          static_cast<uint32_t>(Phase::kFeatureReady)) {
    g_phase.store(static_cast<uint32_t>(g_swapchain_alive.load(std::memory_order_acquire)
        ? Phase::kSwapchainReady : g_device.load(std::memory_order_acquire)
        ? Phase::kDeviceReady : Phase::kAttached), std::memory_order_release);
  }
  Touch(Event::kNgxObserved);
}

inline void ObserveStreamline(uint32_t state, bool startup_failed) {
  g_streamline_state.store(state, std::memory_order_relaxed);
  if (startup_failed) SetFallback(FallbackReason::kStreamlineStartupFailed);
  Touch(Event::kStreamlineObserved);
}

inline void BeginShutdown() {
  g_allowed_capabilities.store(0, std::memory_order_relaxed);
  g_fallback_reason.store(static_cast<uint32_t>(FallbackReason::kShuttingDown),
                          std::memory_order_relaxed);
  g_phase.store(static_cast<uint32_t>(Phase::kShuttingDown), std::memory_order_release);
  Touch(Event::kShutdown);
}

inline bool ProviderReady() {
  return !g_restart_required.load(std::memory_order_acquire) &&
      !g_ngx_uncertain.load(std::memory_order_acquire) &&
      g_phase.load(std::memory_order_acquire) != static_cast<uint32_t>(Phase::kShuttingDown) &&
      g_allowed_capabilities.load(std::memory_order_acquire) != 0;
}

inline Snapshot Read() {
  Snapshot out;
  out.phase = static_cast<Phase>(g_phase.load(std::memory_order_acquire));
  out.last_event = static_cast<Event>(g_last_event.load(std::memory_order_relaxed));
  out.fallback_reason = static_cast<FallbackReason>(
      g_fallback_reason.load(std::memory_order_relaxed));
  out.revision = g_revision.load(std::memory_order_relaxed);
  out.architecture = g_architecture.load(std::memory_order_relaxed);
  out.provider_backend = g_provider_backend.load(std::memory_order_relaxed);
  out.target_sm = g_target_sm.load(std::memory_order_relaxed);
  out.backend_capability_bits = g_backend_capabilities.load(std::memory_order_relaxed);
  out.allowed_capability_bits = g_allowed_capabilities.load(std::memory_order_relaxed);
  out.device = g_device.load(std::memory_order_relaxed);
  out.device_known = out.device != 0;
  out.device_generation = g_device_generation.load(std::memory_order_relaxed);
  out.swapchain = g_swapchain.load(std::memory_order_relaxed);
  out.swapchain_alive = g_swapchain_alive.load(std::memory_order_relaxed);
  out.swapchain_generation = g_swapchain_generation.load(std::memory_order_relaxed);
  out.provider_module = g_provider_module.load(std::memory_order_relaxed);
  out.provider_known = out.provider_module != 0;
  out.provider_identity = g_provider_identity.load(std::memory_order_relaxed);
  out.provider_generation = g_provider_generation.load(std::memory_order_relaxed);
  out.provider_profile_index = g_provider_profile_index.load(std::memory_order_relaxed);
  out.provider_exact_qualified = g_provider_exact.load(std::memory_order_acquire);
  out.provider_qualification = g_provider_qualification.load(std::memory_order_relaxed);
  out.ngx_epoch = g_ngx_epoch.load(std::memory_order_relaxed);
  out.ngx_handles = g_ngx_handles.load(std::memory_order_relaxed);
  out.ngx_tracking_uncertain = g_ngx_uncertain.load(std::memory_order_relaxed);
  out.streamline_state = g_streamline_state.load(std::memory_order_relaxed);
  out.restart_required = g_restart_required.load(std::memory_order_relaxed);
  return out;
}

inline const char* PhaseName(Phase phase) {
  switch (phase) {
    case Phase::kDetached: return "DETACHED";
    case Phase::kAttached: return "ATTACHED";
    case Phase::kDeviceReady: return "DEVICE_READY";
    case Phase::kSwapchainReady: return "SWAPCHAIN_READY";
    case Phase::kFeatureReady: return "FEATURE_READY";
    case Phase::kDegraded: return "DEGRADED";
    case Phase::kShuttingDown: return "SHUTTING_DOWN";
  }
  return "UNKNOWN";
}

inline const char* EventName(Event event) {
  switch (event) {
    case Event::kNone: return "NONE";
    case Event::kAttach: return "ATTACH";
    case Event::kDeviceObserved: return "DEVICE_OBSERVED";
    case Event::kDeviceRecreated: return "DEVICE_RECREATED";
    case Event::kSwapchainCreated: return "SWAPCHAIN_CREATED";
    case Event::kSwapchainDestroyed: return "SWAPCHAIN_DESTROYED";
    case Event::kProviderQualified: return "PROVIDER_QUALIFIED";
    case Event::kProviderUnknown: return "PROVIDER_UNKNOWN";
    case Event::kProviderAmbiguous: return "PROVIDER_AMBIGUOUS";
    case Event::kProviderChanged: return "PROVIDER_CHANGED";
    case Event::kProviderLost: return "PROVIDER_LOST";
    case Event::kNgxObserved: return "NGX_OBSERVED";
    case Event::kNgxTrackingLost: return "NGX_TRACKING_LOST";
    case Event::kStreamlineObserved: return "STREAMLINE_OBSERVED";
    case Event::kShutdown: return "SHUTDOWN";
    case Event::kDeviceDestroyed: return "DEVICE_DESTROYED";
  }
  return "UNKNOWN";
}

inline const char* FallbackReasonName(FallbackReason reason) {
  switch (reason) {
    case FallbackReason::kNone: return "NONE";
    case FallbackReason::kNoArchitecture: return "NO_ARCHITECTURE";
    case FallbackReason::kProviderMissing: return "PROVIDER_MISSING";
    case FallbackReason::kProviderUnknown: return "PROVIDER_UNKNOWN";
    case FallbackReason::kProviderAmbiguous: return "PROVIDER_AMBIGUOUS";
    case FallbackReason::kProviderChanged: return "PROVIDER_CHANGED";
    case FallbackReason::kProviderBlocked: return "PROVIDER_BLOCKED";
    case FallbackReason::kNgxTrackingLost: return "NGX_TRACKING_LOST";
    case FallbackReason::kStreamlineStartupFailed: return "STREAMLINE_STARTUP_FAILED";
    case FallbackReason::kRestartRequired: return "RESTART_REQUIRED";
    case FallbackReason::kShuttingDown: return "SHUTTING_DOWN";
  }
  return "UNKNOWN";
}

using Query = bool (*)(uint32_t, void*);

}  // namespace mfgunlock::runtime
