// SPDX-License-Identifier: MIT
#include "../../../src/addons/mfgunlock/composition_state.hpp"
#include <cstdio>
#include <cstdlib>

namespace c = mfgunlock::composition;
unsigned int g_checks = 0;
void Check(bool good, const char* reason) {
  ++g_checks;
  if (good) return;
  std::fprintf(stderr, "FAIL: %s\n", reason);
  std::exit(1);
}

int main() {
  c::Reset();
  c::Bind(c::Api::kD3D12, 1, 2, 3);
  c::ObservePresent(9, 9);
  Check(c::Read().source_present_calls == 0, "secondary swapchain is ignored");
  c::ObserveEffects(false, 1, 3, 4, 5);
  Check(!c::Read().effects_pair_observed, "finish without matching begin is not a pair");
  c::ObserveEffects(true, 1, 3, 4, 5);
  c::ObserveEffects(false, 1, 3, 4, 6);
  auto s = c::Read();
  Check(s.effects_pair_observed && s.before_effects_resource == 5 && s.after_effects_resource == 6,
        "source/effect output identities remain distinct");
  c::ObserveEffects(true, 1, 3, 4, 5);
  c::ObserveEffects(false, 1, 3, 9, 6);
  Check(!c::Read().effects_pair_observed, "different effect runtime cannot complete a pair");
  c::SetScreenshotEventAvailable(true);
  c::ObserveScreenshot(1, 3);
  c::ObserveOverlay(1, 3);
  c::ObservePresent(2, 6);
  s = c::Read();
  Check(s.screenshots_saved == 1 && s.overlay_callbacks == 1 && s.source_present_calls == 1,
        "callback counts are independent observations");
  Check(!s.generated_resource_known && !s.external_capture_known &&
        !s.display_cadence_known && !s.captured_encoding_known && !s.ui_composed_pixels_verified,
        "screenshot or Present cannot prove OBS pixels, HDR or generated-frame visibility");
  c::ObserveEffects(true, 1, 3, 4, 5);
  c::InvalidateResource(5);
  c::ObserveEffects(false, 1, 3, 4, 5);
  Check(!c::Read().effects_pair_observed, "destroyed resource cannot finish a stale pair");
  const auto generation = s.generation;
  c::Reset();
  c::Bind(c::Api::kVulkan, 1, 2, 3);
  s = c::Read();
  Check(s.generation > generation && s.source_present_calls == 0 && s.effects_runtime == 0,
        "same-address recreation clears prior generation evidence");
  Check(s.api == c::Api::kVulkan && !s.external_capture_known,
        "Vulkan supports metadata, not invented capture/queue completion");
  c::ObserveEffects(true, 1, 3, 4, 5);
  c::InvalidateEffects(4);
  Check(c::Read().effects_runtime == 0, "effect runtime destruction invalidates tracked pair");
  std::printf("composition_state_test PASS (%u checks)\n", g_checks);
}
