// SPDX-License-Identifier: MIT
// Test doubles only. These declarations are not Windows/Streamline/NGX ABI
// compatibility evidence.
#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <cwchar>
#include <map>
#include <string>
#include <tuple>
#include <vector>

#define __cdecl
#define WINAPI
#define NVSDK_CONV
#define __uuidof(T) 0
#define ARRAYSIZE(a) (sizeof(a) / sizeof((a)[0]))
#define MAKEINTRESOURCEW(n) reinterpret_cast<const wchar_t*>(static_cast<uintptr_t>(n))
#define HIWORD(n) static_cast<uint16_t>((n) >> 16)
#define LOWORD(n) static_cast<uint16_t>((n) & 0xffff)
#define SRWLOCK_INIT {}
#define GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS 4u
#define GET_MODULE_HANDLE_EX_FLAG_PIN 1u
#define LOAD_LIBRARY_SEARCH_SYSTEM32 0x800u
#define VS_VERSION_INFO 1

using DWORD = uint32_t;
using UINT = uint32_t;
using LONG = int32_t;
using HRESULT = int32_t;
using HMODULE = void*;
using HANDLE = void*;
using HRSRC = void*;
using HGLOBAL = void*;
using LPCWSTR = const wchar_t*;
using LPCSTR = const char*;
using FARPROC = void (*)();
using REFIID = int;

constexpr HRESULT S_OK = 0;
#define FAILED(x) ((x) < 0)

struct SRWLOCK {};

inline void AcquireSRWLockExclusive(SRWLOCK*) {}
inline void ReleaseSRWLockExclusive(SRWLOCK*) {}
inline void AcquireSRWLockShared(SRWLOCK*) {}
inline void ReleaseSRWLockShared(SRWLOCK*) {}
inline bool TryAcquireSRWLockShared(SRWLOCK*) {
  return true;
}

struct LUID {
  DWORD LowPart = 0;
  LONG HighPart = 0;
};

struct MEMORY_BASIC_INFORMATION {
  void* AllocationBase = nullptr;
};

struct VS_FIXEDFILEINFO {
  DWORD dwSignature = 0;
  DWORD dwStrucVersion = 0;
  DWORD dwFileVersionMS = 0;
  DWORD dwFileVersionLS = 0;
  DWORD dwProductVersionMS = 0;
  DWORD dwProductVersionLS = 0;
  DWORD dwFileFlagsMask = 0;
  DWORD dwFileFlags = 0;
  DWORD dwFileOS = 0;
  DWORD dwFileType = 0;
  DWORD dwFileSubtype = 0;
  DWORD dwFileDateMS = 0;
  DWORD dwFileDateLS = 0;
};

namespace mock {
inline std::map<std::wstring, HMODULE> modules;
inline std::map<HMODULE, std::wstring> paths;
inline std::map<std::pair<HMODULE, std::string>, FARPROC> exports;
inline std::map<HMODULE, std::vector<unsigned char>> resources;
inline std::map<const void*, HMODULE> allocation;
inline bool pin_ok = true;
inline bool install_ok = true;
inline unsigned int installs = 0;
}  // namespace mock

inline HMODULE GetModuleHandleW(LPCWSTR name) {
  auto iterator = mock::modules.find(name);
  return iterator == mock::modules.end() ? nullptr : iterator->second;
}

inline HMODULE LoadLibraryExW(LPCWSTR name, HANDLE, DWORD) {
  return GetModuleHandleW(name);
}

inline bool FreeLibrary(HMODULE) {
  return true;
}

inline FARPROC GetProcAddress(HMODULE module, LPCSTR name) {
  auto iterator = mock::exports.find({module, name});
  return iterator == mock::exports.end() ? nullptr : iterator->second;
}

inline bool GetModuleHandleExW(DWORD, LPCWSTR address, HMODULE* module) {
  if (!mock::pin_ok) return false;
  *module = reinterpret_cast<HMODULE>(const_cast<wchar_t*>(address));
  return true;
}

inline DWORD GetModuleFileNameW(HMODULE module, wchar_t* output, DWORD size) {
  auto iterator = mock::paths.find(module);
  if (iterator == mock::paths.end() || iterator->second.size() + 1 > size) return 0;
  std::wcscpy(output, iterator->second.c_str());
  return static_cast<DWORD>(iterator->second.size());
}

