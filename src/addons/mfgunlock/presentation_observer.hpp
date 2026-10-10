// SPDX-License-Identifier: MIT
#pragma once

#include <atomic>
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <limits>
#include <mutex>

namespace mfgunlock::presentation {

// Optional F2 metrics, owned by the existing presentation observer. No second
// GetState consumer, no pacing control, no allocation or IO in the tap path.
namespace passive {
inline constexpr uint32_t kVersion = 1;
inline constexpr size_t kCapacity = 256;
inline constexpr uint32_t kWarmupSamples = 8;
inline constexpr uint64_t kMaxAgeNs = 2000000000ULL;
enum class CpuKind : uint32_t { kTag, kSetOptions, kGetState, kPresent, kCount };
inline constexpr size_t kCpuKinds = static_cast<size_t>(CpuKind::kCount);
struct Sample { uint64_t value = 0, at = 0, duration = 0; };
struct Ring {
  std::array<Sample, kCapacity> samples{};
  uint64_t total = 0;
  uint32_t size = 0, next = 0;
  void Push(Sample sample) {
    samples[next] = sample;
    next = (next + 1) % kCapacity;
    if (size < kCapacity) ++size;
    ++total;
  }
};
struct CpuStats {
  uint64_t total = 0, retained = 0, warmup = 0, overwritten = 0;
  uint64_t p50_ns = 0, p95_ns = 0, p99_ns = 0;
  uint64_t first_ns = 0, last_ns = 0, age_ns = UINT64_MAX;
  uint32_t valid = 0;
};
struct RateStats {
  uint64_t intervals = 0, count = 0, span_ns = 0, first_ns = 0, last_ns = 0;
  uint64_t age_ns = UINT64_MAX;
  double hz = 0.0;
  uint32_t valid = 0;
};
struct Snapshot {
  uint32_t bytes = sizeof(Snapshot), version = kVersion;
  uint32_t enabled = 0, capacity = kCapacity;
  uint64_t generation = 0, timestamp_ns = 0, lost_events = 0, invalid_events = 0;
  uint32_t native_viewport = UINT32_MAX, native_viewport_conflict = 0;
  uint32_t displayed_frames_known = 0, generated_frames_known = 0;
  uint32_t duplicates_known = 0, dropped_known = 0;
  RateStats source_callbacks{}, native_presented{};
  std::array<CpuStats, kCpuKinds> cpu{};
};
using Query = bool (*)(uint32_t, void*);
using Enable = void (*)(bool);
inline std::atomic_bool g_enabled{false};
inline std::atomic_uint64_t g_generation{1}, g_lost{0};
inline std::atomic_bool g_source_gap{false}, g_native_gap{false};
inline std::mutex g_lock;
inline std::array<Ring, kCpuKinds> g_cpu{};
inline std::array<uint32_t, kCpuKinds> g_warmup{};
inline Ring g_source{}, g_native{};
inline uint64_t g_last_source = 0, g_last_native = 0, g_invalid = 0;
inline uint32_t g_native_viewport = UINT32_MAX;
inline bool g_native_conflict = false;

inline uint64_t NowNs() {
  return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
      std::chrono::steady_clock::now().time_since_epoch()).count());
}
inline void Reset() {
  std::lock_guard guard(g_lock);
  g_generation.fetch_add(1, std::memory_order_acq_rel);
  g_cpu = {}; g_warmup = {}; g_source = {}; g_native = {};
  g_last_source = 0; g_last_native = 0; g_invalid = 0;
  g_native_viewport = UINT32_MAX; g_native_conflict = false;
  g_lost.store(0, std::memory_order_relaxed);
  g_source_gap.store(false); g_native_gap.store(false);
}
inline void SetEnabled(bool enabled) {
  g_enabled.store(false, std::memory_order_release);
  Reset();
  g_enabled.store(enabled, std::memory_order_release);
}
inline bool Fresh(uint64_t now, uint64_t at) {
  return at != 0 && now >= at && now - at <= kMaxAgeNs;
}
inline void RecordCpu(CpuKind kind, uint64_t generation, uint64_t ns, uint64_t at) {
  if (!g_enabled.load(std::memory_order_relaxed)) return;
  std::unique_lock lock(g_lock, std::try_to_lock);
  if (!lock.owns_lock()) { g_lost.fetch_add(1, std::memory_order_relaxed); return; }
  const auto index = static_cast<size_t>(kind);
  if (generation != g_generation.load() || index >= kCpuKinds || at == 0) { ++g_invalid; return; }
  if (g_warmup[index] < kWarmupSamples) { ++g_warmup[index]; return; }
  g_cpu[index].Push({ns, at, 0});
}
inline void SourceTick(uint64_t at) {
  if (!g_enabled.load(std::memory_order_relaxed)) return;
  const uint64_t epoch = g_generation.load(std::memory_order_acquire);
  std::unique_lock lock(g_lock, std::try_to_lock);
  if (!lock.owns_lock()) { g_lost.fetch_add(1); g_source_gap.store(true); return; }
  if (!g_enabled.load(std::memory_order_acquire) || epoch != g_generation.load()) return;
  if (g_source_gap.exchange(false)) { ++g_invalid; g_source = {}; g_last_source = 0; }
  if (!at || (g_last_source && (at <= g_last_source || at - g_last_source > kMaxAgeNs))) {
    ++g_invalid; g_source = {}; g_last_source = at; return;
  }
  if (g_last_source) g_source.Push({1, at, at - g_last_source});
  g_last_source = at;
}
inline void NativeReport(uint64_t at, uint32_t viewport, uint32_t presented, bool valid,
                         uint64_t captured_epoch = 0) {
  if (!g_enabled.load(std::memory_order_relaxed)) return;
  const uint64_t epoch = g_generation.load(std::memory_order_acquire);
  std::unique_lock lock(g_lock, std::try_to_lock);
  if (!lock.owns_lock()) { g_lost.fetch_add(1); g_native_gap.store(true); return; }
  if (!g_enabled.load(std::memory_order_acquire) || epoch != g_generation.load()) return;
  if (captured_epoch && captured_epoch != epoch) { ++g_invalid; return; }
  // A dropped GetState tap consumed a counter we no longer know. Never divide
  // the next interval's count by a span containing that lost observation.
  if (g_native_gap.exchange(false)) { ++g_invalid; g_native = {}; g_last_native = 0; }
  if (g_native_viewport == UINT32_MAX) g_native_viewport = viewport;
  if (g_native_viewport != viewport) g_native_conflict = true;
  if (!valid || !at || presented == UINT32_MAX || g_native_conflict ||
      (g_last_native && (at <= g_last_native || at - g_last_native > kMaxAgeNs))) {
    ++g_invalid; g_native = {}; g_last_native = 0; return;
  }
  // First successful tap has no known start for the consumed native counter.
  if (g_last_native) g_native.Push({presented, at, at - g_last_native});
  g_last_native = at;
}
inline RateStats Rate(const Ring& ring, uint64_t now) {
  RateStats result{};
  for (uint32_t index = 0; index < ring.size; ++index) {
    const auto& sample = ring.samples[index];
    if (!sample.duration || !Fresh(now, sample.at)) continue;
    const uint64_t start = sample.at >= sample.duration ? sample.at - sample.duration : 0;
    if (!result.first_ns || start < result.first_ns) result.first_ns = start;
    result.last_ns = (std::max)(result.last_ns, sample.at);
    ++result.intervals; result.count += sample.value; result.span_ns += sample.duration;
  }
  if (result.last_ns && now >= result.last_ns) result.age_ns = now - result.last_ns;
  result.valid = result.intervals >= 2 && result.span_ns != 0;
  if (result.valid) result.hz = static_cast<double>(result.count) * 1e9 / static_cast<double>(result.span_ns);
  return result;
}
inline Snapshot Read(uint64_t now = NowNs()) {
  Snapshot result{};
  std::array<Ring,kCpuKinds> cpu;
  std::array<uint32_t,kCpuKinds> warmup;
  Ring source, native;
  {
    std::lock_guard guard(g_lock);
    result.enabled = g_enabled.load(); result.generation = g_generation.load();
    result.timestamp_ns = now; result.lost_events = g_lost.load(); result.invalid_events = g_invalid;
    result.native_viewport = g_native_viewport; result.native_viewport_conflict = g_native_conflict;
    cpu = g_cpu; warmup = g_warmup; source = g_source; native = g_native;
  }
  if (!result.enabled) return result;
  // All filtering and sorting outside the tap lock and outside the frame hooks.
  result.source_callbacks = Rate(source, now);
  result.native_presented = Rate(native, now);
  for (size_t kind=0; kind<kCpuKinds; ++kind) {
    const auto& ring = cpu[kind]; auto& stats = result.cpu[kind];
    stats.total = ring.total; stats.warmup = warmup[kind];
    stats.overwritten = ring.total > kCapacity ? ring.total - kCapacity : 0;
    std::array<uint64_t,kCapacity> sorted{};
    for(uint32_t i=0;i<ring.size;++i) {
      const auto& value=ring.samples[i];
      if (!Fresh(now,value.at)) continue;
      sorted[stats.retained++] = value.value;
      if (!stats.first_ns || value.at < stats.first_ns) stats.first_ns = value.at;
      stats.last_ns = (std::max)(stats.last_ns, value.at);
    }
    if (stats.last_ns && now >= stats.last_ns) stats.age_ns = now - stats.last_ns;
    if (!stats.retained) continue;
    std::sort(sorted.begin(), sorted.begin()+static_cast<size_t>(stats.retained));
    const auto quantile=[&](uint64_t percent) { return sorted[(stats.retained*percent+99)/100-1]; };
    stats.p50_ns=quantile(50);stats.p95_ns=quantile(95);stats.p99_ns=quantile(99);stats.valid=1;
  }
  return result;
}
struct ThreadSample {
  uint64_t begin = 0, excluded = 0, pause_begin = 0;
  uint32_t pauses = 0;
  void Pause(uint64_t at) { if (pauses++ == 0) pause_begin = at; }
  void Resume(uint64_t at) {
    if (pauses && --pauses == 0 && at >= pause_begin) excluded += at - pause_begin;
  }
};
// Fixed TLS storage, no pointers to stack objects escape into global state.
inline thread_local std::array<ThreadSample, 16> g_thread_samples{};
inline thread_local uint32_t g_thread_depth = 0;
struct CpuScope {
  CpuKind kind;
  uint64_t generation = 0;
  uint32_t slot = UINT32_MAX;
  explicit CpuScope(CpuKind value) : kind(value) {
    if (!g_enabled.load(std::memory_order_relaxed)) return;
    if (g_thread_depth == g_thread_samples.size()) { g_lost.fetch_add(1); return; }
    generation = g_generation.load(std::memory_order_acquire);
    slot = g_thread_depth++;
    g_thread_samples[slot] = {};
    g_thread_samples[slot].begin = NowNs();
  }
  CpuScope(const CpuScope&) = delete;
  ~CpuScope() {
    if (slot == UINT32_MAX) return;
    const auto end = NowNs();
    if (g_thread_depth != slot + 1) { g_lost.fetch_add(1); return; }
    const auto sample = g_thread_samples[slot];
    --g_thread_depth;
    if (end < sample.begin || sample.excluded > end - sample.begin) return;
    RecordCpu(kind, generation, end - sample.begin - sample.excluded, end);
  }
};
struct NativeCall {
  uint32_t count = g_thread_depth;
  NativeCall() {
    if (!count) return;
    const auto at = NowNs();
    for (uint32_t i=0; i<count; ++i) g_thread_samples[i].Pause(at);
  }
  NativeCall(const NativeCall&) = delete;
  ~NativeCall() {
    if (!count) return;
    const auto at = NowNs();
    for (uint32_t i=0; i<count; ++i) g_thread_samples[i].Resume(at);
  }
};
template<class Callable> inline auto CallNative(Callable&& call) {
  NativeCall excluded;
  return call();
}
inline const char* Name(CpuKind kind) {
  switch(kind){case CpuKind::kTag:return "Tag";case CpuKind::kSetOptions:return "SetOptions";
    case CpuKind::kGetState:return "GetState";case CpuKind::kPresent:return "Present callback";
    default:return "Unknown";}
}
} // namespace passive

