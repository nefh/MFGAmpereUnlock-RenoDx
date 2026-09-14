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
#include <array>
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
#include "./fg_preset.hpp"
#include "./framecount.hpp"
#include "./loadhook.hpp"
#include "./midpoint.hpp"
#include "./streamline_bridge.hpp"
#include "./ui_candidate.hpp"
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
std::atomic_bool g_show_debug_options{false};
std::atomic_int g_validated_warp_mode{
    static_cast<int>(mfgunlock::validatedwarp::Mode::kValidatedWarp)};
std::atomic_bool g_raise_ceiling{false};
std::atomic_bool g_dynamic_stack_validated{false};
std::atomic<reshade::api::swapchain*> g_primary_swapchain{nullptr};
std::atomic<uint64_t> g_primary_swapchain_area{0};
std::atomic<uint32_t> g_primary_output_width{0};
std::atomic<uint32_t> g_primary_output_height{0};
std::atomic_bool g_ui_candidate_d3d12{false};

constexpr size_t kMaxUiRuntimeCandidates = 64;
constexpr size_t kMaxKnownSwapchainBuffers = 16;
constexpr uint64_t kUiCandidateMaxAgeMs = 250;

struct UiRuntimeCandidate {
  bool used = false;
  bool swapchain = false;
  bool already_tagged = false;
  bool state_known = false;
  bool last_clear_transparent = false;
  reshade::api::resource resource{};
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t format = 0;
  uint32_t view_format = 0;
  uint32_t state = 0;
  uint32_t rtv_binds_since_clear = 0;
  uint32_t rtv_binds = 0;
  uint32_t transparent_clears = 0;
  uint32_t current_late_writes = 0;
  uint32_t last_late_writes = 0;
  uint64_t clear_serial = 0;
  uint64_t evaluated_clear_serial = 0;
  uint64_t injected_clear_serial = 0;
  uint64_t hudless_clear_serial = 0;
  uint64_t last_clear_ms = 0;
};

struct UiCandidateDebugSnapshot {
  bool present = false;
  bool stable = false;
  bool safe = false;
  bool ambiguous = false;
  bool overflow = false;
  bool family_valid = false;
  bool swapchain = false;
  bool already_tagged = false;
  bool state_known = false;
  bool transparent_clear = false;
  uint64_t resource = 0;
  uint64_t clear_serial = 0;
  uint64_t age_ms = 0;
  uint64_t family_age_ms = 0;
  uint64_t family_handoffs = 0;
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t format = 0;
  uint32_t state = 0;
  uint32_t stable_frames = 0;
  uint32_t rtv_binds_after_clear = 0;
  uint32_t reject_reasons = mfgunlock::uicandidate::kStale;
  uint32_t late_writes = 0;
  uint32_t family_width = 0;
  uint32_t family_height = 0;
  uint32_t family_format = 0;
};

SRWLOCK g_ui_candidate_lock = SRWLOCK_INIT;
std::array<UiRuntimeCandidate, kMaxUiRuntimeCandidates> g_ui_candidates{};
std::array<uint64_t, kMaxKnownSwapchainBuffers> g_known_swapchain_buffers{};
bool g_ui_candidate_overflow = false;
bool g_ui_candidate_ambiguous = false;
uint64_t g_ui_selected_resource = 0;
uint64_t g_ui_selected_clear_serial = 0;
uint32_t g_ui_selected_stable_frames = 0;
bool g_ui_candidate_family_valid = false;
uint32_t g_ui_candidate_family_width = 0;
uint32_t g_ui_candidate_family_height = 0;
uint32_t g_ui_candidate_family_format = 0;
uint64_t g_ui_candidate_family_last_seen_ms = 0;
uint64_t g_ui_candidate_family_handoffs = 0;

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
  mfgunlock::validatedwarp::Mode mode = mfgunlock::validatedwarp::Mode::kValidatedWarp;
};
std::vector<ValidatedWarpModulePatch> g_validated_warp_modules;
std::vector<HMODULE> g_validated_warp_rejected_modules;
std::string g_validated_warp_detail;

void PatchValidatedWarpInModule(HMODULE mod) {
  if (mod == nullptr || !g_validated_warp_blend.load(std::memory_order_relaxed)) return;
  const auto* profile = mfgunlock::architecture::ActiveProfile();
  if (!g_enabled.load() || !profile || profile->architecture != mfgunlock::Architecture::kAmpere)
    return;
  if (std::any_of(g_validated_warp_modules.begin(), g_validated_warp_modules.end(),
                  [mod](const auto& patch) { return patch.module == mod; })) return;
  if (std::find(g_validated_warp_rejected_modules.begin(),
                g_validated_warp_rejected_modules.end(), mod) !=
      g_validated_warp_rejected_modules.end()) return;

  std::vector<mfgunlock::validatedwarp::Redirect> redirects;
  mfgunlock::validatedwarp::Result result{};
  std::string provider_version;
  const auto mode = mfgunlock::validatedwarp::ConfiguredMode(
      g_show_debug_options.load(std::memory_order_relaxed),
      g_validated_warp_mode.load(std::memory_order_relaxed));
  const bool applied = mfgunlock::validatedwarp::Apply(
      mod, redirects, result, provider_version, mode, mfgunlock::provider::PrepareProvider);
  const std::string detail = result.detail;
  if (!applied || !result.applied) {
    g_validated_warp_rejected_modules.push_back(mod);
    if (g_validated_warp_modules.empty()) g_validated_warp_detail = detail;
    std::stringstream message;
    message << "mfgunlock: Warp diagnostic " << mfgunlock::validatedwarp::ModeName(mode)
            << " not applied"
            << (provider_version.empty() ? "" : " to DLSS-G " + provider_version)
            << " -- " << (detail.empty() ? "provider/kernel did not match the validated profile" : detail)
            << ".";
    reshade::log::message(reshade::log::level::warning, message.str().c_str());
    return;
  }

  g_validated_warp_detail = detail;
  g_validated_warp_modules.push_back(
      {mod, std::move(redirects), provider_version, detail, mode});
  std::stringstream message;
  message << "mfgunlock: Warp diagnostic " << mfgunlock::validatedwarp::ModeName(mode)
          << " applied to DLSS-G " << provider_version
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
  mfgunlock::fgpreset::TryInstall(mod);
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
  for (HMODULE mod : g_dlssg_modules) mfgunlock::fgpreset::TryInstall(mod);
}

