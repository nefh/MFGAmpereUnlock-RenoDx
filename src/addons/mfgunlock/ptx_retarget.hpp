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
};

inline std::string_view Trim(std::string_view value) {
  const size_t first = value.find_first_not_of(" \t\r");
  if (first == value.npos) return {};
  return value.substr(first, value.find_last_not_of(" \t\r") - first + 1);
}

// Preserve byte positions while blanking comments and strings. This prevents a
// filename, quoted string, or comment containing "sm_89" from being retargeted.
inline bool FindTargetDigits(std::span<const unsigned char> raw, uint32_t target_sm,
                              size_t& digit) {
  const auto nul = std::find(raw.begin(), raw.end(), 0);
  if (nul == raw.end()) return false;

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
      const auto target = Trim(line.substr(7));
      if (target != "sm_89" || ++targets != 1) return false;
      digit = static_cast<size_t>(target.data() - text.data()) + 3;
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

  // These instructions are outside the supported backport targets.
  constexpr std::string_view kUnsupported[] = {
      "wgmma.",
      "tcgen05.",
      "tensormap.",
      "cp.async.bulk",
      ".e4m3",
      ".e5m2",
      ".e2m1",
  };
  for (const auto token : kUnsupported) {
    if (text.find(token) != text.npos) return false;
  }

  if (target_sm == 75) {
    if (HasTuringHalfMinMax(text)) return false;
    // PTX features introduced with Ampere cannot be enabled by changing .target.
    constexpr std::string_view kSm80Instructions[] = {
        "cp.async", "mbarrier.", "redux.sync", "mma.sp.", ".bf16", ".tf32",
        ".f16x2.f32", "max.f16", "min.f16", ".relu",
        "mma.sync.aligned.m16n8k16", "mma.sync.aligned.m16n8k32",
        "mma.sync.aligned.m16n8k64", "mma.sync.aligned.m8n8k128",
        "mma.sync.aligned.m16n8k128", "mma.sync.aligned.m16n8k256",
        "mma.sync.aligned.m8n8k4.row.col.f64",
    };
    for (const auto token : kSm80Instructions) {
      if (text.find(token) != text.npos) return false;
    }
  }
  return targets == 1 && versions == 1 && addresses == 1 &&
         text.find(".entry") != text.npos;
}


// The lowered program fits inside the original complete container, but not
// necessarily its compressed PTX slot. Publish one uncompressed SM75 image;
// the existing provider byte transaction owns every write and its rollback.
inline bool RebuildLoweredProvider(std::span<const unsigned char> bytes,
                                   const fatbin::Entry& source,
                                   const std::string& text,
                                   std::vector<unsigned char>& replacement) {
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
  const uint32_t target = 75;
  const uint32_t zero32 = 0;
  const uint64_t zero64 = 0;
  const uint64_t flags = 0x41;
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
  if (source.compressed_bytes == 0 || source.unpacked_bytes == 0) return false;
  if (source.flags == 0x2041) return true;
  // Observed newer legacy-network form: still a bounded LZ4 PTX entry, but
  // with a 120-byte header and 0x2011 flags. Both targets keep the complete
  // entry header and require decoded-byte verification after any rebuild.
  return (profile.target_sm == 86 || profile.target_sm == 75) &&
         source.flags == 0x2011 && source.header_bytes == 120;
}

