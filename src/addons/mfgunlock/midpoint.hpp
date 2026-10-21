/*
 * Temporal (midpoint) correction for DLSS-G frame interpolation on Ada and retargeted providers.
 * SPDX-License-Identifier: MIT
 *
 * ---------------------------------------------------------------------------
 * THE PROBLEM
 *
 * With multi-frame generation unlocked, 3x/4x produce the right NUMBER of
 * frames but not the right CONTENT. The interpolation kernel shipped in
 * nvngx_dlssg.dll blends the two source frames using a compiled-in constant of
 * 0.5 -- 104 `mul.ftz.f32 ..., 0f3F000000;` instructions in its PTX. Every
 * generated frame therefore lands at the temporal midpoint, so 4x shows three
 * identical half-way frames instead of frames at 1/4, 2/4, 3/4. The frame
 * counter doubles while the motion does not get smoother.
 *
 * THE FIX
 *
 * Rewrite that kernel's PTX so the blend weight comes from the kernel's own
 * temporal parameter instead of a constant:
 *
 *     t = parameter-field-at-offset-32;
 *     inverse = 1.0 - t;
 *
 * R6 declares fresh registers at the entry and initializes them before the
 * first executable instruction. The established 104-site recipe uses inverse
 * for its first 52 sites and t for its second 52. Compiler register banks,
 * join labels and payload byte counts are not compatibility conditions.
 *
 * The catch: the fatbin also carries a precompiled sm_89 cubin, and the driver
 * would load that in preference to JIT-ing our edited PTX. So the rebuilt
 * fatbin retains every PTX entry and removes precompiled alternatives to force
 * the JIT path. The edited PTX is emitted uncompressed; unrelated flags and
 * header extensions remain unchanged.
 *
 * Credit: the technique -- the midpoint diagnosis, the injected PTX, and the
 * truncation trick -- is from dashdogy's RTX40MFG-Unlock. That project ships no
 * licence, so nothing here is copied from it; this is an independent
 * implementation of the same idea. The earlier implementation reproduced its
 * published output digest offline (19FB3CD5...104B). R6 deliberately emits
 * fresh register names and an entry initialization block, so that historical
 * digest is not a digest claim for the new output.
 * ---------------------------------------------------------------------------
 */

#pragma once

#include <windows.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <sstream>
#include <vector>

#include "./reshade_compat.hpp"
#include "./provider.hpp"
#include "./midpoint_transform.hpp"

