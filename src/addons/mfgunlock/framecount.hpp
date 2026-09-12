/*
 * DLSS-G frame-count policy.
 * SPDX-License-Identifier: MIT
 *
 * Capability advertising and request forcing are separate. Hosts with a native
 * multiplier selector only need the verified maximum exposed through GetState;
 * hosts with an on/off control can optionally have numFramesToGenerate raised
 * at slDLSSGSetOptions. The caller-owned options structure is always restored
 * before returning.
 */

#pragma once

#include <vector>
#include <windows.h>

#include <atomic>
#include <cstring>
#include <sstream>

#include <sl.h>
#include <sl_dlss_g.h>

#include <include/reshade.hpp>

#include "./architecture.hpp"
#include "./ngx_hook.hpp"

namespace mfgunlock::framecount {

// 0 = leave the game's request alone. 2..6 = force that total multiplier.
inline std::atomic<unsigned int> g_force_multiplier{0};
inline std::atomic_bool g_feature_function_hooked{false};
inline std::atomic_bool g_init_hooked{false};
inline std::atomic_bool g_intercepted{false};
inline std::atomic<unsigned int> g_last_requested{0};
inline std::atomic<unsigned int> g_last_forced{0};
inline std::atomic_bool g_declined_no_pacing{false};
inline std::atomic<unsigned int> g_force_failed_for{0};
inline std::atomic_bool g_state_seen{false};
inline std::atomic<unsigned int> g_dlssg_status{0};
inline std::atomic_bool g_failure_status_logged{false};

// Published by addon.cpp after the active Streamline wrapper's pacing and hard
// ceiling have been verified. Native multiplier selectors may read this value
// from DLSSGState rather than from the NGX parameter block.
inline std::atomic<unsigned int> g_advertised_max_generated{0};
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

inline SetOptionsFn g_real_set_options = nullptr;
inline GetStateFn g_real_get_state = nullptr;
inline hook::AddressHook g_set_options_entry;
inline hook::AddressHook g_get_state_entry;
inline std::atomic_bool g_entry_shutting_down{false};
inline GetFeatureFunctionFn g_real_get_feature_function = nullptr;
inline InitFn g_real_init = nullptr;

inline bool DynamicStackReady() {
  return g_dynamic_d3d12.load(std::memory_order_relaxed) &&
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
    reshade::log::message(
        reshade::log::level::info,
        supported
            ? "mfgunlock: slDLSSGGetState confirms NVIDIA Dynamic MFG support on the validated runtime stack."
            : "mfgunlock: slDLSSGGetState reports NVIDIA Dynamic MFG unsupported; fixed MFG remains active.");
  }
  return true;
}

