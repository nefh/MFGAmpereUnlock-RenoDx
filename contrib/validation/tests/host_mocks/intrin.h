// SPDX-License-Identifier: MIT
// Deterministic caller identity for host-only loader-hook tests, not a CPU intrinsic.
#pragma once
namespace mock {
inline thread_local void* return_address = nullptr;
}
inline void* _ReturnAddress() { return mock::return_address; }
