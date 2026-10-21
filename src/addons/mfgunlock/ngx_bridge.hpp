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
#include "./runtime_controls.hpp"
#include "./feature_capabilities.hpp"
#include "./ngx_hook.hpp"
#include "./count_observer.hpp"
#include "./diagnostic_bridge.hpp"
#include "./integration_contract.hpp"
#include "./vulkan_native.hpp"

// Vulkan dispatchable handles are opaque pointers in the public NGX ABI.
struct VkDevice_T;
struct VkCommandBuffer_T;

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
  std::atomic<uint64_t> native_maximum{runtimecontrols::kUnknownMaximum};
  std::atomic_bool feature_created{false};
  std::atomic_bool feature_active{false};
};
inline Status g_status;
inline std::atomic_bool g_shutting_down{false};

namespace internal {

// Diagnostic build: bounded startup evidence; never rewrites an API result.
struct DiscoveryLastError {
  DWORD value = GetLastError();
  ~DiscoveryLastError() { SetLastError(value); }
};
template <class Writer>
inline void DiscoveryTrace(Writer writer) noexcept {
  DiscoveryLastError error;
  try {
    std::stringstream message;
    message << "DLSSDISC_D1 ";
    writer(message);
    Log(message.str());
  } catch (...) {}
}
inline std::atomic_uint32_t g_discovery_capability_calls{0};
inline std::atomic_uint32_t g_discovery_requirement_calls{0};
inline std::atomic_uint32_t g_discovery_other_creates{0};
inline void TraceCapabilityBlock(const char* api, const char* phase,
                                 NVSDK_NGX_Parameter* parameters, uint32_t call) noexcept {
  if (!parameters || call > 8) return;
  DiscoveryTrace([&](auto& message) {
    message << "NGX_CAPS api=" << api << " call=" << call << " phase=" << phase;
    for (const char* name : {"SuperSampling.Available", "SuperSampling.NeedsUpdatedDriver",
                            "SuperSampling.FeatureInitResult", "FrameGeneration.Available",
                            "FrameGeneration.NeedsUpdatedDriver", "FrameGeneration.FeatureInitResult"}) {
      int value = 0;
      const auto result = parameters->Get(name, &value);
      message << ' ' << name << "={get:0x" << std::hex << static_cast<uint32_t>(result);
      if (result == NVSDK_NGX_Result_Success) message << ",value:0x" << static_cast<uint32_t>(value);
      message << '}' << std::dec;
    }
  });
}

inline bool NeedsCapabilityBridge() {
  // Typed native calls and observations do not depend on a retarget recipe.
  return g_enabled.load(std::memory_order_relaxed);
}

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
  const auto native_architecture = info->architecture;
  const auto* profile = architecture::ActiveProfile();
  if (CanExposeArchitecture(g_enabled.load(), true, gpu == g_arch_scope,
                            PreparedProviderCount() == 1, result, profile)) {
    info->architecture = profile->exposed_arch;
  }
  static std::atomic_uint32_t trace_calls{0};
  if (trace_calls.fetch_add(1) < 32) DiscoveryTrace([&](auto& message) {
    message << "ARCH_QUERY gpu=" << gpu << " scope=" << g_arch_scope
            << " native=0x" << std::hex << native_architecture
            << " returned=0x" << info->architecture;
  });
  return result;
}
static_assert(std::is_same_v<decltype(&HookedGetArchInfo), decltype(&NvAPI_GPU_GetArchInfo)>);