inline constexpr uint32_t kVersion = 2;
inline constexpr uint32_t kUnknown32 = UINT32_MAX;
inline constexpr uint32_t kReliableSourceFrames = 32;

enum class ProviderMode : uint32_t {
  kUnknown = 0,
  kOff,
  kFixed,
  kAuto,
  kDynamic,
};

enum class Confidence : uint32_t {
  kUnknown = 0,
  kObserved,
  kReliable,
};

enum class Health : uint32_t {
  kUnknown = 0,
  kHealthy,
  kStarved,
};

enum class SwapEffect : uint32_t {
  kUnknown = 0,
  kDiscard,
  kSequential,
  kFlipSequential,
  kFlipDiscard,
};

enum class WindowMode : uint32_t {
  kUnknown = 0,
  kWindowedOrBorderless,
  kExclusiveFullscreen,
};

enum class EvidenceReason : uint32_t {
  kUnknown = 0,
  kAwaitingStateBaseline,
  kNoSourcePresents,
  kCorrelated,
  kFixedPresentationHealthy,
  kFixedPresentationStarved,
  kVariableProviderMode,
  kViewportMismatch,
  kSourceBindingUnknown,
};

struct Snapshot {
  uint32_t bytes = sizeof(Snapshot);
  uint32_t version = kVersion;

