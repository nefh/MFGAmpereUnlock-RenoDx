#include <cstdlib>
#include <iostream>

#include "../../../src/addons/mfgunlock/output_state.hpp"

namespace output = mfgunlock::outputstate;

void Check(bool condition, const char* message) {
  if (condition) return;
  std::cerr << "FAIL: " << message << '\n';
  std::exit(1);
}

int main() {
  const auto sdr = output::Classify(
      output::FormatKind::kRgba8Unorm, 28, true,
      output::kColorSpaceSrgbNonlinear, true);
  Check(sdr.encoding == output::Encoding::kSdrSrgb && sdr.known && !sdr.hdr &&
            sdr.dlssg_supported,
        "SDR sRGB output should be known and supported");

  const auto hdr10 = output::Classify(
      output::FormatKind::kRgb10A2Unorm, 24, true,
      output::kColorSpaceHdr10Pq, true);
  Check(hdr10.encoding == output::Encoding::kHdr10Pq && hdr10.hdr &&
            hdr10.wide_gamut && hdr10.dlssg_supported,
        "HDR10 RGB10 output should use the supported HDR path");

  const auto hdr10_wrong_format = output::Classify(
      output::FormatKind::kRgba8Unorm, 28, true,
      output::kColorSpaceHdr10Pq, true);
  Check(hdr10_wrong_format.encoding == output::Encoding::kHdr10Pq &&
            !hdr10_wrong_format.dlssg_supported,
        "HDR10 with a non-RGB10 swapchain must fail closed");

  const auto scrgb = output::Classify(
      output::FormatKind::kRgba16Float, 10, true,
      output::kColorSpaceScRgbLinear, true);
  Check(scrgb.encoding == output::Encoding::kScRgbLinear && scrgb.hdr &&
            scrgb.linear && !scrgb.dlssg_supported,
        "scRGB should be detected but not advertised as DLSS-G supported");

  const auto hlg = output::Classify(
      output::FormatKind::kRgb10A2Unorm, 24, true,
      output::kColorSpaceHdr10Hlg, true);
  Check(hlg.encoding == output::Encoding::kHdrOther && hlg.hdr &&
            !hlg.dlssg_supported,
        "HLG should be detected as HDR but remain unsupported");

  const auto unknown = output::Classify(
      output::FormatKind::kRgb10A2Unorm, 24, true, 0, false);
  Check(!unknown.known && !unknown.dlssg_supported,
        "unknown color space must remain fail closed");

  Check(output::IsUiEncodingCompatible(
            hdr10, output::UiEncoding::kAlphaOnly, true),
        "alpha-only UI should be compatible with HDR10");
  Check(output::IsUiEncodingCompatible(
            hdr10, output::UiEncoding::kOutputEncoded, true),
        "native output-encoded UI should be compatible with HDR10");
  Check(!output::IsUiEncodingCompatible(
            hdr10, output::UiEncoding::kSdrSrgb, false),
        "detected SDR UI must not be treated as HDR10/PQ");
  Check(output::IsUiEncodingCompatible(
            sdr, output::UiEncoding::kSdrSrgb, false),
        "detected SDR UI should remain valid on SDR output");

  output::Reset();
  Check(output::Observe(output::FormatKind::kRgba8Unorm, 28, true,
                        output::kColorSpaceSrgbNonlinear, true),
        "first SDR observation should change state");
  const auto first = output::Read();
  Check(first.encoding == output::Encoding::kSdrSrgb,
        "observed SDR state should be readable");
  Check(output::Observe(output::FormatKind::kRgb10A2Unorm, 24, true,
                        output::kColorSpaceHdr10Pq, true),
        "SDR to HDR transition should change state");
  const auto second = output::Read();
  Check(second.encoding == output::Encoding::kHdr10Pq &&
            second.revision > first.revision,
        "HDR transition should advance revision");
  Check(output::Observe(output::FormatKind::kRgba8Unorm, 28, true,
                        output::kColorSpaceSrgbNonlinear, true),
        "HDR to SDR transition should change state");
  Check(output::Reset(), "reset should invalidate a known output");
  Check(!output::Read().known, "reset output should be unknown");

  std::cout << "output_state_test=PASS\n";
  return 0;
}
