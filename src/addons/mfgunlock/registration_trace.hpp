// SPDX-License-Identifier: MIT
#pragma once
#include <atomic>
#include <cstddef>
#include <cstdio>

namespace mfgunlock::registrationtrace {
// Diagnostic-only append journal. No heap, locks, waits, I/O or SDK access.
// Publish each immutable record only after it is fully formatted. Never wrap
// over a record while the loader/worker may still be writing or reading it.
template <std::size_t Capacity = 96, std::size_t Width = 1536>
class Journal {
  static_assert(Capacity > 0 && Width > 1);
 public:
  template <class... Args>
  void Append(const char* format, Args... args) noexcept {
    const auto slot = next_.fetch_add(1, std::memory_order_relaxed);
    if (slot >= Capacity) return;
    if constexpr (sizeof...(Args) == 0)
      std::snprintf(entries_[slot].text, Width, "%s", format);
    else
      std::snprintf(entries_[slot].text, Width, format, args...);
    entries_[slot].text[Width - 1] = '\0';
    entries_[slot].published.store(true, std::memory_order_release);
  }
  template <class Visitor>
  void Visit(Visitor&& visitor) const {
    const auto count = next_.load(std::memory_order_relaxed);
    for (std::size_t i = 0; i < count && i < Capacity; ++i)
      if (entries_[i].published.load(std::memory_order_acquire))
        visitor(i, entries_[i].text);
  }
  std::size_t Dropped() const noexcept {
    const auto n = next_.load(std::memory_order_relaxed);
    return n > Capacity ? n - Capacity : 0;
  }
 private:
  struct Entry { char text[Width]{}; std::atomic_bool published{false}; };
  std::atomic<std::size_t> next_{0};
  Entry entries_[Capacity]{};
};
} // namespace mfgunlock::registrationtrace
