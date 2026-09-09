/*
 * Thread-safe Detours installation.
 * SPDX-License-Identifier: MIT
 *
 * Detours rewrites the first bytes of the target function. If another thread's
 * instruction pointer is inside that function at the moment of the rewrite, it
 * resumes mid-instruction -- a hang or a crash, not an error code.
 *
 * DetourUpdateThread is the documented answer: every thread that might be
 * executing the patched code must be registered with the transaction so Detours
 * can suspend it and fix up its instruction pointer. Registering only
 * GetCurrentThread() is correct only when the target is idle, and _nvngx.dll is
 * not idle -- we patch it from the ReShade present callback while the render
 * thread is calling DLSS through it.
 */

#pragma once

#include <windows.h>
#include <tlhelp32.h>

#include <detours.h>
#include <atomic>
#include <cstdint>
#include <cstring>

#include <sstream>
#include <tuple>
#include <vector>

#include <include/reshade.hpp>

namespace mfgunlock::hook {

// Function name, storage for the trampoline, replacement.
using HookItem = std::tuple<const char*, void**, void*>;

namespace internal {

// Every thread in this process except the caller.
inline std::vector<HANDLE> OpenOtherThreads() {
  std::vector<HANDLE> threads;
  const DWORD pid = GetCurrentProcessId();
  const DWORD self = GetCurrentThreadId();
  HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
  if (snap == INVALID_HANDLE_VALUE) return threads;

  THREADENTRY32 te = {};
  te.dwSize = sizeof(te);
  if (Thread32First(snap, &te)) {
    do {
      if (te.dwSize < FIELD_OFFSET(THREADENTRY32, th32OwnerProcessID)
                          + sizeof(te.th32OwnerProcessID)) {
        continue;
      }
      if (te.th32OwnerProcessID != pid) continue;
      if (te.th32ThreadID == self) continue;
      HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_SET_CONTEXT,
                            FALSE, te.th32ThreadID);
      if (h != nullptr) threads.push_back(h);
    } while (Thread32Next(snap, &te));
  }
  CloseHandle(snap);
  return threads;
}

}  // namespace internal

// Installs `hooks` into `module`. Returns true only if EVERY hook attached and
// the transaction committed -- a partial install leaves some entry points
// patched and others not, which is worse than none.
inline bool Install(HMODULE module, const std::vector<HookItem>& hooks,
                    const char* module_label) {
  if (module == nullptr) return false;

  std::vector<std::pair<void**, void*>> resolved;
  resolved.reserve(hooks.size());
  for (const auto& [name, real, replacement] : hooks) {
    FARPROC proc = GetProcAddress(module, name);
    if (proc == nullptr) {
      std::stringstream s;
      s << "mfgunlock::hook: " << module_label << " has no export " << name
        << " -- not hooking anything in this module.";
      reshade::log::message(reshade::log::level::error, s.str().c_str());
      return false;
    }
    if (*real != nullptr) return false;  // already installed
    *real = reinterpret_cast<void*>(proc);
    resolved.emplace_back(real, replacement);
  }

  if (DetourTransactionBegin() != NO_ERROR) {
    for (auto& [real, unused] : resolved) *real = nullptr;
    return false;
  }

  auto threads = internal::OpenOtherThreads();
  bool threads_ok = true;
  for (HANDLE h : threads) {
    if (DetourUpdateThread(h) != NO_ERROR) threads_ok = false;
  }
  if (DetourUpdateThread(GetCurrentThread()) != NO_ERROR) threads_ok = false;

  if (!threads_ok) {
    DetourTransactionAbort();
    for (HANDLE h : threads) CloseHandle(h);
    for (auto& [real, unused] : resolved) *real = nullptr;
    reshade::log::message(
        reshade::log::level::error,
        "mfgunlock::hook: could not register every thread with the Detours "
        "transaction -- refusing to patch, because patching code another thread "
        "is executing hangs the process.");
    return false;
  }

  bool ok = true;
  for (auto& [real, replacement] : resolved) {
    if (DetourAttach(real, replacement) != NO_ERROR) {
      ok = false;
      break;
    }
  }

  if (!ok || DetourTransactionCommit() != NO_ERROR) {
    if (!ok) DetourTransactionAbort();
    for (HANDLE h : threads) CloseHandle(h);
    for (auto& [real, unused] : resolved) *real = nullptr;
    std::stringstream s;
    s << "mfgunlock::hook: failed to install hooks in " << module_label << ".";
    reshade::log::message(reshade::log::level::error, s.str().c_str());
    return false;
  }

  for (HANDLE h : threads) CloseHandle(h);
  std::stringstream s;
  s << "mfgunlock::hook: installed " << resolved.size() << " hook(s) in "
    << module_label << ", with " << threads.size()
    << " other thread(s) registered with the transaction.";
  reshade::log::message(reshade::log::level::info, s.str().c_str());
  return true;
}

