// SPDX-License-Identifier: MIT
#include "../../../src/addons/mfgunlock/runtime_controls.hpp"
#include <cassert>
#include <iostream>
#include <bit>

using namespace mfgunlock::runtimecontrols;
int main() {
  for (const auto value : {0.0f, 1.0f, 0.25f, 144.0f, 480.0f, 500.5f, 1000.0f,
                          16777216.0f, (std::numeric_limits<float>::max)(),
                          (std::numeric_limits<float>::min)(), std::numeric_limits<float>::denorm_min()}) {
    assert(ValidDynamicTarget(value));
    const auto encoded = FormatDynamicTarget(value);
    assert(!encoded.empty());
    float decoded = -1.0f;
    assert(ParseDynamicTarget(encoded, decoded));
    assert(std::bit_cast<uint32_t>(decoded) == std::bit_cast<uint32_t>(value));
  }
  for (const auto text : {"", " ", "+", "nan", "NaN", "inf", "-inf", "-1", "1e100",
                          "1e-100", "480oops", "1,5", "0x10", "++1", "+-0"}) {
    float value = 123.5f;
    assert(!ParseDynamicTarget(text, value));
    assert(value == 123.5f);  // Invalid input does not quietly become Auto/480.
  }
  float value = -1.0f;
  assert(ParseDynamicTarget(" \t+600.25\r\n", value) && value == 600.25f);
  assert(ParseDynamicTarget("-0", value) && value == 0.0f && !std::signbit(value));
  assert(ParseDynamicTarget("1e3", value) && value == 1000.0f);
  const char embedded[] = {'4', '8', '0', '\0', 'x'};
  assert(!ParseDynamicTarget(std::string_view(embedded, sizeof(embedded)), value));
  for (float bad : {-1.0f, std::numeric_limits<float>::infinity(),
                    std::numeric_limits<float>::quiet_NaN()}) {
    assert(!ValidDynamicTarget(bad));
    assert(FormatDynamicTarget(bad).empty());
  }
  assert(!MaximumKnown(kUnknownMaximum));
  assert(MaximumKnown(EncodeMaximum(0)));
  assert(AboveMaximum(1, EncodeMaximum(0)));
  assert(!AboveMaximum(UINT32_MAX, kUnknownMaximum));
  assert(!AboveMaximum(3, EncodeMaximum(3)));
  assert(AboveMaximum(5, EncodeMaximum(3)));
  assert(static_cast<uint64_t>(MaximumGenerated(EncodeMaximum(UINT32_MAX))) + 1 == 4294967296ull);

  OriginalMaximum maximum;
  assert(maximum.Observe(false, 0) == kUnknownMaximum);
  assert(maximum.Observe(true, 3) == EncodeMaximum(3));
  maximum.Wrote(5);
  for (int i = 0; i < 100; ++i)
    assert(maximum.Observe(true, 5) == EncodeMaximum(3, true));
  assert(maximum.Observe(false, 0) == kUnknownMaximum);
  assert(maximum.Observe(true, 5) == EncodeMaximum(3, true));
  assert(maximum.Observe(true, 7) == EncodeMaximum(7));
  maximum.Wrote(9);
  maximum.Invalidate();
  assert(maximum.Observe(true, 9) == kUnknownMaximum);
  assert(maximum.Observe(true, 1) == EncodeMaximum(1));
  maximum.Wrote(1); // A rejected Set can read back the original; still honest.
  assert(maximum.Observe(true, 1) == EncodeMaximum(1, true));
  std::cout << "PASS runtime controls: float contract, round trips, diagnostic provenance\n";
}