inline bool RebuildCompressedProvider(std::span<const unsigned char> bytes,
                                      const fatbin::Entry& source,
                                      const std::string& text,
                                      std::vector<unsigned char>& replacement) {
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
  const uint32_t target = 75;
  const uint64_t unpacked_bytes = raw.size();
  std::memcpy(entry + 16, &compressed_bytes, sizeof(compressed_bytes));
  std::memcpy(entry + 28, &target, sizeof(target));
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
                                    std::vector<unsigned char>& replacement) {
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
  const uint32_t size=static_cast<uint32_t>(compressed.size()), target=75;
  std::memcpy(replacement.data()+8,&outer,8);
  std::memcpy(entry+8,&payload,8); std::memcpy(entry+16,&size,4);
  std::memcpy(entry+28,&target,4); std::memcpy(entry+56,&unpacked,8);
  std::memset(entry+source.header_bytes,0,stored);
  std::memcpy(entry+source.header_bytes,compressed.data(),compressed.size());
  std::vector<fatbin::Entry> entries; size_t end=0;
  if(!fatbin::Parse(replacement,entries,end) || entries.size()!=1 ||
      entries[0].architecture!=75 || end!=fatbin::kHeaderBytes+outer) return false;
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
                                 std::vector<unsigned char>& replacement) {
  if(text.size()+1>fatbin::kMaxPtxBytes) return false;
  const size_t total=fatbin::kHeaderBytes+source.header_bytes+((text.size()+1+7)&~size_t{7});
  if(total<=bytes.size() || total>fatbin::kMaxFatbinBytes) return false;
  std::vector<unsigned char> storage(bytes.begin(),bytes.end());storage.resize(total);
  return RebuildLoweredProvider(storage,source,text,replacement);
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
  const auto reject = [&](const char* message) {
    plan = {};
    reason = message;
    return Result::kRejected;
  };

  if (!profile.RequiresProviderRetarget()) {
    plan = {};
    return Result::kUnchanged;
  }
  if (profile.target_sm != 86 && profile.target_sm != 75)
    return reject("unsupported PTX target");

  if (!fatbin::Parse(bytes, entries, end)) return reject("invalid fatbin bounds/layout");

  const auto sm89 = std::find_if(entries.begin(), entries.end(), [](const auto& entry) {
    return entry.architecture == 89;
  });
  if (sm89 == entries.end()) {
    plan = {};
    return Result::kUnchanged;
  }

  // Preserve the established byte layout for the two common forms. Other
  // bounded entry orders use the unique SM89 PTX, compacted to one visible
  // entry; do not let an unrelated image take precedence over that trial.
  const bool single = entries.size() == 1 && entries[0].kind == 1 &&
                      entries[0].architecture == 89;
  const bool mixed = entries.size() == 3 && entries[0].kind == 1 &&
                     entries[0].architecture == 120 && entries[1].kind == 1 &&
                     entries[1].architecture == 89 && entries[2].kind == 2 &&
                     entries[2].architecture == 89;
  const fatbin::Entry* selected = nullptr;
  size_t cubins = 0;
  for (const auto& candidate : entries) {
    if (candidate.kind == 2) ++cubins;
    if (candidate.kind != 1 || candidate.architecture != 89) continue;
    if (selected) return reject("ambiguous SM89 PTX entries");
    selected = &candidate;
  }
  if (!selected) return reject("SM89 image has no retargetable PTX entry");
  const auto& entry = *selected;
  if (!ReviewedInPlacePtxLayout(entry, profile)) {
    return reject("unreviewed PTX compression flags/layout");
  }

  std::vector<unsigned char> raw(entry.unpacked_bytes);
  std::vector<unsigned char> check(entry.unpacked_bytes);
  const auto* compressed = bytes.data() + entry.PayloadOffset();
  if (!fatbin::Lz4BlockDecompress(compressed, entry.compressed_bytes, raw.data(), raw.size())) {
    return reject("invalid LZ4 block");
  }

  if (profile.target_sm == 75) {
    if(!HasTextPtxPrefix(raw))
      return reject("SM75 opaque/unsupported SM89 source encoding: decoded kind-1 image is not textual PTX");
    const auto nul = std::find(raw.begin(), raw.end(), 0);
    if (nul == raw.end()) return reject("PTX terminator missing");
    std::string text(raw.begin(), nul);
    std::erase(text, '\r');
    Sm75LoweringCounts counts;
    if (!LowerSm75Structurally(text, counts, reason)) { plan = {}; return Result::kRejected; }
    if (counts.Total()) {
      // Preserve the established layout of the physically tested reference:
      // packed-half mixed containers use the reclaimed whole-container space;
      // network programs stay compressed. This choice is by layout, not hash.
      if ((counts.mma || counts.half_minmax.Total() || counts.fma_relu) && !CompactProviderPtx(text, reason)) {
        plan = {}; return Result::kRejected;
      }
      std::vector<unsigned char> lowered(text.begin(), text.end());
      lowered.push_back(0);
      size_t digit = 0;
      if (!FindTargetDigits(lowered, 75, digit))
        return reject("lowered PTX has unsupported header/instructions");
      text.replace(digit, 2, "75");
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

  size_t target_digit = 0;
  if (!FindTargetDigits(raw, profile.target_sm, target_digit))
    return reject("unreviewed PTX header/instructions");

  std::vector<unsigned char> replacement(bytes.begin(), bytes.begin() + end);
  const unsigned char digits[] = {
      static_cast<unsigned char>('0' + profile.target_sm / 10),
      static_cast<unsigned char>('0' + profile.target_sm % 10),
  };
  for (size_t i = 0; i < 2; ++i) {
    if (raw[target_digit + i] == digits[i]) continue;
    size_t literal = 0;
    if (!fatbin::Lz4BlockDecompress(compressed, entry.compressed_bytes, check.data(), check.size(),
                                    target_digit + i, &literal) ||
        literal >= entry.compressed_bytes || compressed[literal] != raw[target_digit + i]) {
      return reject("target is not an independent LZ4 literal");
    }
    replacement[entry.PayloadOffset() + literal] = digits[i];
  }
  if (!fatbin::Lz4BlockDecompress(replacement.data() + entry.PayloadOffset(),
                                  entry.compressed_bytes, check.data(), check.size())) {
    return reject("edited LZ4 block invalid");
  }

  raw[target_digit] = digits[0];
  raw[target_digit + 1] = digits[1];
  if (raw != check) return reject("target literal is shared with another output byte");

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
  plan.ptx_bytes = entry.unpacked_bytes;
  plan.hidden_cubins = cubins;
  return Result::kRetargeted;
}

}  // namespace mfgunlock::ptx
