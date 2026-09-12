/*
 * RenoDX MFG Unlock
 * SPDX-License-Identifier: MIT
 *
 * Enables native NVIDIA DLSS-G/MFG compatibility on supported architecture
 * profiles by preparing the mapped provider, relaxing its architecture gates,
 * correcting generated-frame timing, and exposing the verified frame count.
 * All binary changes are in-memory and restored on unload.
 */

#define ImTextureID ImU64

#include <windows.h>
#include <tlhelp32.h>

#include <algorithm>
#include <atomic>
#include <cwchar>
#include <cstring>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include <deps/imgui/imgui.h>
#include <include/reshade.hpp>

#include "./early_load.hpp"
#include "./framecount.hpp"
#include "./loadhook.hpp"
#include "./midpoint.hpp"
#include "./streamline_bridge.hpp"
#include "./validated_warp.hpp"
namespace {

constexpr const char* kConfigSection = "RenoDX.MFGUnlock";
constexpr const char* kAddonSection = "ADDON";
constexpr const char* kEarlyLoadKey = "LoadFromDllMain";
constexpr const char* kAddonFileName = "renodx-mfgunlock.addon64";
constexpr unsigned int kMinCount = 2;
constexpr unsigned int kMaxCount = 5;

using mfgunlock::g_enabled;
std::atomic<unsigned int> g_max_count{4};
std::atomic_bool g_force_flip_meter_off{false};
std::atomic_bool g_temporal_fix{true};
std::atomic_bool g_validated_warp_blend{false};
std::atomic_bool g_raise_ceiling{false};
std::atomic_bool g_dynamic_stack_validated{false};

// Marker scans can match literals embedded in this addon, so never inspect the
// addon's own image as a candidate provider.
HMODULE g_self_module = nullptr;

// ------------------------------------------------------- in-memory arch gate
// The provider uses comparisons against the Blackwell NVAPI architecture ID for
// both capability advertising and runtime generation. Rewrite only the compare
// immediates; the architecture lookup table must remain untouched.

// ------------------------------------------------ locating the DLSS-G provider
// The provider may come from the game directory, the NVIDIA OTA store, or a
// renamed cache entry. Paths are a fast path; NGX provider exports plus a known
// marker cover relocated providers.

constexpr char kDlssgMarker[] = "dlfg_kernel";

std::vector<HMODULE> g_inspected_modules;
std::vector<HMODULE> g_dlssg_modules;
SRWLOCK g_provider_maintenance_lock = SRWLOCK_INIT;

bool ModuleContains(HMODULE mod, const char* needle, size_t needle_len) {
  auto* base = reinterpret_cast<unsigned char*>(mod);
  const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
  if (IsBadReadPtr(base, sizeof(IMAGE_DOS_HEADER)) != 0) return false;
  if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
  const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
  if (nt->Signature != IMAGE_NT_SIGNATURE) return false;
  if (nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) return false;

  const auto* section = IMAGE_FIRST_SECTION(nt);
  for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++section) {
    if ((section->Characteristics & IMAGE_SCN_MEM_READ) == 0) continue;
    unsigned char* start = base + section->VirtualAddress;
    const size_t size = section->Misc.VirtualSize;
    if (size < needle_len) continue;
    for (size_t off = 0; off + needle_len <= size; ++off) {
      if (std::memcmp(start + off, needle, needle_len) == 0) return true;
    }
  }
  return false;
}

void RememberDlssgModule(HMODULE mod) {
  if (mod == nullptr || mod == g_self_module) return;
  if (std::find(g_dlssg_modules.begin(), g_dlssg_modules.end(), mod) == g_dlssg_modules.end()) {
    g_dlssg_modules.push_back(mod);
  }
}

bool HasKnownDlssgPath(HMODULE mod) {
  wchar_t module_path[32768] = {};
  const DWORD length = GetModuleFileNameW(mod, module_path, ARRAYSIZE(module_path));
  if (length == 0 || length >= ARRAYSIZE(module_path)) return false;
  for (DWORD i = 0; i < length; ++i) {
    if (module_path[i] >= L'A' && module_path[i] <= L'Z') {
      module_path[i] = static_cast<wchar_t>(module_path[i] - L'A' + L'a');
    }
  }
  return std::wcsstr(module_path, L"nvngx_dlssg") != nullptr ||
         std::wcsstr(module_path, L"\\models\\dlssg\\") != nullptr;
}

bool IsDlssgProvider(HMODULE mod) {
  // Keep the established D3D/OTA path as the fast path. Vulkan-specific export
  // checks are only needed when a game has renamed or relocated the provider.
  if (HasKnownDlssgPath(mod)) return true;

  // A renamed provider may expose either graphics backend. Streamline's
  // slDLSSGSetOptions/slDLSSGGetState interface is renderer-independent, so
  // discovery must not discard Vulkan snippets before the shared patch path
  // gets a chance to inspect them.
  const bool has_d3d12_entry =
      GetProcAddress(mod, "NVSDK_NGX_D3D12_PopulateDeviceParameters_Impl") != nullptr;
  const bool has_vulkan_entry =
      GetProcAddress(mod, "NVSDK_NGX_VULKAN_PopulateDeviceParameters_Impl") != nullptr;

  // Retain content-based discovery for games that rename or relocate the
  // snippet, but only scan modules exposing an NGX provider entry point.
  if (!has_d3d12_entry && !has_vulkan_entry) return false;
  return ModuleContains(mod, kDlssgMarker, sizeof(kDlssgMarker) - 1);
}

// Perform one shared bootstrap pass for providers that were mapped before this
// addon. Providers mapped later are handled directly by the loader hook, so
// module enumeration never has to run from the presentation thread.
const std::vector<HMODULE>& DiscoverDlssgModules() {
  if (HMODULE fast = GetModuleHandleW(L"nvngx_dlssg.dll")) RememberDlssgModule(fast);

  HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, GetCurrentProcessId());
  if (snap == INVALID_HANDLE_VALUE) return g_dlssg_modules;
  MODULEENTRY32W me = {};
  me.dwSize = sizeof(me);
  if (Module32FirstW(snap, &me)) {
    do {
      if (me.hModule == g_self_module) continue;
      if (std::find(g_inspected_modules.begin(), g_inspected_modules.end(), me.hModule) !=
          g_inspected_modules.end()) {
        continue;
      }
      g_inspected_modules.push_back(me.hModule);
      if (IsDlssgProvider(me.hModule)) RememberDlssgModule(me.hModule);
    } while (Module32NextW(snap, &me));
  }
  CloseHandle(snap);
  return g_dlssg_modules;
}

constexpr unsigned char kArchOld = 0xB0;  // 0x1b0 GB20x
constexpr unsigned char kArchNew = 0x90;  // 0x190 AD10x

struct GateSite {
  unsigned char* address;  // the byte holding the arch id's low octet
  unsigned char original;
};

std::atomic_bool g_gate_patched{false};
std::vector<GateSite> g_gate_sites;
std::vector<HMODULE> g_gate_modules;
std::vector<HMODULE> g_gate_rejected_modules;

