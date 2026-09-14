// SPDX-License-Identifier: MIT
// Hook double for fg_preset_test. No Windows code or Detours is executed.
#pragma once
#include <atomic>
#include "provider.hpp"
#ifndef __cdecl
#define __cdecl
#endif
inline void AcquireSRWLockShared(SRWLOCK* lock) { ++lock->shared; }

namespace preset_mock {
inline bool install_ok = true;
inline unsigned installs = 0;
inline void* original = nullptr;
}

namespace mfgunlock::hook {
struct ModuleIdentity { HMODULE module = nullptr; };
struct AddressHook {
  std::atomic<void*> target{nullptr};
  std::atomic<void*> trampoline{nullptr};
  std::atomic_bool installed{false};
  ModuleIdentity identity{};
  SRWLOCK lock = SRWLOCK_INIT;
  template <class Function> Function Original() const {
    return reinterpret_cast<Function>(trampoline.load());
  }
};
inline bool InstallAddress(AddressHook& entry, void* target, void*, const char*) {
  ++preset_mock::installs;
  if (!preset_mock::install_ok) return false;
  MEMORY_BASIC_INFORMATION memory{};
  if (!VirtualQuery(target, &memory, sizeof(memory))) return false;
  entry.identity.module = memory.AllocationBase;
  entry.target = target;
  entry.trampoline = preset_mock::original;
  entry.installed = true;
  return true;
}
inline void UninstallAddress(AddressHook& entry) {
  entry.installed = false;
  entry.trampoline = nullptr;
  entry.target = nullptr;
  entry.identity = {};
}
}  // namespace mfgunlock::hook
