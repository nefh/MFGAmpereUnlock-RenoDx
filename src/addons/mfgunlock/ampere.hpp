/*
 * Native DLSS-G provider preparation for the selected architecture.
 * SPDX-License-Identifier: MIT
 *
 * Retargets compatible sm_89 PTX in the mapped provider, hides the matching
 * sm_89 cubins, and lowers the provider architecture gate to the selected GPU.
 */
#pragma once

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <limits>
#include <span>
#include <sstream>
#include <string>
#include <vector>

#include <include/reshade.hpp>

#include "./ampere_ptx.hpp"

namespace mfgunlock::ampere {

inline std::atomic_bool g_create_seen{false};

inline void Log(const std::string& message, bool warning = false) {
  reshade::log::message(warning ? reshade::log::level::warning : reshade::log::level::info,
                        ("mfgunlock: " + message).c_str());
}

namespace internal {

struct BytePatch {
  unsigned char* address;
  unsigned char before;
  unsigned char after;
  DWORD original_protection = 0;
};

struct ImageIdentity {
  DWORD timestamp = 0;
  DWORD image_bytes = 0;
  bool operator==(const ImageIdentity&) const = default;
};

struct Provider {
  HMODULE module;
  std::vector<BytePatch> patches;
  bool ready = false;
  unsigned fatbins = 0;
  unsigned hidden_cubins = 0;
  ImageIdentity identity;
  std::string path;
  std::string failure;
  Architecture architecture = Architecture::kUnknown;
};

struct Rejection {
  HMODULE module;
  ImageIdentity identity;
  std::string path;
  std::string reason;
};
inline SRWLOCK g_lock = SRWLOCK_INIT;
inline std::vector<Provider> g_providers;
inline std::vector<Rejection> g_rejected;
inline std::atomic_uint g_ignored_mappings{0};
inline std::atomic_bool g_registry_failed{false};

inline bool IsReadable(const void* pointer, size_t bytes, HMODULE allocation) {
  uintptr_t at = reinterpret_cast<uintptr_t>(pointer);
  if (bytes > std::numeric_limits<uintptr_t>::max() - at) return false;
  const uintptr_t end = at + bytes;
  while (at < end) {
    MEMORY_BASIC_INFORMATION memory = {};
    if (!VirtualQuery(reinterpret_cast<void*>(at), &memory, sizeof(memory)) ||
        memory.AllocationBase != allocation || memory.State != MEM_COMMIT ||
        (memory.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0) {
      return false;
    }
    const DWORD protection = memory.Protect & 0xFF;
    if (protection != PAGE_READONLY && protection != PAGE_READWRITE &&
        protection != PAGE_WRITECOPY && protection != PAGE_EXECUTE_READ &&
        protection != PAGE_EXECUTE_READWRITE && protection != PAGE_EXECUTE_WRITECOPY) {
      return false;
    }
    const auto base = reinterpret_cast<uintptr_t>(memory.BaseAddress);
    if (memory.RegionSize > std::numeric_limits<uintptr_t>::max() - base) return false;
    const auto next = base + memory.RegionSize;
    if (next <= at) return false;
    at = std::min(next, end);
  }
  return true;
}

// A LoadLibrary name match is only a discovery hint. Resource handles may be
// tagged, and failed candidates can disappear after an NGX inspection load.
// Neither is evidence of a second executable provider.
inline bool IsImageMapping(HMODULE module) {
  if (!module || (reinterpret_cast<uintptr_t>(module) & 3u) != 0) return false;
  MEMORY_BASIC_INFORMATION memory = {};
  return VirtualQuery(module, &memory, sizeof(memory)) &&
         memory.AllocationBase == module && memory.State == MEM_COMMIT &&
         memory.Type == MEM_IMAGE && !(memory.Protect & (PAGE_GUARD | PAGE_NOACCESS));
}
inline bool ReadIdentity(HMODULE module, ImageIdentity& identity) {
  if (!IsImageMapping(module) || !IsReadable(module, sizeof(IMAGE_DOS_HEADER), module)) {
    return false;
  }
  const auto* base = reinterpret_cast<const unsigned char*>(module);
  IMAGE_DOS_HEADER dos = {};
  std::memcpy(&dos, base, sizeof(dos));
  if (dos.e_magic != IMAGE_DOS_SIGNATURE || dos.e_lfanew < 64 || dos.e_lfanew > 1024 * 1024)
    return false;
  const auto* location = base + dos.e_lfanew;
  if (!IsReadable(location, sizeof(IMAGE_NT_HEADERS64), module)) return false;
  IMAGE_NT_HEADERS64 nt = {};
  std::memcpy(&nt, location, sizeof(nt));
  if (nt.Signature != IMAGE_NT_SIGNATURE || nt.FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64 ||
      nt.OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) return false;
  identity = {nt.FileHeader.TimeDateStamp, nt.OptionalHeader.SizeOfImage};
  return identity.image_bytes != 0;
}
inline bool IsCurrent(HMODULE module, const ImageIdentity& expected) {
  ImageIdentity actual;
  return ReadIdentity(module, actual) && actual == expected;
}
inline std::string ModulePath(HMODULE module) {
  char path[32768] = {};
  const auto length = GetModuleFileNameA(module, path, ARRAYSIZE(path));
  return length && length < ARRAYSIZE(path) ? std::string(path, length) : "<path unavailable>";
}

struct Image {
  unsigned char* base = nullptr;
  size_t bytes = 0;
  std::vector<IMAGE_SECTION_HEADER> sections;
};
inline bool InspectImage(HMODULE module, Image& image) {
  auto* base = reinterpret_cast<unsigned char*>(module);
  if (!IsReadable(base, sizeof(IMAGE_DOS_HEADER), module)) return false;
  IMAGE_DOS_HEADER dos;
  std::memcpy(&dos, base, sizeof(dos));
  if (dos.e_magic != IMAGE_DOS_SIGNATURE || dos.e_lfanew < 64 || dos.e_lfanew > 1024 * 1024)
    return false;
  auto* location = base + dos.e_lfanew;
  if (!IsReadable(location, sizeof(IMAGE_NT_HEADERS64), module)) return false;
  IMAGE_NT_HEADERS64 nt;
  std::memcpy(&nt, location, sizeof(nt));
  if (nt.Signature != IMAGE_NT_SIGNATURE || nt.FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64 ||
      nt.OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC ||
      nt.FileHeader.SizeOfOptionalHeader != sizeof(IMAGE_OPTIONAL_HEADER64) ||
      nt.FileHeader.NumberOfSections == 0 || nt.FileHeader.NumberOfSections > 96 ||
      nt.OptionalHeader.SizeOfImage == 0 || nt.OptionalHeader.SizeOfImage > 512 * 1024 * 1024)
    return false;
  const size_t table_offset = static_cast<size_t>(dos.e_lfanew) + sizeof(nt);
  const size_t table_bytes = nt.FileHeader.NumberOfSections * sizeof(IMAGE_SECTION_HEADER);
  if (table_offset > nt.OptionalHeader.SizeOfHeaders ||
      table_bytes > nt.OptionalHeader.SizeOfHeaders - table_offset ||
      !IsReadable(base + table_offset, table_bytes, module)) return false;
  if (nt.OptionalHeader.SizeOfHeaders > nt.OptionalHeader.SizeOfImage) return false;
  image.base = base;
  image.bytes = nt.OptionalHeader.SizeOfImage;
  image.sections.resize(nt.FileHeader.NumberOfSections);
  std::memcpy(image.sections.data(), base + table_offset, table_bytes);
  std::vector<std::pair<size_t, size_t>> ranges;
  for (const auto& section : image.sections) {
    if (section.VirtualAddress > image.bytes ||
        section.Misc.VirtualSize > image.bytes - section.VirtualAddress) {
      return false;
    }
    if (section.Misc.VirtualSize != 0) {
      ranges.emplace_back(section.VirtualAddress,
                          section.VirtualAddress + section.Misc.VirtualSize);
    }
  }
  std::sort(ranges.begin(), ranges.end());
  for (size_t i = 1; i < ranges.size(); ++i) {
    if (ranges[i].first < ranges[i - 1].second) return false;
  }
  return true;
}

inline bool OwnCodeExport(const Image& image, HMODULE module, const char* name) {
  const auto* address = reinterpret_cast<const unsigned char*>(GetProcAddress(module, name));
  if (!address || !IsReadable(address, 1, module)) return false;
  const auto value = reinterpret_cast<uintptr_t>(address);
  for (const auto& section : image.sections) {
    const auto begin = reinterpret_cast<uintptr_t>(image.base + section.VirtualAddress);
    if ((section.Characteristics & IMAGE_SCN_MEM_EXECUTE) && value >= begin &&
        value - begin < section.Misc.VirtualSize) {
      return true;
    }
  }
  return false;
}

// Writes never span protection regions: only the changed bytes are stored.
// Both protection restoration and read-back are checked. Undo is registered
// before calling this, including the case where the write succeeds but restore fails.
inline bool WriteByte(BytePatch& patch, bool restore) {
  const auto expected = restore ? patch.after : patch.before;
  const auto desired = restore ? patch.before : patch.after;
  if (*patch.address != expected && !(restore && *patch.address == desired)) return false;

  DWORD old_protection = 0;
  DWORD ignored = 0;
  if (!VirtualProtect(patch.address, 1, PAGE_EXECUTE_READWRITE, &old_protection)) return false;
  if (patch.original_protection == 0) patch.original_protection = old_protection;

  *patch.address = desired;
  const bool protected_again =
      VirtualProtect(patch.address, 1, patch.original_protection, &ignored) != FALSE;
  const bool flushed = FlushInstructionCache(GetCurrentProcess(), patch.address, 1) != FALSE;
  return protected_again && flushed && *patch.address == desired;
}
inline bool Undo(Provider& provider) {
  bool ok = true;
  for (auto it = provider.patches.rbegin(); it != provider.patches.rend(); ++it) {
    if (!IsReadable(it->address, 1, provider.module)) {
      ok = false;
      continue;
    }
    if (*it->address == it->before && it->original_protection == 0) continue;
    if (!WriteByte(*it, true)) ok = false;
  }
  provider.ready = false;
  return ok;
}
}  // namespace internal

struct ProviderStatus {
  unsigned ready = 0;
  unsigned blocked = 0;
  unsigned expired_rejections = 0;
  unsigned ignored_mappings = 0;
  bool busy = false;
  bool failed = false;
  unsigned int QualifiedCount() const {
    return busy || failed || blocked ? 0 : ready;
  }
  const char* Reason() const {
    if (busy) return "provider-registry-busy";
    if (failed) return "provider-registry-failed";
    if (blocked) return "active-provider-rejected-or-invalidated";
    if (ready > 1) return "multiple-prepared-providers";
    if (!ready) return "no-prepared-provider";
    return "one-prepared-provider";
  }
};
inline ProviderStatus GetProviderStatus() {
  ProviderStatus result;
  result.ignored_mappings = internal::g_ignored_mappings.load();
  result.failed = internal::g_registry_failed.load();
  if (!TryAcquireSRWLockShared(&internal::g_lock)) {
    result.busy = true;
    return result;
  }
  for (const auto& provider : internal::g_providers) {
    // Prepared images are retained until process exit. Losing one, its gate or
    // an unfinished transaction is a real failure, not an expired rejection.
    const auto* profile = architecture::ActiveProfile();
    bool valid = profile && provider.architecture == profile->architecture &&
                 provider.ready && !provider.patches.empty() &&
                 internal::IsCurrent(provider.module, provider.identity);
    if (valid) {
      const auto& gate = provider.patches.back();
      valid = internal::IsReadable(gate.address, 1, provider.module) && *gate.address == gate.after;
    }
    if (valid) {
      ++result.ready;
    } else {
      ++result.blocked;
    }
  }
  for (const auto& rejected : internal::g_rejected) {
    // An image reused at the same address is unqualified, not automatically
    // harmless. Only absence of an executable image expires this veto.
    if (internal::IsImageMapping(rejected.module)) {
      ++result.blocked;
    } else {
      ++result.expired_rejections;
    }
  }
  ReleaseSRWLockShared(&internal::g_lock);
  return result;
}
inline unsigned int PreparedProviderCount() {
  return GetProviderStatus().QualifiedCount();
}

// Called through the project's existing provider discovery/maintenance path.
// No CUDA, NVAPI, module enumeration, downloads or compilation here.
inline bool PrepareProviderImpl(HMODULE module) {
  const auto* profile = architecture::ActiveProfile();
  if (!g_enabled.load() || !profile || !profile->NeedsRetarget() ||
      internal::g_registry_failed.load() || module == nullptr) return false;
  if (!internal::IsImageMapping(module)) {
    internal::g_ignored_mappings.fetch_add(1);
    return false;
  }
  if (!TryAcquireSRWLockExclusive(&internal::g_lock)) return false;
  struct Unlock {
    ~Unlock() { ReleaseSRWLockExclusive(&internal::g_lock); }
  } unlock;
  for (const auto& provider : internal::g_providers) {
    if (provider.module != module) continue;
    return provider.architecture == profile->architecture &&
           provider.ready && !provider.patches.empty() &&
           internal::IsCurrent(module, provider.identity) &&
           internal::IsReadable(provider.patches.back().address, 1, module) &&
           *provider.patches.back().address == provider.patches.back().after;
  }

  internal::Image image;
  internal::ImageIdentity identity;
  if (!internal::InspectImage(module, image) || !internal::ReadIdentity(module, identity)) {
    internal::g_registry_failed.store(true);
    Log("invalid provider image: " + internal::ModulePath(module), true);
    return false;
  }
  const auto path = internal::ModulePath(module);
  auto reject = [&](const std::string& reason) {
    auto found = std::find_if(internal::g_rejected.begin(), internal::g_rejected.end(),
        [module](const auto& entry) { return entry.module == module; });
    const bool changed = found == internal::g_rejected.end() ||
        found->identity != identity || found->reason != reason;
    internal::Rejection entry{module, identity, path, reason};
    if (found != internal::g_rejected.end()) {
      *found = std::move(entry);
    } else if (internal::g_rejected.size() < 32) {
      internal::g_rejected.push_back(std::move(entry));
    } else {
      internal::g_registry_failed.store(true);
    }
    if (changed) {
      const bool ready = std::any_of(internal::g_providers.begin(), internal::g_providers.end(),
          [](const auto& provider) { return provider.ready; });
      if (!ready) Log("provider rejected: " + reason + " (" + path + ")", true);
    }
    return false;
  };
  const auto arch_export = GetProcAddress(module, "NVSDK_NGX_GetGPUArchitecture");
  const auto parameters_export = GetProcAddress(module, "NVSDK_NGX_D3D12_PopulateDeviceParameters_Impl");
  if (!arch_export && !parameters_export) {
    internal::g_ignored_mappings.fetch_add(1);
    return false;  // A matching filename alone is not an NGX provider.
  }
  if (!internal::OwnCodeExport(image, module, "NVSDK_NGX_GetGPUArchitecture") ||
      !internal::OwnCodeExport(image, module, "NVSDK_NGX_D3D12_PopulateDeviceParameters_Impl"))
    return reject("incomplete or forwarded provider ABI");

  // Pre-write failures can be retried at the next normal maintenance boundary.
  // They are cleared ONLY after this same image passes the complete transaction.
  if (g_create_seen.load()) return reject("provider loaded after FG Create");
  if (internal::g_providers.size() >= 8) return reject("provider limit reached");
  auto* gate = reinterpret_cast<unsigned char*>(GetProcAddress(module, "NVSDK_NGX_GetGPUArchitecture"));
  const unsigned char expected_gate[] = {0xB8, 0x90, 0x01, 0x00, 0x00, 0xC3};
  if (!gate || !internal::IsReadable(gate, sizeof(expected_gate), module) ||
      std::memcmp(gate, expected_gate, sizeof(expected_gate)) != 0) {
    return reject("unrecognized minimum-architecture export");
  }
  bool executable_gate = false;
  for (const auto& section : image.sections) {
    const auto begin = reinterpret_cast<uintptr_t>(image.base + section.VirtualAddress);
    const auto address = reinterpret_cast<uintptr_t>(gate);
    if ((section.Characteristics & IMAGE_SCN_MEM_EXECUTE) && address >= begin &&
        address - begin <= section.Misc.VirtualSize &&
        sizeof(expected_gate) <= section.Misc.VirtualSize - (address - begin)) {
      executable_gate = true;
    }
  }
  if (!executable_gate) return reject("architecture export is not in code");

  struct Block {
    unsigned char* address;
    std::vector<unsigned char> before;
    std::vector<unsigned char> after;
  };
  std::vector<Block> blocks;
  size_t total_bytes = 0;
  size_t hidden_cubins = 0;
  for (const auto& section : image.sections) {
    if (!(section.Characteristics & IMAGE_SCN_MEM_READ) ||
        (section.Characteristics & IMAGE_SCN_MEM_EXECUTE)) {
      continue;
    }
    auto* start = image.base + section.VirtualAddress;
    const size_t size = section.Misc.VirtualSize;
    if (!internal::IsReadable(start, size, module)) return reject("unreadable data section");
    for (size_t at = 0; at + fatbin::kHeaderBytes <= size;) {
      if (fatbin::ReadU32(start + at) != fatbin::kMagic) {
        ++at;
        continue;
      }
      const uint64_t payload = fatbin::ReadU64(start + at + 8);
      if (payload > size - at - fatbin::kHeaderBytes ||
          payload > fatbin::kMaxFatbinBytes - fatbin::kHeaderBytes)
        return reject("fatbin outside section");
      const size_t bytes = static_cast<size_t>(payload) + fatbin::kHeaderBytes;
      ptx::Plan plan;
      std::string reason;
      const auto result = ptx::Retarget({start + at, bytes}, plan, reason, *profile);
      if (result == ptx::Result::kRejected) return reject(reason);
      if (result == ptx::Result::kRetargeted) {
        if (blocks.size() == 512 || bytes > 64 * 1024 * 1024 - total_bytes) {
          return reject("provider exceeds preparation budget");
        }
        blocks.push_back({start + at, {start + at, start + at + bytes}, std::move(plan.replacement)});
        total_bytes += bytes;
        hidden_cubins += plan.hidden_cubins;
      }
      at += bytes;
    }
  }
  if (blocks.empty()) return reject("no supported sm_89 fatbins");
  internal::Provider candidate{module, {}, false, 0, 0, identity, path, {}, profile->architecture};
  for (const auto& block : blocks) {
    if (std::memcmp(block.address, block.before.data(), block.before.size()) != 0) {
      return reject("image changed during preparation");
    }
    // Payload/header updates precede the minimum-architecture export below.
    for (size_t i = block.before.size(); i-- > 0;) {
      if (block.before[i] != block.after[i]) {
        candidate.patches.push_back(
            {block.address + i, block.before[i], block.after[i]});
      }
    }
  }
  candidate.patches.push_back({gate + 1, 0x90, static_cast<unsigned char>(profile->native_arch)});
  // Reserve all rollback storage before the first write. Keep the mapped image
  // alive while rollback pointers into it are stored. This process-lifetime
  // provider reference does not retain the addon and keeps unload restoration
  // deterministic without racing a provider unmap.
  HMODULE held = nullptr;
  if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
        reinterpret_cast<LPCWSTR>(module), &held) || held != module) return reject("cannot retain provider");
  internal::g_providers.push_back(std::move(candidate));
  auto& saved = internal::g_providers.back();
  bool ok = true;
  for (size_t i = 0; i + 1 < saved.patches.size(); ++i) {
    if (!internal::WriteByte(saved.patches[i], false)) {
      ok = false;
      break;
    }
  }
  for (const auto& block : blocks) {
    if (std::memcmp(block.address, block.after.data(), block.after.size()) != 0) ok = false;
  }
  if (ok) ok = internal::WriteByte(saved.patches.back(), false);
  if (!ok) {
    const bool restored = internal::Undo(saved);
    saved.failure = restored ? "provider preparation rolled back" : "provider rollback failed";
    if (!restored) internal::g_registry_failed.store(true);
    Log(restored ? "provider preparation rolled back" : "provider rollback failed", true);
    return false;
  }
  saved.fatbins = static_cast<unsigned>(blocks.size());
  saved.hidden_cubins = static_cast<unsigned>(hidden_cubins);
  saved.ready = true;
  std::erase_if(internal::g_rejected, [module](const auto& entry) { return entry.module == module; });
  std::stringstream message;
  message << "provider prepared: sm_89 -> sm_" << profile->target_sm << ", "
          << blocks.size() << " fatbins, "
          << hidden_cubins << " cubins hidden";
  Log(message.str());
  return true;
}

