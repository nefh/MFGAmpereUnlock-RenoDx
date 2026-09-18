// SPDX-License-Identifier: MIT
#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>

#include "../mfgunlock/diagnostic_bridge.hpp"

namespace mfgdiagnostics::timeline {
using mfgunlock::diagnostic::EvaluateEvent;
using mfgunlock::diagnostic::EvaluatePhase;
using mfgunlock::diagnostic::ResourceKey;

enum class FrameKind : uint32_t { kUnknown = 0, kSource = 1, kGenerated = 2, kReset = 3 };

struct Classification {
  FrameKind kind = FrameKind::kUnknown;
  uint32_t generated_index = 0;
  uint32_t generated_count = 0;
};

inline const mfgunlock::diagnostic::ResourceSnapshot* FindResource(
    const EvaluateEvent& event, ResourceKey key) {
  const auto index = static_cast<size_t>(key);
  return index < event.resources.size() ? &event.resources[index] : nullptr;
}

inline bool ResetTrue(const EvaluateEvent& event) {
  return (event.reset_known && event.reset != 0) ||
      (event.automode_reset_known && event.automode_reset != 0);
}

inline bool ResetKnown(const EvaluateEvent& event) {
  return ResetTrue(event) || (event.reset_known && event.automode_reset_known);
}

inline Classification Classify(const EvaluateEvent& event) {
  if (ResetTrue(event)) return {FrameKind::kReset, 0, 0};
  if (!ResetKnown(event) || !event.generated_count_known || !event.generated_index_known ||
      event.generated_count == 0 || event.generated_index == 0 ||
      event.generated_index > event.generated_count) return {};
  const auto* output = FindResource(event, ResourceKey::kOutputInterpolated);
  if (!output || !output->known || output->object == 0) return {};
  return {FrameKind::kGenerated, event.generated_index, event.generated_count};
}

inline const char* FrameKindName(FrameKind kind) {
  switch (kind) {
    case FrameKind::kSource: return "source";
    case FrameKind::kGenerated: return "generated";
    case FrameKind::kReset: return "reset";
    default: return "unknown";
  }
}

inline std::string MarkerLabel(const Classification& classification) {
  if (classification.kind != FrameKind::kGenerated || classification.generated_index == 0 ||
      classification.generated_count == 0) return {};
  return "FG " + std::to_string(classification.generated_index) + "/" +
      std::to_string(classification.generated_count);
}

// One-shot metadata capture. It never owns GPU resources; snapshots contain
// only values observed synchronously at the provider Evaluate boundary.
class OneShotCapture {
 public:
  void Arm() {
    std::lock_guard lock(mutex_);
    evaluation_id_ = 0;
    begin_ = {};
    end_ = {};
    complete_.store(false, std::memory_order_relaxed);
    armed_.store(true, std::memory_order_release);
  }

  void Cancel() {
    armed_.store(false, std::memory_order_release);
    std::lock_guard lock(mutex_);
    complete_.store(false, std::memory_order_relaxed);
    evaluation_id_ = 0;
  }

  bool Observe(const EvaluateEvent& event) {
    if (!armed_.load(std::memory_order_acquire)) return false;
    std::lock_guard lock(mutex_);
    if (!armed_.load(std::memory_order_relaxed) || complete_.load(std::memory_order_relaxed))
      return false;
    if (event.phase == EvaluatePhase::kBegin) {
      if (evaluation_id_ != 0) return false;
      evaluation_id_ = event.evaluation_id;
      begin_ = event;
      return true;
    }
    if (event.phase != EvaluatePhase::kEnd || evaluation_id_ == 0 ||
        event.evaluation_id != evaluation_id_) return false;
    end_ = event;
    complete_.store(true, std::memory_order_release);
    armed_.store(false, std::memory_order_release);
    return true;
  }

  bool Armed() const { return armed_.load(std::memory_order_acquire); }

  bool Complete() const { return complete_.load(std::memory_order_acquire); }

  bool Snapshot(EvaluateEvent& begin, EvaluateEvent& end) const {
    if (!complete_.load(std::memory_order_acquire)) return false;
    std::lock_guard lock(mutex_);
    if (!complete_.load(std::memory_order_relaxed)) return false;
    begin = begin_;
    end = end_;
    return true;
  }

 private:
  mutable std::mutex mutex_;
  std::atomic_bool armed_{false};
  std::atomic_bool complete_{false};
  uint64_t evaluation_id_ = 0;
  EvaluateEvent begin_{};
  EvaluateEvent end_{};
};
}  // namespace mfgdiagnostics::timeline
