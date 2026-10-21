// SPDX-License-Identifier: MIT
#pragma once

#include <atomic>
#include <windows.h>

namespace mfgdiagnostics::interposer {

inline bool OwnExecutableExport(HMODULE module, const char* name) {
  if (!module) return false;
  const auto address = GetProcAddress(module, name);
  if (!address) return false;
  MEMORY_BASIC_INFORMATION memory{};
  if (!VirtualQuery(reinterpret_cast<const void*>(address), &memory, sizeof(memory)) ||
      memory.AllocationBase != module || memory.State != MEM_COMMIT ||
      memory.Type != MEM_IMAGE || (memory.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0)
    return false;
  const auto access = memory.Protect & 0xff;
  return access == PAGE_EXECUTE || access == PAGE_EXECUTE_READ ||
         access == PAGE_EXECUTE_READWRITE || access == PAGE_EXECUTE_WRITECOPY;
}

inline bool Modern(HMODULE module) {
  // This public resolver establishes the modern typed call surface. Merely
  // finding a legacy slSetTag export does not establish its modern signature.
  return OwnExecutableExport(module, "slGetFeatureFunction");
}

class InventoryRequest {
 public:
  void Loaded() { pending_.store(true, std::memory_order_release); }
  bool Take() { return pending_.exchange(false, std::memory_order_acq_rel); }
 private:
  std::atomic_bool pending_{true};
};

}  // namespace mfgdiagnostics::interposer
