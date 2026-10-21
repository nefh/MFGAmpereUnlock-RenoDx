// SPDX-License-Identifier: MIT
// Validation and diagnostic values for public runtime controls. No GPU policy.
#pragma once

#include <array>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>

namespace mfgunlock::runtimecontrols {

// sl::DLSSGOptions v5 uses float; zero requests display-refresh autodetection.
// There is no addon FPS ceiling. Backend-specific acceptance remains native.
inline bool ValidDynamicTarget(float value) {
  return std::isfinite(value) && value >= 0.0f;
}

inline bool ParseDynamicTarget(std::string_view text, float& output) {
  constexpr std::string_view whitespace = " \t\r\n";
  const auto first = text.find_first_not_of(whitespace);
  if (first == std::string_view::npos) return false;
  text = text.substr(first, text.find_last_not_of(whitespace) - first + 1);
  if (text.front() == '+') {
    text.remove_prefix(1);
    if (text.empty() || text.front() == '+' || text.front() == '-') return false;
  }
  float value = 0.0f;
  const auto result = std::from_chars(text.data(), text.data() + text.size(), value,
                                      std::chars_format::general);
  if (result.ec != std::errc{} || result.ptr != text.data() + text.size() ||
      !ValidDynamicTarget(value)) return false;
  output = value == 0.0f ? 0.0f : value;
  return true;
}

inline std::string FormatDynamicTarget(float value) {
  if (!ValidDynamicTarget(value)) return {};
  std::array<char, 64> buffer{};
  const auto result = std::to_chars(buffer.data(), buffer.data() + buffer.size(),
                                    value == 0.0f ? 0.0f : value,
                                    std::chars_format::general,
                                    std::numeric_limits<float>::max_digits10);
  return result.ec == std::errc{} ? std::string(buffer.data(), result.ptr) : std::string{};
}

// One atomic word preserves validity and value together. A reported zero is a
// known zero generated frames, not the same thing as an unavailable query.
inline constexpr uint64_t kUnknownMaximum = 0;
inline constexpr uint64_t kMaximumKnown = uint64_t{1} << 32;
inline constexpr uint64_t kRetainedMaximum = uint64_t{1} << 33;
inline constexpr uint64_t EncodeMaximum(uint32_t generated, bool retained = false) {
  return kMaximumKnown | (retained ? kRetainedMaximum : 0) | generated;
}
inline constexpr bool MaximumKnown(uint64_t observation) {
  return (observation & kMaximumKnown) != 0;
}
inline constexpr uint32_t MaximumGenerated(uint64_t observation) {
  return static_cast<uint32_t>(observation);
}
inline constexpr bool AboveMaximum(uint32_t requested_generated, uint64_t observation) {
  return MaximumKnown(observation) && requested_generated > MaximumGenerated(observation);
}

// A vendor-owned NGX block can be returned repeatedly after our own Set.
// Do not promote our own previous write to a new native capability report.
// A matching value is ambiguous: retain only the last unmodified observation,
// explicitly marked as such. Failure never manufactures a zero capability.
struct OriginalMaximum {
  uint64_t original = kUnknownMaximum;
  uint32_t written = 0;
  bool overwritten = false;

  uint64_t Observe(bool success, uint32_t value) {
    if (!success) return kUnknownMaximum;
    if (overwritten && value == written)
      return MaximumKnown(original) ? original | kRetainedMaximum : kUnknownMaximum;
    original = EncodeMaximum(value);
    overwritten = false;
    return original;
  }
  void Wrote(uint32_t value) {
    written = value;
    overwritten = true;
  }
  void Invalidate() {
    original = kUnknownMaximum;
    // Keep the write marker: losing evidence does not undo a vendor-block Set.
  }
};

}  // namespace mfgunlock::runtimecontrols
