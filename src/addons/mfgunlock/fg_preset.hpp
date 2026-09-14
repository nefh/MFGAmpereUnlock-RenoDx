/*
 * Process-local DLSS-G model selection for an exact provider build.
 * SPDX-License-Identifier: MIT
 *
 * The provider caches driver settings internally. Intercept only its verified
 * model-setup read, not NVAPI/DRS writes or override-state reporting. A later
 * feature creation may reuse that cache, so a game restart can be necessary.
 */
#pragma once

#include <windows.h>
#ifdef _MSC_VER
#include <intrin.h>
#endif

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include "./provider.hpp"
#include "./ngx_hook.hpp"

namespace mfgunlock::fgpreset {

enum class Preset : int { kDefault = 0, kA = 1, kB = 2 };

constexpr Preset Parse(int value) {
  return value == 1 ? Preset::kA : value == 2 ? Preset::kB : Preset::kDefault;
}

inline const char* Name(Preset preset) {
  switch (preset) {
    case Preset::kA: return "A";
    case Preset::kB: return "B";
    default: return "Application / driver default";
  }
}

inline std::atomic<Preset> g_requested{Preset::kDefault};
// Evidence of a supplied setup value, NOT proof of the model running on the GPU.
inline std::atomic_int g_last_supplied{-1};
// Runtime observation of the provider's own applied render-preset field.
inline std::atomic_int g_last_applied{-1};

namespace internal {

// NVIDIA NvApiDriverSettings.h: NGX_DLSS_FG_OVERRIDE_RENDER_PRESET_SELECTION_ID.
// A=1 and B=2. The call below is an internal provider reader, NOT an NVAPI ABI.
constexpr uint32_t kPresetSetting = 0x10E41DF1u;
constexpr uint32_t kReaderRva = 0x13EF0u;
constexpr uint32_t kSelectionRva = 0x3424Du;
constexpr uint32_t kSelectionReturnRva = 0x34264u;
constexpr uint32_t kReportOverridesRva = 0x37B00u;
constexpr size_t kAppliedPresetOffset = 0x64u;
constexpr size_t kReaderBytes = 0xCDu;
constexpr size_t kSelectionBytes = 0xEDu;
constexpr size_t kReportOverridesBytes = 0x3F7u;
constexpr uint64_t kReaderHash = 0xAAC59E49B79F45FDull;
constexpr uint64_t kSelectionHash = 0x6BB5E4B6D01B4A2Bull;
constexpr uint64_t kReportOverridesHash = 0x45161D995EC231A3ull;

// Verified from pristine nvngx_dlssg.dll 310.9.1.0, SHA256:
// ff6e90eb78b827927dff5b4ecc6b1c870c2e9bca29ed9f48c7d348cc9e170b82.
// Fingerprints cover the entire reader and the A/B consumer, with no PE base
// relocations in either range. Matching a version string alone is insufficient.
inline uint64_t Fingerprint(const unsigned char* bytes, size_t count) {
  uint64_t hash = 0xCBF29CE484222325ull;
  for (size_t i = 0; i < count; ++i) hash = (hash ^ bytes[i]) * 0x100000001B3ull;
  return hash;
}

inline bool MatchesProvider(HMODULE module) {
  provider::internal::ImageIdentity identity;
  if (!provider::internal::ReadIdentity(module, identity) ||
      identity.timestamp != 0x6A986031u || identity.image_bytes != 0x737000u) return false;
  const auto* base = reinterpret_cast<const unsigned char*>(module);
  if (!provider::internal::IsReadable(base + kReaderRva, kReaderBytes, module) ||
      !provider::internal::IsReadable(base + kSelectionRva, kSelectionBytes, module) ||
      !provider::internal::IsReadable(base + kReportOverridesRva, kReportOverridesBytes, module)) return false;
  MEMORY_BASIC_INFORMATION memory{};
  if (!VirtualQuery(base + kReaderRva, &memory, sizeof(memory)) ||
      (memory.Protect & (PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) == 0)
    return false;
  return Fingerprint(base + kReaderRva, kReaderBytes) == kReaderHash &&
         Fingerprint(base + kSelectionRva, kSelectionBytes) == kSelectionHash &&
         Fingerprint(base + kReportOverridesRva, kReportOverridesBytes) == kReportOverridesHash;
}

using ReadSettingFn = bool (__cdecl*)(uint32_t setting, uint32_t* value);
using ReportOverridesFn = uint32_t (__cdecl*)(void* core, void* states, const void* configuration);
struct ReaderHook {
  hook::AddressHook entry;
  hook::AddressHook observer;
  HMODULE retained = nullptr;
};
// Like the NGX bridge, keep a separate trampoline for each loaded provider.
inline std::array<ReaderHook, 4> g_readers;
inline std::atomic_bool g_shutting_down{false};

inline bool ReadSetting(size_t index, uint32_t setting, uint32_t* value, const void* caller) {
  auto& entry = g_readers[index].entry;
  // InstallAddress publishes its trampoline after committing the detour. This
  // lock also prevents detach from freeing a trampoline during a forwarded call.
  AcquireSRWLockShared(&entry.lock);
  struct Unlock {
    SRWLOCK* lock;
    ~Unlock() { ReleaseSRWLockShared(lock); }
  } unlock{&entry.lock};
  const auto real = entry.Original<ReadSettingFn>();
  if (!real) return false;

  const auto preset = g_requested.load(std::memory_order_relaxed);
  const auto selection_return = reinterpret_cast<uintptr_t>(entry.identity.module) + kSelectionReturnRva;
  if (g_enabled.load(std::memory_order_relaxed) && !g_shutting_down.load(std::memory_order_relaxed) &&
      value && setting == kPresetSetting && preset != Preset::kDefault &&
      reinterpret_cast<uintptr_t>(caller) == selection_return) {
    *value = static_cast<uint32_t>(preset);
    if (g_last_supplied.exchange(static_cast<int>(preset), std::memory_order_relaxed) != static_cast<int>(preset))
      provider::Log(std::string("FG model-setup query supplied Preset ") + Name(preset) +
                    "; active GPU model is not independently observed");
    return true;
  }
  // Includes Default, all other settings, and the provider's reporting reads.
  return real(setting, value);
}

template <size_t Index>
inline bool __cdecl HookedReadSetting(uint32_t setting, uint32_t* value) {
#ifdef _MSC_VER
  const void* caller = _ReturnAddress();
#else
  const void* caller = __builtin_return_address(0);
#endif
  return ReadSetting(Index, setting, value, caller);
}

inline constexpr std::array<ReadSettingFn, 4> kReaders = {
    HookedReadSetting<0>, HookedReadSetting<1>, HookedReadSetting<2>, HookedReadSetting<3>};

inline uint32_t ReportOverrides(size_t index, void* core, void* states,
                                const void* configuration) {
  auto& observer = g_readers[index].observer;
  AcquireSRWLockShared(&observer.lock);
  struct Unlock {
    SRWLOCK* lock;
    ~Unlock() { ReleaseSRWLockShared(lock); }
  } unlock{&observer.lock};
  const auto real = observer.Original<ReportOverridesFn>();
  if (!real) return 0xBAD00000u;

  uint32_t applied = 0;
  if (configuration) {
    const auto* bytes = static_cast<const unsigned char*>(configuration);
    std::memcpy(&applied, bytes + kAppliedPresetOffset, sizeof(applied));
  }
  const uint32_t result = real(core, states, configuration);
  if (g_last_supplied.load(std::memory_order_relaxed) >= 0 &&
      (applied == static_cast<uint32_t>(Preset::kA) ||
       applied == static_cast<uint32_t>(Preset::kB))) {
    const int value = static_cast<int>(applied);
    if (g_last_applied.exchange(value, std::memory_order_relaxed) != value)
      provider::Log(std::string("FG provider reports applied render preset ") +
                    Name(Parse(value)) + " (runtime-observed)");
  }
  return result;
}

template <size_t Index>
inline uint32_t __cdecl HookedReportOverrides(void* core, void* states,
                                              const void* configuration) {
  return ReportOverrides(Index, core, states, configuration);
}

inline constexpr std::array<ReportOverridesFn, 4> kObservers = {
    HookedReportOverrides<0>, HookedReportOverrides<1>,
    HookedReportOverrides<2>, HookedReportOverrides<3>};

}  // namespace internal

// Called only by the existing serialized provider-maintenance path. Do not
// install from Present or from inside the setting-reader callback.
inline bool TryInstall(HMODULE module) {
  if (!g_enabled.load() || internal::g_shutting_down.load() ||
      g_requested.load() == Preset::kDefault) return false;
  for (auto& reader : internal::g_readers) {
    if (reader.entry.installed.load(std::memory_order_acquire) &&
        reader.observer.installed.load(std::memory_order_acquire) &&
        reader.retained == module) return true;
  }
  if (!internal::MatchesProvider(module)) return false;
  for (size_t i = 0; i < internal::g_readers.size(); ++i) {
    auto& reader = internal::g_readers[i];
    if (reader.retained && reader.retained != module) continue;
    if (!reader.retained) {
      HMODULE held = nullptr;
      if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
            reinterpret_cast<LPCWSTR>(module), &held) || held != module) return false;
      // Same process-lifetime retention policy as provider.hpp: a failed
      // installation may be retried without acquiring another DLL reference.
      reader.retained = held;
    }
    if (!hook::InstallAddress(reader.entry,
          reinterpret_cast<unsigned char*>(module) + internal::kReaderRva,
          reinterpret_cast<void*>(internal::kReaders[i]), "DLSS-G model preset reader")) return false;
    const bool observed = hook::InstallAddress(reader.observer,
        reinterpret_cast<unsigned char*>(module) + internal::kReportOverridesRva,
        reinterpret_cast<void*>(internal::kObservers[i]), "DLSS-G render preset observer");
    provider::Log(observed
        ? "FG preset reader and applied-preset observer installed for DLSS-G 310.9.1"
        : "FG preset reader installed; applied-preset observer unavailable");
    return true;
  }
  return false;
}

inline bool HasHook() {
  for (auto& reader : internal::g_readers) {
    if (reader.entry.installed.load(std::memory_order_acquire)) return true;
  }
  return false;
}

inline bool HasObserver() {
  for (auto& reader : internal::g_readers) {
    if (reader.observer.installed.load(std::memory_order_acquire)) return true;
  }
  return false;
}

inline void Shutdown() {
  internal::g_shutting_down.store(true, std::memory_order_release);
  for (auto& reader : internal::g_readers) {
    hook::UninstallAddress(reader.observer);
    hook::UninstallAddress(reader.entry);
  }
}

}  // namespace mfgunlock::fgpreset