  uint64_t source_presents_total = 0;
  uint64_t provider_presented_total = 0;
  uint64_t correlated_source_presents = 0;
  uint64_t correlated_provider_presented = 0;
  uint32_t state_samples = 0;
  uint32_t reset_count = 0;

  uint32_t accepted_mode_known = 0;
  ProviderMode accepted_mode = ProviderMode::kUnknown;
  uint32_t accepted_multiplier_known = 0;
  uint32_t accepted_multiplier = 0;

  // Streamline exposes actually presented frames, but no public counter for
  // frames generated and later dropped. Keep generation itself unknown.
  uint32_t generated_multiplier_known = 0;
  uint32_t generated_multiplier_milli = 0;

  uint32_t effective_multiplier_known = 0;
  uint32_t effective_multiplier_milli = 0;
  Confidence confidence = Confidence::kUnknown;
  Health health = Health::kUnknown;
  EvidenceReason evidence_reason = EvidenceReason::kUnknown;

  uint32_t presented_interpolated_known = 0;
  uint64_t presented_interpolated_frames = 0;
  uint32_t dropped_interpolated_known = 0;
  uint64_t dropped_interpolated_frames = 0;

  uint32_t dxgi_known = 0;
  uint32_t swap_effect_known = 0;
  SwapEffect swap_effect = SwapEffect::kUnknown;
  uint32_t window_mode_known = 0;
  WindowMode window_mode = WindowMode::kUnknown;
  uint32_t waitable_flag_known = 0;
  uint32_t waitable_flag = 0;
  uint32_t waitable_object_known = 0;
  uint32_t waitable_object_available = 0;
  uint32_t max_frame_latency_known = 0;
  uint32_t max_frame_latency = 0;
  uint32_t window_mode_transition_count = 0;
  uint32_t max_frame_latency_change_count = 0;
  uint32_t allow_tearing_flag_known = 0;
  uint32_t allow_tearing_flag = 0;