// Split out so the load-time trigger can patch a module it already holds a
// handle to. That path runs under the loader lock, where CreateToolhelp32Snapshot
// (which FindDlssgModule uses) would deadlock -- so it must never scan.
bool HasTemporalPatch(HMODULE mod);
void PatchArchGatesInModule(HMODULE mod) {
  if (mod == nullptr) return;
  const auto* profile = mfgunlock::architecture::ActiveProfile();
  if (!g_enabled.load() || !profile) return;
  const bool retarget = profile->NeedsRetarget();
  if (retarget && (!mfgunlock::provider::PrepareProvider(mod) || !HasTemporalPatch(mod))) return;
  if (std::find(g_gate_modules.begin(), g_gate_modules.end(), mod) != g_gate_modules.end()) return;
  if (std::find(g_gate_rejected_modules.begin(), g_gate_rejected_modules.end(), mod) !=
      g_gate_rejected_modules.end()) {
    return;
  }

  auto* base = reinterpret_cast<unsigned char*>(mod);
  const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
  if (dos->e_magic != IMAGE_DOS_SIGNATURE) return;
  const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
  if (nt->Signature != IMAGE_NT_SIGNATURE) return;

  std::vector<unsigned char*> found;
  const auto* section = IMAGE_FIRST_SECTION(nt);
  for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++section) {
    if ((section->Characteristics & IMAGE_SCN_MEM_EXECUTE) == 0) continue;
    unsigned char* start = base + section->VirtualAddress;
    const size_t size = section->Misc.VirtualSize;
    if (size < 6) continue;
    for (size_t off = 0; off + 6 <= size; ++off) {
      // 3D id32            cmp eax, imm32
      if (start[off] == 0x3D && start[off + 1] == kArchOld && start[off + 2] == 0x01 &&
          start[off + 3] == 0x00 && start[off + 4] == 0x00) {
        found.push_back(start + off + 1);
        continue;
      }
      // 81 /7 id32         cmp r32, imm32
      if (start[off] == 0x81 && start[off + 1] >= 0xF8 && start[off + 1] <= 0xFF &&
          start[off + 2] == kArchOld && start[off + 3] == 0x01 && start[off + 4] == 0x00 &&
          start[off + 5] == 0x00) {
        found.push_back(start + off + 2);
      }
    }
  }

  if (found.empty() || found.size() > 4) {
    char module_path[MAX_PATH] = {};
    GetModuleFileNameA(mod, module_path, MAX_PATH);
    std::stringstream s;
    s << "mfgunlock: found " << found.size() << " arch-gate comparisons in " << module_path
      << " (expected 1-4); leaving this provider alone.";
    reshade::log::message(reshade::log::level::warning, s.str().c_str());
    g_gate_rejected_modules.push_back(mod);
    return;
  }

  if (retarget) {
    g_gate_sites.reserve(g_gate_sites.size() + found.size());
    g_gate_modules.reserve(g_gate_modules.size() + 1);
    if (!mfgunlock::provider::ApplyMfgComparisons(found)) return;
    for (unsigned char* site : found) g_gate_sites.push_back({site, kArchOld});
    g_gate_modules.push_back(mod);
    g_gate_patched.store(true, std::memory_order_release);
    mfgunlock::provider::Log("MFG comparison gates opened after PTX and temporal preparation");
    return;
  }
  const size_t sites_before = g_gate_sites.size();
  for (unsigned char* site : found) {
    DWORD old_protect = 0;
    if (VirtualProtect(site, 1, PAGE_EXECUTE_READWRITE, &old_protect) == 0) continue;
    g_gate_sites.push_back({site, *site});
    *site = kArchNew;
    DWORD ignored = 0;
    VirtualProtect(site, 1, old_protect, &ignored);
    FlushInstructionCache(GetCurrentProcess(), site, 1);
  }

  const size_t sites_written = g_gate_sites.size() - sites_before;
  if (sites_written == 0) {
    reshade::log::message(reshade::log::level::error,
                          "mfgunlock: could not make the nvngx_dlssg.dll arch gates writable.");
    g_gate_rejected_modules.push_back(mod);
    return;
  }
  g_gate_modules.push_back(mod);
  g_gate_patched.store(true, std::memory_order_release);

  char module_path[MAX_PATH] = {};
  GetModuleFileNameA(mod, module_path, MAX_PATH);
  std::stringstream s;
  s << "mfgunlock: rewrote " << sites_written << " arch gate(s) (0x1b0 -> 0x190) in "
    << module_path << "; multi-frame should report as supported AND generate.";
  reshade::log::message(reshade::log::level::info, s.str().c_str());
}

void TryPatchDlssgArchGate() {
  for (HMODULE mod : g_dlssg_modules) PatchArchGatesInModule(mod);
}

void RestoreDlssgArchGate() {
  if (!g_gate_patched.load(std::memory_order_acquire)) return;
  for (const auto& site : g_gate_sites) {
    DWORD old_protect = 0;
    if (VirtualProtect(site.address, 1, PAGE_EXECUTE_READWRITE, &old_protect) != 0) {
      *site.address = site.original;
      DWORD ignored = 0;
      VirtualProtect(site.address, 1, old_protect, &ignored);
      FlushInstructionCache(GetCurrentProcess(), site.address, 1);
    }
  }
  g_gate_sites.clear();
  g_gate_modules.clear();
  g_gate_rejected_modules.clear();
  g_gate_patched.store(false, std::memory_order_release);
}

// --------------------------------------------------- validated warp blend
// Optional Ampere-only quality patch for the exact 310.9.1 provider. The
// replacement PTX is redirected through provider-owned descriptors and is
// restored before the provider's in-place architecture patches on unload.

struct ValidatedWarpModulePatch {
  HMODULE module = nullptr;
  std::vector<mfgunlock::validatedwarp::Redirect> redirects;
  std::string provider_version;
  std::string detail;
};
std::vector<ValidatedWarpModulePatch> g_validated_warp_modules;
std::vector<HMODULE> g_validated_warp_rejected_modules;
std::string g_validated_warp_detail;

void PatchValidatedWarpInModule(HMODULE mod) {
  if (mod == nullptr || !g_validated_warp_blend.load(std::memory_order_relaxed)) return;
  const auto* profile = mfgunlock::architecture::ActiveProfile();
  if (!g_enabled.load() || !profile || profile->architecture != mfgunlock::Architecture::kAmpere)
    return;
  if (!mfgunlock::provider::PrepareProvider(mod)) return;
  if (std::any_of(g_validated_warp_modules.begin(), g_validated_warp_modules.end(),
                  [mod](const auto& patch) { return patch.module == mod; })) return;
  if (std::find(g_validated_warp_rejected_modules.begin(),
                g_validated_warp_rejected_modules.end(), mod) !=
      g_validated_warp_rejected_modules.end()) return;

  std::vector<mfgunlock::validatedwarp::Redirect> redirects;
  mfgunlock::validatedwarp::Result result{};
  std::string provider_version;
  const bool applied = mfgunlock::validatedwarp::Apply(
      mod, redirects, result, provider_version);
  const std::string detail = result.detail;
  if (!applied || !result.applied) {
    g_validated_warp_rejected_modules.push_back(mod);
    g_validated_warp_detail = detail;
    std::stringstream message;
    message << "mfgunlock: Validated Warp Blend not applied"
            << (provider_version.empty() ? "" : " to DLSS-G " + provider_version)
            << " -- " << (detail.empty() ? "provider/kernel did not match the validated profile" : detail)
            << ".";
    reshade::log::message(reshade::log::level::warning, message.str().c_str());
    return;
  }

  g_validated_warp_detail = detail;
  g_validated_warp_modules.push_back(
      {mod, std::move(redirects), provider_version, detail});
  std::stringstream message;
  message << "mfgunlock: Validated Warp Blend applied to DLSS-G " << provider_version
          << " on Ampere -- " << detail << ".";
  reshade::log::message(reshade::log::level::info, message.str().c_str());
}

