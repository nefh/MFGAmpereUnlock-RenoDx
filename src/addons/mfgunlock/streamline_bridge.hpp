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
#include "./compatibility_runtime.hpp"

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
inline std::atomic_bool g_init_observed{false};
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
  if (!module) return {};
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
// VERSIONINFO strings may carry a more precise build number than the fixed
// value. This reader is diagnostic only. All UTF-16 reads use memcpy so an
// unaligned resource buffer cannot introduce undefined behavior.
inline uint16_t VersionWord(std::span<const unsigned char> data, size_t at) {
  uint16_t value = 0;
  if (at <= data.size() && data.size() - at >= sizeof(value))
    std::memcpy(&value, data.data() + at, sizeof(value));
  return value;
}
struct VersionBlock {
  size_t end = 0, key = 0, key_end = 0, value = 0, value_bytes = 0, children = 0;
  uint16_t type = 0;
};
inline bool ReadVersionBlock(std::span<const unsigned char> data, size_t at,
                             size_t limit, VersionBlock& block) {
  if (limit > data.size() || at > limit || limit - at < 6 || (at & 3) != 0) return false;
  const size_t bytes = VersionWord(data, at);
  const size_t values = VersionWord(data, at + 2);
  block.type = VersionWord(data, at + 4);
  if (bytes < 8 || bytes > limit - at || block.type > 1) return false;
  block.end = at + bytes;
  block.key = at + 6;
  size_t cursor = block.key;
  while (cursor + 2 <= block.end && VersionWord(data, cursor) != 0) cursor += 2;
  if (cursor + 2 > block.end) return false;
  block.key_end = cursor;
  block.value = (cursor + 2 + 3) & ~size_t(3);
  block.value_bytes = values * (block.type == 1 ? 2 : 1);
  if (block.value > block.end || block.value_bytes > block.end - block.value) return false;
  block.children = (block.value + block.value_bytes + 3) & ~size_t(3);
  return true;
}
template <size_t N>
inline bool VersionKey(std::span<const unsigned char> data, const VersionBlock& block,
                       const char16_t (&key)[N]) {
  if (block.key_end - block.key != (N - 1) * 2) return false;
  for (size_t i = 0; i + 1 < N; ++i)
    if (VersionWord(data, block.key + i * 2) != key[i]) return false;
  return true;
}
inline bool ParseDisplayVersion(std::span<const unsigned char> data, ModuleVersion& result) {
  if (data.size() < 2 || (data.size() & 1) != 0) return false;
  size_t pos = 0;
  std::array<unsigned int, 4> parts{};
  const auto space = [](uint16_t ch) { return ch == ' ' || ch == '\t'; };
  for (size_t i = 0; i < parts.size(); ++i) {
    while (pos < data.size() && space(VersionWord(data, pos))) pos += 2;
    const size_t start = pos;
    while (pos < data.size()) {
      const auto ch = VersionWord(data, pos);
      if (ch < '0' || ch > '9') break;
      parts[i] = parts[i] * 10 + ch - '0';
      if (parts[i] > 0xffff) return false;
      pos += 2;
    }
    if (pos == start) return false;
    while (pos < data.size() && space(VersionWord(data, pos))) pos += 2;
    if (i + 1 != parts.size()) {
      if (pos >= data.size() || (VersionWord(data, pos) != '.' && VersionWord(data, pos) != ','))
        return false;
      pos += 2;
    }
  }
  if (pos >= data.size() || VersionWord(data, pos) != 0) return false;
  for (; pos < data.size(); pos += 2) if (VersionWord(data, pos) != 0) return false;
  result = {parts[0], parts[1], parts[2], parts[3]};
  return true;
}
inline bool FindDisplayVersion(std::span<const unsigned char> data, size_t at, size_t limit,
                               unsigned depth, bool string_info, unsigned& budget,
                               bool& found, ModuleVersion& result) {
  if (depth > 3 || budget == 0) return false;
  --budget;
  VersionBlock block;
  if (!ReadVersionBlock(data, at, limit, block)) return false;
  if (depth == 0 && !VersionKey(data, block, u"VS_VERSION_INFO")) return false;
  if (depth == 1) string_info = VersionKey(data, block, u"StringFileInfo");
  if (depth == 3 && string_info && VersionKey(data, block, u"FileVersion")) {
    ModuleVersion candidate;
    if (block.type != 1 || !ParseDisplayVersion(data.subspan(block.value, block.value_bytes), candidate))
      return false;
    if (found && (result.major != candidate.major || result.minor != candidate.minor ||
                  result.build != candidate.build || result.revision != candidate.revision)) return false;
    result = candidate;
    found = true;
  }
  size_t cursor = block.children;
  while (cursor < block.end) {
    if (block.end - cursor < 6) {
      for (; cursor < block.end; ++cursor) if (data[cursor] != 0) return false;
      break;
    }
    if (!FindDisplayVersion(data, cursor, block.end, depth + 1, string_info, budget, found, result))
      return false;
    cursor = (cursor + VersionWord(data, cursor) + 3) & ~size_t(3);
  }
  return true;
}
inline ModuleVersion ReadModuleDisplayVersion(HMODULE module) {
  if (!module) return {};
  HRSRC resource = FindResourceW(module, MAKEINTRESOURCEW(VS_VERSION_INFO), MAKEINTRESOURCEW(16));
  if (!resource) return ReadModuleVersion(module);
  const DWORD bytes = SizeofResource(module, resource);
  HGLOBAL loaded = LoadResource(module, resource);
  const auto* data = loaded ? static_cast<const unsigned char*>(LockResource(loaded)) : nullptr;
  if (data && bytes >= 8) {
    ModuleVersion result;
    unsigned budget = 256;
    bool found = false;
    if (FindDisplayVersion({data, bytes}, 0, bytes, 0, false, budget, found, result) && found)
      return result;
  }
  return ReadModuleVersion(module);
}