inline DWORD GetModuleFileNameA(HMODULE module, char* output, DWORD size) {
  auto iterator = mock::paths.find(module);
  if (iterator == mock::paths.end() || iterator->second.size() + 1 > size) return 0;
  std::string path(iterator->second.begin(), iterator->second.end());
  std::strcpy(output, path.c_str());
  return static_cast<DWORD>(path.size());
}

inline size_t VirtualQuery(const void* address, MEMORY_BASIC_INFORMATION* memory, size_t size) {
  auto iterator = mock::allocation.find(address);
  if (iterator == mock::allocation.end()) return 0;
  memory->AllocationBase = iterator->second;
  return size;
}

inline HRSRC FindResourceW(HMODULE module, LPCWSTR, LPCWSTR) {
  return mock::resources.count(module) ? module : nullptr;
}

inline DWORD SizeofResource(HMODULE module, HRSRC) {
  return static_cast<DWORD>(mock::resources[module].size());
}

inline HGLOBAL LoadResource(HMODULE module, HRSRC) {
  return module;
}

inline void* LockResource(HGLOBAL handle) {
  return mock::resources[handle].data();
}

struct ID3D12GraphicsCommandList {};
using VkInstance = void*;
using VkPhysicalDevice = void*;

struct DXGI_ADAPTER_DESC {
  UINT VendorId = 0;
  LUID AdapterLuid;
};

struct IDXGIAdapter {
  DXGI_ADAPTER_DESC desc;
  HRESULT status = 0;

  HRESULT GetDesc(DXGI_ADAPTER_DESC* output) {
    *output = desc;
    return status;
  }
};

struct IDXGIAdapter1 : IDXGIAdapter {
  void Release() {}
};

struct IDXGIFactory1 {
  std::vector<IDXGIAdapter1*> adapters;

  HRESULT EnumAdapters1(UINT index, IDXGIAdapter1** output) {
    if (index >= adapters.size()) return -1;
    *output = adapters[index];
    return 0;
  }

  void Release() {}
};

using NvU32 = uint32_t;
using NvAPI_Status = int;
using NvPhysicalGpuHandle = void*;
constexpr NvAPI_Status NVAPI_OK = 0;
constexpr NvAPI_Status NVAPI_ERROR = -1;
constexpr unsigned int NVAPI_MAX_PHYSICAL_GPUS = 64;

struct NV_GPU_ARCH_INFO {
  NvU32 version = 0;
  NvU32 architecture = 0;
  NvU32 implementation = 0;
  NvU32 revision = 0;
};
constexpr NvU32 NV_GPU_ARCH_INFO_VER = sizeof(NV_GPU_ARCH_INFO) | (2u << 16);

NvAPI_Status NvAPI_Initialize();
NvAPI_Status NvAPI_EnumPhysicalGPUs(NvPhysicalGpuHandle*, NvU32*);
NvAPI_Status NvAPI_GPU_GetAdapterIdFromPhysicalGpu(NvPhysicalGpuHandle, LUID*);
NvAPI_Status NvAPI_GPU_GetArchInfo(NvPhysicalGpuHandle, NV_GPU_ARCH_INFO*);

struct D3DKMT_OPENADAPTERFROMLUID {
  LUID AdapterLuid;
  UINT hAdapter = 0;
};
struct D3DKMT_CLOSEADAPTER {
  UINT hAdapter = 0;
};
struct D3DKMT_WDDM_2_7_CAPS {
  UINT Value = 0;
};
struct D3DKMT_WDDM_2_9_CAPS {
  UINT Value = 0;
};
enum KMTQUERYADAPTERINFOTYPE {
  KMTQAITYPE_WDDM_2_7_CAPS = 70,
  KMTQAITYPE_WDDM_2_9_CAPS = 72,
};
struct D3DKMT_QUERYADAPTERINFO {
  UINT hAdapter = 0;
  KMTQUERYADAPTERINFOTYPE Type{};
  void* pPrivateDriverData = nullptr;
  UINT PrivateDriverDataSize = 0;
};