void TryPatchValidatedWarp() {
  for (HMODULE mod : g_dlssg_modules) PatchValidatedWarpInModule(mod);
}

void RestoreValidatedWarp() {
  for (auto& module : g_validated_warp_modules)
    mfgunlock::validatedwarp::Restore(module.redirects);
  g_validated_warp_modules.clear();
  g_validated_warp_rejected_modules.clear();
  g_validated_warp_detail.clear();
}

// ------------------------------------------------------ temporal (midpoint)
//
// Unlocking the multipliers gets the right NUMBER of generated frames; this
// gets the right CONTENT. Without it every generated frame is the same 0.5
// blend, so 4x shows three identical half-way frames and the motion is no
// smoother than 2x despite double the counter. See midpoint.hpp.

std::atomic_bool g_midpoint_patched{false};
struct MidpointModulePatch {
  HMODULE module;
  std::vector<mfgunlock::midpoint::Patch> patches;
  void* allocation;
};
std::vector<MidpointModulePatch> g_midpoint_modules;
std::vector<HMODULE> g_midpoint_rejected_modules;
std::string g_midpoint_detail;
bool HasTemporalPatch(HMODULE mod) {
  return std::any_of(g_midpoint_modules.begin(), g_midpoint_modules.end(),
                     [mod](const MidpointModulePatch& patch) { return patch.module == mod; });
}
void PatchMidpointInModule(HMODULE mod) {
  if (mod == nullptr) return;
  const auto* profile = mfgunlock::architecture::ActiveProfile();
  if (!g_enabled.load() || !profile) return;
  if (profile->NeedsRetarget() && !mfgunlock::provider::PrepareProvider(mod)) return;
  const auto already_patched = std::find_if(
      g_midpoint_modules.begin(), g_midpoint_modules.end(),
      [mod](const MidpointModulePatch& patch) { return patch.module == mod; });
  if (already_patched != g_midpoint_modules.end()) return;
  if (std::find(g_midpoint_rejected_modules.begin(), g_midpoint_rejected_modules.end(), mod) !=
      g_midpoint_rejected_modules.end()) {
    return;
  }

  std::vector<mfgunlock::midpoint::Patch> patches;
  void* allocation = nullptr;
  std::string detail;
  if (!mfgunlock::midpoint::Apply(mod, patches, allocation, detail)) {
    char module_path[MAX_PATH] = {};
    GetModuleFileNameA(mod, module_path, MAX_PATH);
    std::stringstream s;
    s << "mfgunlock: temporal fix not applied to " << module_path << " -- " << detail << ".";
    reshade::log::message(reshade::log::level::warning, s.str().c_str());
    g_midpoint_rejected_modules.push_back(mod);
    return;
  }

  g_midpoint_detail = detail;
  g_midpoint_modules.push_back({mod, std::move(patches), allocation});
  g_midpoint_patched.store(true, std::memory_order_release);
  char module_path[MAX_PATH] = {};
  GetModuleFileNameA(mod, module_path, MAX_PATH);
  std::stringstream s;
  s << "mfgunlock: temporal fix applied to " << module_path << " -- " << detail
    << "; generated frames should now land at their own time, not all at the midpoint.";
  reshade::log::message(reshade::log::level::info, s.str().c_str());
}

void TryPatchMidpoint() {
  for (HMODULE mod : g_dlssg_modules) PatchMidpointInModule(mod);
}

// Loader and capability callbacks share the provider inventory. A loader
// callback must not wait on maintenance which may itself be loading a DLL.
// The capability postflight rescans anything missed while that lock was held.
void ProcessLoadedDlssgModule(HMODULE mod) {
  if (!TryAcquireSRWLockExclusive(&g_provider_maintenance_lock)) {
    return;
  }
  struct Unlock {
    ~Unlock() { ReleaseSRWLockExclusive(&g_provider_maintenance_lock); }
  } unlock;
  RememberDlssgModule(mod);
  const auto* profile = mfgunlock::architecture::ActiveProfile();
  if (!g_enabled.load() || !profile) {
    return;
  }
  if (profile->NeedsRetarget()) {
    if (profile->architecture == mfgunlock::Architecture::kAmpere &&
        g_validated_warp_blend.load(std::memory_order_relaxed))
      PatchValidatedWarpInModule(mod);
    if (g_temporal_fix.load(std::memory_order_relaxed)) PatchMidpointInModule(mod);
    PatchArchGatesInModule(mod);
  } else {
    PatchArchGatesInModule(mod);
    if (g_temporal_fix.load(std::memory_order_relaxed)) PatchMidpointInModule(mod);
  }
}

void RunProviderMaintenance() {
  AcquireSRWLockExclusive(&g_provider_maintenance_lock);
  struct Unlock {
    ~Unlock() { ReleaseSRWLockExclusive(&g_provider_maintenance_lock); }
  } unlock;
  DiscoverDlssgModules();
  const auto* profile = mfgunlock::architecture::ActiveProfile();
  if (!g_enabled.load() || !profile) {
    return;
  }
  if (profile->NeedsRetarget()) {
    if (profile->architecture == mfgunlock::Architecture::kAmpere &&
        g_validated_warp_blend.load(std::memory_order_relaxed))
      TryPatchValidatedWarp();
    if (g_temporal_fix.load(std::memory_order_relaxed)) TryPatchMidpoint();
    TryPatchDlssgArchGate();
  } else {
    TryPatchDlssgArchGate();
    if (g_temporal_fix.load(std::memory_order_relaxed)) TryPatchMidpoint();
  }
}

void RestoreMidpoint() {
  if (!g_midpoint_patched.load(std::memory_order_acquire)) return;
  for (auto& module : g_midpoint_modules) {
    mfgunlock::midpoint::Restore(module.patches, module.allocation);
  }
  g_midpoint_modules.clear();
  g_midpoint_rejected_modules.clear();
  g_midpoint_patched.store(false, std::memory_order_release);
}

// ------------------------------------------------- flip metering (sl.dlss_g)
// Multi-frame output needs the plugin's software pacing path on architectures
// without hardware flip metering. The context field moves between plugin builds,
// so derive its offset and disabled value from the plugin's own fallback path
// instead of hardcoding either one.

constexpr char kFlipMarker[] = "FG1 DLL has been detected";

struct FlipSite {
  unsigned char* address;
  unsigned char original[7];
  unsigned char length;
};

std::atomic_bool g_flip_meter_patched{false};
std::atomic_bool g_flip_meter_failed{false};
std::vector<FlipSite> g_flip_meter_sites;

// Records the original bytes before writing, so the instruction can be put back
// exactly as it was. Patches here are either one byte (an immediate flipped in
// place) or seven (a whole store rewritten), never anything else.
bool WriteFlipSite(unsigned char* at, const unsigned char* bytes, size_t length) {
  if (length == 0 || length > sizeof(FlipSite::original)) return false;
  DWORD old_protect = 0;
  if (VirtualProtect(at, length, PAGE_EXECUTE_READWRITE, &old_protect) == 0) return false;
  FlipSite site = {};
  site.address = at;
  site.length = static_cast<unsigned char>(length);
  std::memcpy(site.original, at, length);
  g_flip_meter_sites.push_back(site);
  std::memcpy(at, bytes, length);
  DWORD ignored = 0;
  VirtualProtect(at, length, old_protect, &ignored);
  FlushInstructionCache(GetCurrentProcess(), at, length);
  return true;
}

