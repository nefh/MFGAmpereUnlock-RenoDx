// SPDX-License-Identifier: MIT
// Execute the production loader hooks with the existing host API doubles.
// This is a deterministic notification/cadence regression, not a Windows,
// Detours, real provider or wall-clock performance qualification.
#include "loadhook.hpp"

#include <iostream>
#include <stdexcept>

namespace loader = mfgunlock::loadhook;
namespace {
unsigned int g_checks = 0;
unsigned int g_notifications = 0;
unsigned int g_observations = 0;
unsigned int g_interposer_notifications = 0;
unsigned int g_ngx_notifications = 0;
unsigned int g_raw_calls = 0;
unsigned g_load_reports = 0;
HMODULE g_load_result = nullptr;
DWORD g_load_error = 0;
DWORD g_load_flags = 0;
void ObserveLoadResult(LPCWSTR, HMODULE module, DWORD flags, DWORD error) noexcept {
  ++g_load_reports;
  g_load_result = module;
  g_load_error = error;
  g_load_flags = flags;
  SetLastError(7777);
}
bool g_pending = false;
bool g_resolve_ok = true;
bool g_nested_resolve = false;
bool g_nested_load = false;
const void* g_observed_caller = nullptr;
HMODULE g_provider = reinterpret_cast<HMODULE>(0x10000);
HMODULE g_self = reinterpret_cast<HMODULE>(0x20000);
HMODULE g_game = reinterpret_cast<HMODULE>(0x30000);
void* g_self_caller = reinterpret_cast<void*>(0x20100);
void* g_game_caller = reinterpret_cast<void*>(0x30100);
constexpr const char* kArchitectureExport = "NVSDK_NGX_GetGPUArchitecture";
constexpr DWORD kLastError = 123;

void Check(bool value, const char* reason) {
  ++g_checks;
  if (!value) throw std::runtime_error(reason);
}
void Export() {}
FARPROC WINAPI RawResolve(HMODULE module, LPCSTR) {
  ++g_raw_calls;
  SetLastError(kLastError);
  return module && g_resolve_ok ? &Export : nullptr;
}
HMODULE WINAPI RawLoad(LPCWSTR name) {
  SetLastError(kLastError);
  return GetModuleHandleW(name);
}
HMODULE WINAPI RawLoadEx(LPCWSTR name, HANDLE, DWORD) { return RawLoad(name); }
HMODULE WINAPI RawLoadA(LPCSTR name) {
  SetLastError(kLastError);
  return name && std::strcmp(name, "nvngx_dlssg.dll") == 0 ? g_provider : nullptr;
}
HMODULE WINAPI RawLoadExA(LPCSTR name, HANDLE, DWORD) { return RawLoadA(name); }
void MarkPending(HMODULE) {
  ++g_notifications;
  g_pending = true;
  SetLastError(999);
}
FARPROC Observe(HMODULE module, LPCSTR, FARPROC result, const void* caller) {
  ++g_observations;
  g_observed_caller = caller;
  if (g_nested_resolve) loader::internal::HookedGetProcAddress(module, kArchitectureExport);
  // A real load nested inside an observer must NOT be dropped, even when the
  // outer export lookup came from the addon and was filtered as self-originated.
  if (g_nested_load) loader::internal::HookedLoadLibraryW(L"nvngx_dlssg.dll");
  SetLastError(998);
  return result;
}
void Reset() {
  g_notifications = g_observations = g_raw_calls = 0;
  g_interposer_notifications = g_ngx_notifications = 0;
  g_pending = false;
  g_load_reports = 0;
  loader::g_on_load_result = nullptr;
  g_resolve_ok = true;
  g_nested_resolve = g_nested_load = false;
  g_observed_caller = nullptr;
  mock::paths.clear();
  mock::modules.clear();
  mock::allocation.clear();
  mock::paths[g_provider] = L"C:\\game\\nvngx_dlssg.dll";
  mock::modules[L"nvngx_dlssg.dll"] = g_provider;
  mock::allocation[g_self_caller] = g_self;
  mock::allocation[g_game_caller] = g_game;
  mock::allocation[reinterpret_cast<const void*>(&loader::internal::HookedGetProcAddress)] = g_self;
  mock::return_address = g_self_caller;
  loader::g_on_dlssg_loaded = MarkPending;
  loader::g_on_interposer_loaded = [] { ++g_interposer_notifications; };
  loader::g_on_ngx_loaded = [](HMODULE) { ++g_ngx_notifications; };
  loader::g_on_get_proc_address = Observe;
  loader::internal::g_real_get_proc_address = RawResolve;
  loader::internal::g_real_load_library_a = RawLoadA;
  loader::internal::g_real_load_library_ex_a = RawLoadExA;
  loader::internal::g_real_load_library_w = RawLoad;
  loader::internal::g_real_load_library_ex_w = RawLoadEx;
}
void TestSteadyState() {
  Reset();
  g_pending = true;
  unsigned int refreshes = 0;
  for (unsigned int present = 0; present < 240; ++present) {
    // Same pending-or-periodic scheduling as OnPresentUiOutput. Six lookups
    // model two maintenance passes, each identifying/observing the provider
    // three times. The assertion does not depend on their cost or count.
    if (!g_pending && present % 120 != 0) continue;
    ++refreshes;
    g_pending = false;
    for (unsigned int lookup = 0; lookup < 6; ++lookup)
      loader::internal::HookedGetProcAddress(g_provider, kArchitectureExport);
  }
  std::cout << "steady presents=240 refreshes=" << refreshes
            << " own_notifications=" << g_notifications
            << " raw_lookups=" << g_raw_calls << " pending=" << g_pending << '\n';
  Check(refreshes == 2, "self-resolution re-armed every Present instead of periodic maintenance");
  Check(!g_pending && g_notifications == 0, "own lookup raised a load notification");
  Check(g_raw_calls == 12 && g_observations == 12, "self filtering changed resolver/observer delivery");
  Check(g_observed_caller == g_self_caller, "observer caller changed");
  Check(GetLastError() == kLastError, "GetProcAddress LastError changed");
}
void TestExternalDiscovery() {
  Reset();
  mock::return_address = g_game_caller;
  Check(loader::internal::HookedGetProcAddress(g_provider, kArchitectureExport) == &Export,
        "foreign lookup result changed");
  Check(g_notifications == 1 && g_pending, "foreign provider discovery lost");
  Check(GetLastError() == kLastError, "foreign lookup LastError changed");
  // OTA names need not contain nvngx_dlssg.
  mock::paths[g_provider] = L"C:\\NVIDIA\\models\\dlssg\\opaque.bin";
  loader::internal::HookedGetProcAddress(g_provider, kArchitectureExport);
  Check(g_notifications == 2, "renamed OTA provider discovery lost");
  mock::paths[g_provider] = L"C:\\game\\unrelated.dll";
  loader::internal::HookedGetProcAddress(g_provider, kArchitectureExport);
  Check(g_notifications == 2, "unrelated path treated as a provider");
}
void TestUnknownCaller() {
  Reset();
  mock::return_address = nullptr;
  loader::internal::HookedGetProcAddress(g_provider, kArchitectureExport);
  Check(g_notifications == 1, "null caller must not suppress discovery");
  mock::return_address = reinterpret_cast<void*>(0x40000);
  loader::internal::HookedGetProcAddress(g_provider, kArchitectureExport);
  Check(g_notifications == 2, "unmapped caller must not suppress discovery");
  mock::return_address = g_self_caller;
  mock::allocation.erase(reinterpret_cast<const void*>(&loader::internal::HookedGetProcAddress));
  loader::internal::HookedGetProcAddress(g_provider, kArchitectureExport);
  Check(g_notifications == 3, "unknown hook allocation must not suppress discovery");
}
void TestLoadNotifications() {
  Reset();
  // Real loads from the addon still notify. Filtering them would lose nested
  // loads during provider preparation and would conceal a different bug.
  Check(loader::internal::HookedLoadLibraryW(L"nvngx_dlssg.dll") == g_provider,
        "LoadLibraryW result changed");
  Check(g_notifications == 1 && GetLastError() == kLastError, "real self-load lost");
  loader::internal::HookedLoadLibraryExW(L"nvngx_dlssg.dll", nullptr, 0);
  Check(g_notifications == 2 && GetLastError() == kLastError, "real self-load Ex lost");
  for (DWORD flag : {LOAD_LIBRARY_AS_DATAFILE, LOAD_LIBRARY_AS_DATAFILE_EXCLUSIVE,
                     LOAD_LIBRARY_AS_IMAGE_RESOURCE, DONT_RESOLVE_DLL_REFERENCES})
    loader::internal::HookedLoadLibraryExW(L"nvngx_dlssg.dll", nullptr, flag);
  Check(g_notifications == 2, "data-file/resource-only load notified");
  loader::internal::HookedLoadLibraryW(L"missing.dll");
  Check(g_notifications == 2, "failed load notified");
  mock::modules[L"sl.interposer.dll"] = g_game;
  mock::paths[g_game] = L"C:\\game\\sl.interposer.dll";
  loader::internal::HookedLoadLibraryW(L"sl.interposer.dll");
  Check(g_interposer_notifications == 1, "early slInit load notification lost");
  mock::paths[g_game] = L"C:\\game\\_nvngx.dll";
  mock::modules[L"_nvngx.dll"] = g_game;
  loader::internal::HookedLoadLibraryW(L"_nvngx.dll");
  Check(g_ngx_notifications == 1, "direct NGX load notification lost");
}
void TestLoadResultObservation() {
  Reset();
  loader::g_on_load_result = ObserveLoadResult;
  Check(loader::internal::HookedLoadLibraryW(L"missing-sr.dll") == nullptr &&
        g_load_reports == 1 && g_load_result == nullptr && g_load_error == kLastError &&
        GetLastError() == kLastError && g_notifications == 0,
        "failed load observer preserves NULL and native Win32 error");
  Check(loader::internal::HookedLoadLibraryExW(L"nvngx_dlssg.dll", nullptr, LOAD_LIBRARY_AS_DATAFILE) == g_provider &&
        g_load_reports == 2 && g_load_flags == LOAD_LIBRARY_AS_DATAFILE &&
        GetLastError() == kLastError && g_notifications == 0,
        "data-file diagnostic cannot raise a provider-load notification");
  Check(loader::internal::HookedLoadLibraryW(L"nvngx_dlssg.dll") == g_provider &&
        g_load_reports == 3 && g_load_result == g_provider && g_notifications == 1 &&
        GetLastError() == kLastError, "success observer leaves discovery and LastError intact");
  Check(loader::internal::HookedLoadLibraryExA("missing-sr.dll", nullptr, 0) == nullptr &&
        g_load_reports == 4 && g_load_error == kLastError && GetLastError() == kLastError,
        "LoadLibraryExA failure and native Win32 error observed without modification");
  Check(loader::internal::HookedLoadLibraryA("nvngx_dlssg.dll") == g_provider &&
        g_load_reports == 5 && GetLastError() == kLastError && g_notifications == 1,
        "ANSI observer neither adds notifications nor changes return values");
  loader::g_on_load_result = nullptr;
}
void TestResolverEdges() {
  Reset();
  mock::return_address = g_game_caller;
  loader::internal::HookedGetProcAddress(g_provider, reinterpret_cast<LPCSTR>(7));
  loader::internal::HookedGetProcAddress(g_provider, nullptr);
  loader::internal::HookedGetProcAddress(g_provider, "unrelated_export");
  Check(g_notifications == 0 && g_observations == 3, "ordinal/null/other resolver behavior changed");
  g_resolve_ok = false;
  Check(loader::internal::HookedGetProcAddress(g_provider, kArchitectureExport) == nullptr,
        "failed resolution changed");
  Check(g_notifications == 0 && g_observations == 3, "failed resolution notified");
  g_resolve_ok = true;
  g_nested_resolve = true;
  loader::internal::HookedGetProcAddress(g_provider, kArchitectureExport);
  Check(g_notifications == 1 && g_observations == 4, "reentrant resolver was not bounded");
  Check(GetLastError() == kLastError, "nested resolver LastError changed");
  g_nested_resolve = false;
  g_nested_load = true;
  mock::return_address = g_self_caller;
  loader::internal::HookedGetProcAddress(g_provider, kArchitectureExport);
  Check(g_notifications == 2, "real nested load was swallowed with the own lookup");
  Check(GetLastError() == kLastError, "nested load LastError changed");
  g_nested_load = false;
  loader::g_on_get_proc_address = nullptr;
  mock::return_address = g_game_caller;
  loader::internal::HookedGetProcAddress(g_provider, kArchitectureExport);
  Check(g_notifications == 3, "provider discovery depends on optional resolver observer");
}
}  // namespace
int main() {
  try {
    TestSteadyState();
    TestExternalDiscovery();
    TestUnknownCaller();
    TestLoadNotifications();
    TestLoadResultObservation();
    TestResolverEdges();
    std::cout << "PASS: " << g_checks << " loader notification checks\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "FAIL: " << error.what() << '\n';
    return 1;
  }
}
