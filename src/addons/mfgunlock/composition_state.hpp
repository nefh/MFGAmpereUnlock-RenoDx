// SPDX-License-Identifier: MIT
#pragma once

#include <cstdint>
#include <mutex>

namespace mfgunlock::composition {

inline constexpr uint32_t kVersion = 1;
enum class Api : uint32_t { kUnknown, kD3D12, kVulkan };

// Observation, not resource ownership or proof of pixels reaching a display.
// No generated-frame or external-capture identity is inferred from Present.
struct Snapshot {
  uint32_t bytes = sizeof(Snapshot);
  uint32_t version = kVersion;
  Api api = Api::kUnknown;
  uint64_t generation = 0;
  uint64_t device = 0;
  uint64_t swapchain = 0;
  uint64_t reference_backbuffer = 0;
  uint64_t source_present_calls = 0;
  uint64_t presented_resource = 0;
  uint64_t effects_runtime = 0;
  uint64_t before_effects_resource = 0;
  uint64_t after_effects_resource = 0;
  uint64_t effects_sequence = 0;
  uint64_t overlay_callbacks = 0;
  uint64_t screenshots_saved = 0;
  uint32_t effects_pair_observed = 0;
  uint32_t screenshot_event_available = 0;
  // These require independent evidence, not resource dimensions or API names.
  uint32_t generated_resource_known = 0;
  uint32_t ui_composed_pixels_verified = 0;
  uint32_t display_cadence_known = 0;
  uint32_t external_capture_known = 0;
  uint32_t captured_encoding_known = 0;
};

inline std::mutex g_mutex;
inline Snapshot g_state;
inline bool g_effects_open = false;
using Query = bool (*)(uint32_t version, void* output);

inline void Reset() {
  std::lock_guard lock(g_mutex);
  const auto generation = g_state.generation + 1;
  const auto screenshot_event = g_state.screenshot_event_available;
  g_state = {};
  g_state.generation = generation;
  g_state.screenshot_event_available = screenshot_event;
  g_effects_open = false;
}

inline void Bind(Api api, uint64_t device, uint64_t swapchain, uint64_t backbuffer) {
  std::lock_guard lock(g_mutex);
  if (device == 0 || swapchain == 0 || backbuffer == 0) return;
  if (g_state.device == device && g_state.swapchain == swapchain &&
      g_state.reference_backbuffer == backbuffer && g_state.api == api) return;
  const auto generation = g_state.generation + 1;
  const auto screenshot_event = g_state.screenshot_event_available;
  g_state = {};
  g_state.generation = generation;
  g_state.screenshot_event_available = screenshot_event;
  g_state.api = api;
  g_state.device = device;
  g_state.swapchain = swapchain;
  g_state.reference_backbuffer = backbuffer;
  g_effects_open = false;
}

inline bool MatchesLocked(uint64_t device, uint64_t backbuffer) {
  return device != 0 && backbuffer != 0 && g_state.device == device &&
      g_state.reference_backbuffer == backbuffer;
}

inline void ObservePresent(uint64_t swapchain, uint64_t resource) {
  std::lock_guard lock(g_mutex);
  if (swapchain == 0 || swapchain != g_state.swapchain) return;
  ++g_state.source_present_calls;
  g_state.presented_resource = resource;
}

inline void ObserveEffects(bool begin, uint64_t device, uint64_t backbuffer,
                           uint64_t runtime, uint64_t resource) {
  std::lock_guard lock(g_mutex);
  if (!MatchesLocked(device, backbuffer) || runtime == 0) return;
  if (begin) {
    ++g_state.effects_sequence;
    g_state.effects_runtime = runtime;
    g_state.before_effects_resource = resource;
    g_state.after_effects_resource = 0;
    g_state.effects_pair_observed = 0;
    g_effects_open = resource != 0;
  } else {
    g_state.effects_pair_observed = g_effects_open &&
        runtime == g_state.effects_runtime && resource != 0;
    g_state.after_effects_resource = g_state.effects_pair_observed ? resource : 0;
    g_effects_open = false;
  }
}

inline void ObserveOverlay(uint64_t device, uint64_t backbuffer) {
  std::lock_guard lock(g_mutex);
  if (MatchesLocked(device, backbuffer)) ++g_state.overlay_callbacks;
}

inline void ObserveScreenshot(uint64_t device, uint64_t backbuffer) {
  std::lock_guard lock(g_mutex);
  if (MatchesLocked(device, backbuffer)) ++g_state.screenshots_saved;
  // Saved-file notification does not specify which UI/generated layers it contains.
}

inline void SetScreenshotEventAvailable(bool available) {
  std::lock_guard lock(g_mutex);
  g_state.screenshot_event_available = available;
}

inline void InvalidateResource(uint64_t resource) {
  std::lock_guard lock(g_mutex);
  if (resource == 0) return;
  if (g_state.reference_backbuffer == resource) g_state.reference_backbuffer = 0;
  if (g_state.presented_resource == resource) g_state.presented_resource = 0;
  if (g_state.before_effects_resource == resource || g_state.after_effects_resource == resource) {
    g_state.before_effects_resource = 0;
    g_state.after_effects_resource = 0;
    g_state.effects_pair_observed = 0;
    g_effects_open = false;
  }
}

inline void InvalidateEffects(uint64_t runtime) {
  std::lock_guard lock(g_mutex);
  if (runtime != g_state.effects_runtime) return;
  g_state.effects_runtime = 0;
  g_state.before_effects_resource = 0;
  g_state.after_effects_resource = 0;
  g_state.effects_pair_observed = 0;
  g_effects_open = false;
}

inline Snapshot Read() {
  std::lock_guard lock(g_mutex);
  return g_state;
}

}  // namespace mfgunlock::composition