inline bool PrepareProvider(HMODULE module) {
  try {
    return PrepareProviderImpl(module);
  } catch (...) {
    internal::g_registry_failed.store(true);
    reshade::log::message(reshade::log::level::error,
        "mfgunlock: provider preparation failed");
    return false;
  }
}

// Reuse the upstream's comparison scanner, but keep comparison writes all-or-none.
// On success the caller records the sites in the upstream gate restoration list.
inline bool ApplyMfgComparisons(std::span<unsigned char* const> sites) {
  const auto* profile = architecture::ActiveProfile();
  if (!g_enabled.load() || !profile) return false;
  std::vector<internal::BytePatch> changes;
  changes.reserve(sites.size());
  for (auto* site : sites) {
    if (*site != 0xB0) return false;
    changes.push_back({site, 0xB0, static_cast<unsigned char>(profile->native_arch)});
  }
  size_t attempted = 0;
  for (auto& patch : changes) {
    ++attempted;
    if (internal::WriteByte(patch, false)) continue;
    bool restored = true;
    while (attempted != 0) {
      auto& undo = changes[--attempted];
      if (undo.original_protection != 0 && !internal::WriteByte(undo, true)) restored = false;
    }
    Log(restored ? "MFG gate update rolled back" : "MFG gate rollback failed", true);
    return false;
  }
  return true;
}

inline void Restore() {
  AcquireSRWLockExclusive(&internal::g_lock);
  for (auto it = internal::g_providers.rbegin(); it != internal::g_providers.rend(); ++it) {
    if (!internal::Undo(*it)) Log("provider restore failed", true);
  }
  internal::g_providers.clear();
  internal::g_rejected.clear();
  internal::g_registry_failed.store(false);
  ReleaseSRWLockExclusive(&internal::g_lock);
}
}  // namespace mfgunlock::ampere