  // ReShade's present callback does not expose per-call DXGI SyncInterval or
  // Present flags, and public DXGI has no authoritative IFLIP query.
  uint32_t sync_interval_known = 0;
  uint32_t sync_interval = 0;
  uint32_t present_flags_known = 0;
  uint32_t present_flags = 0;
  uint32_t iflip_known = 0;
  uint32_t iflip = 0;
  uint32_t source_binding_verified = 0;
};

using Query = bool (*)(uint32_t, void*);

inline std::mutex g_mutex;
inline uint32_t g_viewport = kUnknown32;
inline bool g_viewport_conflict = false;
inline bool g_source_binding_verified = false;

inline std::atomic_uint64_t g_source_presents_total{0};
inline std::atomic_uint64_t g_provider_presented_total{0};
inline std::atomic_uint64_t g_correlated_source_presents{0};
inline std::atomic_uint64_t g_correlated_provider_presented{0};
inline std::atomic_uint64_t g_last_state_source_total{0};
inline std::atomic_uint32_t g_state_samples{0};
inline std::atomic_uint32_t g_reset_count{0};
inline std::atomic_bool g_baseline_ready{false};

inline std::atomic_uint32_t g_accepted_mode_known{0};
inline std::atomic_uint32_t g_accepted_mode{static_cast<uint32_t>(ProviderMode::kUnknown)};
inline std::atomic_uint32_t g_accepted_multiplier_known{0};
inline std::atomic_uint32_t g_accepted_multiplier{0};

inline std::atomic_uint32_t g_effective_multiplier_known{0};
inline std::atomic_uint32_t g_effective_multiplier_milli{0};
inline std::atomic_uint32_t g_confidence{static_cast<uint32_t>(Confidence::kUnknown)};
inline std::atomic_uint32_t g_health{static_cast<uint32_t>(Health::kUnknown)};
inline std::atomic_uint32_t g_evidence_reason{static_cast<uint32_t>(EvidenceReason::kUnknown)};
inline std::atomic_uint32_t g_presented_interpolated_known{0};
inline std::atomic_uint64_t g_presented_interpolated_frames{0};
inline std::atomic_uint32_t g_dropped_interpolated_known{0};
inline std::atomic_uint64_t g_dropped_interpolated_frames{0};

inline std::atomic_uint32_t g_dxgi_known{0};
inline std::atomic_uint32_t g_swap_effect_known{0};
inline std::atomic_uint32_t g_swap_effect{static_cast<uint32_t>(SwapEffect::kUnknown)};
inline std::atomic_uint32_t g_window_mode_known{0};
inline std::atomic_uint32_t g_window_mode{static_cast<uint32_t>(WindowMode::kUnknown)};
inline std::atomic_uint32_t g_waitable_flag_known{0};
inline std::atomic_uint32_t g_waitable_flag{0};
inline std::atomic_uint32_t g_waitable_object_known{0};
inline std::atomic_uint32_t g_waitable_object_available{0};
inline std::atomic_uint32_t g_max_frame_latency_known{0};
inline std::atomic_uint32_t g_max_frame_latency{0};
inline std::atomic_uint32_t g_window_mode_transition_count{0};
inline std::atomic_uint32_t g_max_frame_latency_change_count{0};
inline std::atomic_uint32_t g_allow_tearing_flag_known{0};
inline std::atomic_uint32_t g_allow_tearing_flag{0};

inline uint32_t ClampMilli(uint64_t numerator, uint64_t denominator) {
  if (denominator == 0) return 0;
  const long double scaled = static_cast<long double>(numerator) * 1000.0L /
                             static_cast<long double>(denominator);
  if (scaled >= static_cast<long double>(UINT32_MAX)) return UINT32_MAX;
  return static_cast<uint32_t>(scaled + 0.5L);
}

inline void ClearCorrelationLocked(bool count_reset = true) {
  g_correlated_source_presents.store(0, std::memory_order_relaxed);
  g_correlated_provider_presented.store(0, std::memory_order_relaxed);
  g_last_state_source_total.store(g_source_presents_total.load(std::memory_order_relaxed),
                                  std::memory_order_relaxed);
  g_state_samples.store(0, std::memory_order_relaxed);
  g_baseline_ready.store(false, std::memory_order_release);
  g_effective_multiplier_known.store(0, std::memory_order_relaxed);
  g_effective_multiplier_milli.store(0, std::memory_order_relaxed);
  g_confidence.store(static_cast<uint32_t>(Confidence::kUnknown), std::memory_order_relaxed);
  g_health.store(static_cast<uint32_t>(Health::kUnknown), std::memory_order_relaxed);
  g_evidence_reason.store(static_cast<uint32_t>(EvidenceReason::kAwaitingStateBaseline),
                          std::memory_order_relaxed);
  g_presented_interpolated_known.store(0, std::memory_order_relaxed);
  g_presented_interpolated_frames.store(0, std::memory_order_relaxed);
  g_dropped_interpolated_known.store(0, std::memory_order_relaxed);
  g_dropped_interpolated_frames.store(0, std::memory_order_relaxed);
  if (count_reset) g_reset_count.fetch_add(1, std::memory_order_relaxed);
}

