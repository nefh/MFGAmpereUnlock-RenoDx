// SPDX-License-Identifier: MIT
#include <cstdio>
#include <cstdlib>
#include <string_view>

#include "../../../src/addons/mfgunlock/ui_submission.hpp"

namespace ui = mfgunlock::uisubmission;
namespace output = mfgunlock::outputstate;
namespace alpha = mfgunlock::uialpha;

namespace {
unsigned int g_checks = 0;

void Check(bool value, const char* name) {
  ++g_checks;
  if (value) return;
  std::fprintf(stderr, "FAIL %s\n", name);
  std::exit(1);
}

output::State Sdr() {
  return output::Classify(output::FormatKind::kRgba8Unorm,
                          output::kFormatRgba8Unorm, true,
                          output::kColorSpaceSrgbNonlinear, true);
}

output::State Hdr10() {
  return output::Classify(output::FormatKind::kRgb10A2Unorm,
                          output::kFormatRgb10A2Unorm, true,
                          output::kColorSpaceHdr10Pq, true);
}

output::State ScRgb() {
  return output::Classify(output::FormatKind::kRgba16Float,
                          output::kFormatRgba16Float, true,
                          output::kColorSpaceScRgbLinear, true);
}

ui::Candidate Candidate(output::UiEncoding encoding,
                        alpha::AlphaConvention convention,
                        uint32_t format = output::kFormatRgba8Srgb) {
  return {encoding, convention, format};
}
}  // namespace

int main() {
  Check(ui::Select(Sdr(), Candidate(output::UiEncoding::kSdrSrgb,
                                    alpha::AlphaConvention::kPremultiplied),
                   false) == ui::Path::kColorAndAlpha,
        "matching SDR UI uses direct Color+Alpha");

  Check(ui::Select(Sdr(), Candidate(output::UiEncoding::kSdrSrgb,
                                    alpha::AlphaConvention::kUnknown),
                   true) == ui::Path::kColorAndAlpha,
        "matching SDR UI preserves direct Color+Alpha when alpha evidence is incomplete");

  Check(ui::Select(Hdr10(), Candidate(output::UiEncoding::kSdrSrgb,
                                      alpha::AlphaConvention::kPremultiplied),
                   false) == ui::Path::kUnavailable,
        "HDR10 stays closed without alpha backend");

  Check(ui::Select(Hdr10(), Candidate(output::UiEncoding::kSdrSrgb,
                                      alpha::AlphaConvention::kAlphaOnly),
                   true) == ui::Path::kAlphaOnly,
        "HDR10 chooses alpha-only path instead of treating SDR RGB as PQ");

  Check(ui::Select(Hdr10(), Candidate(output::UiEncoding::kUnknown,
                                      alpha::AlphaConvention::kAlphaOnly,
                                      output::kFormatRgba8Unorm),
                   true) == ui::Path::kAlphaOnly,
        "unknown RGB transfer does not block proven alpha-only extraction");

  Check(ui::Select(Hdr10(), Candidate(output::UiEncoding::kSdrSrgb,
                                      alpha::AlphaConvention::kUnknown),
                   true) == ui::Path::kUnavailable,
        "unknown alpha convention fails closed");

  Check(ui::Select(Hdr10(), Candidate(output::UiEncoding::kSdrSrgb,
                                      alpha::AlphaConvention::kAlphaOnly,
                                      output::kFormatRgb10A2Unorm),
                   true) == ui::Path::kUnavailable,
        "unsupported extraction format fails closed");

  Check(ui::Select(ScRgb(), Candidate(output::UiEncoding::kSdrSrgb,
                                      alpha::AlphaConvention::kAlphaOnly),
                   true) == ui::Path::kAlphaOnly,
        "scRGB output metadata cannot veto actual alpha extraction");

  Check(ui::Select({}, Candidate(output::UiEncoding::kUnknown,
                                    alpha::AlphaConvention::kAlphaOnly),
                   true) == ui::Path::kAlphaOnly,
        "unknown output still permits color-independent coverage extraction");

  Check(ui::Select(Sdr(), Candidate(output::UiEncoding::kUnknown,
                                    alpha::AlphaConvention::kAlphaOnly,
                                    output::kFormatRgba8Unorm),
                   true) == ui::Path::kAlphaOnly,
        "alpha-only path can avoid guessing SDR RGB transfer");

  Check(std::string_view(ui::PathName(ui::Path::kAlphaOnly)) == "UI Alpha" &&
            std::string_view(ui::PathName(ui::Path::kUnavailable)) == "unavailable",
        "path names stay stable for diagnostics");

  std::printf("PASS UI submission policy: %u checks\n", g_checks);
}
