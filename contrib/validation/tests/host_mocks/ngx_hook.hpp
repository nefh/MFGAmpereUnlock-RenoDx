// Does not install machine-code detours. Tests trampoline selection only.
#pragma once

#include "mock_api.hpp"

#include <atomic>
#include <tuple>
#include <vector>

namespace mfgunlock::hook {

using HookItem = std::tuple<const char*, void**, void*>;

inline bool Install(HMODULE module, const std::vector<HookItem>& hooks, const char*) {
  ++mock::installs;
  if (!mock::install_ok) return false;

  for (const auto& [name, storage, replacement] : hooks) {
    (void)replacement;
    if (*storage || !GetProcAddress(module, name)) return false;
  }

  for (const auto& [name, storage, replacement] : hooks) {
    (void)replacement;
    *storage = reinterpret_cast<void*>(GetProcAddress(module, name));
  }

  return true;
}

inline void Uninstall(const std::vector<HookItem>& hooks) {
  for (const auto& [name, storage, replacement] : hooks) {
    (void)name;
    (void)replacement;
    *storage = nullptr;
  }
}

struct ModuleIdentity {
  HMODULE module = nullptr;
  DWORD timestamp = 0;
  DWORD image_bytes = 0;

  bool operator==(const ModuleIdentity& other) const {
    return module == other.module &&
           timestamp == other.timestamp &&
           image_bytes == other.image_bytes;
  }
};

inline bool CaptureModule(HMODULE module, ModuleIdentity& identity) {
  identity = {};
  if (!module) return false;

  identity.module = module;
  identity.timestamp = 1;
  identity.image_bytes = 1;
  return true;
}

inline bool IsCurrent(const ModuleIdentity& identity) {
  return identity.module != nullptr;
}

struct AddressHook {
  std::atomic<void*> target{nullptr};
  std::atomic<void*> replacement{nullptr};
  std::atomic<void*> trampoline{nullptr};
  ModuleIdentity identity{};
  std::atomic_bool installed{false};

  template <class Function>
  Function Original() const {
    return reinterpret_cast<Function>(
        trampoline.load(std::memory_order_acquire));
  }

  bool Targets(const void* address) const {
    return installed.load(std::memory_order_acquire) &&
           target.load(std::memory_order_acquire) == address;
  }
};

inline void ClearAddressState(AddressHook& state) {
  state.installed.store(false, std::memory_order_release);
  state.trampoline.store(nullptr, std::memory_order_release);
  state.target.store(nullptr, std::memory_order_release);
  state.replacement.store(nullptr, std::memory_order_release);
  state.identity = {};
}

inline bool InstallAddress(AddressHook& state,
                           void* target,
                           void* replacement,
                           const char*) {
  if (!target || !replacement || !mock::install_ok) return false;

  if (state.installed.load(std::memory_order_acquire)) {
    return state.target.load(std::memory_order_acquire) == target &&
           state.replacement.load(std::memory_order_acquire) == replacement;
  }

  ModuleIdentity identity;
  if (!CaptureModule(reinterpret_cast<HMODULE>(target), identity)) return false;

  state.identity = identity;
  state.target.store(target, std::memory_order_release);
  state.replacement.store(replacement, std::memory_order_release);
  state.trampoline.store(target, std::memory_order_release);
  state.installed.store(true, std::memory_order_release);

  return true;
}

inline void UninstallAddress(AddressHook& state) {
  ClearAddressState(state);
}

}  // namespace mfgunlock::hook
