/*
 * Streamline lifecycle bridge for native DLSS-G.
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <sl_core_api.h>
#include <string>
#include <winver.h>

// Lifecycle callbacks only pass this pointer through. No private C++ interface
// method or SystemCaps layout is accessed by the addon.
namespace sl::param {
struct IParameters;
}

#include "./ampere_ngx.hpp"

namespace mfgunlock::ampere::caps {
namespace internal {
inline HMODULE g_self = nullptr;
inline std::atomic_bool g_shutting_down{false};

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
inline bool KnownStreamline(HMODULE module) {
  const auto version = ReadModuleVersion(module);
  return version.major == 2 && version.minor >= 7 && version.minor <= 12;
}

inline std::atomic_bool g_v2_seen{false};
inline std::atomic_bool g_observers_installed{false};
inline std::atomic_flag g_observers_installing = ATOMIC_FLAG_INIT;
inline std::atomic_uint64_t g_init_calls{0};
inline std::atomic_uint32_t g_init_result{0};
inline thread_local unsigned g_init_depth = 0;

struct Sample {
  std::atomic_uint64_t calls{0};
  std::atomic_uint32_t result{0};
  void Record(sl::Result value) {
    result.store(static_cast<uint32_t>(value));
    calls.fetch_add(1);
  }
};
inline Sample g_support;
inline Sample g_loaded;
inline Sample g_requirements;
inline Sample g_version;
inline std::atomic_bool g_loaded_value{false};
inline PFun_slIsFeatureSupported* g_real_support = nullptr;
inline PFun_slIsFeatureLoaded* g_real_loaded = nullptr;
inline PFun_slGetFeatureRequirements* g_real_requirements = nullptr;
inline PFun_slGetFeatureVersion* g_real_version = nullptr;

inline sl::Result HookedIsFeatureSupported(sl::Feature feature, const sl::AdapterInfo& adapter) {
  const auto result = g_real_support(feature, adapter);
  if (feature == sl::kFeatureDLSS_G) g_support.Record(result);
  return result;
}
inline sl::Result HookedIsFeatureLoaded(sl::Feature feature, bool& loaded) {
  const auto result = g_real_loaded(feature, loaded);
  if (feature == sl::kFeatureDLSS_G) {
    if (result == sl::Result::eOk) g_loaded_value.store(loaded);
    g_loaded.Record(result);
  }
  return result;
}
inline sl::Result HookedGetFeatureRequirements(sl::Feature feature, sl::FeatureRequirements& requirements) {
  // This is NOT NVSDK_NGX_FeatureRequirement. It has no MinHWArchitecture.
  // The host's structure and the original error are deliberately left alone.
  const auto result = g_real_requirements(feature, requirements);
  if (feature == sl::kFeatureDLSS_G) g_requirements.Record(result);
  return result;
}
inline sl::Result HookedGetFeatureVersion(sl::Feature feature, sl::FeatureVersion& version) {
  const auto result = g_real_version(feature, version);
  if (feature == sl::kFeatureDLSS_G) g_version.Record(result);
  return result;
}
static_assert(std::is_same_v<decltype(&HookedIsFeatureSupported), PFun_slIsFeatureSupported*>);
static_assert(std::is_same_v<decltype(&HookedIsFeatureLoaded), PFun_slIsFeatureLoaded*>);
static_assert(std::is_same_v<decltype(&HookedGetFeatureRequirements), PFun_slGetFeatureRequirements*>);
static_assert(std::is_same_v<decltype(&HookedGetFeatureVersion), PFun_slGetFeatureVersion*>);

inline const std::vector<hook::HookItem> kObserverHooks = {
    {"slIsFeatureSupported", reinterpret_cast<void**>(&g_real_support),
     reinterpret_cast<void*>(&HookedIsFeatureSupported)},
    {"slIsFeatureLoaded", reinterpret_cast<void**>(&g_real_loaded),
     reinterpret_cast<void*>(&HookedIsFeatureLoaded)},
    {"slGetFeatureRequirements", reinterpret_cast<void**>(&g_real_requirements),
     reinterpret_cast<void*>(&HookedGetFeatureRequirements)},
    {"slGetFeatureVersion", reinterpret_cast<void**>(&g_real_version),
     reinterpret_cast<void*>(&HookedGetFeatureVersion)},
};

inline void InstallObservers() {
  if (!g_enabled.load() || !architecture::NeedsBridge()) return;
  if (g_observers_installed.load() || g_observers_installing.test_and_set()) return;
  struct Guard {
    ~Guard() { g_observers_installing.clear(); }
  } guard;
  if (g_observers_installed.load()) return;
  HMODULE module = GetModuleHandleW(L"sl.interposer.dll");
  if (!module || !KnownStreamline(module)) return;
  for (const auto& [name, _storage, _replacement] : kObserverHooks)
    if (!GetProcAddress(module, name)) return;
  g_observers_installed.store(
      hook::Install(module, kObserverHooks, "Streamline"));
  if (!g_observers_installed.load()) {
    g_real_support = nullptr;
    g_real_loaded = nullptr;
    g_real_requirements = nullptr;
    g_real_version = nullptr;
  }
}

// Function signatures from Streamline source/core/sl.plugin-manager/internal.h.
// IParameters is opaque here: no private SystemCaps/PluginInfo memory is written.
using GatewayFn = void* (*)(const char*);
using LoadFn = bool (*)(sl::param::IParameters*, const char*, const char**);
using StartupFn = bool (*)(const char*, void*);
using ShutdownFn = void (*)();
struct PluginRuntime {
  HMODULE module = nullptr;
  ModuleVersion version;
  hook::AddressHook gateway_hook;
  hook::AddressHook load_hook;
  hook::AddressHook startup_hook;
  hook::AddressHook shutdown_hook;
  std::atomic_uint32_t load_calls{0};
  std::atomic_uint32_t startup_calls{0};
  std::atomic_uint32_t shutdown_calls{0};
  std::atomic_bool load_ok{false};
  std::atomic_bool startup_ok{false};
  std::atomic_bool running{false};
};
inline std::array<PluginRuntime, 8> g_plugins;
inline SRWLOCK g_plugin_lock = SRWLOCK_INIT;

inline void LifecyclePreflight() {
  if (g_shutting_down.load(std::memory_order_acquire)) return;
  // Called by the plugin manager after LoadLibrary returned, not by its load
  // notification. Patch cached NGX exports before the native plugin builds
  // supportedAdapters, required tags, and the common needNGX flag.
  try {
    ngx::internal::ProbeAdapterBeforeInit();
    InstallObservers();
    ngx::EnsureEntryHooks();
    if (architecture::ActiveProfile() && ngx::g_prepare_loaded_providers)
      ngx::g_prepare_loaded_providers();
  } catch (...) {
    Log("DLSS-G preflight failed", true);
  }
}

template <size_t I>
bool OnLoad(sl::param::IParameters* parameters, const char* loader_json, const char** plugin_json) {
  auto& plugin = g_plugins[I];
  const auto real = plugin.load_hook.Original<LoadFn>();
  if (!real) return false;
  if (g_shutting_down.load(std::memory_order_acquire)) return real(parameters, loader_json, plugin_json);
  LifecyclePreflight();
  ngx::internal::ScopedArchQuery scope(ngx::internal::g_bound_gpu.load());
  const bool result = real(parameters, loader_json, plugin_json);
  if (!result) plugin.running.store(false);
  plugin.load_ok.store(result);
  plugin.load_calls.fetch_add(1);
  if (!result) Log("DLSS-G plugin load failed", true);
  return result;
}

template <size_t I>
bool OnStartup(const char* json, void* device) {
  auto& plugin = g_plugins[I];
  const auto real = plugin.startup_hook.Original<StartupFn>();
  if (!real) return false;
  if (g_shutting_down.load(std::memory_order_acquire)) return real(json, device);
  LifecyclePreflight();
  ngx::internal::ScopedArchQuery scope(ngx::internal::g_bound_gpu.load());
  const bool result = real(json, device);
  plugin.startup_ok.store(result);
  plugin.running.store(result);
  plugin.startup_calls.fetch_add(1);
  Log(result ? "DLSS-G plugin started" : "DLSS-G plugin startup failed", !result);
  return result;
}

template <size_t I>
void OnShutdown() {
  auto& plugin = g_plugins[I];
  const auto real = plugin.shutdown_hook.Original<ShutdownFn>();
  if (real) real();
  plugin.running.store(false);
  plugin.shutdown_calls.fetch_add(1);
}

template <size_t I>
void* Gateway(const char* name) {
  auto& plugin = g_plugins[I];
  const auto real = plugin.gateway_hook.Original<GatewayFn>();
  if (!real) return nullptr;
  void* result = real(name);
  if (g_shutting_down.load(std::memory_order_acquire)) return result;
  if (!name || !result) return result;

  bool ok = true;
  if (std::strcmp(name, "slOnPluginLoad") == 0) {
    ok = hook::InstallAddress(plugin.load_hook, result, reinterpret_cast<void*>(&OnLoad<I>),
                              "DLSS-G slOnPluginLoad");
  } else if (std::strcmp(name, "slOnPluginStartup") == 0) {
    ok = hook::InstallAddress(plugin.startup_hook, result, reinterpret_cast<void*>(&OnStartup<I>),
                              "DLSS-G slOnPluginStartup");
  } else if (std::strcmp(name, "slOnPluginShutdown") == 0) {
    ok = hook::InstallAddress(plugin.shutdown_hook, result, reinterpret_cast<void*>(&OnShutdown<I>),
                              "DLSS-G slOnPluginShutdown");
  }
  if (!ok) Log("DLSS-G lifecycle hook failed", true);
  // Return the native entry; the detour is removed on unload.
  return result;
}
inline const std::array<GatewayFn, 8> kGateways = {
    &Gateway<0>, &Gateway<1>, &Gateway<2>, &Gateway<3>,
    &Gateway<4>, &Gateway<5>, &Gateway<6>, &Gateway<7>};

inline void ClearPluginRuntime(PluginRuntime& plugin) {
  hook::UninstallAddress(plugin.shutdown_hook);
  hook::UninstallAddress(plugin.startup_hook);
  hook::UninstallAddress(plugin.load_hook);
  hook::UninstallAddress(plugin.gateway_hook);
  plugin.module = nullptr;
  plugin.version = {};
  plugin.running.store(false);
}

inline FARPROC BindPlugin(HMODULE module, FARPROC original) {
  if (!g_enabled.load() || !architecture::NeedsBridge() || !KnownStreamline(module)) return original;
  const auto gateway = reinterpret_cast<GatewayFn>(original);

  // The feature probe can execute vendor code. Keep it outside our registry
  // lock so a resolver callback cannot create a lock cycle here.
  if (!gateway("slDLSSGSetOptions") || !gateway("slDLSSGGetState")) return original;

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
    return original;
  }
  Log("DLSS-G plugin limit reached", true);
  return original;
}
}  // namespace internal

// Reuses framecount's slInit detour without forcing OTA flags or removing any
// requested feature. SDK major 1 is not compatible with this signature.
inline sl::Result OnInit(const sl::Preferences& pref, uint64_t sdk, PFun_slInit* next) {
  if (!next) return sl::Result::eErrorNotInitialized;
  if (internal::g_shutting_down.load(std::memory_order_acquire) ||
      !g_enabled.load() || !architecture::NeedsBridge() || internal::g_init_depth != 0) return next(pref, sdk);
  ++internal::g_init_depth;
  struct Guard {
    ~Guard() { --internal::g_init_depth; }
  } guard;
  const bool v2 = (sdk >> 48) == 2 && (sdk & 0xffff) == (sl::kSDKVersion & 0xffff);
  internal::g_v2_seen.store(v2);
  if (v2) {
    ngx::BeforeInit();
    internal::InstallObservers();
  } else {
    Log("unsupported Streamline SDK", true);
  }
  const auto result = next(pref, sdk);
  internal::g_init_result.store(static_cast<uint32_t>(result));
  internal::g_init_calls.fetch_add(1);
  if (result != sl::Result::eOk) {
    std::stringstream message;
    message << "slInit failed (0x" << std::hex << static_cast<uint32_t>(result) << ')';
    Log(message.str(), true);
  }
  return result;
}

inline FARPROC Resolve(HMODULE module, const char* name, FARPROC original, const void* caller) {
  if (internal::g_shutting_down.load(std::memory_order_acquire) ||
      !g_enabled.load() || !architecture::NeedsBridge() || !original || !name ||
      reinterpret_cast<uintptr_t>(name) <= 0xffff)
    return original;
  // The addon's own Detours installer must always see the real export.
  MEMORY_BASIC_INFORMATION memory = {};
  if (caller && VirtualQuery(caller, &memory, sizeof(memory)) && memory.AllocationBase == internal::g_self)
    return original;

  if (std::strcmp(name, "slGetPluginFunction") == 0) {
    internal::BindPlugin(module, original);
    return original;
  }
  // slInit is already covered by framecount's entry detour.
  ngx::Resolve(module, name, original);
  return original;
}

inline void Initialize(HMODULE self) {
  internal::g_shutting_down.store(false, std::memory_order_release);
  internal::g_self = self;
  if (!g_enabled.load()) return;
  Log(std::string("architecture: ") + architecture::Name(architecture::g_configured.load()));
}

inline void Shutdown() {
  // Stop discovery before detaching addon-owned entry hooks.
  internal::g_shutting_down.store(true, std::memory_order_release);
  AcquireSRWLockExclusive(&internal::g_plugin_lock);
  for (auto it = internal::g_plugins.rbegin(); it != internal::g_plugins.rend(); ++it)
    internal::ClearPluginRuntime(*it);
  ReleaseSRWLockExclusive(&internal::g_plugin_lock);
  ngx::Shutdown();
  if (internal::g_observers_installed.exchange(false)) {
    hook::Uninstall(internal::kObserverHooks);
    internal::g_real_support = nullptr;
    internal::g_real_loaded = nullptr;
    internal::g_real_requirements = nullptr;
    internal::g_real_version = nullptr;
  }
  internal::g_self = nullptr;
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
  if (!g_enabled.load() || !profile || !profile->NeedsRetarget()) return;

  ModuleVersion plugin_version{};
  bool plugin_seen = false;
  bool plugin_running = false;
  if (TryAcquireSRWLockShared(&g_plugin_lock)) {
    for (const auto& plugin : g_plugins) {
      if (!plugin.module) continue;
      const bool running = plugin.running.load();
      if (!plugin_seen || running) {
        plugin_seen = true;
        plugin_running = running;
        plugin_version = plugin.version;
      }
      if (running) break;
    }
    ReleaseSRWLockShared(&g_plugin_lock);
  }
  if (plugin_seen) {
    ImGui::Text("DLSS-G: %u.%u.%u.%u (%s)", plugin_version.major, plugin_version.minor,
                plugin_version.build, plugin_version.revision,
                plugin_running ? "running" : "loaded");
  } else {
    ImGui::TextDisabled("DLSS-G: waiting");
  }

  const auto provider_status = GetProviderStatus();
  ModuleVersion provider_version{};
  bool provider_ready = false;
  std::string provider_error;
  if (TryAcquireSRWLockShared(&ampere::internal::g_lock)) {
    for (const auto& provider : ampere::internal::g_providers) {
      if (!provider.ready) continue;
      provider_ready = true;
      provider_version = ReadModuleVersion(provider.module);
      break;
    }
    if (!provider_ready && provider_status.blocked) {
      for (const auto& rejected : ampere::internal::g_rejected) {
        if (!ampere::internal::IsImageMapping(rejected.module) ||
            !ampere::internal::IsCurrent(rejected.module, rejected.identity)) continue;
        provider_error = rejected.reason;
        break;
      }
    }
    ReleaseSRWLockShared(&ampere::internal::g_lock);
  }
  if (provider_ready) {
    ImGui::Text("Provider: %u.%u.%u.%u", provider_version.major, provider_version.minor,
                provider_version.build, provider_version.revision);
  } else if (!provider_error.empty()) {
    ImGui::TextWrapped("Provider: blocked - %s", provider_error.c_str());
  } else {
    ImGui::TextDisabled("Provider: waiting");
  }

  auto& telemetry = ngx::g_telemetry;
  const auto creates = telemetry.creates_ok.load();
  const auto evaluates = telemetry.evaluates.load();
  if (creates && evaluates) {
    ImGui::TextUnformatted("Frame generation: active");
  } else if (creates) {
    ImGui::Text("Frame generation: created");
  } else {
    ImGui::TextDisabled("Frame generation: waiting");
  }
}
}  // namespace mfgunlock::ampere::caps
