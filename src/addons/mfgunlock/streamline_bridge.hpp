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
using provider::Log;
inline void (*g_on_interposer_loaded)() = nullptr;
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

inline const std::vector<hook::HookItem> kSupportHooks = {
    {"slIsFeatureSupported", reinterpret_cast<void**>(&g_real_support),
     reinterpret_cast<void*>(&HookedIsFeatureSupported)},
};

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

inline sl::Result HookedIsFeatureSupported(sl::Feature feature, const sl::AdapterInfo& adapter) {
  if (!g_real_support) return sl::Result::eErrorNotInitialized;
  if (g_shutting_down.load(std::memory_order_acquire) || !g_enabled.load() ||
      !architecture::NeedsBridge() || feature != sl::kFeatureDLSS_G)
    return g_real_support(feature, adapter);

  LifecyclePreflight();
  const auto gpu = ngx::internal::g_bound_gpu.load(std::memory_order_acquire);
  ngx::internal::ScopedArchQuery scope(gpu);
  const auto result = g_real_support(feature, adapter);

  static std::atomic_uint32_t last_result{0xffffffffu};
  const auto value = static_cast<uint32_t>(result);
  if (last_result.exchange(value, std::memory_order_relaxed) != value) {
    std::stringstream message;
    message << "Streamline DLSS-G support query returned 0x" << std::hex << value;
    Log(message.str(), result != sl::Result::eOk);
  }
  return result;
}

inline void InstallSupportHook() {
  if (!g_enabled.load() || !architecture::NeedsBridge()) return;
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
  ngx::internal::ScopedArchQuery scope(ngx::internal::g_bound_gpu.load());
  const bool result = real(parameters, loader_json, plugin_json);
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

inline FARPROC BindPlugin(HMODULE module, FARPROC original) {
  if (!g_enabled.load() || !architecture::NeedsBridge()) return original;
  const auto gateway = reinterpret_cast<GatewayFn>(original);

  // The feature probe can execute vendor code. Keep it outside our registry
  // lock so a resolver callback cannot create a lock cycle here.
  const bool modern = gateway("slDLSSGSetOptions") && gateway("slDLSSGGetState");
  const bool legacy = !modern && ReadModuleVersion(module).major == 1 &&
      ngx::internal::IsModuleNamed(module, L"sl.dlss_g.dll") &&
      gateway("slOnPluginLoad") && gateway("slOnPluginStartup");
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
      !g_enabled.load() || !architecture::NeedsBridge() || !original || !name ||
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
    if (g_on_interposer_loaded) g_on_interposer_loaded();
  }
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
  if (internal::g_support_hooked.exchange(false) &&
      hook::IsCurrent(internal::g_support_identity)) {
    hook::Uninstall(internal::kSupportHooks);
  }
  if (internal::g_legacy_init_hooked.exchange(false) &&
      hook::IsCurrent(internal::g_interposer_identity)) {
    hook::Uninstall(internal::kLegacyHooks);
  }
  internal::g_support_identity = {};
  internal::g_real_support = nullptr;
  internal::g_interposer_identity = {};
  internal::g_real_legacy_init = nullptr;
  internal::g_self = nullptr;
  g_on_interposer_loaded = nullptr;
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