inline void ClearCorrelation(bool count_reset = true) {
  std::lock_guard lock(g_mutex);
  ClearCorrelationLocked(count_reset);
}

inline void Reset() {
  passive::Reset();
  std::lock_guard lock(g_mutex);
  g_viewport = kUnknown32;
  g_viewport_conflict = false;
  g_source_binding_verified = false;
  g_source_presents_total.store(0, std::memory_order_relaxed);
  g_provider_presented_total.store(0, std::memory_order_relaxed);
  g_accepted_mode_known.store(0, std::memory_order_relaxed);
  g_accepted_mode.store(static_cast<uint32_t>(ProviderMode::kUnknown),
                        std::memory_order_relaxed);
  g_accepted_multiplier_known.store(0, std::memory_order_relaxed);
  g_accepted_multiplier.store(0, std::memory_order_relaxed);
  g_reset_count.store(0, std::memory_order_relaxed);
  ClearCorrelationLocked(false);

  g_dxgi_known.store(0, std::memory_order_relaxed);
  g_swap_effect_known.store(0, std::memory_order_relaxed);
  g_swap_effect.store(static_cast<uint32_t>(SwapEffect::kUnknown), std::memory_order_relaxed);
  g_window_mode_known.store(0, std::memory_order_relaxed);
  g_window_mode.store(static_cast<uint32_t>(WindowMode::kUnknown), std::memory_order_relaxed);
  g_waitable_flag_known.store(0, std::memory_order_relaxed);
  g_waitable_flag.store(0, std::memory_order_relaxed);
  g_waitable_object_known.store(0, std::memory_order_relaxed);
  g_waitable_object_available.store(0, std::memory_order_relaxed);
  g_max_frame_latency_known.store(0, std::memory_order_relaxed);
  g_max_frame_latency.store(0, std::memory_order_relaxed);
  g_window_mode_transition_count.store(0, std::memory_order_relaxed);
  g_max_frame_latency_change_count.store(0, std::memory_order_relaxed);
  g_allow_tearing_flag_known.store(0, std::memory_order_relaxed);
  g_allow_tearing_flag.store(0, std::memory_order_relaxed);
}

inline void ObserveApplicationPresent() {
  if (passive::g_enabled.load(std::memory_order_relaxed)) passive::SourceTick(passive::NowNs());
  std::lock_guard lock(g_mutex);
  g_source_presents_total.fetch_add(1, std::memory_order_relaxed);
}

inline bool BindViewportLocked(uint32_t viewport) {
  if (g_viewport == kUnknown32) g_viewport = viewport;
  if (g_viewport != viewport) g_viewport_conflict = true;
  if (!g_viewport_conflict) return true;
  ClearCorrelationLocked();
  g_evidence_reason.store(static_cast<uint32_t>(EvidenceReason::kViewportMismatch),
                          std::memory_order_relaxed);
  return false;
}

// Source presents and provider counters must refer to the same viewport's
// backbuffer. A large sample alone does not establish that association.
inline void ObserveSourceBinding(uint32_t viewport, bool verified) {
  std::lock_guard lock(g_mutex);
  if (verified && !BindViewportLocked(viewport)) verified = false;
  if (g_source_binding_verified == verified) return;
  g_source_binding_verified = verified;
  ClearCorrelationLocked();
}

inline void ObserveProviderOptions(ProviderMode mode, uint32_t multiplier,
                                   uint32_t viewport = 0) {
  std::lock_guard lock(g_mutex);
  if (!BindViewportLocked(viewport)) return;
  const bool multiplier_known = mode == ProviderMode::kOff || mode == ProviderMode::kFixed;
  const uint32_t normalized = mode == ProviderMode::kOff ? 1u : multiplier;
  const bool changed = !g_accepted_mode_known.load(std::memory_order_acquire) ||
      g_accepted_mode.load(std::memory_order_relaxed) != static_cast<uint32_t>(mode) ||
      g_accepted_multiplier_known.load(std::memory_order_relaxed) !=
          static_cast<uint32_t>(multiplier_known) ||
      (multiplier_known && g_accepted_multiplier.load(std::memory_order_relaxed) != normalized);

  g_accepted_mode.store(static_cast<uint32_t>(mode), std::memory_order_relaxed);
  g_accepted_mode_known.store(mode != ProviderMode::kUnknown, std::memory_order_release);
  g_accepted_multiplier.store(multiplier_known ? normalized : 0, std::memory_order_relaxed);
  g_accepted_multiplier_known.store(multiplier_known, std::memory_order_release);
  if (changed) {
    ClearCorrelationLocked();
    if (passive::g_enabled.load(std::memory_order_relaxed)) passive::Reset();
  }
}

