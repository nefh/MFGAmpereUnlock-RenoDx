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

#include "./provider.hpp"
#include "./capability_policy.hpp"
#include "./ngx_hook.hpp"
#include "./count_observer.hpp"
#include "./diagnostic_bridge.hpp"

namespace mfgunlock::ngx {

using architecture::kNvidiaVendorId;
using policy::CanExposeArchitecture;
using policy::CanRelaxRequirements;
using policy::CapabilityFrameCount;
using policy::RequirementsEvidence;
using policy::kDlssGFeatureId;
using provider::GetProviderStatus;
using provider::Log;
using provider::PreparedProviderCount;
using provider::g_create_seen;

// Set by addon.cpp. This deliberately reuses the addon's existing provider
// inventory and maintenance lock instead of maintaining a second module model.
inline void (*g_prepare_loaded_providers)() = nullptr;
// Optional startup barrier. It never treats feature release as CUDA cache unload.
using BeforeFgCreate = bool (*)();
inline std::atomic<BeforeFgCreate> g_before_fg_create{nullptr};
// Zero leaves capability values alone. The addon shares this readiness decision
// with its existing frame-count path.
inline unsigned int (*g_capability_limit)() = nullptr;
// Zero means there is no active Streamline structural ceiling (for example,
// direct NGX). A known Streamline plugin publishes its compiled ceiling here.
inline unsigned int (*g_streamline_ceiling)() = nullptr;

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
  std::wstring path(32768, L'\0');
  const DWORD length = GetModuleFileNameW(module, path.data(), static_cast<DWORD>(path.size()));
  if (length == 0 || length >= path.size()) return false;
  const wchar_t* slash = std::wcsrchr(path.c_str(), L'\\');
  const wchar_t* name = slash == nullptr ? path.c_str() : slash + 1;
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
  std::array<bool, 16> fg_active{};
};

inline std::array<NgxRuntime, 4> g_slots;
inline SRWLOCK g_resolver_lock = SRWLOCK_INIT;
inline thread_local bool g_in_requirements = false;
inline thread_local bool g_in_capabilities = false;
inline thread_local bool g_installing = false;

struct FgLifecycle {
  uint32_t handles = 0;
  uint32_t evaluates = 0;
  uint32_t creates = 0;
  uint32_t releases = 0;
  uint64_t epoch = 0;
  uint64_t sequence = 0;
  uint64_t last_create = 0;
  uint64_t last_release = 0;
  bool zero_boundary = false;
  bool uncertain = false;
};
inline std::mutex g_lifecycle_lock;
inline FgLifecycle g_lifecycle;

inline FgLifecycle LifecycleSnapshot() {
  std::lock_guard lock(g_lifecycle_lock);
  return g_lifecycle;
}
inline bool IsTrackedLocked(const NgxRuntime& slot, const NVSDK_NGX_Handle* handle) {
  return handle && std::find(slot.fg_handles.begin(), slot.fg_handles.end(), handle) != slot.fg_handles.end();
}
inline bool IsTracked(NgxRuntime& slot, const NVSDK_NGX_Handle* handle) {
  std::lock_guard lock(g_lifecycle_lock);
  return IsTrackedLocked(slot, handle);
}
inline void UpdateFeatureStatus() {
  bool active = false;
  uint32_t handles = 0;
  for (const auto& slot : g_slots) {
    for (size_t i = 0; i < slot.fg_handles.size(); ++i) {
      if (!slot.fg_handles[i]) continue;
      ++handles;
      active |= slot.fg_active[i];
    }
  }
  g_lifecycle.handles = handles;
  g_status.feature_created.store(handles != 0, std::memory_order_release);
  g_status.feature_active.store(active, std::memory_order_release);
}
inline diagnostic::LifecycleEvent LifecycleEvent(diagnostic::LifecycleKind kind,
                                                 const NVSDK_NGX_Handle* handle = nullptr) {
  diagnostic::LifecycleEvent event;
  event.kind = kind;
  event.thread = GetCurrentThreadId();
  event.epoch = g_lifecycle.epoch;
  event.handle = reinterpret_cast<uint64_t>(handle);
  event.handles = g_lifecycle.handles;
  event.evaluates = g_lifecycle.evaluates;
  event.creates = g_lifecycle.creates;
  event.releases = g_lifecycle.releases;
  event.tracking_uncertain = g_lifecycle.uncertain;
  return event;
}

enum class FgCallKind { kCreate, kEvaluate, kRelease };
struct FgCall {
  NgxRuntime& slot;
  FgCallKind kind;
  const NVSDK_NGX_Handle* handle;
  bool tracked;
  bool finished = false;

