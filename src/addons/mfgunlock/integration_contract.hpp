// SPDX-License-Identifier: MIT
#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

namespace mfgunlock::integration {

// Evidence labels used by the Stage 4 contract audit. UNKNOWN stays distinct
// from false/invalid; OBSERVED means the integration point was seen but there
// is not enough public ABI evidence to prove validity.
enum class Verdict : uint32_t {
  kUnknown = 0,
  kObserved = 1,
  kValid = 2,
  kInvalid = 3,
};

inline constexpr uint32_t kUnknown32 = UINT32_MAX;
inline constexpr uint64_t kUnknown64 = UINT64_MAX;

// Public DLSS-G status bits from Streamline 2.14.1 sl_dlss_g.h. Keep them as
// raw bits here so older/newer header names cannot collapse unknown failures.
inline constexpr uint32_t kStatusResolutionTooLow = 1u << 0;
inline constexpr uint32_t kStatusReflexMissing = 1u << 1;
inline constexpr uint32_t kStatusHdrUnsupported = 1u << 2;
inline constexpr uint32_t kStatusConstantsInvalid = 1u << 3;
inline constexpr uint32_t kStatusBackBufferIndexMissing = 1u << 4;
inline constexpr uint32_t kStatusReserved5 = 1u << 5;
inline constexpr uint32_t kKnownStatusMask =
    kStatusResolutionTooLow | kStatusReflexMissing | kStatusHdrUnsupported |
    kStatusConstantsInvalid | kStatusBackBufferIndexMissing | kStatusReserved5;

inline constexpr uint32_t kDynamicResolutionFlag = 1u << 1;

// Roles observable either through public Streamline resource tags or through
// the NGX parameter surface already captured by the addon. Output roles are NGX
// observations, not Streamline tag requirements.
enum class ResourceRole : uint32_t {
  kBackbuffer = 0,
  kDepth,
  kMotionVectors,
  kHudLess,
  kUiColorAndAlpha,
  kUiAlpha,
  kBidirectionalDistortionField,
  kOutputInterpolated,
  kOutputReal,
  kOutputDisableInterpolation,
  kCount,
};

struct ResourceState {
  uint32_t seen = 0;
  uint32_t active = 0;
  uint32_t clears = 0;
  uint32_t lifecycle_known = 0;
  uint32_t lifecycle = kUnknown32;
  uint32_t extent_known = 0;
  uint32_t extent_left = 0;
  uint32_t extent_top = 0;
  uint32_t extent_width = 0;
  uint32_t extent_height = 0;
  uint32_t state_known = 0;
  uint32_t state = kUnknown32;
  uint32_t format_known = 0;
  uint32_t format = kUnknown32;
  uint64_t object = 0;
};

struct Snapshot {
  Verdict frame_index = Verdict::kUnknown;
  uint32_t constants_frame_known = 0;
  uint32_t constants_frame = 0;
  uint32_t present_start_known = 0;
  uint32_t present_start_frame = 0;
  uint32_t present_end_known = 0;
  uint32_t present_end_frame = 0;
  uint32_t present_marker_mismatches = 0;
  uint32_t constants_marker_mismatches = 0;

  Verdict resources = Verdict::kUnknown;
  std::array<ResourceState, static_cast<size_t>(ResourceRole::kCount)> resource{};

  Verdict dynamic_resolution = Verdict::kUnknown;
  uint32_t dynamic_resolution_enabled = 0;
  uint32_t dynamic_res_width = 0;
  uint32_t dynamic_res_height = 0;
  uint32_t mvec_depth_width = 0;
  uint32_t mvec_depth_height = 0;
  uint32_t color_width = 0;
  uint32_t color_height = 0;
  uint32_t num_back_buffers = 0;

  Verdict queue_contract = Verdict::kUnknown;
  uint32_t queue_parallelism_known = 0;
  uint32_t queue_parallelism_mode = kUnknown32;
  uint32_t completion_fence_known = 0;
  uint64_t completion_fence = 0;
  uint64_t completion_fence_value = 0;
  uint32_t presenting_queue_seen = 0;
  uint64_t presenting_queue = 0;
  uint32_t queue_destroy_seen = 0;

  Verdict swapchain = Verdict::kUnknown;
  uint32_t swapchain_init_count = 0;
  uint32_t swapchain_destroy_count = 0;
  uint32_t swapchain_present_count = 0;
  uint32_t swapchain_recreation_count = 0;
  uint32_t swapchain_resize_count = 0;
  uint32_t swapchain_buffer_count_known = 0;
  uint32_t swapchain_buffer_count = 0;
  uint32_t swapchain_color_space_known = 0;
  uint32_t swapchain_color_space = kUnknown32;
  uint32_t swapchain_format_known = 0;
  uint32_t swapchain_format = kUnknown32;
  uint32_t output_encoding_known = 0;
  uint32_t output_encoding = kUnknown32;
  uint32_t output_dlssg_supported = 0;
  uint32_t fullscreen_transition_known = 0;
  uint32_t waitable_object_ownership_known = 0;
  uint32_t iflip_known = 0;

