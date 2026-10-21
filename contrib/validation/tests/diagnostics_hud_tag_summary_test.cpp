#include "../../../src/addons/mfgdiagnostics/hud_tag_summary.hpp"
#include <cassert>
#include <cstdint>
using namespace mfgdiagnostics::hudtags;
int main() {
  Summary s;
  assert(s.FrameTotals().frames_with_any_hud_tag == 0);
  assert(Name(Role::ui_alpha)[0] == 'U');
  // A legacy tag can legally outlive a single frame. Do not invent missing pairs.
  s.Observe({Role::hudless, 1, UINT32_MAX, true, 100, true, true});
  s.Observe({Role::ui_alpha, 1, UINT32_MAX, true, 200, true, true});
  assert(s.FrameTotals().frames_with_any_hud_tag == 0);
  assert(s.Hudless().unframed == 1);
  // Frame 7 contains a complete pair; both resources are opaque identities.
  s.Observe({Role::hudless, 1, 7, true, 100, true, true});
  s.Observe({Role::ui_alpha, 1, 7, true, 200, true, true});
  assert(s.FrameTotals().frames_with_observed_hud_pair == 1);
  assert(s.Hudless().distinct_native_resources.size() == 1);
  // An explicit clear is not an active UI layer or a missing frame proof.
  s.Observe({Role::ui_alpha, 1, 8, false, 0, true, true});
  assert(s.UiAlpha().clear == 1);
  assert(s.FrameTotals().frames_with_any_hud_tag == 1);
  // Failed tag requests must not count as an accepted frame pair.
  s.Observe({Role::hudless, 1, 9, true, 100, true, false});
  assert(s.Hudless().failed_calls == 1);
  assert(s.FrameTotals().frames_with_any_hud_tag == 1);
  // A changed handle is reported as a transition, not an error.
  s.Observe({Role::hudless, 2, 10, true, 300, true, true});
  assert(s.Hudless().native_resource_changes == 1);
  assert(s.FrameTotals().frames_with_partial_hud_observation == 1);
  // Valid tag structure with unrecognized resource identity must be reported.
  s.Observe({Role::ui_color_alpha, UINT32_MAX, UINT32_MAX, true, 0, true, true});
  assert(s.UiColorAlpha().native_identity_unavailable == 1);
  return 0;
}
