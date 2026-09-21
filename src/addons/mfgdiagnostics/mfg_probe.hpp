// SPDX-License-Identifier: MIT
#pragma once
#include <array>
#include <cstring>
#include <type_traits>
#include <nvsdk_ngx_params.h>
#include "../mfgunlock/ngx_hook.hpp"
#include "../mfgunlock/count_observer.hpp"

namespace mfgdiagnostics::probe {
namespace count = mfgunlock::countobserver;
using Recording = bool (*)();
inline std::atomic<count::Callback> g_sink{nullptr};
inline std::atomic<Recording> g_recording{nullptr};
inline constexpr const char* kExports[] = {
    "NVSDK_NGX_Parameter_SetUI", "NVSDK_NGX_Parameter_SetI",
    "NVSDK_NGX_Parameter_GetUI", "NVSDK_NGX_Parameter_GetI"};
struct Slot {
  HMODULE module = nullptr;
  std::array<void*, 4> targets{};
  PFN_NVSDK_NGX_Parameter_SetUI set_unsigned = nullptr;
  PFN_NVSDK_NGX_Parameter_SetI set_signed = nullptr;
  PFN_NVSDK_NGX_Parameter_GetUI get_unsigned = nullptr;
  PFN_NVSDK_NGX_Parameter_GetI get_signed = nullptr;
};
inline std::array<Slot, 8> g_slots;
inline unsigned int g_hook_count = 0;
inline bool g_capacity_exhausted = false;

inline bool CountKey(const char* name, count::Key& key) {
  if (!name) return false;
  if (std::strcmp(name, "DLSSG.MultiFrameCount") == 0) key = count::Key::kGenerated;
  else if (std::strcmp(name, "DLSSG.MultiFrameCountMax") == 0) key = count::Key::kMaximum;
  else if (std::strcmp(name, "DLSSG.MultiFrameIndex") == 0) key = count::Key::kIndex;
  else return false;
  return true;
}

inline void Record(NVSDK_NGX_Parameter* parameters, const char* name, int64_t value,
                   bool known, uint32_t result, count::Operation operation,
                   uint64_t caller, uint64_t callee) noexcept {
  const auto sink = g_sink.load(std::memory_order_acquire);
  const auto recording = g_recording.load(std::memory_order_acquire);
  if (!sink || !recording) return;
  struct LastError {
    DWORD value = GetLastError();
    ~LastError() { SetLastError(value); }
  } last_error;
  if (!recording()) return;
  count::Event event;
  if (!CountKey(name, event.key)) return;
  event.backend = count::Backend::kNgxC;
  event.operation = operation;
  event.value = value;
  event.value_known = known;
  event.result = result;
  event.caller = caller;
  event.callee = callee;
  event.object = reinterpret_cast<uint64_t>(parameters);
  // C exports do not identify D3D12/Vulkan or core override intent themselves.
  sink(&event);
}

template <size_t I, class T>
void NVSDK_CONV Set(NVSDK_NGX_Parameter* parameters, const char* name, T value) {
  auto real = [] {
    if constexpr (std::is_same_v<T, unsigned int>) return g_slots[I].set_unsigned;
    else return g_slots[I].set_signed;
  }();
  real(parameters, name, value);
  Record(parameters, name, value, true, count::kUnknown, count::Operation::kWrite,
         count::CallerAddress(), reinterpret_cast<uint64_t>(
             g_slots[I].targets[std::is_same_v<T, unsigned int> ? 0 : 1]));
}

template <size_t I, class T>
NVSDK_NGX_Result NVSDK_CONV Get(NVSDK_NGX_Parameter* parameters, const char* name, T* value) {
  auto real = [] {
    if constexpr (std::is_same_v<T, unsigned int>) return g_slots[I].get_unsigned;
    else return g_slots[I].get_signed;
  }();
  const auto result = real(parameters, name, value);
  const bool known = result == NVSDK_NGX_Result_Success && value != nullptr;
  Record(parameters, name, known ? static_cast<int64_t>(*value) : 0, known,
         static_cast<uint32_t>(result), count::Operation::kRead,
         count::CallerAddress(), reinterpret_cast<uint64_t>(
             g_slots[I].targets[std::is_same_v<T, unsigned int> ? 2 : 3]));
  return result;
}

static_assert(std::is_same_v<decltype(&Set<0, unsigned int>), PFN_NVSDK_NGX_Parameter_SetUI>);
static_assert(std::is_same_v<decltype(&Set<0, int>), PFN_NVSDK_NGX_Parameter_SetI>);
static_assert(std::is_same_v<decltype(&Get<0, unsigned int>), PFN_NVSDK_NGX_Parameter_GetUI>);
static_assert(std::is_same_v<decltype(&Get<0, int>), PFN_NVSDK_NGX_Parameter_GetI>);

template <size_t I>
bool InstallSlot(HMODULE module) {
  auto& slot = g_slots[I];
  const std::array<mfgunlock::hook::HookItem, 4> candidates = {{
      {kExports[0], reinterpret_cast<void**>(&slot.set_unsigned), reinterpret_cast<void*>(&Set<I, unsigned int>)},
      {kExports[1], reinterpret_cast<void**>(&slot.set_signed), reinterpret_cast<void*>(&Set<I, int>)},
      {kExports[2], reinterpret_cast<void**>(&slot.get_unsigned), reinterpret_cast<void*>(&Get<I, unsigned int>)},
      {kExports[3], reinterpret_cast<void**>(&slot.get_signed), reinterpret_cast<void*>(&Get<I, int>)}}};
  std::vector<mfgunlock::hook::HookItem> hooks;
  for (size_t n = 0; n < candidates.size(); ++n) {
    void* target = reinterpret_cast<void*>(GetProcAddress(module, kExports[n]));
    if (!target) continue;
    bool duplicate = false;
    // Shared failure stubs can alias APIs with incompatible Set/Get signatures.
    // Reject every member of that alias group, not only the second export.
    for (size_t other = 0; other < candidates.size(); ++other)
      if (other != n && GetProcAddress(module, kExports[other]) == reinterpret_cast<FARPROC>(target))
        duplicate = true;
    for (const auto& existing : g_slots)
      for (void* address : existing.targets) duplicate |= address == target;
    if (duplicate) continue;
    slot.targets[n] = target;
    hooks.push_back(candidates[n]);
  }
  if (hooks.empty()) { slot.targets = {}; return false; }
  HMODULE retained = nullptr;
  if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                          reinterpret_cast<LPCWSTR>(module), &retained)) {
    slot.targets = {};
    return false;
  }
  if (!mfgunlock::hook::Install(module, hooks, "NGX count observation")) {
    FreeLibrary(retained);
    slot.targets = {};
    return false;
  }
  slot.module = retained;
  g_hook_count += static_cast<unsigned int>(hooks.size());
  return true;
}

