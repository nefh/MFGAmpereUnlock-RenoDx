/*
 * Validated Warp Blend provider patch.
 * SPDX-License-Identifier: MIT
 *
 * Cross-generation Validated Warp Blend. Kernel roles, parameter layouts and
 * local operands are resolved from the actual provider payload; historical
 * whole-file/PTX fingerprints are diagnostic only. Native JIT decides support.
 */

#pragma once

#include <windows.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <set>
#include <sstream>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "./fatbin.hpp"
#include "./provider.hpp"

namespace mfgunlock::validatedwarp {

enum class Mode : int {
  kRedirectControl = 0,
  kBlackwellBaseline = 1,
  kValidatedWarp = 2,
};

constexpr Mode ConfiguredMode(int value) {
  return value >= static_cast<int>(Mode::kRedirectControl) &&
                 value <= static_cast<int>(Mode::kValidatedWarp)
             ? static_cast<Mode>(value) : Mode::kValidatedWarp;
}

inline const char* ModeName(Mode mode) {
  switch (mode) {
    case Mode::kRedirectControl:
      return "redirect control";
    case Mode::kBlackwellBaseline:
      return "Blackwell baseline";
    case Mode::kValidatedWarp:
      return "Validated Warp";
    default:
      return "unknown";
  }
}

inline std::string ModeLabel(Mode mode, uint32_t target_sm) {
  if (mode == Mode::kRedirectControl) return ModeName(mode);
  return std::string(ModeName(mode)) + " sm_" + std::to_string(target_sm);
}

struct DescriptorPatch {
  uint64_t* slot = nullptr;
  uint64_t original = 0;
  DWORD protection = 0;
};

struct Redirect {
  std::vector<DescriptorPatch> descriptors;
  void* allocation = nullptr;
};

struct Result {
  bool detected = false;
  bool applied = false;
  std::string detail;
};

namespace internal {

constexpr uint32_t kFatbinMagic = 0xBA55ED50u;
constexpr size_t kOuterHeader = 16;
constexpr uint32_t kPtxKind = 1;
constexpr uint32_t kAmpereArch = 86;
constexpr uint32_t kAdaArch = 89;
constexpr uint32_t kBlackwellArch = 120;
constexpr uint64_t kUncompressedFlags = 0x41;
constexpr size_t kMaxFatbinSize = fatbin::kMaxFatbinBytes - kOuterHeader;

using ProviderProfile = profiles::ProviderProfile;
using PtxProfile = profiles::PtxProfile;
inline constexpr auto& kProviderProfiles = profiles::kProfiles;
inline constexpr auto& kPtxProfile = profiles::kWarpPtx;

inline uint16_t ReadU16(const uint8_t* bytes) {
  uint16_t value = 0;
  std::memcpy(&value, bytes, sizeof(value));
  return value;
}

inline uint32_t ReadU32(const uint8_t* bytes) {
  uint32_t value = 0;
  std::memcpy(&value, bytes, sizeof(value));
  return value;
}

inline uint64_t ReadU64(const uint8_t* bytes) {
  uint64_t value = 0;
  std::memcpy(&value, bytes, sizeof(value));
  return value;
}

inline uint64_t Fnv1a64(const uint8_t* bytes, size_t size) {
  uint64_t value = 0xcbf29ce484222325ull;
  for (size_t index = 0; index < size; ++index) {
    value = (value ^ bytes[index]) * 0x100000001b3ull;
  }
  return value;
}

inline bool Lz4BlockDecompress(const uint8_t* source, size_t source_size,
                               uint8_t* destination, size_t destination_size) {
  size_t input = 0;
  size_t output = 0;
  while (input < source_size) {
    const uint8_t token = source[input++];
    size_t literals = token >> 4;
    if (literals == 15) {
      uint8_t extra = 0;
      do {
        if (input >= source_size) return false;
        extra = source[input++];
        literals += extra;
      } while (extra == 0xff);
    }
    if (literals > source_size - input || literals > destination_size - output) return false;
    std::memcpy(destination + output, source + input, literals);
    input += literals;
    output += literals;
    if (input == source_size) break;
    if (source_size - input < 2) return false;
    const size_t distance = source[input] | (static_cast<size_t>(source[input + 1]) << 8u);
    input += 2;
    if (distance == 0 || distance > output) return false;
    size_t match = 4 + (token & 0x0f);
    if ((token & 0x0f) == 15) {
      uint8_t extra = 0;
      do {
        if (input >= source_size) return false;
        extra = source[input++];
        match += extra;
      } while (extra == 0xff);
    }
    if (match > destination_size - output) return false;
    for (size_t index = 0; index < match; ++index) {
      destination[output + index] = destination[output + index - distance];
    }
    output += match;
  }
  return input == source_size && output == destination_size;
}

inline bool ReplaceOnce(std::string& text, const std::string& needle,
                        const std::string& replacement, std::string& why) {
  const size_t first = text.find(needle);
  if (first == std::string::npos) {
    why = "required PTX anchor is missing";
    return false;
  }
  if (text.find(needle, first + needle.size()) != std::string::npos) {
    why = "required PTX anchor is ambiguous";
    return false;
  }
  text.replace(first, needle.size(), replacement);
  return true;
}

inline bool MaskPtx(std::string_view input, std::string& code);

// Resolve a real register-array declaration without relying on the producer's
// whitespace, indentation or historical register count. Input is masked code.
inline bool QualityRegisterLimit(std::string_view code, std::string_view type,
                                  std::string_view prefix, unsigned& limit, size_t& end) {
  size_t matches = 0;
  for (size_t at = 0; (at = code.find(".reg", at)) != code.npos; at += 4) {
    size_t cursor = at + 4;
    const auto whitespace = [](char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; };
    if (cursor == code.size() || !whitespace(code[cursor])) continue;
    const auto skip = [&] { while (cursor < code.size() && whitespace(code[cursor])) ++cursor; };
    skip();
    if (code.substr(cursor, type.size()) != type) continue;
    cursor += type.size();
    if (cursor == code.size() || !whitespace(code[cursor])) continue;
    skip();
    if (code.substr(cursor, prefix.size()) != prefix) continue;
    cursor += prefix.size(); skip();
    if (cursor == code.size() || code[cursor++] != '<') continue;
    skip(); unsigned count = 0;
    const auto parsed = std::from_chars(code.data() + cursor, code.data() + code.size(), count);
    if (parsed.ec != std::errc{} || !count) return false;
    cursor = static_cast<size_t>(parsed.ptr - code.data()); skip();
    if (cursor == code.size() || code[cursor++] != '>') return false;
    skip(); if (cursor == code.size() || code[cursor++] != ';') return false;
    // Inserting after the declaration must start a new source statement.
    if (cursor < code.size() && code[cursor] == '\n') ++cursor;
    if (++matches != 1) return false;
    limit = count; end = cursor;
  }
  return matches == 1;
}

// Rewrites consume existing SSA-like PTX registers. Prove declarations and a
// native definition before the insertion, instead of treating a whole-source
// hash as proof of those local facts. Only instruction destinations qualify;
// comments, declarations and mere operand mentions do not.
inline bool LocalOperandsDefined(std::string_view source, size_t before,
                                 std::span<const std::string_view> operands,
                                 std::string& why) {
  std::string code;
  if (!MaskPtx(source, code) || before > code.size()) {
    why = "quality PTX lexical structure is invalid"; return false;
  }
  for (const auto operand : operands) {
    size_t number_at = operand.size();
    while (number_at && operand[number_at - 1] >= '0' && operand[number_at - 1] <= '9') --number_at;
    if (number_at == operand.size()) { why = "quality operand is not a numbered register"; return false; }
    unsigned number = 0;
    auto digits = operand.substr(number_at);
    const auto parsed = std::from_chars(digits.data(), digits.data() + digits.size(), number);
    const auto prefix = operand.substr(0, number_at);
    const auto type = prefix == "%f" ? ".f32" : prefix == "%p" ? ".pred" :
        prefix == "%r" ? ".b32" : prefix == "%rs" ? ".b16" : prefix == "%rd" ? ".b64" : "";
    unsigned limit = 0; size_t declaration_end = 0;
    if (parsed.ec != std::errc{} || !*type ||
        !QualityRegisterLimit(code, type, prefix, limit, declaration_end) ||
        number >= limit || declaration_end > before) {
      why = "quality operand has no unique in-range declaration: " + std::string(operand); return false;
    }
    bool defined = false;
    for (size_t at = declaration_end; at < before;) {
      const auto nl = code.find('\n', at);
      const auto end = std::min(before, nl == code.npos ? code.size() : nl);
      auto line = ptx::Trim(std::string_view(code).substr(at, end - at));
      while (!line.empty() && (line.front() == '{' || line.front() == '}')) line = ptx::Trim(line.substr(1));
      if (line.starts_with("@")) {
        const auto pred = line.find_first_of(" \t");
        if (pred == line.npos) { at = end + 1; continue; }
        line = ptx::Trim(line.substr(pred));
      }
      const auto opcode_end = line.find_first_of(" \t");
      if (opcode_end != line.npos && !line.starts_with(".") && !line.starts_with("st.") &&
          !line.starts_with("bra") && !line.starts_with("ret") && !line.starts_with("call")) {
        auto destination = ptx::Trim(line.substr(opcode_end));
        if (destination.starts_with("{")) {
          const auto close = destination.find('}');
          if (close != destination.npos) destination = destination.substr(1, close - 1);
        } else destination = destination.substr(0, destination.find_first_of(",;"));
        size_t pos = 0;
        while ((pos = destination.find(operand, pos)) != destination.npos) {
          const auto stop = pos + operand.size();
          const auto token = [](char c) { return (c >= '0' && c <= '9') ||
              (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_'; };
          if ((pos == 0 || !token(destination[pos - 1])) &&
              (stop == destination.size() || !token(destination[stop]))) { defined = true; break; }
          pos = stop;
        }
      }
      at = end + 1;
    }
    if (!defined) { why = "quality operand has no native definition before insertion: " +
        std::string(operand); return false; }
  }
  return true;
}

inline std::string FreshRegisterPrefix(std::string_view source, std::string prefix) {
  while (source.find(prefix) != source.npos) prefix += "_r6";
  return prefix;
}

inline void RenameGeneratedPrefix(std::string& text, std::string_view from,
                                  std::string_view to) {
  if (from == to) return;
  for (size_t at = 0; (at = text.find(from, at)) != text.npos;) {
    text.replace(at, from.size(), to); at += to.size();
  }
}

inline bool RewriteValidatedWarpBlend(std::string& ptx, std::string& why) {
  if (ptx.find("MFGUNLOCK_VALIDATED_WARP_BLEND_V1") != std::string::npos) {
    why = "validated-warp PTX is already modified";
    return false;
  }
  constexpr char kInsertion[] = "ld.param.u8 %rs8, [%rd6+220];\n";
  std::string code;
  if (!MaskPtx(ptx, code)) { why = "quality PTX lexical structure is invalid"; return false; }
  const auto insertion = code.find(kInsertion);
  if (insertion == code.npos || code.find(kInsertion, insertion + 1) != code.npos) {
    why = insertion == ptx.npos ? "required PTX anchor is missing" : "required PTX anchor is ambiguous";
    return false;
  }
  constexpr std::string_view operands[] = {
      "%r10", "%r11", "%p16", "%p17", "%f115", "%f116", "%f117", "%f119", "%f120", "%f121",
      "%f123", "%f124", "%f125", "%f126", "%f127", "%f129", "%f130", "%f131", "%f132", "%f133",
      "%f148", "%f149", "%f39", "%f38", "%f37", "%f43", "%f42", "%f41"};
  if (!LocalOperandsDefined(ptx, insertion, operands, why)) return false;
  const auto qv = FreshRegisterPrefix(ptx, "%qv"), qf = FreshRegisterPrefix(ptx, "%qf");
  unsigned predicates = 0; size_t declaration_end = 0;
  if (!QualityRegisterLimit(code, ".pred", "%p", predicates, declaration_end)) {
    why = "quality predicate declaration is ambiguous"; return false;
  }
  constexpr char kProgram[] = R"ptx(// MFGUNLOCK_VALIDATED_WARP_BLEND_V1
cvt.rn.f32.u32 %qf0, %r10;
cvt.rn.f32.u32 %qf1, %r11;
div.approx.ftz.f32 %qf0, 0f3F000000, %qf0;
div.approx.ftz.f32 %qf1, 0f3F000000, %qf1;
sub.ftz.f32 %qf2, 0f3F800000, %qf0;
sub.ftz.f32 %qf3, 0f3F800000, %qf1;
setp.ge.f32 %qv0, %f123, %qf0;
setp.le.f32 %qv2, %f123, %qf2;
and.pred %qv0, %qv0, %qv2;
setp.ge.f32 %qv2, %f124, %qf1;
and.pred %qv0, %qv0, %qv2;
setp.le.f32 %qv2, %f124, %qf3;
and.pred %qv0, %qv0, %qv2;
not.pred %qv2, %p17;
and.pred %qv0, %qv0, %qv2;
setp.ge.f32 %qv1, %f129, %qf0;
setp.le.f32 %qv2, %f129, %qf2;
and.pred %qv1, %qv1, %qv2;
setp.ge.f32 %qv2, %f130, %qf1;
and.pred %qv1, %qv1, %qv2;
setp.le.f32 %qv2, %f130, %qf3;
and.pred %qv1, %qv1, %qv2;
not.pred %qv2, %p16;
and.pred %qv1, %qv1, %qv2;
abs.f32 %qf4, %f125;
abs.f32 %qf5, %f126;
abs.f32 %qf6, %f127;
add.f32 %qf4, %qf4, %qf5;
add.f32 %qf4, %qf4, %qf6;
setp.lt.f32 %qv2, %qf4, 0f7F800000;
and.pred %qv0, %qv0, %qv2;
abs.f32 %qf5, %f131;
abs.f32 %qf6, %f132;
abs.f32 %qf7, %f133;
add.f32 %qf5, %qf5, %qf6;
add.f32 %qf5, %qf5, %qf7;
setp.lt.f32 %qv2, %qf5, 0f7F800000;
and.pred %qv1, %qv1, %qv2;
and.pred %qv3, %qv0, %qv1;
sub.f32 %qf6, %f115, %f119;
sub.f32 %qf7, %f116, %f120;
sub.f32 %qf8, %f117, %f121;
abs.f32 %qf6, %qf6;
abs.f32 %qf7, %qf7;
abs.f32 %qf8, %qf8;
add.f32 %qf6, %qf6, %qf7;
add.f32 %qf6, %qf6, %qf8;
sub.f32 %qf9, %f125, %f131;
sub.f32 %qf10, %f126, %f132;
sub.f32 %qf11, %f127, %f133;
abs.f32 %qf9, %qf9;
abs.f32 %qf10, %qf10;
abs.f32 %qf11, %qf11;
add.f32 %qf9, %qf9, %qf10;
add.f32 %qf9, %qf9, %qf11;
add.f32 %qf10, %qf9, 0f3DA3D70A;
setp.lt.f32 %qv4, %qf10, %qf6;
setp.lt.f32 %qv2, %qf9, 0f3E19999A;
and.pred %qv4, %qv4, %qv2;
and.pred %qv4, %qv4, %qv3;
setp.gt.f32 %qv2, %qf6, 0f3E800000;
setp.ge.f32 %qv5, %f148, 0f3E4CCCCD;
and.pred %qv5, %qv5, %qv2;
or.pred %qv5, %qv5, %qv4;
and.pred %qv0, %qv0, %qv5;
setp.ge.f32 %qv6, %f149, 0f3E4CCCCD;
and.pred %qv6, %qv6, %qv2;
or.pred %qv6, %qv6, %qv4;
and.pred %qv1, %qv1, %qv6;
max.f32 %qf0, %f148, 0f3F59999A;
min.f32 %qf0, %qf0, 0f3F800000;
max.f32 %qf1, %f149, 0f3F59999A;
min.f32 %qf1, %qf1, 0f3F800000;
sub.f32 %qf2, %f125, %f115;
sub.f32 %qf3, %f126, %f116;
sub.f32 %qf4, %f127, %f117;
@%qv0 fma.rn.f32 %f39, %qf0, %qf2, %f115;
@%qv0 fma.rn.f32 %f38, %qf0, %qf3, %f116;
@%qv0 fma.rn.f32 %f37, %qf0, %qf4, %f117;
sub.f32 %qf2, %f131, %f119;
sub.f32 %qf3, %f132, %f120;
sub.f32 %qf4, %f133, %f121;
@%qv1 fma.rn.f32 %f43, %qf1, %qf2, %f119;
@%qv1 fma.rn.f32 %f42, %qf1, %qf3, %f120;
@%qv1 fma.rn.f32 %f41, %qf1, %qf4, %f121;
)ptx";
  std::string result = ptx;
  std::string program = kProgram;
  RenameGeneratedPrefix(program, "%qv", qv); RenameGeneratedPrefix(program, "%qf", qf);
  result.insert(insertion, program);
  result.insert(declaration_end, "\n.reg .pred " + qv + "<7>;\n.reg .f32 " + qf + "<12>;\n");
  ptx.swap(result); return true;
}

inline const ProviderProfile* MatchProvider(const provider::internal::ImageIdentity& identity) {
  for (const auto& profile : kProviderProfiles) {
    if (identity.timestamp == profile.timestamp &&
        identity.image_bytes == profile.image_size) {
      return &profile;
    }
  }
  return nullptr;
}

// Mask comments and strings before resolving semantic entry/header anchors.
// Keep source positions: the returned offsets always address the original text.
inline bool MaskPtx(std::string_view input, std::string& code) {
  code.clear(); code.reserve(input.size());
  bool block = false;
  for (size_t at = 0; at < input.size();) {
    const auto nl = input.find('\n', at);
    const auto end = nl == input.npos ? input.size() : nl;
    std::string line;
    if (!ptx::internal::CodeLine(input.substr(at, end - at), block, line)) return false;
    line.resize(end - at, ' ');
    code += line;
    if (nl != input.npos) code += '\n';
    at = end + (nl != input.npos);
  }
  return !block;
}

inline std::string CompactCode(std::string_view code) {
  std::string compact;
  for (const auto c : code)
    if (c != ' ' && c != '\t' && c != '\r' && c != '\n') compact += c;
  return compact;
}

inline bool TargetDirective(std::string_view code, size_t& offset, size_t& bytes,
                            uint32_t& architecture) {
  size_t matches = 0;
  for (size_t at = 0; at < code.size();) {
    const auto nl = code.find('\n', at);
    const auto end = nl == code.npos ? code.size() : nl;
    auto line = ptx::Trim(code.substr(at, end - at));
    if (line.starts_with(".target") && line.size() > 7 &&
        (line[7] == ' ' || line[7] == '\t')) {
      auto token = ptx::Trim(line.substr(7));
      token = token.substr(0, token.find_first_of(", \t"));
      if (!token.starts_with("sm_") || token.size() <= 3 || ++matches != 1) return false;
      const auto number = token.substr(3);
      const auto parsed = std::from_chars(number.data(), number.data() + number.size(), architecture);
      if (parsed.ec != std::errc{} || parsed.ptr != number.data() + number.size() || architecture == 0)
        return false;
      offset = static_cast<size_t>(token.data() - code.data()); bytes = token.size();
    }
    at = end + 1;
  }
  return matches == 1;
}

inline bool FindPtxEntry(const uint8_t* fatbin, size_t fatbin_size,
                         const PtxProfile& profile, size_t& entry_offset,
                         std::vector<uint8_t>& source, std::string& why) {
  source.clear();
  std::vector<fatbin::Entry> entries;
  size_t end = 0;
  if (!fatbin || !fatbin::Parse({fatbin, fatbin_size}, entries, end) || end != fatbin_size) {
    why = "invalid fatbin header or entry bounds"; return false;
  }
  uint32_t selected_arch = 0;
  size_t matches = 0;
  for (const auto& candidate : entries) {
    if (candidate.kind != kPtxKind) continue;
    std::vector<unsigned char> raw;
    if (!fatbin::DecodePtx({fatbin, fatbin_size}, candidate, raw)) continue;
    const auto nul = std::find(raw.begin(), raw.end(), 0);
    if (nul == raw.end() || nul == raw.begin() ||
        std::any_of(nul, raw.end(), [](unsigned char c) { return c != 0; })) continue;
    std::string text(raw.begin(), nul);
    std::erase(text, '\r');
    std::string code;
    if (!MaskPtx(text, code)) continue;
    const auto compact = CompactCode(code);
    const std::string entry = std::string(".entry") + profile.entry_name + "(";
    const auto start = compact.find(entry);
    if (start == compact.npos || compact.find(entry, start + entry.size()) != compact.npos) continue;
    // A role source preserves its own parameter ABI. A local rewrite proves
    // its actual parameter accesses/operands separately; register numbering or
    // historical parameter-buffer size is never a whole-kernel admission rule.
    size_t target_at = 0, target_bytes = 0;
    uint32_t source_arch = 0;
    if (!TargetDirective(code, target_at, target_bytes, source_arch) || source_arch != candidate.architecture)
      continue;
    // Prefer the most capable actual source for this role. Duplicate same-SM
    // implementations remain ambiguous; older sources are not refused by ID.
    if (source_arch < selected_arch) continue;
    if (source_arch > selected_arch) { selected_arch = source_arch; matches = 0; }
    ++matches;
    entry_offset = candidate.offset;
    source.assign(text.begin(), text.end());
  }
  if (matches != 1) {
    why = matches ? "PTX role has multiple same-target implementations" : "PTX role/parameter contract not resolved";
    source.clear(); return false;
  }
  why.clear(); return true;
}

inline bool ValidatePristineLayout(const uint8_t* bytes, size_t count,
                                   size_t target_entry, std::string& why) {
  std::vector<fatbin::Entry> entries;
  size_t end = 0;
  if (!bytes || !fatbin::Parse({bytes, count}, entries, end) || end != count ||
      std::count_if(entries.begin(), entries.end(), [&](const auto& entry) {
        return entry.offset == target_entry && entry.kind == kPtxKind;
      }) != 1) {
    why = "selected PTX entry is not uniquely bounded by its container";
    return false;
  }
  return true;
}

using RewritePtxCallback = bool (*)(std::string&, std::string&);

inline bool BuildRetargetedFatbin(const uint8_t* bytes, size_t count,
                                   const PtxProfile& profile, RewritePtxCallback rewrite,
                                   std::vector<uint8_t>& rebuilt,
                                   std::string& why, uint32_t target_sm = kAmpereArch) {
  rebuilt.clear();
  if (target_sm == 0) { why = "quality PTX target is unknown"; return false; }
  size_t selected = 0;
  std::vector<uint8_t> source;
  if (!FindPtxEntry(bytes, count, profile, selected, source, why) ||
      !ValidatePristineLayout(bytes, count, selected, why)) return false;
  std::string text(source.begin(), source.end());
  if (rewrite && !rewrite(text, why)) return false;
  if (target_sm == 75) {
    ptx::Sm75LoweringCounts actual;
    // The transformer is transactional. A valid spelling it cannot rewrite
    // remains intact for an empirical JIT attempt; no count/hash admission.
    if (!ptx::LowerSm75Structurally(text, actual, why)) why.clear();
  }
  std::string code;
  size_t target_at = 0, target_bytes = 0;
  uint32_t source_arch = 0;
  if (!MaskPtx(text, code) || !TargetDirective(code, target_at, target_bytes, source_arch)) {
    why = "quality PTX has no unique target directive"; return false;
  }
  text.replace(target_at, target_bytes, "sm_" + std::to_string(target_sm));
  if (text.size() + 1 > fatbin::kMaxPtxBytes) { why = "replacement PTX exceeds storage budget"; return false; }

  std::vector<fatbin::Entry> entries;
  size_t end = 0;
  if (!fatbin::Parse({bytes, count}, entries, end)) { why = "fatbin changed during rebuild"; return false; }
  rebuilt.assign(bytes, bytes + kOuterHeader);
  for (const auto& entry : entries) {
    if (entry.offset != selected) {
      // Preserve other architectures verbatim. A same/older-SM PTX or cubin
      // would bypass the requested replacement, so it is deliberately hidden.
      if (entry.architecture <= target_sm) continue;
      rebuilt.insert(rebuilt.end(), bytes + entry.offset, bytes + entry.End());
      continue;
    }
    const auto at = rebuilt.size();
    const size_t padded = (text.size() + 1 + 7) & ~size_t{7};
    rebuilt.insert(rebuilt.end(), bytes + entry.offset, bytes + entry.PayloadOffset());
    rebuilt.resize(at + entry.header_bytes + padded, 0);
    std::memcpy(rebuilt.data() + at + entry.header_bytes, text.data(), text.size());
    const uint64_t stored = padded, zero64 = 0;
    const uint32_t zero32 = 0;
    const uint64_t flags = entry.flags & ~uint64_t{0x2000};
    std::memcpy(rebuilt.data() + at + 8, &stored, 8);
    std::memcpy(rebuilt.data() + at + 16, &zero32, 4);
    std::memcpy(rebuilt.data() + at + 28, &target_sm, 4);
    std::memcpy(rebuilt.data() + at + 40, &flags, 8);
    std::memcpy(rebuilt.data() + at + 56, &zero64, 8);
  }
  const uint64_t outer = rebuilt.size() - kOuterHeader;
  std::memcpy(rebuilt.data() + 8, &outer, 8);
  std::vector<fatbin::Entry> verified;
  size_t verified_end = 0;
  if (!fatbin::Parse(rebuilt, verified, verified_end) || verified_end != rebuilt.size() ||
      std::count_if(verified.begin(), verified.end(), [&](const auto& entry) {
        return entry.architecture == target_sm && entry.kind == kPtxKind;
      }) != 1) {
    rebuilt.clear(); why = "replacement fatbin selection verification failed"; return false;
  }
  why.clear(); return true;
}

struct LocatedFatbin {
  const uint8_t* address = nullptr;
  size_t size = 0;
};

inline bool BuildRedirectControlFatbin(const LocatedFatbin& located,
                                       std::vector<uint8_t>& rebuilt,
                                       std::string& why,
                                       uint32_t target_sm = kAmpereArch) {
  if (located.address == nullptr || located.size < kOuterHeader ||
      ReadU32(located.address) != kFatbinMagic ||
      ReadU16(located.address + 6) != kOuterHeader) {
    why = "prepared control fatbin header is invalid";
    return false;
  }
  const uint64_t declared = ReadU64(located.address + 8);
  if (declared == 0 || declared > located.size - kOuterHeader) {
    why = "prepared control fatbin size is invalid";
    return false;
  }
  const size_t size = static_cast<size_t>(declared) + kOuterHeader;
  std::vector<fatbin::Entry> entries;
  size_t end = 0;
  if (!fatbin::Parse(std::span<const unsigned char>(located.address, size), entries, end) ||
      end != size) {
    why = "prepared control fatbin has invalid entry bounds";
    return false;
  }
  (void)target_sm;
  // Match the provider's physical backing span as well as its visible header.
  // The normal retargeted path hides the trailing sm_89 cubin by shortening
  // the outer header; keeping those bytes makes pointer relocation the only change.
  rebuilt.assign(located.address, located.address + located.size);
  return true;
}

inline std::string DescribeFatbin(const std::vector<uint8_t>& bytes) {
  if (bytes.size() < kOuterHeader || ReadU32(bytes.data()) != kFatbinMagic)
    return "entries=invalid";
  const uint64_t declared = ReadU64(bytes.data() + 8);
  if (declared > bytes.size() - kOuterHeader) return "entries=invalid";
  const size_t visible = static_cast<size_t>(declared) + kOuterHeader;
  std::vector<fatbin::Entry> entries;
  size_t end = 0;
  if (!fatbin::Parse(std::span<const unsigned char>(bytes.data(), visible), entries, end) ||
      end != visible) {
    return "entries=invalid";
  }
  std::ostringstream stream;
  stream << "visible=" << visible << '/' << bytes.size() << "; entries=[";
  for (size_t index = 0; index < entries.size(); ++index) {
    if (index != 0) stream << ',';
    stream << (entries[index].kind == kPtxKind ? "ptx" : "cubin")
           << "/sm_" << entries[index].architecture
           << '/' << entries[index].payload_bytes;
  }
  stream << ']';
  return stream.str();
}

inline bool LocateUniqueFatbin(HMODULE module, const PtxProfile& profile,
                               LocatedFatbin& located, std::string& why) {
  provider::internal::Image image;
  if (!provider::internal::IsImageMapping(module) ||
      !provider::internal::InspectImage(module, image)) {
    why = "PTX source image is unreadable or structurally invalid"; located = {}; return false;
  }
  auto* base = image.base;
  const size_t image_size = image.bytes;
  size_t matches = 0;
  for (const auto& section : image.sections) {
    if ((section.Characteristics & IMAGE_SCN_MEM_READ) == 0 ||
        (section.Characteristics & IMAGE_SCN_MEM_EXECUTE) != 0 ||
        section.VirtualAddress >= image_size) {
      continue;
    }
    const size_t section_size = std::min<size_t>(
        section.Misc.VirtualSize, image_size - section.VirtualAddress);
    const uint8_t* bytes = base + section.VirtualAddress;
    if (!provider::internal::IsReadable(bytes, section_size, module)) continue;
    for (size_t offset = 0; offset + kOuterHeader <= section_size; ++offset) {
      if (ReadU32(bytes + offset) != kFatbinMagic ||
          ReadU16(bytes + offset + 6) != kOuterHeader) {
        continue;
      }
      const uint64_t declared = ReadU64(bytes + offset + 8);
      if (declared == 0 || declared > kMaxFatbinSize ||
          declared > section_size - offset - kOuterHeader) {
        continue;
      }
      const size_t total = static_cast<size_t>(declared) + kOuterHeader;
      size_t ignored_entry = 0;
      std::vector<uint8_t> ignored_ptx;
      std::string ignored_reason;
      if (!FindPtxEntry(bytes + offset, total, profile, ignored_entry,
                        ignored_ptx, ignored_reason)) {
        continue;
      }
      located = {bytes + offset, total};
      ++matches;
      offset += total - 1;
    }
  }
  if (matches != 1) {
    std::ostringstream stream;
    stream << "found " << matches << " structurally matching PTX fatbins, expected one";
    why = stream.str();
    located = {};
    return false;
  }
  return true;
}

using PrepareProviderCallback = bool (*)(HMODULE);

struct PreparedRedirect {
  HMODULE module = nullptr;
  LocatedFatbin located;
  std::vector<uint64_t*> slots;
  std::vector<uint8_t> rebuilt;
  uint64_t expected = 0;
  uint64_t source_hash = 0;
  size_t source_rva = 0;
};

inline bool PrepareRedirectSource(HMODULE module, const PtxProfile& profile,
                                  PreparedRedirect& prepared, std::string& detail) {
  prepared = {};
  prepared.module = module;
  if (!LocateUniqueFatbin(module, profile, prepared.located, detail)) return false;

  provider::internal::Image image;
  if (!provider::internal::InspectImage(module, image)) {
    detail = "descriptor image changed during source resolution"; prepared = {}; return false;
  }
  auto* base = image.base;
  const size_t image_size = image.bytes;
  prepared.expected = reinterpret_cast<uint64_t>(prepared.located.address);
  prepared.source_hash = Fnv1a64(prepared.located.address, prepared.located.size);
  prepared.source_rva = static_cast<size_t>(prepared.located.address - base);

  for (const auto& section : image.sections) {
    if ((section.Characteristics & IMAGE_SCN_MEM_READ) == 0 ||
        (section.Characteristics & IMAGE_SCN_MEM_EXECUTE) != 0 ||
        section.VirtualAddress >= image_size) {
      continue;
    }
    const size_t section_size = std::min<size_t>(
        section.Misc.VirtualSize, image_size - section.VirtualAddress);
    uint8_t* bytes = base + section.VirtualAddress;
    if (!provider::internal::IsReadable(bytes, section_size, module)) continue;
    for (size_t offset = 0; offset + sizeof(uint64_t) <= section_size;
         offset += alignof(uint64_t)) {
      uint64_t value = 0;
      std::memcpy(&value, bytes + offset, sizeof(value));
      if (value == prepared.expected)
        prepared.slots.push_back(reinterpret_cast<uint64_t*>(bytes + offset));
    }
  }
  if (prepared.slots.empty()) {
    std::ostringstream stream;
    stream << "no materialized descriptor reference for resolved PTX payload";
    detail = stream.str();
    prepared = {};
    return false;
  }
  return true;
}

inline bool PrepareRetargetedRedirect(HMODULE module, const PtxProfile& profile,
                                      RewritePtxCallback rewrite,
                                      PreparedRedirect& prepared,
                                      std::string& detail, uint32_t target_sm = kAmpereArch) {
  if (!PrepareRedirectSource(module, profile, prepared, detail)) return false;
  if (!BuildRetargetedFatbin(prepared.located.address, prepared.located.size,
                             profile, rewrite, prepared.rebuilt, detail, target_sm)) {
    prepared = {};
    return false;
  }
  return true;
}

inline bool CommitPreparedRedirect(PreparedRedirect& prepared,
                                   const std::string& label, Redirect& redirect,
                                   std::string& detail) {
  if (prepared.module == nullptr || prepared.located.address == nullptr ||
      prepared.rebuilt.empty() || prepared.slots.empty()) {
    detail = "prepared redirect is incomplete";
    return false;
  }

  redirect.descriptors.reserve(prepared.slots.size());
  void* allocation = VirtualAlloc(nullptr, prepared.rebuilt.size(),
                                  MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
  if (allocation == nullptr) {
    detail = "replacement fatbin allocation failed";
    return false;
  }
  std::memcpy(allocation, prepared.rebuilt.data(), prepared.rebuilt.size());

  const uint64_t replacement = reinterpret_cast<uint64_t>(allocation);
  bool committed = true;
  for (uint64_t* slot : prepared.slots) {
    if (*slot != prepared.expected) {
      committed = false;
      break;
    }
    DWORD old_protection = 0;
    if (!VirtualProtect(slot, sizeof(uint64_t), PAGE_READWRITE, &old_protection)) {
      committed = false;
      break;
    }
    redirect.descriptors.push_back({slot, *slot, old_protection});
    *slot = replacement;
    DWORD ignored = 0;
    if (!VirtualProtect(slot, sizeof(uint64_t), old_protection, &ignored) ||
        *slot != replacement) {
      committed = false;
      break;
    }
  }
  if (!committed || redirect.descriptors.size() != prepared.slots.size()) {
    bool rolled_back = true;
    for (auto patch = redirect.descriptors.rbegin();
         patch != redirect.descriptors.rend(); ++patch) {
      DWORD old_protection = 0;
      if (!VirtualProtect(patch->slot, sizeof(uint64_t), PAGE_READWRITE, &old_protection)) {
        rolled_back = false;
        continue;
      }
      *patch->slot = patch->original;
      DWORD ignored = 0;
      if (!VirtualProtect(patch->slot, sizeof(uint64_t), patch->protection, &ignored) ||
          *patch->slot != patch->original) {
        rolled_back = false;
      }
    }
    if (rolled_back) {
      redirect.descriptors.clear();
      VirtualFree(allocation, 0, MEM_RELEASE);
    } else {
      redirect.allocation = allocation;
    }
    detail = rolled_back
        ? "descriptor redirect transaction failed"
        : "descriptor redirect transaction failed; replacement retained after incomplete rollback";
    return false;
  }

  redirect.allocation = allocation;
  auto* base = reinterpret_cast<uint8_t*>(prepared.module);
  std::ostringstream stream;
  stream << label
         << "; source-rva=0x" << std::hex << prepared.source_rva
         << "; source-hash=0x" << prepared.source_hash
         << "; replacement-hash=0x" << Fnv1a64(prepared.rebuilt.data(), prepared.rebuilt.size())
         << std::dec << "; redirected " << redirect.descriptors.size()
         << " descriptor(s), " << prepared.located.size << " -> " << prepared.rebuilt.size()
         << " bytes; " << DescribeFatbin(prepared.rebuilt) << "; descriptor-rva=[";
  for (size_t index = 0; index < prepared.slots.size(); ++index) {
    if (index != 0) stream << ',';
    stream << "0x" << std::hex
           << static_cast<size_t>(reinterpret_cast<uint8_t*>(prepared.slots[index]) - base);
  }
  stream << ']';
  detail = stream.str();
  return true;
}

inline bool RedirectFatbin(HMODULE module, const PtxProfile& profile, Mode mode,
                           PrepareProviderCallback prepare_provider, Redirect& redirect,
                           std::string& detail, uint32_t target_sm = kAmpereArch) {
  PreparedRedirect prepared;
  if (!PrepareRedirectSource(module, profile, prepared, detail)) return false;

  if (mode == Mode::kBlackwellBaseline || mode == Mode::kValidatedWarp) {
    const RewritePtxCallback rewrite =
        mode == Mode::kValidatedWarp ? RewriteValidatedWarpBlend : nullptr;
    if (!BuildRetargetedFatbin(prepared.located.address, prepared.located.size,
                               profile, rewrite, prepared.rebuilt, detail, target_sm)) {
      return false;
    }
  } else if (mode != Mode::kRedirectControl) {
    detail = "unknown warp diagnostic mode";
    return false;
  }

  // Provider preparation must complete before replacement pointers are
  // published. Redirect-control intentionally copies the resulting normal
  // retargeted fatbin byte-for-byte, isolating the redirect/allocation mechanism.
  if (prepare_provider != nullptr && !prepare_provider(module)) {
    detail = "provider preparation failed before warp redirect";
    return false;
  }
  if (mode == Mode::kRedirectControl &&
      !BuildRedirectControlFatbin(prepared.located, prepared.rebuilt, detail, target_sm)) {
    return false;
  }

  return CommitPreparedRedirect(
      prepared, std::string("mode=") + ModeLabel(mode, target_sm), redirect, detail);
}

}  // namespace internal

inline bool IsSupportedProvider(HMODULE module, std::string& version,
                                std::string& why) {
  provider::internal::ImageIdentity identity;
  provider::internal::Image image;
  if (!provider::internal::ReadIdentity(module, identity) ||
      !provider::internal::InspectImage(module, image)) {
    why = "provider is not a readable x64 image";
    return false;
  }
  const auto* profile = internal::MatchProvider(identity);
  // The image identity is diagnostic only. LocateUniqueFatbin and the
  // descriptor/kernel checks below decide whether this operation can run.
  version = profile ? profile->version : "unlisted image (structural trial)";
  return true;
}

inline bool Restore(std::vector<Redirect>& redirects) {
  bool restored_all = true;
  for (auto redirect = redirects.rbegin(); redirect != redirects.rend(); ++redirect) {
    bool restored = true;
    for (auto patch = redirect->descriptors.rbegin();
         patch != redirect->descriptors.rend(); ++patch) {
      DWORD old_protection = 0;
      if (!VirtualProtect(patch->slot, sizeof(uint64_t), PAGE_READWRITE, &old_protection)) {
        restored = false;
        continue;
      }
      *patch->slot = patch->original;
      DWORD ignored = 0;
      const DWORD protection = patch->protection != 0 ? patch->protection : old_protection;
      if (!VirtualProtect(patch->slot, sizeof(uint64_t), protection, &ignored) ||
          *patch->slot != patch->original) {
        restored = false;
      }
    }
    // Keep rollback metadata and storage until every descriptor is restored.
    if (restored && redirect->allocation != nullptr) {
      restored = VirtualFree(redirect->allocation, 0, MEM_RELEASE) != FALSE;
      if (restored) redirect->allocation = nullptr;
    }
    if (restored) redirect->descriptors.clear();
    restored_all = restored_all && restored;
  }
  std::erase_if(redirects, [](const Redirect& redirect) {
    return redirect.allocation == nullptr && redirect.descriptors.empty();
  });
  return restored_all;
}

inline bool Apply(HMODULE module, std::vector<Redirect>& redirects, Result& result,
                  std::string& provider_version, Mode mode = Mode::kValidatedWarp,
                  internal::PrepareProviderCallback prepare_provider = nullptr,
                  uint32_t target_sm = internal::kAmpereArch) {
  redirects.clear();
  result = {};

  if (!IsSupportedProvider(module, provider_version, result.detail)) return false;

  internal::LocatedFatbin located;
  std::string detection_reason;
  result.detected = internal::LocateUniqueFatbin(
      module, internal::kPtxProfile, located, detection_reason);
  if (!result.detected) {
    result.detail = detection_reason;
    return false;
  }

  Redirect redirect;
  if (!internal::RedirectFatbin(module, internal::kPtxProfile, mode, prepare_provider,
                                redirect, result.detail, target_sm)) {
    if (redirect.allocation != nullptr) redirects.push_back(std::move(redirect));
    return false;
  }
  result.applied = true;
  redirects.push_back(std::move(redirect));
  return true;
}

}  // namespace mfgunlock::validatedwarp
