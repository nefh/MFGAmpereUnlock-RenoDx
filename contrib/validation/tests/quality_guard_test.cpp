// SPDX-License-Identifier: MIT
#include "quality_guard.hpp"

#include <array>
#include <cstddef>
#include <cstdio>
#include <cstdlib>

namespace guard = mfgunlock::qualityguard;
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
}  // namespace

int main() {
  auto backbuffer = MakeResource(1920, 1080, 28);
  auto hudless = MakeResource(1920, 1080, 28);
  auto ui = MakeResource(1920, 1080, 28);
  std::array tags{
      MakeTag(backbuffer, sl::kBufferTypeBackbuffer),
      MakeTag(hudless, sl::kBufferTypeHUDLessColor),
      MakeTag(ui, sl::kBufferTypeUIColorAndAlpha),
  };

  auto assessment = guard::AssessTags(tags.data(), tags.size(), false);
  Check(assessment.issues == guard::kNone && assessment.has_hudless_color &&
            assessment.has_ui_color_or_alpha && !assessment.suppress_hud_separation,
        "valid HUD-less and UI pair accepted");
  Check(guard::CanAutomaticallyUseUiRecomposition(assessment, false),
        "valid SDR pair is eligible for UI recomposition");
  Check(assessment.has_ui_color_alpha && !assessment.has_ui_alpha,
        "UI Color+Alpha is reported separately from UI Alpha");
  Check(guard::ResolveSuppression(assessment, assessment, false, false) ==
            guard::SuppressionPolicy::kNone,
        "complete validated UI pair is forwarded");

  std::array hudless_only{
      MakeTag(backbuffer, sl::kBufferTypeBackbuffer),
      MakeTag(hudless, sl::kBufferTypeHUDLessColor),
  };
  const auto hudless_assessment =
      guard::AssessTags(hudless_only.data(), hudless_only.size(), false);
  Check(guard::ResolveSuppression(hudless_assessment, hudless_assessment, false, false) ==
            guard::SuppressionPolicy::kNone,
        "HUD-less-only integration keeps the native HUD-less path");

  std::array ui_only{
      MakeTag(backbuffer, sl::kBufferTypeBackbuffer),
      MakeTag(ui, sl::kBufferTypeUIColorAndAlpha),
  };
  const auto ui_assessment = guard::AssessTags(ui_only.data(), ui_only.size(), false);
  Check(guard::ResolveSuppression(ui_assessment, ui_assessment, false, false) ==
            guard::SuppressionPolicy::kUiOnly,
        "UI-only input is withheld until HUD-less exists");

  auto accumulated_pair = hudless_assessment;
  accumulated_pair.has_ui_color_or_alpha = true;
  accumulated_pair.has_ui_color_alpha = true;
  Check(guard::ResolveSuppression(ui_assessment, accumulated_pair, false, false) ==
            guard::SuppressionPolicy::kUiOnly,
        "first split UI transition is withheld");
  Check(guard::ResolveSuppression(ui_assessment, accumulated_pair, false, true) ==
            guard::SuppressionPolicy::kNone,
        "subsequent split UI submission is forwarded");

  auto ui_alpha = MakeResource(1920, 1080, 28);
  std::array ui_alpha_only{
      MakeTag(backbuffer, sl::kBufferTypeBackbuffer),
      MakeTag(ui_alpha, sl::kBufferTypeUIAlpha),
  };
  const auto ui_alpha_assessment =
      guard::AssessTags(ui_alpha_only.data(), ui_alpha_only.size(), false);
  Check(ui_alpha_assessment.has_ui_alpha && !ui_alpha_assessment.has_ui_color_alpha &&
            ui_alpha_assessment.has_ui_color_or_alpha,
        "UI Alpha is reported separately from UI Color+Alpha");

  ui.nativeFormat = 24;
  assessment = guard::AssessTags(tags.data(), tags.size(), false);
  Check((assessment.issues & guard::kUiColorAlphaLowPrecision) != 0 &&
            assessment.suppress_hud_separation,
        "two-bit UI alpha is rejected");
  ui.nativeFormat = 28;

  ui.width = 1280;
  tags[2] = MakeTag(ui, sl::kBufferTypeUIColorAndAlpha);
  assessment = guard::AssessTags(tags.data(), tags.size(), false);
  Check((assessment.issues & guard::kUiExtentMismatch) != 0,
        "UI extent mismatch is rejected");
  ui.width = 1920;
  tags[2] = MakeTag(ui, sl::kBufferTypeUIColorAndAlpha);

  assessment = guard::AssessTags(tags.data(), tags.size(), true);
  Check((assessment.issues & guard::kHdrFinalColorIsolation) != 0 &&
            !guard::CanAutomaticallyUseUiRecomposition(assessment, true),
        "HDR remains on conservative final-color path");

  tags[1].resource->structType = 0;
  assessment = guard::AssessTags(tags.data(), tags.size(), false);
  Check((assessment.issues & guard::kInvalidOptionalResource) != 0,
        "unknown optional resource ABI is rejected");
  tags[1].resource->structType = sl::Resource::s_structType;

  Check(!guard::ShouldRequestAutomaticUiPath(false, false) &&
            guard::ShouldRequestAutomaticUiPath(true, false) &&
            !guard::ShouldRequestAutomaticUiPath(true, true),
        "automatic path requires a known SDR output");

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