// Invoked on the UI thread before capture; never on the per-frame path.
inline void Install(HMODULE module) {
  for (const auto& slot : g_slots) if (slot.module == module) return;
  using Installer = bool (*)(HMODULE);
  static constexpr Installer kInstallers[] = {
      InstallSlot<0>, InstallSlot<1>, InstallSlot<2>, InstallSlot<3>,
      InstallSlot<4>, InstallSlot<5>, InstallSlot<6>, InstallSlot<7>};
  for (size_t i = 0; i < g_slots.size(); ++i) {
    if (!g_slots[i].module) { kInstallers[i](module); return; }
  }
  for (const char* name : kExports)
    if (GetProcAddress(module, name)) g_capacity_exhausted = true;
}


template <size_t I>
bool UninstallSlot() {
  auto& slot = g_slots[I];
  if (!slot.module) return true;
  std::vector<mfgunlock::hook::HookItem> hooks;
  if (slot.targets[0]) hooks.emplace_back(
      kExports[0], reinterpret_cast<void**>(&slot.set_unsigned),
      reinterpret_cast<void*>(&Set<I, unsigned int>));
  if (slot.targets[1]) hooks.emplace_back(
      kExports[1], reinterpret_cast<void**>(&slot.set_signed),
      reinterpret_cast<void*>(&Set<I, int>));
  if (slot.targets[2]) hooks.emplace_back(
      kExports[2], reinterpret_cast<void**>(&slot.get_unsigned),
      reinterpret_cast<void*>(&Get<I, unsigned int>));
  if (slot.targets[3]) hooks.emplace_back(
      kExports[3], reinterpret_cast<void**>(&slot.get_signed),
      reinterpret_cast<void*>(&Get<I, int>));
  if (!hooks.empty() && !mfgunlock::hook::TryUninstall(hooks)) return false;

  HMODULE retained = slot.module;
  slot = {};
  if (g_hook_count >= hooks.size()) g_hook_count -= static_cast<unsigned int>(hooks.size());
  else g_hook_count = 0;
  FreeLibrary(retained);
  return true;
}

inline bool Uninstall() {
  using Uninstaller = bool (*)();
  static constexpr Uninstaller kUninstallers[] = {
      UninstallSlot<0>, UninstallSlot<1>, UninstallSlot<2>, UninstallSlot<3>,
      UninstallSlot<4>, UninstallSlot<5>, UninstallSlot<6>, UninstallSlot<7>};
  bool ok = true;
  for (const auto uninstall : kUninstallers)
    if (!uninstall()) ok = false;
  if (ok) g_capacity_exhausted = false;
  return ok;
}
}  // namespace mfgdiagnostics::probe
