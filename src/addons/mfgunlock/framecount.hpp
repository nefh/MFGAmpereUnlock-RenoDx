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

// 0 = leave the game's request alone. 2..5 = force that multiplier.
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

// Optional compatibility path for hosts that suppress Streamline OTA plugins.
inline std::atomic_bool g_force_ota{false};

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

inline sl::Result InitWithPreferences(const sl::Preferences& pref, uint64_t sdk_version) {
  if (g_real_init == nullptr) return sl::Result::eErrorNotInitialized;
  if (!g_enabled.load() || !architecture::ActiveProfile() ||
      !g_force_ota.load(std::memory_order_relaxed)) return g_real_init(pref, sdk_version);

  constexpr uint64_t kOta = static_cast<uint64_t>(sl::PreferenceFlags::eAllowOTA) |
                            static_cast<uint64_t>(sl::PreferenceFlags::eLoadDownloadedPlugins);
  auto& mutable_pref = const_cast<sl::Preferences&>(pref);
  const auto original = mutable_pref.flags;
  if ((static_cast<uint64_t>(original) & kOta) == kOta) return g_real_init(pref, sdk_version);

  mutable_pref.flags = static_cast<sl::PreferenceFlags>(static_cast<uint64_t>(original) | kOta);
  const sl::Result result = g_real_init(pref, sdk_version);
  mutable_pref.flags = original;
  reshade::log::message(result == sl::Result::eOk ? reshade::log::level::info
                                                  : reshade::log::level::warning,
                       result == sl::Result::eOk
                           ? "mfgunlock: Streamline OTA plugin loading enabled for this slInit call."
                           : "mfgunlock: slInit failed after enabling Streamline OTA plugin loading.");
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
  if (requested >= desired) return g_real_set_options(viewport, options);

  // More than one generated frame needs software pacing. The plugin is loaded
  // by now, so this is our last and best chance to patch it.
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

  // Fall back to the original request if the raised count is rejected.
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
    s << "mfgunlock: raising DLSS-G numFramesToGenerate from " << requested << " to " << desired
      << " (" << multiplier << "x) -- the game only ever asks for "
      << (requested + 1) << "x. slDLSSGSetOptions accepted it.";
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
  const bool alter_init = g_force_ota.load(std::memory_order_relaxed) && compatible_init;
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