  FgCall(NgxRuntime& runtime, FgCallKind type, const NVSDK_NGX_Handle* object = nullptr)
      : slot(runtime), kind(type), handle(object) {
    std::lock_guard lock(g_lifecycle_lock);
    tracked = kind == FgCallKind::kCreate || IsTrackedLocked(slot, handle);
    if (tracked) ++Counter();
  }
  uint32_t& Counter() {
    if (kind == FgCallKind::kCreate) return g_lifecycle.creates;
    if (kind == FgCallKind::kEvaluate) return g_lifecycle.evaluates;
    return g_lifecycle.releases;
  }
  void Finish(NVSDK_NGX_Result result, const NVSDK_NGX_Handle* created = nullptr) {
    if (!tracked) return;
    const DWORD last_error = GetLastError();
    struct RestoreError {
      DWORD value;
      ~RestoreError() { SetLastError(value); }
    } restore_error{last_error};
    diagnostic::LifecycleEvent event;
    bool zero = false;
    bool lost = false;
    {
      std::lock_guard lock(g_lifecycle_lock);
      --Counter();
      finished = true;
      const bool success = result == NVSDK_NGX_Result_Success;
      const auto previous_handles = g_lifecycle.handles;
      const bool previously_uncertain = g_lifecycle.uncertain;
      if (kind == FgCallKind::kCreate) {
        g_lifecycle.last_create = ++g_lifecycle.sequence;
        if (success) {
          const auto free = std::find(slot.fg_handles.begin(), slot.fg_handles.end(), nullptr);
          if (!created || IsTrackedLocked(slot, created) || free == slot.fg_handles.end()) {
            g_lifecycle.uncertain = true;
          } else {
            const size_t i = static_cast<size_t>(free - slot.fg_handles.begin());
            slot.fg_handles[i] = created;
            slot.fg_active[i] = false;
            if (previous_handles == 0) ++g_lifecycle.epoch;
          }
        }
      } else if (kind == FgCallKind::kRelease && success) {
        for (size_t i = 0; i < slot.fg_handles.size(); ++i) {
          if (slot.fg_handles[i] != handle) continue;
          slot.fg_handles[i] = nullptr;
          slot.fg_active[i] = false;
        }
        g_lifecycle.last_release = ++g_lifecycle.sequence;
      } else if (kind == FgCallKind::kEvaluate) {
        for (size_t i = 0; i < slot.fg_handles.size(); ++i) {
          if (slot.fg_handles[i] == handle) slot.fg_active[i] = success;
        }
      }
      UpdateFeatureStatus();
      zero = kind == FgCallKind::kRelease && success && previous_handles != 0 &&
          g_lifecycle.handles == 0 && !g_lifecycle.uncertain;
      if (zero) g_lifecycle.zero_boundary = true;
      if (kind == FgCallKind::kCreate && success) g_lifecycle.zero_boundary = false;
      event = LifecycleEvent(kind == FgCallKind::kCreate ? diagnostic::LifecycleKind::kCreated
                                                       : diagnostic::LifecycleKind::kReleased,
                             kind == FgCallKind::kCreate ? created : handle);
      event.result = static_cast<uint32_t>(result);
      lost = !previously_uncertain && g_lifecycle.uncertain;
    }
    if (kind == FgCallKind::kEvaluate) return;
    diagnostic::Emit(event);
    if (lost) {
      event.kind = diagnostic::LifecycleKind::kTrackingLost;
      diagnostic::Emit(event);
    }
    if (zero) {
      event.kind = diagnostic::LifecycleKind::kZeroHandles;
      diagnostic::Emit(event);
    }
  }
  ~FgCall() {
    if (!tracked || finished) return;
    std::lock_guard lock(g_lifecycle_lock);
    --Counter();
    g_lifecycle.uncertain = true;
    g_lifecycle.zero_boundary = false;
  }
  FgCall(const FgCall&) = delete;
  FgCall& operator=(const FgCall&) = delete;
};


inline void SnapshotUnsigned(const NVSDK_NGX_Parameter* parameters, const char* name,
                             uint32_t& known, uint32_t& value) {
  if (!parameters || !name) return;
  unsigned int read = 0;
  if (parameters->Get(name, &read) != NVSDK_NGX_Result_Success) return;
  known = 1;
  value = read;
}

