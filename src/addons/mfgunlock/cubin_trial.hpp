/*
 * Bounded native-code trial for actual higher-SM native entries on SM75/86.
 * SPDX-License-Identifier: MIT
 *
 * This is NOT a SASS translator or a claim of backward binary compatibility.
 * Instruction, relocation and constant bytes stay unchanged. Only the CUDA ELF
 * and enclosing entry target metadata changes; actual acceptance/execution is
 * decided by the NVIDIA driver. Plaintext PTX continues through ptx_retarget.
 */
#pragma once

#include <string>
#include "./fatbin.hpp"

namespace mfgunlock::cubin {

// Fixed CUDA ELF64 offsets, read with memcpy helpers rather than unaligned casts.
inline bool InspectNative(std::span<const unsigned char> bytes, std::string& reason,
                          uint32_t& source_sm) {
  source_sm = 0;
  const auto fail = [&](const char* message) { reason = message; return false; };
  const auto fits = [&](uint64_t offset, uint64_t length) {
    return offset <= bytes.size() && length <= bytes.size() - offset;
  };
  if (bytes.size() < 64 || bytes.size() > fatbin::kMaxFatbinBytes)
    return fail("native cubin: ELF bounds");
  const auto* p = bytes.data();
  if (std::memcmp(p, "\177ELF", 4) || p[4] != 2 || p[5] != 1 || p[6] != 1 ||
      p[7] != 0x33 || fatbin::ReadU16(p + 16) != 2 ||
      fatbin::ReadU16(p + 18) != 190 || fatbin::ReadU16(p + 52) != 64)
    return fail("native cubin: ELF contract");
  const uint32_t flags = fatbin::ReadU32(p + 48);
  const uint32_t observed_sm = flags & 0xff;
  if (observed_sm == 0)
    return fail("native cubin: source SM unavailable for this metadata trial");
  // The existing recipe changes two matching SM fields. Different values can
  // encode distinct real/virtual targets; they are not proof of invalid ELF.
  // Their separate meaning is not established by this bounded recipe.
  if (((flags >> 16) & 0xff) != observed_sm)
    return fail("native cubin: distinct ELF SM fields are outside this metadata-trial recipe");
  const auto phoff = fatbin::ReadU64(p + 32);
  const auto shoff = fatbin::ReadU64(p + 40);
  const auto phsize = fatbin::ReadU16(p + 54), phnum = fatbin::ReadU16(p + 56);
  const auto shsize = fatbin::ReadU16(p + 58), shnum = fatbin::ReadU16(p + 60);
  const auto strindex = fatbin::ReadU16(p + 62);
  if (shsize != 64 || shnum < 2 || shnum > 4096 || strindex >= shnum ||
      shoff < 64 || !fits(shoff, uint64_t{shnum} * shsize) || phnum > 4096 ||
      (phnum && (phsize != 56 || phoff < 64 || !fits(phoff, uint64_t{phnum} * phsize))))
    return fail("native cubin: ELF table bounds");
  const auto* strings = p + shoff + uint64_t{strindex} * shsize;
  const auto stroff = fatbin::ReadU64(strings + 24), strbytes = fatbin::ReadU64(strings + 32);
  if (fatbin::ReadU32(strings + 4) != 3 || !strbytes || stroff < 64 || !fits(stroff, strbytes))
    return fail("native cubin: section names");
  size_t text_sections = 0;
  for (size_t i = 1; i < shnum; ++i) {
    const auto* s = p + shoff + i * shsize;
    const auto name = fatbin::ReadU32(s), type = fatbin::ReadU32(s + 4);
    const auto properties = fatbin::ReadU64(s + 8);
    const auto offset = fatbin::ReadU64(s + 24), count = fatbin::ReadU64(s + 32);
    const auto alignment = fatbin::ReadU64(s + 48);
    if (name >= strbytes || !std::memchr(p + stroff + name, 0, strbytes - name) ||
        (alignment && (alignment & (alignment - 1))) ||
        (type != 8 && count && (offset < 64 || !fits(offset, count))))
      return fail("native cubin: section bounds");
    if (properties & 4) {
      const std::string_view label(reinterpret_cast<const char*>(p + stroff + name));
      if (type != 1 || !count || (count % 16) || (offset % 16) ||
          !label.starts_with(".text.") ||
          (offset < shoff + uint64_t{shnum} * shsize && shoff < offset + count))
        return fail("native cubin: instruction section layout");
      ++text_sections;
    }
  }
  if (!text_sections) return fail("native cubin: no instruction sections");
  for (size_t i = 0; i < phnum; ++i) {
    const auto* h = p + phoff + i * phsize;
    const auto offset = fatbin::ReadU64(h + 8), count = fatbin::ReadU64(h + 32);
    if (!fits(offset, count) || fatbin::ReadU64(h + 40) < count)
      return fail("native cubin: program bounds");
  }
  source_sm = observed_sm;
  reason.clear();
  return true;
}

// Native-only or opaque-PTX plus a unique native image can be tried without
// inventing a SASS translator. Broken compression/text transformations never
// fall through here as an instruction-byte mutation. The NVIDIA result decides
// actual compatibility, including on SM75.
inline bool RetargetNativeToTarget(std::span<const unsigned char> bytes,
                                   std::vector<unsigned char>& replacement,
                                   std::string& reason, uint32_t target_sm,
                                   uint32_t* source_sm = nullptr) {
  replacement.clear();
  if (source_sm) *source_sm = 0;
  if (target_sm != 75 && target_sm != 86) { reason = "native cubin: no selected backport target"; return false; }
  std::vector<fatbin::Entry> entries;
  size_t end = 0;
  if (!fatbin::Parse(bytes, entries, end)) { reason = "native cubin: fatbin bounds"; return false; }
  if (std::any_of(entries.begin(), entries.end(), [target_sm](const auto& entry) {
        return entry.architecture == target_sm;
      })) {
    reason = "native/target payload already present; no native retarget transaction needed";
    return false;
  }
  const fatbin::Entry* elf = nullptr;
  bool ambiguous = false;
  for (const auto& entry : entries) {
    if (entry.kind != 2 || entry.architecture <= target_sm) continue;
    if (!elf || entry.architecture < elf->architecture) {
      elf = &entry;
      ambiguous = false;
    } else if (entry.architecture == elf->architecture) {
      ambiguous = true;
    }
  }
  if (!elf || ambiguous || elf->compressed_bytes) {
    reason = "native cubin: unique uncompressed native image absent"; return false;
  }
  // Resolve the numeric source from both actual ELF fields. A wider fatbin
  // architecture value must fit and match those fields; never truncate it.
  uint32_t observed_sm = 0;
  const auto payload = bytes.subspan(elf->PayloadOffset(), elf->payload_bytes);
  if (!InspectNative(payload, reason, observed_sm)) return false;
  if (observed_sm != elf->architecture) {
    reason = "native cubin: inconsistent fatbin/ELF source metadata"; return false;
  }
  const fatbin::Entry* ptx = nullptr;
  for (const auto& entry : entries) {
    if (entry.kind != 1 || entry.architecture != observed_sm) continue;
    if (ptx) { reason = "native cubin: multiple PTX entries"; return false; }
    ptx = &entry;
  }
  if (ptx) {
    std::vector<unsigned char> raw;
    if (!fatbin::DecodePtx(bytes, *ptx, raw)) {
      reason = "native cubin: invalid opaque payload encoding"; return false;
    }
    constexpr std::string_view directive = ".version";
    const size_t controls = std::count_if(raw.begin(), raw.end(), [](unsigned char c) {
      return c > 126 || (c < 32 && c != 0 && c != '\t' && c != '\r' && c != '\n');
    });
    if (raw.size() < 32 || controls < raw.size() / 8 ||
        std::search(raw.begin(), raw.end(), directive.begin(), directive.end()) != raw.end()) {
      reason = "native cubin: plaintext or unclassified PTX is not a native fallback"; return false;
    }
  }
  replacement.assign(bytes.begin(), bytes.begin() + end);
  // PTX and all instruction bytes remain exactly native. The requested cubin
  // is selected instead of asking the text retargeter to edit opaque bytes.
  replacement[elf->offset + 28] = static_cast<unsigned char>(target_sm);
  replacement[elf->PayloadOffset() + 48] = static_cast<unsigned char>(target_sm);
  replacement[elf->PayloadOffset() + 50] = static_cast<unsigned char>(target_sm);
  if (source_sm) *source_sm = observed_sm;
  reason.clear();
  return true;
}

// Historical offline names remain source-compatible. They no longer impose an
// SM89 admission test; the current bytes choose and prove the source metadata.
inline bool InspectSm89(std::span<const unsigned char> bytes, std::string& reason) {
  uint32_t source_sm = 0;
  return InspectNative(bytes, reason, source_sm);
}
inline bool RetargetOpaqueSm89ToTarget(std::span<const unsigned char> bytes,
                                      std::vector<unsigned char>& replacement,
                                      std::string& reason, uint32_t target_sm) {
  return RetargetNativeToTarget(bytes, replacement, reason, target_sm);
}
inline bool RetargetOpaqueSm89To86(std::span<const unsigned char> bytes,
                                  std::vector<unsigned char>& replacement,
                                  std::string& reason) {
  return RetargetNativeToTarget(bytes, replacement, reason, 86);
}

}  // namespace mfgunlock::cubin