LONG D3DKMTOpenAdapterFromLuid(D3DKMT_OPENADAPTERFROMLUID*);
LONG D3DKMTQueryAdapterInfo(const D3DKMT_QUERYADAPTERINFO*);
LONG D3DKMTCloseAdapter(const D3DKMT_CLOSEADAPTER*);

enum NVSDK_NGX_Result : uint32_t {
  NVSDK_NGX_Result_Success = 1,
  NVSDK_NGX_Result_FAIL_InvalidParameter = 0xbad00005,
};
enum NVSDK_NGX_Feature : uint32_t {
  NVSDK_NGX_Feature_FrameGeneration = 11,
};
struct NVSDK_NGX_FeatureDiscoveryInfo {
  NVSDK_NGX_Feature FeatureID{};
};
struct NVSDK_NGX_FeatureRequirement {
  unsigned int FeatureSupported = 0;
  unsigned int MinHWArchitecture = 0;
  char MinOSVersion[255]{};
};
struct NVSDK_NGX_Parameter {};
struct NVSDK_NGX_Handle {
  unsigned int value = 0;
};
using PFN_NVSDK_NGX_ProgressCallback = void (*)(float, bool&);

NVSDK_NGX_Result NVSDK_NGX_D3D12_GetFeatureRequirements(
    IDXGIAdapter*, const NVSDK_NGX_FeatureDiscoveryInfo*, NVSDK_NGX_FeatureRequirement*);
NVSDK_NGX_Result NVSDK_NGX_D3D12_CreateFeature(
    ID3D12GraphicsCommandList*, NVSDK_NGX_Feature, NVSDK_NGX_Parameter*, NVSDK_NGX_Handle**);
NVSDK_NGX_Result NVSDK_NGX_D3D12_EvaluateFeature(
    ID3D12GraphicsCommandList*, const NVSDK_NGX_Handle*, const NVSDK_NGX_Parameter*,
    PFN_NVSDK_NGX_ProgressCallback);
NVSDK_NGX_Result NVSDK_NGX_D3D12_ReleaseFeature(NVSDK_NGX_Handle*);
NVSDK_NGX_Result NVSDK_NGX_VULKAN_GetFeatureRequirements(
    VkInstance, VkPhysicalDevice, const NVSDK_NGX_FeatureDiscoveryInfo*,
    NVSDK_NGX_FeatureRequirement*);

namespace sl {
using Feature = uint32_t;
constexpr Feature kFeatureDLSS_G = 1000;

enum class Result : uint32_t {
  eOk = 0,
  eErrorNotInitialized = 21,
  eErrorNoSupportedAdapterFound = 6,
  eErrorAdapterNotSupported = 7,
  eErrorFeatureMissing = 31,
  eErrorFeatureNotSupported = 32,
  eErrorOSDisabledHWS = 4,
};

struct AdapterInfo {
  void* vkPhysicalDevice = nullptr;
  const void* deviceLUID = nullptr;
  uint32_t deviceLUIDSizeInBytes = 0;
};
struct FeatureRequirements {
  uint32_t flags = 0;
  std::array<uint8_t, 64> payload{};
};
struct FeatureVersion {
  std::array<uint32_t, 8> payload{};
};
enum class PreferenceFlags : uint32_t {
  eAllowOTA = 1u << 5,
  eLoadDownloadedPlugins = 1u << 6,
};
struct Preferences {
  PreferenceFlags flags{};
  const Feature* features = nullptr;
  size_t numFeatures = 0;
};
constexpr uint64_t kSDKVersion = (2ull << 48) | (12ull << 32) | 0xfedc;
}  // namespace sl

using PFun_slInit = sl::Result(const sl::Preferences&, uint64_t);
using PFun_slIsFeatureSupported = sl::Result(sl::Feature, const sl::AdapterInfo&);
using PFun_slIsFeatureLoaded = sl::Result(sl::Feature, bool&);
using PFun_slGetFeatureRequirements = sl::Result(sl::Feature, sl::FeatureRequirements&);
using PFun_slGetFeatureVersion = sl::Result(sl::Feature, sl::FeatureVersion&);

namespace ImGui {
inline void Separator() {}
inline void TextUnformatted(const char*) {}
inline void Text(const char*, ...) {}
inline void TextWrapped(const char*, ...) {}
inline void TextDisabled(const char*, ...) {}
}  // namespace ImGui