  uint32_t status_known = 0;
  uint32_t status_raw = 0;
  uint32_t status_unknown_bits = 0;
  uint32_t fail_resolution_too_low = 0;
  uint32_t fail_reflex_missing = 0;
  uint32_t fail_hdr_unsupported = 0;
  uint32_t fail_constants_invalid = 0;
  uint32_t fail_backbuffer_index_missing = 0;

  Verdict viewport_ownership = Verdict::kUnknown;
  uint32_t options_viewport_known = 0;
  uint32_t options_viewport = kUnknown32;
  uint32_t constants_viewport_known = 0;
  uint32_t constants_viewport = kUnknown32;
  uint32_t tag_viewport_known = 0;
  uint32_t tag_viewport = kUnknown32;
  uint32_t state_viewport_known = 0;
  uint32_t state_viewport = kUnknown32;
  uint32_t viewport_mismatches = 0;

  uint32_t set_options_thread_known = 0;
  uint32_t set_options_thread = 0;
  uint32_t present_thread_known = 0;
  uint32_t present_thread = 0;
  uint32_t set_options_on_present_thread = 0;

  uint32_t native_vsync_support_known = 0;
  uint32_t native_vsync_support = 0;
};

struct AtomicResourceState {
  std::atomic_uint32_t seen{0};
  std::atomic_uint32_t active{0};
  std::atomic_uint32_t clears{0};
  std::atomic_uint32_t lifecycle_known{0};
  std::atomic_uint32_t lifecycle{kUnknown32};
  std::atomic_uint32_t extent_known{0};
  std::atomic_uint32_t extent_left{0};
  std::atomic_uint32_t extent_top{0};
  std::atomic_uint32_t extent_width{0};
  std::atomic_uint32_t extent_height{0};
  std::atomic_uint32_t state_known{0};
  std::atomic_uint32_t state{kUnknown32};
  std::atomic_uint32_t format_known{0};
  std::atomic_uint32_t format{kUnknown32};
  std::atomic_uint64_t object{0};
};

inline std::array<AtomicResourceState, static_cast<size_t>(ResourceRole::kCount)> g_resources{};

inline std::atomic_uint32_t g_constants_frame_known{0};
inline std::atomic_uint32_t g_constants_frame{0};
inline std::array<std::atomic_uint32_t, 16> g_recent_constants_frames{};
inline std::atomic_uint32_t g_constants_sequence{0};
inline std::atomic_uint32_t g_present_start_known{0};
inline std::atomic_uint32_t g_present_start_frame{0};
inline std::atomic_uint32_t g_present_end_known{0};
inline std::atomic_uint32_t g_present_end_frame{0};
inline std::atomic_uint32_t g_present_marker_mismatches{0};
inline std::atomic_uint32_t g_constants_marker_mismatches{0};

inline std::atomic_uint32_t g_dynamic_resolution_seen{0};
inline std::atomic_uint32_t g_dynamic_resolution_enabled{0};
inline std::atomic_uint32_t g_dynamic_res_width{0};
inline std::atomic_uint32_t g_dynamic_res_height{0};
inline std::atomic_uint32_t g_mvec_depth_width{0};
inline std::atomic_uint32_t g_mvec_depth_height{0};
inline std::atomic_uint32_t g_color_width{0};
inline std::atomic_uint32_t g_color_height{0};
inline std::atomic_uint32_t g_num_back_buffers{0};

inline std::atomic_uint32_t g_queue_parallelism_known{0};
inline std::atomic_uint32_t g_queue_parallelism_mode{kUnknown32};
inline std::atomic_uint32_t g_completion_fence_known{0};
inline std::atomic_uint64_t g_completion_fence{0};
inline std::atomic_uint64_t g_completion_fence_value{0};
inline std::atomic_uint32_t g_presenting_queue_seen{0};
inline std::atomic_uint64_t g_presenting_queue{0};
inline std::atomic_uint32_t g_queue_destroy_seen{0};

inline std::atomic_uint32_t g_swapchain_init_count{0};
inline std::atomic_uint32_t g_swapchain_destroy_count{0};
inline std::atomic_uint32_t g_swapchain_present_count{0};
inline std::atomic_uint32_t g_swapchain_recreation_count{0};
inline std::atomic_uint32_t g_swapchain_resize_count{0};
inline std::atomic_uint64_t g_last_swapchain{0};
inline std::atomic_uint32_t g_swapchain_alive{0};
inline std::atomic_uint32_t g_swapchain_buffer_count_known{0};
inline std::atomic_uint32_t g_swapchain_buffer_count{0};
inline std::atomic_uint32_t g_swapchain_color_space_known{0};
inline std::atomic_uint32_t g_swapchain_color_space{kUnknown32};
inline std::atomic_uint32_t g_swapchain_format_known{0};
inline std::atomic_uint32_t g_swapchain_format{kUnknown32};
inline std::atomic_uint32_t g_output_encoding_known{0};
inline std::atomic_uint32_t g_output_encoding{kUnknown32};
inline std::atomic_uint32_t g_output_dlssg_supported{0};

inline std::atomic_uint32_t g_status_known{0};
inline std::atomic_uint32_t g_status_raw{0};

inline std::atomic_uint32_t g_options_viewport_known{0};
inline std::atomic_uint32_t g_options_viewport{kUnknown32};
inline std::atomic_uint32_t g_constants_viewport_known{0};
inline std::atomic_uint32_t g_constants_viewport{kUnknown32};
inline std::atomic_uint32_t g_tag_viewport_known{0};
inline std::atomic_uint32_t g_tag_viewport{kUnknown32};
inline std::atomic_uint32_t g_state_viewport_known{0};
inline std::atomic_uint32_t g_state_viewport{kUnknown32};
inline std::atomic_uint32_t g_viewport_mismatches{0};
inline std::atomic_uint32_t g_set_options_thread_known{0};
inline std::atomic_uint32_t g_set_options_thread{0};
inline std::atomic_uint32_t g_present_thread_known{0};
inline std::atomic_uint32_t g_present_thread{0};
inline std::atomic_uint32_t g_vsync_support_known{0};
inline std::atomic_uint32_t g_vsync_support{0};

inline void Reset() {
  for (auto& state : g_resources) {
    state.seen.store(0, std::memory_order_relaxed);
    state.active.store(0, std::memory_order_relaxed);
    state.clears.store(0, std::memory_order_relaxed);
    state.lifecycle_known.store(0, std::memory_order_relaxed);
    state.lifecycle.store(kUnknown32, std::memory_order_relaxed);
    state.extent_known.store(0, std::memory_order_relaxed);
    state.extent_left.store(0, std::memory_order_relaxed);
    state.extent_top.store(0, std::memory_order_relaxed);
    state.extent_width.store(0, std::memory_order_relaxed);
    state.extent_height.store(0, std::memory_order_relaxed);
    state.state_known.store(0, std::memory_order_relaxed);
    state.state.store(kUnknown32, std::memory_order_relaxed);
    state.format_known.store(0, std::memory_order_relaxed);
    state.format.store(kUnknown32, std::memory_order_relaxed);
    state.object.store(0, std::memory_order_relaxed);
  }
  g_constants_frame_known.store(0, std::memory_order_relaxed);
  g_constants_sequence.store(0, std::memory_order_relaxed);
  for (auto& frame : g_recent_constants_frames)
    frame.store(kUnknown32, std::memory_order_relaxed);
  g_present_start_known.store(0, std::memory_order_relaxed);
  g_present_end_known.store(0, std::memory_order_relaxed);
  g_present_marker_mismatches.store(0, std::memory_order_relaxed);
  g_constants_marker_mismatches.store(0, std::memory_order_relaxed);
  g_dynamic_resolution_seen.store(0, std::memory_order_relaxed);
  g_queue_parallelism_known.store(0, std::memory_order_relaxed);
  g_completion_fence_known.store(0, std::memory_order_relaxed);
  g_presenting_queue_seen.store(0, std::memory_order_relaxed);
  g_queue_destroy_seen.store(0, std::memory_order_relaxed);
  g_swapchain_init_count.store(0, std::memory_order_relaxed);
  g_swapchain_destroy_count.store(0, std::memory_order_relaxed);
  g_swapchain_present_count.store(0, std::memory_order_relaxed);
  g_swapchain_recreation_count.store(0, std::memory_order_relaxed);
  g_swapchain_resize_count.store(0, std::memory_order_relaxed);
  g_last_swapchain.store(0, std::memory_order_relaxed);
  g_swapchain_alive.store(0, std::memory_order_relaxed);
  g_swapchain_buffer_count_known.store(0, std::memory_order_relaxed);
  g_swapchain_color_space_known.store(0, std::memory_order_relaxed);
  g_swapchain_color_space.store(kUnknown32, std::memory_order_relaxed);
  g_swapchain_format_known.store(0, std::memory_order_relaxed);
  g_swapchain_format.store(kUnknown32, std::memory_order_relaxed);
  g_output_encoding_known.store(0, std::memory_order_relaxed);
  g_output_encoding.store(kUnknown32, std::memory_order_relaxed);
  g_output_dlssg_supported.store(0, std::memory_order_relaxed);
  g_status_known.store(0, std::memory_order_relaxed);
  g_options_viewport_known.store(0, std::memory_order_relaxed);
  g_constants_viewport_known.store(0, std::memory_order_relaxed);
  g_tag_viewport_known.store(0, std::memory_order_relaxed);
  g_state_viewport_known.store(0, std::memory_order_relaxed);
  g_viewport_mismatches.store(0, std::memory_order_relaxed);
  g_set_options_thread_known.store(0, std::memory_order_relaxed);
  g_present_thread_known.store(0, std::memory_order_relaxed);
  g_vsync_support_known.store(0, std::memory_order_relaxed);
}

inline void ObserveViewport(std::atomic_uint32_t& known,
                            std::atomic_uint32_t& stored, uint32_t viewport) {
  if (!known.exchange(1, std::memory_order_acq_rel)) {
    stored.store(viewport, std::memory_order_relaxed);
    return;
  }
  if (stored.load(std::memory_order_relaxed) != viewport)
    g_viewport_mismatches.fetch_add(1, std::memory_order_relaxed);
}

inline void ObserveSetOptions(uint32_t viewport, uint32_t flags,
                              uint32_t dynamic_width, uint32_t dynamic_height,
                              uint32_t mvec_depth_width, uint32_t mvec_depth_height,
                              uint32_t color_width, uint32_t color_height,
                              uint32_t num_back_buffers,
                              bool queue_mode_known, uint32_t queue_mode,
                              uint32_t thread) {
  ObserveViewport(g_options_viewport_known, g_options_viewport, viewport);
  g_set_options_thread.store(thread, std::memory_order_relaxed);
  g_set_options_thread_known.store(1, std::memory_order_release);
  g_dynamic_resolution_seen.store(1, std::memory_order_release);
  g_dynamic_resolution_enabled.store((flags & kDynamicResolutionFlag) != 0,
                                     std::memory_order_relaxed);
  g_dynamic_res_width.store(dynamic_width, std::memory_order_relaxed);
  g_dynamic_res_height.store(dynamic_height, std::memory_order_relaxed);
  g_mvec_depth_width.store(mvec_depth_width, std::memory_order_relaxed);
  g_mvec_depth_height.store(mvec_depth_height, std::memory_order_relaxed);
  g_color_width.store(color_width, std::memory_order_relaxed);
  g_color_height.store(color_height, std::memory_order_relaxed);
  g_num_back_buffers.store(num_back_buffers, std::memory_order_relaxed);
  if (queue_mode_known) {
    g_queue_parallelism_mode.store(queue_mode, std::memory_order_relaxed);
    g_queue_parallelism_known.store(1, std::memory_order_release);
  }
}

inline bool HasRecentConstantsFrame(uint32_t frame) {
  for (const auto& observed : g_recent_constants_frames)
    if (observed.load(std::memory_order_acquire) == frame) return true;
  return false;
}

inline void ObserveConstants(uint32_t frame, uint32_t viewport) {
  ObserveViewport(g_constants_viewport_known, g_constants_viewport, viewport);
  g_constants_frame.store(frame, std::memory_order_relaxed);
  g_constants_frame_known.store(1, std::memory_order_release);
  const uint32_t sequence = g_constants_sequence.fetch_add(1, std::memory_order_acq_rel);
  g_recent_constants_frames[sequence % g_recent_constants_frames.size()].store(
      frame, std::memory_order_release);
}

inline void ObservePresentMarker(bool start, uint32_t frame) {
  if (start) {
    g_present_start_frame.store(frame, std::memory_order_relaxed);
    g_present_start_known.store(1, std::memory_order_release);
    return;
  }
  g_present_end_frame.store(frame, std::memory_order_relaxed);
  g_present_end_known.store(1, std::memory_order_release);
  if (g_present_start_known.load(std::memory_order_acquire) &&
      g_present_start_frame.load(std::memory_order_relaxed) != frame) {
    g_present_marker_mismatches.fetch_add(1, std::memory_order_relaxed);
  }
  if (g_constants_sequence.load(std::memory_order_acquire) != 0 &&
      !HasRecentConstantsFrame(frame)) {
    g_constants_marker_mismatches.fetch_add(1, std::memory_order_relaxed);
  }
}

inline void ObserveTag(uint32_t viewport, ResourceRole role, bool resource_present,
                       bool lifecycle_known, uint32_t lifecycle,
                       bool extent_known, uint32_t left, uint32_t top,
                       uint32_t width, uint32_t height,
                       bool state_known, uint32_t state,
                       bool format_known, uint32_t format,
                       uint64_t object) {
  ObserveViewport(g_tag_viewport_known, g_tag_viewport, viewport);
  const auto index = static_cast<size_t>(role);
  if (index >= g_resources.size()) return;
  auto& slot = g_resources[index];
  slot.seen.store(1, std::memory_order_release);
  if (!resource_present) {
    slot.active.store(0, std::memory_order_release);
    slot.object.store(0, std::memory_order_relaxed);
    slot.clears.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  slot.active.store(1, std::memory_order_release);
  slot.object.store(object, std::memory_order_relaxed);
  slot.lifecycle_known.store(lifecycle_known, std::memory_order_relaxed);
  slot.lifecycle.store(lifecycle_known ? lifecycle : kUnknown32, std::memory_order_relaxed);
  slot.extent_known.store(extent_known, std::memory_order_relaxed);
  slot.extent_left.store(left, std::memory_order_relaxed);
  slot.extent_top.store(top, std::memory_order_relaxed);
  slot.extent_width.store(width, std::memory_order_relaxed);
  slot.extent_height.store(height, std::memory_order_relaxed);
  slot.state_known.store(state_known, std::memory_order_relaxed);
  slot.state.store(state_known ? state : kUnknown32, std::memory_order_relaxed);
  slot.format_known.store(format_known, std::memory_order_relaxed);
  slot.format.store(format_known ? format : kUnknown32, std::memory_order_relaxed);
}

inline void ObserveNgxResource(ResourceRole role, bool known, uint64_t object,
                               uint64_t width, uint32_t height, uint32_t format) {
  if (!known) return;
  const auto index = static_cast<size_t>(role);
  if (index >= g_resources.size()) return;
  auto& slot = g_resources[index];
  slot.seen.store(1, std::memory_order_release);
  slot.active.store(object != 0, std::memory_order_release);
  slot.object.store(object, std::memory_order_relaxed);
  if (object == 0) {
    slot.clears.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  const bool extent_known = width != 0 && height != 0;
  slot.extent_known.store(extent_known, std::memory_order_relaxed);
  slot.extent_left.store(0, std::memory_order_relaxed);
  slot.extent_top.store(0, std::memory_order_relaxed);
  slot.extent_width.store(
      static_cast<uint32_t>(width > UINT32_MAX ? UINT32_MAX : width),
      std::memory_order_relaxed);
  slot.extent_height.store(height, std::memory_order_relaxed);
  slot.format_known.store(format != 0, std::memory_order_relaxed);
  slot.format.store(format != 0 ? format : kUnknown32, std::memory_order_relaxed);
  // NGX snapshots have no Streamline lifecycle, state, or viewport evidence.
}

inline void ObserveState(uint32_t viewport, uint32_t status,
                         bool vsync_known, bool vsync_supported,
                         bool fence_known, uint64_t fence, uint64_t fence_value) {
  ObserveViewport(g_state_viewport_known, g_state_viewport, viewport);
  g_status_raw.store(status, std::memory_order_relaxed);
  g_status_known.store(1, std::memory_order_release);
  if (vsync_known) {
    g_vsync_support.store(vsync_supported, std::memory_order_relaxed);
    g_vsync_support_known.store(1, std::memory_order_release);
  }
  if (fence_known) {
    g_completion_fence.store(fence, std::memory_order_relaxed);
    g_completion_fence_value.store(fence_value, std::memory_order_relaxed);
    g_completion_fence_known.store(1, std::memory_order_release);
  }
}

inline void ObservePresentThread(uint32_t thread) {
  g_present_thread.store(thread, std::memory_order_relaxed);
  g_present_thread_known.store(1, std::memory_order_release);
}

inline void ObserveQueueInit(uint64_t queue) {
  g_presenting_queue.store(queue, std::memory_order_relaxed);
  g_presenting_queue_seen.store(queue != 0, std::memory_order_release);
}
inline void ObserveQueueDestroy(uint64_t queue) {
  if (queue != 0 && g_presenting_queue_seen.load(std::memory_order_acquire) &&
      g_presenting_queue.load(std::memory_order_relaxed) == queue) {
    g_presenting_queue.store(0, std::memory_order_relaxed);
    g_presenting_queue_seen.store(0, std::memory_order_release);
  }
  g_queue_destroy_seen.store(1, std::memory_order_release);
}

inline void ObserveSwapchainInit(uint64_t swapchain, uint32_t buffer_count,
                                 bool resize) {
  const uint64_t previous = g_last_swapchain.exchange(swapchain, std::memory_order_acq_rel);
  const bool was_alive = g_swapchain_alive.exchange(1, std::memory_order_acq_rel) != 0;
  g_swapchain_init_count.fetch_add(1, std::memory_order_relaxed);
  if (resize) g_swapchain_resize_count.fetch_add(1, std::memory_order_relaxed);
  if ((was_alive && previous != 0 && previous != swapchain) ||
      (!was_alive && previous != 0)) {
    g_swapchain_recreation_count.fetch_add(1, std::memory_order_relaxed);
  }
  if (buffer_count != 0) {
    g_swapchain_buffer_count.store(buffer_count, std::memory_order_relaxed);
    g_swapchain_buffer_count_known.store(1, std::memory_order_release);
  }
}
inline void ObserveSwapchainDestroy(uint64_t swapchain, bool resize) {
  g_swapchain_destroy_count.fetch_add(1, std::memory_order_relaxed);
  if (resize) g_swapchain_resize_count.fetch_add(1, std::memory_order_relaxed);
  if (g_last_swapchain.load(std::memory_order_relaxed) == swapchain)
    g_swapchain_alive.store(0, std::memory_order_release);
}
inline void ObserveSwapchainPresent(uint64_t swapchain, uint32_t thread) {
  g_swapchain_present_count.fetch_add(1, std::memory_order_relaxed);
  if (swapchain != 0) g_last_swapchain.store(swapchain, std::memory_order_relaxed);
  ObservePresentThread(thread);
}
inline void ObserveSwapchainOutput(uint32_t format, bool format_known,
                                  uint32_t color_space, bool color_space_known,
                                  uint32_t encoding, bool encoding_known,
                                  bool dlssg_supported) {
  g_swapchain_format.store(format_known ? format : kUnknown32, std::memory_order_relaxed);
  g_swapchain_format_known.store(format_known, std::memory_order_release);
  g_swapchain_color_space.store(
      color_space_known ? color_space : kUnknown32, std::memory_order_relaxed);
  g_swapchain_color_space_known.store(color_space_known, std::memory_order_release);
  g_output_encoding.store(encoding_known ? encoding : kUnknown32, std::memory_order_relaxed);
  g_output_encoding_known.store(encoding_known, std::memory_order_release);
  g_output_dlssg_supported.store(dlssg_supported, std::memory_order_release);
}

inline void ObserveSwapchainColorSpace(uint32_t color_space, bool known) {
  ObserveSwapchainOutput(kUnknown32, false, color_space, known,
                         kUnknown32, false, false);
}

inline Verdict FrameVerdict() {
  const uint32_t status = g_status_known.load(std::memory_order_acquire)
      ? g_status_raw.load(std::memory_order_relaxed) : 0;
  if ((status & (kStatusReflexMissing | kStatusConstantsInvalid)) != 0 ||
      g_present_marker_mismatches.load(std::memory_order_relaxed) != 0 ||
      g_constants_marker_mismatches.load(std::memory_order_relaxed) != 0)
    return Verdict::kInvalid;
  const bool start = g_present_start_known.load(std::memory_order_acquire) != 0;
  const bool end = g_present_end_known.load(std::memory_order_acquire) != 0;
  const bool constants = g_constants_frame_known.load(std::memory_order_acquire) != 0;
  if (start && end && constants &&
      g_present_start_frame.load(std::memory_order_relaxed) ==
          g_present_end_frame.load(std::memory_order_relaxed) &&
      HasRecentConstantsFrame(g_present_end_frame.load(std::memory_order_relaxed))) {
    return Verdict::kValid;
  }
  return (start || end || constants) ? Verdict::kObserved : Verdict::kUnknown;
}

inline Verdict ResourceVerdict() {
  const bool depth = g_resources[static_cast<size_t>(ResourceRole::kDepth)]
                         .seen.load(std::memory_order_acquire) != 0;
  const bool mvec = g_resources[static_cast<size_t>(ResourceRole::kMotionVectors)]
                        .seen.load(std::memory_order_acquire) != 0;
  const uint32_t status = g_status_known.load(std::memory_order_acquire)
      ? g_status_raw.load(std::memory_order_relaxed) : 0;
  if (status & kStatusConstantsInvalid) return Verdict::kInvalid;
  if (depth && mvec) return Verdict::kValid;
  bool any = false;
  for (const auto& slot : g_resources)
    any |= slot.seen.load(std::memory_order_acquire) != 0;
  return any ? Verdict::kObserved : Verdict::kUnknown;
}

inline Verdict DynamicResolutionVerdict() {
  if (!g_dynamic_resolution_seen.load(std::memory_order_acquire)) return Verdict::kUnknown;
  if (!g_dynamic_resolution_enabled.load(std::memory_order_relaxed)) return Verdict::kValid;
  const uint32_t width = g_dynamic_res_width.load(std::memory_order_relaxed);
  const uint32_t height = g_dynamic_res_height.load(std::memory_order_relaxed);
  // Public SL 2.14.1 explicitly allows 0/0 as the default-half-resolution
  // target, but a half-specified pair is not a coherent target.
  if ((width == 0) != (height == 0)) return Verdict::kInvalid;
  return Verdict::kValid;
}

inline Verdict QueueVerdict() {
  if (!g_queue_parallelism_known.load(std::memory_order_acquire) &&
      !g_presenting_queue_seen.load(std::memory_order_acquire) &&
      !g_completion_fence_known.load(std::memory_order_acquire))
    return Verdict::kUnknown;
  // Mode 1 (block no client queues) requires fence evidence for portable
  // correctness. D3D currently falls back to mode 0 in the public contract, so
  // lack of a fence on a D3D run remains observed rather than invalid here.
  if (g_queue_parallelism_known.load(std::memory_order_acquire) &&
      g_queue_parallelism_mode.load(std::memory_order_relaxed) == 1 &&
      g_completion_fence_known.load(std::memory_order_acquire))
    return Verdict::kValid;
  return Verdict::kObserved;
}

inline Verdict SwapchainVerdict() {
  if (g_swapchain_init_count.load(std::memory_order_acquire) == 0 &&
      g_swapchain_present_count.load(std::memory_order_acquire) == 0)
    return Verdict::kUnknown;
  if (g_status_known.load(std::memory_order_acquire) &&
      (g_status_raw.load(std::memory_order_relaxed) & kStatusBackBufferIndexMissing) != 0)
    return Verdict::kInvalid;
  return Verdict::kObserved;
}

inline Verdict ViewportVerdict() {
  if (g_viewport_mismatches.load(std::memory_order_relaxed) != 0)
    return Verdict::kInvalid;
  uint32_t reference = kUnknown32;
  bool any = false;
  const auto compare = [&](const std::atomic_uint32_t& known,
                           const std::atomic_uint32_t& value) {
    if (!known.load(std::memory_order_acquire)) return true;
    const uint32_t current = value.load(std::memory_order_relaxed);
    if (current == kUnknown32) return true;
    any = true;
    if (reference == kUnknown32) {
      reference = current;
      return true;
    }
    return reference == current;
  };
  if (!compare(g_options_viewport_known, g_options_viewport) ||
      !compare(g_constants_viewport_known, g_constants_viewport) ||
      !compare(g_tag_viewport_known, g_tag_viewport) ||
      !compare(g_state_viewport_known, g_state_viewport))
    return Verdict::kInvalid;
  return any ? Verdict::kObserved : Verdict::kUnknown;
}

inline Snapshot Read() {
  Snapshot out;
  out.frame_index = FrameVerdict();
  out.constants_frame_known = g_constants_frame_known.load(std::memory_order_acquire);
  out.constants_frame = g_constants_frame.load(std::memory_order_relaxed);
  out.present_start_known = g_present_start_known.load(std::memory_order_acquire);
  out.present_start_frame = g_present_start_frame.load(std::memory_order_relaxed);
  out.present_end_known = g_present_end_known.load(std::memory_order_acquire);
  out.present_end_frame = g_present_end_frame.load(std::memory_order_relaxed);
  out.present_marker_mismatches = g_present_marker_mismatches.load(std::memory_order_relaxed);
  out.constants_marker_mismatches = g_constants_marker_mismatches.load(std::memory_order_relaxed);

  out.resources = ResourceVerdict();
  for (size_t i = 0; i < g_resources.size(); ++i) {
    const auto& source = g_resources[i];
    auto& target = out.resource[i];
    target.seen = source.seen.load(std::memory_order_acquire);
    target.active = source.active.load(std::memory_order_relaxed);
    target.clears = source.clears.load(std::memory_order_relaxed);
    target.lifecycle_known = source.lifecycle_known.load(std::memory_order_relaxed);
    target.lifecycle = source.lifecycle.load(std::memory_order_relaxed);
    target.extent_known = source.extent_known.load(std::memory_order_relaxed);
    target.extent_left = source.extent_left.load(std::memory_order_relaxed);
    target.extent_top = source.extent_top.load(std::memory_order_relaxed);
    target.extent_width = source.extent_width.load(std::memory_order_relaxed);
    target.extent_height = source.extent_height.load(std::memory_order_relaxed);
    target.state_known = source.state_known.load(std::memory_order_relaxed);
    target.state = source.state.load(std::memory_order_relaxed);
    target.format_known = source.format_known.load(std::memory_order_relaxed);
    target.format = source.format.load(std::memory_order_relaxed);
    target.object = source.object.load(std::memory_order_relaxed);
  }

  out.dynamic_resolution = DynamicResolutionVerdict();
  out.dynamic_resolution_enabled = g_dynamic_resolution_enabled.load(std::memory_order_relaxed);
  out.dynamic_res_width = g_dynamic_res_width.load(std::memory_order_relaxed);
  out.dynamic_res_height = g_dynamic_res_height.load(std::memory_order_relaxed);
  out.mvec_depth_width = g_mvec_depth_width.load(std::memory_order_relaxed);
  out.mvec_depth_height = g_mvec_depth_height.load(std::memory_order_relaxed);
  out.color_width = g_color_width.load(std::memory_order_relaxed);
  out.color_height = g_color_height.load(std::memory_order_relaxed);
  out.num_back_buffers = g_num_back_buffers.load(std::memory_order_relaxed);

  out.queue_contract = QueueVerdict();
  out.queue_parallelism_known = g_queue_parallelism_known.load(std::memory_order_acquire);
  out.queue_parallelism_mode = g_queue_parallelism_mode.load(std::memory_order_relaxed);
  out.completion_fence_known = g_completion_fence_known.load(std::memory_order_acquire);
  out.completion_fence = g_completion_fence.load(std::memory_order_relaxed);
  out.completion_fence_value = g_completion_fence_value.load(std::memory_order_relaxed);
  out.presenting_queue_seen = g_presenting_queue_seen.load(std::memory_order_acquire);
  out.presenting_queue = g_presenting_queue.load(std::memory_order_relaxed);
  out.queue_destroy_seen = g_queue_destroy_seen.load(std::memory_order_acquire);

  out.swapchain = SwapchainVerdict();
  out.swapchain_init_count = g_swapchain_init_count.load(std::memory_order_relaxed);
  out.swapchain_destroy_count = g_swapchain_destroy_count.load(std::memory_order_relaxed);
  out.swapchain_present_count = g_swapchain_present_count.load(std::memory_order_relaxed);
  out.swapchain_recreation_count = g_swapchain_recreation_count.load(std::memory_order_relaxed);
  out.swapchain_resize_count = g_swapchain_resize_count.load(std::memory_order_relaxed);
  out.swapchain_buffer_count_known = g_swapchain_buffer_count_known.load(std::memory_order_acquire);
  out.swapchain_buffer_count = g_swapchain_buffer_count.load(std::memory_order_relaxed);
  out.swapchain_color_space_known = g_swapchain_color_space_known.load(std::memory_order_acquire);
  out.swapchain_color_space = g_swapchain_color_space.load(std::memory_order_relaxed);
  out.swapchain_format_known = g_swapchain_format_known.load(std::memory_order_acquire);
  out.swapchain_format = g_swapchain_format.load(std::memory_order_relaxed);
  out.output_encoding_known = g_output_encoding_known.load(std::memory_order_acquire);
  out.output_encoding = g_output_encoding.load(std::memory_order_relaxed);
  out.output_dlssg_supported = g_output_dlssg_supported.load(std::memory_order_acquire);
  // ReShade's current callback surface used here does not authoritatively expose
  // exclusive-fullscreen/IFLIP/waitable ownership. Keep these unknown.
  out.fullscreen_transition_known = 0;
  out.waitable_object_ownership_known = 0;
  out.iflip_known = 0;

  out.status_known = g_status_known.load(std::memory_order_acquire);
  out.status_raw = g_status_raw.load(std::memory_order_relaxed);
  out.status_unknown_bits = out.status_raw & ~kKnownStatusMask;
  out.fail_resolution_too_low = !!(out.status_raw & kStatusResolutionTooLow);
  out.fail_reflex_missing = !!(out.status_raw & kStatusReflexMissing);
  out.fail_hdr_unsupported = !!(out.status_raw & kStatusHdrUnsupported);
  out.fail_constants_invalid = !!(out.status_raw & kStatusConstantsInvalid);
  out.fail_backbuffer_index_missing = !!(out.status_raw & kStatusBackBufferIndexMissing);

  out.viewport_ownership = ViewportVerdict();
  out.options_viewport_known = g_options_viewport_known.load(std::memory_order_acquire);
  out.options_viewport = g_options_viewport.load(std::memory_order_relaxed);
  out.constants_viewport_known = g_constants_viewport_known.load(std::memory_order_acquire);
  out.constants_viewport = g_constants_viewport.load(std::memory_order_relaxed);
  out.tag_viewport_known = g_tag_viewport_known.load(std::memory_order_acquire);
  out.tag_viewport = g_tag_viewport.load(std::memory_order_relaxed);
  out.state_viewport_known = g_state_viewport_known.load(std::memory_order_acquire);
  out.state_viewport = g_state_viewport.load(std::memory_order_relaxed);
  out.viewport_mismatches = g_viewport_mismatches.load(std::memory_order_relaxed);
  out.set_options_thread_known = g_set_options_thread_known.load(std::memory_order_acquire);
  out.set_options_thread = g_set_options_thread.load(std::memory_order_relaxed);
  out.present_thread_known = g_present_thread_known.load(std::memory_order_acquire);
  out.present_thread = g_present_thread.load(std::memory_order_relaxed);
  out.set_options_on_present_thread = out.set_options_thread_known && out.present_thread_known &&
      out.set_options_thread == out.present_thread;
  out.native_vsync_support_known = g_vsync_support_known.load(std::memory_order_acquire);
  out.native_vsync_support = g_vsync_support.load(std::memory_order_relaxed);
  return out;
}

inline const char* VerdictName(Verdict verdict) {
  switch (verdict) {
    case Verdict::kUnknown: return "UNKNOWN";
    case Verdict::kObserved: return "OBSERVED";
    case Verdict::kValid: return "VALID";
    case Verdict::kInvalid: return "INVALID";
  }
  return "UNKNOWN";
}

}  // namespace mfgunlock::integration
