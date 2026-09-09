/*
 * NGX/NVAPI bridge for native Ampere DLSS-G.
 * SPDX-License-Identifier: MIT
 *
 * Qualifies the Ampere adapter, scopes the Ada architecture exposure to DLSS-G
 * capability queries, and observes the native Create/Evaluate/Release path.
 */
#pragma once

#include <windows.h>
#include <wincrypt.h>  // Needed by d3dkmthk.h when WIN32_LEAN_AND_MEAN is active.
#include <d3d12.h>
#include <d3dkmthk.h>
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

struct Telemetry {
  std::atomic_uint64_t requirements{0};
  std::atomic_uint64_t overrides{0};
  std::atomic_uint64_t creates{0};
  std::atomic_uint64_t creates_ok{0};
  std::atomic_uint64_t evaluates{0};
  std::atomic_uint64_t releases{0};
  std::atomic_uint32_t requirements_result{0};
  std::atomic_uint32_t flags{0};
  std::atomic_uint32_t min_arch{0};
  std::atomic_uint32_t create_result{0};
  std::atomic_uint32_t evaluate_result{0};
  std::atomic<const char*> decision{"not-observed"};
  std::atomic_uint32_t entry_hooked{0};
};
inline Telemetry g_telemetry;
inline std::atomic_bool g_shutting_down{false};

