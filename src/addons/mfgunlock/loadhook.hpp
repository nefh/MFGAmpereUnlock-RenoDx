/*
 * Load-time discovery for Streamline and NGX modules.
 * SPDX-License-Identifier: MIT
 *
 * A bootstrap scan covers modules mapped before the addon. LoadLibrary and
 * GetProcAddress hooks cover later mappings so capability hooks can be installed
 * before their first use, without polling from the presentation thread.
 */

#pragma once

#include <vector>
#include <windows.h>
#include <intrin.h>
#include <cstdint>
#include <atomic>
#include <cstring>

#include <include/reshade.hpp>

#include "./ngx_hook.hpp"

namespace mfgunlock::loadhook {

// Called with the freshly loaded DLSS-G snippet. Set by the addon.
inline void (*g_on_dlssg_loaded)(HMODULE) = nullptr;
// Optional resolver observer. It receives the result after GetProcAddress resolves it.
inline FARPROC (*g_on_get_proc_address)(HMODULE, LPCSTR, FARPROC, const void*) = nullptr;
// slInit must be hooked before the game calls it, and it calls it early --
// so catch the interposer as it is mapped rather than hoping to beat it.
inline void (*g_on_interposer_loaded)() = nullptr;
inline void (*g_on_ngx_loaded)(HMODULE) = nullptr;
inline std::atomic_bool g_hooked{false};

namespace internal {

constexpr wchar_t kNeedle[] = L"nvngx_dlssg";
constexpr wchar_t kOtaNeedle[] = L"\\models\\dlssg\\";
constexpr wchar_t kInterposerNeedle[] = L"sl.interposer";

inline bool NameContains(const wchar_t* path, const wchar_t* needle) {
  if (path == nullptr) return false;
  // Compare case-insensitively without touching the CRT locale machinery.
  for (const wchar_t* p = path; *p != L'\0'; ++p) {
    size_t i = 0;
    while (needle[i] != L'\0') {
      const wchar_t a = p[i];
      if (a == L'\0') break;
      const wchar_t lower = (a >= L'A' && a <= L'Z') ? static_cast<wchar_t>(a - L'A' + L'a') : a;
      if (lower != needle[i]) break;
      ++i;
    }
    if (needle[i] == L'\0') return true;
  }
  return false;
}

inline void Notify(HMODULE module, const wchar_t* path) {
  if (module == nullptr || (reinterpret_cast<uintptr_t>(module) & 3u) != 0) return;
  // Resolve basename-only and OTA loads before classifying the image.
  wchar_t resolved_path[32768] = {};
  const DWORD length = GetModuleFileNameW(module, resolved_path, ARRAYSIZE(resolved_path));
  if (length != 0 && length < ARRAYSIZE(resolved_path)) path = resolved_path;
  if (g_on_dlssg_loaded && (NameContains(path, kNeedle) || NameContains(path, kOtaNeedle))) {
    g_on_dlssg_loaded(module);
  }
  if (g_on_interposer_loaded && NameContains(path, kInterposerNeedle))
    g_on_interposer_loaded();
  if (g_on_ngx_loaded && NameContains(path, L"nvngx.dll")) g_on_ngx_loaded(module);
}

using LoadLibraryExWFn = HMODULE(WINAPI*)(LPCWSTR, HANDLE, DWORD);
using LoadLibraryWFn = HMODULE(WINAPI*)(LPCWSTR);
using GetProcAddressFn = FARPROC(WINAPI*)(HMODULE, LPCSTR);

inline LoadLibraryExWFn g_real_load_library_ex_w = nullptr;
inline LoadLibraryWFn g_real_load_library_w = nullptr;
inline GetProcAddressFn g_real_get_proc_address = nullptr;
inline HMODULE WINAPI HookedLoadLibraryExW(LPCWSTR file_name, HANDLE file, DWORD flags) {
  HMODULE module = g_real_load_library_ex_w(file_name, file, flags);
  const DWORD last_error = GetLastError();
  // Data-file mappings are not executable images; patching one would be
  // meaningless and the caller is not going to run code from it.
  constexpr DWORD kDataOnly = LOAD_LIBRARY_AS_DATAFILE | LOAD_LIBRARY_AS_DATAFILE_EXCLUSIVE |
                              LOAD_LIBRARY_AS_IMAGE_RESOURCE | DONT_RESOLVE_DLL_REFERENCES;
  if ((flags & kDataOnly) == 0) Notify(module, file_name);
  SetLastError(last_error);
  return module;
}

inline HMODULE WINAPI HookedLoadLibraryW(LPCWSTR file_name) {
  HMODULE module = g_real_load_library_w(file_name);
  const DWORD last_error = GetLastError();
  Notify(module, file_name);
  SetLastError(last_error);
  return module;
}
inline FARPROC WINAPI HookedGetProcAddress(HMODULE module, LPCSTR name) {
  FARPROC resolved = g_real_get_proc_address(module, name);
  const DWORD last_error = GetLastError();
  static thread_local bool resolving = false;
  if (resolved && !resolving) {
    resolving = true;
    struct Guard {
      ~Guard() { resolving = false; }
    } guard;
    if (reinterpret_cast<uintptr_t>(name) > 0xffff &&
        std::strcmp(name, "NVSDK_NGX_GetGPUArchitecture") == 0) Notify(module, nullptr);
    if (g_on_get_proc_address)
      resolved = g_on_get_proc_address(module, name, resolved, _ReturnAddress());
  }
  SetLastError(last_error);
  return resolved;
}

inline const std::vector<hook::HookItem> kHooks = {
    {"LoadLibraryExW", reinterpret_cast<void**>(&g_real_load_library_ex_w),
     reinterpret_cast<void*>(&HookedLoadLibraryExW)},
    {"LoadLibraryW", reinterpret_cast<void**>(&g_real_load_library_w),
     reinterpret_cast<void*>(&HookedLoadLibraryW)},
    {"GetProcAddress", reinterpret_cast<void**>(&g_real_get_proc_address),
     reinterpret_cast<void*>(&HookedGetProcAddress)},
};
}  // namespace internal

inline void TryInstall() {
  if (g_hooked.load(std::memory_order_acquire)) return;
  if (g_on_dlssg_loaded == nullptr) return;
  HMODULE kernel32 = GetModuleHandleW(L"kernel32.dll");
  if (kernel32 == nullptr) return;
  if (!hook::Install(kernel32, internal::kHooks, "kernel32.dll")) return;
  g_hooked.store(true, std::memory_order_release);
}

inline void Uninstall() {
  if (!g_hooked.load(std::memory_order_acquire)) return;
  hook::Uninstall(internal::kHooks);
  g_hooked.store(false, std::memory_order_release);
}

}  // namespace mfgunlock::loadhook
