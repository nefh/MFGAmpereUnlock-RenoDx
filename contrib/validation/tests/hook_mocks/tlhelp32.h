// SPDX-License-Identifier: MIT
#pragma once
#include "windows.h"
constexpr DWORD TH32CS_SNAPTHREAD = 4;
struct THREADENTRY32 { DWORD dwSize, cntUsage, th32ThreadID, th32OwnerProcessID; };
inline HANDLE CreateToolhelp32Snapshot(DWORD, DWORD) {
  return hook_mock::snapshot_ok ? reinterpret_cast<HANDLE>(10) : INVALID_HANDLE_VALUE;
}
inline BOOL Thread32First(HANDLE, THREADENTRY32* entry) {
  if (!hook_mock::enumerate_ok) { hook_mock::error = ERROR_ACCESS_DENIED; return 0; }
  *entry = {sizeof(*entry), 0, 2, 100};
  return 1;
}
inline BOOL Thread32Next(HANDLE, THREADENTRY32*) {
  hook_mock::error = ERROR_NO_MORE_FILES;
  return 0;
}
