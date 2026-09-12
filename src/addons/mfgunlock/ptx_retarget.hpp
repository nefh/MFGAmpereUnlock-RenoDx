/*
 * PTX retargeting for known DLSS-G fatbin layouts.
 * SPDX-License-Identifier: MIT
 *
 * The target change is intentionally narrow: only a verified `.target sm_89`
 * directive is rewritten to the selected SM, and only when the changed LZ4
 * bytes are independent literals. A second decode verifies that no other byte
 * changed.
 */
#pragma once

#include <algorithm>
#include <string>
#include <string_view>

#include "./architecture.hpp"
#include "./fatbin.hpp"

namespace mfgunlock::ampere::ptx {

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
      if (version.size() != 3 || version[0] != '8' || version[1] != '.' ||
          version[2] < '0' || version[2] > '7' || ++versions != 1) {
        return false;
      }
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
    // PTX features introduced with Ampere cannot be enabled by changing .target.
    constexpr std::string_view kSm80Instructions[] = {
        "cp.async", "mbarrier.", "redux.sync", "mma.sp.", ".bf16", ".tf32",
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

inline Result Retarget(std::span<const unsigned char> bytes, Plan& plan, std::string& reason,
                        const ArchitectureProfile& profile = architecture::kAmpere) {
  reason.clear();

  std::vector<fatbin::Entry> entries;
  size_t end = 0;
  const auto reject = [&](const char* message) {
    plan = {};
    reason = message;
    return Result::kRejected;
  };

  if (!profile.NeedsRetarget()) {
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

  // Only these two entry orders have been inspected and regression-tested. A
  // new provider layout must be reviewed as a complete selectable image set.
  const bool single = entries.size() == 1 && entries[0].kind == 1 &&
                      entries[0].architecture == 89;
  const bool mixed = entries.size() == 3 && entries[0].kind == 1 &&
                     entries[0].architecture == 120 && entries[1].kind == 1 &&
                     entries[1].architecture == 89 && entries[2].kind == 2 &&
                     entries[2].architecture == 89;
  if (!single && !mixed) return reject("unreviewed fatbin image ordering");

  const auto& entry = entries[mixed ? 1 : 0];
  if (entry.flags != 0x2041 || entry.compressed_bytes == 0 || entry.unpacked_bytes == 0) {
    return reject("unreviewed PTX compression flags");
  }

  std::vector<unsigned char> raw(entry.unpacked_bytes);
  std::vector<unsigned char> check(entry.unpacked_bytes);
  const auto* compressed = bytes.data() + entry.PayloadOffset();
  if (!fatbin::Lz4BlockDecompress(compressed, entry.compressed_bytes, raw.data(), raw.size())) {
    return reject("invalid LZ4 block");
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

  plan.replacement = std::move(replacement);
  plan.ptx_offset = entry.PayloadOffset();
  plan.ptx_bytes = entry.unpacked_bytes;
  plan.hidden_cubins = mixed ? 1 : 0;
  return Result::kRetargeted;
}

}  // namespace mfgunlock::ampere::ptx