inline bool BuildDynamicOptions(const sl::DLSSGOptions& source,
                                sl::DLSSGOptions& destination) {
  const size_t version = source.structVersion;
  if (version < sl::kStructVersion1 || version > sl::kStructVersion5) return false;

  destination = sl::DLSSGOptions{};
  destination.next = source.next;
  destination.structVersion = sl::kStructVersion5;

  // Version 1.
  destination.mode = source.mode == sl::DLSSGMode::eOff
                         ? sl::DLSSGMode::eOff
                         : sl::DLSSGMode::eDynamic;
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

inline sl::Result HookedSetOptions(const sl::ViewportHandle& viewport,
                                   const sl::DLSSGOptions& options) {
  if (g_real_set_options == nullptr) return sl::Result::eErrorNotInitialized;
  const auto* profile = architecture::ActiveProfile();
  if (!g_enabled.load() || !profile) return g_real_set_options(viewport, options);

  if (options.mode == sl::DLSSGMode::eOff) {
    g_dynamic_applied.store(false, std::memory_order_relaxed);
    g_dynamic_runtime_declined.store(false, std::memory_order_relaxed);
    return g_real_set_options(viewport, options);
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
        sl::Result result = g_real_set_options(viewport, dynamic_options);
        if (result != sl::Result::eOk) {
          // Feature-manager startup can be transient. Retry the same validated
          // request once, then preserve the game's fixed request as fallback.
          result = g_real_set_options(viewport, dynamic_options);
        }
        g_dynamic_result.store(static_cast<unsigned int>(result),
                               std::memory_order_relaxed);
        if (result == sl::Result::eOk) {
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

  const unsigned int multiplier = g_force_multiplier.load(std::memory_order_relaxed);
  // Backported providers need temporal correction before a native 3x/4x request.
  if (profile->NeedsRetarget() && g_multi_frame_ready != nullptr && options.mode != sl::DLSSGMode::eOff &&
      options.numFramesToGenerate > 1) {
    bool ready = g_multi_frame_ready();
    if (ready && g_pacing_ready != nullptr && !g_pacing_ready() &&
        g_ensure_pacing != nullptr) {
      g_ensure_pacing();
    }
    if (ready && g_pacing_ready != nullptr) ready = g_pacing_ready();
    if (!ready) {
      if (!g_declined_backend.exchange(true, std::memory_order_relaxed)) {
        reshade::log::message(
            reshade::log::level::warning,
            "mfgunlock: provider not ready for MFG; using 2x.");
      }
      auto& mutable_options = const_cast<sl::DLSSGOptions&>(options);
      const uint32_t requested = options.numFramesToGenerate;
      mutable_options.numFramesToGenerate = 1;
      const sl::Result result = g_real_set_options(viewport, options);
      mutable_options.numFramesToGenerate = requested;
      return result;
    }
  }
  if (multiplier < 2 || options.mode == sl::DLSSGMode::eOff)
    return g_real_set_options(viewport, options);

  const uint32_t desired = multiplier - 1;  // generated frames, not total
  const uint32_t requested = options.numFramesToGenerate;
  if (desired > 1 && g_multi_frame_ready != nullptr && !g_multi_frame_ready()) {
    if (!g_declined_backend.exchange(true, std::memory_order_relaxed)) {
      reshade::log::message(
          reshade::log::level::warning,
          "mfgunlock: provider not ready for MFG; multiplier unchanged.");
    }
    return g_real_set_options(viewport, options);
  }
  g_last_requested.store(requested, std::memory_order_relaxed);
  if (requested == desired) return g_real_set_options(viewport, options);

  // Fixed multi-frame compatibility can use the legacy software-pacing path.
  // Dynamic MFG returns above and leaves pacing to the current NVIDIA runtime.
  if (desired > 1) {
    if (g_pacing_ready != nullptr && !g_pacing_ready() && g_ensure_pacing != nullptr) {
      g_ensure_pacing();
    }
    if (g_pacing_ready != nullptr && !g_pacing_ready()) {
      if (!g_declined_no_pacing.exchange(true, std::memory_order_relaxed)) {
        reshade::log::message(
            reshade::log::level::warning,
            "mfgunlock: NOT forcing the multiplier -- flip metering is still enabled, and "
            "asking for more than one generated frame without software pacing freezes "
            "presentation. Leaving the game's own request alone.");
      }
      return g_real_set_options(viewport, options);
    }
  }

  // The caller owns this structure, so restore it before returning.
  auto& mutable_options = const_cast<sl::DLSSGOptions&>(options);
  mutable_options.numFramesToGenerate = desired;
  sl::Result result = g_real_set_options(viewport, options);
  mutable_options.numFramesToGenerate = requested;

  // Fall back to the original request if the override is rejected.
  if (result != sl::Result::eOk) {
    // The feature manager can reject the first call while still initializing.
    // Retry the same request once before treating the count as unsupported.
    mutable_options.numFramesToGenerate = desired;
    const sl::Result retry = g_real_set_options(viewport, options);
    mutable_options.numFramesToGenerate = requested;

    if (retry == sl::Result::eOk) {
      if (!g_intercepted.exchange(true, std::memory_order_relaxed)) {
        std::stringstream s;
        s << "mfgunlock: slDLSSGSetOptions returned " << static_cast<unsigned int>(result)
          << " on the first attempt but accepted numFramesToGenerate=" << desired
          << " on retry -- that first failure was feature-manager state, not the count.";
        reshade::log::message(reshade::log::level::info, s.str().c_str());
      }
      g_last_forced.store(desired, std::memory_order_relaxed);
      return retry;
    }

    if (g_force_failed_for.exchange(desired, std::memory_order_relaxed) != desired) {
      std::stringstream s;
      s << "mfgunlock: slDLSSGSetOptions refused numFramesToGenerate=" << desired
        << " twice (sl::Result " << static_cast<unsigned int>(result) << " then "
        << static_cast<unsigned int>(retry) << "); falling back to the game's own request of "
        << requested << ". The count itself is being refused, not a transient state.";
      reshade::log::message(reshade::log::level::warning, s.str().c_str());
    }
    return g_real_set_options(viewport, options);
  }

  if (!g_intercepted.exchange(true, std::memory_order_relaxed)) {
    std::stringstream s;
    s << "mfgunlock: forcing DLSS-G numFramesToGenerate from " << requested << " to " << desired
      << " (" << multiplier << "x). slDLSSGSetOptions accepted it.";
    reshade::log::message(reshade::log::level::info, s.str().c_str());
  }
  g_last_forced.store(desired, std::memory_order_relaxed);
  return result;
}

inline sl::Result HookedGetState(const sl::ViewportHandle& viewport, sl::DLSSGState& state,
                                 const sl::DLSSGOptions* options) {
  if (g_real_get_state == nullptr) return sl::Result::eErrorNotInitialized;
  if (!g_enabled.load() || !architecture::ActiveProfile())
    return g_real_get_state(viewport, state, options);

  const sl::Result result = g_real_get_state(viewport, state, options);
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
      const sl::Result probe = g_real_get_state(viewport, extended, options);
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

  const unsigned int wanted = g_advertised_max_generated.load(std::memory_order_relaxed);
  if (wanted < 2 || reported >= wanted) return result;

  state.numFramesToGenerateMax = wanted;
  if (!g_capacity_advertised.exchange(true, std::memory_order_relaxed)) {
    std::stringstream s;
    s << "mfgunlock: slDLSSGGetState reported a maximum of " << reported
      << " generated frame(s); advertising the verified Streamline ceiling of " << wanted
      << " so the game's native multiplier selector can expose up to " << (wanted + 1) << "x.";
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

inline void NotifyDynamicD3D12(bool d3d12) {
  g_dynamic_d3d12.store(d3d12, std::memory_order_relaxed);
}

inline void NotifyDynamicModeChanged() {
  g_dynamic_applied.store(false, std::memory_order_relaxed);
  g_dynamic_fell_back.store(false, std::memory_order_relaxed);
  g_dynamic_runtime_declined.store(false, std::memory_order_release);
  g_dynamic_result.store(0, std::memory_order_relaxed);
  g_dynamic_probe_attempted.store(false, std::memory_order_relaxed);
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
  internal::g_entry_shutting_down.store(true, std::memory_order_release);
  if (g_init_hooked.load(std::memory_order_acquire)) hook::Uninstall(internal::kInitHook);
  if (g_feature_function_hooked.load(std::memory_order_acquire))
    hook::Uninstall(internal::kFeatureFunctionHook);
  g_init_hooked.store(false, std::memory_order_release);
  g_feature_function_hooked.store(false, std::memory_order_release);

  hook::UninstallAddress(internal::g_get_state_entry);
  hook::UninstallAddress(internal::g_set_options_entry);
  internal::g_real_get_state = nullptr;
  internal::g_real_set_options = nullptr;
}

}  // namespace mfgunlock::framecount
