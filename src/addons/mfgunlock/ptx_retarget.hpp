/*
 * Structural PTX retargeting for bounded DLSS-G fatbin layouts.
 * SPDX-License-Identifier: MIT
 *
 * Ordinary target changes edit independent LZ4 literals and verify the decoded
 * bytes. SM89 programs use operand-checked SM75 compatibility lowerings;
 * rebuilt single-image PTX is recompressed only when it fits its exact container.
 */
#pragma once

#include <algorithm>
#include <string>
#include <string_view>

#include "./architecture.hpp"
#include "./fatbin.hpp"
#include "./ptx_compat.hpp"

namespace mfgunlock::ptx {

enum class Result {
  kUnchanged,
  kRetargeted,
  kRejected,
};

struct Plan {
  std::vector<unsigned char> replacement;
  size_t ptx_offset = 0;
  size_t ptx_bytes = 0;
  size_t hidden_cubins = 0;
  bool requires_external_storage = false;
  uint32_t source_sm = 0;
  uint32_t target_sm = 0;
};

inline std::string_view Trim(std::string_view value) {
  const size_t first = value.find_first_not_of(" \t\r");
  if (first == value.npos) return {};
  return value.substr(first, value.find_last_not_of(" \t\r") - first + 1);
}

// Preserve byte positions while blanking comments and strings. This prevents a
// filename, quoted string, or comment containing "sm_89" from being retargeted.
inline bool FindTargetDigits(std::span<const unsigned char> raw, uint32_t target_sm,
                              size_t& digit, uint32_t source_sm = 89,
                              size_t* digit_bytes = nullptr) {
  const auto nul = std::find(raw.begin(), raw.end(), 0);
  if (nul == raw.end() || std::any_of(nul, raw.end(), [](unsigned char c) { return c != 0; })) return false;

  std::string text(raw.begin(), nul);
  bool block_comment = false;
  bool quoted = false;

  for (size_t i = 0; i < text.size(); ++i) {
    if (block_comment) {
      if (text[i] == '*' && i + 1 < text.size() && text[i + 1] == '/') {
        text[i] = ' ';
        text[i + 1] = ' ';
        ++i;
        block_comment = false;
      } else if (text[i] != '\n') {
        text[i] = ' ';
      }
      continue;
    }

    if (quoted) {
      const char c = text[i];
      text[i] = ' ';
      if (c == '\\' && i + 1 < text.size()) {
        text[++i] = ' ';
      } else if (c == '"') {
        quoted = false;
      } else if (c == '\n') {
        return false;
      }
      continue;
    }

    if (text[i] == '"') {
      text[i] = ' ';
      quoted = true;
    } else if (text[i] == '/' && i + 1 < text.size() && text[i + 1] == '*') {
      text[i] = ' ';
      text[i + 1] = ' ';
      ++i;
      block_comment = true;
    } else if (text[i] == '/' && i + 1 < text.size() && text[i + 1] == '/') {
      for (; i < text.size() && text[i] != '\n'; ++i) text[i] = ' ';
    }
  }

  if (block_comment || quoted) return false;

  size_t targets = 0;
  size_t versions = 0;
  size_t addresses = 0;

  for (size_t offset = 0; offset < text.size();) {
    const size_t newline = text.find('\n', offset);
    const size_t end = newline == text.npos ? text.size() : newline;
    const auto line = Trim(std::string_view(text).substr(offset, end - offset));

    const auto is_directive = [&](std::string_view name) {
      return line.starts_with(name) && line.size() > name.size() &&
             (line[name.size()] == ' ' || line[name.size()] == '\t');
    };

    if (is_directive(".target")) {
      const auto specification = Trim(line.substr(7));
      const auto target = specification.substr(0, specification.find_first_of(", \t"));
      if (target != "sm_" + std::to_string(source_sm) || ++targets != 1) return false;
      digit = static_cast<size_t>(target.data() - text.data()) + 3;
      if (digit_bytes) *digit_bytes = target.size() - 3;
    } else if (is_directive(".version")) {
      const auto version = Trim(line.substr(8));
      const auto dot = version.find('.');
      const auto digits = [](std::string_view value) {
        return !value.empty() && std::all_of(value.begin(), value.end(),
            [](char c) { return c >= '0' && c <= '9'; });
      };
      if (dot == version.npos || !digits(version.substr(0, dot)) ||
          !digits(version.substr(dot + 1)) || ++versions != 1) return false;
      // Keep the declared ISA version unchanged. An unfamiliar number alone
      // does not veto a trial; actual syntax/instructions and the driver do.
    } else if (is_directive(".address_size")) {
      if (Trim(line.substr(13)) != "64" || ++addresses != 1) return false;
    }

    offset = end + 1;
  }

  // Do not decide driver/JIT support from instruction names. Recognized
  // operand-safe lowerings are applied by the caller; all remaining valid PTX
  // instructions receive the actual NVIDIA JIT attempt with its real result.
  (void)target_sm;
  return targets == 1 && versions == 1 && addresses == 1 &&
         text.find(".entry") != text.npos;
}


// The lowered program fits inside the original complete container, but not
// necessarily its compressed PTX slot. Publish one uncompressed SM75 image;
// the existing provider byte transaction owns every write and its rollback.
inline bool RebuildLoweredProvider(std::span<const unsigned char> bytes,
                                   const fatbin::Entry& source,
                                   const std::string& text,
                                   std::vector<unsigned char>& replacement, uint32_t target = 75) {
  if (text.size() > fatbin::kMaxPtxBytes || source.header_bytes < 64 ||
      source.header_bytes % 8 != 0) return false;
  const size_t payload = (text.size() + 1 + 7) & ~size_t{7};
  if (source.offset > bytes.size() || source.header_bytes > bytes.size() - source.offset ||
      bytes.size() < fatbin::kHeaderBytes + source.header_bytes ||
      payload > bytes.size() - fatbin::kHeaderBytes - source.header_bytes) return false;
  replacement.assign(bytes.begin(), bytes.end());
  auto* entry = replacement.data() + fatbin::kHeaderBytes;
  std::memcpy(entry, bytes.data() + source.offset, source.header_bytes);
  const uint64_t outer = source.header_bytes + payload;
  const uint64_t stored = payload;
  const uint32_t zero32 = 0;
  const uint64_t zero64 = 0;
  const uint64_t flags = source.flags & ~uint64_t{0x2000};
  std::memcpy(replacement.data() + 8, &outer, sizeof(outer));
  std::memcpy(entry + 8, &stored, sizeof(stored));
  std::memcpy(entry + 16, &zero32, sizeof(zero32));
  std::memcpy(entry + 28, &target, sizeof(target));
  std::memcpy(entry + 40, &flags, sizeof(flags));
  std::memcpy(entry + 56, &zero64, sizeof(zero64));
  std::memset(entry + source.header_bytes, 0, payload);
  std::memcpy(entry + source.header_bytes, text.data(), text.size());
  std::vector<fatbin::Entry> entries;
  size_t end = 0;
  return fatbin::Parse(replacement, entries, end) && entries.size() == 1 &&
      entries[0].architecture == target && entries[0].flags == flags &&
      end == fatbin::kHeaderBytes + outer;
}


inline bool ReviewedInPlacePtxLayout(const fatbin::Entry& source,
                                    const ArchitectureProfile& profile) {
  if (source.compressed_bytes && source.unpacked_bytes == 0) return false;
  (void)profile;
  // Layout/bounds have been parsed. Successful LZ4 decoding and the decoded
  // text prove the usable representation; unrelated flag bits and extension
  // header sizes must not function as an allowlist. Preserve them in place.
  return source.header_bytes >= 64 && source.header_bytes % 8 == 0;
}

inline bool RebuildCompressedProvider(std::span<const unsigned char> bytes,
                                      const fatbin::Entry& source,
                                      const std::string& text,
                                      std::vector<unsigned char>& replacement, uint32_t target = 75) {
  if (!ReviewedInPlacePtxLayout(source, architecture::kTuring) ||
      text.size() + 1 > fatbin::kMaxPtxBytes || source.PayloadOffset() > bytes.size() ||
      source.payload_bytes > bytes.size() - source.PayloadOffset()) return false;

  std::string raw = text;
  raw.push_back('\0');
  std::vector<unsigned char> compressed;
  if (!fatbin::Lz4BlockCompress(raw, source.payload_bytes, compressed) &&
      !fatbin::Lz4BlockCompressHigh(raw, source.payload_bytes, compressed)) return false;
  if (compressed.empty() || compressed.size() > source.payload_bytes) return false;

  replacement.assign(bytes.begin(), bytes.end());
  auto* entry = replacement.data() + source.offset;
  const uint32_t compressed_bytes = static_cast<uint32_t>(compressed.size());
  const uint64_t unpacked_bytes = raw.size();
  const uint64_t flags = source.flags | uint64_t{0x2000};
  std::memcpy(entry + 16, &compressed_bytes, sizeof(compressed_bytes));
  std::memcpy(entry + 28, &target, sizeof(target));
  std::memcpy(entry + 40, &flags, sizeof(flags));
  std::memcpy(entry + 56, &unpacked_bytes, sizeof(unpacked_bytes));
  std::memset(replacement.data() + source.PayloadOffset(), 0, source.payload_bytes);
  std::memcpy(replacement.data() + source.PayloadOffset(), compressed.data(), compressed.size());

  std::vector<fatbin::Entry> entries;
  size_t end = 0;
  if (!fatbin::Parse(replacement, entries, end) || entries.size() != 1 ||
      entries[0].architecture != target || entries[0].compressed_bytes != compressed.size() ||
      entries[0].unpacked_bytes != raw.size() || end > replacement.size()) return false;
  std::vector<unsigned char> decoded(raw.size());
  return fatbin::Lz4BlockDecompress(
             replacement.data() + entries[0].PayloadOffset(), entries[0].compressed_bytes,
             decoded.data(), decoded.size()) &&
         std::equal(decoded.begin(), decoded.end(),
                    reinterpret_cast<const unsigned char*>(raw.data()));
}


// Reclaim the complete bounded container for one compressed target image.
// Called only after the established in-place encodings failed. Other embedded
// architectures cannot serve SM75, and no byte beyond the original container
// is borrowed. Header extensions are preserved from the selected entry.
inline bool RepackCompressedProvider(std::span<const unsigned char> bytes,
                                    const fatbin::Entry& source, const std::string& text,
                                    std::vector<unsigned char>& replacement, uint32_t target = 75) {
  if (source.offset>bytes.size() || source.header_bytes>bytes.size()-source.offset ||
      bytes.size()<fatbin::kHeaderBytes+source.header_bytes ||
      !ReviewedInPlacePtxLayout(source,architecture::kTuring) ||
      text.size()+1>fatbin::kMaxPtxBytes) return false;
  std::string raw=text; raw.push_back(0);
  const size_t capacity=(bytes.size()-fatbin::kHeaderBytes-source.header_bytes)&~size_t{7};
  std::vector<unsigned char> compressed;
  if (!fatbin::Lz4BlockCompress(raw,capacity,compressed) &&
      !fatbin::Lz4BlockCompressHigh(raw,capacity,compressed)) return false;
  const size_t stored=(compressed.size()+7)&~size_t{7};
  replacement.assign(bytes.begin(),bytes.end());
  auto* entry=replacement.data()+fatbin::kHeaderBytes;
  std::memmove(entry,bytes.data()+source.offset,source.header_bytes);
  // Retain the original physical extent: trailing former entries become
  // padding of this entry, not new containers on a subsequent image scan.
  const uint64_t outer=source.header_bytes+capacity, payload=capacity, unpacked=raw.size();
  const uint64_t flags=source.flags|uint64_t{0x2000};
  const uint32_t size=static_cast<uint32_t>(compressed.size());
  std::memcpy(replacement.data()+8,&outer,8);
  std::memcpy(entry+8,&payload,8); std::memcpy(entry+16,&size,4);
  std::memcpy(entry+28,&target,4); std::memcpy(entry+40,&flags,8); std::memcpy(entry+56,&unpacked,8);
  std::memset(entry+source.header_bytes,0,stored);
  std::memcpy(entry+source.header_bytes,compressed.data(),compressed.size());
  std::vector<fatbin::Entry> entries; size_t end=0;
  if(!fatbin::Parse(replacement,entries,end) || entries.size()!=1 ||
      entries[0].architecture!=target || end!=fatbin::kHeaderBytes+outer) return false;
  std::vector<unsigned char> decoded(raw.size());
  return fatbin::Lz4BlockDecompress(replacement.data()+entries[0].PayloadOffset(),size,
      decoded.data(),decoded.size()) && std::equal(decoded.begin(),decoded.end(),
      reinterpret_cast<const unsigned char*>(raw.data()));
}

// A plan can outgrow its slot, but cannot publish itself. The provider must
// subsequently prove every supported host loadsite and retain the allocation
// with its rollback state. This helper only builds bytes in private memory.
inline bool BuildExternalProvider(std::span<const unsigned char> bytes,
                                 const fatbin::Entry& source, const std::string& text,
                                 std::vector<unsigned char>& replacement, uint32_t target = 75) {
  if(text.size()+1>fatbin::kMaxPtxBytes) return false;
  const size_t total=fatbin::kHeaderBytes+source.header_bytes+((text.size()+1+7)&~size_t{7});
  if(total<=bytes.size() || total>fatbin::kMaxFatbinBytes) return false;
  std::vector<unsigned char> storage(bytes.begin(),bytes.end());storage.resize(total);
  return RebuildLoweredProvider(storage,source,text,replacement,target);
}


// A fatbin kind-1 record is not necessarily readable PTX. Some valid LZ4
// records decode to an opaque image. Classify the source BEFORE invoking text
// lowerings; random binary quote bytes must not be reported as PTX syntax.
// This only selects a parser. FindTargetDigits and the lowering validators
// still authorize every text transformation, with no ISA-version whitelist.
inline bool HasTextPtxPrefix(std::span<const unsigned char> raw) {
  const auto nul=std::find(raw.begin(),raw.end(),0);
  if(nul==raw.end())return false;
  const std::string_view text(reinterpret_cast<const char*>(raw.data()),static_cast<size_t>(nul-raw.begin()));
  size_t at=0;
  for(;;) {
    while(at<text.size()&&(text[at]==' '||text[at]=='\t'||text[at]=='\r'||text[at]=='\n'))++at;
    if(text.substr(at,2)=="//") {
      const auto end=text.find('\n',at+2);if(end==text.npos)return false;at=end+1;
    } else if(text.substr(at,2)=="/*") {
      const auto end=text.find("*/",at+2);if(end==text.npos)return false;at=end+2;
    } else break;
  }
  if(!text.substr(at).starts_with(".version")||at+8>=text.size()||
      (text[at+8]!=' '&&text[at+8]!='\t'))return false;
  // UTF-8 in quoted filenames/comments is not an ISA restriction. Only
  // binary control bytes (other than ordinary text whitespace) disqualify.
  return std::none_of(text.begin(),text.end(),[](unsigned char c){
    return (c<32&&c!='\n'&&c!='\r'&&c!='\t')||c==127;
  });
}

inline Result Retarget(std::span<const unsigned char> bytes, Plan& plan, std::string& reason,
                        const ArchitectureProfile& profile = architecture::kAmpere) {
  reason.clear();
  plan.requires_external_storage = false;

  std::vector<fatbin::Entry> entries;
  size_t end = 0;
  const auto reject = [&](std::string_view message) {
    plan = {};
    reason.assign(message);
    return Result::kRejected;
  };

  if (!profile.RequiresProviderRetarget()) {
    plan = {};
    return Result::kUnchanged;
  }
  if (profile.target_sm != 86 && profile.target_sm != 75)
    return reject("unsupported PTX target");

  if (!fatbin::Parse(bytes, entries, end)) return reject("invalid fatbin bounds/layout");

  const auto native_target = std::find_if(entries.begin(), entries.end(), [&](const auto& entry) {
    return entry.architecture == profile.target_sm;
  });
  if (native_target != entries.end()) {
    // Preserve the already available target selection. Its actual driver
    // load is the evidence; a higher PTX alternative does not force rewriting.
    plan = {};
    return Result::kUnchanged;
  }

  const fatbin::Entry* selected = nullptr;
  size_t candidates = 0, cubins = 0;
  for (const auto& candidate : entries) {
    if (candidate.kind == 2) ++cubins;
    if (candidate.kind != 1 || candidate.architecture <= profile.target_sm) continue;
    if (selected && candidate.architecture > selected->architecture) continue;
    if (!selected || candidate.architecture < selected->architecture) candidates = 0;
    selected = &candidate; ++candidates;
  }
  if (!selected) {
    const bool higher = std::any_of(entries.begin(), entries.end(), [&](const auto& entry) {
      return entry.architecture > profile.target_sm;
    });
    if (higher) return reject("higher-target images have no retargetable PTX entry");
    plan = {}; return Result::kUnchanged;
  }
  if (candidates != 1) return reject("ambiguous SM" + std::to_string(selected->architecture) + " PTX entries");
  // Choose the closest higher actual PTX target to minimize unnecessary ISA
  // changes. Source architecture is verified against its decoded .target;
  // no source SM, provider version or fingerprint allowlist is involved.
  const bool single = entries.size() == 1;
  // Preserve the reference's physical layout as an optimization only.
  const bool mixed = entries.size() == 3 && entries[0].kind == 1 &&
                     entries[0].architecture == 120 && entries[1].kind == 1 &&
                     entries[1].architecture == 89 && entries[2].kind == 2 &&
                     entries[2].architecture == 89;
  const auto& entry = *selected;
  plan.source_sm = entry.architecture;
  plan.target_sm = profile.target_sm;
  if (!ReviewedInPlacePtxLayout(entry, profile)) {
    return reject("PTX compression sizes/layout are incomplete");
  }

  std::vector<unsigned char> raw;
  if (!fatbin::DecodePtx(bytes, entry, raw)) return reject("invalid PTX payload encoding");
  std::vector<unsigned char> check(raw.size());
  const auto* compressed = bytes.data() + entry.PayloadOffset();

  if (profile.target_sm == 75) {
    if(!HasTextPtxPrefix(raw))
      return reject("SM75 opaque/unsupported SM" + std::to_string(entry.architecture) +
          " source encoding: decoded kind-1 image is not textual PTX");
    const auto nul = std::find(raw.begin(), raw.end(), 0);
    if (nul == raw.end()) return reject("PTX terminator missing");
    std::string text(raw.begin(), nul);
    std::erase(text, '\r');
    Sm75LoweringCounts counts;
    if (!LowerSm75Structurally(text, counts, reason)) {
      // A valid vendor spelling outside our transformer grammar is left
      // intact for the native JIT. A failed lowering never commits partial
      // edits, and never causes an architecture-name compatibility veto.
      text.assign(raw.begin(), nul);
      std::erase(text, '\r');
      counts = {};
      reason.clear();
    }
    if (counts.Total()) {
      // Preserve the established layout of the physically tested reference:
      // packed-half mixed containers use the reclaimed whole-container space;
      // network programs stay compressed. This choice is by layout, not hash.
      if ((counts.mma || counts.half_minmax.Total() || counts.fma_relu) && !CompactProviderPtx(text, reason)) {
        // Compaction is only a space optimization. Keep the valid program
        // and try the existing rebuild/overflow paths when it is unavailable.
        reason.clear();
      }
      std::vector<unsigned char> lowered(text.begin(), text.end());
      lowered.push_back(0);
      size_t digit = 0, digit_bytes = 0;
      if (!FindTargetDigits(lowered, 75, digit, entry.architecture, &digit_bytes))
        return reject("lowered PTX header is not structurally valid");
      text.replace(digit, digit_bytes, "75");
      bool rebuilt = false;
      if (single) rebuilt = RebuildCompressedProvider(bytes.first(end), entry, text, plan.replacement);
      if (!rebuilt) rebuilt = RebuildLoweredProvider(bytes.first(end), entry, text, plan.replacement);
      if (!rebuilt) rebuilt = RepackCompressedProvider(bytes.first(end),entry,text,plan.replacement);
      if (!rebuilt) {
        rebuilt = BuildExternalProvider(bytes.first(end),entry,text,plan.replacement);
        plan.requires_external_storage = rebuilt;
      }
      if (!rebuilt) return reject("lowered SM75 storage rebuild failed");
      std::vector<fatbin::Entry> verified;
      size_t rebuilt_end = 0;
      if (!fatbin::Parse(plan.replacement, verified, rebuilt_end) || verified.size() != 1)
        return reject("lowered SM75 rebuild verification failed");
      plan.ptx_offset = verified.front().PayloadOffset();
      plan.ptx_bytes = text.size() + 1;
      plan.hidden_cubins = cubins;
      return Result::kRetargeted;
    }
  }

  size_t target_digit = 0, target_digit_bytes = 0;
  if (!FindTargetDigits(raw, profile.target_sm, target_digit, entry.architecture, &target_digit_bytes))
    return reject("PTX header is not structurally valid");

  std::vector<unsigned char> replacement(bytes.begin(), bytes.begin() + end);
  const std::string digits = std::to_string(profile.target_sm);
  bool independent = target_digit_bytes == digits.size();
  if (independent && !entry.compressed_bytes) {
    std::copy(digits.begin(), digits.end(), replacement.begin() + entry.PayloadOffset() + target_digit);
  } else if (independent) {
    for (size_t i = 0; i < digits.size(); ++i) {
      if (raw[target_digit + i] == digits[i]) continue;
      size_t literal = 0;
      if (!fatbin::Lz4BlockDecompress(compressed, entry.compressed_bytes, check.data(), check.size(),
                                    target_digit + i, &literal) ||
          literal >= entry.compressed_bytes || compressed[literal] != raw[target_digit + i]) {
        independent = false;
        break;
      }
      replacement[entry.PayloadOffset() + literal] = digits[i];
    }
    if (independent && !fatbin::Lz4BlockDecompress(replacement.data() + entry.PayloadOffset(),
          entry.compressed_bytes, check.data(), check.size())) independent = false;
  }
  raw.erase(raw.begin() + target_digit, raw.begin() + target_digit + target_digit_bytes);
  raw.insert(raw.begin() + target_digit, digits.begin(), digits.end());
  if (entry.compressed_bytes && independent && raw != check) independent = false;
  if (!independent) {
    // LZ4 aliases are a property of encoding, not incompatibility. Rebuild the
    // same decoded program instead of refusing when target digits are shared.
    const auto nul = std::find(raw.begin(), raw.end(), 0);
    const std::string text(raw.begin(), nul);
    bool rebuilt = single && RebuildCompressedProvider(bytes.first(end), entry, text,
        plan.replacement, profile.target_sm);
    if (!rebuilt) rebuilt = RebuildLoweredProvider(bytes.first(end), entry, text,
        plan.replacement, profile.target_sm);
    if (!rebuilt) rebuilt = RepackCompressedProvider(bytes.first(end), entry, text,
        plan.replacement, profile.target_sm);
    if (!rebuilt) {
      rebuilt = BuildExternalProvider(bytes.first(end), entry, text,
          plan.replacement, profile.target_sm);
      plan.requires_external_storage = rebuilt;
    }
    if (!rebuilt) return reject("retargeted PTX storage rebuild failed");
    std::vector<fatbin::Entry> verified;
    size_t verified_end = 0;
    if (!fatbin::Parse(plan.replacement, verified, verified_end) || verified.size() != 1)
      return reject("retargeted PTX rebuild verification failed");
    plan.ptx_offset = verified[0].PayloadOffset();
    plan.ptx_bytes = text.size() + 1;
    plan.hidden_cubins = cubins;
    return Result::kRetargeted;
  }

  const uint64_t visible_bytes = entry.End() - fatbin::kHeaderBytes;
  std::memcpy(replacement.data() + entry.offset + 28, &profile.target_sm,
              sizeof(profile.target_sm));
  std::memcpy(replacement.data() + 8, &visible_bytes, sizeof(visible_bytes));

  size_t ptx_offset = entry.PayloadOffset();
  if (!single && !mixed) {
    const size_t entry_bytes = entry.End() - entry.offset;
    std::memmove(replacement.data() + fatbin::kHeaderBytes,
                 replacement.data() + entry.offset, entry_bytes);
    const uint64_t compact_visible = entry_bytes;
    std::memcpy(replacement.data() + 8, &compact_visible, sizeof(compact_visible));
    ptx_offset = fatbin::kHeaderBytes + entry.header_bytes;
  }
  plan.replacement = std::move(replacement);
  plan.ptx_offset = ptx_offset;
  plan.ptx_bytes = raw.size();
  plan.hidden_cubins = cubins;
  return Result::kRetargeted;
}

}  // namespace mfgunlock::ptx