inline void SnapshotUnsigned64(const NVSDK_NGX_Parameter* parameters, const char* name,
                               uint32_t& known, uint64_t& value) {
  if (!parameters || !name) return;
  unsigned long long read = 0;
  if (parameters->Get(name, &read) == NVSDK_NGX_Result_Success) {
    known = 1;
    value = static_cast<uint64_t>(read);
    return;
  }
  unsigned int read32 = 0;
  if (parameters->Get(name, &read32) != NVSDK_NGX_Result_Success) return;
  known = 1;
  value = read32;
}

inline void SnapshotResource(const NVSDK_NGX_Parameter* parameters, const char* name,
                             diagnostic::ResourceKey key,
                             diagnostic::ResourceSnapshot& output) {
  output.key = static_cast<uint32_t>(key);
  if (!parameters || !name) return;
  ID3D12Resource* resource = nullptr;
  if (parameters->Get(name, &resource) != NVSDK_NGX_Result_Success || !resource) return;
  const auto desc = resource->GetDesc();
  output.known = 1;
  output.object = reinterpret_cast<uint64_t>(resource);
  output.width = desc.Width;
  output.height = desc.Height;
  output.depth_or_array = desc.DepthOrArraySize;
  output.mip_levels = desc.MipLevels;
  output.format = static_cast<uint32_t>(desc.Format);
  output.dimension = static_cast<uint32_t>(desc.Dimension);
  output.flags = static_cast<uint32_t>(desc.Flags);
  output.sample_count = desc.SampleDesc.Count;
}

inline void SnapshotEvaluate(const NVSDK_NGX_Parameter* parameters,
                             diagnostic::EvaluateEvent& event) {
  SnapshotUnsigned(parameters, "DLSSG.MultiFrameCount", event.generated_count_known,
                   event.generated_count);
  SnapshotUnsigned(parameters, "DLSSG.MultiFrameIndex", event.generated_index_known,
                   event.generated_index);
  SnapshotUnsigned(parameters, "DLSSG.Reset", event.reset_known, event.reset);
  SnapshotUnsigned(parameters, "DLSSG.AutomodeOverrideReset", event.automode_reset_known,
                   event.automode_reset);
  SnapshotUnsigned64(parameters, "DLSSG.BackbufferFrameID", event.backbuffer_frame_id_known,
                     event.backbuffer_frame_id);
  struct ResourceName {
    const char* name;
    diagnostic::ResourceKey key;
  };
  static constexpr ResourceName kResources[] = {
      {"DLSSG.Backbuffer", diagnostic::ResourceKey::kBackbuffer},
      {"DLSSG.Depth", diagnostic::ResourceKey::kDepth},
      {"DLSSG.MVecs", diagnostic::ResourceKey::kMotionVectors},
      {"DLSSG.HUDLess", diagnostic::ResourceKey::kHudLess},
      {"DLSSG.UI", diagnostic::ResourceKey::kUi},
      {"DLSSG.UIAlpha", diagnostic::ResourceKey::kUiAlpha},
      {"DLSSG.BidirectionalDistortionField", diagnostic::ResourceKey::kBidirectionalDistortionField},
      {"DLSSG.OutputInterpolated", diagnostic::ResourceKey::kOutputInterpolated},
      {"DLSSG.OutputReal", diagnostic::ResourceKey::kOutputReal},
      {"DLSSG.OutputDisableInterpolation", diagnostic::ResourceKey::kOutputDisableInterpolation},
  };
  for (size_t i = 0; i < std::size(kResources); ++i)
    SnapshotResource(parameters, kResources[i].name, kResources[i].key, event.resources[i]);
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
  if (!fg) return real(commands, feature, parameters, handle);
  const auto before_create = g_before_fg_create.load(std::memory_order_acquire);
  if (before_create && !before_create()) {
    if (handle) *handle = nullptr;
    return NVSDK_NGX_Result_FAIL_FeatureNotSupported;
  }
  g_create_seen.store(true, std::memory_order_release);
  FgCall call(slot, FgCallKind::kCreate);
  const NVSDK_NGX_Result result = real(commands, feature, parameters, handle);
  call.Finish(result, result == NVSDK_NGX_Result_Success && handle ? *handle : nullptr);
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
  FgCall call(slot, FgCallKind::kEvaluate, handle);
  const bool fg = call.tracked;
  const auto observer = diagnostic::g_evaluate_callback.load(std::memory_order_acquire);
  diagnostic::EvaluateEvent observed;
  if (fg && observer) {
    struct LastError {
      DWORD value = GetLastError();
      ~LastError() { SetLastError(value); }
    } last_error;
    observed.phase = diagnostic::EvaluatePhase::kBegin;
    observed.thread = GetCurrentThreadId();
    observed.evaluation_id = diagnostic::g_evaluation_sequence.fetch_add(1, std::memory_order_relaxed) + 1;
    observed.caller = countobserver::CallerAddress();
    observed.commands = reinterpret_cast<uint64_t>(commands);
    observed.handle = reinterpret_cast<uint64_t>(handle);
    observed.parameters = reinterpret_cast<uint64_t>(parameters);
    SnapshotEvaluate(parameters, observed);
    observer(&observed);
  }
  const NVSDK_NGX_Result result = real(commands, handle, parameters, callback);
  if (fg && observer) {
    struct LastError {
      DWORD value = GetLastError();
      ~LastError() { SetLastError(value); }
    } last_error;
    observed.phase = diagnostic::EvaluatePhase::kEnd;
    observed.result = static_cast<uint32_t>(result);
    observer(&observed);
  }
  call.Finish(result);
  if (fg && result != NVSDK_NGX_Result_Success) {
    std::stringstream message;
    message << "DLSS-G EvaluateFeature failed (0x" << std::hex
            << static_cast<uint32_t>(result) << ')';
    Log(message.str(), true);
  }
  return result;
}