inline void ObserveProviderUnavailable() {
  std::lock_guard lock(g_mutex);
  // Do not short-circuit the second invalidation.
  const bool had_mode = g_accepted_mode_known.exchange(0, std::memory_order_acq_rel) != 0;
  const bool had_multiplier = g_accepted_multiplier_known.exchange(0, std::memory_order_acq_rel) != 0;
  const bool changed = had_mode || had_multiplier;
  g_accepted_mode.store(static_cast<uint32_t>(ProviderMode::kUnknown), std::memory_order_relaxed);
  g_accepted_multiplier.store(0, std::memory_order_relaxed);
  if (changed) ClearCorrelationLocked();
}

inline void UpdateCorrelationLocked() {
  const uint64_t source = g_correlated_source_presents.load(std::memory_order_relaxed);
  const uint64_t presented = g_correlated_provider_presented.load(std::memory_order_relaxed);
  if (source == 0) {
    g_effective_multiplier_known.store(0, std::memory_order_relaxed);
    g_confidence.store(static_cast<uint32_t>(Confidence::kUnknown), std::memory_order_relaxed);
    g_health.store(static_cast<uint32_t>(Health::kUnknown), std::memory_order_relaxed);
    g_evidence_reason.store(static_cast<uint32_t>(EvidenceReason::kNoSourcePresents),
                            std::memory_order_relaxed);
    return;
  }

  g_effective_multiplier_milli.store(ClampMilli(presented, source),
                                     std::memory_order_relaxed);
  g_effective_multiplier_known.store(1, std::memory_order_release);
  const Confidence confidence = source >= kReliableSourceFrames && g_source_binding_verified
      ? Confidence::kReliable : Confidence::kObserved;
  g_confidence.store(static_cast<uint32_t>(confidence), std::memory_order_relaxed);
  g_presented_interpolated_frames.store(presented > source ? presented - source : 0,
                                        std::memory_order_relaxed);
  g_presented_interpolated_known.store(g_source_binding_verified, std::memory_order_release);

  if (!g_source_binding_verified) {
    g_health.store(static_cast<uint32_t>(Health::kUnknown), std::memory_order_relaxed);
    g_dropped_interpolated_known.store(0, std::memory_order_release);
    g_evidence_reason.store(static_cast<uint32_t>(EvidenceReason::kSourceBindingUnknown),
                            std::memory_order_relaxed);
    return;
  }

  const auto mode = static_cast<ProviderMode>(
      g_accepted_mode.load(std::memory_order_relaxed));
  const bool fixed = g_accepted_mode_known.load(std::memory_order_acquire) &&
      mode == ProviderMode::kFixed &&
      g_accepted_multiplier_known.load(std::memory_order_acquire);
  if (!fixed) {
    g_health.store(static_cast<uint32_t>(Health::kUnknown), std::memory_order_relaxed);
    g_dropped_interpolated_known.store(0, std::memory_order_relaxed);
    g_evidence_reason.store(static_cast<uint32_t>(
        mode == ProviderMode::kAuto || mode == ProviderMode::kDynamic
            ? EvidenceReason::kVariableProviderMode
            : EvidenceReason::kCorrelated), std::memory_order_relaxed);
    return;
  }

  const uint64_t multiplier = g_accepted_multiplier.load(std::memory_order_relaxed);
  if (multiplier == 0 || source > UINT64_MAX / multiplier) {
    g_health.store(static_cast<uint32_t>(Health::kUnknown), std::memory_order_relaxed);
    g_dropped_interpolated_known.store(0, std::memory_order_release);
    return;
  }
  const uint64_t expected = source * multiplier;
  const uint64_t dropped = expected > presented ? expected - presented : 0;
  g_dropped_interpolated_frames.store(dropped, std::memory_order_relaxed);
  g_dropped_interpolated_known.store(1, std::memory_order_release);

  if (confidence != Confidence::kReliable) {
    g_health.store(static_cast<uint32_t>(Health::kUnknown), std::memory_order_relaxed);
    g_evidence_reason.store(static_cast<uint32_t>(EvidenceReason::kCorrelated),
                            std::memory_order_relaxed);
    return;
  }

  // The asynchronous pacer may straddle a GetState boundary by an entire
  // source-frame group. This is a delivery estimate, not display-timing proof.
  const bool starved = presented < expected && expected - presented > multiplier;
  g_health.store(static_cast<uint32_t>(starved ? Health::kStarved : Health::kHealthy),
                 std::memory_order_relaxed);
  g_evidence_reason.store(static_cast<uint32_t>(
      starved ? EvidenceReason::kFixedPresentationStarved
              : EvidenceReason::kFixedPresentationHealthy),
      std::memory_order_relaxed);
}