bool ModuleImage(HMODULE mod, unsigned char** out_base, const IMAGE_NT_HEADERS64** out_nt) {
  if (mod == nullptr) return false;
  auto* base = reinterpret_cast<unsigned char*>(mod);
  const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
  if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
  const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
  if (nt->Signature != IMAGE_NT_SIGNATURE) return false;
  if (nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) return false;
  *out_base = base;
  *out_nt = nt;
  return true;
}

// ------------------------------------------- Streamline frame ceiling
// Some wrappers cache a stale NGX device maximum and clamp their compiled frame
// ceiling down to it. Neutralize only that lowering CMOV while preserving the
// plugin's own compiled maximum.

constexpr unsigned char kCeilingTarget = 5;  // generated frames == 6x

std::atomic_bool g_ceiling_patched{false};
unsigned char* g_ceiling_site = nullptr;
unsigned char g_ceiling_original = 0;
unsigned char g_ceiling_cmov_original = 0;
unsigned int g_ceiling_compiled = 0;
unsigned int g_ceiling_effective = 0;

void PatchFrameCountCeiling(HMODULE mod) {
  if (g_ceiling_patched.load(std::memory_order_acquire)) return;

  unsigned char* base = nullptr;
  const IMAGE_NT_HEADERS64* nt = nullptr;
  if (!ModuleImage(mod, &base, &nt)) return;

  const unsigned char tail[] = {0x3B, 0xCA, 0x0F, 0x42, 0xD1};
  unsigned char* found = nullptr;
  size_t hits = 0;
  const auto* section = IMAGE_FIRST_SECTION(nt);
  for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++section) {
    if ((section->Characteristics & IMAGE_SCN_MEM_EXECUTE) == 0) continue;
    unsigned char* start = base + section->VirtualAddress;
    const size_t size = section->Misc.VirtualSize;
    if (size < 10) continue;
    for (size_t off = 0; off + 10 <= size; ++off) {
      if (start[off] != 0xBA) continue;
      if (start[off + 2] != 0 || start[off + 3] != 0 || start[off + 4] != 0) continue;
      if (std::memcmp(start + off + 5, tail, sizeof(tail)) != 0) continue;
      const unsigned char ceiling = start[off + 1];
      if (ceiling == 0 || ceiling > 8) continue;
      if (found == nullptr) found = start + off;
      ++hits;
    }
  }

  if (hits != 1 || found == nullptr) {
    std::stringstream s;
    s << "mfgunlock: found " << hits
      << " frame-count clamps in the DLSS-G plugin (expected 1); leaving them alone.";
    reshade::log::message(reshade::log::level::warning, s.str().c_str());
    return;
  }

  DWORD old_protect = 0;
  if (VirtualProtect(found, 10, PAGE_EXECUTE_READWRITE, &old_protect) == 0) return;
  g_ceiling_site = found;
  g_ceiling_original = found[1];
  g_ceiling_cmov_original = found[9];
  g_ceiling_compiled = found[1];
  g_ceiling_effective = g_ceiling_compiled;
  if (g_raise_ceiling.load(std::memory_order_relaxed) && found[1] < kCeilingTarget) {
    found[1] = kCeilingTarget;
    g_ceiling_effective = kCeilingTarget;
  }
  found[9] = 0xD2;  // cmovb edx, ecx -> cmovb edx, edx
  DWORD ignored = 0;
  VirtualProtect(found, 10, old_protect, &ignored);
  FlushInstructionCache(GetCurrentProcess(), found, 10);
  g_ceiling_patched.store(true, std::memory_order_release);
  mfgunlock::framecount::g_advertised_max_generated.store(g_ceiling_effective,
                                                           std::memory_order_release);

  std::stringstream message;
  message << "mfgunlock: DLSS-G plugin ceiling: " << g_ceiling_compiled;
  if (g_ceiling_effective != g_ceiling_compiled)
    message << " -> " << g_ceiling_effective;
  message << " generated frame(s).";
  reshade::log::message(reshade::log::level::info, message.str().c_str());
}

void RestoreFrameCountCeiling() {
  if (!g_ceiling_patched.load(std::memory_order_acquire)) return;
  if (g_ceiling_site == nullptr) return;
  DWORD old_protect = 0;
  if (VirtualProtect(g_ceiling_site, 10, PAGE_EXECUTE_READWRITE, &old_protect) != 0) {
    g_ceiling_site[1] = g_ceiling_original;
    g_ceiling_site[9] = g_ceiling_cmov_original;
    DWORD ignored = 0;
    VirtualProtect(g_ceiling_site, 10, old_protect, &ignored);
    FlushInstructionCache(GetCurrentProcess(), g_ceiling_site, 10);
  }
  g_ceiling_site = nullptr;
  g_ceiling_original = 0;
  g_ceiling_cmov_original = 0;
  g_ceiling_compiled = 0;
  g_ceiling_effective = 0;
  mfgunlock::framecount::g_advertised_max_generated.store(0, std::memory_order_release);
  g_ceiling_patched.store(false, std::memory_order_release);
}