enum class InterposerAbi { kUnknown, kLegacy, kModern };

inline bool OwnExecutableExport(HMODULE module, const char* name) {
  const auto address = GetProcAddress(module, name);
  if (!address) return false;
  MEMORY_BASIC_INFORMATION memory{};
  if (!VirtualQuery(reinterpret_cast<const void*>(address), &memory, sizeof(memory)) ||
      memory.State != MEM_COMMIT || memory.Type != MEM_IMAGE ||
      memory.AllocationBase != module ||
      (memory.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0) return false;
  const auto protection = memory.Protect & 0xff;
  return protection == PAGE_EXECUTE || protection == PAGE_EXECUTE_READ ||
         protection == PAGE_EXECUTE_READWRITE || protection == PAGE_EXECUTE_WRITECOPY;
}

inline compatibility::InterposerContract GetInterposerContract(HMODULE module, bool require_common = true) {
  if (!module || !ngx::internal::IsModuleNamed(module, L"sl.interposer.dll")) {
    compatibility::InterposerContract missing;
    missing.admission = {compatibility::FeatureState::kUnavailable,
                         "sl.interposer is missing or has an unexpected identity"};
    return missing;
  }
  constexpr std::array names{
      "slInit", "slShutdown", "slSetTag", "slEvaluateFeature",
      "slGetFeatureFunction", "slGetFeatureRequirements", "slGetFeatureVersion",
      "slGetFeatureSettings", "slSetFeatureConstants"};
  std::vector<compatibility::ExportSymbol> symbols;
  symbols.reserve(names.size());
  for (const char* name : names) {
    if (GetProcAddress(module, name))
      symbols.push_back({name, OwnExecutableExport(module, name)});
  }
  hook::ModuleIdentity identity{};
  const bool current = hook::CaptureModule(module, identity) && hook::IsCurrent(identity);
  return compatibility::AssessInterposer(symbols,
      !require_common || GetModuleHandleW(L"sl.common.dll") != nullptr, current);
}

inline InterposerAbi GetInterposerAbi(HMODULE module, bool require_common = true) {
  const auto contract = GetInterposerContract(module, require_common);
  if (!contract.admission.Available()) return InterposerAbi::kUnknown;
  if (contract.typed_abi == compatibility::TypedInterposerAbi::kLegacy1)
    return InterposerAbi::kLegacy;
  if (contract.typed_abi == compatibility::TypedInterposerAbi::kModern2)
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

// Legacy public ABI: bool result and optional uint32_t adapter mask.
using LegacySupportFn = bool (*)(sl::Feature, uint32_t*);
using LegacyEnabledFn = bool (*)(sl::Feature);
using LegacySetEnabledFn = bool (*)(sl::Feature, bool);
using LegacySettingsFn = bool (*)(sl::Feature, const void*, void*);
inline LegacySupportFn g_real_legacy_support = nullptr;
inline LegacyEnabledFn g_real_legacy_enabled = nullptr;
inline LegacySetEnabledFn g_real_legacy_set_enabled = nullptr;
inline LegacySettingsFn g_real_legacy_settings = nullptr;
inline std::array<std::atomic_uint32_t, 16> g_legacy_query_traces{};
inline bool TraceLegacyQuery(unsigned api, sl::Feature feature) {
  const auto id = static_cast<uint32_t>(feature);
  const unsigned slot = id == 0 ? 0 : id == 1000 ? 1 : id == 3 ? 2 : 3;
  return g_legacy_query_traces[api * 4 + slot].fetch_add(1) < 16;
}
inline bool HookedLegacySupport(sl::Feature feature, uint32_t* adapters) {
  const auto real = g_real_legacy_support;
  if (!real) return false;
  const bool result = real(feature, adapters);
  if (TraceLegacyQuery(0, feature)) ngx::internal::DiscoveryTrace([&](auto& message) {
    message << "SL_SUPPORT feature=" << static_cast<uint32_t>(feature)
            << " result=" << result << " mask_ptr=" << adapters;
    // On failure the output may be uninitialized. Never read it then.
    if (result && adapters) message << " mask=0x" << std::hex << *adapters;
  });
  return result;
}
inline bool HookedLegacyEnabled(sl::Feature feature) {
  const auto real = g_real_legacy_enabled;
  if (!real) return false;
  const bool result = real(feature);
  if (TraceLegacyQuery(1, feature)) ngx::internal::DiscoveryTrace([&](auto& message) {
    message << "SL_ENABLED feature=" << static_cast<uint32_t>(feature) << " result=" << result;
  });
  return result;
}
inline bool HookedLegacySettings(sl::Feature feature, const void* settings_in, void* settings_out) {
  const auto real = g_real_legacy_settings;
  if (!real) return false;
  const bool result = real(feature, settings_in, settings_out);
  if (TraceLegacyQuery(2, feature)) ngx::internal::DiscoveryTrace([&](auto& message) {
    message << "SL_SETTINGS feature=" << static_cast<uint32_t>(feature) << " result=" << result;
  });
  return result;
}
inline bool HookedLegacySetEnabled(sl::Feature feature, bool enabled) {
  const auto real = g_real_legacy_set_enabled;
  if (!real) return false;
  const bool result = real(feature, enabled);
  if (TraceLegacyQuery(3, feature)) ngx::internal::DiscoveryTrace([&](auto& message) {
    message << "SL_SET_ENABLED feature=" << static_cast<uint32_t>(feature)
            << " requested=" << enabled << " result=" << result;
  });
  return result;
}
inline const std::vector<hook::HookItem> kLegacyDiscoveryHooks = {
    {"slSetFeatureEnabled", reinterpret_cast<void**>(&g_real_legacy_set_enabled),
     reinterpret_cast<void*>(&HookedLegacySetEnabled)},
    {"slIsFeatureSupported", reinterpret_cast<void**>(&g_real_legacy_support),
     reinterpret_cast<void*>(&HookedLegacySupport)},
    {"slIsFeatureEnabled", reinterpret_cast<void**>(&g_real_legacy_enabled),
     reinterpret_cast<void*>(&HookedLegacyEnabled)},
    {"slGetFeatureSettings", reinterpret_cast<void**>(&g_real_legacy_settings),
     reinterpret_cast<void*>(&HookedLegacySettings)},
};

inline void RefreshPluginLifetimes();

inline void InstallLegacyInitHook() {
  if (!g_enabled.load() || !architecture::NeedsBridge()) return;
  if (g_legacy_installing.test_and_set()) return;
  struct Guard {
    ~Guard() { g_legacy_installing.clear(); }
  } guard;

  if (g_interposer_identity.module && !hook::IsCurrent(g_interposer_identity)) {
    g_interposer_identity = {};
    g_real_legacy_init = nullptr;
    for (const auto& entry : kLegacyDiscoveryHooks) *std::get<1>(entry) = nullptr;
    g_legacy_init_hooked.store(false, std::memory_order_release);
  }
  HMODULE module = GetModuleHandleW(L"sl.interposer.dll");
  if (!module || GetInterposerAbi(module) != InterposerAbi::kLegacy) return;
  if (g_interposer_identity.module && g_interposer_identity.module != module) return;
  if (!hook::CaptureModule(module, g_interposer_identity)) return;
  std::vector<hook::HookItem> discovery_hooks;
  for (const auto& entry : kLegacyDiscoveryHooks) {
    const auto& [name, storage, replacement] = entry;
    (void)replacement;
    if (!*storage && OwnExecutableExport(module, name)) discovery_hooks.push_back(entry);
  }
  if (!discovery_hooks.empty()) hook::Install(module, discovery_hooks, "Legacy DLSS discovery D1");
  if (g_legacy_init_hooked.load(std::memory_order_acquire)) return;
  if (hook::Install(module, kLegacyHooks, "Streamline 1.x"))
    g_legacy_init_hooked.store(true, std::memory_order_release);
}
inline bool HookedLegacyInit(const LegacyPreferences& preferences, int application_id) {
  if (!g_real_legacy_init) return false;
  g_init_observed.store(true, std::memory_order_release);
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
enum class PluginRole { kFg, kSr, kCommon };
inline const char* PluginLabel(PluginRole role) {
  return role == PluginRole::kFg ? "sl.dlss_g" : role == PluginRole::kSr ? "sl.dlss" : "sl.common";
}
struct PluginRuntime {
  HMODULE module = nullptr;
  PluginRole role = PluginRole::kFg;
  ModuleVersion version;
  compatibility::TypedInterposerAbi typed_abi = compatibility::TypedInterposerAbi::kNone;
  hook::AddressHook gateway_hook;
  hook::AddressHook load_hook;
  hook::AddressHook startup_hook;
};
inline std::array<PluginRuntime, 8> g_plugins;
inline SRWLOCK g_plugin_lock = SRWLOCK_INIT;
inline std::atomic<HMODULE> g_last_unqualified_plugin{nullptr};

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
  // Vulkan physicalDevice takes precedence over a simultaneously supplied LUID.
  // Requirements records the instance-dispatched identity. Without that evidence,
  // leave native support untouched; never match a different DXGI adapter.
  if (adapter.vkPhysicalDevice) {
    const auto id = vulkannative::KnownPhysical(adapter.vkPhysicalDevice);
    return id.valid && id.vendor == architecture::kNvidiaVendorId &&
        id.luid.LowPart == ngx::internal::g_luid_low.load(std::memory_order_acquire) &&
        static_cast<uint32_t>(id.luid.HighPart) == ngx::internal::g_luid_high.load(std::memory_order_acquire);
  }
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

  // A physical handle may be reused after instance recreation. Only a fresh
  // nested requirements observation in THIS support call can relax admission.
  if (adapter.vkPhysicalDevice) vulkannative::ResetIdentity();
  LifecyclePreflight();
  RefreshPluginLifetimes();
  const auto gpu = SupportAdapterMatchesBoundGpu(adapter)
      ? ngx::internal::g_bound_gpu.load(std::memory_order_acquire) : nullptr;
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
      g_enabled.load(std::memory_order_relaxed),
      ngx::internal::g_bound_gpu.load(std::memory_order_acquire) != nullptr,
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

  if (g_diagnostic_identity.module && !hook::IsCurrent(g_diagnostic_identity)) {
    g_diagnostic_identity = {};
    g_real_requirements = nullptr;
    g_real_loaded = nullptr;
    g_real_set_d3d_device = nullptr;
    g_diagnostic_hooked.store(false, std::memory_order_release);
  }
  HMODULE module = GetModuleHandleW(L"sl.interposer.dll");
  if (!module || GetInterposerAbi(module) != InterposerAbi::kModern) return;
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

  if (g_support_identity.module && !hook::IsCurrent(g_support_identity)) {
    g_support_identity = {};
    g_real_support = nullptr;
    g_support_hooked.store(false, std::memory_order_release);
  }
  HMODULE module = GetModuleHandleW(L"sl.interposer.dll");
  if (!module || GetInterposerAbi(module) != InterposerAbi::kModern) return;
  if (g_support_hooked.load(std::memory_order_acquire)) return;
  if (g_support_identity.module && g_support_identity.module != module) return;
  if (GetProcAddress(module, "slIsFeatureSupported") == nullptr ||
      !hook::CaptureModule(module, g_support_identity)) return;
  if (hook::Install(module, kSupportHooks, "Streamline support"))
    g_support_hooked.store(true, std::memory_order_release);
}

inline std::string DiscoveryJson(const char* text) {
  if (!text) return "NULL";
  std::string output;
  output.reserve(512);
  constexpr size_t limit = 4096;
  while (output.size() < limit) {
    const auto origin = reinterpret_cast<uintptr_t>(text);
    if (origin > UINTPTR_MAX - output.size()) return output + "<UNREADABLE>";
    const auto at = origin + output.size();
    MEMORY_BASIC_INFORMATION memory{};
    if (!VirtualQuery(reinterpret_cast<const void*>(at), &memory, sizeof(memory)) ||
        memory.State != MEM_COMMIT || (memory.Protect & (PAGE_GUARD | PAGE_NOACCESS)))
      return output + "<UNREADABLE>";
    const DWORD protection = memory.Protect & 0xff;
    if (protection != PAGE_READONLY && protection != PAGE_READWRITE &&
        protection != PAGE_WRITECOPY && protection != PAGE_EXECUTE_READ &&
        protection != PAGE_EXECUTE_READWRITE && protection != PAGE_EXECUTE_WRITECOPY)
      return output + "<UNREADABLE>";
    const auto base = reinterpret_cast<uintptr_t>(memory.BaseAddress);
    if (at < base || at - base >= memory.RegionSize) return output + "<UNREADABLE>";
    const size_t count = std::min(limit - output.size(), memory.RegionSize - (at - base));
    for (size_t i = 0; i < count; ++i) {
      const char c = text[output.size()];
      if (!c) return output;
      output.push_back(c == '\n' || c == '\r' || c == '\t' ? ' ' : c);
    }
  }
  return output + "<TRUNCATED>";
}

template <size_t I>
bool OnLoad(sl::param::IParameters* parameters, const char* loader_json, const char** plugin_json) {
  auto& plugin = g_plugins[I];
  const auto real = plugin.load_hook.Original<LoadFn>();
  if (!real) return false;
  if (g_shutting_down.load(std::memory_order_acquire)) return real(parameters, loader_json, plugin_json);
  if (plugin.role != PluginRole::kFg) {
    ngx::internal::DiscoveryTrace([&](auto& message) {
      message << "PLUGIN_LOAD_BEGIN plugin=" << PluginLabel(plugin.role)
              << " arch_scope=" << ngx::internal::g_arch_scope
              << " input=" << DiscoveryJson(loader_json);
    });
    const bool result = real(parameters, loader_json, plugin_json);
    ngx::internal::DiscoveryTrace([&](auto& message) {
      message << "PLUGIN_LOAD_END plugin=" << PluginLabel(plugin.role) << " result=" << result;
      if (result && plugin_json) message << " json=" << DiscoveryJson(*plugin_json);
    });
    return result;
  }
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
  if (plugin.role != PluginRole::kFg) {
    ngx::internal::DiscoveryTrace([&](auto& message) {
      message << "PLUGIN_STARTUP_BEGIN plugin=" << PluginLabel(plugin.role) << " device=" << device
              << " arch_scope=" << ngx::internal::g_arch_scope
              << " input=" << DiscoveryJson(json);
    });
    const bool result = real(json, device);
    ngx::internal::DiscoveryTrace([&](auto& message) {
      message << "PLUGIN_STARTUP_END plugin=" << PluginLabel(plugin.role) << " result=" << result;
    });
    return result;
  }
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
  ngx::internal::DiscoveryLastError native_error;
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
  plugin.role = PluginRole::kFg;
  plugin.version = {};
  plugin.typed_abi = compatibility::TypedInterposerAbi::kNone;
}

inline void RefreshPluginLifetimes() {
  std::array<HMODULE, 8> unloaded{};
  size_t unloaded_count = 0;
  AcquireSRWLockExclusive(&g_plugin_lock);
  for (auto& plugin : g_plugins) {
    if (!plugin.module || hook::IsCurrent(plugin.gateway_hook.identity)) continue;
    if (plugin.role == PluginRole::kFg && unloaded_count < unloaded.size())
      unloaded[unloaded_count++] = plugin.module;
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
  const auto interposer = GetModuleHandleW(L"sl.interposer.dll");
  const bool sr = ngx::internal::IsModuleNamed(module, L"sl.dlss.dll");
  const bool common = ngx::internal::IsModuleNamed(module, L"sl.common.dll");
  if (sr || common) {
    // Passive observer of the existing shared plugin lifecycle ABI. Do not
    // invoke the gateway to probe for features, prepare FG, or alter JSON.
    hook::ModuleIdentity identity{};
    if (GetInterposerAbi(interposer, false) == InterposerAbi::kUnknown ||
        !OwnExecutableExport(module, "slGetPluginFunction") ||
        GetProcAddress(module, "slGetPluginFunction") != original ||
        !hook::CaptureModule(module, identity) || !hook::IsCurrent(identity)) return original;
    AcquireSRWLockExclusive(&g_plugin_lock);
    struct Unlock { ~Unlock() { ReleaseSRWLockExclusive(&g_plugin_lock); } } unlock;
    for (const auto& plugin : g_plugins)
      if (plugin.module == module) return original;
    for (size_t i = 0; i < g_plugins.size(); ++i) {
      auto& plugin = g_plugins[i];
      if (plugin.module) continue;
      plugin.module = module;
      plugin.role = sr ? PluginRole::kSr : PluginRole::kCommon;
      if (!hook::InstallAddress(plugin.gateway_hook, reinterpret_cast<void*>(original),
                                reinterpret_cast<void*>(kGateways[i]), "DLSS discovery lifecycle D1")) {
        plugin.module = nullptr;
        plugin.role = PluginRole::kFg;
        return original;
      }
      ngx::internal::DiscoveryTrace([&](auto& message) {
        message << "PLUGIN_OBSERVER plugin=" << PluginLabel(plugin.role) << " module=" << module;
      });
      return original;
    }
    return original;
  }
  // An unrelated plugin is not a failed DLSS-G gateway.
  if (!ngx::internal::IsModuleNamed(module, L"sl.dlss_g.dll")) return original;
  const auto parent_abi = GetInterposerAbi(interposer);
  compatibility::PluginGatewayEvidence evidence{};
  evidence.named_dlssg = ngx::internal::IsModuleNamed(module, L"sl.dlss_g.dll");
  evidence.own_code_gateway = OwnExecutableExport(module, "slGetPluginFunction") &&
      GetProcAddress(module, "slGetPluginFunction") == original;
  hook::ModuleIdentity module_identity{};
  evidence.current_image = hook::CaptureModule(module, module_identity) &&
      hook::IsCurrent(module_identity);
  evidence.parent_abi_qualified = parent_abi != InterposerAbi::kUnknown;
  const auto lookup = compatibility::GatewayLookup(evidence);
  if (!lookup.Available()) {
    if (g_last_unqualified_plugin.exchange(module, std::memory_order_acq_rel) != module)
      Log(std::string("DLSS-G gateway left native: ") + lookup.reason, true);
    return original;
  }
  // The same live binding needs no further vendor calls or bind notifications.
  // Run contract/current-image checks first; missing dependencies must not be
  // hidden by this fast path. Do not hold our registry lock across vendor code.
  {
    AcquireSRWLockShared(&g_plugin_lock);
    bool bound = false;
    for (const auto& plugin : g_plugins) {
      if (plugin.module == module &&
          plugin.gateway_hook.Targets(reinterpret_cast<void*>(original)) &&
          hook::IsCurrent(plugin.gateway_hook.identity) &&
          plugin.typed_abi == (parent_abi == InterposerAbi::kModern
              ? compatibility::TypedInterposerAbi::kModern2
              : compatibility::TypedInterposerAbi::kLegacy1)) {
        bound = true;
        break;
      }
    }
    ReleaseSRWLockShared(&g_plugin_lock);
    if (bound) return original;
  }
  const auto version = ReadModuleDisplayVersion(module);
  // Only a typed, own-code gateway can be invoked to inspect returned
  // functions. An export name by itself is never a calling convention witness.
  const auto gateway = reinterpret_cast<GatewayFn>(original);
  {
    std::stringstream message;
    message << "TuringDiag slGetPluginFunction discovered module=" << module
            << " version=" << version.major << '.' << version.minor << '.'
            << version.build << '.' << version.revision;
    Log(message.str());
  }

  // The feature probe can execute vendor code. Keep it outside our registry
  // lock so a resolver callback cannot create a lock cycle here.
  evidence.set_options = gateway("slDLSSGSetOptions") != nullptr;
  evidence.get_state = gateway("slDLSSGGetState") != nullptr;
  if (parent_abi == InterposerAbi::kLegacy) {
    evidence.on_load = gateway("slOnPluginLoad") != nullptr;
    evidence.on_startup = gateway("slOnPluginStartup") != nullptr;
  }
  const auto functions = compatibility::PluginFunctions(evidence,
      parent_abi == InterposerAbi::kModern ? compatibility::TypedInterposerAbi::kModern2
                                           : compatibility::TypedInterposerAbi::kLegacy1);
  const bool modern = parent_abi == InterposerAbi::kModern && functions.Available();
  const bool legacy = parent_abi == InterposerAbi::kLegacy &&
      functions.state == compatibility::FeatureState::kDegraded;
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
    plugin.role = PluginRole::kFg;
    plugin.version = version;
    plugin.typed_abi = modern ? compatibility::TypedInterposerAbi::kModern2
                              : compatibility::TypedInterposerAbi::kLegacy1;
    if (!hook::InstallAddress(plugin.gateway_hook, reinterpret_cast<void*>(original),
                              reinterpret_cast<void*>(kGateways[i]),
                              "DLSS-G function table")) {
      plugin.module = nullptr;
      plugin.version = {};
      plugin.typed_abi = compatibility::TypedInterposerAbi::kNone;
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
  if (!module || !ngx::internal::IsModuleNamed(module, L"sl.interposer.dll") ||
      // slInit can precede sl.common loading: do not reintroduce the late-hook
      // regression by requiring that dependency for the init hook itself.
      internal::GetInterposerAbi(module, false) != internal::InterposerAbi::kModern ||
      !internal::OwnExecutableExport(module, "slInit")) return false;
  hook::ModuleIdentity identity{};
  return hook::CaptureModule(module, identity) && hook::IsCurrent(identity);
}

inline bool FullModernContractReady(HMODULE module) {
  return internal::GetInterposerAbi(module) == internal::InterposerAbi::kModern;
}

// Reuses framecount's slInit detour without forcing OTA flags, removing any
// requested feature, or interpreting the host's Preferences layout.
inline sl::Result OnInit(const sl::Preferences& pref, uint64_t sdk, PFun_slInit* next) {
  if (!next) return sl::Result::eErrorNotInitialized;
  internal::g_init_observed.store(true, std::memory_order_release);

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
  internal::g_init_observed.store(false, std::memory_order_release);
  internal::g_last_unqualified_plugin.store(nullptr, std::memory_order_release);
  internal::g_d3d12_device_observed.store(false, std::memory_order_release);
  internal::g_set_device_active.store(false, std::memory_order_release);
  internal::g_set_device_completed.store(false, std::memory_order_release);
  internal::g_self = self;
  if (!g_enabled.load()) return;
  Log("RuntimeRepairR5 DLSSDISC_D1 active");
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
  const bool legacy_init_hooked = internal::g_legacy_init_hooked.exchange(false);
  if (hook::IsCurrent(internal::g_interposer_identity)) {
    // Query observers may have installed even when the separate init detour failed.
    hook::Uninstall(internal::kLegacyDiscoveryHooks);
    if (legacy_init_hooked) hook::Uninstall(internal::kLegacyHooks);
  }
  internal::g_diagnostic_identity = {};
  internal::g_real_requirements = nullptr;
  internal::g_real_loaded = nullptr;
  internal::g_real_set_d3d_device = nullptr;
  internal::g_support_identity = {};
  internal::g_real_support = nullptr;
  internal::g_interposer_identity = {};
  internal::g_real_legacy_init = nullptr;
  internal::g_real_legacy_support = nullptr;
  internal::g_real_legacy_enabled = nullptr;
  internal::g_real_legacy_set_enabled = nullptr;
  internal::g_real_legacy_settings = nullptr;
  internal::g_self = nullptr;
  internal::g_last_unqualified_plugin.store(nullptr, std::memory_order_release);
  g_on_interposer_loaded = nullptr;
  g_on_dlssg_plugin_bound = nullptr;
  g_on_dlssg_plugin_unloaded = nullptr;
}

inline void NotifyD3D12DeviceObserved() {
  if (!internal::g_d3d12_device_observed.exchange(true, std::memory_order_acq_rel))
    Log("TuringDiag ReShade observed D3D12 device creation");
}

inline void NotifyD3D12DeviceRecreated() {
  internal::g_d3d12_device_observed.store(true, std::memory_order_release);
  internal::g_set_device_active.store(false, std::memory_order_release);
  internal::g_set_device_completed.store(false, std::memory_order_release);
  Log("TuringDiag primary D3D12 device generation changed; device-owned evidence invalidated");
}

inline void Draw() {
  using namespace internal;
  ImGui::Separator();
  const auto configured = architecture::g_configured.load();
  const auto active = architecture::g_active.load();
  ImGui::Text("Architecture: %s | active %s", architecture::Name(configured),
              architecture::Name(active));

  auto draw_module = [](const char* label, const wchar_t* name) {
    if (HMODULE module = GetModuleHandleW(name)) {
      const auto version = ReadModuleDisplayVersion(module);
      ImGui::Text("%s: %u.%u.%u.%u | LOADED", label, version.major, version.minor,
                  version.build, version.revision);
    } else {
      ImGui::TextDisabled("%s: NOT_LOADED", label);
    }
  };
  if (HMODULE interposer = GetModuleHandleW(L"sl.interposer.dll")) {
    const auto version = ReadModuleDisplayVersion(interposer);
    const auto abi = GetInterposerAbi(interposer);
    ImGui::Text("Streamline: %u.%u.%u.%u | %s", version.major, version.minor,
        version.build, version.revision,
        abi == InterposerAbi::kModern ? "MODERN" :
        abi == InterposerAbi::kLegacy ? "LEGACY" : "CONTRACT_UNAVAILABLE");
  } else {
    ImGui::TextDisabled("Streamline: NOT_LOADED");
  }
  ModuleVersion plugin_version{};
  compatibility::TypedInterposerAbi plugin_abi = compatibility::TypedInterposerAbi::kNone;
  bool plugin_bound = false;
  if (TryAcquireSRWLockShared(&g_plugin_lock)) {
    for (const auto& plugin : g_plugins) {
      if (plugin.role != PluginRole::kFg || !plugin.module ||
          !hook::IsCurrent(plugin.gateway_hook.identity)) continue;
      plugin_version = plugin.version;
      plugin_abi = plugin.typed_abi;
      plugin_bound = true;
      break;
    }
    ReleaseSRWLockShared(&g_plugin_lock);
  }
  if (plugin_bound) {
    ImGui::Text("DLSS-G plugin: %u.%u.%u.%u | %s", plugin_version.major,
        plugin_version.minor, plugin_version.build, plugin_version.revision,
        plugin_abi == compatibility::TypedInterposerAbi::kLegacy1 ? "LEGACY" : "MODERN");
  } else {
    draw_module("DLSS-G plugin", L"sl.dlss_g.dll");
  }
  draw_module("DLSS SR plugin", L"sl.dlss.dll");
  draw_module("DLSS SR", L"nvngx_dlss.dll");

  bool locked = TryAcquireSRWLockShared(&provider::internal::g_lock) != 0;
  HMODULE provider_module = nullptr;
  bool ready = false;
  if (locked) {
    for (const auto& item : provider::internal::g_providers) {
      if (!item.ready || !provider::internal::IsImageMapping(item.module)) continue;
      provider_module = item.module;
      ready = true;
      break;
    }
  }
  if (!provider_module) provider_module = GetModuleHandleW(L"nvngx_dlssg.dll");
  if (provider_module) {
    const auto version = ReadModuleDisplayVersion(provider_module);
    ImGui::Text("Provider: %u.%u.%u.%u | %s", version.major, version.minor,
        version.build, version.revision, ready ? "PREPARED" : "LOADED");
  } else {
    ImGui::TextDisabled("Provider: NOT_LOADED");
  }
  if (locked) ReleaseSRWLockShared(&provider::internal::g_lock);

  auto& status = ngx::g_status;
  ImGui::Text("FG: %s | NGX: %s",
      status.feature_active.load() ? "ACTIVE" : status.feature_created.load() ? "CREATED" :
      status.capabilities_seen.load() ? (status.available.load() ? "AVAILABLE" : "UNAVAILABLE") :
      "UNOBSERVED", !status.capabilities_seen.load() ? "UNOBSERVED" :
      status.vulkan_capabilities.load() ? "VULKAN" : "D3D12");
}

}  // namespace mfgunlock::streamline
