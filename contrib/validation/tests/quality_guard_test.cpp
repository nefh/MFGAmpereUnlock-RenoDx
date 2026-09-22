// SPDX-License-Identifier: MIT
#include "quality_guard.hpp"

#include <array>
#include <cstddef>
#include <cstdio>
#include <cstdlib>

namespace guard = mfgunlock::qualityguard;
namespace output = mfgunlock::outputstate;
namespace {
unsigned int g_checks = 0;
void Check(bool value, const char* name) {
  ++g_checks;
  if (value) return;
  std::fprintf(stderr, "FAIL %s\n", name);
  std::exit(1);
}
sl::Resource MakeResource(uint32_t width, uint32_t height, uint32_t format) {
  sl::Resource resource{};
  resource.width = width;
  resource.height = height;
  resource.nativeFormat = format;
  return resource;
}
sl::ResourceTag MakeTag(sl::Resource& resource, sl::BufferType type) {
  sl::ResourceTag tag{};
  tag.resource = &resource;
  tag.type = type;
  tag.lifecycle = sl::ResourceLifecycle::eValidUntilPresent;
  tag.extent.width = resource.width;
  tag.extent.height = resource.height;
  return tag;
}
output::State SdrOutput() {
  return output::Classify(output::FormatKind::kRgba8Unorm, 28, true,
                          output::kColorSpaceSrgbNonlinear, true);
}
output::State Hdr10Output() {
  return output::Classify(output::FormatKind::kRgb10A2Unorm, 24, true,
                          output::kColorSpaceHdr10Pq, true);
}
}  // namespace