// Handles the DLSS-G Streamline plugin wherever it was loaded from. Returns true
// once a module has been dealt with, so the caller stops scanning.
bool TryPatchFlipMeteringInModule(HMODULE mod) {
  unsigned char* base = nullptr;
  const IMAGE_NT_HEADERS64* nt = nullptr;
  if (!ModuleImage(mod, &base, &nt)) return false;

  // 1. Is this the DLSS-G plugin? The marker string identifies it regardless of
  //    what the OTA layer decided to call the file.
  const size_t marker_len = sizeof(kFlipMarker) - 1;
  const unsigned char* marker = nullptr;
  const auto* section = IMAGE_FIRST_SECTION(nt);
  for (WORD i = 0; i < nt->FileHeader.NumberOfSections && marker == nullptr; ++i, ++section) {
    if ((section->Characteristics & IMAGE_SCN_MEM_READ) == 0) continue;
    unsigned char* start = base + section->VirtualAddress;
    const size_t size = section->Misc.VirtualSize;
    if (size < marker_len) continue;
    for (size_t off = 0; off + marker_len <= size; ++off) {
      if (std::memcmp(start + off, kFlipMarker, marker_len) == 0) {
        marker = start + off;
        break;
      }
    }
  }
  if (marker == nullptr) return false;

  // 2. Find the code referencing it, then read the (offset, value) the fallback
  //    writes: C6 /r disp32 imm8 == mov byte ptr [reg+disp32], imm8.
  unsigned int want_offset = 0;
  int want_value = -1;
  section = IMAGE_FIRST_SECTION(nt);
  for (WORD i = 0; i < nt->FileHeader.NumberOfSections && want_value < 0; ++i, ++section) {
    if ((section->Characteristics & IMAGE_SCN_MEM_EXECUTE) == 0) continue;
    unsigned char* start = base + section->VirtualAddress;
    const size_t size = section->Misc.VirtualSize;
    if (size < 8) continue;
    for (size_t off = 0; off + 8 <= size && want_value < 0; ++off) {
      // lea reg, [rip+disp32] pointing at the marker string
      if (!(start[off] == 0x48 || start[off] == 0x4C)) continue;
      if (start[off + 1] != 0x8D) continue;
      if ((start[off + 2] & 0xC7) != 0x05) continue;
      int disp = 0;
      std::memcpy(&disp, start + off + 3, sizeof(disp));
      if (start + off + 7 + disp != marker) continue;

      const size_t window = 0x200;
      const size_t limit = (off + window < size) ? (off + window) : size;
      for (size_t w = off; w + 7 <= limit; ++w) {
        if (start[w] != 0xC6) continue;
        if (start[w + 1] < 0x80 || start[w + 1] > 0xBF) continue;  // mod=10, disp32
        unsigned int field = 0;
        std::memcpy(&field, start + w + 2, sizeof(field));
        const unsigned char imm = start[w + 6];
        if (field <= 0x100 || field >= 0x20000) continue;
        if (imm > 1) continue;
        want_offset = field;
        want_value = imm;
        break;
      }
    }
  }

  if (want_value < 0) {
    // Name the module. Which DLSS-G plugin is actually loaded varies wildly --
    // game-bundled, driver OTA, or an NVIDIA App override copy -- and without
    // the path this warning says nothing actionable.
    char module_path[MAX_PATH] = {};
    GetModuleFileNameA(mod, module_path, MAX_PATH);
    std::stringstream s;
    s << "mfgunlock: located a DLSS-G plugin (" << module_path
      << ") but could not read its flip-metering fallback state; leaving it alone.";
    reshade::log::message(reshade::log::level::warning, s.str().c_str());
    g_flip_meter_failed.store(true, std::memory_order_release);
    return true;
  }

  // Pin the derived field to the fallback value. Handle the immediate store
  // directly; rewrite only the seven-byte REX register form whose instruction
  // boundary is identical to the replacement. Other encodings are left alone.
  const unsigned char opposite = static_cast<unsigned char>(1 - want_value);
  section = IMAGE_FIRST_SECTION(nt);
  for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++section) {
    if ((section->Characteristics & IMAGE_SCN_MEM_EXECUTE) == 0) continue;
    unsigned char* start = base + section->VirtualAddress;
    const size_t size = section->Misc.VirtualSize;
    if (size < 7) continue;
    for (size_t off = 0; off + 7 <= size; ++off) {
      // C6 /0 disp32 imm8 -- replace the immediate.
      if (start[off] == 0xC6) {
        if (start[off + 1] < 0x80 || start[off + 1] > 0xBF) continue;
        unsigned int field = 0;
        std::memcpy(&field, start + off + 2, sizeof(field));
        if (field != want_offset) continue;
        if (start[off + 6] != opposite) continue;
        const unsigned char imm = static_cast<unsigned char>(want_value);
        WriteFlipSite(start + off + 6, &imm, 1);
        continue;
      }

      // 40 88 /r disp32 -- replace the seven-byte register store in place.
      if (start[off] != 0x40 || start[off + 1] != 0x88) continue;
      const unsigned char modrm = start[off + 2];
      if (modrm < 0x80 || modrm > 0xBF) continue;  // mod=10, disp32
      const unsigned char rm = static_cast<unsigned char>(modrm & 7);
      if (rm == 4) continue;                       // rm=100 means a SIB byte follows
      unsigned int field = 0;
      std::memcpy(&field, start + off + 3, sizeof(field));
      if (field != want_offset) continue;

      unsigned char replacement[7] = {0xC6, static_cast<unsigned char>(0x80 | rm),
                                      0,    0,
                                      0,    0,
                                      static_cast<unsigned char>(want_value)};
      std::memcpy(replacement + 2, &want_offset, sizeof(want_offset));
      WriteFlipSite(start + off, replacement, sizeof(replacement));
    }
  }

  char module_path[MAX_PATH] = {};
  GetModuleFileNameA(mod, module_path, MAX_PATH);

  // Deriving the field alone is not success; at least one write must change.
  if (g_flip_meter_sites.empty()) {
    std::stringstream s;
    s << "mfgunlock: flip-metering field +0x" << std::hex << want_offset << std::dec
      << " derived from " << module_path
      << ", but nothing writes it in a form this can patch -- no immediate store of "
      << (1 - want_value) << ", and no 7-byte register store. Nothing changed.";
    reshade::log::message(reshade::log::level::warning, s.str().c_str());
    g_flip_meter_failed.store(true, std::memory_order_release);
    return true;
  }

  g_flip_meter_patched.store(true, std::memory_order_release);

  std::stringstream s;
  s << "mfgunlock: forced flip-metering off in " << module_path << " -- field +0x" << std::hex
    << want_offset << std::dec << " pinned to " << want_value << " at "
    << g_flip_meter_sites.size() << " site(s); multi-frame should pace in software (RSYNC).";
  reshade::log::message(reshade::log::level::info, s.str().c_str());

  // Same module, and by now we know it is the right one.
  PatchFrameCountCeiling(mod);
  return true;
}

void TryPatchFlipMetering() {
  if (!g_enabled.load() || !mfgunlock::architecture::ActiveProfile()) return;
  if (g_flip_meter_patched.load(std::memory_order_acquire)) return;
  if (g_flip_meter_failed.load(std::memory_order_acquire)) return;

  HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, GetCurrentProcessId());
  if (snap == INVALID_HANDLE_VALUE) return;

  MODULEENTRY32W me = {};
  me.dwSize = sizeof(me);
  if (Module32FirstW(snap, &me)) {
    do {
      if (me.hModule == g_self_module) continue;
      if (TryPatchFlipMeteringInModule(me.hModule)) break;
    } while (Module32NextW(snap, &me));
  }
  CloseHandle(snap);
}

void RestoreFlipMetering() {
  if (!g_flip_meter_patched.load(std::memory_order_acquire)) return;
  for (const auto& site : g_flip_meter_sites) {
    DWORD old_protect = 0;
    if (VirtualProtect(site.address, site.length, PAGE_EXECUTE_READWRITE, &old_protect) != 0) {
      std::memcpy(site.address, site.original, site.length);
      DWORD ignored = 0;
      VirtualProtect(site.address, site.length, old_protect, &ignored);
      FlushInstructionCache(GetCurrentProcess(), site.address, site.length);
    }
  }
  g_flip_meter_sites.clear();
  g_flip_meter_patched.store(false, std::memory_order_release);
  g_flip_meter_failed.store(false, std::memory_order_release);
}

bool IsVersion(const mfgunlock::streamline::internal::ModuleVersion& version,
               unsigned int major, unsigned int minor, unsigned int build) {
  return version.major == major && version.minor == minor &&
         version.build == build;
}

bool DynamicRuntimeStackReady() {
  if (g_dynamic_stack_validated.load(std::memory_order_acquire)) return true;
  const auto* profile = mfgunlock::architecture::ActiveProfile();
  if (!g_enabled.load() || !profile ||
      profile->architecture != mfgunlock::Architecture::kAmpere) return false;

  HMODULE interposer = GetModuleHandleW(L"sl.interposer.dll");
  if (!interposer || !IsVersion(mfgunlock::streamline::internal::ReadModuleVersion(interposer),
                                2, 14, 1)) return false;

  bool plugin_ok = false;
  if (!TryAcquireSRWLockShared(&mfgunlock::streamline::internal::g_plugin_lock)) return false;
  for (const auto& plugin : mfgunlock::streamline::internal::g_plugins) {
    if (!plugin.module) continue;
    if (IsVersion(plugin.version, 2, 14, 1)) {
      plugin_ok = true;
      break;
    }
  }
  ReleaseSRWLockShared(&mfgunlock::streamline::internal::g_plugin_lock);
  if (!plugin_ok) return false;

  bool provider_ok = false;
  if (!TryAcquireSRWLockShared(&mfgunlock::provider::internal::g_lock)) return false;
  for (const auto& provider : mfgunlock::provider::internal::g_providers) {
    if (!provider.ready) continue;
    const auto version = mfgunlock::streamline::internal::ReadModuleVersion(provider.module);
    if (IsVersion(version, 310, 9, 1)) {
      provider_ok = true;
      break;
    }
  }
  ReleaseSRWLockShared(&mfgunlock::provider::internal::g_lock);
  if (!provider_ok) return false;

  g_dynamic_stack_validated.store(true, std::memory_order_release);
  mfgunlock::provider::Log("validated Dynamic MFG runtime stack: Streamline 2.14.1 + DLSS-G 310.9.1");
  return true;
}

