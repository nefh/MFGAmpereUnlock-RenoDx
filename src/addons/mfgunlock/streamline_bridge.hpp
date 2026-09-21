/*
 * Streamline lifecycle bridge for native DLSS-G.
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <array>
#include <atomic>
#include <cstring>
#include <sstream>
#include <vector>
#include <sl_core_api.h>
#include <string>
#include <winver.h>

// Lifecycle callbacks only pass this pointer through. No private C++ interface
// method or SystemCaps layout is accessed by the addon.
namespace sl::param {
struct IParameters;
}

#include "./ngx_bridge.hpp"
#include "./provider.hpp"

namespace mfgunlock::streamline {
using provider::GetProviderStatus;
inline void Log(const std::string& message, bool warning = false) {
  if (!warning && message.starts_with("TuringDiag") &&
      !diagnostic::g_verbose.load(std::memory_order_relaxed)) return;
  provider::Log(message, warning);
}
inline void (*g_on_interposer_loaded)() = nullptr;
inline void (*g_on_dlssg_plugin_bound)(HMODULE) = nullptr;
inline void (*g_on_dlssg_plugin_unloaded)(HMODULE) = nullptr;
namespace internal {
inline HMODULE g_self = nullptr;
inline std::atomic_bool g_shutting_down{false};

enum class LifecycleState : int {
  kNotSeen,
  kRequested,
  kLoaded,
  kStartupObserved,
  kStartupSucceeded,
  kStartupFailed,
  kUnloaded,
  kUnknown,
};

inline std::atomic<LifecycleState> g_lifecycle_state{LifecycleState::kNotSeen};
inline std::atomic_int g_feature_requested{-1};
inline std::atomic_bool g_d3d12_device_observed{false};
inline std::atomic_bool g_set_device_active{false};
inline std::atomic_bool g_set_device_completed{false};

inline bool LifecycleEnabled() {
  if (!g_enabled.load(std::memory_order_relaxed)) return false;
  return architecture::NeedsDetection() || architecture::ActiveProfile() != nullptr;
}

inline const char* LifecycleName(LifecycleState state) {
  switch (state) {
    case LifecycleState::kNotSeen: return "NOT_SEEN";
    case LifecycleState::kRequested: return "REQUESTED";
    case LifecycleState::kLoaded: return "LOADED";
    case LifecycleState::kStartupObserved: return "STARTUP_OBSERVED";
    case LifecycleState::kStartupSucceeded: return "STARTUP_SUCCEEDED";
    case LifecycleState::kStartupFailed: return "STARTUP_FAILED";
    case LifecycleState::kUnloaded: return "UNLOADED";
    case LifecycleState::kUnknown: return "UNKNOWN";
  }
  return "UNKNOWN";
}

inline void SetLifecycleState(LifecycleState state, const char* reason) {
  const auto previous = g_lifecycle_state.exchange(state, std::memory_order_acq_rel);
  if (previous == state) return;
  std::stringstream message;
  message << "TuringDiag lifecycle " << LifecycleName(previous) << " -> "
          << LifecycleName(state);
  if (reason && *reason) message << " (" << reason << ')';
  Log(message.str(), state == LifecycleState::kStartupFailed);
}

inline const char* SupportPhase() {
  if (g_set_device_active.load(std::memory_order_acquire)) return "during-slSetD3DDevice";
  if (g_set_device_completed.load(std::memory_order_acquire)) return "post-slSetD3DDevice";
  if (g_d3d12_device_observed.load(std::memory_order_acquire)) return "post-D3D12-create";
  return "pre-device";
}

struct ModuleVersion {
  unsigned int major = 0;
  unsigned int minor = 0;
  unsigned int build = 0;
  unsigned int revision = 0;
};
inline ModuleVersion ReadModuleVersion(HMODULE module) {
  // The root VS_VERSION_INFO contains one fixed-size value. Never read beyond
  // the resource or assume that arbitrary strings in .rdata identify an ABI.
  HRSRC resource = FindResourceW(module, MAKEINTRESOURCEW(VS_VERSION_INFO), MAKEINTRESOURCEW(16));
  if (!resource) return {};
  const DWORD size = SizeofResource(module, resource);
  HGLOBAL loaded = LoadResource(module, resource);
  const auto* data = loaded ? static_cast<const unsigned char*>(LockResource(loaded)) : nullptr;
  const char16_t key[] = u"VS_VERSION_INFO";
  const size_t value_at = (6 + sizeof(key) + 3) & ~size_t(3);
  if (!data || size < value_at + sizeof(VS_FIXEDFILEINFO)) return {};
  uint16_t total = 0;
  uint16_t value_size = 0;
  uint16_t type = 0;
  std::memcpy(&total, data, 2);
  std::memcpy(&value_size, data + 2, 2);
  std::memcpy(&type, data + 4, 2);
  if (type != 0 || total > size || total < value_at + sizeof(VS_FIXEDFILEINFO) ||
      value_size < sizeof(VS_FIXEDFILEINFO) || std::memcmp(data + 6, key, sizeof(key)) != 0)
    return {};
  VS_FIXEDFILEINFO fixed = {};
  std::memcpy(&fixed, data + value_at, sizeof(fixed));
  if (fixed.dwSignature != 0xfeef04bd) return {};
  return {HIWORD(fixed.dwFileVersionMS), LOWORD(fixed.dwFileVersionMS),
          HIWORD(fixed.dwFileVersionLS), LOWORD(fixed.dwFileVersionLS)};
}
enum class InterposerAbi { kUnknown, kLegacy, kModern };

inline InterposerAbi GetInterposerAbi(HMODULE module) {
  if (!module) return InterposerAbi::kUnknown;
  const auto version = ReadModuleVersion(module);
  if (version.major == 1) return InterposerAbi::kLegacy;
  if (version.major >= 2 || GetProcAddress(module, "slGetFeatureFunction"))
    return InterposerAbi::kModern;
  return InterposerAbi::kUnknown;
}

// SL 1.x passes an application ID and returns bool; SL 2.x passes an SDK
// version and returns Result. The legacy Preferences layout stays opaque.
struct LegacyPreferences;
using LegacyInitFn = bool (*)(const LegacyPreferences&, int);
inline LegacyInitFn g_real_legacy_init = nullptr;
inline hook::ModuleIdentity g_interposer_identity{};
inline std::atomic_bool g_legacy_init_hooked{false};
inline std::atomic_flag g_legacy_installing = ATOMIC_FLAG_INIT;
inline thread_local unsigned g_init_depth = 0;
inline bool HookedLegacyInit(const LegacyPreferences& preferences, int application_id);

inline const std::vector<hook::HookItem> kLegacyHooks = {
    {"slInit", reinterpret_cast<void**>(&g_real_legacy_init),
     reinterpret_cast<void*>(&HookedLegacyInit)},
};

inline PFun_slIsFeatureSupported* g_real_support = nullptr;
inline hook::ModuleIdentity g_support_identity{};
inline std::atomic_bool g_support_hooked{false};
inline std::atomic_flag g_support_installing = ATOMIC_FLAG_INIT;
inline sl::Result HookedIsFeatureSupported(sl::Feature feature, const sl::AdapterInfo& adapter);

using GetFeatureRequirementsFn = sl::Result (*)(sl::Feature, sl::FeatureRequirements&);
using IsFeatureLoadedFn = sl::Result (*)(sl::Feature, bool&);
using SetD3DDeviceFn = sl::Result (*)(void*);
inline GetFeatureRequirementsFn g_real_requirements = nullptr;
inline IsFeatureLoadedFn g_real_loaded = nullptr;
inline SetD3DDeviceFn g_real_set_d3d_device = nullptr;
inline hook::ModuleIdentity g_diagnostic_identity{};
inline std::atomic_bool g_diagnostic_hooked{false};
inline std::atomic_flag g_diagnostic_installing = ATOMIC_FLAG_INIT;
inline sl::Result HookedGetFeatureRequirements(sl::Feature feature, sl::FeatureRequirements& requirements);
inline sl::Result HookedIsFeatureLoaded(sl::Feature feature, bool& loaded);
inline sl::Result HookedSetD3DDevice(void* device);

inline const std::vector<hook::HookItem> kSupportHooks = {
    {"slIsFeatureSupported", reinterpret_cast<void**>(&g_real_support),
     reinterpret_cast<void*>(&HookedIsFeatureSupported)},
};

inline const std::vector<hook::HookItem> kDiagnosticHooks = {
    {"slGetFeatureRequirements", reinterpret_cast<void**>(&g_real_requirements),
     reinterpret_cast<void*>(&HookedGetFeatureRequirements)},
    {"slIsFeatureLoaded", reinterpret_cast<void**>(&g_real_loaded),
     reinterpret_cast<void*>(&HookedIsFeatureLoaded)},
    {"slSetD3DDevice", reinterpret_cast<void**>(&g_real_set_d3d_device),
     reinterpret_cast<void*>(&HookedSetD3DDevice)},
};

inline void RefreshPluginLifetimes();

inline void InstallLegacyInitHook() {
  if (!g_enabled.load() || !architecture::NeedsBridge()) return;
  if (g_legacy_installing.test_and_set()) return;
  struct Guard {
    ~Guard() { g_legacy_installing.clear(); }
  } guard;

  HMODULE module = GetModuleHandleW(L"sl.interposer.dll");
  if (!module || GetInterposerAbi(module) != InterposerAbi::kLegacy) return;

  if (g_interposer_identity.module && !hook::IsCurrent(g_interposer_identity)) {
    g_interposer_identity = {};
    g_real_legacy_init = nullptr;
    g_legacy_init_hooked.store(false, std::memory_order_release);
  }
  if (g_legacy_init_hooked.load(std::memory_order_acquire)) return;
  if (g_interposer_identity.module && g_interposer_identity.module != module) return;
  if (!hook::CaptureModule(module, g_interposer_identity)) return;
  if (hook::Install(module, kLegacyHooks, "Streamline 1.x"))
    g_legacy_init_hooked.store(true, std::memory_order_release);
}
inline bool HookedLegacyInit(const LegacyPreferences& preferences, int application_id) {
  if (!g_real_legacy_init) return false;
  if (g_shutting_down.load() || !g_enabled.load() || !architecture::NeedsBridge() || g_init_depth)
    return g_real_legacy_init(preferences, application_id);

  ++g_init_depth;
  struct Guard {
    ~Guard() { --g_init_depth; }
  } guard;

  ngx::BeforeInit();
  ngx::internal::ScopedArchQuery scope(ngx::internal::g_bound_gpu.load());
  const bool result = g_real_legacy_init(preferences, application_id);
  static std::atomic_bool initialized_logged{false};
  if (!result) {
    Log("legacy slInit failed", true);
  } else if (!initialized_logged.exchange(true, std::memory_order_relaxed)) {
    Log("legacy Streamline initialized");
  }
  return result;
}

// Function signatures from Streamline source/core/sl.plugin-manager/internal.h.
// IParameters is opaque here: no private SystemCaps/PluginInfo memory is written.
using GatewayFn = void* (*)(const char*);
using LoadFn = bool (*)(sl::param::IParameters*, const char*, const char**);
using StartupFn = bool (*)(const char*, void*);
struct PluginRuntime {
  HMODULE module = nullptr;
  ModuleVersion version;
  hook::AddressHook gateway_hook;
  hook::AddressHook load_hook;
  hook::AddressHook startup_hook;
};
inline std::array<PluginRuntime, 8> g_plugins;
inline SRWLOCK g_plugin_lock = SRWLOCK_INIT;

inline void LifecyclePreflight() {
  if (g_shutting_down.load(std::memory_order_acquire)) return;
  if (!architecture::NeedsBridge()) return;
  // Called by the plugin manager after LoadLibrary returned, not by its load
  // notification. Patch cached NGX exports before the native plugin builds
  // supportedAdapters, required tags, and the common needNGX flag.
  try {
    ngx::internal::ProbeAdapterBeforeInit();
    InstallLegacyInitHook();
    ngx::EnsureEntryHooks();
    if (architecture::ActiveProfile() && ngx::g_prepare_loaded_providers)
      ngx::g_prepare_loaded_providers();
  } catch (...) {
    Log("DLSS-G preflight failed", true);
  }
}

inline bool SupportAdapterMatchesBoundGpu(const sl::AdapterInfo& adapter) {
  if (!adapter.deviceLUID || adapter.deviceLUIDSizeInBytes != sizeof(LUID)) return false;
  LUID luid = {};
  std::memcpy(&luid, adapter.deviceLUID, sizeof(luid));
  return luid.LowPart == ngx::internal::g_luid_low.load(std::memory_order_acquire) &&
         static_cast<uint32_t>(luid.HighPart) ==
             ngx::internal::g_luid_high.load(std::memory_order_acquire);
}

inline sl::Result HookedIsFeatureSupported(sl::Feature feature, const sl::AdapterInfo& adapter) {
  if (!g_real_support) return sl::Result::eErrorNotInitialized;
  if (g_shutting_down.load(std::memory_order_acquire) || !g_enabled.load() ||
      !LifecycleEnabled() || feature != sl::kFeatureDLSS_G)
    return g_real_support(feature, adapter);

  LifecyclePreflight();
  RefreshPluginLifetimes();
  const auto gpu = ngx::internal::g_bound_gpu.load(std::memory_order_acquire);
  const auto* profile = architecture::ActiveProfile();
  sl::Result native_result = sl::Result::eErrorNotInitialized;
  if (profile && profile->RequiresProviderRetarget()) {
    ngx::internal::ScopedArchQuery scope(gpu);
    native_result = g_real_support(feature, adapter);
  } else {
    native_result = g_real_support(feature, adapter);
  }

  const auto native_value = static_cast<uint32_t>(native_result);
  std::stringstream message;
  message << "TuringDiag slIsFeatureSupported(DLSS_G) phase=" << SupportPhase()
          << " result=0x" << std::hex << native_value
          << " lifecycle=" << LifecycleName(g_lifecycle_state.load(std::memory_order_acquire));
  Log(message.str(), native_result != sl::Result::eOk);

  const auto provider_status = GetProviderStatus();
  const policy::StreamlineSupportEvidence evidence{
      g_enabled.load(std::memory_order_relaxed), gpu != nullptr,
      SupportAdapterMatchesBoundGpu(adapter), provider_status.QualifiedCount(), native_value};
  if (policy::CanRelaxStreamlineSupport(evidence, profile)) {
    static std::atomic_bool adjusted_logged{false};
    if (!adjusted_logged.exchange(true, std::memory_order_relaxed))
      Log("Streamline DLSS-G adapter admission relaxed (0x6 -> eOk)");
    return sl::Result::eOk;
  }
  return native_result;
}

inline sl::Result HookedGetFeatureRequirements(sl::Feature feature,
                                                      sl::FeatureRequirements& requirements) {
  if (!g_real_requirements) return sl::Result::eErrorNotInitialized;
  const auto result = g_real_requirements(feature, requirements);
  if (feature == sl::kFeatureDLSS_G) {
    RefreshPluginLifetimes();
    std::stringstream message;
    message << "TuringDiag slGetFeatureRequirements(DLSS_G) phase=" << SupportPhase()
            << " result=0x" << std::hex << static_cast<uint32_t>(result);
    Log(message.str(), result != sl::Result::eOk);
  }
  return result;
}

inline sl::Result HookedIsFeatureLoaded(sl::Feature feature, bool& loaded) {
  if (!g_real_loaded) return sl::Result::eErrorNotInitialized;
  const auto result = g_real_loaded(feature, loaded);
  if (feature == sl::kFeatureDLSS_G) {
    RefreshPluginLifetimes();
    std::stringstream message;
    message << "TuringDiag slIsFeatureLoaded(DLSS_G) phase=" << SupportPhase()
            << " result=0x" << std::hex << static_cast<uint32_t>(result)
            << " loaded=" << (loaded ? "yes" : "no");
    Log(message.str(), result != sl::Result::eOk || !loaded);
  }
  return result;
}

inline sl::Result HookedSetD3DDevice(void* device) {
  if (!g_real_set_d3d_device) return sl::Result::eErrorNotInitialized;
  g_set_device_active.store(true, std::memory_order_release);
  Log("TuringDiag slSetD3DDevice entered");
  const auto result = g_real_set_d3d_device(device);
  g_set_device_active.store(false, std::memory_order_release);
  g_set_device_completed.store(true, std::memory_order_release);
  std::stringstream message;
  message << "TuringDiag slSetD3DDevice returned 0x" << std::hex
          << static_cast<uint32_t>(result) << " lifecycle="
          << LifecycleName(g_lifecycle_state.load(std::memory_order_acquire));
  Log(message.str(), result != sl::Result::eOk);
  return result;
}

inline void InstallDiagnosticHooks() {
  if (!LifecycleEnabled()) return;
  if (g_diagnostic_installing.test_and_set()) return;
  struct Guard {
    ~Guard() { g_diagnostic_installing.clear(); }
  } guard;

  HMODULE module = GetModuleHandleW(L"sl.interposer.dll");
  if (!module || GetInterposerAbi(module) != InterposerAbi::kModern) return;

  if (g_diagnostic_identity.module && !hook::IsCurrent(g_diagnostic_identity)) {
    g_diagnostic_identity = {};
    g_real_requirements = nullptr;
    g_real_loaded = nullptr;
    g_real_set_d3d_device = nullptr;
    g_diagnostic_hooked.store(false, std::memory_order_release);
  }
  if (g_diagnostic_hooked.load(std::memory_order_acquire)) return;
  if (g_diagnostic_identity.module && g_diagnostic_identity.module != module) return;
  if (!hook::CaptureModule(module, g_diagnostic_identity)) return;
  if (hook::Install(module, kDiagnosticHooks, "Streamline lifecycle diagnostics")) {
    g_diagnostic_hooked.store(true, std::memory_order_release);
    Log("TuringDiag Streamline lifecycle diagnostics installed");
  } else {
    g_diagnostic_identity = {};
  }
}

inline void InstallSupportHook() {
  if (!LifecycleEnabled()) return;
  if (g_support_installing.test_and_set()) return;
  struct Guard {
    ~Guard() { g_support_installing.clear(); }
  } guard;

  HMODULE module = GetModuleHandleW(L"sl.interposer.dll");
  if (!module || GetInterposerAbi(module) != InterposerAbi::kModern) return;

  if (g_support_identity.module && !hook::IsCurrent(g_support_identity)) {
    g_support_identity = {};
    g_real_support = nullptr;
    g_support_hooked.store(false, std::memory_order_release);
  }
  if (g_support_hooked.load(std::memory_order_acquire)) return;
  if (g_support_identity.module && g_support_identity.module != module) return;
  if (GetProcAddress(module, "slIsFeatureSupported") == nullptr ||
      !hook::CaptureModule(module, g_support_identity)) return;
  if (hook::Install(module, kSupportHooks, "Streamline support"))
    g_support_hooked.store(true, std::memory_order_release);
}

template <size_t I>
bool OnLoad(sl::param::IParameters* parameters, const char* loader_json, const char** plugin_json) {
  auto& plugin = g_plugins[I];
  const auto real = plugin.load_hook.Original<LoadFn>();
  if (!real) return false;
  if (g_shutting_down.load(std::memory_order_acquire)) return real(parameters, loader_json, plugin_json);
  LifecyclePreflight();
  Log("TuringDiag slOnPluginLoad entered");
  const auto* profile = architecture::ActiveProfile();
  bool result = false;
  if (profile && profile->RequiresProviderRetarget()) {
    ngx::internal::ScopedArchQuery scope(ngx::internal::g_bound_gpu.load());
    result = real(parameters, loader_json, plugin_json);
  } else {
    result = real(parameters, loader_json, plugin_json);
  }
  if (result) SetLifecycleState(LifecycleState::kLoaded, "slOnPluginLoad succeeded");
  else Log("DLSS-G plugin load failed", true);
  Log(result ? "TuringDiag slOnPluginLoad returned true"
             : "TuringDiag slOnPluginLoad returned false", !result);
  return result;
}

template <size_t I>
bool OnStartup(const char* json, void* device) {
  auto& plugin = g_plugins[I];
  const auto real = plugin.startup_hook.Original<StartupFn>();
  if (!real) return false;
  if (g_shutting_down.load(std::memory_order_acquire)) return real(json, device);
  LifecyclePreflight();
  SetLifecycleState(LifecycleState::kStartupObserved, "slOnPluginStartup entered");
  Log("TuringDiag slOnPluginStartup entered");
  const auto* profile = architecture::ActiveProfile();
  bool result = false;
  if (profile && profile->RequiresProviderRetarget()) {
    ngx::internal::ScopedArchQuery scope(ngx::internal::g_bound_gpu.load());
    result = real(json, device);
  } else {
    result = real(json, device);
  }
  SetLifecycleState(result ? LifecycleState::kStartupSucceeded
                           : LifecycleState::kStartupFailed,
                    result ? "slOnPluginStartup returned true"
                           : "slOnPluginStartup returned false");
  Log(result ? "DLSS-G plugin started" : "DLSS-G plugin startup failed", !result);
  return result;
}

template <size_t I>
void* Gateway(const char* name) {
  auto& plugin = g_plugins[I];
  const auto real = plugin.gateway_hook.Original<GatewayFn>();
  if (!real) return nullptr;
  void* result = real(name);
  if (g_shutting_down.load(std::memory_order_acquire)) return result;
  if (!name || !result) return result;

  if (std::strcmp(name, "slOnPluginLoad") == 0 ||
      std::strcmp(name, "slOnPluginStartup") == 0 ||
      std::strcmp(name, "slDLSSGSetOptions") == 0 ||
      std::strcmp(name, "slDLSSGGetState") == 0) {
    std::stringstream message;
    message << "TuringDiag slGetPluginFunction lookup " << name
            << " -> " << (result ? "present" : "missing");
    Log(message.str(), result == nullptr);
  }

  bool ok = true;
  if (std::strcmp(name, "slOnPluginLoad") == 0) {
    ok = hook::InstallAddress(plugin.load_hook, result, reinterpret_cast<void*>(&OnLoad<I>),
                              "DLSS-G slOnPluginLoad");
  } else if (std::strcmp(name, "slOnPluginStartup") == 0) {
    ok = hook::InstallAddress(plugin.startup_hook, result, reinterpret_cast<void*>(&OnStartup<I>),
                              "DLSS-G slOnPluginStartup");
  }
  if (!ok) Log("DLSS-G lifecycle hook failed", true);
  // Return the native entry; the detour is removed on unload.
  return result;
}
inline const std::array<GatewayFn, 8> kGateways = {
    &Gateway<0>, &Gateway<1>, &Gateway<2>, &Gateway<3>,
    &Gateway<4>, &Gateway<5>, &Gateway<6>, &Gateway<7>};

inline void ClearPluginRuntime(PluginRuntime& plugin) {
  hook::UninstallAddress(plugin.startup_hook);
  hook::UninstallAddress(plugin.load_hook);
  hook::UninstallAddress(plugin.gateway_hook);
  plugin.module = nullptr;
  plugin.version = {};
}

inline void RefreshPluginLifetimes() {
  std::array<HMODULE, 8> unloaded{};
  size_t unloaded_count = 0;
  AcquireSRWLockExclusive(&g_plugin_lock);
  for (auto& plugin : g_plugins) {
    if (!plugin.module || hook::IsCurrent(plugin.gateway_hook.identity)) continue;
    if (unloaded_count < unloaded.size()) unloaded[unloaded_count++] = plugin.module;
    ClearPluginRuntime(plugin);
  }
  ReleaseSRWLockExclusive(&g_plugin_lock);
  if (unloaded_count != 0) {
    for (size_t i = 0; i < unloaded_count; ++i) {
      if (g_on_dlssg_plugin_unloaded) g_on_dlssg_plugin_unloaded(unloaded[i]);
    }
    SetLifecycleState(LifecycleState::kUnloaded, "plugin image disappeared");
  }
}

inline FARPROC BindPlugin(HMODULE module, FARPROC original) {
  if (!LifecycleEnabled()) return original;
  RefreshPluginLifetimes();
  const auto gateway = reinterpret_cast<GatewayFn>(original);

  const auto version = ReadModuleVersion(module);
  {
    std::stringstream message;
    message << "TuringDiag slGetPluginFunction discovered module=" << module
            << " version=" << version.major << '.' << version.minor << '.'
            << version.build << '.' << version.revision;
    Log(message.str());
  }

  // The feature probe can execute vendor code. Keep it outside our registry
  // lock so a resolver callback cannot create a lock cycle here.
  const bool modern = gateway("slDLSSGSetOptions") && gateway("slDLSSGGetState");
  const bool legacy = !modern && ReadModuleVersion(module).major == 1 &&
      ngx::internal::IsModuleNamed(module, L"sl.dlss_g.dll") &&
      gateway("slOnPluginLoad") && gateway("slOnPluginStartup");
  {
    std::stringstream message;
    message << "TuringDiag plugin classification modern=" << (modern ? "yes" : "no")
            << " legacy=" << (legacy ? "yes" : "no");
    Log(message.str(), !modern && !legacy);
  }
  if (!modern && !legacy) return original;

  AcquireSRWLockExclusive(&g_plugin_lock);
  struct Unlock {
    ~Unlock() { ReleaseSRWLockExclusive(&g_plugin_lock); }
  } unlock;

  for (auto& plugin : g_plugins) {
    if (plugin.module != module) continue;
    if (plugin.gateway_hook.Targets(reinterpret_cast<void*>(original))) return original;
    // A stale slot is safe to retire only when its old mapped image disappeared.
    if (!hook::IsCurrent(plugin.gateway_hook.identity)) {
      ClearPluginRuntime(plugin);
      break;
    }
    Log("DLSS-G function table changed", true);
    return original;
  }

  // Identify the feature by its native function table, not a game name or an
  // OTA filename. No wrapper pointer is returned to the caller.
  for (size_t i = 0; i < g_plugins.size(); ++i) {
    auto& plugin = g_plugins[i];
    if (plugin.module) continue;
    plugin.module = module;
    plugin.version = ReadModuleVersion(module);
    if (!hook::InstallAddress(plugin.gateway_hook, reinterpret_cast<void*>(original),
                              reinterpret_cast<void*>(kGateways[i]),
                              "DLSS-G function table")) {
      plugin.module = nullptr;
      plugin.version = {};
      return original;
    }
    SetLifecycleState(LifecycleState::kLoaded, "slGetPluginFunction gateway bound");
    Log("TuringDiag DLSS-G gateway bound");
    if (g_on_dlssg_plugin_bound) g_on_dlssg_plugin_bound(module);
    return original;
  }
  Log("DLSS-G plugin limit reached", true);
  return original;
}
}  // namespace internal

inline bool CanHookInit(HMODULE module) {
  return internal::GetInterposerAbi(module) == internal::InterposerAbi::kModern &&
         GetProcAddress(module, "slInit") != nullptr;
}

// Reuses framecount's slInit detour without forcing OTA flags, removing any
// requested feature, or interpreting the host's Preferences layout.
inline sl::Result OnInit(const sl::Preferences& pref, uint64_t sdk, PFun_slInit* next) {
  if (!next) return sl::Result::eErrorNotInitialized;

  int requested = -1;
  if (pref.structType == sl::Preferences::s_structType &&
      pref.structVersion == sl::kStructVersion1) {
    if (pref.featuresToLoad == nullptr) {
      requested = pref.numFeaturesToLoad == 0 ? 0 : -1;
    } else if (pref.numFeaturesToLoad <= 256) {
      requested = 0;
      for (uint32_t i = 0; i < pref.numFeaturesToLoad; ++i) {
        if (pref.featuresToLoad[i] == sl::kFeatureDLSS_G) {
          requested = 1;
          break;
        }
      }
    }
  }
  internal::g_feature_requested.store(requested, std::memory_order_release);
  if (requested == 1)
    internal::SetLifecycleState(internal::LifecycleState::kRequested, "slInit featuresToLoad");
  else if (requested < 0)
    internal::SetLifecycleState(internal::LifecycleState::kUnknown,
                                "slInit feature list malformed/unknown");
  {
    std::stringstream message;
    message << "TuringDiag slInit DLSS-G requested="
            << (requested > 0 ? "yes" : requested == 0 ? "no" : "unknown")
            << " numFeaturesToLoad=" << pref.numFeaturesToLoad;
    Log(message.str(), requested <= 0);
  }
  if (internal::g_shutting_down.load(std::memory_order_acquire) ||
      !g_enabled.load() || !architecture::NeedsBridge() || internal::g_init_depth != 0) return next(pref, sdk);
  ++internal::g_init_depth;
  struct Guard {
    ~Guard() { --internal::g_init_depth; }
  } guard;
  // The capability preflight does not read or write Preferences.
  ngx::BeforeInit();
  const auto result = next(pref, sdk);
  if (result != sl::Result::eOk) {
    std::stringstream message;
    message << "slInit failed (0x" << std::hex << static_cast<uint32_t>(result) << ')';
    Log(message.str(), true);
  }
  return result;
}

inline FARPROC Resolve(HMODULE module, const char* name, FARPROC original, const void* caller) {
  if (internal::g_shutting_down.load(std::memory_order_acquire) ||
      !internal::LifecycleEnabled() || !original || !name ||
      reinterpret_cast<uintptr_t>(name) <= 0xffff)
    return original;
  // The addon's own Detours installer must always see the real export.
  MEMORY_BASIC_INFORMATION memory = {};
  if (caller && VirtualQuery(caller, &memory, sizeof(memory)) && memory.AllocationBase == internal::g_self)
    return original;

  static thread_local bool resolving = false;
  if (resolving) return original;
  resolving = true;
  struct Guard {
    ~Guard() { resolving = false; }
  } guard;

  if (std::strcmp(name, "slGetPluginFunction") == 0) {
    internal::BindPlugin(module, original);
    return original;
  }
  if (std::strncmp(name, "sl", 2) == 0 &&
      ngx::internal::IsModuleNamed(module, L"sl.interposer.dll")) {
    internal::InstallLegacyInitHook();
    internal::InstallSupportHook();
    internal::InstallDiagnosticHooks();
    if (g_on_interposer_loaded) g_on_interposer_loaded();
  }
  ngx::Resolve(module, name, original);
  return original;
}

inline void Initialize(HMODULE self) {
  internal::g_shutting_down.store(false, std::memory_order_release);
  internal::g_lifecycle_state.store(internal::LifecycleState::kNotSeen, std::memory_order_release);
  internal::g_feature_requested.store(-1, std::memory_order_release);
  internal::g_d3d12_device_observed.store(false, std::memory_order_release);
  internal::g_set_device_active.store(false, std::memory_order_release);
  internal::g_set_device_completed.store(false, std::memory_order_release);
  internal::g_self = self;
  if (!g_enabled.load()) return;
  Log("Stage5 build active (qualified provider profiles; V4 endpoint backend preserved)");
  Log(std::string("architecture: ") + architecture::Name(architecture::g_configured.load()));
}

inline void Shutdown() {
  {
    std::stringstream message;
    message << "TuringDiag shutdown snapshot requested="
            << internal::g_feature_requested.load(std::memory_order_acquire)
            << " lifecycle="
            << internal::LifecycleName(internal::g_lifecycle_state.load(std::memory_order_acquire))
            << " d3d12=" << (internal::g_d3d12_device_observed.load() ? "yes" : "no")
            << " setDevice=" << (internal::g_set_device_completed.load() ? "yes" : "no");
    Log(message.str());
  }
  // Stop discovery before detaching addon-owned entry hooks.
  internal::g_shutting_down.store(true, std::memory_order_release);
  AcquireSRWLockExclusive(&internal::g_plugin_lock);
  for (auto it = internal::g_plugins.rbegin(); it != internal::g_plugins.rend(); ++it)
    internal::ClearPluginRuntime(*it);
  ReleaseSRWLockExclusive(&internal::g_plugin_lock);
  ngx::Shutdown();
  if (internal::g_diagnostic_hooked.exchange(false) &&
      hook::IsCurrent(internal::g_diagnostic_identity)) {
    hook::Uninstall(internal::kDiagnosticHooks);
  }
  if (internal::g_support_hooked.exchange(false) &&
      hook::IsCurrent(internal::g_support_identity)) {
    hook::Uninstall(internal::kSupportHooks);
  }
  if (internal::g_legacy_init_hooked.exchange(false) &&
      hook::IsCurrent(internal::g_interposer_identity)) {
    hook::Uninstall(internal::kLegacyHooks);
  }
  internal::g_diagnostic_identity = {};
  internal::g_real_requirements = nullptr;
  internal::g_real_loaded = nullptr;
  internal::g_real_set_d3d_device = nullptr;
  internal::g_support_identity = {};
  internal::g_real_support = nullptr;
  internal::g_interposer_identity = {};
  internal::g_real_legacy_init = nullptr;
  internal::g_self = nullptr;
  g_on_interposer_loaded = nullptr;
  g_on_dlssg_plugin_bound = nullptr;
  g_on_dlssg_plugin_unloaded = nullptr;
}

inline void NotifyD3D12DeviceObserved() {
  if (!internal::g_d3d12_device_observed.exchange(true, std::memory_order_acq_rel))
    Log("TuringDiag ReShade observed D3D12 device creation");
}

inline void Draw() {
  using namespace internal;
  ImGui::Separator();
  const auto configured = architecture::g_configured.load();
  const auto active = architecture::g_active.load();
  if (configured == Architecture::kAuto && active == Architecture::kAuto) {
    ImGui::TextDisabled("Architecture: Auto (waiting)");
  } else if (configured == Architecture::kAuto) {
    ImGui::Text("Architecture: Auto -> %s", architecture::Name(active));
  } else {
    ImGui::Text("Architecture: %s", architecture::Name(active));
  }
  const auto* profile = architecture::ActiveProfile();
  if (!g_enabled.load() || !profile || !profile->RequiresProviderRetarget()) return;

  if (HMODULE interposer = GetModuleHandleW(L"sl.interposer.dll")) {
    const auto version = ReadModuleVersion(interposer);
    ImGui::Text("Streamline: %u.%u.%u.%u", version.major, version.minor,
                version.build, version.revision);
  }

  ModuleVersion plugin_version{};
  bool plugin_seen = false;
  if (TryAcquireSRWLockShared(&g_plugin_lock)) {
    for (const auto& plugin : g_plugins) {
      if (!plugin.module) continue;
      plugin_seen = true;
      plugin_version = plugin.version;
      break;
    }
    ReleaseSRWLockShared(&g_plugin_lock);
  }
  if (plugin_seen) {
    ImGui::Text("DLSS-G: %u.%u.%u.%u", plugin_version.major, plugin_version.minor,
                plugin_version.build, plugin_version.revision);
  } else if (ngx::g_status.capabilities_seen.load()) {
    ImGui::Text("DLSS-G: NGX %s", ngx::g_status.vulkan_capabilities.load() ? "Vulkan" : "D3D12");
  } else {
    ImGui::TextDisabled("DLSS-G: waiting");
  }

  const auto provider_status = GetProviderStatus();
  ModuleVersion provider_version{};
  bool provider_ready = false;
  std::string provider_error;
  if (TryAcquireSRWLockShared(&provider::internal::g_lock)) {
    for (const auto& provider : provider::internal::g_providers) {
      if (!provider.ready) continue;
      provider_ready = true;
      provider_version = ReadModuleVersion(provider.module);
      break;
    }
    if (!provider_ready && provider_status.blocked) {
      for (const auto& rejected : provider::internal::g_rejected) {
        if (!provider::internal::IsImageMapping(rejected.module) ||
            !provider::internal::IsCurrent(rejected.module, rejected.identity)) continue;
        provider_error = rejected.reason;
        break;
      }
    }
    ReleaseSRWLockShared(&provider::internal::g_lock);
  }
  if (provider_ready) {
    ImGui::Text("Provider: %u.%u.%u.%u", provider_version.major, provider_version.minor,
                provider_version.build, provider_version.revision);
  } else if (!provider_error.empty()) {
    ImGui::TextWrapped("Provider: blocked - %s", provider_error.c_str());
  } else {
    ImGui::TextDisabled("Provider: waiting");
  }

  auto& status = ngx::g_status;
  if (status.feature_active.load()) {
    ImGui::TextUnformatted("Frame generation: active");
  } else if (status.feature_created.load()) {
    ImGui::Text("Frame generation: created");
  } else if (status.capabilities_seen.load()) {
    ImGui::Text("Frame generation: %s", status.available.load() ? "available" : "unavailable");
  } else {
    ImGui::TextDisabled("Frame generation: waiting");
  }
}
}  // namespace mfgunlock::streamline