namespace mfgunlock::midpoint {

namespace internal {

inline bool PointsToCString(const uint8_t* base, size_t image_size, uint64_t value,
                            const char* expected) {
  const auto start = reinterpret_cast<uintptr_t>(base);
  if (image_size > UINTPTR_MAX - start || value < start || value >= start + image_size) return false;
  const char* s = reinterpret_cast<const char*>(value);
  const size_t len = std::strlen(expected);
  if (len == SIZE_MAX || len + 1 > start + image_size - value ||
      !provider::internal::IsReadable(s, len + 1, base)) return false;
  return std::memcmp(s, expected, len + 1) == 0;
}

inline bool ReadRelativePointer(const uint8_t* base, size_t image_size, const uint8_t* slot,
                                ptrdiff_t displacement, uint64_t& value) {
  const uintptr_t start = reinterpret_cast<uintptr_t>(base);
  if (image_size > UINTPTR_MAX - start) return false;
  const uintptr_t end = start + image_size;
  const uintptr_t address = reinterpret_cast<uintptr_t>(slot);
  uintptr_t field = address;
  if (displacement >= 0) {
    const auto distance = static_cast<uintptr_t>(displacement);
    if (distance > UINTPTR_MAX - address) return false;
    field = address + distance;
  } else {
    const auto distance = static_cast<uintptr_t>(-(displacement + 1)) + 1;
    if (distance > address) return false;
    field = address - distance;
  }
  if (field < start || field > end || sizeof(value) > end - field ||
      !provider::internal::IsReadable(reinterpret_cast<const void*>(field), sizeof(value), base)) return false;
  std::memcpy(&value, reinterpret_cast<const void*>(field), sizeof(value));
  return true;
}

}  // namespace internal

struct Patch {
  uint64_t* slot;
  uint64_t original;
  DWORD protection = 0;
};

inline bool Restore(std::vector<Patch>& patches, void*& allocation) {
  bool restored = true;
  for (auto& patch : patches) {
    DWORD old_protect = 0;
    if (VirtualProtect(patch.slot, sizeof(uint64_t), PAGE_READWRITE, &old_protect) == 0) {
      restored = false;
      continue;
    }
    if (patch.protection == 0) patch.protection = old_protect;
    *patch.slot = patch.original;
    DWORD ignored = 0;
    if (!VirtualProtect(patch.slot, sizeof(uint64_t), patch.protection, &ignored) ||
        *patch.slot != patch.original) restored = false;
  }
  if (restored) patches.clear();
  // Never release replacement memory while any live descriptor may still point
  // at it. A process-lifetime leak is safer than a dangling provider pointer.
  if (allocation != nullptr && restored) {
    restored = VirtualFree(allocation, 0, MEM_RELEASE) != FALSE;
    if (restored) allocation = nullptr;
  }
  return restored;
}


inline bool RedirectDescriptors(const std::vector<uint64_t*>& slots,
                                const std::vector<uint8_t>& rebuilt,
                                std::vector<Patch>& patches, void*& allocation,
                                std::string& detail) {
  if (slots.empty() || rebuilt.empty() || !patches.empty() || allocation) return false;
  patches.reserve(slots.size());
  allocation = VirtualAlloc(nullptr, rebuilt.size(), MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
  if (!allocation) { detail = "allocation failed"; return false; }
  std::memcpy(allocation, rebuilt.data(), rebuilt.size());
  for (uint64_t* slot : slots) {
    DWORD old_protect = 0;
    bool written = VirtualProtect(slot, sizeof(uint64_t), PAGE_READWRITE, &old_protect) != 0;
    if (written) {
      patches.push_back({slot, *slot, old_protect});
      *slot = reinterpret_cast<uint64_t>(allocation);
      DWORD ignored = 0;
      written = VirtualProtect(slot, sizeof(uint64_t), old_protect, &ignored) != 0 &&
                *slot == reinterpret_cast<uint64_t>(allocation);
    }
    if (written) continue;
    detail = Restore(patches, allocation) ? "descriptor redirect rolled back"
                                           : "descriptor redirect rollback incomplete; restart required";
    return false;
  }
  return true;
}

// Replaces the dlfg kernel fatbin in every descriptor that references it. The
// original observation had eight descriptors in separate tables. Actual
// bounded role-bearing references are discovered; eight is not a requirement.
inline bool Apply(HMODULE module, std::vector<Patch>& patches, void*& allocation,
                  std::string& detail) {
  provider::internal::Image image;
  if (!provider::internal::InspectImage(module, image)) {
    detail = "provider PE layout is unreadable or invalid";
    return false;
  }
  auto* base = image.base;
  const size_t image_size = image.bytes;
  const auto start = reinterpret_cast<uintptr_t>(base);

  // Find descriptor slots: a bounded fatbin pointer accompanied by the entry
  // and semantic descriptor-role pointer fields of a known structure layout.
  // The offsets express those field meanings, not a DLL version or payload size.
  std::vector<uint64_t*> slots;
  const uint8_t* fat = nullptr;
  size_t fat_size = 0;
  const internal::TemporalProfile* selected_profile = nullptr;

  for (const auto& section_value : image.sections) {
    const auto* section = &section_value;
    if ((section->Characteristics & IMAGE_SCN_MEM_READ) == 0) continue;
    if ((section->Characteristics & IMAGE_SCN_MEM_EXECUTE) != 0) continue;
    uint8_t* sec = base + section->VirtualAddress;
    const size_t size = section->Misc.VirtualSize;
    if (!provider::internal::IsReadable(sec, size, module)) continue;
    for (size_t off = 0; off + sizeof(uint64_t) <= size; off += sizeof(uint64_t)) {
      uint64_t value = 0;
      std::memcpy(&value, sec + off, sizeof(value));
      if (value < start || image_size < internal::kOuterHeader ||
          value > start + image_size - internal::kOuterHeader) {
        continue;
      }
      const auto* candidate = reinterpret_cast<const uint8_t*>(value);
      if (!provider::internal::IsReadable(candidate, internal::kOuterHeader, module)) continue;
      if (internal::ReadU32(candidate) != internal::kFatbinMagic) continue;

      const internal::TemporalProfile* name_profile = nullptr;
      for (const auto& profile : internal::kTemporalProfiles) {
        uint64_t entry_name = 0;
        uint64_t desc_name = 0;
        if (!internal::ReadRelativePointer(base, image_size, sec + off,
                                           profile.entry_name_offset, entry_name) ||
            !internal::ReadRelativePointer(base, image_size, sec + off,
                                           profile.descriptor_name_offset, desc_name)) {
          continue;
        }
        if (internal::PointsToCString(base, image_size, entry_name, profile.entry_name) &&
            internal::PointsToCString(base, image_size, desc_name, profile.descriptor_name)) {
          name_profile = &profile;
          break;
        }
      }
      if (name_profile == nullptr) continue;

      const uint64_t declared = internal::ReadU64(candidate + 8);
      if (declared > static_cast<uint64_t>((std::numeric_limits<size_t>::max)() -
                                           internal::kOuterHeader)) {
        continue;
      }
      const size_t total = static_cast<size_t>(declared) + internal::kOuterHeader;
      if (total < internal::kOuterHeader || total > fatbin::kMaxFatbinBytes) continue;
      if (total > start + image_size - value ||
          !provider::internal::IsReadable(candidate, total, module)) continue;
      const auto* fat_profile = internal::FindTemporalProfile(candidate, total);
      if (fat_profile == nullptr || fat_profile != name_profile) continue;
      if (fat == nullptr) {
        fat = candidate;
        fat_size = total;
        selected_profile = fat_profile;
      } else if (candidate != fat) {
        // This API owns one rebuild allocation. Keep attempting its resolved
        // payload without coupling that operation to another descriptor table.
        continue;
      }
      slots.push_back(reinterpret_cast<uint64_t*>(sec + off));
    }
  }

  if (fat == nullptr || slots.empty() || selected_profile == nullptr) {
    detail = "temporal descriptor pointer roles and PTX operation were not resolved";
    return false;
  }

  std::vector<uint8_t> rebuilt;
  std::string why;
  if (!internal::BuildTemporalFatbin(fat, fat_size, *selected_profile, rebuilt, why)) {
    detail = why;
    return false;
  }

  if (!RedirectDescriptors(slots, rebuilt, patches, allocation, detail)) return false;
  std::stringstream s;
  s << "redirected " << patches.size() << " " << selected_profile->descriptor_name
    << " descriptor(s) from a " << fat_size << "-byte fatbin to a " << rebuilt.size()
    << "-byte temporal-corrected rebuild";
  detail = s.str();
  return true;
}


}  // namespace mfgunlock::midpoint
