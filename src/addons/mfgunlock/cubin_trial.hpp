/*
 * Bounded native-code trial for opaque SM89 fatbin entries on SM86.
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
inline bool InspectSm89(std::span<const unsigned char> bytes, std::string& reason) {
  const auto fail = [&](const char* message) { reason = message; return false; };
  const auto fits = [&](uint64_t offset, uint64_t length) {
    return offset <= bytes.size() && length <= bytes.size() - offset;
  };
  if (bytes.size() < 64 || bytes.size() > fatbin::kMaxFatbinBytes)
    return fail("native cubin: ELF bounds");
  const auto* p = bytes.data();
  if (std::memcmp(p, "\177ELF", 4) || p[4] != 2 || p[5] != 1 || p[6] != 1 ||
      p[7] != 0x33 || p[8] != 7 || fatbin::ReadU16(p + 16) != 2 ||
      fatbin::ReadU16(p + 18) != 190 || fatbin::ReadU16(p + 52) != 64)
    return fail("native cubin: ELF contract");
  const uint32_t flags = fatbin::ReadU32(p + 48);
  if ((flags & 0xff) != 89 || ((flags >> 16) & 0xff) != 89)
    return fail("native cubin: inconsistent target metadata");
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
  reason.clear();
  return true;
}

// Only the opaque-PTX/one-native-image form is eligible. No generic fallback
// from an unsupported plaintext instruction (e.g. FP8) to unchanged SASS.
inline bool RetargetOpaqueSm89To86(std::span<const unsigned char> bytes,
                                  std::vector<unsigned char>& replacement,
                                  std::string& reason) {
  replacement.clear();
  std::vector<fatbin::Entry> entries;
  size_t end = 0;
  if (!fatbin::Parse(bytes, entries, end)) { reason = "native cubin: fatbin bounds"; return false; }
  const fatbin::Entry* ptx = nullptr;
  const fatbin::Entry* elf = nullptr;
  for (const auto& entry : entries) {
    if (entry.architecture != 89 || (entry.kind != 1 && entry.kind != 2)) {
      reason = "native cubin: mixed target selection"; return false;
    }
    if (entry.kind == 1) {
      if (ptx) { reason = "native cubin: multiple PTX entries"; return false; }
      ptx = &entry;
    } else {
      if (elf) { reason = "native cubin: multiple native entries"; return false; }
      elf = &entry;
    }
  }
  if (!ptx || !elf || ptx->flags != 0x2041 || !ptx->compressed_bytes || !ptx->unpacked_bytes ||
      !fatbin::ReadU64(bytes.data() + ptx->offset + 48) ||
      elf->flags != 0x41 || elf->compressed_bytes) {
    reason = "native cubin: opaque/native pair absent"; return false;
  }
  std::vector<unsigned char> raw(ptx->unpacked_bytes);
  if (!fatbin::Lz4BlockDecompress(bytes.data() + ptx->PayloadOffset(), ptx->compressed_bytes,
                                raw.data(), raw.size())) {
    reason = "native cubin: invalid opaque compression"; return false;
  }
  constexpr std::string_view directive = ".version";
  const size_t controls = std::count_if(raw.begin(), raw.end(), [](unsigned char c) {
    return c > 126 || (c < 32 && c != 0 && c != '\t' && c != '\r' && c != '\n');
  });
  if (raw.size() < 32 || controls < raw.size() / 8 ||
      std::search(raw.begin(), raw.end(), directive.begin(), directive.end()) != raw.end()) {
    reason = "native cubin: not an opaque PTX payload"; return false;
  }
  const auto payload = bytes.subspan(elf->PayloadOffset(), elf->payload_bytes);
  if (!InspectSm89(payload, reason)) return false;
  replacement.assign(bytes.begin(), bytes.begin() + end);
  // PTX and all instruction bytes remain exactly native. A matching SM86 cubin
  // is selected instead of asking the text retargeter to edit opaque bytes.
  replacement[elf->offset + 28] = 86;
  replacement[elf->PayloadOffset() + 48] = 86;
  replacement[elf->PayloadOffset() + 50] = 86;
  reason.clear();
  return true;
}

}  // namespace mfgunlock::cubin