void RefreshRuntime() {
  mfgunlock::ngx::BeforeInit();
  RunProviderMaintenance();
  if (g_force_flip_meter_off.load(std::memory_order_relaxed)) TryPatchFlipMetering();
  mfgunlock::framecount::TryInstall();
  mfgunlock::loadhook::TryInstall();
}

// Graphics initialization is also a finite fallback for already loaded modules.
void OnInitDevice(reshade::api::device* device) {
  if (device != nullptr && device->get_api() == reshade::api::device_api::d3d12)
    mfgunlock::framecount::NotifyDynamicD3D12(true);
  RefreshRuntime();
}

void OnInitCommandQueue(reshade::api::command_queue* /*queue*/) {
  RefreshRuntime();
}

// ---------------------------------------------------------------- config

std::atomic_bool g_early_load_restart_required{false};

void EnsureEarlyLoadConfig() {
  size_t value_size = 0;
  if (!reshade::get_config_value(nullptr, kAddonSection, kEarlyLoadKey, nullptr, &value_size)) {
    reshade::set_config_value(nullptr, kAddonSection, kEarlyLoadKey, kAddonFileName);
    g_early_load_restart_required.store(true, std::memory_order_release);
    mfgunlock::provider::Log("early loading configured; restart the game once");
    return;
  }

  if (value_size == 0) return;
  std::vector<char> values(value_size);
  size_t actual_size = values.size();
  if (!reshade::get_config_value(nullptr, kAddonSection, kEarlyLoadKey,
                                 values.data(), &actual_size)) return;
  values.resize(actual_size);
  if (mfgunlock::early_load::Contains(values, kAddonFileName)) return;

  const auto merged = mfgunlock::early_load::Append(values, kAddonFileName);
  reshade::set_config_value(nullptr, kAddonSection, kEarlyLoadKey,
                            merged.data(), merged.size());
  g_early_load_restart_required.store(true, std::memory_order_release);
  mfgunlock::provider::Log("early loading configured; restart the game once");
}

// ---------------------------------------------------------------- overlay