// A preset edit must not apply unrelated pending Warp/temporal changes live.
void InstallPresetForLoadedProviders() {
  if (!mfgunlock::architecture::ActiveProfile()) return;
  AcquireSRWLockExclusive(&g_provider_maintenance_lock);
  for (HMODULE mod : g_dlssg_modules) mfgunlock::fgpreset::TryInstall(mod);
  ReleaseSRWLockExclusive(&g_provider_maintenance_lock);
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

bool UiCandidateObservationEnabled() {
  return g_show_debug_options.load(std::memory_order_relaxed) ||
         mfgunlock::framecount::g_ui_candidate_injection_enabled.load(
             std::memory_order_relaxed);
}

bool IsUiCandidateFormat(reshade::api::format resource_format,
                         reshade::api::format view_format) {
  const auto unorm = reshade::api::format::r8g8b8a8_unorm;
  const auto srgb = reshade::api::format::r8g8b8a8_unorm_srgb;
  return resource_format == view_format &&
         (resource_format == unorm || resource_format == srgb);
}

bool IsTransparentClear(const float color[4]) {
  return color != nullptr && color[0] == 0.0f && color[1] == 0.0f &&
         color[2] == 0.0f && color[3] == 0.0f;
}

bool ToD3D12ResourceState(reshade::api::resource_usage usage, uint32_t& state) {
  if (usage == reshade::api::resource_usage::undefined) return false;
  if (usage == reshade::api::resource_usage::general ||
      usage == reshade::api::resource_usage::present) {
    state = 0;
    return true;
  }
  const uint32_t value = static_cast<uint32_t>(usage);
  if ((value & 0x80000000u) != 0) return false;
  state = value;
  return true;
}

bool IsKnownSwapchainBufferLocked(uint64_t resource) {
  return std::find(g_known_swapchain_buffers.begin(), g_known_swapchain_buffers.end(),
                   resource) != g_known_swapchain_buffers.end();
}

UiRuntimeCandidate* FindUiRuntimeCandidateLocked(uint64_t resource) {
  UiRuntimeCandidate* empty = nullptr;
  for (auto& candidate : g_ui_candidates) {
    if (candidate.used && candidate.resource.handle == resource) return &candidate;
    if (!candidate.used && empty == nullptr) empty = &candidate;
  }
  return empty;
}

void ResetUiSelectionLocked() {
  g_ui_selected_resource = 0;
  g_ui_selected_clear_serial = 0;
  g_ui_selected_stable_frames = 0;
}

void ResetUiFamilyLocked() {
  g_ui_candidate_family_valid = false;
  g_ui_candidate_family_width = 0;
  g_ui_candidate_family_height = 0;
  g_ui_candidate_family_format = 0;
  g_ui_candidate_family_last_seen_ms = 0;
}

bool MatchesUiFamilyLocked(const UiRuntimeCandidate& candidate) {
  return g_ui_candidate_family_valid &&
      candidate.width == g_ui_candidate_family_width &&
      candidate.height == g_ui_candidate_family_height &&
      candidate.format == g_ui_candidate_family_format;
}

void EstablishUiFamilyLocked(const UiRuntimeCandidate& candidate, uint64_t now_ms) {
  g_ui_candidate_family_valid = true;
  g_ui_candidate_family_width = candidate.width;
  g_ui_candidate_family_height = candidate.height;
  g_ui_candidate_family_format = candidate.format;
  g_ui_candidate_family_last_seen_ms = now_ms;
}

void ResetUiCandidateSelection() {
  AcquireSRWLockExclusive(&g_ui_candidate_lock);
  ResetUiSelectionLocked();
  ResetUiFamilyLocked();
  g_ui_candidate_ambiguous = false;
  ReleaseSRWLockExclusive(&g_ui_candidate_lock);
  mfgunlock::framecount::g_ui_candidate_format.store(0, std::memory_order_release);
  mfgunlock::framecount::g_ui_candidate_options_synced.store(
      false, std::memory_order_release);
}

void ClearUiRuntimeCandidates() {
  AcquireSRWLockExclusive(&g_ui_candidate_lock);
  for (auto& candidate : g_ui_candidates) candidate = {};
  g_known_swapchain_buffers = {};
  g_ui_candidate_overflow = false;
  g_ui_candidate_ambiguous = false;
  ResetUiSelectionLocked();
  ResetUiFamilyLocked();
  ReleaseSRWLockExclusive(&g_ui_candidate_lock);
}

void MarkSwapchainBuffers(reshade::api::swapchain* swapchain, bool present) {
  if (swapchain == nullptr) return;
  const uint32_t count = swapchain->get_back_buffer_count();
  AcquireSRWLockExclusive(&g_ui_candidate_lock);
  for (uint32_t index = 0; index < count; ++index) {
    const uint64_t resource = swapchain->get_back_buffer(index).handle;
    if (resource == 0) continue;
    if (present) {
      auto known = std::find(g_known_swapchain_buffers.begin(),
                             g_known_swapchain_buffers.end(), resource);
      if (known == g_known_swapchain_buffers.end()) {
        auto empty = std::find(g_known_swapchain_buffers.begin(),
                               g_known_swapchain_buffers.end(), uint64_t{0});
        if (empty != g_known_swapchain_buffers.end()) *empty = resource;
      }
      for (auto& candidate : g_ui_candidates) {
        if (candidate.used && candidate.resource.handle == resource)
          candidate.swapchain = true;
      }
    } else {
      for (auto& known : g_known_swapchain_buffers) {
        if (known == resource) known = 0;
      }
    }
  }
  ReleaseSRWLockExclusive(&g_ui_candidate_lock);
}

void RecordUiCandidateView(reshade::api::command_list* cmd_list,
                           reshade::api::resource_view view,
                           bool clear, const float* clear_color = nullptr) {
  if (!UiCandidateObservationEnabled() ||
      !g_ui_candidate_d3d12.load(std::memory_order_relaxed) ||
      cmd_list == nullptr || view.handle == 0) {
    return;
  }
  auto* device = cmd_list->get_device();
  if (device == nullptr || device->get_api() != reshade::api::device_api::d3d12) return;
  const auto resource = device->get_resource_from_view(view);
  if (resource.handle == 0) return;
  const auto desc = device->get_resource_desc(resource);
  const auto view_desc = device->get_resource_view_desc(view);
  if (desc.type != reshade::api::resource_type::texture_2d ||
      !IsUiCandidateFormat(desc.texture.format, view_desc.format)) {
    return;
  }

  AcquireSRWLockExclusive(&g_ui_candidate_lock);
  UiRuntimeCandidate* candidate = FindUiRuntimeCandidateLocked(resource.handle);
  if (candidate == nullptr) {
    g_ui_candidate_overflow = true;
    ReleaseSRWLockExclusive(&g_ui_candidate_lock);
    return;
  }
  if (!candidate->used) {
    candidate->used = true;
    candidate->resource = resource;
    candidate->width = desc.texture.width;
    candidate->height = desc.texture.height;
    candidate->format = static_cast<uint32_t>(desc.texture.format);
    candidate->view_format = static_cast<uint32_t>(view_desc.format);
    candidate->swapchain = IsKnownSwapchainBufferLocked(resource.handle);
  }

  candidate->state_known = true;
  candidate->state = static_cast<uint32_t>(reshade::api::resource_usage::render_target);
  if (clear) {
    if (candidate->hudless_clear_serial == candidate->clear_serial)
      candidate->last_late_writes = candidate->current_late_writes;
    else
      candidate->last_late_writes = 0;
    ++candidate->clear_serial;
    candidate->rtv_binds_since_clear = 0;
    candidate->current_late_writes = 0;
    candidate->last_clear_transparent = IsTransparentClear(clear_color);
    candidate->last_clear_ms = GetTickCount64();
    if (candidate->last_clear_transparent) ++candidate->transparent_clears;
  } else {
    ++candidate->rtv_binds;
    ++candidate->rtv_binds_since_clear;
    if (candidate->hudless_clear_serial != 0 &&
        candidate->hudless_clear_serial == candidate->clear_serial) {
      ++candidate->current_late_writes;
    }
  }
  ReleaseSRWLockExclusive(&g_ui_candidate_lock);
}

void OnBindUiCandidateRenderTargets(reshade::api::command_list* cmd_list,
                                    uint32_t count,
                                    const reshade::api::resource_view* rtvs,
                                    reshade::api::resource_view) {
  if (rtvs == nullptr) return;
  for (uint32_t index = 0; index < count; ++index)
    RecordUiCandidateView(cmd_list, rtvs[index], false);
}

bool OnClearUiCandidateRenderTarget(reshade::api::command_list* cmd_list,
                                    reshade::api::resource_view rtv,
                                    const float color[4], uint32_t,
                                    const reshade::api::rect*) {
  RecordUiCandidateView(cmd_list, rtv, true, color);
  return false;
}

void OnUiCandidateBarrier(reshade::api::command_list*, uint32_t count,
                          const reshade::api::resource* resources,
                          const reshade::api::resource_usage*,
                          const reshade::api::resource_usage* new_states) {
  if (!UiCandidateObservationEnabled() || resources == nullptr || new_states == nullptr)
    return;
  AcquireSRWLockExclusive(&g_ui_candidate_lock);
  for (uint32_t index = 0; index < count; ++index) {
    if (resources[index].handle == 0) continue;
    UiRuntimeCandidate* candidate = FindUiRuntimeCandidateLocked(resources[index].handle);
    if (candidate == nullptr || !candidate->used) continue;
    candidate->state_known = ToD3D12ResourceState(new_states[index], candidate->state);
  }
  ReleaseSRWLockExclusive(&g_ui_candidate_lock);
}

void OnDestroyUiCandidateResource(reshade::api::device* device,
                                  reshade::api::resource resource) {
  if (device == nullptr || device->get_api() != reshade::api::device_api::d3d12 ||
      resource.handle == 0) {
    return;
  }
  bool selected_destroyed = false;
  AcquireSRWLockExclusive(&g_ui_candidate_lock);
  for (auto& candidate : g_ui_candidates) {
    if (!candidate.used || candidate.resource.handle != resource.handle) continue;
    candidate = {};
    if (g_ui_selected_resource == resource.handle) {
      ResetUiSelectionLocked();
      selected_destroyed = true;
    }
    break;
  }
  for (auto& known : g_known_swapchain_buffers) {
    if (known == resource.handle) known = 0;
  }
  ReleaseSRWLockExclusive(&g_ui_candidate_lock);
  if (selected_destroyed) {
    mfgunlock::framecount::g_ui_candidate_active.store(
        false, std::memory_order_relaxed);
    mfgunlock::framecount::g_ui_pair_eligible.store(
        false, std::memory_order_relaxed);
    mfgunlock::framecount::g_ui_native_hudless_fallback.store(
        true, std::memory_order_relaxed);
  }
}

mfgunlock::uicandidate::Evidence BuildUiCandidateEvidence(
    const UiRuntimeCandidate& candidate, uint64_t now_ms) {
  mfgunlock::uicandidate::Evidence evidence{};
  const uint32_t output_width = g_primary_output_width.load(std::memory_order_relaxed);
  const uint32_t output_height = g_primary_output_height.load(std::memory_order_relaxed);
  evidence.output_match = output_width != 0 && output_height != 0 &&
      candidate.width == output_width && candidate.height == output_height;
  evidence.swapchain = candidate.swapchain;
  evidence.already_tagged = candidate.already_tagged;
  evidence.rgba8_with_alpha = candidate.format == candidate.view_format &&
      (candidate.format == static_cast<uint32_t>(reshade::api::format::r8g8b8a8_unorm) ||
       candidate.format == static_cast<uint32_t>(reshade::api::format::r8g8b8a8_unorm_srgb));
  evidence.fresh_transparent_clear = candidate.last_clear_transparent &&
      candidate.clear_serial != 0 &&
      candidate.clear_serial != candidate.evaluated_clear_serial;
  evidence.rtv_binds_after_clear = candidate.rtv_binds_since_clear;
  evidence.state_known = candidate.state_known;
  evidence.recent = candidate.last_clear_ms != 0 && now_ms >= candidate.last_clear_ms &&
      now_ms - candidate.last_clear_ms <= kUiCandidateMaxAgeMs;
  evidence.late_writes = candidate.last_late_writes;
  return evidence;
}

void ObserveNativeStreamlineTags(const sl::ResourceTag* tags, uint32_t count) {
  if (!UiCandidateObservationEnabled() || tags == nullptr || count == 0) return;
  AcquireSRWLockExclusive(&g_ui_candidate_lock);
  for (uint32_t index = 0; index < count; ++index) {
    if (tags[index].resource == nullptr) continue;
    const auto* bytes = reinterpret_cast<const unsigned char*>(tags[index].resource);
    uint64_t matched = 0;
    uint32_t matches = 0;
    const size_t scan_bytes = (std::min)(sizeof(sl::Resource), size_t{64});
    for (size_t offset = 0; offset + sizeof(uint64_t) <= scan_bytes;
         offset += sizeof(uint64_t)) {
      uint64_t value = 0;
      std::memcpy(&value, bytes + offset, sizeof(value));
      if (value == 0 || value == matched) continue;
      for (const auto& candidate : g_ui_candidates) {
        if (!candidate.used || candidate.resource.handle != value) continue;
        matched = value;
        ++matches;
        break;
      }
    }
    if (matches != 1) continue;
    for (auto& candidate : g_ui_candidates) {
      if (candidate.used && candidate.resource.handle == matched) {
        candidate.already_tagged = true;
        break;
      }
    }
  }
  ReleaseSRWLockExclusive(&g_ui_candidate_lock);
}

bool ObserveUiCandidate(mfgunlock::framecount::UiCandidateSnapshot* snapshot) {
  if (snapshot == nullptr || !UiCandidateObservationEnabled() ||
      !g_ui_candidate_d3d12.load(std::memory_order_relaxed)) {
    return false;
  }

  UiRuntimeCandidate* matches[kMaxUiRuntimeCandidates]{};
  size_t match_count = 0;
  const uint64_t now_ms = GetTickCount64();
  AcquireSRWLockExclusive(&g_ui_candidate_lock);
  for (auto& candidate : g_ui_candidates) {
    if (!candidate.used) continue;
    const auto evidence = BuildUiCandidateEvidence(candidate, now_ms);
    if (mfgunlock::uicandidate::Assess(evidence) == mfgunlock::uicandidate::kAccept)
      matches[match_count++] = &candidate;
  }
  for (auto& candidate : g_ui_candidates) {
    if (candidate.used && candidate.clear_serial != candidate.evaluated_clear_serial)
      candidate.evaluated_clear_serial = candidate.clear_serial;
  }

  g_ui_candidate_ambiguous = match_count > 1;
  if (match_count != 1) {
    ResetUiSelectionLocked();
    ReleaseSRWLockExclusive(&g_ui_candidate_lock);
    return false;
  }

  UiRuntimeCandidate& candidate = *matches[0];
  const bool same_resource = g_ui_selected_resource == candidate.resource.handle;
  if (same_resource) {
    if (g_ui_selected_clear_serial != candidate.clear_serial)
      ++g_ui_selected_stable_frames;
  } else {
    const uint64_t family_age_ms = g_ui_candidate_family_valid &&
        now_ms >= g_ui_candidate_family_last_seen_ms
        ? now_ms - g_ui_candidate_family_last_seen_ms
        : mfgunlock::uicandidate::kHandoverMaxGapMs + 1;
    const bool warm_handover = mfgunlock::uicandidate::CanWarmHandover(
        g_ui_candidate_family_valid, MatchesUiFamilyLocked(candidate), family_age_ms);
    g_ui_selected_stable_frames = warm_handover
        ? mfgunlock::uicandidate::HandoverSeedStableFrames()
        : 1;
    if (warm_handover) ++g_ui_candidate_family_handoffs;
  }
  g_ui_selected_resource = candidate.resource.handle;
  g_ui_selected_clear_serial = candidate.clear_serial;
  candidate.hudless_clear_serial = candidate.clear_serial;
  candidate.current_late_writes = 0;
  if (mfgunlock::uicandidate::IsStable(g_ui_selected_stable_frames)) {
    if (!MatchesUiFamilyLocked(candidate))
      EstablishUiFamilyLocked(candidate, now_ms);
    else
      g_ui_candidate_family_last_seen_ms = now_ms;
  }

  snapshot->native_resource = candidate.resource.handle;
  snapshot->width = candidate.width;
  snapshot->height = candidate.height;
  snapshot->format = candidate.format;
  snapshot->state = candidate.state;
  snapshot->state_known = candidate.state_known;
  snapshot->stable_frames = g_ui_selected_stable_frames;
  snapshot->rtv_binds_after_clear = candidate.rtv_binds_since_clear;
  snapshot->reject_reasons = mfgunlock::uicandidate::kAccept;
  snapshot->late_writes = candidate.last_late_writes;
  snapshot->clear_serial = candidate.clear_serial;
  snapshot->recent = true;
  ReleaseSRWLockExclusive(&g_ui_candidate_lock);
  return true;
}

bool AcquireUiCandidate(const mfgunlock::framecount::UiCandidateSnapshot* snapshot) {
  if (snapshot == nullptr || snapshot->native_resource == 0) return false;
  IUnknown* reference = nullptr;
  AcquireSRWLockShared(&g_ui_candidate_lock);
  for (const auto& candidate : g_ui_candidates) {
    if (!candidate.used || candidate.resource.handle != snapshot->native_resource ||
        candidate.clear_serial != snapshot->clear_serial ||
        candidate.state != snapshot->state || candidate.format != snapshot->format) {
      continue;
    }
    auto evidence = BuildUiCandidateEvidence(candidate, GetTickCount64());
    evidence.fresh_transparent_clear = candidate.last_clear_transparent &&
        candidate.clear_serial == snapshot->clear_serial;
    evidence.late_writes = candidate.current_late_writes;
    if (mfgunlock::uicandidate::Assess(evidence) != mfgunlock::uicandidate::kAccept)
      continue;
    reference = reinterpret_cast<IUnknown*>(
        static_cast<uintptr_t>(candidate.resource.handle));
    reference->AddRef();
    break;
  }
  ReleaseSRWLockShared(&g_ui_candidate_lock);
  return reference != nullptr;
}

void ReleaseUiCandidate(const mfgunlock::framecount::UiCandidateSnapshot* snapshot,
                        bool injected, sl::Result result) {
  if (snapshot == nullptr || snapshot->native_resource == 0) return;
  if (injected && result == sl::Result::eOk) {
    AcquireSRWLockExclusive(&g_ui_candidate_lock);
    for (auto& candidate : g_ui_candidates) {
      if (!candidate.used || candidate.resource.handle != snapshot->native_resource ||
          candidate.clear_serial != snapshot->clear_serial) {
        continue;
      }
      candidate.injected_clear_serial = snapshot->clear_serial;
      candidate.current_late_writes = 0;
      break;
    }
    ReleaseSRWLockExclusive(&g_ui_candidate_lock);
  }
  reinterpret_cast<IUnknown*>(static_cast<uintptr_t>(snapshot->native_resource))->Release();
}

UiCandidateDebugSnapshot ReadUiCandidateDebugSnapshot() {
  UiCandidateDebugSnapshot snapshot{};
  if (!TryAcquireSRWLockShared(&g_ui_candidate_lock)) return snapshot;
  snapshot.ambiguous = g_ui_candidate_ambiguous;
  snapshot.overflow = g_ui_candidate_overflow;
  snapshot.family_valid = g_ui_candidate_family_valid;
  snapshot.family_width = g_ui_candidate_family_width;
  snapshot.family_height = g_ui_candidate_family_height;
  snapshot.family_format = g_ui_candidate_family_format;
  snapshot.family_handoffs = g_ui_candidate_family_handoffs;
  const uint64_t family_now_ms = GetTickCount64();
  snapshot.family_age_ms = g_ui_candidate_family_valid &&
      family_now_ms >= g_ui_candidate_family_last_seen_ms
      ? family_now_ms - g_ui_candidate_family_last_seen_ms
      : 0;
  snapshot.resource = g_ui_selected_resource;
  snapshot.clear_serial = g_ui_selected_clear_serial;
  snapshot.stable_frames = g_ui_selected_stable_frames;
  snapshot.stable = mfgunlock::uicandidate::IsStable(snapshot.stable_frames);
  for (const auto& candidate : g_ui_candidates) {
    if (!candidate.used || candidate.resource.handle != g_ui_selected_resource) continue;
    snapshot.present = true;
    snapshot.width = candidate.width;
    snapshot.height = candidate.height;
    snapshot.format = candidate.format;
    snapshot.state = candidate.state;
    snapshot.state_known = candidate.state_known;
    snapshot.swapchain = candidate.swapchain;
    snapshot.already_tagged = candidate.already_tagged;
    snapshot.transparent_clear = candidate.last_clear_transparent;
    snapshot.rtv_binds_after_clear = candidate.rtv_binds_since_clear;
    snapshot.late_writes = candidate.current_late_writes != 0
        ? candidate.current_late_writes : candidate.last_late_writes;
    const uint64_t now_ms = GetTickCount64();
    snapshot.age_ms = now_ms >= candidate.last_clear_ms
        ? now_ms - candidate.last_clear_ms : 0;
    auto evidence = BuildUiCandidateEvidence(candidate, now_ms);
    evidence.fresh_transparent_clear = candidate.last_clear_transparent &&
        candidate.clear_serial == g_ui_selected_clear_serial;
    evidence.late_writes = snapshot.late_writes;
    snapshot.reject_reasons = mfgunlock::uicandidate::Assess(evidence);
    snapshot.safe = snapshot.stable && snapshot.reject_reasons == mfgunlock::uicandidate::kAccept;
    break;
  }
  ReleaseSRWLockShared(&g_ui_candidate_lock);
  return snapshot;
}

const char* UiCandidateFormatName(uint32_t format) {
  if (format == static_cast<uint32_t>(reshade::api::format::r8g8b8a8_unorm))
    return "RGBA8 UNORM";
  if (format == static_cast<uint32_t>(reshade::api::format::r8g8b8a8_unorm_srgb))
    return "RGBA8 sRGB";
  return "unknown";
}

// Graphics initialization is also a finite fallback for already loaded modules.
void OnInitDevice(reshade::api::device* device) {
  const bool d3d12 = device != nullptr &&
      device->get_api() == reshade::api::device_api::d3d12;
  g_ui_candidate_d3d12.store(d3d12, std::memory_order_relaxed);
  if (d3d12) mfgunlock::framecount::NotifyDynamicD3D12(true);
  RefreshRuntime();
}

void OnInitCommandQueue(reshade::api::command_queue* /*queue*/) {
  RefreshRuntime();
}

bool IsHdrColorSpace(reshade::api::color_space color_space) {
  return color_space == reshade::api::color_space::scrgb ||
         color_space == reshade::api::color_space::hdr10_pq ||
         color_space == reshade::api::color_space::hdr10_hlg;
}

void ObserveUiOutput(reshade::api::swapchain* swapchain) {
  if (swapchain == nullptr ||
      swapchain != g_primary_swapchain.load(std::memory_order_acquire)) {
    return;
  }
  const auto color_space = swapchain->get_color_space();
  if (color_space == reshade::api::color_space::unknown) return;
  mfgunlock::framecount::NotifyHdrState(IsHdrColorSpace(color_space));
}

void OnInitSwapchain(reshade::api::swapchain* swapchain, bool /*resize*/) {
  if (swapchain == nullptr) return;
  auto* device = swapchain->get_device();
  if (device == nullptr || swapchain->get_back_buffer_count() == 0) return;

  MarkSwapchainBuffers(swapchain, true);
  const auto back_buffer = swapchain->get_back_buffer(0);
  const auto desc = device->get_resource_desc(back_buffer);
  const uint64_t area = static_cast<uint64_t>(desc.texture.width) * desc.texture.height;
  const uint64_t selected_area = g_primary_swapchain_area.load(std::memory_order_relaxed);
  reshade::api::swapchain* selected = g_primary_swapchain.load(std::memory_order_acquire);
  if (selected != swapchain && selected != nullptr && area <= selected_area) return;

  const bool changed = selected != swapchain;
  g_primary_swapchain_area.store(area, std::memory_order_relaxed);
  g_primary_output_width.store(desc.texture.width, std::memory_order_relaxed);
  g_primary_output_height.store(desc.texture.height, std::memory_order_relaxed);
  g_primary_swapchain.store(swapchain, std::memory_order_release);
  if (changed) ResetUiCandidateSelection();

  const auto color_space = swapchain->get_color_space();
  if (color_space == reshade::api::color_space::unknown) {
    mfgunlock::framecount::NotifyOutputUnknown();
    return;
  }
  mfgunlock::framecount::NotifyHdrState(IsHdrColorSpace(color_space));
}

void OnDestroySwapchain(reshade::api::swapchain* swapchain, bool /*resize*/) {
  if (swapchain == nullptr) return;
  MarkSwapchainBuffers(swapchain, false);
  reshade::api::swapchain* expected = swapchain;
  if (!g_primary_swapchain.compare_exchange_strong(
          expected, nullptr, std::memory_order_acq_rel)) {
    return;
  }
  g_primary_swapchain_area.store(0, std::memory_order_relaxed);
  g_primary_output_width.store(0, std::memory_order_relaxed);
  g_primary_output_height.store(0, std::memory_order_relaxed);
  ResetUiCandidateSelection();
  mfgunlock::framecount::NotifyOutputUnknown();
}

void OnPresentUiOutput(reshade::api::command_queue* /*queue*/,
                       reshade::api::swapchain* swapchain,
                       const reshade::api::rect* /*source_rect*/,
                       const reshade::api::rect* /*dest_rect*/,
                       uint32_t /*dirty_rect_count*/,
                       const reshade::api::rect* /*dirty_rects*/) {
  if (swapchain == nullptr) return;
  if (g_primary_swapchain.load(std::memory_order_acquire) == nullptr)
    OnInitSwapchain(swapchain, false);
  if (g_primary_swapchain.load(std::memory_order_acquire) != swapchain) return;

  auto* device = swapchain->get_device();
  if (device != nullptr && swapchain->get_back_buffer_count() != 0) {
    const auto desc = device->get_resource_desc(swapchain->get_back_buffer(0));
    const uint32_t old_width = g_primary_output_width.exchange(
        desc.texture.width, std::memory_order_relaxed);
    const uint32_t old_height = g_primary_output_height.exchange(
        desc.texture.height, std::memory_order_relaxed);
    if ((old_width != 0 && old_width != desc.texture.width) ||
        (old_height != 0 && old_height != desc.texture.height)) {
      ResetUiCandidateSelection();
    }
  }
  ObserveUiOutput(swapchain);
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

void SetWarpMode(int value) {
  const int mode = static_cast<int>(mfgunlock::validatedwarp::ConfiguredMode(
      g_show_debug_options.load(std::memory_order_relaxed), value));
  g_validated_warp_mode.store(mode, std::memory_order_relaxed);
  reshade::set_config_value(nullptr, kConfigSection, "WarpDiagnosticMode", mode);
}

void DrawWarpStatus(bool requested, bool debug_options) {
  if (!TryAcquireSRWLockShared(&g_provider_maintenance_lock)) {
    ImGui::TextDisabled("Warp provider update in progress.");
    return;
  }
  struct Unlock {
    ~Unlock() { ReleaseSRWLockShared(&g_provider_maintenance_lock); }
  } unlock;
  const auto mode = mfgunlock::validatedwarp::ConfiguredMode(
      debug_options, g_validated_warp_mode.load(std::memory_order_relaxed));
  if (!g_validated_warp_modules.empty()) {
    const bool matches = std::all_of(g_validated_warp_modules.begin(), g_validated_warp_modules.end(),
                                    [mode](const auto& patch) { return patch.mode == mode; });
    if (!requested) {
      ImGui::TextWrapped("Warp remains applied. Restart the game to disable it.");
    } else if (!matches) {
      ImGui::TextWrapped("Warp mode change pending. Restart the game to apply the selected mode.");
    } else if (mode == mfgunlock::validatedwarp::Mode::kValidatedWarp) {
      ImGui::TextUnformatted("Validated Warp Blend: applied.");
    } else {
      ImGui::Text("Diagnostic Warp path applied: %s.", mfgunlock::validatedwarp::ModeName(mode));
    }
    if (debug_options) {
      for (const auto& patch : g_validated_warp_modules)
        ImGui::TextWrapped("Applied: %s; %s.", mfgunlock::validatedwarp::ModeName(patch.mode),
                           patch.detail.c_str());
    }
  } else if (requested) {
    if (!g_validated_warp_detail.empty()) {
      ImGui::TextWrapped("Warp was not applied. See the ReShade log for the reason.");
      if (debug_options) ImGui::TextWrapped("%s", g_validated_warp_detail.c_str());
    } else {
      ImGui::TextDisabled("Warp: waiting for provider load. Restart after changing settings.");
    }
  }
}

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
    if (enabled) InstallPresetForLoadedProviders();
  }
  bool debug_options = g_show_debug_options.load(std::memory_order_relaxed);
  if (ImGui::Checkbox("Show advanced/debug options", &debug_options)) {
    g_show_debug_options.store(debug_options, std::memory_order_relaxed);
    reshade::set_config_value(nullptr, kConfigSection, "ShowDebugOptions", debug_options ? 1 : 0);
    if (!debug_options) {
      SetWarpMode(static_cast<int>(mfgunlock::validatedwarp::Mode::kValidatedWarp));
      mfgunlock::framecount::g_ui_candidate_injection_enabled.store(
          false, std::memory_order_relaxed);
      mfgunlock::framecount::NotifyUiCandidateInjectionChanged();
      reshade::set_config_value(nullptr, kConfigSection, "UICandidateInjection", 0);
    }
  }

  static const char* kRuntimeModes[] = {
      "Game default", "Prefer local runtime", "Force NVIDIA OTA runtime"};
  int runtime_mode = static_cast<int>(
      mfgunlock::framecount::g_runtime_selection_mode.load(std::memory_order_relaxed));
  bool runtime_changed = false;
  if (debug_options) {
    runtime_changed = ImGui::Combo("Streamline runtime", &runtime_mode, kRuntimeModes, ARRAYSIZE(kRuntimeModes));
  } else {
    bool local = runtime_mode == 1;
    if (ImGui::Checkbox("Prefer local runtime", &local)) {
      runtime_mode = local ? 1 : 0;
      runtime_changed = true;
    }
    if (runtime_mode == 2) ImGui::TextDisabled("NVIDIA OTA runtime forced (advanced setting).");
  }
  if (runtime_changed) {
    mfgunlock::framecount::g_runtime_selection_mode.store(
        static_cast<unsigned int>(runtime_mode), std::memory_order_relaxed);
    reshade::set_config_value(nullptr, kConfigSection, "RuntimeSelectionMode", runtime_mode);
  }
  ImGui::TextDisabled("Restart required after changing the Streamline runtime policy.");

  if (debug_options) {
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
  } else if (debug_options && mfgunlock::framecount::g_feature_function_hooked.load(std::memory_order_acquire)) {
    ImGui::TextDisabled("slGetFeatureFunction hook installed; DLSS-G not exercised yet.");
  } else if (debug_options && mfgunlock::framecount::g_init_hooked.load(std::memory_order_acquire)) {
    ImGui::TextDisabled("Streamline startup hook installed; no slGetFeatureFunction export.");
  } else if (debug_options && GetModuleHandleW(L"sl.interposer.dll")) {
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
  static const char* kFgPresets[] = {"Application / driver default", "Preset A", "Preset B"};
  int preset = static_cast<int>(mfgunlock::fgpreset::g_requested.load(std::memory_order_relaxed));
  if (ImGui::Combo("Frame Generation model", &preset, kFgPresets, ARRAYSIZE(kFgPresets))) {
    mfgunlock::fgpreset::g_requested.store(mfgunlock::fgpreset::Parse(preset), std::memory_order_relaxed);
    reshade::set_config_value(nullptr, kConfigSection, "FrameGenerationPreset", preset);
    InstallPresetForLoadedProviders();
  }
  ImGui::TextDisabled("Model changes may require restarting the game; FG off/on may reuse cached settings.");
  if (preset != 0) {
    if (!enabled) {
      ImGui::TextDisabled("Preset override is disabled with MFG Unlock.");
    } else if (!mfgunlock::fgpreset::HasHook()) {
      ImGui::TextDisabled("Preset override unavailable: requires the supported DLSS-G 310.9.1 build.");
    } else {
      ImGui::Text("Requested preset: %s. Applies on the next model-setup read.",
                  mfgunlock::fgpreset::Name(mfgunlock::fgpreset::Parse(preset)));
    }
  }
  const int supplied = mfgunlock::fgpreset::g_last_supplied.load(std::memory_order_relaxed);
  if (supplied >= 0) {
    ImGui::Text("Last model-setup override supplied: %s.",
                mfgunlock::fgpreset::Name(mfgunlock::fgpreset::Parse(supplied)));
    if (supplied != preset || !enabled)
      ImGui::TextDisabled("The previous model may remain cached. Restart to apply the new selection.");
  }
  const int applied = mfgunlock::fgpreset::g_last_applied.load(std::memory_order_relaxed);
  if (applied >= 0) {
    ImGui::Text("Provider-applied preset: %s (runtime-observed).",
                mfgunlock::fgpreset::Name(mfgunlock::fgpreset::Parse(applied)));
    if (applied != preset)
      ImGui::TextDisabled("Provider state differs from the current selector; restart the game.");
  } else if (supplied >= 0 && mfgunlock::fgpreset::HasObserver()) {
    ImGui::TextDisabled("Provider-applied preset: waiting for NVIDIA override-state reporting.");
  }

  ImGui::Separator();
  bool ui_composition = mfgunlock::framecount::g_ui_composition_enabled.load(
      std::memory_order_relaxed);
  if (ImGui::Checkbox("Automatic UI Composition", &ui_composition)) {
    mfgunlock::framecount::g_ui_composition_enabled.store(
        ui_composition, std::memory_order_relaxed);
    mfgunlock::framecount::NotifyUiCompositionChanged();
    reshade::set_config_value(nullptr, kConfigSection, "UIComposition",
                              ui_composition ? 1 : 0);
    if (!ui_composition) {
      mfgunlock::framecount::g_ui_candidate_injection_enabled.store(
          false, std::memory_order_relaxed);
      mfgunlock::framecount::NotifyUiCandidateInjectionChanged();
      reshade::set_config_value(nullptr, kConfigSection, "UICandidateInjection", 0);
    }
  }
  ImGui::TextDisabled(
      "Keeps HUD/UI separate from generated scene frames when Streamline supplies a valid pair.\n"
      "Independent of the Frame Generation model; toggle frame generation off/on after changing it.");
  const auto ui_debug = mfgunlock::framecount::ReadUiDebugSnapshot();
  if (!ui_composition) {
    ImGui::TextDisabled("UI Composition: disabled; native final-color path.");
  } else if (!mfgunlock::framecount::g_hdr_state_seen.load(std::memory_order_acquire)) {
    ImGui::TextDisabled("UI Composition: waiting for output color-space detection.");
  } else if (mfgunlock::framecount::g_hdr_active.load(std::memory_order_relaxed)) {
    ImGui::TextDisabled("UI Composition: HDR output detected; guarded final-color fallback.");
  } else if (mfgunlock::framecount::g_ui_composition_fell_back.load(
                 std::memory_order_relaxed)) {
    ImGui::Text("UI Composition: runtime rejected the request (sl::Result 0x%x); native path restored.",
                mfgunlock::framecount::g_ui_composition_result.load(
                    std::memory_order_relaxed));
  } else if (mfgunlock::framecount::g_ui_composition_applied.load(
                 std::memory_order_relaxed)) {
    if (mfgunlock::framecount::g_ui_pair_eligible.load(std::memory_order_relaxed)) {
      ImGui::TextUnformatted("UI Composition: active; validated HUD-less + UI inputs observed.");
    } else if (mfgunlock::framecount::g_ui_native_hudless_fallback.load(
                   std::memory_order_relaxed)) {
      ImGui::TextDisabled(
          "UI Composition: HUD-less present, UI partner missing; preserving native HUD-less path.");
    } else if (ui_debug.suppression_active) {
      ImGui::TextDisabled("UI Composition: path active; unsafe optional UI inputs are being guarded.");
    } else {
      ImGui::TextDisabled("UI Composition: path active; waiting for a complete HUD-less + UI pair.");
    }
  } else {
    ImGui::TextDisabled("UI Composition: ready; toggle frame generation off/on to submit it.");
  }
  if (debug_options) {
    ImGui::TextDisabled(
        "UI inputs: HUD-less=%s (%llu), UI Color+Alpha=%s (%llu), UI Alpha=%s (%llu).",
        ui_debug.hudless_seen ? "yes" : "no",
        mfgunlock::framecount::g_ui_hudless_tags_seen.load(std::memory_order_relaxed),
        ui_debug.ui_color_alpha_seen ? "yes" : "no",
        mfgunlock::framecount::g_ui_color_alpha_tags_seen.load(std::memory_order_relaxed),
        ui_debug.ui_alpha_seen ? "yes" : "no",
        mfgunlock::framecount::g_ui_alpha_tags_seen.load(std::memory_order_relaxed));
    ImGui::TextDisabled(
        "UI state: viewports=%u, options=%s, pair(now)=%s, pair(history)=%s, invalid=%s.",
        ui_debug.viewport_count, ui_debug.options_seen ? "yes" : "no",
        mfgunlock::framecount::g_ui_pair_eligible.load(std::memory_order_relaxed)
            ? "yes" : "no",
        ui_debug.pair_eligible ? "yes" : "no", ui_debug.invalid ? "yes" : "no");
    ImGui::TextDisabled(
        "UI guard: suppression=%s, native fallback=%s.",
        ui_debug.suppression_active ? "yes" : "no",
        mfgunlock::framecount::g_ui_native_hudless_fallback.load(std::memory_order_relaxed)
            ? "yes" : "no");
    ImGui::TextDisabled(
        "UI request: accepted=%s, runtime fallback=%s, output=%s/%s.",
        mfgunlock::framecount::g_ui_composition_applied.load(std::memory_order_relaxed)
            ? "yes" : "no",
        mfgunlock::framecount::g_ui_composition_fell_back.load(std::memory_order_relaxed)
            ? "yes" : "no",
        mfgunlock::framecount::g_hdr_state_seen.load(std::memory_order_acquire)
            ? "known" : "unknown",
        mfgunlock::framecount::g_hdr_active.load(std::memory_order_relaxed) ? "HDR" : "SDR");
    ImGui::TextDisabled(
        "UI options: source v%u, result=0x%x, suppressed tags=%llu; issue mask=0x%x; resets=%llu/%llu.",
        mfgunlock::framecount::g_ui_composition_source_version.load(std::memory_order_relaxed),
        mfgunlock::framecount::g_ui_composition_result.load(std::memory_order_relaxed),
        mfgunlock::framecount::g_ui_optional_tags_suppressed.load(std::memory_order_relaxed),
        mfgunlock::framecount::g_ui_issue_mask.load(std::memory_order_relaxed),
        mfgunlock::framecount::g_ui_resets_injected.load(std::memory_order_relaxed),
        mfgunlock::framecount::g_ui_resets_requested.load(std::memory_order_relaxed));

    ImGui::Separator();
    bool inject_ui_candidate =
        mfgunlock::framecount::g_ui_candidate_injection_enabled.load(
            std::memory_order_relaxed);
    ImGui::BeginDisabled(!ui_composition);
    if (ImGui::Checkbox("Inject detected UI Color+Alpha (experimental)",
                        &inject_ui_candidate)) {
      mfgunlock::framecount::g_ui_candidate_injection_enabled.store(
          inject_ui_candidate, std::memory_order_relaxed);
      mfgunlock::framecount::NotifyUiCandidateInjectionChanged();
      reshade::set_config_value(nullptr, kConfigSection, "UICandidateInjection",
                                inject_ui_candidate ? 1 : 0);
    }
    ImGui::EndDisabled();
    ImGui::TextDisabled(
        "Opt-in diagnostic path. Metadata can identify a strong UI target but cannot prove\n"
        "premultiplied UI pixels. The tag uses OnlyValidNow and fails back to native HUD-less.");

    const auto candidate = ReadUiCandidateDebugSnapshot();
    ImGui::TextDisabled(
        "Detected UI candidate: present=%s, stable=%s, safe=%s, ambiguous=%s, overflow=%s.",
        candidate.present ? "yes" : "no", candidate.stable ? "yes" : "no",
        candidate.safe ? "yes" : "no", candidate.ambiguous ? "yes" : "no",
        candidate.overflow ? "yes" : "no");
    ImGui::TextDisabled(
        "Candidate family: valid=%s, %ux%u, format=%u, handovers=%llu, age=%llums.",
        candidate.family_valid ? "yes" : "no", candidate.family_width,
        candidate.family_height, candidate.family_format,
        static_cast<unsigned long long>(candidate.family_handoffs),
        static_cast<unsigned long long>(candidate.family_age_ms));
    if (candidate.present) {
      ImGui::TextDisabled(
          "Candidate: resource=0x%llx, %ux%u, format=%u (%s), alpha=8 bits.",
          static_cast<unsigned long long>(candidate.resource), candidate.width,
          candidate.height, candidate.format, UiCandidateFormatName(candidate.format));
      ImGui::TextDisabled(
          "Candidate evidence: transparent clear=%s, RTV binds after clear=%u, stable frames=%u, age=%llums.",
          candidate.transparent_clear ? "yes" : "no", candidate.rtv_binds_after_clear,
          candidate.stable_frames, static_cast<unsigned long long>(candidate.age_ms));
      ImGui::TextDisabled(
          "Candidate state: D3D12 known=%s state=0x%x, swapchain=%s, native-tagged=%s, "
          "late writes=%u, reject mask=0x%x.",
          candidate.state_known ? "yes" : "no", candidate.state,
          candidate.swapchain ? "yes" : "no", candidate.already_tagged ? "yes" : "no",
          candidate.late_writes, candidate.reject_reasons);
    }
    ImGui::TextDisabled(
        "UI candidate injection: enabled=%s, active=%s, format=%u, options synced=%s, hard declined=%s.",
        inject_ui_candidate ? "yes" : "no",
        mfgunlock::framecount::g_ui_candidate_active.load(std::memory_order_relaxed)
            ? "yes" : "no",
        mfgunlock::framecount::g_ui_candidate_format.load(std::memory_order_relaxed),
        mfgunlock::framecount::g_ui_candidate_options_synced.load(std::memory_order_acquire)
            ? "yes" : "no",
        mfgunlock::framecount::g_ui_candidate_runtime_declined.load(std::memory_order_acquire)
            ? "yes" : "no");
    ImGui::TextDisabled(
        "UI retry: cooldown=%u frames, consecutive failures=%u, last failed resource=0x%llx, recoveries=%llu.",
        mfgunlock::framecount::g_ui_candidate_retry_frames.load(std::memory_order_acquire),
        mfgunlock::framecount::g_ui_candidate_consecutive_failures.load(std::memory_order_acquire),
        static_cast<unsigned long long>(
            mfgunlock::framecount::g_ui_candidate_last_failed_resource.load(
                std::memory_order_acquire)),
        mfgunlock::framecount::g_ui_candidate_recoveries.load(std::memory_order_relaxed));
    ImGui::TextDisabled(
        "UI tag injection: injected=%llu, failures=%llu, native fallbacks=%llu, "
        "missing command buffer=%llu, full batches=%llu, last result=0x%x; "
        "lifecycle=OnlyValidNow.",
        mfgunlock::framecount::g_ui_candidate_tags_injected.load(std::memory_order_relaxed),
        mfgunlock::framecount::g_ui_candidate_injection_failures.load(std::memory_order_relaxed),
        mfgunlock::framecount::g_ui_candidate_native_fallbacks.load(std::memory_order_relaxed),
        mfgunlock::framecount::g_ui_candidate_missing_command_buffer.load(std::memory_order_relaxed),
        mfgunlock::framecount::g_ui_candidate_full_batches.load(std::memory_order_relaxed),
        mfgunlock::framecount::g_ui_candidate_last_result.load(std::memory_order_relaxed));
  }

  ImGui::Separator();
  bool warp = g_validated_warp_blend.load(std::memory_order_relaxed);
  if (ImGui::Checkbox("Validated Warp Blend (Ampere, 310.9.1)", &warp)) {
    g_validated_warp_blend.store(warp, std::memory_order_relaxed);
    reshade::set_config_value(nullptr, kConfigSection, "ValidatedWarpBlend", warp ? 1 : 0);
  }
  ImGui::TextDisabled("Changes apply after restarting the game.");
  if (warp && debug_options) {
    static const char* kWarpModes[] = {
        "Redirect control",
        "Blackwell baseline sm_86",
        "Validated Warp sm_86",
    };
    int warp_mode = g_validated_warp_mode.load(std::memory_order_relaxed);
    if (ImGui::Combo("Warp diagnostic mode", &warp_mode, kWarpModes, ARRAYSIZE(kWarpModes)))
      SetWarpMode(warp_mode);
    ImGui::TextDisabled("0: relocation control; 1: baseline rebuild; 2: normal Warp quality path.");
  }
  DrawWarpStatus(warp, debug_options);

  if (debug_options) {
    ImGui::Separator();
    bool temporal = g_temporal_fix.load(std::memory_order_relaxed);
    if (ImGui::Checkbox("Temporal fix (stops all frames landing at the midpoint)", &temporal)) {
      g_temporal_fix.store(temporal, std::memory_order_relaxed);
      reshade::set_config_value(nullptr, kConfigSection, "TemporalFix", temporal ? 1 : 0);
    }
    ImGui::TextDisabled("Applied once at load; restart the game to change it.");
    if (TryAcquireSRWLockShared(&g_provider_maintenance_lock)) {
      if (g_midpoint_patched.load(std::memory_order_acquire)) {
        ImGui::TextWrapped("Temporal fix: %zu provider(s); last result: %s.",
                           g_midpoint_modules.size(), g_midpoint_detail.c_str());
      } else {
        ImGui::TextDisabled("Temporal fix: pending.");
      }
      ReleaseSRWLockShared(&g_provider_maintenance_lock);
    }
  } else if (!g_temporal_fix.load(std::memory_order_relaxed)) {
    ImGui::TextDisabled("Temporal correction disabled (advanced setting).");
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
  int debug_options = 0;
  reshade::get_config_value(nullptr, kConfigSection, "ShowDebugOptions", debug_options);
  g_show_debug_options.store(debug_options != 0, std::memory_order_relaxed);
  int ui_candidate_injection = 0;
  if (debug_options != 0) {
    reshade::get_config_value(nullptr, kConfigSection, "UICandidateInjection",
                              ui_candidate_injection);
  }
  mfgunlock::framecount::g_ui_candidate_injection_enabled.store(
      ui_candidate_injection != 0, std::memory_order_relaxed);
  if (debug_options == 0 && reshade::get_config_value(
          nullptr, kConfigSection, "UICandidateInjection", ui_candidate_injection) &&
      ui_candidate_injection != 0) {
    reshade::set_config_value(nullptr, kConfigSection, "UICandidateInjection", 0);
  }
  int warp_mode = static_cast<int>(mfgunlock::validatedwarp::Mode::kValidatedWarp);
  const bool has_mode = reshade::get_config_value(nullptr, kConfigSection, "WarpDiagnosticMode", warp_mode);
  const int configured_mode = static_cast<int>(mfgunlock::validatedwarp::ConfiguredMode(debug_options != 0, warp_mode));
  g_validated_warp_mode.store(configured_mode, std::memory_order_relaxed);
  // Migrate diagnostic INIs before any provider preparation, even if the panel
  // is never opened. Hiding compatibility settings does not reset those keys.
  if (has_mode && warp_mode != configured_mode)
    reshade::set_config_value(nullptr, kConfigSection, "WarpDiagnosticMode", configured_mode);

  int preset = 0;
  const bool has_preset = reshade::get_config_value(nullptr, kConfigSection, "FrameGenerationPreset", preset);
  const auto configured_preset = mfgunlock::fgpreset::Parse(preset);
  mfgunlock::fgpreset::g_requested.store(configured_preset, std::memory_order_relaxed);
  if (has_preset && preset != static_cast<int>(configured_preset))
    reshade::set_config_value(nullptr, kConfigSection, "FrameGenerationPreset", static_cast<int>(configured_preset));
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
  if (reshade::get_config_value(nullptr, kConfigSection, "UIComposition", value)) {
    mfgunlock::framecount::g_ui_composition_enabled.store(
        value != 0, std::memory_order_relaxed);
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
      mfgunlock::framecount::g_observe_streamline_ui_tags = ObserveNativeStreamlineTags;
      mfgunlock::framecount::g_observe_ui_candidate = ObserveUiCandidate;
      mfgunlock::framecount::g_acquire_ui_candidate = AcquireUiCandidate;
      mfgunlock::framecount::g_release_ui_candidate = ReleaseUiCandidate;
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
      reshade::register_event<reshade::addon_event::destroy_resource>(
          OnDestroyUiCandidateResource);
      reshade::register_event<reshade::addon_event::barrier>(OnUiCandidateBarrier);
      reshade::register_event<reshade::addon_event::bind_render_targets_and_depth_stencil>(
          OnBindUiCandidateRenderTargets);
      reshade::register_event<reshade::addon_event::clear_render_target_view>(
          OnClearUiCandidateRenderTarget);
      reshade::register_event<reshade::addon_event::init_swapchain>(OnInitSwapchain);
      reshade::register_event<reshade::addon_event::destroy_swapchain>(OnDestroySwapchain);
      reshade::register_event<reshade::addon_event::present>(OnPresentUiOutput);
      break;
    case DLL_PROCESS_DETACH:
      reshade::unregister_event<reshade::addon_event::present>(OnPresentUiOutput);
      reshade::unregister_event<reshade::addon_event::destroy_swapchain>(OnDestroySwapchain);
      reshade::unregister_event<reshade::addon_event::init_swapchain>(OnInitSwapchain);
      reshade::unregister_event<reshade::addon_event::clear_render_target_view>(
          OnClearUiCandidateRenderTarget);
      reshade::unregister_event<reshade::addon_event::bind_render_targets_and_depth_stencil>(
          OnBindUiCandidateRenderTargets);
      reshade::unregister_event<reshade::addon_event::barrier>(OnUiCandidateBarrier);
      reshade::unregister_event<reshade::addon_event::destroy_resource>(
          OnDestroyUiCandidateResource);
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
      mfgunlock::framecount::g_observe_streamline_ui_tags = nullptr;
      mfgunlock::framecount::g_observe_ui_candidate = nullptr;
      mfgunlock::framecount::g_acquire_ui_candidate = nullptr;
      mfgunlock::framecount::g_release_ui_candidate = nullptr;
      ClearUiRuntimeCandidates();
      g_ui_candidate_d3d12.store(false, std::memory_order_relaxed);
      mfgunlock::fgpreset::Shutdown();
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
