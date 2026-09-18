// SPDX-License-Identifier: MIT
#pragma once
#include "../provider_mocks/windows.h"
#include <cstddef>
using LONG = int32_t;
constexpr LONG NO_ERROR = 0, ERROR_INVALID_OPERATION = 4317;
constexpr DWORD ERROR_NO_MORE_FILES = 18, ERROR_INVALID_PARAMETER = 87, ERROR_ACCESS_DENIED = 5;
constexpr DWORD THREAD_SUSPEND_RESUME = 2, THREAD_GET_CONTEXT = 8, THREAD_SET_CONTEXT = 16;
#define FIELD_OFFSET(type, field) offsetof(type, field)
#define INVALID_HANDLE_VALUE reinterpret_cast<HANDLE>(static_cast<intptr_t>(-1))
namespace hook_mock {
inline DWORD error = 0;
inline bool snapshot_ok = true, enumerate_ok = true, open_ok = true;
inline unsigned closed = 0;
}
inline DWORD GetCurrentProcessId() { return 100; }
inline DWORD GetCurrentThreadId() { return 1; }
inline HANDLE GetCurrentThread() { return reinterpret_cast<HANDLE>(1); }
inline DWORD GetLastError() { return hook_mock::error; }
inline HANDLE OpenThread(DWORD, BOOL, DWORD id) {
  if (!hook_mock::open_ok) { hook_mock::error = ERROR_ACCESS_DENIED; return nullptr; }
  return reinterpret_cast<HANDLE>(static_cast<uintptr_t>(id));
}
inline BOOL CloseHandle(HANDLE) { ++hook_mock::closed; return 1; }
inline BOOL FreeLibrary(HMODULE) { return 1; }
inline void SetLastError(DWORD value) { hook_mock::error = value; }