inline void EnsureArchitectureHook() {
  const auto* profile = architecture::ActiveProfile();
  if (!g_enabled.load() || !profile || !profile->RequiresProviderRetarget()) return;
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

inline NvPhysicalGpuHandle MatchAdapterLuid(const LUID& luid, uint32_t vendor) {
  if (vendor != kNvidiaVendorId) return nullptr;
  AcquireSRWLockExclusive(&g_adapter_lock);
  struct Unlock {
    ~Unlock() { ReleaseSRWLockExclusive(&g_adapter_lock); }
  } unlock;
  if (const auto bound = g_bound_gpu.load()) {
    return luid.LowPart == g_luid_low.load() &&
           static_cast<uint32_t>(luid.HighPart) == g_luid_high.load() ? bound : nullptr;
  }

  auto& api = GetNvapi();
  if (!api.ready) return nullptr;
  NvPhysicalGpuHandle physical[NVAPI_MAX_PHYSICAL_GPUS] = {};
  NvU32 count = 0;
  if (api.enumerate(physical, &count) != NVAPI_OK || count == 0 || count > NVAPI_MAX_PHYSICAL_GPUS)
    return nullptr;
  NvPhysicalGpuHandle matched = nullptr;
  for (NvU32 i = 0; i < count; ++i) {
    LUID current_luid = {};
    if (api.adapter_id(physical[i], &current_luid) != NVAPI_OK ||
        std::memcmp(&current_luid, &luid, sizeof(luid)) != 0) continue;
    if (matched) return nullptr;
    matched = physical[i];
  }
  if (!matched) return nullptr;

  // Explicit profiles are authoritative. Auto reads the original NVAPI value.
  if (!ResolveArchitecture(matched, vendor)) return nullptr;
  g_luid_low.store(luid.LowPart);
  g_luid_high.store(static_cast<uint32_t>(luid.HighPart));
  g_bound_gpu.store(matched);
  return matched;
}

inline NvPhysicalGpuHandle MatchAdapter(IDXGIAdapter* adapter) {
  if (!adapter) return nullptr;
  DXGI_ADAPTER_DESC desc = {};
  if (FAILED(adapter->GetDesc(&desc))) return nullptr;
  return MatchAdapterLuid(desc.AdapterLuid, desc.VendorId);
}

inline NvPhysicalGpuHandle MatchVulkanAdapter(void* instance, void* physical) {
  const auto identity = vulkannative::Identify(instance, physical);
  return identity.valid ? MatchAdapterLuid(identity.luid, identity.vendor) : nullptr;
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
  if (!module) return false;
  if (IsModuleNamed(module, L"_nvngx.dll") || IsModuleNamed(module, L"nvngx.dll")) return true;
  // Public parameter APIs distinguish the NGX runtime from provider-private
  // CreateFeature exports, which can have a different signature.
  return GetProcAddress(module, "NVSDK_NGX_D3D12_GetCapabilityParameters") ||
      GetProcAddress(module, "NVSDK_NGX_D3D12_GetParameters") ||
      GetProcAddress(module, "NVSDK_NGX_VULKAN_GetCapabilityParameters") ||
      GetProcAddress(module, "NVSDK_NGX_VULKAN_GetParameters");
}

using RequirementsFn = decltype(&NVSDK_NGX_D3D12_GetFeatureRequirements);
using CreateFn = decltype(&NVSDK_NGX_D3D12_CreateFeature);
using EvaluateFn = decltype(&NVSDK_NGX_D3D12_EvaluateFeature);
using ReleaseFn = decltype(&NVSDK_NGX_D3D12_ReleaseFeature);
using VulkanCreateFn = NVSDK_NGX_Result(NVSDK_CONV*)(
    VkCommandBuffer_T*, NVSDK_NGX_Feature, NVSDK_NGX_Parameter*, NVSDK_NGX_Handle**);
using VulkanCreate1Fn = NVSDK_NGX_Result(NVSDK_CONV*)(
    VkDevice_T*, VkCommandBuffer_T*, NVSDK_NGX_Feature, NVSDK_NGX_Parameter*, NVSDK_NGX_Handle**);
using VulkanEvaluateFn = NVSDK_NGX_Result(NVSDK_CONV*)(
    VkCommandBuffer_T*, const NVSDK_NGX_Handle*, const NVSDK_NGX_Parameter*,
    PFN_NVSDK_NGX_ProgressCallback);
// D3D12 and Vulkan GetParameters/GetCapabilityParameters share this signature.
using ParametersFn = NVSDK_NGX_Result(NVSDK_CONV*)(NVSDK_NGX_Parameter**);
static_assert(std::is_same_v<ParametersFn, decltype(&NVSDK_NGX_D3D12_GetCapabilityParameters)>);
inline constexpr std::array<const char*, 4> kParameterExports = {
    "NVSDK_NGX_D3D12_GetCapabilityParameters", "NVSDK_NGX_D3D12_GetParameters",
    "NVSDK_NGX_VULKAN_GetCapabilityParameters", "NVSDK_NGX_VULKAN_GetParameters"};
// Vulkan dispatchable handles use the public header ABI; no Vulkan loader
// is linked or replaced by this addon.
// The function is resolved dynamically from _nvngx.dll/nvngx.dll.
using VulkanRequirementsFn = NVSDK_NGX_Result(NVSDK_CONV*)(
    VkInstance instance, VkPhysicalDevice physical_device,
    const NVSDK_NGX_FeatureDiscoveryInfo* discovery,
    NVSDK_NGX_FeatureRequirement* output);

struct NgxRuntime {
  HMODULE module = nullptr;
  hook::ModuleIdentity identity{};
  RequirementsFn entry_requirements = nullptr;
  CreateFn entry_create = nullptr;
  EvaluateFn entry_evaluate = nullptr;
  ReleaseFn entry_release = nullptr;
  VulkanCreateFn entry_vulkan_create = nullptr;
  VulkanCreate1Fn entry_vulkan_create1 = nullptr;
  VulkanEvaluateFn entry_vulkan_evaluate = nullptr;
  ReleaseFn entry_vulkan_release = nullptr;
  VulkanRequirementsFn entry_vulkan_requirements = nullptr;
  std::array<ParametersFn, 4> parameters{};
  std::array<std::atomic_bool, 4> parameters_seen{};
  std::array<const NVSDK_NGX_Handle*, 16> fg_handles{};
  std::array<bool, 16> fg_active{};
  // Guarded by g_lifecycle_lock. Pointer equality is not an object lifetime.
  uint64_t generation = 1;
  std::array<uint64_t, 16> fg_generation{};
  std::array<diagnostic::Backend, 16> fg_backend{};
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
using VulkanImages = std::array<vulkancontract::Image, static_cast<size_t>(diagnostic::ResourceKey::kCount)>;
struct VulkanObservation {
  uint64_t generation = 1, create_ok = 0, evaluate_ok = 0, release_ok = 0, failures = 0;
  uint32_t last_result = UINT32_MAX;
  bool snapshot_current = false;
  diagnostic::EvaluateEvent last_evaluate{};
  VulkanImages images{};
};
inline VulkanObservation g_vulkan_observation; // Protected by existing lifecycle lock.
inline VulkanObservation ReadVulkanObservation() {
  std::lock_guard lock(g_lifecycle_lock);
  return g_vulkan_observation;
}
inline void ResetVulkanObservationLocked() {
  const auto generation = g_vulkan_observation.generation + 1;
  g_vulkan_observation = {};
  g_vulkan_observation.generation = generation;
  vulkannative::ResetIdentity();
}

// Never reset on device/feature recreation. Zero denotes an untracked object.
inline uint64_t g_feature_generation = 0;

// Bounded Turing diagnostic v2 tracing. BEGIN without a matching END identifies
// a vendor call that did not return without flooding the per-frame log.
inline std::atomic_uint32_t g_turing_diag_create_calls{0};
inline std::atomic_uint32_t g_turing_diag_evaluate_calls{0};
inline std::atomic_uint32_t g_turing_diag_release_calls{0};
inline constexpr uint32_t kTuringDiagNgxTraceLimit = 8;

inline void TuringDiagLog(const std::string& message, bool warning = false) {
  const DWORD last_error = GetLastError();
  Log(message, warning);
  SetLastError(last_error);
}

inline FgLifecycle LifecycleSnapshot() {
  std::lock_guard lock(g_lifecycle_lock);
  return g_lifecycle;
}
inline bool IsTrackedLocked(const NgxRuntime& slot, const NVSDK_NGX_Handle* handle,
                            diagnostic::Backend backend = diagnostic::Backend::kNgxD3D12) {
  if (handle == nullptr) return false;
  for (size_t i = 0; i < slot.fg_handles.size(); ++i) {
    if (slot.fg_handles[i] == handle && slot.fg_backend[i] == backend) return true;
  }
  return false;
}
inline bool IsTracked(NgxRuntime& slot, const NVSDK_NGX_Handle* handle,
                      diagnostic::Backend backend = diagnostic::Backend::kNgxD3D12) {
  std::lock_guard lock(g_lifecycle_lock);
  return IsTrackedLocked(slot, handle, backend);
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
  diagnostic::Backend backend;
  bool tracked;
  bool finished = false;
  bool completion_current = false;
  uint64_t runtime_generation = 0;
  uint64_t feature_generation = 0;
  size_t handle_index = SIZE_MAX;

  FgCall(NgxRuntime& runtime, FgCallKind type, const NVSDK_NGX_Handle* object = nullptr,
         diagnostic::Backend api = diagnostic::Backend::kNgxD3D12)
      : slot(runtime), kind(type), handle(object), backend(api) {
    std::lock_guard lock(g_lifecycle_lock);
    runtime_generation = slot.generation;
    for (size_t i = 0; handle && i < slot.fg_handles.size(); ++i) {
      if (slot.fg_handles[i] != handle || slot.fg_backend[i] != backend) continue;
      handle_index = i;
      feature_generation = slot.fg_generation[i];
      break;
    }
    tracked = kind == FgCallKind::kCreate || handle_index < slot.fg_handles.size();
    if (tracked) ++Counter();
  }
  bool CurrentLocked() const {
    if (!tracked || slot.generation != runtime_generation) return false;
    return kind == FgCallKind::kCreate ||
        (handle_index < slot.fg_handles.size() &&
         slot.fg_handles[handle_index] == handle &&
         slot.fg_backend[handle_index] == backend &&
         slot.fg_generation[handle_index] == feature_generation);
  }
  void PublishBegin(const diagnostic::EvaluateEvent& event);
  void PublishEnd(const diagnostic::EvaluateEvent& event, NVSDK_NGX_Result result,
                  const VulkanImages* images = nullptr);
  uint32_t& Counter() {
    if (kind == FgCallKind::kCreate) return g_lifecycle.creates;
    if (kind == FgCallKind::kEvaluate) return g_lifecycle.evaluates;
    return g_lifecycle.releases;
  }
  void Finish(NVSDK_NGX_Result result, const NVSDK_NGX_Handle* created = nullptr) {
    if (!tracked || finished) return;
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
      completion_current = CurrentLocked();
      if (completion_current && backend == diagnostic::Backend::kNgxVulkan) {
        auto& vk = g_vulkan_observation;
        vk.last_result = static_cast<uint32_t>(result);
        if (!success) ++vk.failures;
        else if (kind == FgCallKind::kCreate) ++vk.create_ok;
        else if (kind == FgCallKind::kEvaluate) ++vk.evaluate_ok;
        else ++vk.release_ok;
        if (success && kind != FgCallKind::kEvaluate) {
          // Descriptors borrowed from a former feature are not live resources.
          vk.last_evaluate = {};
          vk.images = {};
          vk.snapshot_current = false;
        }
      }

      if (!completion_current) {
        // A late completion belongs to the old generation, never its successor.
        // Keep the native API result, but do not register/activate/release it.
        g_lifecycle.uncertain = true;
        g_lifecycle.zero_boundary = false;
      } else if (kind == FgCallKind::kCreate) {
        g_lifecycle.last_create = ++g_lifecycle.sequence;
        if (success) {
          integration::InvalidateNgxFeatureEvidence();
          const auto free = std::find(slot.fg_handles.begin(), slot.fg_handles.end(), nullptr);
          if (!created || IsTrackedLocked(slot, created, backend) || free == slot.fg_handles.end() ||
              g_feature_generation == UINT64_MAX) {
            g_lifecycle.uncertain = true;
          } else {
            const size_t i = static_cast<size_t>(free - slot.fg_handles.begin());
            slot.fg_handles[i] = created;
            slot.fg_generation[i] = ++g_feature_generation;
            slot.fg_backend[i] = backend;
            slot.fg_active[i] = false;
            if (previous_handles == 0) ++g_lifecycle.epoch;
          }
        }
      } else if (kind == FgCallKind::kRelease && success) {
        integration::InvalidateNgxFeatureEvidence();
        for (size_t i = 0; i < slot.fg_handles.size(); ++i) {
          if (slot.fg_handles[i] != handle || slot.fg_backend[i] != backend) continue;
          slot.fg_handles[i] = nullptr;
          slot.fg_generation[i] = 0;
          slot.fg_active[i] = false;
        }
        g_lifecycle.last_release = ++g_lifecycle.sequence;
      } else if (kind == FgCallKind::kEvaluate) {
        for (size_t i = 0; i < slot.fg_handles.size(); ++i) {
          if (slot.fg_handles[i] == handle && slot.fg_backend[i] == backend) slot.fg_active[i] = success;
        }
      }
      UpdateFeatureStatus();
      zero = completion_current && kind == FgCallKind::kRelease && success && previous_handles != 0 &&
          g_lifecycle.handles == 0 && !g_lifecycle.uncertain;
      if (zero) g_lifecycle.zero_boundary = true;
      if (kind == FgCallKind::kCreate && success) g_lifecycle.zero_boundary = false;
      event = LifecycleEvent(kind == FgCallKind::kCreate ? diagnostic::LifecycleKind::kCreated
                                                       : diagnostic::LifecycleKind::kReleased,
                             kind == FgCallKind::kCreate ? created : handle);
      event.result = static_cast<uint32_t>(result);
      lost = !previously_uncertain && g_lifecycle.uncertain;
    }
    if (kind != FgCallKind::kEvaluate) diagnostic::Emit(event);
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
  if (parameters->Get(name, &resource) != NVSDK_NGX_Result_Success) return;
  output.known = 1;
  if (!resource) return;  // A successful null tag clears prior evidence.
  const auto desc = resource->GetDesc();
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

inline void SnapshotEvaluateScalars(const NVSDK_NGX_Parameter* parameters,
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
}

inline void ObserveEvaluateHistory(const diagnostic::EvaluateEvent& event) {
  const bool both_resets_known = event.reset_known != 0 && event.automode_reset_known != 0;
  const bool reset = (event.reset_known != 0 && event.reset != 0) ||
      (event.automode_reset_known != 0 && event.automode_reset != 0);
  integration::ObserveHistory(reset || both_resets_known, reset, false, false);
}

inline void SnapshotEvaluate(const NVSDK_NGX_Parameter* parameters,
                             diagnostic::EvaluateEvent& event) {
  SnapshotEvaluateScalars(parameters, event);
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
  for (size_t i = 0; i < std::size(kResources); ++i) {
    SnapshotResource(parameters, kResources[i].name, kResources[i].key, event.resources[i]);
  }
}

inline void SnapshotVulkanEvaluate(const NVSDK_NGX_Parameter* parameters,
                                   diagnostic::EvaluateEvent& event,
                                   VulkanImages* images = nullptr) {
  if (images) *images = {};
  SnapshotEvaluateScalars(parameters, event);
  static constexpr const char* names[] = {
      "DLSSG.Backbuffer", "DLSSG.Depth", "DLSSG.MVecs", "DLSSG.HUDLess", "DLSSG.UI",
      "DLSSG.UIAlpha", "DLSSG.BidirectionalDistortionField", "DLSSG.OutputInterpolated",
      "DLSSG.OutputReal", "DLSSG.OutputDisableInterpolation"};
  static_assert(std::size(names) == static_cast<size_t>(diagnostic::ResourceKey::kCount));
  for (size_t i = 0; i < std::size(names); ++i) {
    auto& dst = event.resources[i];
    dst.key = static_cast<uint32_t>(i);
    if (!parameters) continue;
    void* resource = nullptr;
    if (parameters->Get(names[i], &resource) != NVSDK_NGX_Result_Success) continue;
    if (!resource) { dst.known = 1; continue; }
    vulkancontract::Image image{};
    if (!vulkannative::DecodeImage(resource, image)) continue;
    if (images) (*images)[i] = image;
    dst.known = 1; dst.object = image.image; dst.width = image.width; dst.height = image.height;
    dst.format = image.format;
    // Existing observer ABI: backend=kNgxVulkan identifies the VkFormat domain.
    // Do not invent DXGI dimension/sample count, a layout or queue ownership.
    dst.depth_or_array = image.layer_count <= UINT16_MAX ? static_cast<uint16_t>(image.layer_count) : 0;
    dst.mip_levels = image.mip_count <= UINT16_MAX ? static_cast<uint16_t>(image.mip_count) : 0;
  }
}

inline void ObserveEvaluateResources(const diagnostic::EvaluateEvent& event) {
  for (size_t i = 0; i < event.resources.size(); ++i) {
    const auto& resource = event.resources[i];
    if (event.backend == diagnostic::Backend::kNgxVulkan && !resource.known) continue;
    integration::ResourceRole role{};
    switch (static_cast<diagnostic::ResourceKey>(resource.key)) {
      case diagnostic::ResourceKey::kBackbuffer: role = integration::ResourceRole::kBackbuffer; break;
      case diagnostic::ResourceKey::kDepth: role = integration::ResourceRole::kDepth; break;
      case diagnostic::ResourceKey::kMotionVectors: role = integration::ResourceRole::kMotionVectors; break;
      case diagnostic::ResourceKey::kHudLess: role = integration::ResourceRole::kHudLess; break;
      case diagnostic::ResourceKey::kUi: role = integration::ResourceRole::kUiColorAndAlpha; break;
      case diagnostic::ResourceKey::kUiAlpha: role = integration::ResourceRole::kUiAlpha; break;
      case diagnostic::ResourceKey::kBidirectionalDistortionField:
        role = integration::ResourceRole::kBidirectionalDistortionField; break;
      case diagnostic::ResourceKey::kOutputInterpolated:
        role = integration::ResourceRole::kOutputInterpolated; break;
      case diagnostic::ResourceKey::kOutputReal: role = integration::ResourceRole::kOutputReal; break;
      case diagnostic::ResourceKey::kOutputDisableInterpolation:
        role = integration::ResourceRole::kOutputDisableInterpolation; break;
      default: continue;
    }
    integration::ObserveNgxResource(
        role, resource.known != 0, resource.object,
        resource.width, resource.height, resource.format,
        event.backbuffer_frame_id_known != 0, event.backbuffer_frame_id);
  }
}

// The driver call stays outside g_lifecycle_lock. Only bounded, metadata-only
// publication is serialized with creation/release so a reused handle cannot
// resurrect observations from an earlier feature or module generation.
inline void FgCall::PublishBegin(const diagnostic::EvaluateEvent& event) {
  std::lock_guard lock(g_lifecycle_lock);
  if (!CurrentLocked() || finished) return;
  // Clear prior NGX metadata first for Vulkan; a missing descriptor must not
  // inherit a prior frame/COM snapshot. Native Streamline tags remain independent.
  if (backend == diagnostic::Backend::kNgxVulkan) integration::InvalidateNgxResources();
  ObserveEvaluateResources(event);
}
inline void FgCall::PublishEnd(const diagnostic::EvaluateEvent& event, NVSDK_NGX_Result result,
                               const VulkanImages* images) {
  std::lock_guard lock(g_lifecycle_lock);
  if (!finished || !completion_current || !CurrentLocked()) return;
  if (backend == diagnostic::Backend::kNgxVulkan) {
    g_vulkan_observation.last_evaluate = event;
    g_vulkan_observation.images = images ? *images : VulkanImages{};
    g_vulkan_observation.snapshot_current = result == NVSDK_NGX_Result_Success;
  }

  if (result == NVSDK_NGX_Result_Success) ObserveEvaluateHistory(event);
  else integration::InvalidateFrameEvidence();
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

inline void TraceVulkanRequirement(const char* phase,
    const NVSDK_NGX_FeatureDiscoveryInfo* discovery,
    const NVSDK_NGX_FeatureRequirement* output, NVSDK_NGX_Result result) noexcept {
  static std::atomic_uint calls{0};
  if (calls.fetch_add(1, std::memory_order_relaxed) >= 128) return;
  DiscoveryTrace([&](auto& message) {
    message << "NGX_VULKAN_REQUIREMENTS phase=" << phase << " feature="
            << (discovery ? static_cast<uint32_t>(discovery->FeatureID) : UINT32_MAX)
            << " result=0x" << std::hex << static_cast<uint32_t>(result);
    if (result == NVSDK_NGX_Result_Success && output)
      message << " supported=0x" << static_cast<uint32_t>(output->FeatureSupported)
              << " min_arch=0x" << output->MinHWArchitecture;
  });
}

template <size_t I>
NVSDK_NGX_Result NVSDK_CONV VulkanRequirements(
    VkInstance instance, VkPhysicalDevice physical_device,
    const NVSDK_NGX_FeatureDiscoveryInfo* discovery,
    NVSDK_NGX_FeatureRequirement* output) {
  auto& slot = g_slots[I];
  const auto real = slot.entry_vulkan_requirements;
  if (!real) return NVSDK_NGX_Result_FAIL_InvalidParameter;
  if (g_shutting_down.load(std::memory_order_acquire)) {
    ScopedArchQuery no_spoof(nullptr);
    const auto result = real(instance, physical_device, discovery, output);
    TraceVulkanRequirement("NATIVE", discovery, output, result);
    return result;
  }
  const bool fg = discovery != nullptr && static_cast<uint32_t>(discovery->FeatureID) ==
      static_cast<uint32_t>(NVSDK_NGX_Feature_FrameGeneration);
  if (!g_enabled.load(std::memory_order_relaxed) || !architecture::NeedsBridge() ||
      !fg || g_in_requirements) {
    ScopedArchQuery no_spoof(nullptr);
    const auto result = real(instance, physical_device, discovery, output);
    TraceVulkanRequirement("NATIVE", discovery, output, result);
    return result;
  }

  g_in_requirements = true;
  struct Guard {
    ~Guard() { g_in_requirements = false; }
  } guard;

  NvPhysicalGpuHandle physical_gpu = nullptr;
  try {
    physical_gpu = MatchVulkanAdapter(instance, physical_device);
    EnsureArchitectureHook();
    if (physical_gpu != nullptr && g_prepare_loaded_providers != nullptr)
      g_prepare_loaded_providers();
  } catch (...) {
    Log("Vulkan NGX requirements preflight failed", true);
  }

  ScopedArchQuery architecture_scope(physical_gpu);
  const NVSDK_NGX_Result result = real(instance, physical_device, discovery, output);
  TraceVulkanRequirement("NATIVE", discovery, output, result);

  try {
    if (physical_gpu != nullptr && g_prepare_loaded_providers != nullptr)
      g_prepare_loaded_providers();
  } catch (...) {
    Log("Vulkan NGX requirements postflight failed", true);
  }

  const auto returned = FinalizeRequirements(result, output, physical_gpu != nullptr);
  TraceVulkanRequirement("RETURNED", discovery, output, returned);
  return returned;
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
    const auto result = real(adapter, discovery, output);
    const auto call = g_discovery_requirement_calls.fetch_add(1) + 1;
    if (call <= 32) DiscoveryTrace([&](auto& message) {
      message << "NGX_REQUIREMENTS feature=" << (discovery ? static_cast<uint32_t>(discovery->FeatureID) : UINT32_MAX)
              << " result=0x" << std::hex << static_cast<uint32_t>(result);
      if (result == NVSDK_NGX_Result_Success && output)
        message << " supported=0x" << static_cast<uint32_t>(output->FeatureSupported)
                << " min_arch=0x" << output->MinHWArchitecture;
      message << " path=NATIVE";
    });
    return result;
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
  if (!fg) {
    ScopedArchQuery no_spoof(nullptr);
    const auto result = real(commands, feature, parameters, handle);
    const auto call = g_discovery_other_creates.fetch_add(1) + 1;
    if (call <= 16) DiscoveryTrace([&](auto& message) {
      message << "NGX_CREATE feature=" << static_cast<uint32_t>(feature)
              << " result=0x" << std::hex << static_cast<uint32_t>(result);
      if (result == NVSDK_NGX_Result_Success && handle) message << " handle=" << *handle;
      message << " path=NATIVE";
    });
    return result;
  }
  const uint32_t diag_call =
      g_turing_diag_create_calls.fetch_add(1, std::memory_order_relaxed) + 1;
  const bool diag_trace = diagnostic::g_verbose.load(std::memory_order_relaxed) &&
                          diag_call <= kTuringDiagNgxTraceLimit;
  if (diag_trace) {
    std::stringstream message;
    message << "TuringDiagV2 NGX CreateFeature#" << diag_call
            << " BEGIN thread=" << GetCurrentThreadId()
            << " commands=" << commands << " parameters=" << parameters;
    TuringDiagLog(message.str());
  }
  const auto before_create = g_before_fg_create.load(std::memory_order_acquire);
  if (before_create && !before_create()) {
    if (handle) *handle = nullptr;
    if (diag_trace) {
      std::stringstream message;
      message << "TuringDiagV2 NGX CreateFeature#" << diag_call
              << " END result=0x" << std::hex
              << static_cast<uint32_t>(NVSDK_NGX_Result_FAIL_FeatureNotSupported)
              << " source=addon-preflight";
      TuringDiagLog(message.str(), true);
    }
    return NVSDK_NGX_Result_FAIL_FeatureNotSupported;
  }
  g_create_seen.store(true, std::memory_order_release);
  FgCall call(slot, FgCallKind::kCreate);
  const NVSDK_NGX_Result result = real(commands, feature, parameters, handle);
  if (diag_trace) {
    std::stringstream message;
    message << "TuringDiagV2 NGX CreateFeature#" << diag_call
            << " END result=0x" << std::hex << static_cast<uint32_t>(result);
    if (result == NVSDK_NGX_Result_Success && handle)
      message << " handle=" << *handle;
    TuringDiagLog(message.str(), result != NVSDK_NGX_Result_Success);
  }
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
  const uint32_t diag_call = fg
      ? g_turing_diag_evaluate_calls.fetch_add(1, std::memory_order_relaxed) + 1 : 0;
  const bool diag_trace = fg && diagnostic::g_verbose.load(std::memory_order_relaxed) &&
                          diag_call <= kTuringDiagNgxTraceLimit;
  if (diag_trace) {
    std::stringstream message;
    message << "TuringDiagV2 NGX EvaluateFeature#" << diag_call
            << " BEGIN thread=" << GetCurrentThreadId()
            << " commands=" << commands << " handle=" << handle
            << " parameters=" << parameters;
    TuringDiagLog(message.str());
  }
  diagnostic::EvaluateEvent observed;
  if (fg) {
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
    call.PublishBegin(observed);
    diagnostic::Emit(observed);
  }
  const NVSDK_NGX_Result result = real(commands, handle, parameters, callback);
  if (diag_trace) {
    std::stringstream message;
    message << "TuringDiagV2 NGX EvaluateFeature#" << diag_call
            << " END result=0x" << std::hex << static_cast<uint32_t>(result);
    TuringDiagLog(message.str(), result != NVSDK_NGX_Result_Success);
  }
  call.Finish(result);
  if (fg) {
    struct LastError {
      DWORD value = GetLastError();
      ~LastError() { SetLastError(value); }
    } last_error;
    observed.phase = diagnostic::EvaluatePhase::kEnd;
    observed.result = static_cast<uint32_t>(result);
    call.PublishEnd(observed, result);
    diagnostic::Emit(observed);
  }
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
  const bool fg = call.tracked;
  const uint32_t diag_call = fg
      ? g_turing_diag_release_calls.fetch_add(1, std::memory_order_relaxed) + 1 : 0;
  const bool diag_trace = fg && diagnostic::g_verbose.load(std::memory_order_relaxed) &&
                          diag_call <= kTuringDiagNgxTraceLimit;
  if (diag_trace) {
    std::stringstream message;
    message << "TuringDiagV2 NGX ReleaseFeature#" << diag_call
            << " BEGIN thread=" << GetCurrentThreadId() << " handle=" << handle;
    TuringDiagLog(message.str());
  }
  const NVSDK_NGX_Result result = real(handle);
  if (diag_trace) {
    std::stringstream message;
    message << "TuringDiagV2 NGX ReleaseFeature#" << diag_call
            << " END result=0x" << std::hex << static_cast<uint32_t>(result);
    TuringDiagLog(message.str(), result != NVSDK_NGX_Result_Success);
  }
  call.Finish(result);
  return result;
}

// Public NGX Vulkan ABI, no dependency on a Vulkan SDK or D3D12 resource layout.
// CreateFeature1 may dispatch through CreateFeature: track the outer call once.
inline thread_local bool g_in_vulkan_call = false;
struct VulkanCallScope {
  bool previous = g_in_vulkan_call;
  VulkanCallScope() { g_in_vulkan_call = true; }
  ~VulkanCallScope() { g_in_vulkan_call = previous; }
};

template <size_t I, typename Forward>
NVSDK_NGX_Result CreateVulkanFeature(NVSDK_NGX_Feature feature,
                                     NVSDK_NGX_Handle** handle, Forward&& forward) {
  if (feature != NVSDK_NGX_Feature_FrameGeneration) {
    ScopedArchQuery no_spoof(nullptr);
    const auto result = forward();
    static std::atomic_uint calls{0};
    if (calls.fetch_add(1, std::memory_order_relaxed) < 32) DiscoveryTrace([&](auto& message) {
      message << "NGX_VULKAN_CREATE feature=" << static_cast<uint32_t>(feature)
              << " result=0x" << std::hex << static_cast<uint32_t>(result)
              << " path=NATIVE";
    });
    return result;
  }
  if (g_shutting_down.load(std::memory_order_acquire) || g_in_vulkan_call) return forward();
  VulkanCallScope scope;
  const auto before_create = g_before_fg_create.load(std::memory_order_acquire);
  if (before_create && !before_create()) {
    if (handle) *handle = nullptr;
    return NVSDK_NGX_Result_FAIL_FeatureNotSupported;
  }
  g_create_seen.store(true, std::memory_order_release);
  FgCall call(g_slots[I], FgCallKind::kCreate, nullptr, diagnostic::Backend::kNgxVulkan);
  const auto result = forward();
  call.Finish(result, result == NVSDK_NGX_Result_Success && handle ? *handle : nullptr);
  return result;
}

template <size_t I>
NVSDK_NGX_Result NVSDK_CONV VulkanCreate(VkCommandBuffer_T* commands,
    NVSDK_NGX_Feature feature, NVSDK_NGX_Parameter* parameters, NVSDK_NGX_Handle** handle) {
  const auto real = g_slots[I].entry_vulkan_create;
  if (!real) return NVSDK_NGX_Result_FAIL_InvalidParameter;
  return CreateVulkanFeature<I>(feature, handle,
      [&] { return real(commands, feature, parameters, handle); });
}

template <size_t I>
NVSDK_NGX_Result NVSDK_CONV VulkanCreate1(VkDevice_T* device, VkCommandBuffer_T* commands,
    NVSDK_NGX_Feature feature, NVSDK_NGX_Parameter* parameters, NVSDK_NGX_Handle** handle) {
  const auto real = g_slots[I].entry_vulkan_create1;
  if (!real) return NVSDK_NGX_Result_FAIL_InvalidParameter;
  return CreateVulkanFeature<I>(feature, handle,
      [&] { return real(device, commands, feature, parameters, handle); });
}

template <size_t I>
NVSDK_NGX_Result NVSDK_CONV VulkanEvaluate(VkCommandBuffer_T* commands,
    const NVSDK_NGX_Handle* handle, const NVSDK_NGX_Parameter* parameters,
    PFN_NVSDK_NGX_ProgressCallback callback) {
  const auto real = g_slots[I].entry_vulkan_evaluate;
  if (!real) return NVSDK_NGX_Result_FAIL_InvalidParameter;
  if (g_shutting_down.load(std::memory_order_acquire) || g_in_vulkan_call)
    return real(commands, handle, parameters, callback);
  VulkanCallScope scope;
  FgCall call(g_slots[I], FgCallKind::kEvaluate, handle, diagnostic::Backend::kNgxVulkan);
  diagnostic::EvaluateEvent event;
  VulkanImages images{};
  if (call.tracked) {
    event.backend = diagnostic::Backend::kNgxVulkan;
    event.thread = GetCurrentThreadId();
    event.evaluation_id = diagnostic::g_evaluation_sequence.fetch_add(1, std::memory_order_relaxed) + 1;
    event.commands = reinterpret_cast<uint64_t>(commands);
    event.handle = reinterpret_cast<uint64_t>(handle);
    event.parameters = reinterpret_cast<uint64_t>(parameters);
    SnapshotVulkanEvaluate(parameters, event, &images);
    // Borrow descriptor metadata only. The host owns VkImage/view, command
    // buffers, submission waits/signals, transitions and GPU resource lifetime.
    call.PublishBegin(event);
    diagnostic::Emit(event);
  }
  const auto result = real(commands, handle, parameters, callback);
  call.Finish(result);
  if (call.tracked) {
    event.phase = diagnostic::EvaluatePhase::kEnd;
    event.result = static_cast<uint32_t>(result);
    call.PublishEnd(event, result, &images);
    diagnostic::Emit(event);
  }
  return result;
}

template <size_t I>
NVSDK_NGX_Result NVSDK_CONV VulkanRelease(NVSDK_NGX_Handle* handle) {
  const auto real = g_slots[I].entry_vulkan_release;
  if (!real) return NVSDK_NGX_Result_FAIL_InvalidParameter;
  if (g_shutting_down.load(std::memory_order_acquire) || g_in_vulkan_call) return real(handle);
  VulkanCallScope scope;
  FgCall call(g_slots[I], FgCallKind::kRelease, handle, diagnostic::Backend::kNgxVulkan);
  const auto result = real(handle);
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

// Diagnostic ownership only; no cached pointer is dereferenced. Keep taint
// markers until the owning NGX module retires, even across feature recreation.
struct CapabilityOriginal {
  const void* owner = nullptr;
  NVSDK_NGX_Parameter* block = nullptr;
  runtimecontrols::OriginalMaximum maximum;
};
inline std::mutex g_capability_evidence_lock;
inline uint64_t g_capability_evidence_epoch = 1;
inline std::vector<CapabilityOriginal> g_capability_originals;
inline const void* g_last_capability_owner = nullptr;
inline constexpr size_t kCapabilityEvidenceBudget = 1024;

inline void ForgetCapabilityOwner(const void* owner) {
  std::lock_guard lock(g_capability_evidence_lock);
  ++g_capability_evidence_epoch;
  std::erase_if(g_capability_originals, [owner](const auto& record) { return record.owner == owner; });
  if (g_last_capability_owner == owner)
    g_status.native_maximum.store(runtimecontrols::kUnknownMaximum, std::memory_order_release);
}
inline void InvalidateCapabilityEvidence() {
  std::lock_guard lock(g_capability_evidence_lock);
  ++g_capability_evidence_epoch;
  for (auto& record : g_capability_originals) record.maximum.Invalidate();
  g_status.native_maximum.store(runtimecontrols::kUnknownMaximum, std::memory_order_release);
}

inline void ApplyCapabilities(NVSDK_NGX_Parameter* parameters, unsigned int limit,
                              unsigned int structural_ceiling = 0,
                              countobserver::Backend backend = countobserver::Backend::kNgxD3D12,
                              uint64_t caller = 0, const void* owner = nullptr) {
  if (!parameters) return;
  uint64_t evidence_epoch = 0;
  {
    std::lock_guard lock(g_capability_evidence_lock);
    evidence_epoch = g_capability_evidence_epoch;
    g_status.native_maximum.store(runtimecontrols::kUnknownMaximum, std::memory_order_release);
  }
  // Only call with the evidence mutex held. Never hold it across vendor code,
  // an observer callback, or logging: any of those can re-enter lifecycle work.
  const auto record = [=]() -> runtimecontrols::OriginalMaximum* {
    auto found = std::find_if(g_capability_originals.begin(), g_capability_originals.end(),
        [=](const auto& value) { return value.owner == owner && value.block == parameters; });
    if (found != g_capability_originals.end()) return &found->maximum;
    if (g_capability_originals.size() >= kCapabilityEvidenceBudget) return nullptr;
    g_capability_originals.push_back({owner, parameters, {}});
    return &g_capability_originals.back().maximum;
  };
  // No pointer/iterator into this vector survives a native call or callback.
  // An exhausted diagnostics budget means UNKNOWN, never a denied trial.
  int available = 0;
  int maximum = 0;
  const auto available_result = parameters->Get("FrameGeneration.Available", &available);
  const auto maximum_result = parameters->Get("DLSSG.MultiFrameCountMax", &maximum);
  ObserveCapabilityCount(parameters, maximum, maximum_result, countobserver::Operation::kRead, backend, caller);
  {
    std::lock_guard lock(g_capability_evidence_lock);
    if (evidence_epoch == g_capability_evidence_epoch) {
      auto* original = record();
      const uint64_t native_maximum = original
          ? original->Observe(
              maximum_result == NVSDK_NGX_Result_Success && maximum >= 0, static_cast<uint32_t>(maximum))
          : runtimecontrols::kUnknownMaximum;
      g_last_capability_owner = owner;
      g_status.native_maximum.store(native_maximum, std::memory_order_release);
    }
  }
  const bool count_ready = g_enabled.load() && limit != 0;
  const bool admission_ready = g_enabled.load();

  if (admission_ready && available_result == NVSDK_NGX_Result_Success && available == 0) {
    // Available exposes the user's selected trial. NeedsUpdatedDriver and
    // FeatureInitResult remain native diagnostic values; neither substitutes
    // for the actual Create/Evaluate operation, whose result is never rewritten.
    parameters->Set("FrameGeneration.Available", 1);
    const auto stored = parameters->Get("FrameGeneration.Available", &available);
    if (stored == NVSDK_NGX_Result_Success && available == 1 && !g_status.available.load())
      Log("FrameGeneration.Available: 0 -> 1 for native trial; native failure fields preserved");
  }
  if (count_ready && available_result == NVSDK_NGX_Result_Success && available > 0 &&
      maximum_result == NVSDK_NGX_Result_Success && maximum >= 0) {
    const auto wanted = CapabilityFrameCount(maximum, limit, structural_ceiling);
    if (wanted != maximum) {
      {
        std::lock_guard lock(g_capability_evidence_lock);
        if (auto* value = record()) value->Wrote(static_cast<uint32_t>(wanted));
      }
      parameters->Set("DLSSG.MultiFrameCountMax", wanted);
      ObserveCapabilityCount(parameters, wanted, NVSDK_NGX_Result_Success,
                             countobserver::Operation::kWrite, backend, caller);
      int stored = maximum;
      const auto readback = parameters->Get("DLSSG.MultiFrameCountMax", &stored);
      ObserveCapabilityCount(parameters, stored, readback, countobserver::Operation::kRead, backend, caller);
      if (readback == NVSDK_NGX_Result_Success && stored >= 0) {
        std::lock_guard lock(g_capability_evidence_lock);
        if (auto* value = record()) value->Wrote(static_cast<uint32_t>(stored));
      }
      if (readback == NVSDK_NGX_Result_Success && stored >= 0 && stored != maximum) {
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
      !NeedsCapabilityBridge() || g_in_capabilities) return real(output);

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
  DiscoveryLastError native_error;
  const auto trace_call = g_discovery_capability_calls.fetch_add(1) + 1;
  if (trace_call <= 8) DiscoveryTrace([&](auto& message) {
    message << "NGX_CAPS_RESULT api=" << kParameterExports[Api] << " call=" << trace_call
            << " result=0x" << std::hex << static_cast<uint32_t>(result);
  });
  g_status.capabilities_seen.store(true, std::memory_order_relaxed);
  g_status.vulkan_capabilities.store(Api >= 2, std::memory_order_relaxed);
  if (!slot.parameters_seen[Api].exchange(true)) {
    Log(std::string(kParameterExports[Api]) + ": " + std::to_string(static_cast<uint32_t>(result)));
  }
  if (result != NVSDK_NGX_Result_Success || !output || !*output) {
    g_status.native_maximum.store(runtimecontrols::kUnknownMaximum, std::memory_order_release);
    return result;
  }
  TraceCapabilityBlock(kParameterExports[Api], "NATIVE", *output, trace_call);
  try {
    // The native call can load the provider while populating the block.
    if (g_prepare_loaded_providers) g_prepare_loaded_providers();
    ApplyCapabilities(*output, g_capability_limit ? g_capability_limit() : 0,
                      g_streamline_ceiling ? g_streamline_ceiling() : 0,
                      Api >= 2 ? countobserver::Backend::kNgxVulkan : countobserver::Backend::kNgxD3D12,
                      countobserver::CallerAddress(), &slot);
  } catch (...) {
    Log("NGX capability update failed", true);
  }
  TraceCapabilityBlock(kParameterExports[Api], "RETURNED", *output, trace_call);
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
  static_assert(std::is_same_v<decltype(&VulkanCreate<I>), VulkanCreateFn>);
  static_assert(std::is_same_v<decltype(&VulkanCreate1<I>), VulkanCreate1Fn>);
  static_assert(std::is_same_v<decltype(&VulkanEvaluate<I>), VulkanEvaluateFn>);
  static_assert(std::is_same_v<decltype(&VulkanRelease<I>), ReleaseFn>);
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
      {"NVSDK_NGX_VULKAN_CreateFeature", reinterpret_cast<void**>(&slot.entry_vulkan_create),
       reinterpret_cast<void*>(&VulkanCreate<I>)},
      {"NVSDK_NGX_VULKAN_CreateFeature1", reinterpret_cast<void**>(&slot.entry_vulkan_create1),
       reinterpret_cast<void*>(&VulkanCreate1<I>)},
      {"NVSDK_NGX_VULKAN_EvaluateFeature", reinterpret_cast<void**>(&slot.entry_vulkan_evaluate),
       reinterpret_cast<void*>(&VulkanEvaluate<I>)},
      {"NVSDK_NGX_VULKAN_ReleaseFeature", reinterpret_cast<void**>(&slot.entry_vulkan_release),
       reinterpret_cast<void*>(&VulkanRelease<I>)},
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
  ForgetCapabilityOwner(&slot);
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
  ResetVulkanObservationLocked();
  ++slot.generation;
  slot.fg_handles.fill(nullptr);
  slot.fg_generation.fill(0);
  slot.fg_active.fill(false);
  integration::InvalidateNgxFeatureEvidence();
  UpdateFeatureStatus();
}

inline bool ShouldHookEntry(const char* name, bool provider_bridge) {
  (void)name;
  (void)provider_bridge;
  // Each entry is checked for a callable ABI below. GPU profile selection does
  // not decide whether a public requirements call can be observed or retried.
  return true;
}

inline bool IsTargetName(const char* name) {
  if (!name || reinterpret_cast<uintptr_t>(name) <= 0xffff) return false;
  for (const auto* parameter : kParameterExports)
    if (std::strcmp(name, parameter) == 0) return true;
  return std::strcmp(name, "NVSDK_NGX_D3D12_GetFeatureRequirements") == 0 ||
         std::strcmp(name, "NVSDK_NGX_VULKAN_GetFeatureRequirements") == 0 ||
         std::strcmp(name, "NVSDK_NGX_D3D12_CreateFeature") == 0 ||
         std::strcmp(name, "NVSDK_NGX_D3D12_EvaluateFeature") == 0 ||
         std::strcmp(name, "NVSDK_NGX_D3D12_ReleaseFeature") == 0 ||
         std::strcmp(name, "NVSDK_NGX_VULKAN_CreateFeature") == 0 ||
         std::strcmp(name, "NVSDK_NGX_VULKAN_CreateFeature1") == 0 ||
         std::strcmp(name, "NVSDK_NGX_VULKAN_EvaluateFeature") == 0 ||
         std::strcmp(name, "NVSDK_NGX_VULKAN_ReleaseFeature") == 0;
}
}  // namespace internal

inline bool ResetFeatureTrackingForDeviceRecreation() {
  internal::InvalidateCapabilityEvidence();
  std::lock_guard lock(internal::g_lifecycle_lock);
  if (internal::g_lifecycle.creates != 0 || internal::g_lifecycle.evaluates != 0 ||
      internal::g_lifecycle.releases != 0) {
    internal::g_lifecycle.uncertain = true;
    internal::g_lifecycle.zero_boundary = false;
    return false;
  }
  internal::ResetVulkanObservationLocked();
  for (auto& slot : internal::g_slots) {
    ++slot.generation;
    slot.fg_handles.fill(nullptr);
    slot.fg_generation.fill(0);
    slot.fg_active.fill(false);
  }
  integration::InvalidateNgxFeatureEvidence();
  ++internal::g_lifecycle.epoch;
  ++internal::g_lifecycle.sequence;
  internal::g_lifecycle.handles = 0;
  internal::g_lifecycle.zero_boundary = true;
  internal::g_lifecycle.uncertain = false;
  g_status.feature_created.store(false, std::memory_order_release);
  g_status.feature_active.store(false, std::memory_order_release);
  return true;
}

// Called at module discovery or export lookup, before handing a native address
// to the host. No driver initialization or provider scan belongs in this path.
inline void TryInstall(HMODULE module) {
  if (g_shutting_down.load(std::memory_order_acquire) || !g_enabled.load() ||
      !internal::NeedsCapabilityBridge() || internal::g_installing ||
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
  const bool provider_bridge = architecture::NeedsBridge();
  std::vector<hook::HookItem> pending;
  for (size_t i = 0; i < hooks.size(); ++i) {
    const auto& [name, storage, replacement] = hooks[i];
    // Native Ada needs lifecycle/resource observation too; only admission
    // requirements rewriting is specific to provider retarget architectures.
    if (!internal::ShouldHookEntry(name, provider_bridge)) continue;
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

inline void RefreshEntryLifetimes() {
  if (!TryAcquireSRWLockExclusive(&internal::g_resolver_lock)) return;
  for (size_t i = 0; i < internal::g_slots.size(); ++i) {
    const auto& slot = internal::g_slots[i];
    if (slot.module && !hook::IsCurrent(slot.identity)) internal::ClearRuntime(i);
  }
  ReleaseSRWLockExclusive(&internal::g_resolver_lock);
}

inline void EnsureEntryHooks() {
  RefreshEntryLifetimes();
  for (const auto* name : {L"_nvngx.dll", L"nvngx.dll"}) TryInstall(GetModuleHandleW(name));
}

inline void BeforeInit() {
  if (g_shutting_down.load(std::memory_order_acquire)) return;
  try {
    internal::ProbeAdapterBeforeInit();
    EnsureEntryHooks();
    if (g_prepare_loaded_providers) g_prepare_loaded_providers();
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