// Mirror of Install. Best-effort: used only on process detach.
inline void Uninstall(const std::vector<HookItem>& hooks) {
  if (DetourTransactionBegin() != NO_ERROR) return;
  auto threads = internal::OpenOtherThreads();
  for (HANDLE h : threads) DetourUpdateThread(h);
  DetourUpdateThread(GetCurrentThread());
  for (const auto& [name, real, replacement] : hooks) {
    if (*real != nullptr) DetourDetach(real, replacement);
  }
  if (DetourTransactionCommit() != NO_ERROR) DetourTransactionAbort();
  for (HANDLE h : threads) CloseHandle(h);
}

// Hook native entries so callers keep the original function address.
struct ModuleIdentity {
  HMODULE module = nullptr;
  DWORD timestamp = 0;
  DWORD image_bytes = 0;

  bool operator==(const ModuleIdentity& other) const {
    return module == other.module && timestamp == other.timestamp &&
           image_bytes == other.image_bytes;
  }
};

namespace internal {
inline bool Readable(const void* address, size_t bytes, HMODULE allocation) {
  if (address == nullptr || bytes == 0 || allocation == nullptr) return false;
  auto cursor = reinterpret_cast<uintptr_t>(address);
  if (cursor > UINTPTR_MAX - bytes) return false;
  const uintptr_t end = cursor + bytes;
  while (cursor < end) {
    MEMORY_BASIC_INFORMATION memory = {};
    if (VirtualQuery(reinterpret_cast<const void*>(cursor), &memory, sizeof(memory)) == 0) return false;
    if (memory.State != MEM_COMMIT || memory.AllocationBase != allocation ||
        (memory.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0) {
      return false;
    }
    const uintptr_t base = reinterpret_cast<uintptr_t>(memory.BaseAddress);
    if (base > UINTPTR_MAX - memory.RegionSize) return false;
    const uintptr_t next = base + memory.RegionSize;
    if (next <= cursor) return false;
    cursor = next < end ? next : end;
  }
  return true;
}
}  // namespace internal

inline bool CaptureModule(HMODULE module, ModuleIdentity& identity) {
  identity = {};
  if (!module || !internal::Readable(module, sizeof(IMAGE_DOS_HEADER), module)) return false;

  IMAGE_DOS_HEADER dos = {};
  std::memcpy(&dos, module, sizeof(dos));
  if (dos.e_magic != IMAGE_DOS_SIGNATURE || dos.e_lfanew <= 0 || dos.e_lfanew > 16 * 1024 * 1024) {
    return false;
  }

  const auto base = reinterpret_cast<uintptr_t>(module);
  const auto offset = static_cast<uintptr_t>(dos.e_lfanew);
  if (base > UINTPTR_MAX - offset) return false;
  const auto* nt_address = reinterpret_cast<const void*>(base + offset);
  if (!internal::Readable(nt_address, sizeof(IMAGE_NT_HEADERS64), module)) return false;

  IMAGE_NT_HEADERS64 nt = {};
  std::memcpy(&nt, nt_address, sizeof(nt));
  if (nt.Signature != IMAGE_NT_SIGNATURE ||
      nt.FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64 ||
      nt.OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC ||
      nt.OptionalHeader.SizeOfImage == 0) {
    return false;
  }

  identity = {module, nt.FileHeader.TimeDateStamp, nt.OptionalHeader.SizeOfImage};
  return true;
}

inline bool IsCurrent(const ModuleIdentity& identity) {
  if (!identity.module) return false;
  ModuleIdentity current;
  return CaptureModule(identity.module, current) && current == identity;
}

struct AddressHook {
  std::atomic<void*> target{nullptr};
  std::atomic<void*> replacement{nullptr};
  std::atomic<void*> trampoline{nullptr};
  ModuleIdentity identity{};
  std::atomic_bool installed{false};
  SRWLOCK lock = SRWLOCK_INIT;

  template <class Function>
  Function Original() const {
    return reinterpret_cast<Function>(trampoline.load(std::memory_order_acquire));
  }