namespace internal {

// Original NVAPI pointers are kept separately from the entry hook. Adapter
// qualification must never consult our exposed architecture.
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
inline std::atomic<NvPhysicalGpuHandle> g_single_gpu{nullptr};
inline std::atomic_uint32_t g_actual_arch{0};
inline std::atomic_uint32_t g_actual_implementation{0};
inline std::atomic_uint32_t g_luid_low{0};
inline std::atomic_uint32_t g_luid_high{0};
inline std::atomic_uint32_t g_hags27{0};
inline std::atomic_uint32_t g_hags29{0};
inline std::atomic_bool g_hags27_known{false};
inline std::atomic_bool g_hags29_known{false};
inline std::atomic_uint64_t g_arch_calls{0};
inline std::atomic_uint64_t g_arch_changes{0};
inline std::atomic_uint32_t g_exposed_arch{0};
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

inline NvAPI_Status __cdecl HookedGetArchInfo(NvPhysicalGpuHandle gpu, NV_GPU_ARCH_INFO* info) {
  const auto real = RealArchitecture();
  if (!real) return NVAPI_ERROR;
  const auto result = real(gpu, info);
  if (g_shutting_down.load(std::memory_order_acquire) ||
      !g_arch_scope || result != NVAPI_OK || !info) return result;
  const auto actual = info->architecture;
  if (CanExposeAda(g_enabled.load(), true, gpu == g_arch_scope,
                   PreparedProviderCount() == 1, result, actual, info->implementation)) {
    info->architecture = kAdaArchitecture;
    g_arch_changes.fetch_add(1);
  }
  g_arch_calls.fetch_add(1);
  g_exposed_arch.store(info->architecture);
  return result;
}
static_assert(std::is_same_v<decltype(&HookedGetArchInfo), decltype(&NvAPI_GPU_GetArchInfo)>);

inline void ReadHardwareScheduling(const LUID& luid) {
  // Diagnostics only. Spoofing HAGS cannot enable the kernel scheduler needed
  // by native DLSS-G. Failure means unknown, not unsupported or enabled.
  HMODULE gdi = LoadLibraryExW(L"gdi32.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
  if (!gdi) return;
  const auto open = reinterpret_cast<decltype(&D3DKMTOpenAdapterFromLuid)>(
      GetProcAddress(gdi, "D3DKMTOpenAdapterFromLuid"));
  const auto query = reinterpret_cast<decltype(&D3DKMTQueryAdapterInfo)>(
      GetProcAddress(gdi, "D3DKMTQueryAdapterInfo"));
  const auto close = reinterpret_cast<decltype(&D3DKMTCloseAdapter)>(
      GetProcAddress(gdi, "D3DKMTCloseAdapter"));
  if (open && query && close) {
    D3DKMT_OPENADAPTERFROMLUID adapter = {};
    adapter.AdapterLuid = luid;
    if (open(&adapter) == 0) {
      D3DKMT_WDDM_2_7_CAPS c27 = {};
      D3DKMT_WDDM_2_9_CAPS c29 = {};
      D3DKMT_QUERYADAPTERINFO request = {};
      request.hAdapter = adapter.hAdapter;
      request.Type = KMTQAITYPE_WDDM_2_7_CAPS;
      request.pPrivateDriverData = &c27;
      request.PrivateDriverDataSize = sizeof(c27);
      if (query(&request) == 0) {
        g_hags27.store(c27.Value);
        g_hags27_known.store(true);
      }
      request.Type = KMTQAITYPE_WDDM_2_9_CAPS;
      request.pPrivateDriverData = &c29;
      request.PrivateDriverDataSize = sizeof(c29);
      if (query(&request) == 0) {
        g_hags29.store(c29.Value);
        g_hags29_known.store(true);
      }
      D3DKMT_CLOSEADAPTER finish = {};
      finish.hAdapter = adapter.hAdapter;
      close(&finish);
    }
  }
  FreeLibrary(gdi);
}

inline NvPhysicalGpuHandle MatchAmpereAdapter(IDXGIAdapter* adapter) {
  if (!adapter) return nullptr;
  DXGI_ADAPTER_DESC desc = {};
  if (FAILED(adapter->GetDesc(&desc)) || desc.VendorId != 0x10de) return nullptr;
  auto& api = GetNvapi();
  if (!api.ready) return nullptr;
  NvPhysicalGpuHandle physical[NVAPI_MAX_PHYSICAL_GPUS] = {};
  NvU32 count = 0;
  // A provider contains shared device code. Do not retarget it while a second
  // NVIDIA adapter could select the same image.
  if (api.enumerate(physical, &count) != NVAPI_OK || count != 1) return nullptr;
  LUID luid = {};
  NV_GPU_ARCH_INFO arch = {};
  arch.version = NV_GPU_ARCH_INFO_VER;
  const auto architecture = RealArchitecture();
  if (api.adapter_id(physical[0], &luid) != NVAPI_OK ||
      std::memcmp(&luid, &desc.AdapterLuid, sizeof(luid)) != 0 ||
      !architecture || architecture(physical[0], &arch) != NVAPI_OK) {
    return nullptr;
  }
  g_actual_arch.store(arch.architecture);
  g_actual_implementation.store(arch.implementation);
  if (!IsSupportedAmpere(desc.VendorId, arch.architecture, arch.implementation)) {
    g_other_gpu.store(true);
    return nullptr;
  }
  g_single_gpu.store(physical[0]);
  g_luid_low.store(luid.LowPart);
  g_luid_high.store(static_cast<uint32_t>(luid.HighPart));
  g_other_gpu.store(false);
  if (!g_device_confirmed.exchange(true)) {
    ReadHardwareScheduling(luid);
    std::stringstream message;
    message << "Ampere GPU detected (arch=0x" << std::hex << arch.architecture
            << ", impl=0x" << arch.implementation << ')';
    Log(message.str());
  }
  return physical[0];
}

inline void ProbeAdapterBeforeInit() {
  if (!g_enabled.load()) return;
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
    MatchAmpereAdapter(adapter);
    adapter->Release();
  }
  factory->Release();
  auto& api = GetNvapi();
  if (api.ready && g_device_confirmed.load() && !g_arch_hook.installed.load() &&
      !g_arch_installing.test_and_set()) {
    struct Guard {
      ~Guard() { g_arch_installing.clear(); }
    } guard;
    if (g_arch_hook.installed.load()) return;
    if (!hook::InstallAddress(g_arch_hook, reinterpret_cast<void*>(api.architecture),
                              reinterpret_cast<void*>(&HookedGetArchInfo),
                              "NVAPI")) {
      Log("NVAPI hook failed", true);
    }
  }
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

inline bool IsNgxRuntime(HMODULE module) {
  if (module == nullptr) return false;
  wchar_t path[32768] = {};
  const DWORD length = GetModuleFileNameW(module, path, ARRAYSIZE(path));
  if (length == 0 || length >= ARRAYSIZE(path)) return false;
  const wchar_t* slash = std::wcsrchr(path, L'\\');
  const wchar_t* name = slash == nullptr ? path : slash + 1;
  return EqualsInsensitive(name, L"_nvngx.dll") || EqualsInsensitive(name, L"nvngx.dll");
}

using RequirementsFn = decltype(&NVSDK_NGX_D3D12_GetFeatureRequirements);
using CreateFn = decltype(&NVSDK_NGX_D3D12_CreateFeature);
using EvaluateFn = decltype(&NVSDK_NGX_D3D12_EvaluateFeature);
using ReleaseFn = decltype(&NVSDK_NGX_D3D12_ReleaseFeature);

struct NgxRuntime {
  HMODULE module = nullptr;
  hook::ModuleIdentity identity{};
  RequirementsFn requirements = nullptr;
  // Detours owns these four trampoline pointers. They are initialized before
  // commit and consulted by the same wrappers used by the resolver route.
  RequirementsFn entry_requirements = nullptr;
  CreateFn entry_create = nullptr;
  EvaluateFn entry_evaluate = nullptr;
  ReleaseFn entry_release = nullptr;
  bool entries_installed = false;
  CreateFn create = nullptr;
  EvaluateFn evaluate = nullptr;
  ReleaseFn release = nullptr;
  std::array<const NVSDK_NGX_Handle*, 16> fg_handles{};
  SRWLOCK handles_lock = SRWLOCK_INIT;
  std::atomic_uint64_t evaluations{0};
};

inline std::array<NgxRuntime, 4> g_slots;
inline SRWLOCK g_resolver_lock = SRWLOCK_INIT;
inline thread_local bool g_in_requirements = false;

inline bool IsTracked(NgxRuntime& slot, const NVSDK_NGX_Handle* handle) {
  if (handle == nullptr) return false;
  AcquireSRWLockShared(&slot.handles_lock);
  const bool found =
      std::find(slot.fg_handles.begin(), slot.fg_handles.end(), handle) != slot.fg_handles.end();
  ReleaseSRWLockShared(&slot.handles_lock);
  return found;
}

template <size_t I>
NVSDK_NGX_Result NVSDK_CONV Requirements(
    IDXGIAdapter* adapter, const NVSDK_NGX_FeatureDiscoveryInfo* discovery,
    NVSDK_NGX_FeatureRequirement* output) {
  auto& slot = g_slots[I];
  const auto real = slot.entry_requirements ? slot.entry_requirements : slot.requirements;
  if (!real) return NVSDK_NGX_Result_FAIL_InvalidParameter;
  if (g_shutting_down.load(std::memory_order_acquire)) {
    ScopedArchQuery no_spoof(nullptr);
    return real(adapter, discovery, output);
  }
  const bool fg = discovery != nullptr && static_cast<uint32_t>(discovery->FeatureID) ==
      static_cast<uint32_t>(NVSDK_NGX_Feature_FrameGeneration);
  if (!g_enabled.load(std::memory_order_relaxed) || !fg || g_in_requirements) {
    ScopedArchQuery no_spoof(nullptr);
    return real(adapter, discovery, output);
  }

  g_in_requirements = true;
  struct Guard {
    ~Guard() { g_in_requirements = false; }
  } guard;

  bool is_ampere = false;
  NvPhysicalGpuHandle physical_gpu = nullptr;
  try {
    physical_gpu = MatchAmpereAdapter(adapter);
    is_ampere = physical_gpu != nullptr;
    if (is_ampere && g_prepare_loaded_providers != nullptr) g_prepare_loaded_providers();
  } catch (...) {
    Log("NGX requirements preflight failed", true);
  }

  ScopedArchQuery architecture_scope(physical_gpu);
  const NVSDK_NGX_Result result = real(adapter, discovery, output);

  // The original call can load nvngx_dlssg.dll. The existing load-time provider
  // callback prepares it synchronously; this second pass also covers providers
  // that predated the callback without tying admission to a particular callsite.
  try {
    if (is_ampere && g_prepare_loaded_providers != nullptr) g_prepare_loaded_providers();
  } catch (...) {
    Log("NGX requirements postflight failed", true);
  }

  const bool valid = result == NVSDK_NGX_Result_Success && output != nullptr;
  const uint32_t flags = valid ? static_cast<uint32_t>(output->FeatureSupported) : 0;
  const uint32_t arch = valid ? output->MinHWArchitecture : 0;
  const auto provider_status = GetProviderStatus();
  const unsigned int prepared_providers = provider_status.QualifiedCount();
  const bool change = valid && CanRelaxRequirements(
      {true, is_ampere, prepared_providers, static_cast<uint32_t>(result),
       static_cast<uint32_t>(NVSDK_NGX_Feature_FrameGeneration), flags, arch});
  if (change) {
    output->MinHWArchitecture = 0x170;
    output->FeatureSupported = static_cast<decltype(output->FeatureSupported)>(0);
  }

  g_telemetry.requirements.fetch_add(1);
  g_telemetry.requirements_result.store(static_cast<uint32_t>(result));
  g_telemetry.flags.store(flags);
  g_telemetry.min_arch.store(arch);
  g_telemetry.decision.store(RequirementsDecision({true, is_ampere, prepared_providers,
      static_cast<uint32_t>(result), 11, flags, arch}));
  if (valid && is_ampere && prepared_providers != 1) {
    g_telemetry.decision.store(provider_status.Reason());
  }
  if (change) {
    const auto previous = g_telemetry.overrides.fetch_add(1);
    if (previous == 0) Log("NGX frame-generation requirements adjusted for Ampere");
  } else if (result != NVSDK_NGX_Result_Success) {
    std::stringstream message;
    message << "NGX frame-generation requirements failed (0x" << std::hex
            << static_cast<uint32_t>(result) << ')';
    Log(message.str(), true);
  }
  return result;
}

template <size_t I>
NVSDK_NGX_Result NVSDK_CONV Create(ID3D12GraphicsCommandList* commands, NVSDK_NGX_Feature feature,
                                   NVSDK_NGX_Parameter* parameters, NVSDK_NGX_Handle** handle) {
  auto& slot = g_slots[I];
  const auto real = slot.entry_create ? slot.entry_create : slot.create;
  if (!real) return NVSDK_NGX_Result_FAIL_InvalidParameter;
  if (g_shutting_down.load(std::memory_order_acquire))
    return real(commands, feature, parameters, handle);
  const bool fg = static_cast<uint32_t>(feature) ==
      static_cast<uint32_t>(NVSDK_NGX_Feature_FrameGeneration);
  if (fg) g_create_seen.store(true, std::memory_order_relaxed);
  const NVSDK_NGX_Result result = real(commands, feature, parameters, handle);
  if (fg) {
    g_telemetry.creates.fetch_add(1);
    g_telemetry.create_result.store(static_cast<uint32_t>(result));
    if (result == NVSDK_NGX_Result_Success && handle && *handle) g_telemetry.creates_ok.fetch_add(1);
    const NVSDK_NGX_Handle* created =
        result == NVSDK_NGX_Result_Success && handle != nullptr ? *handle : nullptr;
    if (created != nullptr) {
      AcquireSRWLockExclusive(&slot.handles_lock);
      const auto free = std::find(slot.fg_handles.begin(), slot.fg_handles.end(), nullptr);
      if (free != slot.fg_handles.end()) *free = created;
      ReleaseSRWLockExclusive(&slot.handles_lock);
    }
    if (result == NVSDK_NGX_Result_Success && created != nullptr) {
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
  const auto real = slot.entry_evaluate ? slot.entry_evaluate : slot.evaluate;
  if (!real) return NVSDK_NGX_Result_FAIL_InvalidParameter;
  if (g_shutting_down.load(std::memory_order_acquire))
    return real(commands, handle, parameters, callback);
  const bool fg = IsTracked(slot, handle);
  const NVSDK_NGX_Result result = real(commands, handle, parameters, callback);
  if (fg) {
    g_telemetry.evaluates.fetch_add(1);
    g_telemetry.evaluate_result.store(static_cast<uint32_t>(result));
    const uint64_t count = slot.evaluations.fetch_add(1, std::memory_order_relaxed) + 1;
    if (result == NVSDK_NGX_Result_Success) {
      if (count == 1) Log("DLSS-G active");
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
  const auto real = slot.entry_release ? slot.entry_release : slot.release;
  if (!real) return NVSDK_NGX_Result_FAIL_InvalidParameter;
  if (g_shutting_down.load(std::memory_order_acquire)) return real(handle);
  const bool fg = IsTracked(slot, handle);
  const NVSDK_NGX_Result result = real(handle);
  if (fg) g_telemetry.releases.fetch_add(1);
  if (result == NVSDK_NGX_Result_Success && handle != nullptr) {
    AcquireSRWLockExclusive(&slot.handles_lock);
    for (auto& entry : slot.fg_handles) {
      if (entry == handle) entry = nullptr;
    }
    ReleaseSRWLockExclusive(&slot.handles_lock);
  }
  return result;
}

template <size_t I>
inline FARPROC ResolveForRuntime(NgxRuntime& slot, const char* name, FARPROC original) {
  static_assert(std::is_same_v<decltype(&Requirements<I>), RequirementsFn>);
  static_assert(std::is_same_v<decltype(&Create<I>), CreateFn>);
  static_assert(std::is_same_v<decltype(&Evaluate<I>), EvaluateFn>);
  static_assert(std::is_same_v<decltype(&Release<I>), ReleaseFn>);

  if (std::strcmp(name, "NVSDK_NGX_D3D12_GetFeatureRequirements") == 0) {
    const auto fn = reinterpret_cast<RequirementsFn>(original);
    if (slot.requirements != nullptr && slot.requirements != fn) return original;
    slot.requirements = fn;
    return original;
  }
  if (std::strcmp(name, "NVSDK_NGX_D3D12_CreateFeature") == 0) {
    const auto fn = reinterpret_cast<CreateFn>(original);
    if (slot.create != nullptr && slot.create != fn) return original;
    slot.create = fn;
    return original;
  }
  if (std::strcmp(name, "NVSDK_NGX_D3D12_EvaluateFeature") == 0) {
    const auto fn = reinterpret_cast<EvaluateFn>(original);
    if (slot.evaluate != nullptr && slot.evaluate != fn) return original;
    slot.evaluate = fn;
    return original;
  }
  if (std::strcmp(name, "NVSDK_NGX_D3D12_ReleaseFeature") == 0) {
    const auto fn = reinterpret_cast<ReleaseFn>(original);
    if (slot.release != nullptr && slot.release != fn) return original;
    slot.release = fn;
    return original;
  }
  return original;
}

inline bool IsTargetName(const char* name) {
  if (name == nullptr || reinterpret_cast<uintptr_t>(name) <= 0xFFFF) return false;
  return std::strcmp(name, "NVSDK_NGX_D3D12_GetFeatureRequirements") == 0 ||
         std::strcmp(name, "NVSDK_NGX_D3D12_CreateFeature") == 0 ||
         std::strcmp(name, "NVSDK_NGX_D3D12_EvaluateFeature") == 0 ||
         std::strcmp(name, "NVSDK_NGX_D3D12_ReleaseFeature") == 0;
}

}  // namespace internal

// Called from the project's existing kernel32 GetProcAddress detour. This path
// only records native entry addresses. The caller always receives the original
// pointer, so cached NGX addresses remain valid after this addon unloads. Actual
// entry detours are installed later from slInit/native plugin lifecycle callbacks,
// never while a DLL-load callback may hold the loader lock.
inline FARPROC Resolve(HMODULE module, const char* name, FARPROC original) {
  if (!g_enabled.load(std::memory_order_relaxed) || original == nullptr ||
      !internal::IsTargetName(name) || !internal::IsNgxRuntime(module)) {
    return original;
  }

  AcquireSRWLockExclusive(&internal::g_resolver_lock);
  struct Unlock {
    ~Unlock() { ReleaseSRWLockExclusive(&internal::g_resolver_lock); }
  } unlock;

  size_t index = internal::g_slots.size();
  for (size_t i = 0; i < internal::g_slots.size(); ++i) {
    if (internal::g_slots[i].module == module) {
      if (!hook::IsCurrent(internal::g_slots[i].identity)) {
        // The old mapping disappeared. No detach is possible or necessary for
        // that image; discard its stale pointers before reusing the slot.
        internal::g_slots[i].module = nullptr;
        internal::g_slots[i].identity = {};
        internal::g_slots[i].requirements = nullptr;
        internal::g_slots[i].entry_requirements = nullptr;
        internal::g_slots[i].entry_create = nullptr;
        internal::g_slots[i].entry_evaluate = nullptr;
        internal::g_slots[i].entry_release = nullptr;
        internal::g_slots[i].entries_installed = false;
        internal::g_slots[i].create = nullptr;
        internal::g_slots[i].evaluate = nullptr;
        internal::g_slots[i].release = nullptr;
        internal::g_slots[i].fg_handles.fill(nullptr);
        internal::g_slots[i].evaluations.store(0);
        break;
      }
      index = i;
      break;
    }
  }
  if (index == internal::g_slots.size()) {
    for (size_t i = 0; i < internal::g_slots.size(); ++i) {
      if (internal::g_slots[i].module == nullptr) {
        hook::ModuleIdentity identity;
        if (!hook::CaptureModule(module, identity)) return original;
        internal::g_slots[i].module = module;
        internal::g_slots[i].identity = identity;
        index = i;
        break;
      }
    }
  }
  if (index == internal::g_slots.size()) {
    Log("NGX runtime limit reached", true);
    return original;
  }

  switch (index) {
    case 0: return internal::ResolveForRuntime<0>(internal::g_slots[0], name, original);
    case 1: return internal::ResolveForRuntime<1>(internal::g_slots[1], name, original);
    case 2: return internal::ResolveForRuntime<2>(internal::g_slots[2], name, original);
    case 3: return internal::ResolveForRuntime<3>(internal::g_slots[3], name, original);
    default: return original;
  }
}


namespace internal {
template <size_t I>
bool InstallEntryHooks(NgxRuntime& slot) {
  if (slot.entries_installed) return true;
  const std::vector<hook::HookItem> hooks = {
      {"NVSDK_NGX_D3D12_GetFeatureRequirements",
       reinterpret_cast<void**>(&slot.entry_requirements),
       reinterpret_cast<void*>(&Requirements<I>)},
      {"NVSDK_NGX_D3D12_CreateFeature", reinterpret_cast<void**>(&slot.entry_create),
       reinterpret_cast<void*>(&Create<I>)},
      {"NVSDK_NGX_D3D12_EvaluateFeature", reinterpret_cast<void**>(&slot.entry_evaluate),
       reinterpret_cast<void*>(&Evaluate<I>)},
      {"NVSDK_NGX_D3D12_ReleaseFeature", reinterpret_cast<void**>(&slot.entry_release),
       reinterpret_cast<void*>(&Release<I>)},
  };
  for (const auto& [name, _storage, _hook] : hooks)
    if (!GetProcAddress(slot.module, name)) return false;
  slot.entries_installed = hook::Install(slot.module, hooks, "NGX");
  if (!slot.entries_installed) {
    slot.entry_requirements = nullptr;
    slot.entry_create = nullptr;
    slot.entry_evaluate = nullptr;
    slot.entry_release = nullptr;
    return false;
  }
  g_telemetry.entry_hooked.fetch_add(1);
  return true;
}
}  // namespace internal

// Called only from slInit / real plugin lifecycle callbacks, never from the
// LoadLibrary/GetProcAddress notification. Entry detours cover cached exports.
inline void EnsureEntryHooks() {
  if (g_shutting_down.load(std::memory_order_acquire) || !g_enabled.load()) return;
  for (const auto* name : {L"_nvngx.dll", L"nvngx.dll"}) {
    HMODULE module = GetModuleHandleW(name);
    if (!module) continue;
    FARPROC fn = GetProcAddress(module, "NVSDK_NGX_D3D12_GetFeatureRequirements");
    if (fn) Resolve(module, "NVSDK_NGX_D3D12_GetFeatureRequirements", fn);
  }
  AcquireSRWLockExclusive(&internal::g_resolver_lock);
  struct Unlock {
    ~Unlock() { ReleaseSRWLockExclusive(&internal::g_resolver_lock); }
  } unlock;
  for (size_t i = 0; i < internal::g_slots.size(); ++i) {
    auto& slot = internal::g_slots[i];
    if (!slot.module || slot.entries_installed) continue;
    switch (i) {
      case 0: internal::InstallEntryHooks<0>(slot); break;
      case 1: internal::InstallEntryHooks<1>(slot); break;
      case 2: internal::InstallEntryHooks<2>(slot); break;
      case 3: internal::InstallEntryHooks<3>(slot); break;
    }
  }
}

inline void BeforeInit() {
  if (g_shutting_down.load(std::memory_order_acquire)) return;
  try {
    internal::ProbeAdapterBeforeInit();
    EnsureEntryHooks();
    if (g_device_confirmed.load() && g_prepare_loaded_providers) g_prepare_loaded_providers();
  } catch (...) {
    Log("NGX preflight failed", true);
  }
}

namespace internal {
template <size_t I>
void UninstallEntryHooks(NgxRuntime& slot) {
  if (!slot.entries_installed) return;
  if (hook::IsCurrent(slot.identity)) {
    const std::vector<hook::HookItem> hooks = {
        {"NVSDK_NGX_D3D12_GetFeatureRequirements",
         reinterpret_cast<void**>(&slot.entry_requirements),
         reinterpret_cast<void*>(&Requirements<I>)},
        {"NVSDK_NGX_D3D12_CreateFeature", reinterpret_cast<void**>(&slot.entry_create),
         reinterpret_cast<void*>(&Create<I>)},
        {"NVSDK_NGX_D3D12_EvaluateFeature", reinterpret_cast<void**>(&slot.entry_evaluate),
         reinterpret_cast<void*>(&Evaluate<I>)},
        {"NVSDK_NGX_D3D12_ReleaseFeature", reinterpret_cast<void**>(&slot.entry_release),
         reinterpret_cast<void*>(&Release<I>)},
    };
    hook::Uninstall(hooks);
  } else {
    // Windows already unmapped the code that carried these entry detours.
    slot.entry_requirements = nullptr;
    slot.entry_create = nullptr;
    slot.entry_evaluate = nullptr;
    slot.entry_release = nullptr;
  }
  slot.entries_installed = false;
}
}  // namespace internal

inline void Shutdown() {
  g_shutting_down.store(true, std::memory_order_release);
  AcquireSRWLockExclusive(&internal::g_resolver_lock);
  for (size_t i = internal::g_slots.size(); i-- > 0;) {
    auto& slot = internal::g_slots[i];
    switch (i) {
      case 0: internal::UninstallEntryHooks<0>(slot); break;
      case 1: internal::UninstallEntryHooks<1>(slot); break;
      case 2: internal::UninstallEntryHooks<2>(slot); break;
      case 3: internal::UninstallEntryHooks<3>(slot); break;
    }
    slot.module = nullptr;
    slot.identity = {};
    slot.requirements = nullptr;
    slot.create = nullptr;
    slot.evaluate = nullptr;
    slot.release = nullptr;
    slot.fg_handles.fill(nullptr);
    slot.evaluations.store(0);
  }
  ReleaseSRWLockExclusive(&internal::g_resolver_lock);
  hook::UninstallAddress(internal::g_arch_hook);
  internal::g_single_gpu.store(nullptr);
  g_device_confirmed.store(false);
}
}  // namespace mfgunlock::ampere::ngx