void OnRegisterOverlay(reshade::api::effect_runtime* /*runtime*/) {
  if (g_early_load_restart_required.load(std::memory_order_acquire)) {
    ImGui::Text("Restart the game once to enable early loading.");
    ImGui::Separator();
  }
  mfgunlock::streamline::Draw();
  bool enabled = g_enabled.load(std::memory_order_relaxed);
  if (ImGui::Checkbox("Enable multi-frame override", &enabled)) {
    g_enabled.store(enabled, std::memory_order_relaxed);
    reshade::set_config_value(nullptr, kConfigSection, "Enabled", enabled ? 1 : 0);
  }

  static const char* kRuntimeModes[] = {
      "Game default", "Prefer local runtime", "Force NVIDIA OTA runtime"};
  int runtime_mode = static_cast<int>(
      mfgunlock::framecount::g_runtime_selection_mode.load(std::memory_order_relaxed));
  if (ImGui::Combo("Streamline runtime", &runtime_mode, kRuntimeModes, ARRAYSIZE(kRuntimeModes))) {
    mfgunlock::framecount::g_runtime_selection_mode.store(
        static_cast<unsigned int>(runtime_mode), std::memory_order_relaxed);
    reshade::set_config_value(nullptr, kConfigSection, "RuntimeSelectionMode", runtime_mode);
  }
  ImGui::TextDisabled("Restart required after changing the Streamline runtime policy.");

  int count = static_cast<int>(g_max_count.load(std::memory_order_relaxed));
  if (ImGui::SliderInt("Reported MultiFrameCountMax", &count,
                       static_cast<int>(kMinCount), static_cast<int>(kMaxCount))) {
    if (count < static_cast<int>(kMinCount)) count = static_cast<int>(kMinCount);
    if (count > static_cast<int>(kMaxCount)) count = static_cast<int>(kMaxCount);
    g_max_count.store(static_cast<unsigned int>(count), std::memory_order_relaxed);
    reshade::set_config_value(nullptr, kConfigSection, "MaxCount", count);
  }
  ImGui::TextDisabled(
      "Takes effect when DLSS-G next queries the runtime -- toggle frame\n"
      "generation off and on in the game if the option does not appear.");

  bool flip_off = g_force_flip_meter_off.load(std::memory_order_relaxed);
  if (ImGui::Checkbox("Force legacy software flip pacing (compatibility)", &flip_off)) {
    g_force_flip_meter_off.store(flip_off, std::memory_order_relaxed);
    reshade::set_config_value(nullptr, kConfigSection, "ForceFlipMeteringOff", flip_off ? 1 : 0);
  }
  ImGui::TextDisabled(
      "Leave off with current Streamline builds. Enable only if 3x/4x freezes;\n"
      "applied once per session, so restart the game after changing it.");

  ImGui::Separator();
  if (g_flip_meter_patched.load(std::memory_order_acquire)) {
    ImGui::TextUnformatted("Software pacing: active");
  } else if (g_flip_meter_failed.load(std::memory_order_acquire)) {
    ImGui::TextDisabled("Software pacing: unavailable; see the ReShade log.");
  } else {
    ImGui::TextDisabled("Software pacing: inactive.");
  }
  if (g_ceiling_patched.load(std::memory_order_acquire))
    ImGui::Text("Streamline maximum: %ux.", g_ceiling_effective + 1);
  if (mfgunlock::framecount::g_capacity_advertised.load(std::memory_order_relaxed)) {
    ImGui::Text("Native menu maximum: runtime %ux, advertised %ux.",
                mfgunlock::framecount::g_runtime_max_generated.load(std::memory_order_relaxed) + 1,
                mfgunlock::framecount::g_advertised_max_generated.load(
                    std::memory_order_relaxed) + 1);
  }
  if (mfgunlock::framecount::g_state_seen.load(std::memory_order_relaxed)) {
    const unsigned int status =
        mfgunlock::framecount::g_dlssg_status.load(std::memory_order_relaxed);
    if (status != 0) ImGui::Text("DLSS-G runtime status: 0x%x.", status);
  }

  int force = static_cast<int>(
      mfgunlock::framecount::g_force_multiplier.load(std::memory_order_relaxed));
  // The Streamline API expresses this as generated frames, so 6x maps to 5.
  if (ImGui::SliderInt("Force frame multiplier", &force, 0, 6,
                       force == 0 ? "off (game decides)" : "%dx")) {
    if (force != 0 && force < 2) force = 2;
    mfgunlock::framecount::g_force_multiplier.store(static_cast<unsigned int>(force),
                                                    std::memory_order_relaxed);
    reshade::set_config_value(nullptr, kConfigSection, "ForceMultiplier", force);
  }
  ImGui::TextDisabled(
      "Leave off for games with their own 2x/3x/4x selector -- forcing would\n"
      "override your in-game choice. Use it where frame gen is only on/off.");
  if (mfgunlock::framecount::g_intercepted.load(std::memory_order_relaxed)) {
    ImGui::Text("Game asked for %ux, forced to %u generated frame(s).",
                mfgunlock::framecount::g_last_requested.load(std::memory_order_relaxed) + 1,
                mfgunlock::framecount::g_last_forced.load(std::memory_order_relaxed));
  } else if (mfgunlock::framecount::g_declined_no_pacing.load(std::memory_order_relaxed)) {
    ImGui::TextWrapped("Declined to force: flip metering was still on when DLSS-G started.");
  } else if (mfgunlock::framecount::g_feature_function_hooked.load(std::memory_order_acquire)) {
    ImGui::TextDisabled("slGetFeatureFunction hook installed; DLSS-G not exercised yet.");
  } else if (mfgunlock::framecount::g_init_hooked.load(std::memory_order_acquire)) {
    ImGui::TextDisabled("Streamline startup hook installed; no slGetFeatureFunction export.");
  } else if (GetModuleHandleW(L"sl.interposer.dll")) {
    ImGui::TextDisabled("Streamline multiplier control: unavailable.");
  }

  ImGui::Separator();
  bool dynamic_mfg = mfgunlock::framecount::g_dynamic_mfg_enabled.load(
      std::memory_order_relaxed);
  if (ImGui::Checkbox("NVIDIA Dynamic MFG", &dynamic_mfg)) {
    mfgunlock::framecount::g_dynamic_mfg_enabled.store(dynamic_mfg,
                                                       std::memory_order_relaxed);
    mfgunlock::framecount::NotifyDynamicModeChanged();
    reshade::set_config_value(nullptr, kConfigSection, "DynamicMFG",
                              dynamic_mfg ? 1 : 0);
  }
  int dynamic_target = static_cast<int>(
      mfgunlock::framecount::g_dynamic_target_fps.load(std::memory_order_relaxed));
  if (dynamic_mfg && ImGui::SliderInt("Dynamic output target", &dynamic_target, 0, 480,
                                     dynamic_target == 0 ? "display refresh" : "%d FPS")) {
    mfgunlock::framecount::g_dynamic_target_fps.store(
        static_cast<unsigned int>(dynamic_target), std::memory_order_relaxed);
    mfgunlock::framecount::NotifyDynamicModeChanged();
    reshade::set_config_value(nullptr, kConfigSection, "DynamicTargetFPS", dynamic_target);
  }
  ImGui::TextDisabled(
      "Ampere + D3D12 + Streamline 2.14.1 + DLSS-G 310.9.1 only.\n"
      "When accepted, NVIDIA owns multiplier selection and pacing; 0 follows display refresh.");
  const auto* active_profile = mfgunlock::architecture::ActiveProfile();
  if (!active_profile || active_profile->architecture != mfgunlock::Architecture::kAmpere) {
    ImGui::TextDisabled("Dynamic MFG: Ampere profile required.");
  } else if (!mfgunlock::framecount::g_dynamic_d3d12.load(std::memory_order_relaxed)) {
    ImGui::TextDisabled("Dynamic MFG: waiting for a D3D12 device.");
  } else if (!mfgunlock::framecount::DynamicStackReady()) {
    ImGui::TextDisabled("Dynamic MFG: validated 2.14.1 / 310.9.1 runtime stack not active.");
  } else if (!mfgunlock::framecount::g_dynamic_support_seen.load(std::memory_order_acquire)) {
    ImGui::TextDisabled("Dynamic MFG: capability pending; toggle frame generation once.");
  } else if (!mfgunlock::framecount::g_dynamic_supported.load(std::memory_order_relaxed)) {
    ImGui::TextDisabled("Dynamic MFG: runtime reports unsupported.");
  } else if (mfgunlock::framecount::g_dynamic_applied.load(std::memory_order_relaxed)) {
    ImGui::TextUnformatted("Dynamic MFG: active.");
  } else if (mfgunlock::framecount::g_dynamic_fell_back.load(std::memory_order_relaxed)) {
    ImGui::Text("Dynamic MFG: rejected (sl::Result 0x%x); fixed mode restored.",
                mfgunlock::framecount::g_dynamic_result.load(std::memory_order_relaxed));
  } else if (dynamic_mfg) {
    ImGui::TextDisabled("Dynamic MFG: ready; toggle frame generation off/on to submit it.");
  }

  ImGui::Separator();
  bool warp = g_validated_warp_blend.load(std::memory_order_relaxed);
  if (ImGui::Checkbox("Validated Warp Blend (Ampere, 310.9.1)", &warp)) {
    g_validated_warp_blend.store(warp, std::memory_order_relaxed);
    reshade::set_config_value(nullptr, kConfigSection, "ValidatedWarpBlend", warp ? 1 : 0);
  }
  ImGui::TextDisabled("Quality patch; exact provider/kernel identity only. Restart required.");
  if (!g_validated_warp_modules.empty()) {
    ImGui::TextWrapped("Validated Warp Blend: active; %s.", g_validated_warp_detail.c_str());
  } else if (warp && !g_validated_warp_detail.empty()) {
    ImGui::TextWrapped("Validated Warp Blend: not applied; %s.", g_validated_warp_detail.c_str());
  } else if (warp) {
    ImGui::TextDisabled("Validated Warp Blend: pending restart/provider load.");
  }

  ImGui::Separator();
  bool temporal = g_temporal_fix.load(std::memory_order_relaxed);
  if (ImGui::Checkbox("Temporal fix (stops all frames landing at the midpoint)", &temporal)) {
    g_temporal_fix.store(temporal, std::memory_order_relaxed);
    reshade::set_config_value(nullptr, kConfigSection, "TemporalFix", temporal ? 1 : 0);
  }
  ImGui::TextDisabled("Applied once at load; restart the game to change it.");

  ImGui::Separator();
  if (g_midpoint_patched.load(std::memory_order_acquire)) {
    ImGui::TextWrapped("Temporal fix: %zu provider(s); last result: %s.",
                       g_midpoint_modules.size(), g_midpoint_detail.c_str());
  } else {
    ImGui::TextDisabled("Temporal fix: pending.");
  }

}