  bool Targets(const void* address) const {
    return installed.load(std::memory_order_acquire) &&
           target.load(std::memory_order_acquire) == address;
  }
};

inline void ClearAddressState(AddressHook& state) {
  state.installed.store(false, std::memory_order_release);
  state.trampoline.store(nullptr, std::memory_order_release);
  state.target.store(nullptr, std::memory_order_release);
  state.replacement.store(nullptr, std::memory_order_release);
  state.identity = {};
}

inline bool InstallAddress(AddressHook& state, void* target, void* replacement,
                           const char* label) {
  if (!target || !replacement) return false;
  AcquireSRWLockExclusive(&state.lock);
  struct Unlock {
    SRWLOCK* lock;
    ~Unlock() { ReleaseSRWLockExclusive(lock); }
  } unlock{&state.lock};

  if (state.installed.load(std::memory_order_acquire)) {
    if (IsCurrent(state.identity)) {
      return state.target.load(std::memory_order_acquire) == target &&
             state.replacement.load(std::memory_order_acquire) == replacement;
    }
    // The old image disappeared. Its detour disappeared with the mapping too,
    // so this record can be reused without writing through stale pointers.
    ClearAddressState(state);
  }

  MEMORY_BASIC_INFORMATION memory = {};
  if (VirtualQuery(target, &memory, sizeof(memory)) == 0 ||
      memory.State != MEM_COMMIT || memory.Type != MEM_IMAGE || !memory.AllocationBase) {
    return false;
  }
  ModuleIdentity identity;
  if (!CaptureModule(reinterpret_cast<HMODULE>(memory.AllocationBase), identity)) return false;

  void* trampoline = target;
  if (DetourTransactionBegin() != NO_ERROR) return false;
  auto threads = internal::OpenOtherThreads();
  bool threads_ok = true;
  for (HANDLE thread : threads) {
    if (DetourUpdateThread(thread) != NO_ERROR) threads_ok = false;
  }
  if (DetourUpdateThread(GetCurrentThread()) != NO_ERROR) threads_ok = false;
  if (!threads_ok) {
    DetourTransactionAbort();
    for (HANDLE thread : threads) CloseHandle(thread);
    reshade::log::message(
        reshade::log::level::error,
        "mfgunlock::hook: address hook setup failed.");
    return false;
  }

  const bool attached = DetourAttach(&trampoline, replacement) == NO_ERROR;
  const LONG commit = attached ? DetourTransactionCommit() : ERROR_INVALID_OPERATION;
  if (!attached) DetourTransactionAbort();
  for (HANDLE thread : threads) CloseHandle(thread);
  if (!attached || commit != NO_ERROR) {
    std::stringstream message;
    message << "mfgunlock::hook: failed to hook " << label << ".";
    reshade::log::message(reshade::log::level::error, message.str().c_str());
    return false;
  }

  state.identity = identity;
  state.target.store(target, std::memory_order_release);
  state.replacement.store(replacement, std::memory_order_release);
  state.trampoline.store(trampoline, std::memory_order_release);
  state.installed.store(true, std::memory_order_release);
  return true;
}

inline void UninstallAddress(AddressHook& state) {
  AcquireSRWLockExclusive(&state.lock);
  struct Unlock {
    SRWLOCK* lock;
    ~Unlock() { ReleaseSRWLockExclusive(lock); }
  } unlock{&state.lock};

  if (!state.installed.load(std::memory_order_acquire)) {
    ClearAddressState(state);
    return;
  }
  if (!IsCurrent(state.identity)) {
    // The patched image has already been unmapped, so there is no mapped code
    // left to restore and no stale address should be touched.
    ClearAddressState(state);
    return;
  }

  void* trampoline = state.trampoline.load(std::memory_order_acquire);
  void* replacement = state.replacement.load(std::memory_order_acquire);
  if (!trampoline || !replacement || DetourTransactionBegin() != NO_ERROR) return;

  auto threads = internal::OpenOtherThreads();
  for (HANDLE thread : threads) DetourUpdateThread(thread);
  DetourUpdateThread(GetCurrentThread());
  const bool detached = DetourDetach(&trampoline, replacement) == NO_ERROR;
  const LONG commit = detached ? DetourTransactionCommit() : ERROR_INVALID_OPERATION;
  if (!detached) DetourTransactionAbort();
  for (HANDLE thread : threads) CloseHandle(thread);
  if (!detached || commit != NO_ERROR) {
    reshade::log::message(reshade::log::level::error,
                         "mfgunlock::hook: failed to remove address hook.");
    return;
  }
  ClearAddressState(state);
}

}  // namespace mfgunlock::hook
