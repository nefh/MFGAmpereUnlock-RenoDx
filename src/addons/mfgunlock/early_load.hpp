/*
 * ReShade early-load configuration helpers.
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <span>
#include <string_view>
#include <vector>

namespace mfgunlock::early_load {

inline bool EqualsInsensitive(std::string_view a, std::string_view b) {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); ++i) {
    const auto lower = [](char c) { return c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c; };
    if (lower(a[i]) != lower(b[i])) return false;
  }
  return true;
}

inline std::string_view FileName(std::string_view path) {
  const auto slash = path.find_last_of("/\\");
  return slash == path.npos ? path : path.substr(slash + 1);
}

// ReShade exposes string-array config values as NUL-separated elements.
inline bool Contains(std::span<const char> values, std::string_view addon) {
  size_t offset = 0;
  while (offset < values.size()) {
    size_t end = offset;
    while (end < values.size() && values[end] != '\0') ++end;
    const std::string_view value(values.data() + offset, end - offset);
    if (!value.empty() && EqualsInsensitive(FileName(value), addon)) return true;
    offset = end == values.size() ? end : end + 1;
  }
  return false;
}

inline std::vector<char> Append(std::span<const char> values, std::string_view addon) {
  std::vector<char> result(values.begin(), values.end());
  while (!result.empty() && result.back() == '\0') result.pop_back();
  if (!result.empty()) result.push_back('\0');
  result.insert(result.end(), addon.begin(), addon.end());
  result.push_back('\0');
  return result;
}

}  // namespace mfgunlock::early_load
