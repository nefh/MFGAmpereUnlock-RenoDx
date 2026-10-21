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

#include "./reshade_compat.hpp"

#include "./ptx_retarget.hpp"
#include "./cubin_trial.hpp"
#include "./endpoint_backend.hpp"
#include "./fatbin_storage.hpp"
#include "./mfg_gate.hpp"
#include "./endpoint_flat.hpp"

namespace mfgunlock::provider {

inline std::atomic_bool g_create_seen{false};

// Optional passive startup observer. It cannot replace a result or call vendor code.
using StartupLogObserver = void (*)(const char*, bool) noexcept;
inline std::atomic<StartupLogObserver> g_startup_log_observer{nullptr};
inline void Log(const std::string& message, bool warning = false) {
  if (const auto observer = g_startup_log_observer.load(std::memory_order_relaxed))
    observer(message.c_str(), warning);
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

// Provider-owned overflow storage is retained with the byte undo log. No
// replacement is released until all published pointer/loadsite bytes restore.
struct PayloadStorage {
  unsigned char* original = nullptr;
  size_t original_bytes = 0;
  void* allocation = nullptr;
  size_t allocation_bytes = 0;
  std::vector<storage::LoadSite> sites;

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
  profiles::Qualification qualification = profiles::Qualification::kUnknown;
  uint32_t profile_index = 0;
  bool exact_inventory = false;
  bool endpoint_selected = false;
  std::vector<endpoint::Selector> endpoint_selectors{};
  std::vector<endpoint::VtableBinding> endpoint_loaders{};
  size_t endpoint_payloads = 0;
  std::vector<PayloadStorage> overflow_storage{};
  endpoint::Family endpoint_family=endpoint::Family::kUnknown;
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
inline std::atomic_bool g_registry_failed{false};
inline std::atomic_bool g_comparison_restore_failed{false};

inline bool IsReadable(const void* pointer, size_t bytes, const void* allocation) {
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

inline bool EndpointSelectorsApplied(const Provider& provider) {
  if (!provider.endpoint_selected) return true;
  const auto* base = reinterpret_cast<const unsigned char*>(provider.module);
  if (provider.endpoint_selectors.size() != 2 || provider.endpoint_loaders.size() != 2 ||
      provider.endpoint_payloads == 0) return false;
  for (const auto& site : provider.endpoint_selectors) {
    if (!IsReadable(base + site.rva, 2, provider.module) ||
        base[site.rva] != site.after[0] || base[site.rva + 1] != site.after[1]) return false;
  }
  for (const auto& binding : provider.endpoint_loaders) {
    if (!IsReadable(base + binding.rva, 8, provider.module) ||
        fatbin::ReadU64(base + binding.rva) !=
            reinterpret_cast<uintptr_t>(base) + binding.target_rva) return false;
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
  std::string path(32768, '\0');
  const auto length = GetModuleFileNameA(module, path.data(), static_cast<DWORD>(path.size()));
  if (length == 0 || length >= path.size()) return "<path unavailable>";
  path.resize(length);
  return path;
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

// This identifies the provider; it does not alias or call the two distinct
// Populate* ABIs. In particular the legacy function has no device argument.
inline bool HasParameterExports(const Image& image, HMODULE module) {
  return OwnCodeExport(image, module, "NVSDK_NGX_D3D12_PopulateDeviceParameters_Impl") ||
         OwnCodeExport(image, module, "NVSDK_NGX_VULKAN_PopulateDeviceParameters_Impl") ||
         OwnCodeExport(image, module, "NVSDK_NGX_D3D12_PopulateParameters_Impl") ||
         OwnCodeExport(image, module, "NVSDK_NGX_VULKAN_PopulateParameters_Impl");
}

inline bool NamedLegacyDlssg(const Image& image, HMODULE module) {
  // A loader basename alone is insufficient. Older DLSS-G providers predate
  // GetFeatureRequirements, so identify the contract by the internal PE export
  // name plus the complete common D3D12 lifecycle that exists across both the
  // pre-requirements and later legacy families. Version strings/hashes are not
  // part of this decision and the PopulateParameters ABI is never called here.
  constexpr const char* exports[] = {
      "NVSDK_NGX_GetSnippetVersion", "NVSDK_NGX_D3D12_Init",
      "NVSDK_NGX_D3D12_CreateFeature", "NVSDK_NGX_D3D12_EvaluateFeature",
      "NVSDK_NGX_D3D12_GetScratchBufferSize", "NVSDK_NGX_D3D12_PopulateParameters_Impl",
      "NVSDK_NGX_D3D12_ReleaseFeature", "NVSDK_NGX_D3D12_Shutdown"};
  for (const auto* name : exports) if (!OwnCodeExport(image, module, name)) return false;
  IMAGE_DOS_HEADER dos{};
  std::memcpy(&dos, image.base, sizeof(dos));
  IMAGE_NT_HEADERS64 nt{};
  std::memcpy(&nt, image.base + dos.e_lfanew, sizeof(nt));
  if (nt.OptionalHeader.NumberOfRvaAndSizes <= IMAGE_DIRECTORY_ENTRY_EXPORT) return false;
  const auto dir = nt.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
  if (dir.Size < sizeof(IMAGE_EXPORT_DIRECTORY) || dir.VirtualAddress > image.bytes ||
      dir.Size > image.bytes - dir.VirtualAddress ||
      !IsReadable(image.base + dir.VirtualAddress, sizeof(IMAGE_EXPORT_DIRECTORY), module)) return false;
  IMAGE_EXPORT_DIRECTORY exports_header{};
  std::memcpy(&exports_header, image.base + dir.VirtualAddress, sizeof(exports_header));
  constexpr char expected[] = "nvngx_dlssg.dll";
  if (exports_header.Name > image.bytes || sizeof(expected) > image.bytes - exports_header.Name ||
      !IsReadable(image.base + exports_header.Name, sizeof(expected), module)) return false;
  for (size_t i = 0; i < sizeof(expected); ++i) {
    auto c = image.base[exports_header.Name + i];
    if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
    if (c != static_cast<unsigned char>(expected[i])) return false;
  }
  return true;
}

inline bool IsDlssgImage(const Image& image, HMODULE module) {
  if (!OwnCodeExport(image, module, "NVSDK_NGX_GetGPUArchitecture") ||
      !HasParameterExports(image, module)) {
    return false;
  }
  if (NamedLegacyDlssg(image, module)) return true;
  constexpr const char* kMarkers[] = {
      "dlfg_kernel", "Kernel_EstimateIntermMvecsScatter", "Kernel_OutputPull"};
  bool seen[3]{};
  for (const auto& section : image.sections) {
    if (!(section.Characteristics & IMAGE_SCN_MEM_READ)) continue;
    const auto* begin = image.base + section.VirtualAddress;
    const size_t bytes = section.Misc.VirtualSize;
    if (!IsReadable(begin, bytes, module)) continue;
    for (size_t index = 0; index < 3; ++index) {
      const auto length = std::strlen(kMarkers[index]);
      if (bytes >= length &&
          std::search(begin, begin + bytes, kMarkers[index], kMarkers[index] + length) != begin + bytes)
        seen[index] = true;
    }
  }
  // The 310.9 family uses renamed kernel symbols. Neither a filename nor one
  // generic interpolation symbol alone identifies a modern DLSS-G provider.
  return seen[0] || (seen[1] && seen[2]);
}

// Cheap readiness validation: only the indirection cell and argument edits,
// never a rescan/recompression/hash of the replacement program per Present.
inline bool StoragePublished(const Provider& p) {
  for(const auto& r:p.overflow_storage) {
    if(!r.allocation || !r.allocation_bytes || !IsReadable(r.original,8,p.module) ||
        !IsReadable(r.allocation,r.allocation_bytes,r.allocation) ||
        fatbin::ReadU64(r.original)!=reinterpret_cast<uintptr_t>(r.allocation))return false;
    const auto* base=reinterpret_cast<const unsigned char*>(p.module);
    for(const auto& site:r.sites) {
      if(!IsReadable(base+site.lea_rva,7,p.module) || !IsReadable(base+site.length_rva,4,p.module) ||
          base[site.lea_rva]!=0x48 || base[site.lea_rva+1]!=0x8b || base[site.lea_rva+2]!=0x15 ||
          fatbin::ReadU32(base+site.length_rva)!=r.allocation_bytes)return false;
      int32_t delta=0;std::memcpy(&delta,base+site.lea_rva+3,4);
      if(base+site.lea_rva+7+delta!=r.original)return false;
    }
  }
  return true;
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
  // Same ownership rule as existing descriptor redirects: never free storage
  // while any original alias/loadsite might still point into it.
  if(ok) {
    for(auto& storage:provider.overflow_storage) {
      if(storage.allocation && !VirtualFree(storage.allocation,0,MEM_RELEASE)) {ok=false;continue;}
      storage.allocation=nullptr;
    }
  }
  provider.ready = false;
  return ok;
}
}  // namespace internal

// Optional backend selection is discovered in the actual mapped image. All
// reads used by the structural matcher are confined to verified readable
// sections; discovery never calls provider code or writes to the image.
inline bool FindTuringEndpointPlan(HMODULE module, endpoint::StructuralPlan& plan,
                                  std::string& reason) {
  plan = {};
  internal::Image image;
  if (!internal::InspectImage(module, image)) { reason = "endpoint: invalid image"; return false; }
  std::vector<endpoint::ImageRange> ranges;
  for (const auto& s : image.sections) {
    if (!s.Misc.VirtualSize) continue;
    if (!internal::IsReadable(image.base + s.VirtualAddress, s.Misc.VirtualSize, module)) {
      // Discardable sections (for example .reloc) need not remain readable
      // after the native loader finishes. Never expose them to the scanner.
      continue;
    }
    ranges.push_back({s.VirtualAddress,s.Misc.VirtualSize,(s.Characteristics & IMAGE_SCN_MEM_EXECUTE)!=0});
  }
  return endpoint::FindEndpointPlan({image.base,image.bytes},ranges,plan,reason);
}

inline bool IsDlssgProvider(HMODULE module) {
  internal::Image image;
  return internal::IsImageMapping(module) && internal::InspectImage(module, image) &&
         internal::IsDlssgImage(image, module);
}

inline bool IsImageMapping(HMODULE module) {
  return internal::IsImageMapping(module);
}

struct Observation {
  bool mapped = false;
  bool dlssg = false;
  bool profile_known = false;
  bool exact_inventory = false;
  bool prepared = false;
  internal::ImageIdentity identity{};
  const profiles::ProviderProfile* profile = nullptr;
  uint32_t profile_index = 0;
  profiles::Qualification qualification = profiles::Qualification::kUnknown;
};

inline Observation InspectProvider(HMODULE module) {
  Observation out;
  out.mapped = internal::IsImageMapping(module);
  if (!out.mapped) return out;

  internal::Image image;
  if (!internal::ReadIdentity(module, out.identity) || !internal::InspectImage(module, image))
    return out;
  out.dlssg = internal::IsDlssgImage(image, module);
  if (!out.dlssg) return out;

  out.profile = profiles::Find(out.identity.timestamp, out.identity.image_bytes);
  out.profile_index = profiles::IndexOf(out.profile);
  out.profile_known = out.profile != nullptr;

  if (TryAcquireSRWLockShared(&internal::g_lock)) {
    for (const auto& provider : internal::g_providers) {
      if (provider.module != module || !internal::IsCurrent(module, provider.identity)) continue;
      out.profile_index = provider.profile_index;
      if (out.profile_index != 0 && out.profile_index <= profiles::kProfiles.size())
        out.profile = &profiles::kProfiles[out.profile_index - 1];
      out.profile_known = out.profile != nullptr || provider.exact_inventory;
      out.prepared = provider.ready && provider.qualification == profiles::Qualification::kPatchable;
      out.exact_inventory = provider.exact_inventory;
      out.qualification = provider.qualification;
      ReleaseSRWLockShared(&internal::g_lock);
      return out;
    }
    ReleaseSRWLockShared(&internal::g_lock);
  }

  if (!out.profile) return out;
  bool readable = true;
  for (const auto& region : out.profile->retarget_payloads) {
    if (region.rva > image.bytes || region.bytes > image.bytes - region.rva ||
        !internal::IsReadable(image.base + region.rva, region.bytes, module)) {
      readable = false;
      break;
    }
  }
  out.exact_inventory = readable &&
      profiles::MatchesRegions({image.base, image.bytes}, out.profile->retarget_payloads);
  out.qualification = profiles::Classify(out.profile_known, out.exact_inventory, false);
  return out;
}

// Reference-inventory evidence only; not a prerequisite for runtime patch trials.
// Descriptor redirection keeps these original containers intact. Individual
// patch implementations validate their own actual structures independently.
inline bool QualifiedNativeInventory(HMODULE module) {
  internal::Image image;
  internal::ImageIdentity identity;
  if (!internal::ReadIdentity(module, identity) || !internal::InspectImage(module, image) ||
      !internal::IsDlssgImage(image, module)) return false;
  const auto* profile = profiles::Find(identity.timestamp, identity.image_bytes);
  if (!profile) return false;
  for (const auto& region : profile->retarget_payloads) {
    if (region.rva > image.bytes || region.bytes > image.bytes - region.rva ||
        !internal::IsReadable(image.base + region.rva, region.bytes, module)) return false;
  }
  return profiles::MatchesRegions({image.base, image.bytes}, profile->retarget_payloads);
}

// Host MFG qualification is independent from provider/PTX identity. Keep a
// snapshot of the actual semantic witnesses until commit; never cache raw RVAs
// as authorization for another mapping.
struct MfgGatePlan {
  HMODULE module = nullptr;
  internal::ImageIdentity identity{};
  mfggate::Plan contract;
};
inline bool FindMfgGatePlan(HMODULE module, MfgGatePlan& plan) {
  plan = {};
  plan.module = module;
  internal::Image image;
  if (!internal::InspectImage(module, image) ||
      !internal::ReadIdentity(module, plan.identity)) {
    plan.contract.reason = "INVALID_IMAGE_MAPPING";
    return false;
  }
  const auto nt = fatbin::ReadU32(image.base + 0x3c);
  const auto headers = fatbin::ReadU32(image.base + nt + 24 + 60);
  if (!internal::IsReadable(image.base, headers, module)) {
    plan.contract.reason = "UNREADABLE_IMAGE_HEADERS";
    return false;
  }
  std::vector<mfggate::Range> ranges;
  for (const auto& section : image.sections) {
    if (!section.Misc.VirtualSize) continue;
    if (!internal::IsReadable(image.base + section.VirtualAddress,
                              section.Misc.VirtualSize, module)) {
      // Match the existing endpoint reader: discarded sections such as .reloc
      // must not veto an otherwise readable host contract. The view only exposes
      // readable ranges, and every semantic table/reference is range-checked.
      continue;
    }
    ranges.push_back({section.VirtualAddress, section.Misc.VirtualSize,
                     (section.Characteristics & IMAGE_SCN_MEM_EXECUTE) != 0});
  }
  return mfggate::FindPlan({image.base, image.bytes}, ranges, plan.contract);
}
inline bool ValidateMfgGatePlan(const MfgGatePlan& plan) {
  if (!internal::IsCurrent(plan.module, plan.identity)) return false;
  const auto* base = reinterpret_cast<const unsigned char*>(plan.module);
  for (const auto& witness : plan.contract.witnesses) {
    if (witness.rva > plan.identity.image_bytes ||
        witness.bytes.size() > plan.identity.image_bytes - witness.rva ||
        !internal::IsReadable(base + witness.rva, witness.bytes.size(), plan.module)) return false;
  }
  return mfggate::ValidatePlan({base, plan.identity.image_bytes}, plan.contract);
}
// Compatibility entry point for existing offline callers; it now uses the same
// structural discovery as the runtime. No fingerprint-table fallback exists.
inline bool FindQualifiedMfgGates(HMODULE module, std::vector<unsigned char*>& output) {
  output.clear();
  MfgGatePlan plan;
  if (!FindMfgGatePlan(module, plan)) return false;
  auto* base = reinterpret_cast<unsigned char*>(module);
  for (const auto& site : plan.contract.sites) output.push_back(base + site.immediate);
  return true;
}

struct ProviderStatus {
  unsigned ready = 0;
  unsigned blocked = 0;
  bool busy = false;
  bool failed = false;
  unsigned int QualifiedCount() const {
    // A separate inspected/rejected provider cannot revoke successful work on
    // a current retained provider. Real registry/rollback failures still can.
    return busy || failed ? 0 : ready;
  }
};
inline ProviderStatus GetProviderStatus() {
  ProviderStatus result;
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
                 provider.ready && provider.qualification == profiles::Qualification::kPatchable &&
                 !provider.patches.empty() &&
                 internal::IsCurrent(provider.module, provider.identity);
    if (valid) {
      const auto& gate = provider.patches.back();
      valid = internal::IsReadable(gate.address, 1, provider.module) && *gate.address == gate.after &&
              internal::EndpointSelectorsApplied(provider) && internal::StoragePublished(provider);
    }
    if (valid) {
      ++result.ready;
    } else {
      ++result.blocked;
    }
  }
  for (const auto& rejected : internal::g_rejected) {
    // Keep a current diagnostic count without turning unrelated discovery
    // results into a veto for separately committed providers.
    if (internal::IsImageMapping(rejected.module)) {
      ++result.blocked;
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
inline bool PrepareProviderImpl(HMODULE module,
    std::span<const profiles::ProviderProfile> known_profiles = profiles::kProfiles) {
  const auto* profile = architecture::ActiveProfile();
  if (!g_enabled.load() || !profile || !profile->RequiresProviderRetarget() ||
      internal::g_registry_failed.load() || module == nullptr) return false;
  if (!internal::IsImageMapping(module)) return false;
  if (!TryAcquireSRWLockExclusive(&internal::g_lock)) return false;
  struct Unlock {
    ~Unlock() { ReleaseSRWLockExclusive(&internal::g_lock); }
  } unlock;
  for (const auto& provider : internal::g_providers) {
    if (provider.module != module) continue;
    return provider.architecture == profile->architecture &&
           provider.ready && provider.qualification == profiles::Qualification::kPatchable &&
                 !provider.patches.empty() &&
           internal::IsCurrent(module, provider.identity) &&
           internal::IsReadable(provider.patches.back().address, 1, module) &&
           *provider.patches.back().address == provider.patches.back().after &&
           internal::EndpointSelectorsApplied(provider) && internal::StoragePublished(provider);
  }

  internal::Image image;
  internal::ImageIdentity identity;
  if (!internal::InspectImage(module, image) || !internal::ReadIdentity(module, identity)) {
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
      // Bounded diagnostic history is not a compatibility admission gate.
      internal::g_rejected.erase(internal::g_rejected.begin());
      internal::g_rejected.push_back(std::move(entry));
    }
    if (changed) Log("provider preparation result: " + reason + " (" + path + ")", true);
    return false;
  };
  const auto arch_export = GetProcAddress(module, "NVSDK_NGX_GetGPUArchitecture");
  const bool parameter_exports = internal::HasParameterExports(image, module);
  if (!arch_export && !parameter_exports)
    return false;  // A matching filename alone is not an NGX provider.
  if (!internal::OwnCodeExport(image, module, "NVSDK_NGX_GetGPUArchitecture") ||
      !parameter_exports)
    return reject("incomplete or forwarded provider ABI");

  if (!internal::IsDlssgImage(image, module)) return reject("not a DLSS-G provider");

  // Pre-write failures can be retried at the next normal maintenance boundary.
  // In-place payload mutation needs quiescent consumers: after Create, CUDA
  // may retain pointers into the containers. No per-module quiescence contract
  // is available here, so live rewrites remain deferred until a fresh process.
  if (g_create_seen.load()) return reject("provider loaded after FG Create");
  auto* gate = reinterpret_cast<unsigned char*>(GetProcAddress(module, "NVSDK_NGX_GetGPUArchitecture"));
  constexpr size_t kArchitectureGateBytes = 6;
  uint32_t minimum_arch = 0;
  if (!gate || !internal::IsReadable(gate, kArchitectureGateBytes, module) ||
      gate[0] != 0xB8 || gate[5] != 0xC3) {
    return reject("unrecognized minimum-architecture export");
  }
  std::memcpy(&minimum_arch, gate + 1, sizeof(minimum_arch));
  // The export's decoded constant is the actual minimum. Its numeric value is
  // not a version/architecture allowlist: a newer value still receives the
  // same bounded transaction and is lowered only to the detected native ID.
  bool executable_gate = false;
  for (const auto& section : image.sections) {
    const auto begin = reinterpret_cast<uintptr_t>(image.base + section.VirtualAddress);
    const auto address = reinterpret_cast<uintptr_t>(gate);
    if ((section.Characteristics & IMAGE_SCN_MEM_EXECUTE) && address >= begin &&
        address - begin <= section.Misc.VirtualSize &&
        kArchitectureGateBytes <= section.Misc.VirtualSize - (address - begin)) {
      executable_gate = true;
    }
  }
  if (!executable_gate) return reject("architecture export is not in code");

  // A registry hit is diagnostic reference evidence, never authorization or
  // a prerequisite for trying the generic PTX transaction. Inspect every actual
  // container below even when metadata, hashes, RVAs or inventory counts differ.
  const auto* qualified = profiles::Find(identity.timestamp, identity.image_bytes, known_profiles);
  bool exact_inventory = qualified != nullptr;
  if (qualified) {
    for (const auto& region : qualified->retarget_payloads) {
      if (region.rva > image.bytes || region.bytes > image.bytes - region.rva ||
          !internal::IsReadable(image.base + region.rva, region.bytes, module)) {
        exact_inventory = false;
        break;
      }
    }
    exact_inventory = exact_inventory &&
        profiles::MatchesRegions({image.base, image.bytes}, qualified->retarget_payloads);
  }

  // Discover the existing DL1/DL2 PTX factory by its actual branch, RTTI,
  // vtable and loader call graph. A metadata/fingerprint miss does not bypass
  // selection when the same structural contract exists at different RVAs.
  endpoint::StructuralPlan endpoint_plan;
  std::string endpoint_reason;
  const bool turing_endpoint = profile->architecture == Architecture::kTuring &&
      FindTuringEndpointPlan(module, endpoint_plan, endpoint_reason);

  struct Block {
    unsigned char* address;
    std::vector<unsigned char> before;
    std::vector<unsigned char> after;
    bool external = false;
    std::vector<storage::LoadSite> loadsites;
  };
  std::vector<Block> blocks;
  size_t total_bytes = 0;
  size_t hidden_cubins = 0;
  std::vector<std::pair<uint32_t, size_t>> native_cubin_trials;
  size_t native_containers = 0;
  std::vector<endpoint::ImageRange> ranges;
  for(const auto& sec:image.sections) {
    if(sec.Misc.VirtualSize && internal::IsReadable(image.base+sec.VirtualAddress,sec.Misc.VirtualSize,module))
      ranges.push_back({sec.VirtualAddress,sec.Misc.VirtualSize,(sec.Characteristics&IMAGE_SCN_MEM_EXECUTE)!=0});
  }
  endpoint::structural::ImageView storage_image{{image.base,image.bytes},ranges,{}};

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
      auto result = ptx::Retarget({start + at, bytes}, plan, reason, *profile);
      if (result == ptx::Result::kRejected && (profile->target_sm == 75 || profile->target_sm == 86)) {
        std::string native_reason;
        uint32_t source_sm = 0;
        if (cubin::RetargetNativeToTarget({start + at, bytes}, plan.replacement, native_reason,
                                          profile->target_sm, &source_sm)) {
          result = ptx::Result::kRetargeted;
          plan.source_sm = source_sm;
          plan.target_sm = profile->target_sm;
          const auto trial = std::find_if(native_cubin_trials.begin(), native_cubin_trials.end(),
              [source_sm](const auto& item) { return item.first == source_sm; });
          if (trial == native_cubin_trials.end()) native_cubin_trials.emplace_back(source_sm, 1);
          else ++trial->second;
        } else if (!native_reason.empty()) {
          reason += "; " + native_reason;
        }
      }
      if (result == ptx::Result::kRejected) return reject(reason);
      if (result == ptx::Result::kUnchanged) ++native_containers;
      if (result == ptx::Result::kRetargeted) {
        if (blocks.size() == 512 || bytes > 64 * 1024 * 1024 - total_bytes) {
          return reject("provider exceeds preparation budget");
        }
        std::vector<storage::LoadSite> loads;
        if(plan.requires_external_storage) {
          if(plan.replacement.size()>fatbin::kMaxFatbinBytes || plan.replacement.size()<=bytes ||
              (storage_image.functions.empty() && !storage_image.ReadFunctions()) ||
              !storage::FindLoads(storage_image,static_cast<uint32_t>(start+at-image.base),
                                  static_cast<uint32_t>(bytes),loads,reason,plan.source_sm))
            return reject(reason.empty()?"SM75 storage: unproven relocation":reason);
          for(const auto& load:loads)
            if(!endpoint::structural::PtxHasEntry(plan.replacement,profile->target_sm,load.entry_name))
              return reject("SM75 storage: rebuilt entry mismatch");
        } else if(plan.replacement.size()!=bytes) return reject("invalid in-place replacement size");
        if(plan.replacement.size()>64*1024*1024-total_bytes)return reject("provider storage budget");
        blocks.push_back({start + at, {start + at, start + at + bytes}, std::move(plan.replacement),
                          plan.requires_external_storage,std::move(loads)});
        total_bytes += blocks.back().after.size();
        hidden_cubins += plan.hidden_cubins;
      }
      at += bytes;
    }
  }
  if (blocks.empty()) return reject(native_containers
      ? "native/target payload already present; no retarget transaction needed"
      : "no retarget transaction planned; native provider path remains unchanged");
  if (exact_inventory) {
    exact_inventory = blocks.size() == qualified->retarget_payloads.size();
    for (const auto& block : blocks) {
      const auto rva = static_cast<uint32_t>(block.address - image.base);
      const auto found = std::find_if(qualified->retarget_payloads.begin(),
          qualified->retarget_payloads.end(), [rva](const auto& entry) { return entry.rva == rva; });
      if (found == qualified->retarget_payloads.end() || found->bytes != block.before.size())
        exact_inventory = false;
    }
  }
  if (turing_endpoint) {
    for (const auto& payload : endpoint_plan.payloads) {
      const auto found = std::find_if(blocks.begin(), blocks.end(), [&](const Block& block) {
        return block.address == image.base + payload.rva;
      });
      if (found == blocks.end() || found->before.size() != payload.bytes ||
          (!found->external && found->after.size() != payload.bytes) ||
          !endpoint::structural::PtxHasEntry(found->after,75,payload.entry_name))
        return reject("Turing endpoint requires every referenced network payload in SM75");
    }
  }
  internal::Provider candidate{module, {}, false, 0, 0, identity, path, {}, profile->architecture};
  candidate.exact_inventory = exact_inventory;
  candidate.endpoint_selected = turing_endpoint;
  if (turing_endpoint) {
    candidate.endpoint_selectors.assign(endpoint_plan.selectors.begin(),endpoint_plan.selectors.end());
    candidate.endpoint_loaders.assign(endpoint_plan.loaders.begin(),endpoint_plan.loaders.end());
    candidate.endpoint_payloads = endpoint_plan.payloads.size();
    candidate.endpoint_family = endpoint_plan.family;
  }
  struct PendingAllocations {
    std::vector<void*> pointers;
    bool adopted=false;
    ~PendingAllocations() { if(!adopted)for(auto* p:pointers)VirtualFree(p,0,MEM_RELEASE); }
  } pending;
  pending.pointers.reserve(blocks.size());
  candidate.overflow_storage.reserve(blocks.size());
  for (auto& block : blocks) {
    if (std::memcmp(block.address, block.before.data(), block.before.size()) != 0) {
      return reject("image changed during preparation");
    }
    if(block.external) {
      void* memory=VirtualAlloc(nullptr,block.after.size(),MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE);
      if(!memory)return reject("SM75 storage: allocation failed");
      pending.pointers.push_back(memory);
      std::memcpy(memory,block.after.data(),block.after.size());
      DWORD previous=0;
      if(!VirtualProtect(memory,block.after.size(),PAGE_READONLY,&previous) ||
          std::memcmp(memory,block.after.data(),block.after.size())!=0)
        return reject("SM75 storage: copy/protection failed");
      internal::PayloadStorage allocation{block.address,block.before.size(),memory,block.after.size(),std::move(block.loadsites)};
      const uint64_t pointer=reinterpret_cast<uintptr_t>(allocation.allocation);
      for(size_t i=0;i<8;++i) {
        const auto after=static_cast<unsigned char>(pointer>>(8*i));
        if(block.before[i]!=after)candidate.patches.push_back({block.address+i,block.before[i],after});
      }
      for(const auto& site:allocation.sites) {
        candidate.patches.push_back({image.base+site.lea_rva+1,0x8d,0x8b});
        const uint32_t length=static_cast<uint32_t>(allocation.allocation_bytes);
        for(size_t i=0;i<4;++i) {
          const auto before=image.base[site.length_rva+i],after=static_cast<unsigned char>(length>>(8*i));
          if(before!=after)candidate.patches.push_back({image.base+site.length_rva+i,before,after});
        }
      }
      candidate.overflow_storage.push_back(std::move(allocation));
      continue;
    }
    // Payload/header updates precede the minimum-architecture export below.
    for (size_t i = block.before.size(); i-- > 0;) {
      if (block.before[i] != block.after[i]) {
        candidate.patches.push_back(
            {block.address + i, block.before[i], block.after[i]});
      }
    }
  }
  if (turing_endpoint) {
    for(const auto& witness:endpoint_plan.witnesses)
      if(!internal::IsReadable(image.base+witness.rva,witness.bytes.size(),module))
        return reject("Turing endpoint: host witness became unreadable");
    if(!internal::IsCurrent(module,identity)||!endpoint::WitnessesMatch({image.base,image.bytes},endpoint_plan))
      return reject("Turing endpoint: host witnesses changed before commit");
    // Select the existing PTX classes with the plan's branch semantics.
    // Native loader cubins bypass fatbin retargeting. Publish a redirect only
    // with every referenced network payload prepared for SM75.
    for (const auto& site : endpoint_plan.selectors) {
      for (size_t i = 0; i < site.before.size(); ++i)
        candidate.patches.push_back({image.base + site.rva + i, site.before[i], site.after[i]});
    }
  }
  // Lower the provider's minimum architecture only when the requested target
  // is older than the minimum it already reports. A provider that already
  // reports Turing (0x160) needs no architecture-gate write on Ampere/Turing.
  const size_t gate_patch_begin = candidate.patches.size();
  const uint32_t desired_minimum_arch = std::min(minimum_arch, profile->native_arch);
  for (size_t i = 0; i < sizeof(desired_minimum_arch); ++i) {
    const auto before = static_cast<unsigned char>((minimum_arch >> (i * 8)) & 0xff);
    const auto after = static_cast<unsigned char>((desired_minimum_arch >> (i * 8)) & 0xff);
    if (before != after) candidate.patches.push_back({gate + 1 + i, before, after});
  }
  // Reserve all rollback storage before the first write. Keep the mapped image
  // alive while rollback pointers into it are stored. This process-lifetime
  // provider reference does not retain the addon and keeps rollback ownership
  // stable without racing a provider unmap.
  internal::g_providers.reserve(internal::g_providers.size() + 1);
  HMODULE held = nullptr;
  if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
        reinterpret_cast<LPCWSTR>(module), &held) || held != module) return reject("cannot retain provider");
  internal::g_providers.push_back(std::move(candidate));
  pending.adopted=true;
  auto& saved = internal::g_providers.back();
  bool ok = true;
  // Commit payload/selector changes first. The architecture gate is the final
  // publication step when it actually needs lowering; an already-permissive
  // gate has no synthetic/no-op write.
  for (size_t i = 0; i < gate_patch_begin; ++i) {
    if (!internal::WriteByte(saved.patches[i], false)) {
      ok = false;
      break;
    }
  }
  for (const auto& block : blocks) {
    if (!block.external && std::memcmp(block.address, block.after.data(), block.after.size()) != 0) ok = false;
  }
  if (ok) ok = internal::EndpointSelectorsApplied(saved) && internal::StoragePublished(saved);
  for (size_t i = gate_patch_begin; ok && i < saved.patches.size(); ++i) {
    if (!internal::WriteByte(saved.patches[i], false)) ok = false;
  }
  if (!ok) {
    const bool restored = internal::Undo(saved);
    saved.failure = restored ? "provider preparation rolled back" : "provider rollback failed";
    if (!restored) internal::g_registry_failed.store(true);
    Log(restored ? "provider preparation rolled back" : "provider rollback failed", true);
    return false;
  }
  saved.fatbins = static_cast<unsigned>(blocks.size());
  saved.hidden_cubins = static_cast<unsigned>(hidden_cubins);
  saved.qualification = profiles::Qualification::kPatchable;
  saved.profile_index = exact_inventory ? profiles::IndexOf(qualified) : 0;
  saved.ready = true;
  std::erase_if(internal::g_rejected, [module](const auto& entry) { return entry.module == module; });
  std::stringstream message;
  message << "provider prepared: target sm_" << profile->target_sm << ", "
          << blocks.size() << " fatbins, "
          << hidden_cubins << " cubins hidden";
  Log(message.str());
  for (const auto& [source_sm, count] : native_cubin_trials) {
    Log("StockCubinR6: native_code_trial source_sm=" + std::to_string(source_sm) + " target_sm=" +
        std::to_string(profile->target_sm) + " containers=" + std::to_string(count) + " instruction_bytes_changed=0");
  }
  Log(exact_inventory
      ? std::string("ProviderProfile ") + qualified->id + " PATCHABLE (mapped exact inventory; target preparation committed)"
      : "ProviderProfile structural trial PATCHABLE (no registry requirement; target preparation committed; GPU execution not yet observed)");
  if (!saved.overflow_storage.empty())
    Log("SM75StorageT2: retained out-of-line payloads=" + std::to_string(saved.overflow_storage.size()));
  if (turing_endpoint)
    Log(std::string("TuringEndpointSM75: selected=1 family=") + endpoint::FamilyName(endpoint_plan.family) +
        " network0=" + std::to_string(endpoint_plan.network_counts[0]) +
        " network1=" + std::to_string(endpoint_plan.network_counts[1]) +
        " payloads=" + std::to_string(endpoint_plan.payloads.size()));
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

// Exact-qualified comparison sites share one checked transaction on all targets.
// Undo ownership is published before success; failed restore remains retryable.
inline bool ApplyMfgComparisons(std::span<unsigned char* const> sites,
                                std::vector<internal::BytePatch>& ownership) {
  const auto* profile = architecture::ActiveProfile();
  if (!g_enabled.load() || !profile || sites.empty()) return false;
  std::vector<internal::BytePatch> changes;
  changes.reserve(sites.size());
  for (auto* site : sites) {
    if (!site || *site != 0xB0 || std::any_of(changes.begin(), changes.end(),
        [site](const auto& patch) { return patch.address == site; })) return false;
    changes.push_back({site, 0xB0, static_cast<unsigned char>(profile->native_arch)});
  }
  ownership.reserve(ownership.size() + changes.size());
  size_t attempted = 0;
  for (auto& patch : changes) {
    ++attempted;
    if (internal::WriteByte(patch, false)) continue;
    bool restored = true;
    while (attempted != 0) {
      auto& undo = changes[--attempted];
      if (undo.original_protection != 0 && !internal::WriteByte(undo, true)) restored = false;
    }
    if (!restored) {
      for (const auto& change : changes)
        if (change.original_protection != 0) ownership.push_back(change);
      internal::g_comparison_restore_failed.store(true, std::memory_order_release);
      internal::g_registry_failed.store(true, std::memory_order_release);
    }
    Log(restored ? "MFG gate update rolled back" : "MFG gate rollback failed; undo metadata retained", true);
    return false;
  }
  ownership.insert(ownership.end(), changes.begin(), changes.end());
  return true;
}

// Same lifetime policy as prepared providers: a committed host-code patch
// retains its image until process exit. This prevents stale pointers after
// FreeLibrary/reload, including native Ada (which does not retarget PTX).
// The caller serializes maintenance; no live code mutation after FG Create.
inline bool ApplyMfgGatePlan(const MfgGatePlan& plan,
                             std::vector<internal::BytePatch>& ownership) {
  if (g_create_seen.load(std::memory_order_acquire) || !ValidateMfgGatePlan(plan)) return false;
  HMODULE held = nullptr;
  if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
      reinterpret_cast<LPCWSTR>(plan.module), &held) || held != plan.module) return false;
  // Retain the reference on failure too: incomplete rollback may still own
  // addresses. It is deliberately not an advertised live-unload mechanism.
  if (!ValidateMfgGatePlan(plan)) return false;
  auto* base = reinterpret_cast<unsigned char*>(plan.module);
  std::array<unsigned char*, 2> sites{};
  for (size_t i = 0; i < sites.size(); ++i) sites[i] = base + plan.contract.sites[i].immediate;
  return ApplyMfgComparisons(sites, ownership);
}

inline bool RestoreMfgComparisons(std::vector<internal::BytePatch>& ownership) {
  bool complete = true;
  for (auto it = ownership.rbegin(); it != ownership.rend(); ++it) {
    MEMORY_BASIC_INFORMATION memory{};
    if (VirtualQuery(it->address, &memory, sizeof(memory)) &&
        internal::IsReadable(it->address, 1, static_cast<HMODULE>(memory.AllocationBase)) &&
        internal::WriteByte(*it, true)) {
      it->address = nullptr;
    } else {
      complete = false;
    }
  }
  std::erase_if(ownership, [](const auto& patch) { return patch.address == nullptr; });
  internal::g_comparison_restore_failed.store(!complete, std::memory_order_release);
  if (!complete) internal::g_registry_failed.store(true, std::memory_order_release);
  return complete;
}

inline void Restore() {
  AcquireSRWLockExclusive(&internal::g_lock);
  bool complete = true;
  for (auto it = internal::g_providers.rbegin(); it != internal::g_providers.rend(); ++it) {
    if (internal::Undo(*it)) {
      it->patches.clear();
    } else {
      complete = false;
      it->failure = "provider restore failed; rollback metadata retained";
      Log(it->failure, true);
    }
  }
  std::erase_if(internal::g_providers, [](const auto& entry) { return entry.patches.empty(); });
  if (complete) internal::g_rejected.clear();
  internal::g_registry_failed.store(!complete || internal::g_comparison_restore_failed.load());
  ReleaseSRWLockExclusive(&internal::g_lock);
}
}  // namespace mfgunlock::provider