int main() {
  const auto sdr = SdrOutput();
  const auto hdr10 = Hdr10Output();
  const auto scrgb = output::Classify(
      output::FormatKind::kRgba16Float, 10, true,
      output::kColorSpaceScRgbLinear, true);
  const output::State unknown{};

  auto backbuffer = MakeResource(1920, 1080, 28);
  auto hudless = MakeResource(1920, 1080, 28);
  auto ui = MakeResource(1920, 1080, 28);
  std::array tags{
      MakeTag(backbuffer, sl::kBufferTypeBackbuffer),
      MakeTag(hudless, sl::kBufferTypeHUDLessColor),
      MakeTag(ui, sl::kBufferTypeUIColorAndAlpha),
  };

  auto assessment = guard::AssessTags(tags.data(), tags.size(), sdr);
  Check(assessment.issues == guard::kNone && assessment.has_hudless_color &&
            assessment.has_ui_color_or_alpha && !assessment.suppress_hud_separation,
        "valid HUD-less and UI pair accepted");
  Check(guard::CanAutomaticallyUseUiRecomposition(assessment, sdr),
        "valid SDR pair is eligible for UI recomposition");
  Check(assessment.render_encoding_known &&
            assessment.render_encoding == output::Encoding::kSdrSrgb &&
            assessment.has_ui_color_alpha && !assessment.has_ui_alpha &&
            assessment.ui_encoding == output::UiEncoding::kOutputEncoded,
        "render, UI and output domains retain independent provenance");
  Check(guard::ResolveSuppression(assessment, assessment, sdr, false) ==
            guard::SuppressionPolicy::kNone,
        "complete validated UI pair is forwarded");

  std::array hudless_only{
      MakeTag(backbuffer, sl::kBufferTypeBackbuffer),
      MakeTag(hudless, sl::kBufferTypeHUDLessColor),
  };
  const auto hudless_assessment =
      guard::AssessTags(hudless_only.data(), hudless_only.size(), sdr);
  Check(guard::ResolveSuppression(hudless_assessment, hudless_assessment, sdr, false) ==
            guard::SuppressionPolicy::kNone,
        "HUD-less-only integration keeps the native HUD-less path");

  std::array ui_only{
      MakeTag(backbuffer, sl::kBufferTypeBackbuffer),
      MakeTag(ui, sl::kBufferTypeUIColorAndAlpha),
  };
  const auto ui_assessment = guard::AssessTags(ui_only.data(), ui_only.size(), sdr);
  Check(guard::ResolveSuppression(ui_assessment, ui_assessment, sdr, false) ==
            guard::SuppressionPolicy::kUiOnly,
        "UI-only input is withheld until HUD-less exists");

  auto accumulated_pair = hudless_assessment;
  accumulated_pair.has_ui_color_or_alpha = true;
  accumulated_pair.has_ui_color_alpha = true;
  accumulated_pair.ui_encoding = guard::NativeUiEncoding(accumulated_pair);
  Check(guard::ResolveSuppression(ui_assessment, accumulated_pair, sdr, false) ==
            guard::SuppressionPolicy::kUiOnly,
        "first split UI transition is withheld");
  Check(guard::ResolveSuppression(ui_assessment, accumulated_pair, sdr, true) ==
            guard::SuppressionPolicy::kNone,
        "subsequent split UI submission is forwarded");

  auto ui_alpha = MakeResource(1920, 1080, 28);
  std::array ui_alpha_only{
      MakeTag(backbuffer, sl::kBufferTypeBackbuffer),
      MakeTag(ui_alpha, sl::kBufferTypeUIAlpha),
  };
  const auto ui_alpha_assessment =
      guard::AssessTags(ui_alpha_only.data(), ui_alpha_only.size(), hdr10);
  Check(ui_alpha_assessment.has_ui_alpha && !ui_alpha_assessment.has_ui_color_alpha &&
            ui_alpha_assessment.ui_encoding == output::UiEncoding::kAlphaOnly,
        "UI Alpha is tracked independently from UI Color+Alpha");

  assessment = guard::AssessTags(tags.data(), tags.size(), hdr10);
  Check(guard::CanAutomaticallyUseUiRecomposition(assessment, hdr10) &&
            !assessment.suppress_hud_separation,
        "native HDR10 UI pair is preserved and can request UIR");

  assessment = guard::AssessTags(tags.data(), tags.size(), scrgb);
  Check((assessment.issues & guard::kOutputEncodingUnsupported) != 0 &&
            !guard::CanAutomaticallyUseUiRecomposition(assessment, scrgb) &&
            !assessment.suppress_hud_separation,
        "scRGB is detected as unsupported without rewriting native tags");

  assessment = guard::AssessTags(tags.data(), tags.size(), unknown);
  Check((assessment.issues & guard::kOutputEncodingUnknown) != 0 &&
            !guard::CanAutomaticallyUseUiRecomposition(assessment, unknown) &&
            !assessment.suppress_hud_separation,
        "unknown output declines automatic UIR without rewriting native tags");

  ui.nativeFormat = 24;
  assessment = guard::AssessTags(tags.data(), tags.size(), sdr);
  Check((assessment.issues & guard::kUiColorAlphaLowPrecision) != 0 &&
            assessment.suppress_hud_separation,
        "two-bit UI alpha is rejected");
  ui.nativeFormat = 28;

  ui.width = 1280;
  tags[2] = MakeTag(ui, sl::kBufferTypeUIColorAndAlpha);
  assessment = guard::AssessTags(tags.data(), tags.size(), sdr);
  Check((assessment.issues & guard::kUiExtentMismatch) != 0,
        "UI extent mismatch is rejected");
  ui.width = 1920;
  tags[2] = MakeTag(ui, sl::kBufferTypeUIColorAndAlpha);

  tags[1].resource->structType = 0;
  assessment = guard::AssessTags(tags.data(), tags.size(), sdr);
  Check((assessment.issues & guard::kInvalidOptionalResource) != 0,
        "unknown optional resource ABI is rejected");
  tags[1].resource->structType = sl::Resource::s_structType;

  Check(guard::ShouldRequestAutomaticUiPath(sdr) &&
            guard::ShouldRequestAutomaticUiPath(hdr10) &&
            !guard::ShouldRequestAutomaticUiPath(scrgb) &&
            !guard::ShouldRequestAutomaticUiPath(unknown),
        "automatic path requires a supported known output encoding");

  sl::Constants constants{};
  constants.reset = sl::Boolean::eFalse;
  std::array<std::byte, sizeof(sl::Constants)> storage{};
  Check(guard::CopyConstantsWithReset(constants, storage.data(), storage.size()) &&
            reinterpret_cast<const sl::Constants*>(storage.data())->reset == sl::Boolean::eTrue,
        "temporal reset copy sets reset without changing caller state");
  Check(constants.reset == sl::Boolean::eFalse,
        "temporal reset copy leaves caller constants unchanged");

  constants.structVersion = 99;
  Check(!guard::CopyConstantsWithReset(constants, storage.data(), storage.size()),
        "unknown Constants ABI fails closed");

  std::printf("PASS UI quality guard: %u checks\n", g_checks);
}
