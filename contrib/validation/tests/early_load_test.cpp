// SPDX-License-Identifier: MIT
#include "../../../src/addons/mfgunlock/early_load.hpp"

#include <cstdio>
#include <cstdlib>
#include <span>
#include <string>
#include <vector>

namespace {
unsigned int g_checks = 0;

void Check(bool value, const char* name) {
  ++g_checks;
  if (value) return;
  std::fprintf(stderr, "FAIL %s\n", name);
  std::exit(1);
}

std::vector<char> Values(std::initializer_list<const char*> items) {
  std::vector<char> result;
  for (const char* item : items) {
    const std::string value(item);
    result.insert(result.end(), value.begin(), value.end());
    result.push_back('\0');
  }
  return result;
}
}  // namespace

int main() {
  constexpr std::string_view addon = "renodx-mfgunlock.addon64";

  const auto empty = mfgunlock::early_load::Append({}, addon);
  Check(mfgunlock::early_load::Contains(empty, addon), "append to empty list");

  const auto other = Values({"renodx-dlss.addon64", "example.addon64"});
  Check(!mfgunlock::early_load::Contains(other, addon), "missing addon detected");
  const auto merged = mfgunlock::early_load::Append(other, addon);
  Check(mfgunlock::early_load::Contains(merged, "renodx-dlss.addon64"), "first entry preserved");
  Check(mfgunlock::early_load::Contains(merged, "example.addon64"), "second entry preserved");
  Check(mfgunlock::early_load::Contains(merged, addon), "addon appended");

  const auto existing = Values({"addons\\RENODX-MFGUNLOCK.ADDON64"});
  Check(mfgunlock::early_load::Contains(existing, addon), "path and case ignored");

  const auto trailing = Values({"one.addon64"});
  auto padded = trailing;
  padded.push_back('\0');
  const auto normalized = mfgunlock::early_load::Append(padded, addon);
  Check(mfgunlock::early_load::Contains(normalized, "one.addon64"), "trailing NUL normalized");
  Check(mfgunlock::early_load::Contains(normalized, addon), "append after trailing NUL");

  std::printf("PASS early-load list: %u checks\n", g_checks);
}
