// SPDX-License-Identifier: MIT
#pragma once
#include <windows.h>
#include <vector>
struct DETOUR_TRAMPOLINE {};
using PDETOUR_TRAMPOLINE = DETOUR_TRAMPOLINE*;
namespace detour_mock {
enum class Failure { kNone, kBegin, kUpdate, kAttach, kCommit, kDetach };
inline Failure failure = Failure::kNone;
inline bool transaction = false;
inline unsigned begins = 0, commits = 0, aborts = 0, attaches = 0, detaches = 0;
inline void (*during_commit)() = nullptr;
inline std::vector<void**> pending;
inline void Reset() {
  failure = Failure::kNone;
  transaction = false;
  begins = commits = aborts = attaches = detaches = 0;
  pending.clear();
  during_commit = nullptr;
}
}
inline LONG DetourTransactionBegin() {
  ++detour_mock::begins;
  if (detour_mock::failure == detour_mock::Failure::kBegin) return 1;
  detour_mock::transaction = true;
  return NO_ERROR;
}
inline LONG DetourUpdateThread(HANDLE) {
  return detour_mock::failure == detour_mock::Failure::kUpdate ? 1 : NO_ERROR;
}
inline LONG DetourAttach(void** pointer, void*) {
  ++detour_mock::attaches;
  if (detour_mock::failure == detour_mock::Failure::kAttach) return 1;
  detour_mock::pending.push_back(pointer);
  return NO_ERROR;
}
inline LONG DetourAttachEx(void** pointer, void* replacement, PDETOUR_TRAMPOLINE* trampoline,
                           void**, void**) {
  const auto result = DetourAttach(pointer, replacement);
  if (result == NO_ERROR && trampoline) *trampoline = reinterpret_cast<PDETOUR_TRAMPOLINE>(*pointer);
  return result;
}
inline LONG DetourDetach(void**, void*) {
  ++detour_mock::detaches;
  return detour_mock::failure == detour_mock::Failure::kDetach ? 1 : NO_ERROR;
}
inline LONG DetourTransactionAbort() {
  ++detour_mock::aborts;
  detour_mock::transaction = false;
  detour_mock::pending.clear();
  return NO_ERROR;
}
inline LONG DetourTransactionCommit() {
  ++detour_mock::commits;
  detour_mock::transaction = false;
  detour_mock::pending.clear();
  if (detour_mock::failure == detour_mock::Failure::kCommit) return 1;
  if (detour_mock::during_commit) detour_mock::during_commit();
  return NO_ERROR;
}
