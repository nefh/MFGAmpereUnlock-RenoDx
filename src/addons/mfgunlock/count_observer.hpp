// SPDX-License-Identifier: MIT
#pragma once
#include "./observer_scope.hpp"
#include <atomic>
#include <cstdint>
#include <mutex>
#include <shared_mutex>
#if defined(_MSC_VER)
#include <intrin.h>
#endif

namespace mfgunlock::countobserver {
inline constexpr uint32_t kVersion = 1;
inline constexpr uint32_t kUnknown = UINT32_MAX;
enum class Backend : uint32_t { kStreamline, kNgxD3D12, kNgxVulkan, kNgxC };
enum class Operation : uint32_t { kRequest, kForward, kRead, kAdvertise, kWrite };
enum class Origin : uint32_t {
  kUnknown, kNative, kFixed, kRetry, kNativeFallback, kDynamic,
  kBackendFallback, kCapabilityPolicy,
};
enum class Key : uint32_t { kGenerated, kMaximum, kIndex };

// POD boundary shared only with the optional diagnostic addon. Counts describe
// API traffic, not display cadence. A void NGX Set has no success result.
struct Event {
  uint32_t bytes = sizeof(Event);
  uint32_t version = kVersion;
  Backend backend = Backend::kStreamline;
  Operation operation = Operation::kRequest;
  Origin origin = Origin::kUnknown;
  Key key = Key::kGenerated;
  uint32_t result = kUnknown;
  uint32_t value_known = 0;
  int64_t value = 0;
  uint32_t viewport = kUnknown;
  uint32_t requested = kUnknown;
  uint32_t mode = kUnknown;
  uint32_t ui_fallback = 0;
  uint64_t caller = 0;
  uint64_t callee = 0;
  uint64_t object = 0;
};
using Callback = void (*)(const Event*) noexcept;
using Register = bool (*)(uint32_t, Callback);
inline std::atomic<Callback> g_callback{nullptr};
inline std::shared_mutex g_callback_mutex;

inline bool SetCallback(Callback callback) noexcept {
  // A callback may disconnect itself after a bounded capture ends. It already
  // executes under the shared guard, so clearing the atomic slot is sufficient;
  // a later unload-side unregister takes the exclusive guard and drains it.
  if (observers::g_callback_depth != 0) {
    if (callback != nullptr) return false;
    g_callback.store(nullptr, std::memory_order_release);
    return true;
  }
  std::unique_lock lock(g_callback_mutex);
  g_callback.store(callback, std::memory_order_release);
  return true;
}

inline void Emit(const Event& event) noexcept {
  if (observers::g_callback_depth != 0) return;  // Diagnostic re-entry must not recurse into the shared mutex.
  std::shared_lock lock(g_callback_mutex);
  const auto callback = g_callback.load(std::memory_order_acquire);
  if (!callback) return;
  ++observers::g_callback_depth;
  callback(&event);
  --observers::g_callback_depth;
}

// Must inline so the return address belongs to the observed API entry.
#if defined(_MSC_VER)
__forceinline uint64_t CallerAddress() { return reinterpret_cast<uint64_t>(_ReturnAddress()); }
#else
__attribute__((always_inline)) inline uint64_t CallerAddress() {
  return reinterpret_cast<uint64_t>(__builtin_return_address(0));
}
#endif

struct Context { uint64_t caller = 0; uint32_t requested = kUnknown; };
inline thread_local Context g_context;
struct Scope {
  bool active = g_callback.load(std::memory_order_relaxed) != nullptr;
  Context previous{};
  Scope(uint64_t caller, uint32_t requested) {
    if (!active) return;
    previous = g_context;
    g_context = {caller, requested};
  }
  ~Scope() { if (active) g_context = previous; }
};
}  // namespace mfgunlock::countobserver
