// SPDX-License-Identifier: MIT
// Manual read-only v4 query, executed synchronously on the native GetState path.
#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>

#include <sl.h>
#include <sl_dlss_g.h>

namespace mfgdiagnostics::dynamicprobe {

using GetState = sl::Result (*)(const sl::ViewportHandle&, sl::DLSSGState&,
                                const sl::DLSSGOptions*);

inline bool KnownState(const sl::DLSSGState& state) {
  return state.structType == sl::DLSSGState::s_structType &&
         state.structVersion >= sl::kStructVersion1 &&
         state.structVersion <= sl::kStructVersion4;
}

inline bool Capability(const sl::DLSSGState& state, sl::Result result) {
  return result == sl::Result::eOk && KnownState(state) &&
         state.structVersion == sl::kStructVersion4 &&
         (state.bIsDynamicMFGSupported == sl::Boolean::eTrue ||
          state.bIsDynamicMFGSupported == sl::Boolean::eFalse);
}

inline uint32_t AddPresented(uint32_t first, uint32_t second) {
  return UINT32_MAX - first < second ? UINT32_MAX : first + second;
}

// Copy only the public fields belonging to the actual source allocation. The
// second query must not replay the game's extension chain or error callback.
inline bool CopyQueryOptions(const sl::DLSSGOptions* source, sl::DLSSGOptions& copy) {
  if (!source || source->structType != sl::DLSSGOptions::s_structType ||
      source->structVersion < sl::kStructVersion1 ||
      source->structVersion > sl::kStructVersion5) return false;
  copy = sl::DLSSGOptions{};
  copy.structVersion = source->structVersion;
  copy.next = nullptr;
  copy.mode = source->mode;
  copy.numFramesToGenerate = source->numFramesToGenerate;
  copy.flags = source->flags;
  copy.dynamicResWidth = source->dynamicResWidth;
  copy.dynamicResHeight = source->dynamicResHeight;
  copy.numBackBuffers = source->numBackBuffers;
  copy.mvecDepthWidth = source->mvecDepthWidth;
  copy.mvecDepthHeight = source->mvecDepthHeight;
  copy.colorWidth = source->colorWidth;
  copy.colorHeight = source->colorHeight;
  copy.colorBufferFormat = source->colorBufferFormat;
  copy.mvecBufferFormat = source->mvecBufferFormat;
  copy.depthBufferFormat = source->depthBufferFormat;
  copy.hudLessBufferFormat = source->hudLessBufferFormat;
  copy.uiBufferFormat = source->uiBufferFormat;
  copy.onErrorCallback = nullptr;
  if (source->structVersion >= sl::kStructVersion2) copy.bReserved15 = source->bReserved15;
  if (source->structVersion >= sl::kStructVersion3)
    copy.queueParallelismMode = source->queueParallelismMode;
  if (source->structVersion >= sl::kStructVersion4)
    copy.enableUserInterfaceRecomposition = source->enableUserInterfaceRecomposition;
  if (source->structVersion >= sl::kStructVersion5)
    copy.dynamicTargetFrameRate = source->dynamicTargetFrameRate;
  return true;
}

struct Snapshot {
  bool seen = false;
  bool supported = false;
  bool from_probe = false;
  bool attempted = false;
  uint32_t result = UINT32_MAX;
  uint32_t state_version = 0;
  uint32_t viewport = UINT32_MAX;
  uint64_t generation = 0;
};

class ManualProbe {
 public:
  bool Armed() const { return armed_.load(std::memory_order_acquire) != 0; }

  void Arm() {
    std::lock_guard lock(mutex_);
    snapshot_ = {};
    snapshot_.generation = generation_.load(std::memory_order_relaxed);
    armed_.store(++request_, std::memory_order_release);
  }

  void Invalidate() {
    std::lock_guard lock(mutex_);
    generation_.fetch_add(1, std::memory_order_release);
    armed_.store(0, std::memory_order_release);
    snapshot_ = {};
    pending_ = {};
    has_pending_.store(false, std::memory_order_release);
  }

  Snapshot Read() const {
    std::lock_guard lock(mutex_);
    return snapshot_;
  }