inline void ObserveProviderPresentation(uint32_t frames_presented_since_last_state,
                                         uint32_t viewport = 0) {
  std::lock_guard lock(g_mutex);
  if (!BindViewportLocked(viewport)) return;
  g_provider_presented_total.fetch_add(frames_presented_since_last_state,
                                       std::memory_order_relaxed);
  g_state_samples.fetch_add(1, std::memory_order_relaxed);
  const uint64_t current_source = g_source_presents_total.load(std::memory_order_relaxed);
  if (!g_baseline_ready.exchange(true, std::memory_order_acq_rel)) {
    g_last_state_source_total.store(current_source, std::memory_order_relaxed);
    g_evidence_reason.store(static_cast<uint32_t>(EvidenceReason::kAwaitingStateBaseline),
                            std::memory_order_relaxed);
    return;
  }

  const uint64_t previous_source = g_last_state_source_total.exchange(
      current_source, std::memory_order_acq_rel);
  if (current_source < previous_source) {
    ClearCorrelationLocked();
    return;
  }
  const uint64_t source_delta = current_source - previous_source;
  // GetState consumes its counter even if the application did not present.
  // Keep every post-baseline sample; dropping a zero-source interval loses
  // delayed pacer frames and creates false starvation.
  g_correlated_source_presents.fetch_add(source_delta, std::memory_order_relaxed);
  g_correlated_provider_presented.fetch_add(frames_presented_since_last_state,
                                            std::memory_order_relaxed);
  UpdateCorrelationLocked();
}

inline void ObserveDxgi(bool dxgi_known,
                        bool swap_effect_known, SwapEffect swap_effect,
                        bool window_mode_known, WindowMode window_mode,
                        bool waitable_flag_known, bool waitable_flag,
                        bool waitable_object_known, bool waitable_object_available,
                        bool max_frame_latency_known, uint32_t max_frame_latency,
                        bool allow_tearing_flag_known, bool allow_tearing_flag) {
  std::lock_guard lock(g_mutex);
  g_dxgi_known.store(dxgi_known, std::memory_order_release);
  g_swap_effect.store(static_cast<uint32_t>(swap_effect), std::memory_order_relaxed);
  g_swap_effect_known.store(swap_effect_known, std::memory_order_release);
  const bool previous_window_known = g_window_mode_known.load(std::memory_order_acquire) != 0;
  const uint32_t previous_window = g_window_mode.load(std::memory_order_relaxed);
  if (window_mode_known && previous_window_known &&
      previous_window != static_cast<uint32_t>(window_mode)) {
    g_window_mode_transition_count.fetch_add(1, std::memory_order_relaxed);
  }
  g_window_mode.store(static_cast<uint32_t>(window_mode), std::memory_order_relaxed);
  g_window_mode_known.store(window_mode_known, std::memory_order_release);
  g_waitable_flag.store(waitable_flag, std::memory_order_relaxed);
  g_waitable_flag_known.store(waitable_flag_known, std::memory_order_release);
  g_waitable_object_available.store(waitable_object_available, std::memory_order_relaxed);
  g_waitable_object_known.store(waitable_object_known, std::memory_order_release);
  const bool previous_latency_known =
      g_max_frame_latency_known.load(std::memory_order_acquire) != 0;
  const uint32_t previous_latency = g_max_frame_latency.load(std::memory_order_relaxed);
  if (max_frame_latency_known && previous_latency_known &&
      previous_latency != max_frame_latency) {
    g_max_frame_latency_change_count.fetch_add(1, std::memory_order_relaxed);
  }
  g_max_frame_latency.store(max_frame_latency, std::memory_order_relaxed);
  g_max_frame_latency_known.store(max_frame_latency_known, std::memory_order_release);
  g_allow_tearing_flag.store(allow_tearing_flag, std::memory_order_relaxed);
  g_allow_tearing_flag_known.store(allow_tearing_flag_known, std::memory_order_release);
}

