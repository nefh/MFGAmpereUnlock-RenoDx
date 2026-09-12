/*
 * NGX/NVAPI bridge for native DLSS-G.
 * SPDX-License-Identifier: MIT
 *
 * Binds the game adapter, scopes the architecture exposure to DLSS-G
 * capability queries, and observes the native Create/Evaluate/Release path.
 */
#pragma once

#include <string>
#include <vector>
#include <windows.h>
#include <d3d12.h>
#include <dxgi.h>
#include <nvapi.h>
#include <nvsdk_ngx.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <cwchar>
#include <mutex>
#include <sstream>
#include <type_traits>

#include "./ampere.hpp"
#include "./ampere_policy.hpp"
#include "./ngx_hook.hpp"

namespace mfgunlock::ampere::ngx {

// Set by addon.cpp. This deliberately reuses the addon's existing provider
// inventory and maintenance lock instead of maintaining a second module model.
inline void (*g_prepare_loaded_providers)() = nullptr;
// Zero leaves capability values alone. The addon shares this readiness decision
// with its existing frame-count path.
inline unsigned int (*g_capability_limit)() = nullptr;

struct Status {
  std::atomic_bool capabilities_seen{false};
  std::atomic_bool vulkan_capabilities{false};
  std::atomic_bool available{false};
  std::atomic_uint32_t capability_max{0};
  std::atomic_bool feature_created{false};
  std::atomic_bool feature_active{false};
};
inline Status g_status;
inline std::atomic_bool g_shutting_down{false};

namespace internal {

// Original NVAPI pointers are kept separately from the entry hook. Architecture
// detection must never consult our exposed architecture.
using QueryFn = void*(__cdecl*)(NvU32);
struct NvapiApi {
  decltype(&NvAPI_EnumPhysicalGPUs) enumerate = nullptr;
  decltype(&NvAPI_GPU_GetAdapterIdFromPhysicalGpu) adapter_id = nullptr;
  decltype(&NvAPI_GPU_GetArchInfo) architecture = nullptr;
  HMODULE module = nullptr;
  bool ready = false;
};
inline NvapiApi& GetNvapi() {
  static NvapiApi api;
  static std::once_flag initialize;
  std::call_once(initialize, [] {
    api.module = LoadLibraryExW(L"nvapi64.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!api.module) return;
    const auto query = reinterpret_cast<QueryFn>(GetProcAddress(api.module, "nvapi_QueryInterface"));
    if (!query) return;
    const auto init = reinterpret_cast<decltype(&NvAPI_Initialize)>(query(0x0150e828));
    api.enumerate = reinterpret_cast<decltype(api.enumerate)>(query(0xe5ac921f));
    api.adapter_id = reinterpret_cast<decltype(api.adapter_id)>(query(0x0ff07fde));
    api.architecture = reinterpret_cast<decltype(api.architecture)>(query(0xd8265d24));
    api.ready = init && api.enumerate && api.adapter_id && api.architecture && init() == NVAPI_OK;
  });
  return api;
}
inline std::atomic<NvPhysicalGpuHandle> g_bound_gpu{nullptr};
inline std::atomic_uint32_t g_luid_low{0};
inline std::atomic_uint32_t g_luid_high{0};
inline thread_local NvPhysicalGpuHandle g_arch_scope = nullptr;
struct ScopedArchQuery {
  NvPhysicalGpuHandle previous;

  explicit ScopedArchQuery(NvPhysicalGpuHandle gpu) : previous(g_arch_scope) {
    g_arch_scope = gpu;
  }
  ~ScopedArchQuery() { g_arch_scope = previous; }
  ScopedArchQuery(const ScopedArchQuery&) = delete;
  ScopedArchQuery& operator=(const ScopedArchQuery&) = delete;
};
inline hook::AddressHook g_arch_hook;
inline std::atomic_flag g_arch_installing = ATOMIC_FLAG_INIT;

inline decltype(&NvAPI_GPU_GetArchInfo) RealArchitecture() {
  const auto trampoline = g_arch_hook.Original<decltype(&NvAPI_GPU_GetArchInfo)>();
  return trampoline ? trampoline : GetNvapi().architecture;
}

inline bool ResolveArchitecture(NvPhysicalGpuHandle gpu, uint32_t vendor) {
  if (!architecture::NeedsDetection()) return true;
  NV_GPU_ARCH_INFO arch = {};
  arch.version = NV_GPU_ARCH_INFO_VER;
  const auto real = RealArchitecture();
  if (!gpu || !real || real(gpu, &arch) != NVAPI_OK) return false;
  if (architecture::ResolveAuto(vendor, arch.architecture, arch.implementation)) {
    Log(std::string("architecture: Auto -> ") + architecture::Name(architecture::g_active.load()));
  }
  return true;
}

inline NvAPI_Status __cdecl HookedGetArchInfo(NvPhysicalGpuHandle gpu, NV_GPU_ARCH_INFO* info) {
  const auto real = RealArchitecture();
  if (!real) return NVAPI_ERROR;
  const auto result = real(gpu, info);
  if (g_shutting_down.load(std::memory_order_acquire) ||
      !g_arch_scope || result != NVAPI_OK || !info) return result;
  const auto* profile = architecture::ActiveProfile();
  if (CanExposeArchitecture(g_enabled.load(), true, gpu == g_arch_scope,
                            PreparedProviderCount() == 1, result, profile)) {
    info->architecture = profile->exposed_arch;
  }
  return result;
}
static_assert(std::is_same_v<decltype(&HookedGetArchInfo), decltype(&NvAPI_GPU_GetArchInfo)>);

inline void EnsureArchitectureHook() {
  const auto* profile = architecture::ActiveProfile();
  if (!g_enabled.load() || !profile || !profile->NeedsRetarget()) return;
  auto& api = GetNvapi();
  if (!api.ready || g_arch_hook.installed.load() || g_arch_installing.test_and_set()) return;
  struct Guard {
    ~Guard() { g_arch_installing.clear(); }
  } guard;
  if (g_arch_hook.installed.load()) return;
  if (!hook::InstallAddress(g_arch_hook, reinterpret_cast<void*>(api.architecture),
                            reinterpret_cast<void*>(&HookedGetArchInfo), "NVAPI")) {
    Log("NVAPI hook failed", true);
  }
}

inline SRWLOCK g_adapter_lock = SRWLOCK_INIT;

inline NvPhysicalGpuHandle MatchAdapter(IDXGIAdapter* adapter) {
  if (!adapter) return nullptr;
  DXGI_ADAPTER_DESC desc = {};
  if (FAILED(adapter->GetDesc(&desc)) || desc.VendorId != kNvidiaVendorId) return nullptr;

  AcquireSRWLockExclusive(&g_adapter_lock);
  struct Unlock {
    ~Unlock() { ReleaseSRWLockExclusive(&g_adapter_lock); }
  } unlock;
  if (const auto bound = g_bound_gpu.load()) {
    return desc.AdapterLuid.LowPart == g_luid_low.load() &&
           static_cast<uint32_t>(desc.AdapterLuid.HighPart) == g_luid_high.load() ? bound : nullptr;
  }

  auto& api = GetNvapi();
  if (!api.ready) return nullptr;
  NvPhysicalGpuHandle physical[NVAPI_MAX_PHYSICAL_GPUS] = {};
  NvU32 count = 0;
  if (api.enumerate(physical, &count) != NVAPI_OK || count == 0 || count > NVAPI_MAX_PHYSICAL_GPUS)
    return nullptr;
  NvPhysicalGpuHandle matched = nullptr;
  for (NvU32 i = 0; i < count; ++i) {
    LUID luid = {};
    if (api.adapter_id(physical[i], &luid) != NVAPI_OK ||
        std::memcmp(&luid, &desc.AdapterLuid, sizeof(luid)) != 0) continue;
    if (matched) return nullptr;
    matched = physical[i];
  }
  if (!matched) return nullptr;

  // Explicit profiles are authoritative. Auto reads the original NVAPI value.
  if (!ResolveArchitecture(matched, desc.VendorId)) return nullptr;
  g_luid_low.store(desc.AdapterLuid.LowPart);
  g_luid_high.store(static_cast<uint32_t>(desc.AdapterLuid.HighPart));
  g_bound_gpu.store(matched);
  return matched;
}

inline void ProbeAdapterBeforeInit() {
  if (!g_enabled.load() || !architecture::NeedsBridge()) return;
  auto& api = GetNvapi();
  NvPhysicalGpuHandle physical[NVAPI_MAX_PHYSICAL_GPUS] = {};
  NvU32 count = 0;
  const bool enumerated = api.ready && api.enumerate(physical, &count) == NVAPI_OK &&
                          count > 0 && count <= NVAPI_MAX_PHYSICAL_GPUS;

  // Auto can resolve from NVAPI as soon as there is exactly one NVIDIA GPU.
  // Multi-GPU systems still wait for the renderer/NGX adapter LUID below.
  if (architecture::NeedsDetection() && enumerated && count == 1)
    ResolveArchitecture(physical[0], kNvidiaVendorId);
  if (!architecture::NeedsBridge()) return;

  if (!g_bound_gpu.load()) {
    // With multiple NVIDIA GPUs, wait for the actual NGX adapter rather than
    // selecting whichever adapter happens to be enumerated first.
    if (!enumerated || count != 1) return;
    HMODULE dxgi = GetModuleHandleW(L"dxgi.dll");
    if (!dxgi) return;
    using CreateFactoryFn = HRESULT(WINAPI*)(REFIID, void**);
    const auto create = reinterpret_cast<CreateFactoryFn>(GetProcAddress(dxgi, "CreateDXGIFactory1"));
    if (!create) return;
    IDXGIFactory1* factory = nullptr;
    if (FAILED(create(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory)))) return;
    for (UINT i = 0; i < 16; ++i) {
      IDXGIAdapter1* adapter = nullptr;
      if (factory->EnumAdapters1(i, &adapter) != S_OK) break;
      const auto matched = MatchAdapter(adapter);
      adapter->Release();
      if (matched) break;
    }
    factory->Release();
  }
  if (g_bound_gpu.load()) EnsureArchitectureHook();
}

inline bool EqualsInsensitive(const wchar_t* a, const wchar_t* b) {
  if (a == nullptr || b == nullptr) return false;
  for (;; ++a, ++b) {
    wchar_t ca = *a;
    wchar_t cb = *b;
    if (ca >= L'A' && ca <= L'Z') ca = static_cast<wchar_t>(ca - L'A' + L'a');
    if (cb >= L'A' && cb <= L'Z') cb = static_cast<wchar_t>(cb - L'A' + L'a');
    if (ca != cb) return false;
    if (ca == L'\0') return true;
  }
}

inline bool IsModuleNamed(HMODULE module, const wchar_t* wanted) {
  if (module == nullptr) return false;
  wchar_t path[32768] = {};
  const DWORD length = GetModuleFileNameW(module, path, ARRAYSIZE(path));
  if (length == 0 || length >= ARRAYSIZE(path)) return false;
  const wchar_t* slash = std::wcsrchr(path, L'\\');
  const wchar_t* name = slash == nullptr ? path : slash + 1;
  return EqualsInsensitive(name, wanted);
}

inline bool IsNgxRuntime(HMODULE module) {
  return IsModuleNamed(module, L"_nvngx.dll") || IsModuleNamed(module, L"nvngx.dll");
}

using RequirementsFn = decltype(&NVSDK_NGX_D3D12_GetFeatureRequirements);
using CreateFn = decltype(&NVSDK_NGX_D3D12_CreateFeature);
using EvaluateFn = decltype(&NVSDK_NGX_D3D12_EvaluateFeature);
using ReleaseFn = decltype(&NVSDK_NGX_D3D12_ReleaseFeature);
// D3D12 and Vulkan GetParameters/GetCapabilityParameters share this signature.
using ParametersFn = NVSDK_NGX_Result(NVSDK_CONV*)(NVSDK_NGX_Parameter**);
static_assert(std::is_same_v<ParametersFn, decltype(&NVSDK_NGX_D3D12_GetCapabilityParameters)>);
inline constexpr std::array<const char*, 4> kParameterExports = {
    "NVSDK_NGX_D3D12_GetCapabilityParameters", "NVSDK_NGX_D3D12_GetParameters",
    "NVSDK_NGX_VULKAN_GetCapabilityParameters", "NVSDK_NGX_VULKAN_GetParameters"};
// Vulkan dispatchable handles are pointer-sized opaque handles. Avoid pulling
// the full Vulkan SDK into this addon for a single NGX requirements entrypoint.
// The function is resolved dynamically from _nvngx.dll/nvngx.dll.
using VulkanRequirementsFn = NVSDK_NGX_Result(NVSDK_CONV*)(
    void* instance, void* physical_device,
    const NVSDK_NGX_FeatureDiscoveryInfo* discovery,
    NVSDK_NGX_FeatureRequirement* output);

struct NgxRuntime {
  HMODULE module = nullptr;
  hook::ModuleIdentity identity{};
  RequirementsFn entry_requirements = nullptr;
  CreateFn entry_create = nullptr;
  EvaluateFn entry_evaluate = nullptr;
  ReleaseFn entry_release = nullptr;
  VulkanRequirementsFn entry_vulkan_requirements = nullptr;
  std::array<ParametersFn, 4> parameters{};
  std::array<std::atomic_bool, 4> parameters_seen{};
  std::array<const NVSDK_NGX_Handle*, 16> fg_handles{};
  SRWLOCK handles_lock = SRWLOCK_INIT;
};

inline std::array<NgxRuntime, 4> g_slots;
inline SRWLOCK g_resolver_lock = SRWLOCK_INIT;
inline thread_local bool g_in_requirements = false;
inline thread_local bool g_in_capabilities = false;
inline thread_local bool g_installing = false;

inline bool IsTracked(NgxRuntime& slot, const NVSDK_NGX_Handle* handle) {
  if (handle == nullptr) return false;
  AcquireSRWLockShared(&slot.handles_lock);
  const bool found =
      std::find(slot.fg_handles.begin(), slot.fg_handles.end(), handle) != slot.fg_handles.end();
  ReleaseSRWLockShared(&slot.handles_lock);
  return found;
}

inline NVSDK_NGX_Result FinalizeRequirements(NVSDK_NGX_Result result,
                                                NVSDK_NGX_FeatureRequirement* output,
                                                bool adapter_bound) {
  const bool valid = result == NVSDK_NGX_Result_Success && output != nullptr;
  const uint32_t flags = valid ? static_cast<uint32_t>(output->FeatureSupported) : 0;
  const uint32_t arch = valid ? output->MinHWArchitecture : 0;
  const auto provider_status = GetProviderStatus();
  const unsigned int prepared_providers = provider_status.QualifiedCount();
  const auto* profile = architecture::ActiveProfile();
  const RequirementsEvidence evidence{g_enabled.load(), adapter_bound, prepared_providers,
      static_cast<uint32_t>(result), kDlssGFeatureId, flags, arch};
  const bool change = valid && CanRelaxRequirements(evidence, profile);
  if (change) {
    output->MinHWArchitecture = profile->native_arch;
    output->FeatureSupported = static_cast<decltype(output->FeatureSupported)>(0);
  }

  static std::atomic_bool adjusted_logged{false};
  if (change) {
    if (!adjusted_logged.exchange(true, std::memory_order_relaxed))
      Log("NGX frame-generation requirements adjusted");
  } else if (result != NVSDK_NGX_Result_Success) {
    std::stringstream message;
    message << "NGX frame-generation requirements failed (0x" << std::hex
            << static_cast<uint32_t>(result) << ')';
    Log(message.str(), true);
  }
  return result;
}

template <size_t I>
NVSDK_NGX_Result NVSDK_CONV VulkanRequirements(
    void* instance, void* physical_device,
    const NVSDK_NGX_FeatureDiscoveryInfo* discovery,
    NVSDK_NGX_FeatureRequirement* output) {
  auto& slot = g_slots[I];
  const auto real = slot.entry_vulkan_requirements;
  if (!real) return NVSDK_NGX_Result_FAIL_InvalidParameter;
  if (g_shutting_down.load(std::memory_order_acquire)) {
    ScopedArchQuery no_spoof(nullptr);
    return real(instance, physical_device, discovery, output);
  }
  const bool fg = discovery != nullptr && static_cast<uint32_t>(discovery->FeatureID) ==
      static_cast<uint32_t>(NVSDK_NGX_Feature_FrameGeneration);
  if (!g_enabled.load(std::memory_order_relaxed) || !architecture::NeedsBridge() ||
      !fg || g_in_requirements) {
    ScopedArchQuery no_spoof(nullptr);
    return real(instance, physical_device, discovery, output);
  }

  g_in_requirements = true;
  struct Guard {
    ~Guard() { g_in_requirements = false; }
  } guard;

  NvPhysicalGpuHandle physical_gpu = nullptr;
  try {
    ProbeAdapterBeforeInit();
    physical_gpu = g_bound_gpu.load(std::memory_order_acquire);
    EnsureArchitectureHook();
    if (physical_gpu != nullptr && g_prepare_loaded_providers != nullptr)
      g_prepare_loaded_providers();
  } catch (...) {
    Log("Vulkan NGX requirements preflight failed", true);
  }

  ScopedArchQuery architecture_scope(physical_gpu);
  const NVSDK_NGX_Result result = real(instance, physical_device, discovery, output);

  try {
    if (physical_gpu != nullptr && g_prepare_loaded_providers != nullptr)
      g_prepare_loaded_providers();
  } catch (...) {
    Log("Vulkan NGX requirements postflight failed", true);
  }

  return FinalizeRequirements(result, output, physical_gpu != nullptr);
}

template <size_t I>
NVSDK_NGX_Result NVSDK_CONV Requirements(
    IDXGIAdapter* adapter, const NVSDK_NGX_FeatureDiscoveryInfo* discovery,
    NVSDK_NGX_FeatureRequirement* output) {
  auto& slot = g_slots[I];
  const auto real = slot.entry_requirements;
  if (!real) return NVSDK_NGX_Result_FAIL_InvalidParameter;
  if (g_shutting_down.load(std::memory_order_acquire)) {
    ScopedArchQuery no_spoof(nullptr);
    return real(adapter, discovery, output);
  }
  const bool fg = discovery != nullptr && static_cast<uint32_t>(discovery->FeatureID) ==
      static_cast<uint32_t>(NVSDK_NGX_Feature_FrameGeneration);
  if (!g_enabled.load(std::memory_order_relaxed) || !architecture::NeedsBridge() || !fg || g_in_requirements) {
    ScopedArchQuery no_spoof(nullptr);
    return real(adapter, discovery, output);
  }

  g_in_requirements = true;
  struct Guard {
    ~Guard() { g_in_requirements = false; }
  } guard;

  bool adapter_bound = false;
  NvPhysicalGpuHandle physical_gpu = nullptr;
  try {
    physical_gpu = MatchAdapter(adapter);
    EnsureArchitectureHook();
    adapter_bound = physical_gpu != nullptr;
    if (adapter_bound && g_prepare_loaded_providers != nullptr) g_prepare_loaded_providers();
  } catch (...) {
    Log("NGX requirements preflight failed", true);
  }

  ScopedArchQuery architecture_scope(physical_gpu);
  const NVSDK_NGX_Result result = real(adapter, discovery, output);

  // The original call can load nvngx_dlssg.dll. The existing load-time provider
  // callback prepares it synchronously; this second pass also covers providers
  // that predated the callback without tying admission to a particular callsite.
  try {
    if (adapter_bound && g_prepare_loaded_providers != nullptr) g_prepare_loaded_providers();
  } catch (...) {
    Log("NGX requirements postflight failed", true);
  }

  return FinalizeRequirements(result, output, adapter_bound);
}

template <size_t I>
NVSDK_NGX_Result NVSDK_CONV Create(ID3D12GraphicsCommandList* commands, NVSDK_NGX_Feature feature,
                                   NVSDK_NGX_Parameter* parameters, NVSDK_NGX_Handle** handle) {
  auto& slot = g_slots[I];
  const auto real = slot.entry_create;
  if (!real) return NVSDK_NGX_Result_FAIL_InvalidParameter;
  if (g_shutting_down.load(std::memory_order_acquire))
    return real(commands, feature, parameters, handle);
  const bool fg = static_cast<uint32_t>(feature) ==
      static_cast<uint32_t>(NVSDK_NGX_Feature_FrameGeneration);
  if (fg) g_create_seen.store(true, std::memory_order_relaxed);
  const NVSDK_NGX_Result result = real(commands, feature, parameters, handle);
  if (fg) {
    const NVSDK_NGX_Handle* created =
        result == NVSDK_NGX_Result_Success && handle != nullptr ? *handle : nullptr;
    if (created != nullptr) {
      AcquireSRWLockExclusive(&slot.handles_lock);
      const auto free = std::find(slot.fg_handles.begin(), slot.fg_handles.end(), nullptr);
      if (free != slot.fg_handles.end()) *free = created;
      ReleaseSRWLockExclusive(&slot.handles_lock);
    }
    if (result == NVSDK_NGX_Result_Success && created != nullptr) {
      if (!g_status.feature_created.exchange(true, std::memory_order_relaxed))
        Log("DLSS-G feature created");
    } else if (result != NVSDK_NGX_Result_Success) {
      std::stringstream message;
      message << "DLSS-G CreateFeature failed (0x" << std::hex
              << static_cast<uint32_t>(result) << ')';
      Log(message.str(), true);
    }
  }
  return result;
}

template <size_t I>
NVSDK_NGX_Result NVSDK_CONV Evaluate(ID3D12GraphicsCommandList* commands,
                                     const NVSDK_NGX_Handle* handle,
                                     const NVSDK_NGX_Parameter* parameters,
                                     PFN_NVSDK_NGX_ProgressCallback callback) {
  auto& slot = g_slots[I];
  const auto real = slot.entry_evaluate;
  if (!real) return NVSDK_NGX_Result_FAIL_InvalidParameter;
  if (g_shutting_down.load(std::memory_order_acquire))
    return real(commands, handle, parameters, callback);
  const bool fg = IsTracked(slot, handle);
  const NVSDK_NGX_Result result = real(commands, handle, parameters, callback);
  if (fg) {
    if (result == NVSDK_NGX_Result_Success) {
      if (!g_status.feature_active.exchange(true, std::memory_order_relaxed))
        Log("DLSS-G active");
    } else {
      std::stringstream message;
      message << "DLSS-G EvaluateFeature failed (0x" << std::hex
              << static_cast<uint32_t>(result) << ')';
      Log(message.str(), true);
    }
  }
  return result;
}

template <size_t I>
NVSDK_NGX_Result NVSDK_CONV Release(NVSDK_NGX_Handle* handle) {
  auto& slot = g_slots[I];
  const auto real = slot.entry_release;
  if (!real) return NVSDK_NGX_Result_FAIL_InvalidParameter;
  if (g_shutting_down.load(std::memory_order_acquire)) return real(handle);
  const NVSDK_NGX_Result result = real(handle);
  if (result == NVSDK_NGX_Result_Success && handle != nullptr) {
    AcquireSRWLockExclusive(&slot.handles_lock);
    for (auto& entry : slot.fg_handles) {
      if (entry == handle) entry = nullptr;
    }
    ReleaseSRWLockExclusive(&slot.handles_lock);
  }
  return result;
}

// Update the vendor-owned block before the caller caches capabilities. This
// covers both signed and unsigned Get overloads without replacing its vtable.
inline void ApplyCapabilities(NVSDK_NGX_Parameter* parameters, unsigned int limit) {
  if (!parameters) return;
  int available = 0;
  int maximum = 0;
  const auto available_result = parameters->Get("FrameGeneration.Available", &available);
  const auto maximum_result = parameters->Get("DLSSG.MultiFrameCountMax", &maximum);
  const auto* profile = architecture::ActiveProfile();
  const bool ready = g_enabled.load() && profile && profile->NeedsRetarget() && limit != 0;

  if (ready && available_result == NVSDK_NGX_Result_Success && available == 0) {
    int needs_driver = 0;
    unsigned int feature_result = NVSDK_NGX_Result_Success;
    const auto driver_result = parameters->Get("FrameGeneration.NeedsUpdatedDriver", &needs_driver);
    const auto init_result = parameters->Get("FrameGeneration.FeatureInitResult", &feature_result);
    // A qualified provider does not waive an explicit driver/initialization error.
    const bool init_ok = init_result != NVSDK_NGX_Result_Success ||
        feature_result == NVSDK_NGX_Result_Success ||
        feature_result == NVSDK_NGX_Result_FAIL_FeatureNotSupported;
    if (driver_result == NVSDK_NGX_Result_Success && needs_driver == 0 && init_ok) {
      parameters->Set("FrameGeneration.Available", 1);
      const auto stored = parameters->Get("FrameGeneration.Available", &available);
      if (stored == NVSDK_NGX_Result_Success && available == 1 && !g_status.available.load())
        Log("FrameGeneration.Available: 0 -> 1");
    }
  }
  if (ready && available_result == NVSDK_NGX_Result_Success && available > 0 &&
      maximum_result == NVSDK_NGX_Result_Success && maximum >= 0) {
    const auto wanted = CapabilityFrameCount(maximum, limit);
    if (wanted != maximum) {
      parameters->Set("DLSSG.MultiFrameCountMax", wanted);
      int stored = maximum;
      if (parameters->Get("DLSSG.MultiFrameCountMax", &stored) == NVSDK_NGX_Result_Success &&
          stored != maximum) {
        if (g_status.capability_max.load() != static_cast<unsigned int>(stored))
          Log("DLSSG.MultiFrameCountMax: " + std::to_string(maximum) + " -> " + std::to_string(stored));
        maximum = stored;
      }
    }
  }
  g_status.available.store(available_result == NVSDK_NGX_Result_Success && available > 0);
  g_status.capability_max.store(maximum_result == NVSDK_NGX_Result_Success && maximum >= 0
                                    ? static_cast<unsigned int>(maximum) : 0);
}

template <size_t I, size_t Api>
NVSDK_NGX_Result NVSDK_CONV Capabilities(NVSDK_NGX_Parameter** output) {
  auto& slot = g_slots[I];
  const auto real = slot.parameters[Api];
  if (!real) return NVSDK_NGX_Result_FAIL_InvalidParameter;
  if (g_shutting_down.load(std::memory_order_acquire) || !g_enabled.load() ||
      !architecture::NeedsBridge() || g_in_capabilities) return real(output);

  g_in_capabilities = true;
  struct Guard {
    ~Guard() { g_in_capabilities = false; }
  } guard;
  try {
    ProbeAdapterBeforeInit();
    if (g_prepare_loaded_providers) g_prepare_loaded_providers();
  } catch (...) {
    Log("NGX capability preflight failed", true);
  }
  const auto result = real(output);
  g_status.capabilities_seen.store(true, std::memory_order_relaxed);
  g_status.vulkan_capabilities.store(Api >= 2, std::memory_order_relaxed);
  if (!slot.parameters_seen[Api].exchange(true)) {
    Log(std::string(kParameterExports[Api]) + ": " + std::to_string(static_cast<uint32_t>(result)));
  }
  if (result != NVSDK_NGX_Result_Success || !output || !*output) return result;
  try {
    // The native call can load the provider while populating the block.
    if (g_prepare_loaded_providers) g_prepare_loaded_providers();
    ApplyCapabilities(*output, g_capability_limit ? g_capability_limit() : 0);
  } catch (...) {
    Log("NGX capability update failed", true);
  }
  return result;
}

template <size_t I>
std::vector<hook::HookItem> EntryHooks(NgxRuntime& slot) {
  static_assert(std::is_same_v<decltype(&Requirements<I>), RequirementsFn>);
  static_assert(std::is_same_v<decltype(&Create<I>), CreateFn>);
  static_assert(std::is_same_v<decltype(&Evaluate<I>), EvaluateFn>);
  static_assert(std::is_same_v<decltype(&Release<I>), ReleaseFn>);
  static_assert(std::is_same_v<decltype(&VulkanRequirements<I>), VulkanRequirementsFn>);
  static_assert(std::is_same_v<decltype(&Capabilities<I, 0>), ParametersFn>);
  return {
      {"NVSDK_NGX_D3D12_GetFeatureRequirements", reinterpret_cast<void**>(&slot.entry_requirements),
       reinterpret_cast<void*>(&Requirements<I>)},
      {"NVSDK_NGX_D3D12_CreateFeature", reinterpret_cast<void**>(&slot.entry_create),
       reinterpret_cast<void*>(&Create<I>)},
      {"NVSDK_NGX_D3D12_EvaluateFeature", reinterpret_cast<void**>(&slot.entry_evaluate),
       reinterpret_cast<void*>(&Evaluate<I>)},
      {"NVSDK_NGX_D3D12_ReleaseFeature", reinterpret_cast<void**>(&slot.entry_release),
       reinterpret_cast<void*>(&Release<I>)},
      {"NVSDK_NGX_VULKAN_GetFeatureRequirements", reinterpret_cast<void**>(&slot.entry_vulkan_requirements),
       reinterpret_cast<void*>(&VulkanRequirements<I>)},
      {kParameterExports[0], reinterpret_cast<void**>(&slot.parameters[0]),
       reinterpret_cast<void*>(&Capabilities<I, 0>)},
      {kParameterExports[1], reinterpret_cast<void**>(&slot.parameters[1]),
       reinterpret_cast<void*>(&Capabilities<I, 1>)},
      {kParameterExports[2], reinterpret_cast<void**>(&slot.parameters[2]),
       reinterpret_cast<void*>(&Capabilities<I, 2>)},
      {kParameterExports[3], reinterpret_cast<void**>(&slot.parameters[3]),
       reinterpret_cast<void*>(&Capabilities<I, 3>)},
  };
}

inline std::vector<hook::HookItem> EntryHooks(size_t index) {
  auto& slot = g_slots[index];
  switch (index) {
    case 0: return EntryHooks<0>(slot);
    case 1: return EntryHooks<1>(slot);
    case 2: return EntryHooks<2>(slot);
    case 3: return EntryHooks<3>(slot);
    default: return {};
  }
}

inline void ClearRuntime(size_t index) {
  auto& slot = g_slots[index];
  for (const auto& entry : EntryHooks(index)) *std::get<1>(entry) = nullptr;
  slot.module = nullptr;
  slot.identity = {};
  for (auto& seen : slot.parameters_seen) seen.store(false);
  slot.fg_handles.fill(nullptr);
}

inline bool IsTargetName(const char* name) {
  if (!name || reinterpret_cast<uintptr_t>(name) <= 0xffff) return false;
  for (const auto* parameter : kParameterExports)
    if (std::strcmp(name, parameter) == 0) return true;
  return std::strcmp(name, "NVSDK_NGX_D3D12_GetFeatureRequirements") == 0 ||
         std::strcmp(name, "NVSDK_NGX_VULKAN_GetFeatureRequirements") == 0 ||
         std::strcmp(name, "NVSDK_NGX_D3D12_CreateFeature") == 0 ||
         std::strcmp(name, "NVSDK_NGX_D3D12_EvaluateFeature") == 0 ||
         std::strcmp(name, "NVSDK_NGX_D3D12_ReleaseFeature") == 0;
}
}  // namespace internal

// Called at module discovery or export lookup, before handing a native address
// to the host. No driver initialization or provider scan belongs in this path.
inline void TryInstall(HMODULE module) {
  if (g_shutting_down.load(std::memory_order_acquire) || !g_enabled.load() ||
      !architecture::NeedsBridge() || internal::g_installing ||
      !internal::IsNgxRuntime(module)) return;
  internal::g_installing = true;
  struct Guard {
    ~Guard() { internal::g_installing = false; }
  } guard;

  // A loader callback must not wait on another thread which may be loading a DLL.
  if (!TryAcquireSRWLockExclusive(&internal::g_resolver_lock)) return;
  struct Unlock {
    ~Unlock() { ReleaseSRWLockExclusive(&internal::g_resolver_lock); }
  } unlock;

  size_t index = internal::g_slots.size();
  for (size_t i = 0; i < internal::g_slots.size(); ++i) {
    auto& slot = internal::g_slots[i];
    if (slot.module && !hook::IsCurrent(slot.identity)) internal::ClearRuntime(i);
    if (slot.module == module) { index = i; break; }
  }
  if (index == internal::g_slots.size()) {
    for (size_t i = 0; i < internal::g_slots.size(); ++i) {
      auto& slot = internal::g_slots[i];
      if (slot.module) continue;
      if (!hook::CaptureModule(module, slot.identity)) return;
      slot.module = module;
      index = i;
      break;
    }
  }
  if (index == internal::g_slots.size()) return;

  auto hooks = internal::EntryHooks(index);
  const size_t first_parameter = hooks.size() - internal::kParameterExports.size();
  std::vector<hook::HookItem> pending;
  for (size_t i = 0; i < hooks.size(); ++i) {
    const auto& [name, storage, replacement] = hooks[i];
    if (*storage) continue;
    const auto target = GetProcAddress(module, name);
    if (!target) continue;
    bool alias = false;
    for (size_t j = 0; j < hooks.size(); ++j) {
      if (i == j || target != GetProcAddress(module, std::get<0>(hooks[j]))) continue;
      // Parameter exports have identical ABI and policy. Hook an alias once;
      // ambiguous aliases of unlike APIs (e.g. common failure stubs) stay native.
      if (i < first_parameter || j < first_parameter || j < i) { alias = true; break; }
    }
    if (!alias) pending.push_back(hooks[i]);
  }
  if (!pending.empty()) hook::Install(module, pending, "NGX");
}

inline FARPROC Resolve(HMODULE module, const char* name, FARPROC original) {
  if (original && internal::IsTargetName(name)) TryInstall(module);
  return original;
}

inline void EnsureEntryHooks() {
  for (const auto* name : {L"_nvngx.dll", L"nvngx.dll"}) TryInstall(GetModuleHandleW(name));
}

inline void BeforeInit() {
  if (g_shutting_down.load(std::memory_order_acquire)) return;
  try {
    internal::ProbeAdapterBeforeInit();
    EnsureEntryHooks();
    if (architecture::ActiveProfile() && g_prepare_loaded_providers) g_prepare_loaded_providers();
  } catch (...) {
    Log("NGX preflight failed", true);
  }
}

inline void Shutdown() {
  g_shutting_down.store(true, std::memory_order_release);
  AcquireSRWLockExclusive(&internal::g_resolver_lock);
  for (size_t i = internal::g_slots.size(); i-- > 0;) {
    auto& slot = internal::g_slots[i];
    if (slot.module && hook::IsCurrent(slot.identity)) hook::Uninstall(internal::EntryHooks(i));
    internal::ClearRuntime(i);
  }
  ReleaseSRWLockExclusive(&internal::g_resolver_lock);
  hook::UninstallAddress(internal::g_arch_hook);
  internal::g_bound_gpu.store(nullptr);
  internal::g_luid_low.store(0);
  internal::g_luid_high.store(0);
}
}  // namespace mfgunlock::ampere::ngx
