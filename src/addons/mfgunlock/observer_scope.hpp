// SPDX-License-Identifier: MIT
#pragma once
#include <cstdint>

namespace mfgunlock::observers {
// Shared by observer kinds: cross-kind recursion can otherwise acquire the
// callback mutexes in opposite orders. Nested diagnostic events are dropped;
// the observed game/provider calls themselves are never suppressed.
inline thread_local uint32_t g_callback_depth = 0;
}  // namespace mfgunlock::observers