inline Snapshot Read() {
  std::lock_guard lock(g_mutex);
  Snapshot out;
  out.source_presents_total = g_source_presents_total.load(std::memory_order_relaxed);
  out.provider_presented_total = g_provider_presented_total.load(std::memory_order_relaxed);
  out.correlated_source_presents = g_correlated_source_presents.load(std::memory_order_relaxed);
  out.correlated_provider_presented =
      g_correlated_provider_presented.load(std::memory_order_relaxed);
  out.state_samples = g_state_samples.load(std::memory_order_relaxed);
  out.reset_count = g_reset_count.load(std::memory_order_relaxed);
  out.accepted_mode_known = g_accepted_mode_known.load(std::memory_order_acquire);
  out.accepted_mode = static_cast<ProviderMode>(g_accepted_mode.load(std::memory_order_relaxed));
  out.accepted_multiplier_known = g_accepted_multiplier_known.load(std::memory_order_acquire);
  out.accepted_multiplier = g_accepted_multiplier.load(std::memory_order_relaxed);
  out.effective_multiplier_known = g_effective_multiplier_known.load(std::memory_order_acquire);
  out.effective_multiplier_milli = g_effective_multiplier_milli.load(std::memory_order_relaxed);
  out.confidence = static_cast<Confidence>(g_confidence.load(std::memory_order_relaxed));
  out.health = static_cast<Health>(g_health.load(std::memory_order_relaxed));
  out.evidence_reason = static_cast<EvidenceReason>(
      g_evidence_reason.load(std::memory_order_relaxed));
  out.presented_interpolated_known =
      g_presented_interpolated_known.load(std::memory_order_acquire);
  out.presented_interpolated_frames =
      g_presented_interpolated_frames.load(std::memory_order_relaxed);
  out.dropped_interpolated_known = g_dropped_interpolated_known.load(std::memory_order_acquire);
  out.dropped_interpolated_frames = g_dropped_interpolated_frames.load(std::memory_order_relaxed);

  out.source_binding_verified = g_source_binding_verified;
  out.dxgi_known = g_dxgi_known.load(std::memory_order_acquire);
  out.swap_effect_known = g_swap_effect_known.load(std::memory_order_acquire);
  out.swap_effect = static_cast<SwapEffect>(g_swap_effect.load(std::memory_order_relaxed));
  out.window_mode_known = g_window_mode_known.load(std::memory_order_acquire);
  out.window_mode = static_cast<WindowMode>(g_window_mode.load(std::memory_order_relaxed));
  out.waitable_flag_known = g_waitable_flag_known.load(std::memory_order_acquire);
  out.waitable_flag = g_waitable_flag.load(std::memory_order_relaxed);
  out.waitable_object_known = g_waitable_object_known.load(std::memory_order_acquire);
  out.waitable_object_available = g_waitable_object_available.load(std::memory_order_relaxed);
  out.max_frame_latency_known = g_max_frame_latency_known.load(std::memory_order_acquire);
  out.max_frame_latency = g_max_frame_latency.load(std::memory_order_relaxed);
  out.window_mode_transition_count =
      g_window_mode_transition_count.load(std::memory_order_relaxed);
  out.max_frame_latency_change_count =
      g_max_frame_latency_change_count.load(std::memory_order_relaxed);
  out.allow_tearing_flag_known = g_allow_tearing_flag_known.load(std::memory_order_acquire);
  out.allow_tearing_flag = g_allow_tearing_flag.load(std::memory_order_relaxed);
  return out;
}

inline const char* ProviderModeName(ProviderMode value) {
  switch (value) {
    case ProviderMode::kUnknown: return "UNKNOWN";
    case ProviderMode::kOff: return "OFF";
    case ProviderMode::kFixed: return "FIXED";
    case ProviderMode::kAuto: return "AUTO";
    case ProviderMode::kDynamic: return "DYNAMIC";
  }
  return "UNKNOWN";
}

inline const char* EvidenceReasonName(EvidenceReason value) {
  switch (value) {
    case EvidenceReason::kSourceBindingUnknown: return "SOURCE_BINDING_UNKNOWN";
    case EvidenceReason::kUnknown: return "UNKNOWN";
    case EvidenceReason::kViewportMismatch: return "VIEWPORT_MISMATCH";
    case EvidenceReason::kAwaitingStateBaseline: return "AWAITING_STATE_BASELINE";
    case EvidenceReason::kNoSourcePresents: return "NO_SOURCE_PRESENTS";
    case EvidenceReason::kCorrelated: return "CORRELATED";
    case EvidenceReason::kFixedPresentationHealthy: return "FIXED_PRESENTATION_HEALTHY";
    case EvidenceReason::kFixedPresentationStarved: return "FIXED_PRESENTATION_STARVED";
    case EvidenceReason::kVariableProviderMode: return "VARIABLE_PROVIDER_MODE";
  }
  return "UNKNOWN";
}

inline const char* ConfidenceName(Confidence value) {
  switch (value) {
    case Confidence::kUnknown: return "UNKNOWN";
    case Confidence::kObserved: return "OBSERVED";
    case Confidence::kReliable: return "RELIABLE";
  }
  return "UNKNOWN";
}

inline const char* HealthName(Health value) {
  switch (value) {
    case Health::kUnknown: return "UNKNOWN";
    case Health::kHealthy: return "HEALTHY";
    case Health::kStarved: return "STARVED";
  }
  return "UNKNOWN";
}

inline const char* SwapEffectName(SwapEffect value) {
  switch (value) {
    case SwapEffect::kUnknown: return "UNKNOWN";
    case SwapEffect::kDiscard: return "DISCARD";
    case SwapEffect::kSequential: return "SEQUENTIAL";
    case SwapEffect::kFlipSequential: return "FLIP_SEQUENTIAL";
    case SwapEffect::kFlipDiscard: return "FLIP_DISCARD";
  }
  return "UNKNOWN";
}

inline const char* WindowModeName(WindowMode value) {
  switch (value) {
    case WindowMode::kUnknown: return "UNKNOWN";
    case WindowMode::kWindowedOrBorderless: return "WINDOWED_OR_BORDERLESS";
    case WindowMode::kExclusiveFullscreen: return "EXCLUSIVE_FULLSCREEN";
  }
  return "UNKNOWN";
}

}  // namespace mfgunlock::presentation
