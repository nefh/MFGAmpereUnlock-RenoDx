// SPDX-License-Identifier: MIT
#pragma once

#include <cstdint>

namespace mfgunlock::uialphabackend {

// One slot owns its source SRV/reference and extracted alpha texture until the
// queue has completed extraction AND the copy recorded by SL for eOnlyValidNow.
// Neither SetTag success nor CPU Present is a GPU completion event.
struct PendingFrame {
  uint64_t source_resource = 0;
  uint64_t submission_queue = 0;
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t source_format = 0;
  uint32_t source_state = 0;
  uint64_t generation = 0;
  uint32_t submission_thread = 0;
  bool state_known = false;
  bool source_submitted = false;
  bool prepared = false;
  bool extraction_recorded = false;
};

struct State {
  uint64_t generation = 1;
  uint64_t prepare_count = 0;
  uint64_t extraction_count = 0;
  uint64_t extraction_failures = 0;
  uint64_t accepted_count = 0;
  uint64_t completion_fence_value = 0;
  bool gpu_pending = false;
  bool poisoned = false;
  PendingFrame pending{};
};

inline bool CanPrepare(uint64_t source_resource, uint64_t submission_queue,
                       uint32_t width, uint32_t height, uint32_t source_format,
                       bool state_known, bool source_submitted,
                       uint32_t submission_thread, uint32_t current_thread) {
  return source_resource != 0 && submission_queue != 0 && width != 0 &&
      height != 0 && source_format != 0 && state_known && source_submitted &&
      submission_thread != 0 && submission_thread == current_thread;
}

inline bool FenceComplete(uint64_t requested, uint64_t completed) {
  // UINT64_MAX means removed D3D12 device, not successful completion.
  return requested != 0 && requested != UINT64_MAX && completed != UINT64_MAX &&
      completed >= requested;
}

inline bool Reclaim(State& state, uint64_t completed) {
  if (state.poisoned || state.pending.prepared) return false;
  if (state.gpu_pending) {
    if (!FenceComplete(state.completion_fence_value, completed)) return false;
    state.gpu_pending = false;
    state.completion_fence_value = 0;
  }
  return true;
}

inline bool Prepare(State& state, uint64_t source_resource,
                    uint64_t submission_queue, uint32_t width,
                    uint32_t height, uint32_t source_format,
                    uint32_t source_state, bool state_known,
                    bool source_submitted, uint32_t submission_thread) {
  if (state.gpu_pending || state.poisoned || state.pending.prepared ||
      !CanPrepare(source_resource, submission_queue, width, height, source_format,
                  state_known, source_submitted, submission_thread, submission_thread))
    return false;
  ++state.prepare_count;
  if (++state.generation == 0) ++state.generation;
  state.pending = {source_resource, submission_queue, width, height, source_format,
      source_state, state.generation, submission_thread, state_known,
      source_submitted, true, false};
  return true;
}

inline bool ReadyForExtraction(const State& state) {
  return !state.poisoned && !state.gpu_pending && state.pending.prepared &&
      state.pending.state_known && state.pending.source_submitted &&
      state.pending.submission_queue != 0 && state.pending.generation == state.generation;
}

inline void FinishExtraction(State& state, bool recorded) {
  if (!ReadyForExtraction(state)) return;
  if (recorded) {
    state.pending.extraction_recorded = true;
    ++state.extraction_count;
  } else {
    ++state.extraction_failures;
    state.pending = {};
  }
}

inline bool ReadyToTag(const State& state) {
  return ReadyForExtraction(state) && state.pending.extraction_recorded;
}

// Called exactly once after recording the optional tag, flushing the owned
// command list, and attempting its queue signal. Rejected tags can still have
// recorded a copy, so they obey exactly the same resource lifetime.
inline bool SealCopy(State& state, uint64_t ticket, bool submitted,
                     bool accepted, bool signaled, uint64_t fence_value) {
  if (!state.pending.prepared || !state.pending.extraction_recorded ||
      state.pending.generation != ticket) return false;
  const bool current = state.generation == ticket;
  state.gpu_pending = true;
  state.poisoned = !submitted || !signaled || fence_value == 0 || fence_value == UINT64_MAX;
  state.completion_fence_value = state.poisoned ? 0 : fence_value;
  state.pending = {};
  if (accepted && current && !state.poisoned) ++state.accepted_count;
  return accepted && current && !state.poisoned;
}

inline void Invalidate(State& state) {
  if (++state.generation == 0) ++state.generation;
  // Do not discard an in-flight lease, fence or poison. A matching stale
  // completion must still seal its GPU lifetime, but cannot publish Ready.
  if (!state.pending.extraction_recorded) state.pending = {};
}

}  // namespace mfgunlock::uialphabackend