void LoadConfig() {
  int value = 0;
  if (reshade::get_config_value(nullptr, kConfigSection, "Enabled", value)) {
    g_enabled.store(value != 0, std::memory_order_relaxed);
  }
  char architecture_name[32] = {};
  size_t architecture_length = sizeof(architecture_name);
  const bool has_architecture = reshade::get_config_value(
      nullptr, kConfigSection, "Architecture", architecture_name, &architecture_length);
  architecture_name[sizeof(architecture_name) - 1] = '\0';
  const auto selected = has_architecture && architecture_length > sizeof(architecture_name)
      ? mfgunlock::Architecture::kUnknown
      : mfgunlock::architecture::Parse(has_architecture ? architecture_name : nullptr);
  mfgunlock::architecture::Configure(selected);
  if (selected == mfgunlock::Architecture::kUnknown)
    mfgunlock::provider::Log("invalid Architecture setting", true);
  if (reshade::get_config_value(nullptr, kConfigSection, "MaxCount", value)) {
    if (value < static_cast<int>(kMinCount)) value = static_cast<int>(kMinCount);
    if (value > static_cast<int>(kMaxCount)) value = static_cast<int>(kMaxCount);
    g_max_count.store(static_cast<unsigned int>(value), std::memory_order_relaxed);
  }
  if (reshade::get_config_value(nullptr, kConfigSection, "ForceFlipMeteringOff", value)) {
    g_force_flip_meter_off.store(value != 0, std::memory_order_relaxed);
  }
  if (reshade::get_config_value(nullptr, kConfigSection, "TemporalFix", value)) {
    g_temporal_fix.store(value != 0, std::memory_order_relaxed);
  }
  if (reshade::get_config_value(nullptr, kConfigSection, "ValidatedWarpBlend", value)) {
    g_validated_warp_blend.store(value != 0, std::memory_order_relaxed);
  }
  if (reshade::get_config_value(nullptr, kConfigSection, "DynamicMFG", value)) {
    mfgunlock::framecount::g_dynamic_mfg_enabled.store(value != 0,
                                                       std::memory_order_relaxed);
  }
  if (reshade::get_config_value(nullptr, kConfigSection, "DynamicTargetFPS", value)) {
    if (value < 0) value = 0;
    if (value > 480) value = 480;
    mfgunlock::framecount::g_dynamic_target_fps.store(
        static_cast<unsigned int>(value), std::memory_order_relaxed);
  }
  if (reshade::get_config_value(nullptr, kConfigSection, "RaiseFrameCeiling", value)) {
    g_raise_ceiling.store(value != 0, std::memory_order_relaxed);
  }
  if (reshade::get_config_value(nullptr, kConfigSection, "RuntimeSelectionMode", value)) {
    if (value < static_cast<int>(mfgunlock::framecount::RuntimeSelectionMode::kGameDefault) ||
        value > static_cast<int>(mfgunlock::framecount::RuntimeSelectionMode::kForceOta)) {
      value = static_cast<int>(mfgunlock::framecount::RuntimeSelectionMode::kGameDefault);
    }
    mfgunlock::framecount::g_runtime_selection_mode.store(
        static_cast<unsigned int>(value), std::memory_order_relaxed);
  } else if (reshade::get_config_value(nullptr, kConfigSection, "ForceOTAPlugins", value) &&
             value != 0) {
    // Backward-compatible migration for configurations written before the
    // three-way runtime selector existed.
    mfgunlock::framecount::g_runtime_selection_mode.store(
        static_cast<unsigned int>(mfgunlock::framecount::RuntimeSelectionMode::kForceOta),
        std::memory_order_relaxed);
  }
  if (reshade::get_config_value(nullptr, kConfigSection, "ForceMultiplier", value)) {
    if (value != 0 && (value < 2 || value > 6)) value = 0;
    mfgunlock::framecount::g_force_multiplier.store(static_cast<unsigned int>(value),
                                                    std::memory_order_relaxed);
  }
}

}  // namespace

extern "C" __declspec(dllexport) constexpr const char* NAME = "MFG Unlock";
extern "C" __declspec(dllexport) constexpr const char* DESCRIPTION =
    "Native NVIDIA DLSS frame generation compatibility and multiplier control";

BOOL APIENTRY DllMain(HMODULE h_module, DWORD fdw_reason, LPVOID /*lpv_reserved*/) {
  switch (fdw_reason) {
    case DLL_PROCESS_ATTACH:
      g_self_module = h_module;
      if (!reshade::register_addon(h_module)) return FALSE;
      LoadConfig();
      EnsureEarlyLoadConfig();
      mfgunlock::streamline::Initialize(g_self_module);
      mfgunlock::ngx::g_prepare_loaded_providers = RunProviderMaintenance;
      mfgunlock::streamline::g_on_interposer_loaded = mfgunlock::framecount::TryInstall;
      mfgunlock::loadhook::g_on_get_proc_address = mfgunlock::streamline::Resolve;
      mfgunlock::framecount::g_on_init = mfgunlock::streamline::OnInit;
      mfgunlock::framecount::g_init_compatible = mfgunlock::streamline::CanHookInit;
      mfgunlock::framecount::g_multi_frame_ready = []() {
        const auto* profile = mfgunlock::architecture::ActiveProfile();
        if (!g_enabled.load() || !profile) return false;
        if (!profile->NeedsRetarget()) return true;
        return mfgunlock::provider::PreparedProviderCount() == 1 &&
               g_midpoint_patched.load(std::memory_order_acquire) &&
               g_gate_patched.load(std::memory_order_acquire);
      };
      mfgunlock::framecount::g_dynamic_stack_ready = DynamicRuntimeStackReady;
      mfgunlock::ngx::g_capability_limit = []() -> unsigned int {
        return mfgunlock::framecount::g_multi_frame_ready() ? g_max_count.load() : 0;
      };
      // The fixed-count compatibility path can request the legacy software
      // pacing fallback after the DLSS-G plugin is loaded. Dynamic MFG returns
      // before this path and keeps pacing under the current NVIDIA runtime.
      mfgunlock::framecount::g_ensure_pacing = []() { TryPatchFlipMetering(); };
      mfgunlock::framecount::g_pacing_ready = []() {
        return g_flip_meter_patched.load(std::memory_order_acquire);
      };

      // Install load-time discovery before the bootstrap scan. Already mapped
      // providers are found by that one shared scan; anything mapped later is
      // patched directly here without enumerating modules from Present.
      mfgunlock::loadhook::g_on_interposer_loaded = []() {
        mfgunlock::streamline::internal::InstallLegacyInitHook();
        mfgunlock::streamline::internal::InstallSupportHook();
        mfgunlock::framecount::TryInstall();
      };
      mfgunlock::loadhook::g_on_ngx_loaded = mfgunlock::ngx::TryInstall;
      mfgunlock::loadhook::g_on_dlssg_loaded = ProcessLoadedDlssgModule;
      mfgunlock::loadhook::TryInstall();
      mfgunlock::ngx::EnsureEntryHooks();
      mfgunlock::streamline::internal::InstallLegacyInitHook();
      mfgunlock::streamline::internal::InstallSupportHook();
      RunProviderMaintenance();
      if (g_force_flip_meter_off.load(std::memory_order_relaxed)) TryPatchFlipMetering();
      mfgunlock::framecount::TryInstall();

      reshade::register_overlay("MFG Unlock", OnRegisterOverlay);
      reshade::register_event<reshade::addon_event::init_device>(OnInitDevice);
      reshade::register_event<reshade::addon_event::init_command_queue>(OnInitCommandQueue);
      break;
    case DLL_PROCESS_DETACH:
      reshade::unregister_event<reshade::addon_event::init_command_queue>(OnInitCommandQueue);
      reshade::unregister_event<reshade::addon_event::init_device>(OnInitDevice);
      reshade::unregister_overlay("MFG Unlock", OnRegisterOverlay);
      mfgunlock::loadhook::Uninstall();
      // Detach entry hooks before restoring mapped-image patches.
      mfgunlock::loadhook::g_on_get_proc_address = nullptr;
      mfgunlock::loadhook::g_on_dlssg_loaded = nullptr;
      mfgunlock::loadhook::g_on_interposer_loaded = nullptr;
      mfgunlock::loadhook::g_on_ngx_loaded = nullptr;
      mfgunlock::ngx::g_capability_limit = nullptr;
      mfgunlock::framecount::g_on_init = nullptr;
      mfgunlock::framecount::g_init_compatible = nullptr;
      mfgunlock::framecount::g_multi_frame_ready = nullptr;
      mfgunlock::framecount::g_dynamic_stack_ready = nullptr;
      mfgunlock::ngx::g_prepare_loaded_providers = nullptr;
      mfgunlock::streamline::Shutdown();
      mfgunlock::framecount::Uninstall();
      RestoreValidatedWarp();
      RestoreMidpoint();
      RestoreDlssgArchGate();
      mfgunlock::provider::Restore();
      RestoreFrameCountCeiling();
      RestoreFlipMetering();
      reshade::unregister_addon(h_module);
      break;
    default:
      break;
  }
  return TRUE;
}