  // Current cheaply validates the observed binding. Acquire must hold its
  // module for the optional extra call and runs only when a probe is armed.
  // Trace records BEGIN before the call and its actual result afterwards.
  template <class Current, class Acquire, class Trace>
  sl::Result OnGetState(GetState real, const sl::ViewportHandle& viewport,
                       sl::DLSSGState& state, const sl::DLSSGOptions* options,
                       Current&& current, Acquire&& acquire, Trace&& trace) {
    if (!real) return sl::Result::eErrorNotInitialized;
    const auto generation = generation_.load(std::memory_order_acquire);
    const auto result = real(viewport, state, options);
    if (generation_.load(std::memory_order_acquire) != generation || !current()) return result;
    const uint32_t id = static_cast<uint32_t>(viewport);
    const bool writable = result == sl::Result::eOk && KnownState(state);
    if (writable && has_pending_.load(std::memory_order_acquire)) {
      std::lock_guard lock(mutex_);
      if (generation_.load(std::memory_order_relaxed) == generation) {
        for (auto& pending : pending_) {
          if (pending.used && !pending.inflight && pending.real == real &&
              pending.viewport == id) {
            state.numFramesActuallyPresented = AddPresented(
                state.numFramesActuallyPresented, pending.presented);
            pending = {};
          }
        }
        RefreshPending();
      }
    }
    if (Capability(state, result)) {
      std::unique_lock lock(mutex_, std::try_to_lock);
      // Keep the last explicit probe result visible until it is rearmed or its
      // generation ends. A default false in a later game v4 buffer cannot erase
      // an unwritten sentinel from the owned query.
      if (lock.owns_lock() && !snapshot_.attempted &&
          generation_.load(std::memory_order_relaxed) == generation) {
        snapshot_ = {true, state.bIsDynamicMFGSupported == sl::Boolean::eTrue,
                     false, false, static_cast<uint32_t>(result),
                     static_cast<uint32_t>(state.structVersion), id, generation};
      }
    }
    uint64_t request = armed_.load(std::memory_order_acquire);
    if (!request || !acquire() || !current()) return result;
    size_t reserved = pending_.size();
    {
      std::lock_guard lock(mutex_);
      if (generation_.load(std::memory_order_relaxed) != generation || !current()) return result;
      for (size_t i = 0; i < pending_.size(); ++i) {
        if (!pending_[i].used) { reserved = i; break; }
      }
      // Reserve storage before any consuming query. The user can retry once a
      // native query has drained earlier counts; silently losing them is wrong.
      if (reserved == pending_.size() ||
          !armed_.compare_exchange_strong(request, 0, std::memory_order_acq_rel)) return result;
      pending_[reserved] = {true, true, real, id, 0, request};
      if (request_ == request)
        snapshot_ = {false, false, true, true, UINT32_MAX, 4, id, generation};
    }

    sl::DLSSGState owned{};
    owned.structVersion = sl::kStructVersion4;
    owned.next = nullptr;
    owned.bIsDynamicMFGSupported = static_cast<sl::Boolean>(UINT32_MAX);
    sl::DLSSGOptions copied{};
    const auto* query_options = CopyQueryOptions(options, copied) ? &copied : nullptr;
    trace(true, sl::Result::eOk);
    const auto probe_result = real(viewport, owned, query_options);
    trace(false, probe_result);
    const uint32_t consumed = probe_result == sl::Result::eOk && KnownState(owned)
        ? owned.numFramesActuallyPresented : 0;
    // Both queries belong to this same call/viewport/function even if a device
    // callback invalidated global diagnostics during the second query.
    if (writable)
      state.numFramesActuallyPresented = AddPresented(state.numFramesActuallyPresented, consumed);
    {
      std::lock_guard lock(mutex_);
      if (generation_.load(std::memory_order_relaxed) != generation || !current()) return result;
      auto& pending = pending_[reserved];
      if (pending.used && pending.request == request) {
        if (writable || consumed == 0) pending = {};
        else {
          pending.inflight = false;
          pending.presented = consumed;
        }
      }
      RefreshPending();
      if (request_ == request) {
        const bool seen = Capability(owned, probe_result);
        snapshot_ = {seen, seen && owned.bIsDynamicMFGSupported == sl::Boolean::eTrue,
                     true, true, static_cast<uint32_t>(probe_result),
                     static_cast<uint32_t>(owned.structVersion), id, generation};
      }
    }
    return result;
  }

 private:
  struct Pending {
    bool used = false;
    bool inflight = false;
    GetState real = nullptr;
    uint32_t viewport = UINT32_MAX;
    uint32_t presented = 0;
    uint64_t request = 0;
  };
  void RefreshPending() {
    bool any = false;
    for (const auto& pending : pending_) any |= pending.used && !pending.inflight;
    has_pending_.store(any, std::memory_order_release);
  }
  mutable std::mutex mutex_;
  std::atomic<uint64_t> generation_{1};
  std::atomic<uint64_t> armed_{0};
  std::atomic_bool has_pending_{false};
  uint64_t request_ = 0;
  Snapshot snapshot_{};
  std::array<Pending, 8> pending_{};
};

}  // namespace mfgdiagnostics::dynamicprobe
