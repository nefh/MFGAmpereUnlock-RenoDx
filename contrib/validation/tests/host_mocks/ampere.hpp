// SPDX-License-Identifier: MIT
// Provider test double. Production provider behavior is covered separately by
// provider_state_test and ampere_ptx_test.
#pragma once

#include "mock_api.hpp"

namespace mfgunlock::ampere {

inline std::atomic_bool g_enabled{false};
inline std::atomic_bool g_device_confirmed{false};
inline std::atomic_bool g_other_gpu{false};
inline std::atomic_bool g_create_seen{false};
inline unsigned int g_test_prepared_count = 0;
inline unsigned int g_test_blocking_count = 0;
inline unsigned int g_test_expired_count = 0;
inline std::vector<std::string> g_test_log;

struct ProviderStatus {
  unsigned int ready = 0;
  unsigned int blocked = 0;
  unsigned int expired_rejections = 0;
  unsigned int ignored_mappings = 0;
  bool busy = false;
  bool failed = false;

  unsigned int QualifiedCount() const {
    return busy || failed || blocked ? 0 : ready;
  }

  const char* Reason() const {
    if (blocked) return "active-provider-rejected-or-invalidated";
    return ready == 1 ? "one-prepared-provider" : "no-prepared-provider";
  }
};

inline ProviderStatus GetProviderStatus() {
  return {g_test_prepared_count, g_test_blocking_count, g_test_expired_count, 0, false, false};
}

inline unsigned int PreparedProviderCount() {
  return GetProviderStatus().QualifiedCount();
}

inline void Log(const std::string& message, bool = false) {
  g_test_log.push_back(message);
}

namespace internal {

struct Provider {
  HMODULE module = nullptr;
  bool ready = false;
  unsigned int fatbins = 0;
  unsigned int hidden_cubins = 0;
  std::string path;
  std::string failure;
};

struct Rejection {
  HMODULE module = nullptr;
  unsigned int identity = 0;
  std::string path;
  std::string reason;
};

inline bool IsImageMapping(HMODULE) {
  return true;
}

inline bool IsCurrent(HMODULE, unsigned int) {
  return true;
}

inline SRWLOCK g_lock = SRWLOCK_INIT;
inline std::vector<Provider> g_providers;
inline std::vector<Rejection> g_rejected;

}  // namespace internal
}  // namespace mfgunlock::ampere
