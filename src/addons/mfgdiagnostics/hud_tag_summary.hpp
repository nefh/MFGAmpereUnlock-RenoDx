// SPDX-License-Identifier: MIT
// Export-time summaries for MFGDiagnostics' *existing* Streamline tag observer.
// No rendering hooks, GPU resource access, or background data collection.
#pragma once

#include <array>
#include <cstdint>
#include <limits>
#include <unordered_map>
#include <unordered_set>

namespace mfgdiagnostics::hudtags {

enum class Role : uint8_t { other, hudless, ui_alpha, ui_color_alpha };

inline const char* Name(Role role) {
  switch (role) {
    case Role::hudless: return "HUDLessColor";
    case Role::ui_alpha: return "UIAlpha";
    case Role::ui_color_alpha: return "UIColorAndAlpha";
    default: return "other";
  }
}

struct Observation {
  Role role = Role::other;
  uint32_t viewport = UINT32_MAX;
  uint32_t frame = UINT32_MAX; // Legacy slSetTag has no frame identity.
  bool tag_resource_present = false;
  uint64_t native_resource = 0; // Opaque identity, never dereferenced.
  bool call_result_known = false;
  bool call_succeeded = false;
};

struct Counts {
  uint64_t total = 0;
  uint64_t active = 0;
  uint64_t clear = 0;
  uint64_t native_identity_unavailable = 0;
  uint64_t failed_calls = 0;
  uint64_t unframed = 0;
  uint64_t native_resource_changes = 0;
  uint64_t last_native_resource = 0;
  std::unordered_set<uint64_t> distinct_native_resources;
};

struct Totals {
  uint64_t frames_with_any_hud_tag = 0;
  uint64_t frames_with_observed_hud_pair = 0;
  uint64_t frames_with_partial_hud_observation = 0;
};

// Counts observations, *not* missing tags for frames that were not captured.
// A successful slSetTag only proves CPU-side API acceptance, never valid pixels.
class Summary {
 public:
  void Observe(const Observation& observation) {
    auto* counts = Counters(observation.role);
    if (!counts) return;
    ++counts->total;
    if (!observation.tag_resource_present) {
      ++counts->clear;
    } else {
      ++counts->active;
      if (observation.native_resource == 0) {
        ++counts->native_identity_unavailable;
      } else {
        counts->distinct_native_resources.insert(observation.native_resource);
        if (counts->last_native_resource != 0 &&
            counts->last_native_resource != observation.native_resource) {
          ++counts->native_resource_changes;
        }
        counts->last_native_resource = observation.native_resource;
      }
    }
    if (observation.call_result_known && !observation.call_succeeded) ++counts->failed_calls;
    if (observation.frame == UINT32_MAX || observation.viewport == UINT32_MAX) {
      ++counts->unframed;
      return;
    }
    if (!observation.tag_resource_present ||
        (observation.call_result_known && !observation.call_succeeded)) return;

    const uint64_t key = (uint64_t(observation.viewport) << 32) | observation.frame;
    unsigned bit = 0;
    if (observation.role == Role::hudless) bit = 1;
    else if (observation.role == Role::ui_alpha) bit = 2;
    else if (observation.role == Role::ui_color_alpha) bit = 4;
    frame_bits_[key] |= bit;
  }

  const Counts& Hudless() const { return counters_[0]; }
  const Counts& UiAlpha() const { return counters_[1]; }
  const Counts& UiColorAlpha() const { return counters_[2]; }

  Totals FrameTotals() const {
    Totals totals;
    for (const auto& [key, mask] : frame_bits_) {
      (void)key;
      ++totals.frames_with_any_hud_tag;
      if ((mask & 1u) && (mask & 6u)) ++totals.frames_with_observed_hud_pair;
      else ++totals.frames_with_partial_hud_observation;
    }
    return totals;
  }

 private:
  Counts* Counters(Role role) {
    if (role == Role::hudless) return &counters_[0];
    if (role == Role::ui_alpha) return &counters_[1];
    if (role == Role::ui_color_alpha) return &counters_[2];
    return nullptr;
  }
  std::array<Counts, 3> counters_{};
  std::unordered_map<uint64_t, unsigned> frame_bits_;
};

}  // namespace mfgdiagnostics::hudtags
