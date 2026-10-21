// SPDX-License-Identifier: MIT
#pragma once

// Shared portable implementation used by both the runtime and corpus probe.
// Descriptor roles and the 144-byte parameter ABI identify the operation; PTX
// byte count, compiler register banks, labels and compression are not profiles.
#include <algorithm>
#include <charconv>
#include <string>
#include <string_view>
#include <vector>

#include "./fatbin.hpp"
#include "./ptx_compat.hpp"

namespace mfgunlock::midpoint::internal {

inline constexpr uint32_t kFatbinMagic = fatbin::kMagic;
inline constexpr size_t kOuterHeader = fatbin::kHeaderBytes;
inline constexpr uint32_t kPtxKind = 1;
inline constexpr uint64_t kUncompressedFlags = 0x41;

// These are descriptor field roles, not provider releases. The first layout
// names its entry at slot+16 and role at slot+40; the second names its role at
// slot-8 and entry at slot+16. An arbitrary nearby pointer is not this ABI.
struct TemporalProfile {
  const char* entry_name;
  const char* descriptor_name;
  ptrdiff_t entry_name_offset;
  ptrdiff_t descriptor_name_offset;
};
inline constexpr TemporalProfile kTemporalProfiles[] = {
    {"main_kernel", "dlfg_kernel", 0x10, 0x28},
    {"Kernel_EstimateIntermMvecsScatter", "EstimateIntermMvecsScatter", 0x10, -0x08},
};
inline constexpr size_t kExpectedMidpoints = 104;
inline constexpr size_t kTemporalParameterBytes = 144;
inline constexpr size_t kTemporalParameterOffset = 32;
inline constexpr std::string_view kMidpointBits = "0f3F000000";
using fatbin::ReadU16;
using fatbin::ReadU32;
using fatbin::ReadU64;
using fatbin::Lz4BlockDecompress;

struct PtxToken {
  size_t at = 0;
  size_t bytes = 0;
};

inline bool SymbolChar(char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
      (c >= '0' && c <= '9') || c == '_' || c == '$' || c == '.' || c == '%';
}

inline bool MaskTemporalPtx(std::string_view source, std::string& code) {
  code.clear(); code.reserve(source.size());
  bool block = false;
  for (size_t at = 0; at < source.size();) {
    const auto nl = source.find('\n', at);
    const auto end = nl == source.npos ? source.size() : nl;
    std::string line;
    if (!ptx::internal::CodeLine(source.substr(at, end - at), block, line)) return false;
    line.resize(end - at, ' ');
    code += line;
    if (nl != source.npos) code += '\n';
    at = end + (nl != source.npos);
  }
  return !block;
}

inline std::vector<PtxToken> TemporalTokens(std::string_view code) {
  std::vector<PtxToken> result;
  for (size_t at = 0; at < code.size();) {
    if (code[at] == ' ' || code[at] == '\t' || code[at] == '\r' || code[at] == '\n') {
      ++at; continue;
    }
    const size_t begin = at++;
    if (SymbolChar(code[begin]))
      while (at < code.size() && SymbolChar(code[at])) ++at;
    result.push_back({begin, at - begin});
  }
  return result;
}

inline bool Decimal(std::string_view token, uint32_t& value) {
  const auto parsed = std::from_chars(token.data(), token.data() + token.size(), value);
  return !token.empty() && parsed.ec == std::errc{} && parsed.ptr == token.data() + token.size();
}

inline std::string FreshTemporalPrefix(std::string_view source) {
  // Inventory candidate namespaces once instead of repeatedly searching an
  // ever-longer name. The first unused index is bounded by the inventory size.
  constexpr std::string_view base = "%mfg_mid_";
  std::vector<size_t> used;
  for (size_t at = 0; (at = source.find(base, at)) != source.npos;) {
    at += base.size();
    size_t index = 0;
    const auto parsed = std::from_chars(source.data() + at, source.data() + source.size(), index);
    if (parsed.ec == std::errc{} && parsed.ptr != source.data() + at) used.push_back(index);
  }
  std::sort(used.begin(), used.end());
  size_t index = 0;
  for (size_t value : used) {
    if (value > index) break;
    if (value == index) ++index;
  }
  return std::string(base) + std::to_string(index);
}

struct TemporalProgram {
  const TemporalProfile* profile = nullptr;
  fatbin::Entry entry{};
  std::string source;
  std::string parameter;
  size_t declaration_at = 0;
  size_t initialization_at = 0;
  std::vector<PtxToken> weights;
};

inline bool InspectTemporalProgram(std::string_view source, uint32_t architecture,
                                   const TemporalProfile& profile, TemporalProgram& program) {
  std::string code;
  if (!MaskTemporalPtx(source, code)) return false;
  const auto tokens = TemporalTokens(code);
  const auto token = [&](size_t i) -> std::string_view {
    return i < tokens.size() ? std::string_view(code).substr(tokens[i].at, tokens[i].bytes)
                             : std::string_view{};
  };
  // Header architecture must describe this actual PTX, not an unrelated image
  // preceding it in the container. No SM/version allowlist is involved.
  size_t targets = 0;
  for (size_t i = 0; i < tokens.size(); ++i) {
    if (token(i) != ".target") continue;
    const auto sm = token(i + 1);
    if (!sm.starts_with("sm_") || ++targets != 1) return false;
    const auto numeric_end = sm.find_first_not_of("0123456789", 3);
    const auto digits = sm.substr(3, numeric_end == sm.npos ? sm.size() - 3 : numeric_end - 3);
    uint32_t actual = 0;
    if (!Decimal(digits, actual) || actual != architecture) return false;
  }
  if (targets != 1) return false;

  size_t match = tokens.size();
  for (size_t i = 0; i + 2 < tokens.size(); ++i) {
    if (token(i) != ".entry" || token(i + 1) != profile.entry_name) continue;
    if (match != tokens.size() || token(i + 2) != "(") return false;
    match = i;
  }
  if (match == tokens.size()) return false;

  // The temporal scalar is field+32 of this known parameter ABI. Derive the
  // parameter symbol from the declaration instead of the compiler's naming.
  size_t at = match + 3;
  if (token(at++) != ".param" || token(at++) != ".align") return false;
  uint32_t alignment = 0;
  if (!Decimal(token(at++), alignment) || alignment < 4 || (alignment & (alignment - 1))) return false;
  if (token(at++) != ".b8") return false;
  const auto parameter = token(at++);
  if (parameter.empty() || parameter.front() == '%' || !SymbolChar(parameter.front())) return false;
  uint32_t parameter_bytes = 0;
  if (token(at++) != "[" || !Decimal(token(at++), parameter_bytes) ||
      parameter_bytes != kTemporalParameterBytes || token(at++) != "]" ||
      token(at++) != ")") return false;
  // Entry attributes may follow the parameter list. They do not change field
  // layout; do not scan through another function/statement to find a body.
  while (at < tokens.size() && token(at) != "{") {
    if (token(at) == ";" || token(at) == ".entry" || token(at) == ".func") return false;
    ++at;
  }
  if (at == tokens.size()) return false;
  const size_t body = at++;
  size_t depth = 1, close = tokens.size();
  for (size_t i = at; i < tokens.size(); ++i) {
    if (token(i) == "{") ++depth;
    else if (token(i) == "}" && --depth == 0) { close = i; break; }
  }
  if (close == tokens.size()) return false;

  // Put fresh declarations first, initialization after existing declarations
  // and before the first executable token/label. Every entry path then defines
  // t and (1-t); no compiler-specific join label or old register is borrowed.
  size_t first_code = body + 1;
  while (first_code < close) {
    const auto name = token(first_code);
    const bool declaration = name == ".reg" || name == ".local" || name == ".shared" ||
        name == ".const" || name == ".param";
    if (declaration) {
      size_t initializer_depth = 0;
      while (first_code < close && token(first_code) != ";") {
        if (token(first_code) == "{") ++initializer_depth;
        else if (token(first_code) == "}") {
          if (!initializer_depth) return false;
          --initializer_depth;
        }
        ++first_code;
      }
      if (first_code == close || initializer_depth != 0) return false;
      ++first_code;
    } else if (name == ".loc" || name == ".pragma") {
      const auto nl = code.find('\n', tokens[first_code].at);
      const size_t end = nl == code.npos ? code.size() : nl;
      while (first_code < close && tokens[first_code].at < end) ++first_code;
    } else break;
  }
  if (first_code == close) return false;

  std::vector<PtxToken> weights;
  for (size_t i = body + 1; i + 6 < close; ++i) {
    if (token(i) != "mul.ftz.f32" || !token(i + 1).starts_with("%") ||
        token(i + 2) != "," || token(i + 4) != "," || token(i + 6) != ";") continue;
    const bool right = token(i + 5) == kMidpointBits && token(i + 3).starts_with("%");
    const bool left = token(i + 3) == kMidpointBits && token(i + 5).starts_with("%");
    if (right != left) weights.push_back(tokens[i + (right ? 5 : 3)]);
  }
  // Retain the previously established two 52-site direction groups. A new
  // count/order/parameter layout needs a new local algorithm proof; it is not
  // guessed from half literals elsewhere in an unfamiliar program.
  if (weights.size() != kExpectedMidpoints) return false;
  program.profile = &profile;
  program.parameter = std::string(parameter);
  program.declaration_at = tokens[body].at + 1;
  program.initialization_at = tokens[first_code].at;
  program.weights = std::move(weights);
  return true;
}

inline bool FindTemporalProgram(const uint8_t* fat, size_t bytes,
                                const TemporalProfile* required,
                                TemporalProgram& program, std::string& why) {
  program = {};
  std::vector<fatbin::Entry> entries;
  size_t end = 0;
  if (!fat || !fatbin::Parse({fat, bytes}, entries, end) || end != bytes) {
    why = "invalid temporal fatbin bounds"; return false;
  }
  bool found = false;
  for (const auto& entry : entries) {
    if (entry.kind != kPtxKind) continue;
    std::vector<unsigned char> raw;
    if (!fatbin::DecodePtx({fat, bytes}, entry, raw)) continue;
    const auto nul = std::find(raw.begin(), raw.end(), 0);
    if (!std::all_of(nul, raw.end(), [](unsigned char value) { return value == 0; })) continue;
    const std::string source(raw.begin(), nul);
    for (const auto& profile : kTemporalProfiles) {
      if (required && (std::string_view(required->entry_name) != profile.entry_name ||
                      std::string_view(required->descriptor_name) != profile.descriptor_name)) continue;
      TemporalProgram candidate;
      if (!InspectTemporalProgram(source, entry.architecture, profile, candidate)) continue;
      if (found) { program = {}; why = "multiple temporal PTX operations need an unambiguous target"; return false; }
      candidate.entry = entry;
      candidate.source = source;
      program = std::move(candidate);
      found = true;
    }
  }
  why = found ? "" : "temporal role/parameter ABI/two 52-site direction groups not resolved";
  return found;
}

inline const TemporalProfile* FindTemporalProfile(const uint8_t* fat, size_t bytes) {
  TemporalProgram program;
  std::string why;
  return FindTemporalProgram(fat, bytes, nullptr, program, why) ? program.profile : nullptr;
}

inline bool FindCompatiblePtxEntry(const uint8_t* fat, size_t bytes, size_t& entry_offset) {
  TemporalProgram program;
  std::string why;
  if (!FindTemporalProgram(fat, bytes, nullptr, program, why)) return false;
  entry_offset = program.entry.offset;
  return true;
}

inline bool BuildTemporalFatbin(const uint8_t* fat, size_t bytes,
                                const TemporalProfile& profile,
                                std::vector<uint8_t>& out, std::string& why) {
  TemporalProgram program;
  if (!FindTemporalProgram(fat, bytes, &profile, program, why)) { out.clear(); return false; }
  const std::string prefix = FreshTemporalPrefix(program.source);
  const std::string t = prefix + "_t", one = prefix + "_one", inverse = prefix + "_inverse";
  const char* nl = program.source.find("\r\n") != std::string::npos ? "\r\n" : "\n";
  const std::string declarations = std::string(nl) + "// MFGUNLOCK_MIDPOINT_R6" + nl +
      ".reg .f32 " + t + ", " + one + ", " + inverse + ";" + nl;
  const std::string initialize = "ld.param.f32 " + t + ", [" + program.parameter + "+32];" + nl +
      "mov.f32 " + one + ", 0f3F800000;" + nl +
      "sub.ftz.f32 " + inverse + ", " + one + ", " + t + ";" + nl;
  struct Edit { size_t at, bytes; std::string text; };
  std::vector<Edit> edits{{program.declaration_at, 0, declarations},
                        {program.initialization_at, 0, initialize}};
  for (size_t i = 0; i < program.weights.size(); ++i)
    edits.push_back({program.weights[i].at, program.weights[i].bytes,
                     i < kExpectedMidpoints / 2 ? inverse : t});
  std::stable_sort(edits.begin(), edits.end(), [](const auto& a, const auto& b) { return a.at < b.at; });
  std::string patched;
  size_t copied = 0;
  for (const auto& edit : edits) {
    if (edit.at < copied || edit.at > program.source.size() || edit.bytes > program.source.size() - edit.at) {
      why = "overlapping temporal PTX edit"; out.clear(); return false;
    }
    patched.append(program.source, copied, edit.at - copied);
    patched += edit.text;
    copied = edit.at + edit.bytes;
  }
  patched.append(program.source, copied);
  patched.push_back('\0');
  if (patched.size() > fatbin::kMaxPtxBytes) { why = "temporal PTX size bound"; out.clear(); return false; }

  std::vector<fatbin::Entry> entries;
  size_t end = 0;
  if (!fatbin::Parse({fat, bytes}, entries, end)) { why = "temporal container changed"; out.clear(); return false; }
  std::vector<uint8_t> rebuilt(fat, fat + kOuterHeader);
  for (const auto& entry : entries) {
    // The precompiled alternatives would bypass the edited PTX. Preserve all
    // other PTX modules byte-for-byte while forcing the native JIT path.
    if (entry.kind != kPtxKind) continue;
    if (entry.offset != program.entry.offset) {
      rebuilt.insert(rebuilt.end(), fat + entry.offset, fat + entry.End());
      continue;
    }
    const size_t at = rebuilt.size(), padded = (patched.size() + 7) & ~size_t{7};
    rebuilt.insert(rebuilt.end(), fat + entry.offset, fat + entry.PayloadOffset());
    rebuilt.resize(at + entry.header_bytes + padded, 0);
    std::memcpy(rebuilt.data() + at + entry.header_bytes, patched.data(), patched.size());
    const uint64_t payload = padded, flags = entry.flags & ~uint64_t{0x2000}, zero64 = 0;
    const uint32_t zero32 = 0;
    std::memcpy(rebuilt.data() + at + 8, &payload, sizeof(payload));
    std::memcpy(rebuilt.data() + at + 16, &zero32, sizeof(zero32));
    std::memcpy(rebuilt.data() + at + 40, &flags, sizeof(flags));
    std::memcpy(rebuilt.data() + at + 56, &zero64, sizeof(zero64));
  }
  if (rebuilt.size() > fatbin::kMaxFatbinBytes) { why = "temporal container size bound"; out.clear(); return false; }
  const uint64_t payload = rebuilt.size() - kOuterHeader;
  std::memcpy(rebuilt.data() + 8, &payload, sizeof(payload));
  std::vector<fatbin::Entry> check;
  if (!fatbin::Parse(rebuilt, check, end) || end != rebuilt.size()) {
    why = "rebuilt temporal container failed validation"; out.clear(); return false;
  }
  out = std::move(rebuilt);
  why.clear();
  return true;
}

}  // namespace mfgunlock::midpoint::internal