template <size_t I>
NVSDK_NGX_Result NVSDK_CONV Release(NVSDK_NGX_Handle* handle) {
  auto& slot = g_slots[I];
  const auto real = slot.entry_release;
  if (!real) return NVSDK_NGX_Result_FAIL_InvalidParameter;
  if (g_shutting_down.load(std::memory_order_acquire)) return real(handle);
  FgCall call(slot, FgCallKind::kRelease, handle);
  const NVSDK_NGX_Result result = real(handle);
  call.Finish(result);
  return result;
}

// Update the vendor-owned block before the caller caches capabilities. This
// covers both signed and unsigned Get overloads without replacing its vtable.
inline void ObserveCapabilityCount(NVSDK_NGX_Parameter* parameters, int value,
                                   NVSDK_NGX_Result result, countobserver::Operation operation,
                                   countobserver::Backend backend, uint64_t caller) {
  countobserver::Event event;
  event.backend = backend;
  event.operation = operation;
  event.origin = countobserver::Origin::kCapabilityPolicy;
  event.key = countobserver::Key::kMaximum;
  event.value = value;
  event.value_known = operation == countobserver::Operation::kWrite || result == NVSDK_NGX_Result_Success;
  event.result = operation == countobserver::Operation::kWrite
      ? countobserver::kUnknown : static_cast<uint32_t>(result);
  event.object = reinterpret_cast<uint64_t>(parameters);
  event.caller = caller;
  countobserver::Emit(event);
}

inline void ApplyCapabilities(NVSDK_NGX_Parameter* parameters, unsigned int limit,
                              unsigned int structural_ceiling = 0,
                              countobserver::Backend backend = countobserver::Backend::kNgxD3D12,
                              uint64_t caller = 0) {
  if (!parameters) return;
  int available = 0;
  int maximum = 0;
  const auto available_result = parameters->Get("FrameGeneration.Available", &available);
  const auto maximum_result = parameters->Get("DLSSG.MultiFrameCountMax", &maximum);
  ObserveCapabilityCount(parameters, maximum, maximum_result, countobserver::Operation::kRead, backend, caller);
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
    const auto wanted = CapabilityFrameCount(maximum, limit, structural_ceiling);
    if (wanted != maximum) {
      parameters->Set("DLSSG.MultiFrameCountMax", wanted);
      ObserveCapabilityCount(parameters, wanted, NVSDK_NGX_Result_Success,
                             countobserver::Operation::kWrite, backend, caller);
      int stored = maximum;
      const auto readback = parameters->Get("DLSSG.MultiFrameCountMax", &stored);
      ObserveCapabilityCount(parameters, stored, readback, countobserver::Operation::kRead, backend, caller);
      if (readback == NVSDK_NGX_Result_Success && stored != maximum) {
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
    ApplyCapabilities(*output, g_capability_limit ? g_capability_limit() : 0,
                      g_streamline_ceiling ? g_streamline_ceiling() : 0,
                      Api >= 2 ? countobserver::Backend::kNgxVulkan : countobserver::Backend::kNgxD3D12,
                      countobserver::CallerAddress());
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
  std::lock_guard lock(g_lifecycle_lock);
  if (std::any_of(slot.fg_handles.begin(), slot.fg_handles.end(),
                  [](const auto* handle) { return handle != nullptr; })) {
    g_lifecycle.uncertain = true;
    g_lifecycle.zero_boundary = false;
  }
  slot.fg_handles.fill(nullptr);
  slot.fg_active.fill(false);
  UpdateFeatureStatus();
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
}  // namespace mfgunlock::ngx
