/*
 * RenoDX MFG Unlock
 * SPDX-License-Identifier: MIT
 *
 * Enables native NVIDIA DLSS-G/MFG compatibility on supported architecture
 * profiles by preparing the mapped provider, relaxing its architecture gates,
 * correcting generated-frame timing, and exposing the verified frame count.
 * All binary changes are process-local. Orderly rollback is attempted during addon teardown;
 * live unload is not advertised safe while teardown still runs under the loader lock.
 */

#define ImTextureID ImU64

#include <windows.h>
#include <tlhelp32.h>
#include <dxgi1_5.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cwchar>
#include <cstring>
#include <cstdio>
#include <sstream>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "./imgui_compat.hpp"
#include "./reshade_compat.hpp"
#include "./addon_entry.hpp"
#include "./registration_trace.hpp"
#include <embed/shaders.h>

#include "./blackwell_temporal.hpp"
#include "./compatibility_runtime.hpp"
#include "./advanced_features.hpp"
#include "./diagnostic_bridge.hpp"
#include "./quality_config.hpp"
#include "./early_load.hpp"
#include "./fg_preset.hpp"
#include "./feature_capabilities.hpp"
#include "./framecount.hpp"
#include "./integration_contract.hpp"
#include "./loadhook.hpp"
#include "./midpoint.hpp"
#include "./output_state.hpp"
#include "./presentation_observer.hpp"
#include "./composition_state.hpp"
#include "./runtime_lifecycle.hpp"
#include "./streamline_bridge.hpp"
#include "./ui_alpha.hpp"
#include "./ui_alpha_backend.hpp"
#include "./ui_candidate.hpp"
#include "./validated_warp.hpp"
namespace {

constexpr const char* kConfigSection = "RenoDX.MFGUnlock";
constexpr const char* kAddonSection = "ADDON";
constexpr const char* kEarlyLoadKey = "LoadFromDllMain";
constexpr const char* kAddonFileName = "renodx-mfgunlock.addon64";

using mfgunlock::g_enabled;
std::atomic<unsigned int> g_max_count{4};
std::atomic_bool g_force_flip_meter_off{false};
std::atomic_bool g_temporal_fix{true};
std::atomic_bool g_intermediate_scatter_retention{false};
std::atomic<unsigned int> g_boundary_artifact_mode{
    static_cast<unsigned int>(mfgunlock::blackwelltemporal::BoundaryArtifactMode::kOff)};
std::atomic_bool g_validated_warp_blend{false};
std::atomic_bool g_show_debug_options{false};
std::atomic_int g_validated_warp_mode{
    static_cast<int>(mfgunlock::validatedwarp::Mode::kValidatedWarp)};
std::atomic_bool g_dynamic_stack_validated{false};
std::atomic<const char*> g_dynamic_last_reason{nullptr};
std::atomic<reshade::api::swapchain*> g_primary_swapchain{nullptr};
std::atomic<reshade::api::device*> g_primary_device{nullptr};
std::atomic<uint64_t> g_primary_swapchain_area{0};
std::atomic<uint32_t> g_primary_output_width{0};
std::atomic<uint32_t> g_primary_output_height{0};
std::atomic_bool g_ui_candidate_d3d12{false};
std::atomic<reshade::api::command_queue*> g_primary_present_queue{nullptr};

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
  mfgunlock::uialpha::Evidence alpha_evidence{};
  reshade::api::command_list* last_draw_command_list = nullptr;
  reshade::api::command_queue* last_draw_queue = nullptr;
  uint32_t last_draw_submission_thread = 0;
  bool last_draw_submitted = false;
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
  uint32_t ui_encoding = static_cast<uint32_t>(
      mfgunlock::outputstate::UiEncoding::kUnknown);
  uint32_t state = 0;
  uint32_t stable_frames = 0;
  uint32_t rtv_binds_after_clear = 0;
  uint32_t reject_reasons = mfgunlock::uicandidate::kStale;
  uint32_t late_writes = 0;
  uint32_t alpha_convention = static_cast<uint32_t>(
      mfgunlock::uialpha::AlphaConvention::kUnknown);
  uint32_t alpha_draws = 0;
  uint32_t alpha_unproven_draws = 0;
  uint32_t family_width = 0;
  uint32_t family_height = 0;
  uint32_t family_format = 0;
};

constexpr size_t kMaxUiBlendPipelines = 1024;
constexpr size_t kMaxUiCommandLists = 128;
constexpr uint32_t kMaxUiRenderTargets = 8;

struct UiBlendPipelineState {
  bool used = false;
  uint64_t pipeline = 0;
  std::array<mfgunlock::uialpha::BlendState, kMaxUiRenderTargets> targets{};
};

struct UiCommandRenderState {
  bool used = false;
  reshade::api::command_list* command_list = nullptr;
  uint64_t pipeline = 0;
  std::array<uint64_t, kMaxUiRenderTargets> render_targets{};
  uint32_t render_target_count = 0;
};

SRWLOCK g_ui_candidate_lock = SRWLOCK_INIT;
UiRuntimeCandidate* FindUiRuntimeCandidateLocked(uint64_t resource);
void InvalidateUiAlphaSource(uint64_t native_resource);
std::array<UiRuntimeCandidate, kMaxUiRuntimeCandidates> g_ui_candidates{};
std::array<uint64_t, kMaxKnownSwapchainBuffers> g_known_swapchain_buffers{};
bool g_ui_candidate_overflow = false;
bool g_ui_candidate_ambiguous = false;
uint64_t g_ui_selected_resource = 0;
uint64_t g_ui_selected_clear_serial = 0;
uint64_t g_ui_clear_sequence = 0;  // Protected by g_ui_candidate_lock; never reset on recreate.
uint32_t g_ui_selected_stable_frames = 0;
bool g_ui_candidate_family_valid = false;
uint32_t g_ui_candidate_family_width = 0;
uint32_t g_ui_candidate_family_height = 0;
uint32_t g_ui_candidate_family_format = 0;
uint64_t g_ui_candidate_family_last_seen_ms = 0;
uint64_t g_ui_candidate_family_handoffs = 0;

SRWLOCK g_ui_blend_lock = SRWLOCK_INIT;
std::array<UiBlendPipelineState, kMaxUiBlendPipelines> g_ui_blend_pipelines{};
std::array<UiCommandRenderState, kMaxUiCommandLists> g_ui_command_states{};

#if defined(MFGUNLOCK_RESHADE_API) && MFGUNLOCK_RESHADE_API >= 18
// A bounded staging pool, not a provider-history pool. SL owns the volatile
// clone consumed by FG; each slot owns only extraction + that clone's copy.
constexpr size_t kUiAlphaSlots = 4;
constexpr uint64_t kUiAlphaBudget = 128ull * 1024ull * 1024ull;
struct UiAlphaFrame {
  reshade::api::resource alpha_resource{};
  reshade::api::resource_view alpha_uav{};
  reshade::api::resource source_resource{};
  reshade::api::resource_view source_srv{};
  IUnknown* source_reference = nullptr;
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t source_format = 0;
  mfgunlock::uialphabackend::State state{};
};
struct UiAlphaGpuBackend {
  reshade::api::device* device = nullptr;
  reshade::api::command_queue* queue = nullptr;
  reshade::api::pipeline_layout layout{};
  reshade::api::pipeline pipeline{};
  reshade::api::fence retirement_fence{};
  uint64_t next_retirement_fence_value = 1;
  uint32_t active_slot = UINT32_MAX;
  std::array<UiAlphaFrame, kUiAlphaSlots> slots{};
};
SRWLOCK g_ui_alpha_backend_lock = SRWLOCK_INIT;
UiAlphaGpuBackend g_ui_alpha_backend{};
uint64_t g_ui_alpha_ticket_sequence = 0; // Never reset on queue/device address reuse.
thread_local bool g_ui_alpha_internal_work = false;
struct UiAlphaInternalScope {
  bool previous = g_ui_alpha_internal_work;
  UiAlphaInternalScope() { g_ui_alpha_internal_work = true; }
  ~UiAlphaInternalScope() { g_ui_alpha_internal_work = previous; }
};
UiAlphaFrame& ActiveUiAlphaFrame() {
  return g_ui_alpha_backend.slots[g_ui_alpha_backend.active_slot];
}
#endif
std::atomic<unsigned long long> g_ui_alpha_prepare_successes{0};
std::atomic<unsigned long long> g_ui_alpha_prepare_failures{0};
std::atomic<unsigned long long> g_ui_alpha_extractions{0};
std::atomic<unsigned long long> g_ui_alpha_extraction_failures{0};

// Marker scans can match literals embedded in this addon, so never inspect the
// addon's own image as a candidate provider.
HMODULE g_self_module = nullptr;
std::atomic_bool g_bootstrap_complete{false};
std::atomic_bool g_bootstrap_busy{false};
std::atomic_bool g_bootstrap_shutting_down{false};
std::atomic_bool g_streamline_scan_pending{true};
std::atomic_bool g_ngx_scan_pending{true};
void TryAttachRuntime();
void DumpDlssStartupEvidence(const char* phase);

// ------------------------------------------------------- in-memory arch gate
// The provider uses comparisons against the Blackwell NVAPI architecture ID for
// both capability advertising and runtime generation. Rewrite only the compare
// immediates; the architecture lookup table must remain untouched.

// ------------------------------------------------ locating the DLSS-G provider
// The provider may come from the game directory, the NVIDIA OTA store, or a
// renamed cache entry. Paths are a fast path; NGX provider exports plus a known
// marker cover relocated providers.

std::vector<mfgunlock::hook::ModuleIdentity> g_inspected_modules;
std::vector<HMODULE> g_dlssg_modules;
SRWLOCK g_provider_maintenance_lock = SRWLOCK_INIT;
std::atomic_bool g_provider_scan_pending{true};
bool CleanProviderUnloaded();

std::atomic<uint64_t> g_quality_requested_epoch{0};
std::atomic<uint64_t> g_quality_off_epoch{UINT64_MAX};
std::atomic<uint64_t> g_quality_off_release{UINT64_MAX};
std::atomic<uint32_t> g_quality_off_viewport{UINT32_MAX};
std::atomic_bool g_quality_retained_feature{false};

mfgunlock::qualityconfig::Config RequestedQuality() {
  return {g_temporal_fix.load(std::memory_order_relaxed),
          g_intermediate_scatter_retention.load(std::memory_order_relaxed),
          g_boundary_artifact_mode.load(std::memory_order_relaxed),
          g_validated_warp_blend.load(std::memory_order_relaxed),
          static_cast<uint32_t>(mfgunlock::validatedwarp::ConfiguredMode(
              g_validated_warp_mode.load(std::memory_order_relaxed)))};
}

void QualityEvent(mfgunlock::diagnostic::LifecycleKind kind) {
  const auto lifecycle = mfgunlock::ngx::internal::LifecycleSnapshot();
  mfgunlock::diagnostic::LifecycleEvent event;
  event.kind = kind;
  event.thread = GetCurrentThreadId();
  event.epoch = lifecycle.epoch;
  event.handles = lifecycle.handles;
  event.evaluates = lifecycle.evaluates;
  event.creates = lifecycle.creates;
  event.releases = lifecycle.releases;
  event.tracking_uncertain = lifecycle.uncertain;
  event.requested_quality = mfgunlock::qualityconfig::g_requested.load(std::memory_order_acquire);
  event.prepared_quality = mfgunlock::qualityconfig::g_prepared.load(std::memory_order_acquire);
  mfgunlock::diagnostic::Emit(event);
}

void SaveQualityRequest() {
  namespace qc = mfgunlock::qualityconfig;
  const auto requested = qc::Encode(RequestedQuality());
  const auto previous = qc::g_requested.exchange(requested, std::memory_order_acq_rel);
  if (requested == previous) return;
  g_quality_requested_epoch.store(mfgunlock::ngx::internal::LifecycleSnapshot().epoch);
  g_quality_off_epoch.store(UINT64_MAX);
  g_quality_retained_feature.store(false);
  QualityEvent(qc::Pending(previous, qc::g_prepared.load())
      ? mfgunlock::diagnostic::LifecycleKind::kQualitySuperseded
      : mfgunlock::diagnostic::LifecycleKind::kQualityRequested);
  if (qc::Pending(requested, qc::g_prepared.load()))
    QualityEvent(mfgunlock::diagnostic::LifecycleKind::kQualityRestartRequired);
}

mfgunlock::qualityconfig::Config PreparedQuality() {
  namespace qc = mfgunlock::qualityconfig;
  const bool first = qc::g_prepared.load(std::memory_order_acquire) == qc::kUnknown;
  const auto config = qc::Prepared();
  if (first) QualityEvent(mfgunlock::diagnostic::LifecycleKind::kQualityFrozen);
  return config;
}

void ObserveFgMode(uint32_t viewport, uint32_t mode, uint32_t result) {
  if (result != static_cast<uint32_t>(sl::Result::eOk)) return;
  const bool off = mode == static_cast<uint32_t>(sl::DLSSGMode::eOff);
  const auto state = mfgunlock::ngx::internal::LifecycleSnapshot();
  mfgunlock::diagnostic::LifecycleEvent event;
  event.kind = off ? mfgunlock::diagnostic::LifecycleKind::kModeOff
                   : mfgunlock::diagnostic::LifecycleKind::kModeOn;
  event.thread = GetCurrentThreadId();
  event.epoch = state.epoch;
  event.handles = state.handles;
  event.viewport = viewport;
  event.mode = mode;
  event.result = result;
  event.tracking_uncertain = state.uncertain;
  mfgunlock::diagnostic::Emit(event);
  namespace qc = mfgunlock::qualityconfig;
  if (!qc::Pending(qc::g_requested.load(), qc::g_prepared.load())) return;
  if (off) {
    g_quality_off_viewport.store(viewport);
    g_quality_off_release.store(state.last_release);
    g_quality_off_epoch.store(state.epoch);
  } else if (g_quality_off_viewport.load() == viewport &&
             g_quality_off_epoch.load() == state.epoch &&
             g_quality_off_release.load() == state.last_release && state.handles != 0) {
    g_quality_retained_feature.store(true);
  }
}

bool BeforeFgCreate() {
  // Serialize only with our preparation. Never hold this lock across NVIDIA's
  // CreateFeature, and never infer CUDA module teardown from an NGX handle count.
  if (!TryAcquireSRWLockExclusive(&g_provider_maintenance_lock)) return false;
  mfgunlock::provider::g_create_seen.store(true, std::memory_order_release);
  ReleaseSRWLockExclusive(&g_provider_maintenance_lock);
  return true;
}

void RememberDlssgModule(HMODULE mod) {
  if (mod == nullptr || mod == g_self_module || !mfgunlock::provider::IsDlssgProvider(mod)) return;
  if (std::find(g_dlssg_modules.begin(), g_dlssg_modules.end(), mod) == g_dlssg_modules.end()) {
    g_dlssg_modules.push_back(mod);
  }
}

uint64_t ProviderIdentityToken(const mfgunlock::provider::internal::ImageIdentity& identity) {
  return (static_cast<uint64_t>(identity.timestamp) << 32) | identity.image_bytes;
}

void PruneProviderInventoryLocked() {
  std::erase_if(g_dlssg_modules, [](HMODULE module) {
    return !mfgunlock::provider::IsImageMapping(module);
  });
  std::erase_if(g_inspected_modules, [](const auto& identity) {
    return !mfgunlock::hook::IsCurrent(identity);
  });
}

void SyncProviderLifecycleLocked() {
  PruneProviderInventoryLocked();
  if (g_dlssg_modules.empty()) {
    mfgunlock::runtime::ObserveProviderMissing(CleanProviderUnloaded());
    return;
  }
  if (g_dlssg_modules.size() != 1) {
    mfgunlock::runtime::ObserveProviderAmbiguous();
    return;
  }

  const HMODULE module = g_dlssg_modules.front();
  const auto observed = mfgunlock::provider::InspectProvider(module);
  const auto* architecture_profile = mfgunlock::architecture::ActiveProfile();
  if (!observed.mapped || !observed.dlssg) {
    mfgunlock::runtime::ObserveProviderMissing();
    return;
  }
  if (architecture_profile && architecture_profile->RequiresProviderRetarget()) {
    const auto registry = mfgunlock::provider::GetProviderStatus();
    if (registry.busy || registry.failed || registry.blocked != 0) {
      mfgunlock::runtime::ObserveProviderBlocked();
      return;
    }
  }
  const auto prior = mfgunlock::runtime::Read();
  const uint64_t identity = ProviderIdentityToken(observed.identity);
  if (prior.provider_known && prior.provider_module == reinterpret_cast<uint64_t>(module) &&
      prior.provider_identity != identity && CleanProviderUnloaded()) {
    // A fresh, unmodified image may reuse the same base address. Close the
    // previous generation before evaluating the new one.
    mfgunlock::runtime::ObserveProviderMissing(true);
  }
  mfgunlock::runtime::ObserveProvider(
      reinterpret_cast<uint64_t>(module), identity,
      observed.profile_index, observed.qualification, observed.exact_inventory,
      architecture_profile);
  if (!prior.provider_known || prior.provider_module != reinterpret_cast<uint64_t>(module) ||
      prior.provider_identity != identity ||
      prior.provider_profile_index != observed.profile_index ||
      prior.provider_qualification != static_cast<uint32_t>(observed.qualification) ||
      prior.provider_exact_qualified != static_cast<uint32_t>(observed.exact_inventory)) {
    const auto version = mfgunlock::streamline::internal::ReadModuleDisplayVersion(module);
    std::stringstream message;
    message << "provider observation path=" << mfgunlock::provider::internal::ModulePath(module)
            << " identity=" << std::hex << identity << std::dec
            << " version=" << version.major << '.' << version.minor << '.' << version.build
            << '.' << version.revision
            << " profile=" << observed.profile_index
            << " qualified=" << (observed.exact_inventory ? "yes" : "no");
    mfgunlock::provider::Log(message.str());  // Optional layout recognition is informational.
  }
}

void SyncRuntimeLifecycleEvidence() {
  const auto ngx = mfgunlock::ngx::internal::LifecycleSnapshot();
  mfgunlock::runtime::ObserveNgx(ngx.epoch, ngx.handles, ngx.uncertain);
  const auto streamline = mfgunlock::streamline::internal::g_lifecycle_state.load(
      std::memory_order_acquire);
  mfgunlock::runtime::ObserveStreamline(
      static_cast<uint32_t>(streamline),
      streamline == mfgunlock::streamline::internal::LifecycleState::kStartupFailed);
}

// Bootstrap and maintenance discover providers mapped before this addon.
// Loader notifications request deferred work. Present also performs a periodic
// fallback pass; this is not a zero-cost, per-frame operation.
const std::vector<HMODULE>& DiscoverDlssgModules() {
  if (HMODULE fast = GetModuleHandleW(L"nvngx_dlssg.dll")) RememberDlssgModule(fast);

  HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, GetCurrentProcessId());
  if (snap == INVALID_HANDLE_VALUE) return g_dlssg_modules;
  MODULEENTRY32W me = {};
  me.dwSize = sizeof(me);
  if (Module32FirstW(snap, &me)) {
    do {
      if (me.hModule == g_self_module) continue;
      mfgunlock::streamline::internal::ObserveCommonModule(me.hModule);
      mfgunlock::hook::ModuleIdentity identity{};
      if (!mfgunlock::hook::CaptureModule(me.hModule, identity)) continue;
      if (std::find(g_inspected_modules.begin(), g_inspected_modules.end(), identity) !=
          g_inspected_modules.end()) {
        continue;
      }
      g_inspected_modules.push_back(identity);
      RememberDlssgModule(me.hModule);
    } while (Module32NextW(snap, &me));
  }
  CloseHandle(snap);
  return g_dlssg_modules;
}

std::atomic_bool g_gate_patched{false};
std::vector<mfgunlock::provider::internal::BytePatch> g_gate_sites;
std::vector<HMODULE> g_gate_modules;
std::atomic<unsigned int> g_mfg_gate_ceiling{0};
std::atomic<unsigned int> g_mfg_gate_candidates{0};
std::atomic<const char*> g_mfg_gate_status{"NOT_ATTEMPTED"};

// Split out so the load-time trigger can patch a module it already holds a
// handle to. That path runs under the loader lock, where CreateToolhelp32Snapshot
// (which FindDlssgModule uses) would deadlock -- so it must never scan.
bool HasTemporalPatch(HMODULE mod);
void PatchArchGatesInModule(HMODULE mod) {
  if (mod == nullptr) return;
  const auto* profile = mfgunlock::architecture::ActiveProfile();
  if (!g_enabled.load()) return;
  // This byte rewrite needs the actual numeric architecture operand. It is
  // independent of admission to the public native MFG/SetOptions operations.
  if (!profile) {
    g_mfg_gate_status.store("NATIVE_ARCH_VALUE_MISSING", std::memory_order_release);
    return;
  }
  const bool retarget = profile->RequiresProviderRetarget();
  if (retarget) mfgunlock::provider::PrepareProvider(mod);
  // Only an incomplete memory transaction prevents another image mutation.
  if (mfgunlock::provider::internal::g_registry_failed.load(std::memory_order_acquire)) {
    g_mfg_gate_status.store("MEMORY_TRANSACTION_INCOMPLETE", std::memory_order_release);
    return;
  }
  // Optional temporal/warp transforms are not prerequisites for discovering
  // and opening this provider's independent MFG capability gates.
  if (std::find(g_gate_modules.begin(), g_gate_modules.end(), mod) != g_gate_modules.end()) return;
  if (!g_gate_sites.empty() && !g_gate_patched.load(std::memory_order_acquire)) {
    g_mfg_gate_status.store("ROLLBACK_INCOMPLETE", std::memory_order_release);
    return;
  }
  if (mfgunlock::provider::g_create_seen.load(std::memory_order_acquire)) {
    g_mfg_gate_status.store("STARTUP_MISSED", std::memory_order_release);
    return;
  }

  // Maintenance is already change-driven and serialized. Do not negatively
  // cache a bare HMODULE: an unpatched image can unload and reuse that address.
  mfgunlock::provider::MfgGatePlan plan;
  const bool found = mfgunlock::provider::FindMfgGatePlan(mod, plan);
  g_mfg_gate_candidates.store(plan.contract.candidates, std::memory_order_relaxed);
  if (!found) {
    g_mfg_gate_status.store(mfgunlock::mfggate::Name(plan.contract.result), std::memory_order_release);
    mfgunlock::provider::Log("MFG_GATE " + std::string(mfgunlock::mfggate::Name(plan.contract.result)) +
        " reason=" + plan.contract.reason);
    return;
  }
  g_mfg_gate_status.store("STRUCTURAL_VERIFIED", std::memory_order_release);
  g_gate_modules.reserve(g_gate_modules.size() + 1);
  if (!mfgunlock::provider::ApplyMfgGatePlan(plan, g_gate_sites)) {
    g_mfg_gate_status.store(g_gate_sites.empty() ? "COMMIT_FAILED" : "ROLLBACK_INCOMPLETE",
                            std::memory_order_release);
    // Undo ownership is not a successfully applied capability.
    return;
  }
  g_gate_modules.push_back(mod);
  g_mfg_gate_ceiling.store(plan.contract.ceiling, std::memory_order_release);
  g_gate_patched.store(true, std::memory_order_release);
  g_mfg_gate_status.store("APPLIED", std::memory_order_release);
  std::stringstream details;
  details << "MFG_GATE APPLIED source=STRUCTURAL ceiling=" << plan.contract.ceiling
          << " advertise_rva=0x" << std::hex << plan.contract.sites[0].instruction
          << " execute_rva=0x" << plan.contract.sites[1].instruction
          << " flag_offset=0x" << plan.contract.flag_offset;
  mfgunlock::provider::Log(details.str());
}

void TryPatchDlssgArchGate() {
  for (HMODULE mod : g_dlssg_modules) PatchArchGatesInModule(mod);
}

void RestoreDlssgArchGate() {
  if (g_gate_sites.empty()) return;
  g_gate_patched.store(false, std::memory_order_release);
  g_mfg_gate_ceiling.store(0, std::memory_order_release);
  if (!mfgunlock::provider::RestoreMfgComparisons(g_gate_sites)) {
    g_mfg_gate_status.store("ROLLBACK_INCOMPLETE", std::memory_order_release);
    mfgunlock::provider::Log("MFG gate restore incomplete; undo metadata retained", true);
    return;
  }
  g_gate_modules.clear();
  g_mfg_gate_status.store("RESTORED", std::memory_order_release);
  g_gate_patched.store(false, std::memory_order_release);
}

// --------------------------------------------------- validated warp blend
// Optional cross-generation quality transform discovered from kernel structure.
// Native SM89 uses the matching payload directly; retargeted backends rebuild
// it for their target SM and keep provider-owned descriptors available for orderly rollback.

struct ValidatedWarpModulePatch {
  HMODULE module = nullptr;
  std::vector<mfgunlock::validatedwarp::Redirect> redirects;
  std::string provider_version;
  std::string detail;
  mfgunlock::validatedwarp::Mode mode = mfgunlock::validatedwarp::Mode::kValidatedWarp;
  uint32_t target_sm = 86;
};
std::vector<ValidatedWarpModulePatch> g_validated_warp_modules;
std::vector<HMODULE> g_validated_warp_rejected_modules;
std::string g_validated_warp_detail;
// Incomplete commits are not applied features, but still own rollback metadata.
std::vector<mfgunlock::validatedwarp::Redirect> g_failed_quality_redirects;

void KeepFailedQualityRedirects(std::vector<mfgunlock::validatedwarp::Redirect>& redirects) {
  if (redirects.empty()) return;
  mfgunlock::provider::internal::g_registry_failed.store(true, std::memory_order_release);
  for (auto& redirect : redirects) g_failed_quality_redirects.push_back(std::move(redirect));
  redirects.clear();
}

bool PrepareProviderForOptionalRedirect(HMODULE mod) {
  mfgunlock::provider::PrepareProvider(mod);
  // The independently rebuilt quality payload may still be tried after an
  // unrelated recipe fails. An incomplete memory transaction retains ownership.
  return !mfgunlock::provider::internal::g_registry_failed.load(std::memory_order_acquire);
}

void PatchValidatedWarpInModule(HMODULE mod) {
  if (mod == nullptr || mfgunlock::provider::g_create_seen.load(std::memory_order_acquire)) return;
  if (!PreparedQuality().warp) return;
  const auto* profile = mfgunlock::architecture::ActiveProfile();
  if (!g_enabled.load() || !profile ||
      !mfgunlock::features::HasQualityBackend(profile)) {
    return;
  }
  const uint32_t target_sm = profile->target_sm;
  if (std::any_of(g_validated_warp_modules.begin(), g_validated_warp_modules.end(),
                  [mod](const auto& patch) { return patch.module == mod; })) return;
  if (std::find(g_validated_warp_rejected_modules.begin(),
                g_validated_warp_rejected_modules.end(), mod) !=
      g_validated_warp_rejected_modules.end()) return;

  std::vector<mfgunlock::validatedwarp::Redirect> redirects;
  mfgunlock::validatedwarp::Result result{};
  std::string provider_version;
  const auto mode = static_cast<mfgunlock::validatedwarp::Mode>(PreparedQuality().warp_mode);
  const auto prepare_provider = profile->RequiresProviderRetarget()
      ? PrepareProviderForOptionalRedirect : nullptr;
  const bool applied = mfgunlock::validatedwarp::Apply(
      mod, redirects, result, provider_version, mode,
      prepare_provider, target_sm);
  const std::string detail = result.detail;
  if (!applied || !result.applied) {
    KeepFailedQualityRedirects(redirects);
    g_validated_warp_rejected_modules.push_back(mod);
    if (g_validated_warp_modules.empty()) g_validated_warp_detail = detail;
    std::stringstream message;
    message << "mfgunlock: Warp diagnostic "
            << mfgunlock::validatedwarp::ModeLabel(mode, target_sm)
            << " not applied"
            << (provider_version.empty() ? "" : " to DLSS-G " + provider_version)
            << " -- " << (detail.empty() ? "required kernel operands were not located" : detail)
            << ".";
    reshade::log::message(reshade::log::level::warning, message.str().c_str());
    return;
  }

  g_validated_warp_detail = detail;
  g_validated_warp_modules.push_back(
      {mod, std::move(redirects), provider_version, detail, mode, target_sm});
  std::stringstream message;
  message << "mfgunlock: Warp diagnostic "
          << mfgunlock::validatedwarp::ModeLabel(mode, target_sm)
          << " applied to DLSS-G " << provider_version
          << " on " << mfgunlock::architecture::Name(profile->architecture)
          << " -- " << detail << ".";
  reshade::log::message(reshade::log::level::info, message.str().c_str());
}

void TryPatchValidatedWarp() {
  for (HMODULE mod : g_dlssg_modules) PatchValidatedWarpInModule(mod);
}

void RestoreValidatedWarp() {
  const bool restored = mfgunlock::qualityconfig::RestoreRecords(
      g_validated_warp_modules, [](auto& module) {
        return mfgunlock::validatedwarp::Restore(module.redirects);
      });
  if (!restored) {
    mfgunlock::provider::internal::g_registry_failed.store(true, std::memory_order_release);
    mfgunlock::provider::Log("Warp restore incomplete; replacement and rollback metadata retained", true);
    return;
  }
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
std::atomic_bool g_blackwell_temporal_patched{false};
struct MidpointModulePatch {
  HMODULE module;
  std::vector<mfgunlock::midpoint::Patch> patches;
  void* allocation;
};
struct BlackwellTemporalModulePatch {
  HMODULE module = nullptr;
  std::vector<mfgunlock::blackwelltemporal::Redirect> redirects;
  std::string provider_version;
  std::string detail;
  bool intermediate_scatter = false;
  mfgunlock::blackwelltemporal::BoundaryArtifactMode boundary_mode =
      mfgunlock::blackwelltemporal::BoundaryArtifactMode::kOff;
  uint32_t target_sm = 86;
};
std::vector<MidpointModulePatch> g_midpoint_modules;
std::vector<MidpointModulePatch> g_failed_midpoint_modules;
std::vector<BlackwellTemporalModulePatch> g_blackwell_temporal_modules;
std::vector<HMODULE> g_midpoint_rejected_modules;
std::vector<HMODULE> g_blackwell_temporal_rejected_modules;

bool CleanProviderUnloaded() {
  // No patch pointer may outlive an image which can be admitted on reload.
  // A patched provider is retained by provider.hpp, so an actual disappearance
  // with any undo record is never considered a clean reload.
  if (!g_gate_sites.empty() || !g_validated_warp_modules.empty() ||
      !g_failed_quality_redirects.empty() || !g_midpoint_modules.empty() ||
      !g_failed_midpoint_modules.empty() || !g_blackwell_temporal_modules.empty()) return false;
  if (!TryAcquireSRWLockShared(&mfgunlock::provider::internal::g_lock)) return false;
  const bool clean = std::all_of(mfgunlock::provider::internal::g_providers.begin(),
      mfgunlock::provider::internal::g_providers.end(),
      [](const auto& provider) { return provider.patches.empty(); });
  ReleaseSRWLockShared(&mfgunlock::provider::internal::g_lock);
  return clean;
}
std::string g_midpoint_detail;
std::string g_blackwell_temporal_detail;

bool HasBlackwellTemporalPatch(HMODULE mod) {
  return std::any_of(
      g_blackwell_temporal_modules.begin(), g_blackwell_temporal_modules.end(),
      [mod](const BlackwellTemporalModulePatch& patch) { return patch.module == mod; });
}

bool HasTemporalPatch(HMODULE mod) {
  return HasBlackwellTemporalPatch(mod) ||
      std::any_of(g_midpoint_modules.begin(), g_midpoint_modules.end(),
                  [mod](const MidpointModulePatch& patch) { return patch.module == mod; });
}

bool BlackwellTemporalRejected(HMODULE mod) {
  return std::find(g_blackwell_temporal_rejected_modules.begin(),
                   g_blackwell_temporal_rejected_modules.end(), mod) !=
      g_blackwell_temporal_rejected_modules.end();
}

void RejectBlackwellTemporal(HMODULE mod, const std::string& provider_version,
                             const std::string& detail,
                             bool intermediate_scatter,
                             mfgunlock::blackwelltemporal::BoundaryArtifactMode boundary_mode,
                             bool midpoint_fallback = true) {
  if (!BlackwellTemporalRejected(mod)) g_blackwell_temporal_rejected_modules.push_back(mod);
  if (g_blackwell_temporal_modules.empty()) g_blackwell_temporal_detail = detail;
  std::stringstream message;
  message << "mfgunlock: ";
  if (!intermediate_scatter) {
    message << "Blackwell temporal framework";
  } else if (boundary_mode == mfgunlock::blackwelltemporal::BoundaryArtifactMode::kOff) {
    message << "Intermediate Scatter Retention";
  } else {
    message << "Boundary Artifact Mitigation "
            << mfgunlock::blackwelltemporal::BoundaryArtifactModeName(boundary_mode);
  }
  message << " not applied"
          << (provider_version.empty() ? "" : " to DLSS-G " + provider_version)
          << " -- "
          << (detail.empty() ? "full Blackwell temporal backend did not match" : detail)
          << (midpoint_fallback
                  ? "; midpoint temporal fallback will be used."
                  : "; native temporal path retained; MFG discovery is independent.");
  reshade::log::message(reshade::log::level::warning, message.str().c_str());
}

void PatchMidpointInModule(HMODULE mod) {
  if (mfgunlock::provider::g_create_seen.load(std::memory_order_acquire)) return;
  if (mod == nullptr) return;
  const auto* profile = mfgunlock::architecture::ActiveProfile();
  if (!g_enabled.load()) return;
  if (profile && profile->RequiresProviderRetarget()) mfgunlock::provider::PrepareProvider(mod);
  if (mfgunlock::provider::internal::g_registry_failed.load(std::memory_order_acquire)) return;
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
    if (allocation != nullptr) {
      mfgunlock::provider::internal::g_registry_failed.store(true, std::memory_order_release);
      g_failed_midpoint_modules.push_back({mod, std::move(patches), allocation});
    }
    char module_path[MAX_PATH] = {};
    GetModuleFileNameA(mod, module_path, MAX_PATH);
    std::stringstream message;
    message << "mfgunlock: temporal fix not applied to " << module_path << " -- " << detail << ".";
    reshade::log::message(reshade::log::level::warning, message.str().c_str());
    g_midpoint_rejected_modules.push_back(mod);
    return;
  }

  g_midpoint_detail = detail;
  g_midpoint_modules.push_back({mod, std::move(patches), allocation});
  g_midpoint_patched.store(true, std::memory_order_release);
  char module_path[MAX_PATH] = {};
  GetModuleFileNameA(mod, module_path, MAX_PATH);
  std::stringstream message;
  message << "mfgunlock: temporal fix applied to " << module_path << " -- " << detail
          << "; generated frames should now land at their own time, not all at the midpoint.";
  reshade::log::message(reshade::log::level::info, message.str().c_str());
}

void PatchQualityProviderInModule(HMODULE mod) {
  if (mfgunlock::provider::g_create_seen.load(std::memory_order_acquire)) return;
  if (mod == nullptr) return;
  const auto* profile = mfgunlock::architecture::ActiveProfile();
  if (!g_enabled.load() || !profile ||
      !mfgunlock::features::HasQualityBackend(profile)) {
    return;
  }
  const uint32_t target_sm = profile->target_sm;
  const auto quality = PreparedQuality();
  const bool temporal_requested = quality.temporal;
  const bool scatter_requested = temporal_requested && quality.scatter;
  const bool native_blackwell_requested = temporal_requested &&
      profile->provider_backend == mfgunlock::ProviderBackend::kNativeSm89;
  const bool blackwell_temporal_requested =
      (scatter_requested || native_blackwell_requested) &&
      !HasTemporalPatch(mod) && !BlackwellTemporalRejected(mod);
  const auto boundary_mode = scatter_requested
      ? mfgunlock::blackwelltemporal::ParseBoundaryArtifactMode(
            static_cast<int>(quality.boundary))
      : mfgunlock::blackwelltemporal::BoundaryArtifactMode::kOff;

  mfgunlock::blackwelltemporal::Plan temporal_plan;
  mfgunlock::blackwelltemporal::Result temporal_result;
  std::string temporal_version;
  bool temporal_prepared = false;
  if (blackwell_temporal_requested) {
    temporal_prepared = mfgunlock::blackwelltemporal::Prepare(
        mod, scatter_requested, boundary_mode,
        temporal_plan, temporal_result, temporal_version, target_sm);
    if (!temporal_prepared) {
      RejectBlackwellTemporal(
          mod, temporal_version, temporal_result.detail,
          scatter_requested, boundary_mode);
    }
  }

  // Both quality paths inspect pristine provider fatbins. Prepare the complete
  // temporal plan first; Warp may then run provider preparation before either
  // temporal descriptor is published.
  if (quality.warp) {
    PatchValidatedWarpInModule(mod);
  }

  if (temporal_prepared) {
    const bool redirect_memory_ready = !profile->RequiresProviderRetarget() ||
        PrepareProviderForOptionalRedirect(mod);
    if (!redirect_memory_ready) {
      RejectBlackwellTemporal(mod, temporal_version,
                              "provider memory transaction is incomplete before temporal redirect",
                              scatter_requested, boundary_mode, false);
      return;
    } else {
      std::vector<mfgunlock::blackwelltemporal::Redirect> redirects;
      if (mfgunlock::blackwelltemporal::Commit(
              temporal_plan, redirects, temporal_result)) {
        g_blackwell_temporal_detail = temporal_result.detail;
        g_blackwell_temporal_modules.push_back(
            {mod, std::move(redirects), temporal_version, temporal_result.detail,
             temporal_result.intermediate_scatter, temporal_result.boundary_mode, target_sm});
        g_blackwell_temporal_patched.store(true, std::memory_order_release);
        std::stringstream message;
        message << "mfgunlock: ";
        if (!temporal_result.intermediate_scatter) {
          message << "Blackwell temporal framework";
        } else if (temporal_result.boundary_mode ==
            mfgunlock::blackwelltemporal::BoundaryArtifactMode::kOff) {
          message << "Intermediate Scatter Retention";
        } else {
          message << "Boundary Artifact Mitigation "
                  << mfgunlock::blackwelltemporal::BoundaryArtifactModeName(
                         temporal_result.boundary_mode);
        }
        message << " applied to DLSS-G "
                << temporal_version
                << " on " << mfgunlock::architecture::Name(profile->architecture)
                << " through the complete Blackwell temporal sm_" << target_sm
                << " backend -- " << temporal_result.detail << ".";
        reshade::log::message(reshade::log::level::info, message.str().c_str());
      } else {
        KeepFailedQualityRedirects(redirects);
        RejectBlackwellTemporal(mod, temporal_version, temporal_result.detail,
                                scatter_requested, boundary_mode,
                                temporal_result.fallback_safe);
        if (!temporal_result.fallback_safe) {
          mfgunlock::provider::internal::g_registry_failed.store(true, std::memory_order_release);
          return;
        }
      }
    }
  }

  if (temporal_requested && !HasTemporalPatch(mod)) PatchMidpointInModule(mod);
  PatchArchGatesInModule(mod);
}

void TryPatchMidpoint() {
  for (HMODULE mod : g_dlssg_modules) PatchMidpointInModule(mod);
}

void TryPatchQualityProviders() {
  for (HMODULE mod : g_dlssg_modules) PatchQualityProviderInModule(mod);
}

// Loader and capability callbacks share the provider inventory. A loader
// callback must not wait on maintenance which may itself be loading a DLL.
// The capability postflight rescans anything missed while that lock was held.
void TraceDlssLoadResult(LPCWSTR path, HMODULE module, DWORD flags, DWORD error) noexcept {
  namespace loader = mfgunlock::loadhook::internal;
  if (!path || !(loader::NameContains(path, L"nvngx") ||
                loader::NameContains(path, L"sl.") ||
                loader::NameContains(path, L"\\models\\dlss"))) return;
  static std::atomic_uint32_t calls{0};
  if (calls.fetch_add(1) >= 64) return;
  mfgunlock::ngx::internal::DiscoveryTrace([&](auto& message) {
    message << "LOAD_RESULT module=" << module << " flags=0x" << std::hex << flags;
    if (!module) message << " win32_error=" << std::dec << error;
    message << " path=";
    for (size_t i = 0; i < 1024 && path[i]; ++i)
      message << (path[i] >= 32 && path[i] < 127 ? static_cast<char>(path[i]) : '?');
  });
}

void ProcessLoadedDlssgModule(HMODULE mod) {
  // The LoadLibrary callback may itself be nested in a DLL initializer. It
  // only records a pending scan; Detours, provider parsing and image writes
  // occur at a later safe maintenance boundary.
  if (mod) g_provider_scan_pending.store(true, std::memory_order_release);
}

void RunProviderMaintenance() {
  AcquireSRWLockExclusive(&g_provider_maintenance_lock);
  struct Unlock {
    ~Unlock() { ReleaseSRWLockExclusive(&g_provider_maintenance_lock); }
  } unlock;
  g_provider_scan_pending.store(false, std::memory_order_release);
  DiscoverDlssgModules();
  SyncProviderLifecycleLocked();
  if (mfgunlock::runtime::Read().restart_required) return;
  const auto* profile = mfgunlock::architecture::ActiveProfile();
  if (!g_enabled.load()) {
    return;
  }
  if (profile && mfgunlock::features::HasQualityBackend(profile)) {
    TryPatchQualityProviders();
  } else {
    if (PreparedQuality().temporal) TryPatchMidpoint();
    TryPatchDlssgArchGate();
  }
  for (HMODULE mod : g_dlssg_modules) mfgunlock::fgpreset::TryInstall(mod);
  SyncProviderLifecycleLocked();
  SyncRuntimeLifecycleEvidence();
}

// A preset edit must not apply unrelated pending Warp/temporal changes live.
void InstallPresetForLoadedProviders() {
  if (!g_enabled.load()) return;
  AcquireSRWLockExclusive(&g_provider_maintenance_lock);
  for (HMODULE mod : g_dlssg_modules) mfgunlock::fgpreset::TryInstall(mod);
  ReleaseSRWLockExclusive(&g_provider_maintenance_lock);
}

void RestoreBlackwellTemporal() {
  g_blackwell_temporal_patched.store(false, std::memory_order_release);
  const bool restored = mfgunlock::qualityconfig::RestoreRecords(
      g_blackwell_temporal_modules, [](auto& module) {
        return mfgunlock::blackwelltemporal::Restore(module.redirects);
      });
  if (!restored) {
    mfgunlock::provider::internal::g_registry_failed.store(true, std::memory_order_release);
    mfgunlock::provider::Log("temporal restore incomplete; rollback metadata retained", true);
    return;
  }
  g_blackwell_temporal_rejected_modules.clear();
  g_blackwell_temporal_detail.clear();
}

void RestoreMidpoint() {
  g_midpoint_patched.store(false, std::memory_order_release);
  const auto restore = [](auto& module) {
    return mfgunlock::midpoint::Restore(module.patches, module.allocation);
  };
  const bool restored = mfgunlock::qualityconfig::RestoreRecords(g_midpoint_modules, restore);
  const bool failed_restored = mfgunlock::qualityconfig::RestoreRecords(g_failed_midpoint_modules, restore);
  if (!restored || !failed_restored) {
    mfgunlock::provider::internal::g_registry_failed.store(true, std::memory_order_release);
    mfgunlock::provider::Log("midpoint restore incomplete; rollback metadata retained", true);
    return;
  }
  g_midpoint_rejected_modules.clear();
}

// ------------------------------------------------- flip metering (sl.dlss_g)
// Multi-frame output needs the plugin's software pacing path on architectures
// without hardware flip metering. The context field moves between plugin builds,
// so derive its offset and disabled value from the plugin's own fallback path
// instead of hardcoding either one.

constexpr char kFlipMarker[] = "FG1 DLL has been detected";

struct FlipSite {
  struct Protection {
    unsigned char* address = nullptr;
    unsigned char length = 0;
    DWORD original = 0;
  };
  unsigned char* address = nullptr;
  HMODULE module = nullptr;
  unsigned char original[7]{};
  unsigned char replacement[7]{};
  unsigned char length = 0;
  // Seven bytes can cross protection boundaries. Keep each original protection
  // separately; VirtualProtect reports only the first page of a range.
  std::array<Protection, 7> protections{};
  size_t protection_count = 0;
  bool committed = false;
  bool rollback_pending = false;
};

std::atomic_bool g_flip_meter_patched{false};
std::atomic_bool g_flip_meter_failed{false};
std::atomic_bool g_flip_meter_rollback_pending{false};
std::vector<FlipSite> g_flip_meter_sites;
// One explicit process-lifetime reference for each image submitted to a write.
// Do not call FreeLibrary for these references from DLL_PROCESS_DETACH: cleanup
// runs under the loader lock, and incomplete undo still owns these addresses.
std::vector<HMODULE> g_flip_meter_modules;
SRWLOCK g_flip_meter_scan_lock = SRWLOCK_INIT;
enum class FlipPatchResult { kNotApplied, kApplied, kRollbackIncomplete };
struct FlipAttempt {
  HMODULE module = nullptr;
  mfgunlock::provider::internal::ImageIdentity identity{};
  uint64_t epoch = 0;
  uint64_t revision = 0;
  FlipPatchResult result = FlipPatchResult::kNotApplied;
};
std::vector<FlipAttempt> g_flip_meter_attempts;

struct FlipModuleReference {
  HMODULE module = nullptr;
  bool temporary = false;
  explicit FlipModuleReference(HMODULE requested) {
    if (!requested) return;
    if (std::find(g_flip_meter_modules.begin(), g_flip_meter_modules.end(), requested) !=
        g_flip_meter_modules.end()) {
      module = requested;
    } else if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                   reinterpret_cast<LPCWSTR>(requested), &module)) {
      temporary = true;
      if (module != requested) {
        FreeLibrary(module);
        module = nullptr;
        temporary = false;
      }
    }
  }
  FlipModuleReference(const FlipModuleReference&) = delete;
  FlipModuleReference& operator=(const FlipModuleReference&) = delete;
  ~FlipModuleReference() { if (temporary) FreeLibrary(module); }
  bool Keep() {
    if (!module) return false;
    if (!temporary) return true;
    // Allocation happens before any byte or protection is changed.
    try { g_flip_meter_modules.push_back(module); } catch (...) { return false; }
    temporary = false;
    return true;
  }
};

void RefreshFlipTransactionState() {
  const bool pending = std::any_of(g_flip_meter_sites.begin(), g_flip_meter_sites.end(),
      [](const auto& site) { return site.rollback_pending; });
  g_flip_meter_rollback_pending.store(pending, std::memory_order_release);
  g_flip_meter_patched.store(!pending && std::any_of(
      g_flip_meter_sites.begin(), g_flip_meter_sites.end(),
      [](const auto& site) { return site.committed; }), std::memory_order_release);
}

// Records the original bytes before writing, so the instruction can be put back
// exactly as it was. Patches here are either one byte (an immediate flipped in
// place) or seven (a whole store rewritten), never anything else.
bool WriteFlipSite(unsigned char* at, const unsigned char* bytes, size_t length) {
  if (!at || !bytes || (length != 1 && length != 7) ||
      g_flip_meter_rollback_pending.load(std::memory_order_acquire)) return false;
  FlipSite site = {};
  site.address = at;
  site.length = static_cast<unsigned char>(length);
  size_t offset = 0;
  while (offset < length) {
    MEMORY_BASIC_INFORMATION memory{};
    const auto current = reinterpret_cast<uintptr_t>(at) + offset;
    if (current < reinterpret_cast<uintptr_t>(at) ||
        !VirtualQuery(reinterpret_cast<void*>(current), &memory, sizeof(memory)) ||
        memory.State != MEM_COMMIT || memory.Type != MEM_IMAGE ||
        (memory.Protect & (PAGE_NOACCESS | PAGE_GUARD))) return false;
    const auto start = reinterpret_cast<uintptr_t>(memory.BaseAddress);
    if (start > current || current - start >= memory.RegionSize) return false;
    if (!site.module) site.module = static_cast<HMODULE>(memory.AllocationBase);
    if (memory.AllocationBase != site.module) return false;
    const size_t span = (std::min)(length - offset, memory.RegionSize - (current - start));
    site.protections[site.protection_count++] = {
        reinterpret_cast<unsigned char*>(current), static_cast<unsigned char>(span), 0};
    offset += span;
  }
  if (std::find(g_flip_meter_modules.begin(), g_flip_meter_modules.end(), site.module) ==
          g_flip_meter_modules.end() ||
      !mfgunlock::provider::internal::IsReadable(at, length, site.module)) return false;
  std::memcpy(site.original, at, length);
  std::memcpy(site.replacement, bytes, length);
  try { g_flip_meter_sites.push_back(site); } catch (...) { return false; }
  auto& undo = g_flip_meter_sites.back();
  bool writable = true;
  size_t opened = 0;
  for (auto& protection : undo.protections) {
    if (opened == undo.protection_count) break;
    DWORD old_protect = 0;
    if (!VirtualProtect(protection.address, protection.length, PAGE_EXECUTE_READWRITE,
                        &old_protect)) {
      writable = false;
      break;
    }
    protection.original = old_protect;
    ++opened;
  }
  if (opened == 0) {
    g_flip_meter_sites.pop_back();  // Nothing was changed; no undo obligation.
    return false;
  }
  if (writable) std::memcpy(at, undo.replacement, length);
  bool protected_again = true;
  for (size_t i = opened; i != 0; --i) {
    const auto& protection = undo.protections[i - 1];
    DWORD ignored = 0;
    if (!VirtualProtect(protection.address, protection.length, protection.original, &ignored))
      protected_again = false;
  }
  const bool flushed = !writable ||
      FlushInstructionCache(GetCurrentProcess(), at, length) != FALSE;
  const bool matches = writable && mfgunlock::provider::internal::IsReadable(at, length, undo.module) &&
      std::memcmp(at, undo.replacement, length) == 0;
  if (writable && protected_again && flushed && matches) return true;
  undo.rollback_pending = true;
  RefreshFlipTransactionState();
  return false;
}

bool RestoreFlipSite(FlipSite& site) {
  bool ok = true;
  for (size_t i = site.protection_count; i != 0; --i) {
    const auto& protection = site.protections[i - 1];
    if (protection.original == 0) continue;  // This region was never opened.
    if (!mfgunlock::provider::internal::IsReadable(protection.address, protection.length,
                                                 site.module)) {
      ok = false;
      continue;
    }
    DWORD ignored = 0;
    if (!VirtualProtect(protection.address, protection.length, PAGE_EXECUTE_READWRITE, &ignored)) {
      ok = false;
      continue;
    }
    const size_t offset = static_cast<size_t>(protection.address - site.address);
    std::memcpy(protection.address, site.original + offset, protection.length);
    if (!VirtualProtect(protection.address, protection.length, protection.original, &ignored))
      ok = false;
  }
  const bool flushed = FlushInstructionCache(GetCurrentProcess(), site.address, site.length) != FALSE;
  const bool matches = mfgunlock::provider::internal::IsReadable(site.address, site.length, site.module) &&
      std::memcmp(site.address, site.original, site.length) == 0;
  return ok && flushed && matches;
}

bool RestoreFlipSitesFrom(size_t first) {
  if (first > g_flip_meter_sites.size()) return false;
  bool ok = true;
  for (size_t i = g_flip_meter_sites.size(); i > first; --i) {
    auto& site = g_flip_meter_sites[i - 1];
    site.committed = false;
    site.rollback_pending = true;
    if (RestoreFlipSite(site))
      g_flip_meter_sites.erase(g_flip_meter_sites.begin() + static_cast<ptrdiff_t>(i - 1));
    else {
      ok = false;
      // Keep reverse undo order even if two selected instruction ranges touch.
      // An older snapshot must not be restored ahead of a failed newer one.
      break;
    }
  }
  RefreshFlipTransactionState();
  return ok;
}

bool ModuleImage(HMODULE mod, unsigned char** out_base, const IMAGE_NT_HEADERS64** out_nt) {
  if (!out_base || !out_nt) return false;
  *out_base = nullptr;
  *out_nt = nullptr;
  // Reuse the provider's bounded PE reader. Plugin scans must not trust an
  // e_lfanew, section count or virtual range before dereferencing that range.
  mfgunlock::provider::internal::Image image;
  if (!mfgunlock::provider::internal::IsImageMapping(mod) ||
      !mfgunlock::provider::internal::InspectImage(mod, image)) return false;
  for (const auto& section : image.sections) {
    if ((section.Characteristics & (IMAGE_SCN_MEM_READ | IMAGE_SCN_MEM_EXECUTE)) &&
        !mfgunlock::provider::internal::IsReadable(
            image.base + section.VirtualAddress, section.Misc.VirtualSize, mod)) return false;
  }
  const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(image.base);
  *out_base = image.base;
  *out_nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(image.base + dos->e_lfanew);
  return true;
}

// ------------------------------------------- Streamline frame ceiling
// Some wrappers cache a stale NGX device maximum and clamp their compiled frame
// ceiling down to it. Neutralize only that lowering CMOV while preserving the
// plugin's own compiled maximum.

std::atomic_bool g_ceiling_patched{false};
unsigned char* g_ceiling_site = nullptr;
std::atomic<HMODULE> g_ceiling_module{nullptr};
std::recursive_mutex g_ceiling_lock;
mfgunlock::hook::ModuleIdentity g_ceiling_identity{};
unsigned char g_ceiling_cmov_original = 0;
unsigned int g_ceiling_compiled = 0;

void RestoreFrameCountCeiling();

void ObserveFrameCountCeiling(HMODULE mod) {
  const std::lock_guard ceiling_guard(g_ceiling_lock);
  if (!mod) return;
  if (mod == g_ceiling_module && mfgunlock::hook::IsCurrent(g_ceiling_identity)) return;
  RestoreFrameCountCeiling();
  // The loader/public resolver selected a different current plugin. Do not
  // retain a ceiling or instruction address from an earlier local/OTA image.
  g_ceiling_site = nullptr;
  g_ceiling_module = mod;
  mfgunlock::hook::CaptureModule(mod, g_ceiling_identity);
  g_ceiling_compiled = 0;
  g_ceiling_patched.store(false, std::memory_order_release);
  mfgunlock::framecount::g_streamline_plugin_seen.store(true, std::memory_order_release);
  mfgunlock::framecount::g_streamline_max_generated.store(0, std::memory_order_release);

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

  g_ceiling_site = found;
  g_ceiling_module = mod;
  g_ceiling_cmov_original = found[9];
  g_ceiling_compiled = found[1];
  mfgunlock::framecount::g_streamline_max_generated.store(
      g_ceiling_compiled, std::memory_order_release);
  mfgunlock::framecount::g_capacity_advertised.store(false, std::memory_order_relaxed);
  mfgunlock::framecount::g_unknown_ceiling_logged.store(false, std::memory_order_relaxed);

  std::stringstream message;
  message << "mfgunlock: Streamline structural ceiling: " << g_ceiling_compiled
          << " generated frame(s).";
  reshade::log::message(reshade::log::level::info, message.str().c_str());
}

void ObserveDlssgFeatureEntry(void* entry) {
  static std::atomic_bool announced{false};
  if (!announced.exchange(true, std::memory_order_relaxed))
    mfgunlock::provider::Log("EMPIRICAL_COMPAT_R6 native-selected FG plugin; native operation trials");
  MEMORY_BASIC_INFORMATION memory{};
  if (!entry || !VirtualQuery(entry, &memory, sizeof(memory)) ||
      memory.State != MEM_COMMIT || memory.Type != MEM_IMAGE ||
      (memory.Protect & (PAGE_GUARD | PAGE_NOACCESS)) ||
      !(memory.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY))) return;
  const auto mod = static_cast<HMODULE>(memory.AllocationBase);
  const auto gateway = GetProcAddress(mod, "slGetPluginFunction");
  if (gateway) mfgunlock::streamline::internal::BindPlugin(
      mod, gateway, mfgunlock::streamline::internal::PluginRole::kFg);
  ObserveFrameCountCeiling(mod);
}

void PatchFrameCountCeiling(HMODULE mod) {
  const std::lock_guard ceiling_guard(g_ceiling_lock);
  ObserveFrameCountCeiling(mod);
  if (g_ceiling_patched.load(std::memory_order_acquire)) return;
  if (g_ceiling_site == nullptr) return;

  DWORD old_protect = 0;
  if (VirtualProtect(g_ceiling_site, 10, PAGE_EXECUTE_READWRITE, &old_protect) == 0) {
    reshade::log::message(
        reshade::log::level::warning,
        "mfgunlock: Streamline stale runtime ceiling lowering could not be neutralized.");
    return;
  }
  g_ceiling_site[9] = 0xD2;  // cmovb edx, ecx -> cmovb edx, edx
  DWORD ignored = 0;
  VirtualProtect(g_ceiling_site, 10, old_protect, &ignored);
  FlushInstructionCache(GetCurrentProcess(), g_ceiling_site, 10);
  g_ceiling_patched.store(true, std::memory_order_release);
  reshade::log::message(
      reshade::log::level::info,
      "mfgunlock: Streamline stale runtime ceiling lowering neutralized.");
}

void RestoreFrameCountCeiling() {
  const std::lock_guard ceiling_guard(g_ceiling_lock);
  if (g_ceiling_patched.load(std::memory_order_acquire) && g_ceiling_site != nullptr &&
      mfgunlock::hook::IsCurrent(g_ceiling_identity)) {
    DWORD old_protect = 0;
    if (VirtualProtect(g_ceiling_site, 10, PAGE_EXECUTE_READWRITE, &old_protect) != 0) {
      g_ceiling_site[9] = g_ceiling_cmov_original;
      DWORD ignored = 0;
      VirtualProtect(g_ceiling_site, 10, old_protect, &ignored);
      FlushInstructionCache(GetCurrentProcess(), g_ceiling_site, 10);
    }
  }
  g_ceiling_site = nullptr;
  g_ceiling_module = nullptr;
  g_ceiling_identity = {};
  g_ceiling_cmov_original = 0;
  g_ceiling_compiled = 0;
  mfgunlock::framecount::g_streamline_plugin_seen.store(false, std::memory_order_release);
  mfgunlock::framecount::g_streamline_max_generated.store(0, std::memory_order_release);
  mfgunlock::framecount::g_capacity_advertised.store(false, std::memory_order_relaxed);
  g_ceiling_patched.store(false, std::memory_order_release);
}

// Derive one unique fallback store. Multiple candidates, invalid C6 opcodes
// and SIB forms cannot establish the private field and are left untouched.
bool FindFlipMeteringFallback(const unsigned char* base, const IMAGE_NT_HEADERS64* nt,
                             const unsigned char* marker, unsigned int& field_out,
                             int& value_out) {
  field_out = 0;
  value_out = -1;
  size_t matches = 0;
  unsigned int field = 0;
  int value = -1;
  const auto* section = IMAGE_FIRST_SECTION(nt);
  for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++section) {
    if ((section->Characteristics & IMAGE_SCN_MEM_EXECUTE) == 0) continue;
    const auto* start = base + section->VirtualAddress;
    const size_t size = section->Misc.VirtualSize;
    for (size_t off = 0; off + 8 <= size; ++off) {
      if (!(start[off] == 0x48 || start[off] == 0x4C) || start[off + 1] != 0x8D ||
          (start[off + 2] & 0xC7) != 0x05) continue;
      int displacement = 0;
      std::memcpy(&displacement, start + off + 3, sizeof(displacement));
      // Integer comparison avoids forming an out-of-bounds C++ pointer for an
      // untrusted displacement. The candidate address is never dereferenced.
      const uintptr_t target = reinterpret_cast<uintptr_t>(start + off + 7) +
          static_cast<uintptr_t>(static_cast<intptr_t>(displacement));
      if (target != reinterpret_cast<uintptr_t>(marker)) continue;
      const size_t limit = (std::min)(off + size_t{0x200}, size);
      for (size_t w = off; w + 7 <= limit; ++w) {
        if (start[w] != 0xC6) continue;
        const unsigned char modrm = start[w + 1];
        if ((modrm & 0xF8) != 0x80 || (modrm & 7) == 4) continue;  // C6 /0, no SIB
        unsigned int candidate = 0;
        std::memcpy(&candidate, start + w + 2, sizeof(candidate));
        if (candidate <= 0x100 || candidate >= 0x20000 || start[w + 6] > 1) continue;
        ++matches;
        field = candidate;
        value = start[w + 6];
      }
    }
  }
  if (matches != 1) return false;
  field_out = field;
  value_out = value;
  return true;
}

// Handles the DLSS-G Streamline plugin wherever it was loaded from. Returns true
// once a module has been dealt with, so the caller stops scanning.
bool TryPatchFlipMeteringInModule(HMODULE mod, FlipPatchResult* result = nullptr) {
  if (result) *result = FlipPatchResult::kNotApplied;
  // Pending undo forbids another memory mutation, never the caller's native
  // SetOptions attempt. The pacing callback remains best effort.
  if (g_flip_meter_rollback_pending.load(std::memory_order_acquire)) {
    if (result) *result = FlipPatchResult::kRollbackIncomplete;
    return true;
  }
  FlipModuleReference reference(mod);
  if (!reference.module) return false;
  unsigned char* base = nullptr;
  const IMAGE_NT_HEADERS64* nt = nullptr;
  if (!ModuleImage(mod, &base, &nt)) return false;

  // 1. Is this the DLSS-G plugin? The marker string identifies it regardless of
  //    what the OTA layer decided to call the file.
  const size_t marker_len = sizeof(kFlipMarker) - 1;
  const unsigned char* marker = nullptr;
  size_t marker_matches = 0;
  const auto* section = IMAGE_FIRST_SECTION(nt);
  for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++section) {
    if ((section->Characteristics & IMAGE_SCN_MEM_READ) == 0) continue;
    unsigned char* start = base + section->VirtualAddress;
    const size_t size = section->Misc.VirtualSize;
    if (size < marker_len) continue;
    for (size_t off = 0; off + marker_len <= size; ++off) {
      if (std::memcmp(start + off, kFlipMarker, marker_len) == 0) {
        marker = start + off;
        ++marker_matches;
      }
    }
  }
  if (marker == nullptr) return false;
  if (marker_matches != 1) {
    mfgunlock::provider::Log("Optional flip-metering override: ambiguous marker; no patch applied");
    g_flip_meter_failed.store(true, std::memory_order_release);
    return true;
  }
  PatchFrameCountCeiling(mod);

  // 2. Find the code referencing it, then read the (offset, value) the fallback
  //    writes: C6 /r disp32 imm8 == mov byte ptr [reg+disp32], imm8.
  unsigned int want_offset = 0;
  int want_value = -1;
  FindFlipMeteringFallback(base, nt, marker, want_offset, want_value);

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

  const size_t sites_before = g_flip_meter_sites.size();
  bool writes_ok = true;
  // Pin the derived field to the fallback value. Handle the immediate store
  // directly; rewrite only the seven-byte REX register form whose instruction
  // boundary is identical to the replacement. Other encodings are left alone.
  const unsigned char opposite = static_cast<unsigned char>(1 - want_value);
  section = IMAGE_FIRST_SECTION(nt);
  for (WORD i = 0; i < nt->FileHeader.NumberOfSections && writes_ok; ++i, ++section) {
    if ((section->Characteristics & IMAGE_SCN_MEM_EXECUTE) == 0) continue;
    unsigned char* start = base + section->VirtualAddress;
    const size_t size = section->Misc.VirtualSize;
    if (size < 7) continue;
    for (size_t off = 0; off + 7 <= size; ++off) {
      // C6 /0 disp32 imm8 -- replace the immediate.
      if (start[off] == 0xC6) {
        if ((start[off + 1] & 0xF8) != 0x80 || (start[off + 1] & 7) == 4) continue;
        unsigned int field = 0;
        std::memcpy(&field, start + off + 2, sizeof(field));
        if (field != want_offset) continue;
        if (start[off + 6] != opposite) continue;
        const unsigned char imm = static_cast<unsigned char>(want_value);
        if (!reference.Keep() || !WriteFlipSite(start + off + 6, &imm, 1)) {
          writes_ok = false;
          break;
        }
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
      if (!reference.Keep() || !WriteFlipSite(start + off, replacement, sizeof(replacement))) {
        writes_ok = false;
        break;
      }
    }
  }

  char module_path[MAX_PATH] = {};
  GetModuleFileNameA(mod, module_path, MAX_PATH);

  if (!writes_ok) {
    const bool restored = RestoreFlipSitesFrom(sites_before);
    if (result) *result = restored ? FlipPatchResult::kNotApplied
                                   : FlipPatchResult::kRollbackIncomplete;
    g_flip_meter_failed.store(true, std::memory_order_release);
    mfgunlock::provider::Log(restored
        ? "flip-metering write transaction failed; rollback complete; native requests remain enabled"
        : "flip-metering rollback incomplete; undo and module retained; native requests remain enabled", true);
    return true;
  }

  // Deriving the field alone is not success; at least one write must change.
  if (g_flip_meter_sites.size() == sites_before) {
    std::stringstream s;
    s << "mfgunlock: flip-metering field +0x" << std::hex << want_offset << std::dec
      << " derived from " << module_path
      << ", but nothing writes it in a form this can patch -- no immediate store of "
      << (1 - want_value) << ", and no 7-byte register store. Nothing changed.";
    reshade::log::message(reshade::log::level::warning, s.str().c_str());
    g_flip_meter_failed.store(true, std::memory_order_release);
    return true;
  }

  for (size_t i = sites_before; i < g_flip_meter_sites.size(); ++i)
    g_flip_meter_sites[i].committed = true;
  if (result) *result = FlipPatchResult::kApplied;
  RefreshFlipTransactionState();
  g_flip_meter_failed.store(false, std::memory_order_release);

  std::stringstream s;
  s << "mfgunlock: forced flip-metering off in " << module_path << " -- field +0x" << std::hex
    << want_offset << std::dec << " pinned to " << want_value << " at "
    << (g_flip_meter_sites.size() - sites_before)
    << " site(s); native runtime will determine pacing behavior.";
  reshade::log::message(reshade::log::level::info, s.str().c_str());

  return true;
}

bool TryPatchFlipMeteringOnce(HMODULE mod, uint64_t epoch, uint64_t revision) {
  if (g_flip_meter_rollback_pending.load(std::memory_order_acquire)) return false;
  // The snapshot's HMODULE is not a loader reference. Hold it before reading
  // identity or scanning; successful journals retain a separate durable owner.
  FlipModuleReference reference(mod);
  if (!reference.module) return false;
  mfgunlock::provider::internal::ImageIdentity identity{};
  if (!mfgunlock::provider::internal::ReadIdentity(mod, identity)) return false;
  auto found = std::find_if(g_flip_meter_attempts.begin(), g_flip_meter_attempts.end(),
      [mod](const auto& attempt) { return attempt.module == mod; });
  if (found != g_flip_meter_attempts.end() && found->identity == identity &&
      (found->result == FlipPatchResult::kApplied ||
       (found->epoch == epoch && found->revision == revision)))
    return found->result == FlipPatchResult::kApplied;
  // Reserve the cache record before invoking the writer. Publishing success
  // must not allocate after executable bytes have changed.
  const size_t index = static_cast<size_t>(found - g_flip_meter_attempts.begin());
  try {
    if (found == g_flip_meter_attempts.end()) g_flip_meter_attempts.emplace_back();
  } catch (...) {
    g_flip_meter_failed.store(true, std::memory_order_release);
    return false;
  }
  FlipPatchResult result = FlipPatchResult::kNotApplied;
  TryPatchFlipMeteringInModule(mod, &result);
  g_flip_meter_attempts[index] = {mod, identity, epoch, revision, result};
  return result == FlipPatchResult::kApplied;
}

void TryPatchFlipMetering() {
  if (!g_enabled.load()) return;
  if (!TryAcquireSRWLockExclusive(&g_flip_meter_scan_lock)) return;
  struct Unlock { ~Unlock() { ReleaseSRWLockExclusive(&g_flip_meter_scan_lock); } } unlock;
  const auto epoch = mfgunlock::framecount::g_options_cache_epoch.load(std::memory_order_acquire);
  const auto revision = mfgunlock::framecount::g_options_revision.load(std::memory_order_acquire);

  HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, GetCurrentProcessId());
  if (snap == INVALID_HANDLE_VALUE) return;

  MODULEENTRY32W me = {};
  me.dwSize = sizeof(me);
  if (Module32FirstW(snap, &me)) {
    do {
      if (me.hModule == g_self_module) continue;
      TryPatchFlipMeteringOnce(me.hModule, epoch, revision);
    } while (Module32NextW(snap, &me));
  }
  CloseHandle(snap);
}

void RestoreFlipMetering() {
  AcquireSRWLockExclusive(&g_flip_meter_scan_lock);
  struct Unlock { ~Unlock() { ReleaseSRWLockExclusive(&g_flip_meter_scan_lock); } } unlock;
  const bool restored = RestoreFlipSitesFrom(0);
  g_flip_meter_failed.store(!restored, std::memory_order_release);
  if (restored) {
    g_flip_meter_attempts.clear();
  } else {
    // Do not preserve cached APPLIED after any of its stores were restored.
    for (auto& attempt : g_flip_meter_attempts) {
      const bool pending = std::any_of(g_flip_meter_sites.begin(), g_flip_meter_sites.end(),
          [&](const auto& site) { return site.module == attempt.module; });
      attempt.result = pending ? FlipPatchResult::kRollbackIncomplete : FlipPatchResult::kNotApplied;
    }
    mfgunlock::provider::Log(
        "flip-metering restore incomplete; undo and module retained for retry", true);
  }
}

bool DynamicRuntimeStackReady() {
  namespace fc = mfgunlock::framecount;
  mfgunlock::compatibility::DynamicEvidence e{};
  // These are our own buffers and current typed native entrypoints. Capability
  // reports, render API, retarget recipes and unrelated hooks are observations.
  e.live_set_options = fc::internal::RealSetOptions() != nullptr;
  e.live_get_state = fc::internal::RealGetState() != nullptr;
  e.options_v5 = sl::DLSSGOptions{}.structVersion >= sl::kStructVersion5;
  e.state_v4 = sl::DLSSGState{}.structVersion >= sl::kStructVersion4;
  e.api_support_observed = fc::g_dynamic_support_seen.load(std::memory_order_acquire);
  e.api_supports_dynamic = fc::g_dynamic_supported.load(std::memory_order_relaxed);
  const auto operation = mfgunlock::compatibility::DynamicStack(e);
  const bool ready = g_enabled.load(std::memory_order_acquire) && operation.Available();
  const bool previous = g_dynamic_stack_validated.exchange(ready, std::memory_order_acq_rel);
  const char* prior_reason = g_dynamic_last_reason.exchange(operation.reason, std::memory_order_acq_rel);
  if (previous != ready || prior_reason != operation.reason) {
    mfgunlock::provider::Log(std::string("R6 Dynamic native trial ") +
        (ready ? "callable: " : "waiting: ") + operation.reason);
  }
  return ready;
}

void OnDlssgPluginUnloaded(HMODULE module) {
  const std::lock_guard ceiling_guard(g_ceiling_lock);
  if (module != g_ceiling_module || mfgunlock::hook::IsCurrent(g_ceiling_identity)) return;
  mfgunlock::framecount::NotifyDlssgPluginUnloaded(module);
  g_dynamic_stack_validated.store(false, std::memory_order_release);
  g_ceiling_site = nullptr;
  g_ceiling_module = nullptr;
  g_ceiling_identity = {};
  g_ceiling_cmov_original = 0;
  g_ceiling_compiled = 0;
  g_ceiling_patched.store(false, std::memory_order_release);
}

// The periodic Present tick only checks small module identities. Full discovery
// and provider preparation run after a real change/loader notification, not on
// a timer. Both locks are try-only: rendering never waits for this liveness poll.
void PollRuntimeModuleChanges(bool remember_only) {
  static SRWLOCK poll_lock = SRWLOCK_INIT;
  static std::array<mfgunlock::hook::ModuleIdentity, 6> named{};
  static std::vector<mfgunlock::hook::ModuleIdentity> providers;
  static std::vector<mfgunlock::hook::ModuleIdentity> plugins;
  if (!TryAcquireSRWLockExclusive(&poll_lock)) return;
  struct Unlock {
    ~Unlock() { ReleaseSRWLockExclusive(&poll_lock); }
  } unlock;
  constexpr const wchar_t* names[] = {L"nvngx_dlssg.dll", L"sl.interposer.dll",
      L"sl.common.dll", L"sl.dlss_g.dll", L"_nvngx.dll", L"nvngx.dll"};
  for (size_t i = 0; i < named.size(); ++i) {
    mfgunlock::hook::ModuleIdentity now{};
    mfgunlock::hook::CaptureModule(GetModuleHandleW(names[i]), now);
    if (!remember_only && !(now == named[i])) {
      if (i == 0) g_provider_scan_pending.store(true, std::memory_order_release);
      else if (i < 4) g_streamline_scan_pending.store(true, std::memory_order_release);
      else g_ngx_scan_pending.store(true, std::memory_order_release);
    }
    named[i] = now;
  }
  if (remember_only) {
    if (TryAcquireSRWLockShared(&mfgunlock::streamline::internal::g_plugin_lock)) {
      plugins.clear();
      for (const auto& plugin : mfgunlock::streamline::internal::g_plugins)
        if (plugin.module) plugins.push_back(plugin.gateway_hook.identity);
      ReleaseSRWLockShared(&mfgunlock::streamline::internal::g_plugin_lock);
    }
    if (!TryAcquireSRWLockShared(&g_provider_maintenance_lock)) return;
    providers.clear();
    for (HMODULE module : g_dlssg_modules) {
      mfgunlock::hook::ModuleIdentity identity{};
      if (mfgunlock::hook::CaptureModule(module, identity)) providers.push_back(identity);
    }
    ReleaseSRWLockShared(&g_provider_maintenance_lock);
  } else {
    for (const auto& identity : plugins)
      if (!mfgunlock::hook::IsCurrent(identity))
        g_streamline_scan_pending.store(true, std::memory_order_release);
    // Also cover providers loaded under a renamed/cache path.
    for (const auto& identity : providers) {
      if (!mfgunlock::hook::IsCurrent(identity))
        g_provider_scan_pending.store(true, std::memory_order_release);
    }
  }
}

void RefreshRuntime() {
  TryAttachRuntime();
  if (!g_bootstrap_complete.load(std::memory_order_acquire)) return;
  g_streamline_scan_pending.store(false, std::memory_order_release);
  g_ngx_scan_pending.store(false, std::memory_order_release);
  mfgunlock::streamline::internal::RefreshPluginLifetimes();
  mfgunlock::ngx::BeforeInit();
  if (!mfgunlock::architecture::ActiveProfile()) RunProviderMaintenance();
  if (g_force_flip_meter_off.load(std::memory_order_relaxed)) TryPatchFlipMetering();
  mfgunlock::streamline::internal::InstallLegacyInitHook();
  mfgunlock::streamline::internal::InstallSupportHook();
  mfgunlock::streamline::internal::InstallDiagnosticHooks();
  mfgunlock::framecount::TryInstall();
  mfgunlock::loadhook::TryInstall();
  PollRuntimeModuleChanges(true);
}

bool UiCandidateObservationEnabled() {
  return mfgunlock::diagnostic::g_verbose.load(std::memory_order_relaxed) ||
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

mfgunlock::outputstate::FormatKind OutputFormatKind(reshade::api::format format) {
  const uint32_t raw = static_cast<uint32_t>(format);
  return mfgunlock::outputstate::ClassifyFormat(raw, raw != 0);
}

mfgunlock::outputstate::UiEncoding DetectedUiEncoding(
    uint32_t view_format) {
  return mfgunlock::outputstate::DetectedUiEncoding(
      OutputFormatKind(static_cast<reshade::api::format>(view_format)));
}

mfgunlock::uialpha::BlendFactor UiBlendFactor(reshade::api::blend_factor factor) {
  using Result = mfgunlock::uialpha::BlendFactor;
  switch (factor) {
    case reshade::api::blend_factor::zero: return Result::kZero;
    case reshade::api::blend_factor::one: return Result::kOne;
    case reshade::api::blend_factor::source_alpha: return Result::kSourceAlpha;
    case reshade::api::blend_factor::one_minus_source_alpha:
      return Result::kOneMinusSourceAlpha;
    default: return Result::kUnknown;
  }
}

mfgunlock::uialpha::BlendOp UiBlendOp(reshade::api::blend_op operation) {
  return operation == reshade::api::blend_op::add
      ? mfgunlock::uialpha::BlendOp::kAdd
      : mfgunlock::uialpha::BlendOp::kUnknown;
}

mfgunlock::uialpha::BlendState UiBlendState(
    const reshade::api::blend_desc& desc, uint32_t target) {
  mfgunlock::uialpha::BlendState state{};
  if (target >= kMaxUiRenderTargets) return state;
  state.alpha_to_coverage = desc.alpha_to_coverage_enable;
  state.blend_enabled = desc.blend_enable[target];
  state.logic_op_enabled = desc.logic_op_enable[target];
  state.source_color = UiBlendFactor(desc.source_color_blend_factor[target]);
  state.dest_color = UiBlendFactor(desc.dest_color_blend_factor[target]);
  state.color_op = UiBlendOp(desc.color_blend_op[target]);
  state.source_alpha = UiBlendFactor(desc.source_alpha_blend_factor[target]);
  state.dest_alpha = UiBlendFactor(desc.dest_alpha_blend_factor[target]);
  state.alpha_op = UiBlendOp(desc.alpha_blend_op[target]);
  state.write_mask = desc.render_target_write_mask[target];
  return state;
}

UiBlendPipelineState* FindUiBlendPipelineLocked(uint64_t pipeline) {
  UiBlendPipelineState* empty = nullptr;
  for (auto& state : g_ui_blend_pipelines) {
    if (state.used && state.pipeline == pipeline) return &state;
    if (!state.used && empty == nullptr) empty = &state;
  }
  return empty;
}

UiCommandRenderState* FindUiCommandStateLocked(
    reshade::api::command_list* command_list) {
  UiCommandRenderState* empty = nullptr;
  for (auto& state : g_ui_command_states) {
    if (state.used && state.command_list == command_list) return &state;
    if (!state.used && empty == nullptr) empty = &state;
  }
  return empty;
}

void OnInitUiBlendPipeline(reshade::api::device* device,
                           reshade::api::pipeline_layout,
                           uint32_t subobject_count,
                           const reshade::api::pipeline_subobject* subobjects,
                           reshade::api::pipeline pipeline) {
  if (device == nullptr || device->get_api() != reshade::api::device_api::d3d12 ||
      pipeline.handle == 0 || subobjects == nullptr) {
    return;
  }

  const reshade::api::blend_desc* blend = nullptr;
  for (uint32_t index = 0; index < subobject_count; ++index) {
    if (subobjects[index].type != reshade::api::pipeline_subobject_type::blend_state ||
        subobjects[index].count == 0 || subobjects[index].data == nullptr) {
      continue;
    }
    blend = static_cast<const reshade::api::blend_desc*>(subobjects[index].data);
    break;
  }
  if (blend == nullptr) return;

  AcquireSRWLockExclusive(&g_ui_blend_lock);
  UiBlendPipelineState* state = FindUiBlendPipelineLocked(pipeline.handle);
  if (state != nullptr) {
    *state = {};
    state->used = true;
    state->pipeline = pipeline.handle;
    for (uint32_t target = 0; target < kMaxUiRenderTargets; ++target)
      state->targets[target] = UiBlendState(*blend, target);
  }
  ReleaseSRWLockExclusive(&g_ui_blend_lock);
}

void OnDestroyUiBlendPipeline(reshade::api::device* device,
                              reshade::api::pipeline pipeline) {
  if (device == nullptr || device->get_api() != reshade::api::device_api::d3d12 ||
      pipeline.handle == 0) {
    return;
  }
  AcquireSRWLockExclusive(&g_ui_blend_lock);
  for (auto& state : g_ui_blend_pipelines) {
    if (state.used && state.pipeline == pipeline.handle) state = {};
  }
  for (auto& state : g_ui_command_states) {
    if (state.used && state.pipeline == pipeline.handle) state.pipeline = 0;
  }
  ReleaseSRWLockExclusive(&g_ui_blend_lock);
}

void OnInitUiCommandList(reshade::api::command_list* command_list) {
  if (command_list == nullptr || command_list->get_device() == nullptr ||
      command_list->get_device()->get_api() != reshade::api::device_api::d3d12) {
    return;
  }
  AcquireSRWLockExclusive(&g_ui_blend_lock);
  UiCommandRenderState* state = FindUiCommandStateLocked(command_list);
  if (state != nullptr) {
    *state = {};
    state->used = true;
    state->command_list = command_list;
  }
  ReleaseSRWLockExclusive(&g_ui_blend_lock);
}

void OnDestroyUiCommandList(reshade::api::command_list* command_list) {
  if (command_list == nullptr) return;
  AcquireSRWLockExclusive(&g_ui_blend_lock);
  for (auto& state : g_ui_command_states) {
    if (state.used && state.command_list == command_list) state = {};
  }
  ReleaseSRWLockExclusive(&g_ui_blend_lock);
}

void OnResetUiCommandList(reshade::api::command_list* command_list) {
  if (command_list == nullptr) return;
  AcquireSRWLockExclusive(&g_ui_blend_lock);
  for (auto& state : g_ui_command_states) {
    if (!state.used || state.command_list != command_list) continue;
    auto* preserved = state.command_list;
    state = {};
    state.used = true;
    state.command_list = preserved;
    break;
  }
  ReleaseSRWLockExclusive(&g_ui_blend_lock);
}

void OnBindUiBlendPipeline(reshade::api::command_list* command_list,
                           reshade::api::pipeline_stage stages,
                           reshade::api::pipeline pipeline) {
  if (command_list == nullptr || command_list->get_device() == nullptr ||
      command_list->get_device()->get_api() != reshade::api::device_api::d3d12) {
    return;
  }
  const uint32_t stage_bits = static_cast<uint32_t>(stages);
  const uint32_t graphics_bits =
      static_cast<uint32_t>(reshade::api::pipeline_stage::all_graphics);
  if ((stage_bits & graphics_bits) == 0) return;

  AcquireSRWLockExclusive(&g_ui_blend_lock);
  UiCommandRenderState* state = FindUiCommandStateLocked(command_list);
  if (state != nullptr) {
    if (!state->used) {
      state->used = true;
      state->command_list = command_list;
    }
    state->pipeline = pipeline.handle;
  }
  ReleaseSRWLockExclusive(&g_ui_blend_lock);
}

void TrackUiRenderTargets(reshade::api::command_list* command_list,
                          uint32_t count,
                          const reshade::api::resource_view* rtvs) {
  if (command_list == nullptr || command_list->get_device() == nullptr ||
      command_list->get_device()->get_api() != reshade::api::device_api::d3d12) {
    return;
  }
  auto* device = command_list->get_device();
  std::array<uint64_t, kMaxUiRenderTargets> resources{};
  const uint32_t tracked = (std::min)(count, kMaxUiRenderTargets);
  if (rtvs != nullptr) {
    for (uint32_t index = 0; index < tracked; ++index)
      resources[index] = device->get_resource_from_view(rtvs[index]).handle;
  }

  AcquireSRWLockExclusive(&g_ui_blend_lock);
  UiCommandRenderState* state = FindUiCommandStateLocked(command_list);
  if (state != nullptr) {
    if (!state->used) {
      state->used = true;
      state->command_list = command_list;
    }
    state->render_targets = resources;
    state->render_target_count = tracked;
  }
  ReleaseSRWLockExclusive(&g_ui_blend_lock);
}

void ObserveUiCandidateDraw(reshade::api::command_list* command_list) {
  if (!UiCandidateObservationEnabled() || command_list == nullptr) return;

  struct DrawTarget {
    uint64_t resource = 0;
    bool blend_known = false;
    mfgunlock::uialpha::BlendState blend{};
  };
  std::array<DrawTarget, kMaxUiRenderTargets> targets{};
  uint32_t target_count = 0;

  AcquireSRWLockShared(&g_ui_blend_lock);
  UiCommandRenderState* command = nullptr;
  for (auto& state : g_ui_command_states) {
    if (state.used && state.command_list == command_list) {
      command = &state;
      break;
    }
  }
  const UiBlendPipelineState* pipeline = nullptr;
  if (command != nullptr && command->pipeline != 0) {
    for (const auto& state : g_ui_blend_pipelines) {
      if (state.used && state.pipeline == command->pipeline) {
        pipeline = &state;
        break;
      }
    }
  }
  if (command != nullptr) {
    target_count = command->render_target_count;
    for (uint32_t index = 0; index < target_count; ++index) {
      targets[index].resource = command->render_targets[index];
      if (pipeline != nullptr) {
        targets[index].blend = pipeline->targets[index];
        targets[index].blend_known = true;
      }
    }
  }
  ReleaseSRWLockShared(&g_ui_blend_lock);
  if (target_count == 0) return;

  AcquireSRWLockExclusive(&g_ui_candidate_lock);
  for (uint32_t index = 0; index < target_count; ++index) {
    if (targets[index].resource == 0) continue;
    UiRuntimeCandidate* candidate = FindUiRuntimeCandidateLocked(targets[index].resource);
    if (candidate == nullptr || !candidate->used || candidate->clear_serial == 0 ||
        !candidate->last_clear_transparent) {
      continue;
    }
    mfgunlock::uialpha::ObserveDraw(
        candidate->alpha_evidence,
        targets[index].blend_known ? &targets[index].blend : nullptr);
    candidate->last_draw_command_list = command_list;
    candidate->last_draw_queue = nullptr;
    candidate->last_draw_submission_thread = 0;
    candidate->last_draw_submitted = false;
  }
  ReleaseSRWLockExclusive(&g_ui_candidate_lock);
}

bool OnDrawUiCandidate(reshade::api::command_list* command_list, uint32_t,
                       uint32_t, uint32_t, uint32_t) {
  ObserveUiCandidateDraw(command_list);
  return false;
}

bool OnDrawIndexedUiCandidate(reshade::api::command_list* command_list,
                              uint32_t, uint32_t, uint32_t, int32_t, uint32_t) {
  ObserveUiCandidateDraw(command_list);
  return false;
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
  mfgunlock::framecount::internal::NotifyUiCandidateUnavailable();
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
  mfgunlock::framecount::internal::NotifyUiCandidateUnavailable();
}

void ClearUiBlendTracking() {
  AcquireSRWLockExclusive(&g_ui_blend_lock);
  g_ui_blend_pipelines = {};
  g_ui_command_states = {};
  ReleaseSRWLockExclusive(&g_ui_blend_lock);
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
    if (++g_ui_clear_sequence == 0) ++g_ui_clear_sequence;
    candidate->clear_serial = g_ui_clear_sequence;
    candidate->rtv_binds_since_clear = 0;
    candidate->current_late_writes = 0;
    candidate->alpha_evidence = {};
    candidate->last_draw_command_list = nullptr;
    candidate->last_draw_queue = nullptr;
    candidate->last_draw_submission_thread = 0;
    candidate->last_draw_submitted = false;
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
  TrackUiRenderTargets(cmd_list, count, rtvs);
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

void OnUiCandidateBarrier(reshade::api::command_list* command_list, uint32_t count,
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
    if (command_list != nullptr) {
      // A post-UI barrier is still a use of the source resource. Require the
      // command list carrying the latest observed state to be submitted before
      // the extraction queue is allowed to read it.
      candidate->last_draw_command_list = command_list;
      candidate->last_draw_queue = nullptr;
      candidate->last_draw_submission_thread = 0;
      candidate->last_draw_submitted = false;
    }
  }
  ReleaseSRWLockExclusive(&g_ui_candidate_lock);
}

void OnDestroyUiCandidateResource(reshade::api::device* device,
                                  reshade::api::resource resource) {
  if (device == g_primary_device.load(std::memory_order_acquire) && resource.handle != 0) {
    mfgunlock::integration::InvalidateResource(resource.handle);
    mfgunlock::composition::InvalidateResource(resource.handle);
  }
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
  InvalidateUiAlphaSource(resource.handle);
  if (selected_destroyed) {
    mfgunlock::framecount::internal::NotifyUiCandidateUnavailable();
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
    const auto& resource = *tags[index].resource;
    if (tags[index].structType != sl::ResourceTag::s_structType ||
        tags[index].structVersion != sl::kStructVersion1 ||
        resource.structType != sl::Resource::s_structType ||
        resource.structVersion != sl::kStructVersion1) continue;
    const uint64_t matched = reinterpret_cast<uint64_t>(resource.native);
    if (!matched) continue;
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
    mfgunlock::framecount::internal::NotifyUiCandidateUnavailable();
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
  snapshot->output_revision = mfgunlock::outputstate::Read().revision;
  snapshot->width = candidate.width;
  snapshot->height = candidate.height;
  snapshot->format = candidate.format;
  snapshot->ui_encoding = static_cast<uint32_t>(
      DetectedUiEncoding(candidate.view_format));
  snapshot->state = candidate.state;
  snapshot->state_known = candidate.state_known;
  snapshot->stable_frames = g_ui_selected_stable_frames;
  snapshot->rtv_binds_after_clear = candidate.rtv_binds_since_clear;
  snapshot->reject_reasons = mfgunlock::uicandidate::kAccept;
  snapshot->late_writes = candidate.last_late_writes;
  snapshot->alpha_convention = static_cast<uint32_t>(
      mfgunlock::uialpha::Resolve(candidate.alpha_evidence));
  snapshot->alpha_draws = candidate.alpha_evidence.draw_count;
  snapshot->alpha_unproven_draws = candidate.alpha_evidence.unproven_draws;
  snapshot->clear_serial = candidate.clear_serial;
  snapshot->submission_queue = reinterpret_cast<uint64_t>(candidate.last_draw_queue);
  snapshot->submission_thread = candidate.last_draw_submission_thread;
  snapshot->source_submitted = candidate.last_draw_submitted;
  snapshot->recent = true;
  ReleaseSRWLockExclusive(&g_ui_candidate_lock);
  return true;
}

bool AcquireUiCandidate(const mfgunlock::framecount::UiCandidateSnapshot* snapshot) {
  if (snapshot == nullptr || snapshot->native_resource == 0 ||
      snapshot->output_revision != mfgunlock::outputstate::Read().revision) return false;
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
    const auto alpha_convention = mfgunlock::uialpha::Resolve(candidate.alpha_evidence);
    if (static_cast<uint32_t>(alpha_convention) != snapshot->alpha_convention) {
      continue;
    }
    reference = reinterpret_cast<IUnknown*>(
        static_cast<uintptr_t>(candidate.resource.handle));
    reference->AddRef();
    break;
  }
  ReleaseSRWLockShared(&g_ui_candidate_lock);
  return reference != nullptr;
}

void MarkUiCandidateInjected(
    const mfgunlock::framecount::UiCandidateSnapshot* snapshot) {
  if (snapshot == nullptr || snapshot->native_resource == 0) return;
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

void ReleaseUiCandidate(const mfgunlock::framecount::UiCandidateSnapshot* snapshot,
                        bool injected, sl::Result result) {
  if (snapshot == nullptr || snapshot->native_resource == 0) return;
  if (injected && result == sl::Result::eOk) MarkUiCandidateInjected(snapshot);
  reinterpret_cast<IUnknown*>(static_cast<uintptr_t>(snapshot->native_resource))->Release();
}


#if defined(MFGUNLOCK_RESHADE_API) && MFGUNLOCK_RESHADE_API >= 18
reshade::api::resource_usage UiResourceUsage(uint32_t state) {
  return state == 0 ? reshade::api::resource_usage::general
                    : static_cast<reshade::api::resource_usage>(state);
}

void DestroyUiAlphaSourceLocked(UiAlphaFrame& frame) {
  // Only called on a completed slot or after teardown has quiesced the queue.
  // Detach before Release: the last COM reference may re-enter destroy_resource.
  const auto srv = frame.source_srv;
  auto* reference = frame.source_reference;
  frame.source_srv = {};
  frame.source_resource = {};
  frame.source_reference = nullptr;
  frame.source_format = 0;
  UiAlphaInternalScope scope;
  if (g_ui_alpha_backend.device && srv.handle != 0)
    g_ui_alpha_backend.device->destroy_resource_view(srv);
  if (reference) reference->Release();
}

void DestroyUiAlphaFrameLocked(UiAlphaFrame& frame) {
  DestroyUiAlphaSourceLocked(frame);
  UiAlphaInternalScope scope;
  auto* device = g_ui_alpha_backend.device;
  if (device) {
    if (frame.alpha_uav.handle) device->destroy_resource_view(frame.alpha_uav);
    if (frame.alpha_resource.handle) device->destroy_resource(frame.alpha_resource);
  }
  const uint64_t generation = frame.state.generation + 1;
  frame = {};
  frame.state.generation = generation == 0 ? 1 : generation;
}

void ReclaimUiAlphaSlotsLocked() {
  auto* device = g_ui_alpha_backend.device;
  if (!device || !g_ui_alpha_backend.retirement_fence.handle) return;
  const uint64_t completed = device->get_completed_fence_value(g_ui_alpha_backend.retirement_fence);
  for (auto& frame : g_ui_alpha_backend.slots)
    mfgunlock::uialphabackend::Reclaim(frame.state, completed);
}

void DestroyUiAlphaBackendLocked(bool wait_for_gpu) {
  // No waits on the frame path. Device teardown is the only quiescence wait.
  if (wait_for_gpu && g_ui_alpha_backend.queue)
    g_ui_alpha_backend.queue->wait_idle();
  auto* device = g_ui_alpha_backend.device;
  if (device) {
    for (auto& frame : g_ui_alpha_backend.slots) DestroyUiAlphaFrameLocked(frame);
    if (g_ui_alpha_backend.pipeline.handle) device->destroy_pipeline(g_ui_alpha_backend.pipeline);
    if (g_ui_alpha_backend.layout.handle) device->destroy_pipeline_layout(g_ui_alpha_backend.layout);
    if (g_ui_alpha_backend.retirement_fence.handle)
      device->destroy_fence(g_ui_alpha_backend.retirement_fence);
  }
  g_ui_alpha_backend = {};
}

void DestroyUiAlphaBackend(reshade::api::device* device, bool wait_for_gpu) {
  AcquireSRWLockExclusive(&g_ui_alpha_backend_lock);
  if (!device || g_ui_alpha_backend.device == device)
    DestroyUiAlphaBackendLocked(wait_for_gpu);
  ReleaseSRWLockExclusive(&g_ui_alpha_backend_lock);
}

void InvalidateUiAlphaSource(uint64_t native_resource) {
  if (!native_resource || g_ui_alpha_internal_work) return;
  AcquireSRWLockExclusive(&g_ui_alpha_backend_lock);
  for (auto& frame : g_ui_alpha_backend.slots) {
    if (frame.source_resource.handle == native_resource)
      mfgunlock::uialphabackend::Invalidate(frame.state);
  }
  // Invalidating evidence does not release pending GPU resources.
  ReleaseSRWLockExclusive(&g_ui_alpha_backend_lock);
}

void InvalidateUiAlphaPending() {
  if (g_ui_alpha_internal_work) return;
  AcquireSRWLockExclusive(&g_ui_alpha_backend_lock);
  for (auto& frame : g_ui_alpha_backend.slots)
    mfgunlock::uialphabackend::Invalidate(frame.state);
  ReleaseSRWLockExclusive(&g_ui_alpha_backend_lock);
}

bool EnsureUiAlphaPipelineLocked(reshade::api::device* device) {
  if (device == nullptr || device->get_api() != reshade::api::device_api::d3d12 ||
      !device->check_capability(reshade::api::device_caps::compute_shader) ||
      !device->check_format_support(
          reshade::api::format::r8_unorm,
          reshade::api::resource_usage::unordered_access |
              reshade::api::resource_usage::shader_resource)) {
    return false;
  }
  if (g_ui_alpha_backend.pipeline.handle != 0 &&
      g_ui_alpha_backend.layout.handle != 0 &&
      g_ui_alpha_backend.retirement_fence.handle != 0) {
    return g_ui_alpha_backend.device == device;
  }
  if (g_ui_alpha_backend.device != nullptr && g_ui_alpha_backend.device != device)
    return false;
  g_ui_alpha_backend.device = device;
  if (!device->create_fence(0, reshade::api::fence_flags::none,
                            &g_ui_alpha_backend.retirement_fence)) {
    g_ui_alpha_backend.device = nullptr;
    return false;
  }

  reshade::api::descriptor_range source_range{};
  source_range.binding = 0;
  source_range.dx_register_index = 0;
  source_range.dx_register_space = 0;
  source_range.count = 1;
  source_range.visibility = reshade::api::shader_stage::compute;
  source_range.array_size = 1;
  source_range.type = reshade::api::descriptor_type::shader_resource_view;
  reshade::api::descriptor_range alpha_range{};
  alpha_range.binding = 0;
  alpha_range.dx_register_index = 0;
  alpha_range.dx_register_space = 0;
  alpha_range.count = 1;
  alpha_range.visibility = reshade::api::shader_stage::compute;
  alpha_range.array_size = 1;
  alpha_range.type = reshade::api::descriptor_type::unordered_access_view;
  const reshade::api::pipeline_layout_param params[] = {
      reshade::api::pipeline_layout_param(source_range),
      reshade::api::pipeline_layout_param(alpha_range)};
  if (!device->create_pipeline_layout(2, params, &g_ui_alpha_backend.layout))
  {
    device->destroy_fence(g_ui_alpha_backend.retirement_fence);
    g_ui_alpha_backend.retirement_fence = {};
    g_ui_alpha_backend.device = nullptr;
    return false;
  }

  reshade::api::shader_desc compute{};
  compute.code = __ui_alpha_extract.data();
  compute.code_size = __ui_alpha_extract.size();
  reshade::api::pipeline_subobject shader{};
  shader.type = reshade::api::pipeline_subobject_type::compute_shader;
  shader.count = 1;
  shader.data = &compute;
  if (!device->create_pipeline(g_ui_alpha_backend.layout, 1, &shader,
                               &g_ui_alpha_backend.pipeline)) {
    device->destroy_pipeline_layout(g_ui_alpha_backend.layout);
    g_ui_alpha_backend.layout = {};
    device->destroy_fence(g_ui_alpha_backend.retirement_fence);
    g_ui_alpha_backend.retirement_fence = {};
    g_ui_alpha_backend.device = nullptr;
    return false;
  }
  return true;
}

bool EnsureUiAlphaSourceLocked(
    const mfgunlock::framecount::UiCandidateSnapshot* candidate) {
  if (!candidate || !candidate->native_resource || !g_ui_alpha_backend.device) return false;
  auto& frame = ActiveUiAlphaFrame();
  if (frame.source_resource.handle == candidate->native_resource &&
      frame.source_srv.handle && frame.source_format == candidate->format) {
    // Revalidate the fresh clear/generation even when reusing an SRV.
    if (!AcquireUiCandidate(candidate)) return false;
    ReleaseUiCandidate(candidate, false, sl::Result::eOk);
    return true;
  }
  DestroyUiAlphaSourceLocked(frame);
  if (!AcquireUiCandidate(candidate)) return false;
  reshade::api::resource resource{candidate->native_resource};
  const auto desc = g_ui_alpha_backend.device->get_resource_desc(resource);
  const auto format = static_cast<reshade::api::format>(candidate->format);
  const bool valid = desc.type == reshade::api::resource_type::texture_2d &&
      desc.texture.width == candidate->width && desc.texture.height == candidate->height &&
      desc.texture.depth_or_layers == 1 && desc.texture.levels == 1 &&
      desc.texture.samples == 1 && desc.texture.format == format &&
      (format == reshade::api::format::r8g8b8a8_unorm ||
       format == reshade::api::format::r8g8b8a8_unorm_srgb);
  if (!valid || !g_ui_alpha_backend.device->create_resource_view(
          resource, reshade::api::resource_usage::shader_resource,
          reshade::api::resource_view_desc(format), &frame.source_srv)) {
    ReleaseUiCandidate(candidate, false, sl::Result::eOk);
    return false;
  }
  frame.source_resource = resource;
  frame.source_reference = reinterpret_cast<IUnknown*>(static_cast<uintptr_t>(candidate->native_resource));
  frame.source_format = candidate->format;
  return true;
}

bool EnsureUiAlphaOutputLocked(uint32_t width, uint32_t height) {
  if (!g_ui_alpha_backend.device || !width || !height || width > 16384 || height > 16384)
    return false;
  auto& frame = ActiveUiAlphaFrame();
  if (frame.alpha_resource.handle && frame.width == width && frame.height == height) return true;
  uint64_t bytes = uint64_t{width} * height;
  for (const auto& slot : g_ui_alpha_backend.slots)
    if (&slot != &frame) bytes += uint64_t{slot.width} * slot.height;
  if (bytes > kUiAlphaBudget) return false;
  auto* device = g_ui_alpha_backend.device;
  UiAlphaInternalScope scope;
  if (frame.alpha_uav.handle) device->destroy_resource_view(frame.alpha_uav);
  if (frame.alpha_resource.handle) device->destroy_resource(frame.alpha_resource);
  frame.alpha_uav = {}; frame.alpha_resource = {}; frame.width = frame.height = 0;
  const auto usage = reshade::api::resource_usage::unordered_access |
      reshade::api::resource_usage::shader_resource;
  const reshade::api::resource_desc desc(width, height, 1, 1,
      reshade::api::format::r8_unorm, 1, reshade::api::memory_heap::gpu_only, usage);
  if (!device->create_resource(desc, nullptr, reshade::api::resource_usage::unordered_access,
                               &frame.alpha_resource)) return false;
  if (!device->create_resource_view(frame.alpha_resource,
          reshade::api::resource_usage::unordered_access,
          reshade::api::resource_view_desc(reshade::api::format::r8_unorm), &frame.alpha_uav)) {
    device->destroy_resource(frame.alpha_resource); frame.alpha_resource = {}; return false;
  }
  device->set_resource_name(frame.alpha_resource, "MFG Unlock UI Alpha Copy Staging");
  frame.width = width; frame.height = height;
  return true;
}

bool DispatchUiAlphaExtractionLocked(
    reshade::api::command_queue* queue,
    const mfgunlock::framecount::UiCandidateSnapshot* candidate,
    sl::CommandBuffer** copy_commands) {
  if (queue == nullptr || candidate == nullptr || copy_commands == nullptr ||
      queue->get_device() != g_ui_alpha_backend.device ||
      ActiveUiAlphaFrame().source_resource.handle == 0 ||
      ActiveUiAlphaFrame().source_srv.handle == 0 ||
      ActiveUiAlphaFrame().alpha_resource.handle == 0 ||
      ActiveUiAlphaFrame().alpha_uav.handle == 0 ||
      g_ui_alpha_backend.pipeline.handle == 0 ||
      g_ui_alpha_backend.layout.handle == 0) {
    return false;
  }
  const uint32_t queue_type = static_cast<uint32_t>(queue->get_type());
  if ((queue_type & static_cast<uint32_t>(reshade::api::command_queue_type::graphics)) == 0)
    return false;
  auto* commands = queue->get_immediate_command_list();
  if (commands == nullptr || commands->get_native() == 0) return false;
  *copy_commands = reinterpret_cast<sl::CommandBuffer*>(commands->get_native());

  const auto source_state = UiResourceUsage(candidate->state);
  const auto shader_state = reshade::api::resource_usage::shader_resource_non_pixel;
  if (source_state != shader_state)
    commands->barrier(ActiveUiAlphaFrame().source_resource, source_state, shader_state);
  commands->barrier(ActiveUiAlphaFrame().alpha_resource,
                    reshade::api::resource_usage::unordered_access,
                    reshade::api::resource_usage::unordered_access);
  commands->bind_pipeline(reshade::api::pipeline_stage::compute_shader,
                          g_ui_alpha_backend.pipeline);

  reshade::api::descriptor_table_update source_update{};
  source_update.binding = 0;
  source_update.array_offset = 0;
  source_update.count = 1;
  source_update.type = reshade::api::descriptor_type::shader_resource_view;
  source_update.descriptors = &ActiveUiAlphaFrame().source_srv;
  commands->push_descriptors(reshade::api::shader_stage::compute,
                             g_ui_alpha_backend.layout, 0, source_update);
  reshade::api::descriptor_table_update alpha_update{};
  alpha_update.binding = 0;
  alpha_update.array_offset = 0;
  alpha_update.count = 1;
  alpha_update.type = reshade::api::descriptor_type::unordered_access_view;
  alpha_update.descriptors = &ActiveUiAlphaFrame().alpha_uav;
  commands->push_descriptors(reshade::api::shader_stage::compute,
                             g_ui_alpha_backend.layout, 1, alpha_update);
  commands->dispatch((candidate->width + 7) / 8,
                     (candidate->height + 7) / 8, 1);
  commands->barrier(ActiveUiAlphaFrame().alpha_resource,
                    reshade::api::resource_usage::unordered_access,
                    reshade::api::resource_usage::unordered_access);
  if (source_state != shader_state)
    commands->barrier(ActiveUiAlphaFrame().source_resource, shader_state, source_state);
  // The SL eOnlyValidNow copy is appended to THIS list before its flush.
  return true;
}

bool PrepareUiAlphaCandidate(
    const mfgunlock::framecount::UiCandidateSnapshot* candidate,
    sl::CommandBuffer* command_buffer,
    mfgunlock::framecount::UiAlphaCandidateSnapshot* output) {
  if (!candidate || !command_buffer || !output) return false;
  if (!mfgunlock::uialphabackend::CanPrepare(candidate->native_resource,
          candidate->submission_queue, candidate->width, candidate->height, candidate->format,
          candidate->state_known, candidate->source_submitted,
          candidate->submission_thread, GetCurrentThreadId())) return false;
  auto* queue = reinterpret_cast<reshade::api::command_queue*>(
      static_cast<uintptr_t>(candidate->submission_queue));
  if (!queue || queue != g_primary_present_queue.load(std::memory_order_acquire) ||
      queue->get_device() != g_primary_device.load(std::memory_order_acquire)) return false;

  bool ready = false;
  AcquireSRWLockExclusive(&g_ui_alpha_backend_lock);
  UiAlphaInternalScope internal;
  ReclaimUiAlphaSlotsLocked();
  // A queue change cannot advance the same fence timeline while old work is pending.
  bool idle = true;
  for (const auto& frame : g_ui_alpha_backend.slots)
    idle = idle && !frame.state.gpu_pending && !frame.state.pending.prepared && !frame.state.poisoned;
  if (g_ui_alpha_backend.active_slot == UINT32_MAX &&
      (g_ui_alpha_backend.queue == queue || idle) &&
      g_ui_alpha_ticket_sequence < UINT64_MAX - 2 &&
      EnsureUiAlphaPipelineLocked(queue->get_device())) {
    g_ui_alpha_backend.queue = queue;
    for (uint32_t index = 0; index < kUiAlphaSlots; ++index) {
      auto& frame = g_ui_alpha_backend.slots[index];
      if (frame.state.gpu_pending || frame.state.poisoned || frame.state.pending.prepared) continue;
      g_ui_alpha_backend.active_slot = index;
      if (EnsureUiAlphaSourceLocked(candidate) &&
          EnsureUiAlphaOutputLocked(candidate->width, candidate->height)) {
        frame.state.generation = ++g_ui_alpha_ticket_sequence;
        const bool prepared = mfgunlock::uialphabackend::Prepare(frame.state,
            candidate->native_resource, candidate->submission_queue, candidate->width,
            candidate->height, candidate->format, candidate->state, candidate->state_known,
            candidate->source_submitted, candidate->submission_thread);
        g_ui_alpha_ticket_sequence = frame.state.generation;
        sl::CommandBuffer* copy_commands = nullptr;
        const bool recorded = prepared && DispatchUiAlphaExtractionLocked(queue, candidate, &copy_commands);
        if (prepared) mfgunlock::uialphabackend::FinishExtraction(frame.state, recorded);
        ready = mfgunlock::uialphabackend::ReadyToTag(frame.state);
        if (ready) {
          output->native_resource = frame.alpha_resource.handle;
          output->width = candidate->width; output->height = candidate->height;
          output->format = static_cast<uint32_t>(reshade::api::format::r8_unorm);
          output->state = static_cast<uint32_t>(reshade::api::resource_usage::unordered_access);
          output->copy_commands = copy_commands;
          output->slot = index; output->ticket = frame.state.pending.generation;
        }
      }
      if (!ready) {
        if (!frame.state.gpu_pending && !frame.state.pending.extraction_recorded)
          DestroyUiAlphaSourceLocked(frame);
        g_ui_alpha_backend.active_slot = UINT32_MAX;
      }
      break;
    }
  }
  ReleaseSRWLockExclusive(&g_ui_alpha_backend_lock);
  if (ready) g_ui_alpha_prepare_successes.fetch_add(1, std::memory_order_relaxed);
  else g_ui_alpha_prepare_failures.fetch_add(1, std::memory_order_relaxed);
  return ready;
}

bool FinishUiAlphaCandidate(
    const mfgunlock::framecount::UiCandidateSnapshot* candidate,
    const mfgunlock::framecount::UiAlphaCandidateSnapshot* output,
    bool tagged, sl::Result result) {
  if (!output || output->slot >= kUiAlphaSlots) return false;
  bool accepted = false;
  AcquireSRWLockExclusive(&g_ui_alpha_backend_lock);
  UiAlphaInternalScope internal;
  auto& frame = g_ui_alpha_backend.slots[output->slot];
  if (g_ui_alpha_backend.active_slot == output->slot &&
      frame.state.pending.generation == output->ticket) {
    auto* queue = g_ui_alpha_backend.queue;
    bool signaled = false;
    uint64_t value = g_ui_alpha_backend.next_retirement_fence_value;
    if (queue && value != UINT64_MAX && value != 0) {
      // Extraction and SL's own volatile clone-copy share this exact queue/list.
      // The fence protects the source SRV/reference and staging output even
      // when SetTag rejected the optional input after recording GPU work.
      queue->flush_immediate_command_list();
      signaled = queue->signal(g_ui_alpha_backend.retirement_fence, value);
      ++g_ui_alpha_backend.next_retirement_fence_value;
    }
    const bool current_output = candidate &&
        candidate->output_revision == mfgunlock::outputstate::Read().revision &&
        queue == g_primary_present_queue.load(std::memory_order_acquire);
    accepted = mfgunlock::uialphabackend::SealCopy(frame.state, output->ticket,
        queue != nullptr, tagged && result == sl::Result::eOk && current_output, signaled, value);
    g_ui_alpha_backend.active_slot = UINT32_MAX;
    if (signaled) g_ui_alpha_extractions.fetch_add(1, std::memory_order_relaxed);
    else g_ui_alpha_extraction_failures.fetch_add(1, std::memory_order_relaxed);
  }
  ReleaseSRWLockExclusive(&g_ui_alpha_backend_lock);
  if (accepted) MarkUiCandidateInjected(candidate);
  return accepted;
}
#else
void DestroyUiAlphaBackend(reshade::api::device*, bool) {}
void InvalidateUiAlphaSource(uint64_t) {}
void InvalidateUiAlphaPending() {}

bool PrepareUiAlphaCandidate(
    const mfgunlock::framecount::UiCandidateSnapshot*, sl::CommandBuffer*,
    mfgunlock::framecount::UiAlphaCandidateSnapshot*) {
  g_ui_alpha_prepare_failures.fetch_add(1, std::memory_order_relaxed);
  return false;
}

bool FinishUiAlphaCandidate(
    const mfgunlock::framecount::UiCandidateSnapshot*,
    const mfgunlock::framecount::UiAlphaCandidateSnapshot*, bool, sl::Result) { return false; }
#endif

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
    snapshot.ui_encoding = static_cast<uint32_t>(
        DetectedUiEncoding(candidate.view_format));
    snapshot.state = candidate.state;
    snapshot.state_known = candidate.state_known;
    snapshot.swapchain = candidate.swapchain;
    snapshot.already_tagged = candidate.already_tagged;
    snapshot.transparent_clear = candidate.last_clear_transparent;
    snapshot.rtv_binds_after_clear = candidate.rtv_binds_since_clear;
    snapshot.late_writes = candidate.current_late_writes != 0
        ? candidate.current_late_writes : candidate.last_late_writes;
    snapshot.alpha_convention = static_cast<uint32_t>(
        mfgunlock::uialpha::Resolve(candidate.alpha_evidence));
    snapshot.alpha_draws = candidate.alpha_evidence.draw_count;
    snapshot.alpha_unproven_draws = candidate.alpha_evidence.unproven_draws;
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

void OnExecuteUiCommandList(reshade::api::command_queue* queue,
                            reshade::api::command_list* command_list) {
  if (queue == nullptr || command_list == nullptr || queue->get_device() == nullptr ||
      queue->get_device()->get_api() != reshade::api::device_api::d3d12) {
    return;
  }
  const uint32_t thread = GetCurrentThreadId();
  AcquireSRWLockExclusive(&g_ui_candidate_lock);
  for (auto& candidate : g_ui_candidates) {
    if (!candidate.used || candidate.last_draw_command_list != command_list) continue;
    candidate.last_draw_queue = queue;
    candidate.last_draw_submission_thread = thread;
    candidate.last_draw_submitted = true;
  }
  ReleaseSRWLockExclusive(&g_ui_candidate_lock);
}

// Graphics initialization is also a finite fallback for already loaded modules.
void OnInitDevice(reshade::api::device* device) {
  if (device && !g_primary_device.load(std::memory_order_acquire)) {
    if (device->get_api() == reshade::api::device_api::vulkan)
      mfgunlock::outputstate::SetResourceApi(mfgunlock::outputstate::ResourceApi::kVulkan);
    else if (device->get_api() == reshade::api::device_api::d3d12)
      mfgunlock::outputstate::SetResourceApi(mfgunlock::outputstate::ResourceApi::kD3D12);
  }
  RefreshRuntime();
}

void OnInitCommandQueue(reshade::api::command_queue* /*queue*/) {
  RefreshRuntime();
}

void OnDestroyCommandQueue(reshade::api::command_queue* queue) {
  auto* expected = queue;
  g_primary_present_queue.compare_exchange_strong(
      expected, nullptr, std::memory_order_acq_rel);
#if defined(MFGUNLOCK_RESHADE_API) && MFGUNLOCK_RESHADE_API >= 18
  AcquireSRWLockExclusive(&g_ui_alpha_backend_lock);
  if (g_ui_alpha_backend.queue == queue) {
    // Teardown, not a frame-path wait: the queue is still alive in this event.
    // Drain our copy work before releasing its retained source references.
    UiAlphaInternalScope internal;
    DestroyUiAlphaBackendLocked(true);
  }
  ReleaseSRWLockExclusive(&g_ui_alpha_backend_lock);
#endif
  mfgunlock::integration::ObserveQueueDestroy(reinterpret_cast<uint64_t>(queue));
}

mfgunlock::outputstate::State ClassifySwapchainOutput(
    reshade::api::swapchain* swapchain) {
  if (swapchain == nullptr) return {};
  auto* device = swapchain->get_device();
  if (device == nullptr || swapchain->get_back_buffer_count() == 0) return {};

  const auto back_buffer = swapchain->get_back_buffer(0);
  const auto desc = device->get_resource_desc(back_buffer);
  const auto format = desc.texture.format;
  const bool format_known = static_cast<uint32_t>(format) != 0;
#if MFGUNLOCK_RESHADE_HAS_COLOR_SPACE
  const auto color_space = swapchain->get_color_space();
  const bool color_known = color_space != reshade::api::color_space::unknown;
  return mfgunlock::outputstate::Classify(
      OutputFormatKind(format), static_cast<uint32_t>(format), format_known,
      static_cast<uint32_t>(color_space), color_known);
#else
  return mfgunlock::outputstate::Classify(
      OutputFormatKind(format), static_cast<uint32_t>(format), format_known, 0u, false);
#endif
}

void ObserveUiOutput(reshade::api::swapchain* swapchain) {
  if (swapchain == nullptr ||
      swapchain != g_primary_swapchain.load(std::memory_order_acquire)) {
    return;
  }

  const auto state = ClassifySwapchainOutput(swapchain);
  mfgunlock::integration::ObserveSwapchainOutput(
      state.format, state.format_known, state.color_space, state.color_space_known,
      static_cast<uint32_t>(state.encoding), state.known, state.dlssg_supported);
  const bool changed = mfgunlock::outputstate::Observe(
      state.format_kind, state.format, state.format_known,
      state.color_space, state.color_space_known);
  if (changed) {
    InvalidateUiAlphaPending();
    mfgunlock::integration::InvalidateFrameEvidence();
    mfgunlock::presentation::Reset();
    ResetUiCandidateSelection();
    mfgunlock::framecount::NotifyOutputStateChanged();
  }
}

mfgunlock::presentation::SwapEffect PresentationSwapEffect(DXGI_SWAP_EFFECT effect) {
  using mfgunlock::presentation::SwapEffect;
  switch (effect) {
    case DXGI_SWAP_EFFECT_DISCARD: return SwapEffect::kDiscard;
    case DXGI_SWAP_EFFECT_SEQUENTIAL: return SwapEffect::kSequential;
    case DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL: return SwapEffect::kFlipSequential;
    case DXGI_SWAP_EFFECT_FLIP_DISCARD: return SwapEffect::kFlipDiscard;
    default: return SwapEffect::kUnknown;
  }
}

void ObservePresentationDxgiState(reshade::api::swapchain* swapchain) {
  using namespace mfgunlock::presentation;
  if (swapchain == nullptr || swapchain->get_device() == nullptr ||
      swapchain->get_device()->get_api() != reshade::api::device_api::d3d12 ||
      swapchain->get_native() == 0) {
    ObserveDxgi(false, false, SwapEffect::kUnknown,
                false, WindowMode::kUnknown,
                false, false, false, false, false, 0, false, false);
    return;
  }

  auto* native = reinterpret_cast<IDXGISwapChain*>(swapchain->get_native());
  DXGI_SWAP_CHAIN_DESC desc{};
  const bool desc_known = SUCCEEDED(native->GetDesc(&desc));
  const bool swap_effect_known = desc_known;
  const auto swap_effect = desc_known
      ? PresentationSwapEffect(desc.SwapEffect) : SwapEffect::kUnknown;
  const bool waitable_flag_known = desc_known;
  const bool waitable_flag = desc_known &&
      (desc.Flags & DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT) != 0;
  const bool allow_tearing_flag_known = desc_known;
  const bool allow_tearing_flag = desc_known &&
      (desc.Flags & DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING) != 0;

  BOOL fullscreen = FALSE;
  IDXGIOutput* output = nullptr;
  const bool window_mode_known = SUCCEEDED(native->GetFullscreenState(&fullscreen, &output));
  if (output != nullptr) output->Release();
  const auto window_mode = !window_mode_known
      ? WindowMode::kUnknown
      : (fullscreen ? WindowMode::kExclusiveFullscreen
                    : WindowMode::kWindowedOrBorderless);

  bool waitable_object_known = desc_known && !waitable_flag;
  bool waitable_object_available = false;
  bool max_frame_latency_known = false;
  uint32_t max_frame_latency = 0;
  IDXGISwapChain2* swapchain2 = nullptr;
  if (SUCCEEDED(native->QueryInterface(__uuidof(IDXGISwapChain2),
                                       reinterpret_cast<void**>(&swapchain2))) &&
      swapchain2 != nullptr) {
    UINT latency = 0;
    if (SUCCEEDED(swapchain2->GetMaximumFrameLatency(&latency))) {
      max_frame_latency_known = true;
      max_frame_latency = latency;
    }
    // Do not acquire a host-owned wait handle, or infer its use from a proxy flag.
    swapchain2->Release();
  }

  ObserveDxgi(desc_known || window_mode_known,
              swap_effect_known, swap_effect,
              window_mode_known, window_mode,
              waitable_flag_known, waitable_flag,
              waitable_object_known, waitable_object_available,
              max_frame_latency_known, max_frame_latency,
              allow_tearing_flag_known, allow_tearing_flag);
}

void InvalidatePrimaryDeviceState() {
  ClearUiRuntimeCandidates();
  ClearUiBlendTracking();
  mfgunlock::framecount::internal::InvalidateLiveOptions();
  mfgunlock::integration::Reset();
  mfgunlock::outputstate::Reset();
  mfgunlock::presentation::Reset();
  mfgunlock::composition::Reset();
  g_dynamic_stack_validated.store(false, std::memory_order_release);
  if (!mfgunlock::ngx::ResetFeatureTrackingForDeviceRecreation()) {
    const auto ngx = mfgunlock::ngx::internal::LifecycleSnapshot();
    mfgunlock::runtime::ObserveNgx(ngx.epoch, ngx.handles, true);
  }
  mfgunlock::vulkannative::ResetIdentity();
  if (mfgunlock::outputstate::GetResourceApi() == mfgunlock::outputstate::ResourceApi::kD3D12)
    mfgunlock::streamline::NotifyD3D12DeviceRecreated();
}

void OnDestroyDevice(reshade::api::device* device) {
  if (device == nullptr) return;
  // An older device may be destroyed after another swapchain became primary.
  // Retire only the backend actually owned by this device before returning.
  DestroyUiAlphaBackend(device, true);
  auto* expected = device;
  if (!g_primary_device.compare_exchange_strong(expected, nullptr, std::memory_order_acq_rel)) return;
  g_primary_swapchain.store(nullptr, std::memory_order_release);
  g_primary_swapchain_area.store(0, std::memory_order_relaxed);
  g_primary_output_width.store(0, std::memory_order_relaxed);
  g_primary_output_height.store(0, std::memory_order_relaxed);
  g_ui_candidate_d3d12.store(false, std::memory_order_relaxed);
  g_primary_present_queue.store(nullptr, std::memory_order_release);
  mfgunlock::framecount::ReleaseGlobalAutomaticUiTags();
  mfgunlock::framecount::NotifyDynamicD3D12(false);
  mfgunlock::runtime::ObserveDeviceDestroy(reinterpret_cast<uint64_t>(device));
  InvalidatePrimaryDeviceState();
}

void HandleInitSwapchain(reshade::api::swapchain* swapchain, bool resize = false) {
  if (swapchain == nullptr) return;
  const uint32_t back_buffer_count = swapchain->get_back_buffer_count();
  auto* device = swapchain->get_device();
  if (device == nullptr || back_buffer_count == 0) return;
  const auto api = device->get_api();
  if (api != reshade::api::device_api::d3d12 && api != reshade::api::device_api::vulkan) return;

  MarkSwapchainBuffers(swapchain, true);
  const auto back_buffer = swapchain->get_back_buffer(0);
  const auto desc = device->get_resource_desc(back_buffer);
  const uint64_t area = static_cast<uint64_t>(desc.texture.width) * desc.texture.height;
  const uint64_t selected_area = g_primary_swapchain_area.load(std::memory_order_relaxed);
  reshade::api::swapchain* selected = g_primary_swapchain.load(std::memory_order_acquire);
  if (selected != swapchain && selected != nullptr && area <= selected_area) return;

  const bool changed = selected != swapchain;
  auto* previous_device = g_primary_device.load(std::memory_order_acquire);
  if (previous_device && previous_device != device) DestroyUiAlphaBackend(previous_device, true);
  g_primary_swapchain_area.store(area, std::memory_order_relaxed);
  g_primary_output_width.store(desc.texture.width, std::memory_order_relaxed);
  g_primary_output_height.store(desc.texture.height, std::memory_order_relaxed);
  g_primary_swapchain.store(swapchain, std::memory_order_release);
  g_primary_device.store(device, std::memory_order_release);
  const bool d3d12 = api == reshade::api::device_api::d3d12;
  mfgunlock::outputstate::SetResourceApi(d3d12
      ? mfgunlock::outputstate::ResourceApi::kD3D12 : mfgunlock::outputstate::ResourceApi::kVulkan);
  g_ui_candidate_d3d12.store(d3d12, std::memory_order_relaxed);
  mfgunlock::framecount::NotifyDynamicD3D12(d3d12);
  if (d3d12) mfgunlock::streamline::NotifyD3D12DeviceObserved();
  const bool device_recreated = mfgunlock::runtime::ObserveDevice(
      reinterpret_cast<uint64_t>(device));
  if (device_recreated) InvalidatePrimaryDeviceState();
  else if (changed || resize) mfgunlock::integration::InvalidateFrameEvidence();
  mfgunlock::integration::ObserveSwapchainInit(
      reinterpret_cast<uint64_t>(swapchain), back_buffer_count, resize);
  mfgunlock::composition::Bind(d3d12 ? mfgunlock::composition::Api::kD3D12
                                     : mfgunlock::composition::Api::kVulkan,
      reinterpret_cast<uint64_t>(device), reinterpret_cast<uint64_t>(swapchain), back_buffer.handle);
  mfgunlock::runtime::ObserveSwapchainInit(reinterpret_cast<uint64_t>(swapchain));
  if (changed) ResetUiCandidateSelection();
  if (changed || resize) mfgunlock::presentation::Reset();

  ObservePresentationDxgiState(swapchain);
  ObserveUiOutput(swapchain);
}

void HandleDestroySwapchain(reshade::api::swapchain* swapchain, bool resize = false) {
  if (swapchain == nullptr) return;
  MarkSwapchainBuffers(swapchain, false);
  reshade::api::swapchain* expected = swapchain;
  if (!g_primary_swapchain.compare_exchange_strong(
          expected, nullptr, std::memory_order_acq_rel)) {
    return;
  }
  mfgunlock::integration::ObserveSwapchainDestroy(reinterpret_cast<uint64_t>(swapchain), resize);
  mfgunlock::integration::InvalidateFrameEvidence();
  mfgunlock::composition::Reset();
  mfgunlock::runtime::ObserveSwapchainDestroy(reinterpret_cast<uint64_t>(swapchain));
  mfgunlock::framecount::internal::InvalidateLiveOptions();
  g_primary_swapchain_area.store(0, std::memory_order_relaxed);
  g_primary_output_width.store(0, std::memory_order_relaxed);
  g_primary_output_height.store(0, std::memory_order_relaxed);
  g_primary_present_queue.store(nullptr, std::memory_order_release);
  mfgunlock::framecount::ReleaseGlobalAutomaticUiTags();
  InvalidateUiAlphaPending();
  mfgunlock::integration::ObserveSwapchainOutput(
      mfgunlock::diagnostic::kUnknown32, false, 0u, false,
      mfgunlock::diagnostic::kUnknown32, false, false);
  mfgunlock::presentation::ClearCorrelation();
  mfgunlock::presentation::ObserveDxgi(
      false, false, mfgunlock::presentation::SwapEffect::kUnknown,
      false, mfgunlock::presentation::WindowMode::kUnknown,
      false, false, false, false, false, 0, false, false);
  ResetUiCandidateSelection();
  mfgunlock::framecount::NotifyOutputUnknown();
}

#ifndef MFGUNLOCK_RESHADE_SWAPCHAIN_RESIZE_ARG
#if RESHADE_API_VERSION == 14
#define MFGUNLOCK_RESHADE_SWAPCHAIN_RESIZE_ARG 0
#elif RESHADE_API_VERSION >= 18
#define MFGUNLOCK_RESHADE_SWAPCHAIN_RESIZE_ARG 1
#else
#error "Unsupported ReShade swapchain callback ABI"
#endif
#endif

#if MFGUNLOCK_RESHADE_SWAPCHAIN_RESIZE_ARG == 0
void OnInitSwapchain(reshade::api::swapchain* swapchain) {
  HandleInitSwapchain(swapchain);
}

void OnDestroySwapchain(reshade::api::swapchain* swapchain) {
  HandleDestroySwapchain(swapchain);
}
#elif MFGUNLOCK_RESHADE_SWAPCHAIN_RESIZE_ARG == 1
void OnInitSwapchain(reshade::api::swapchain* swapchain, bool resize) {
  HandleInitSwapchain(swapchain, resize);
}

void OnDestroySwapchain(reshade::api::swapchain* swapchain, bool resize) {
  HandleDestroySwapchain(swapchain, resize);
}
#else
#error "Unsupported ReShade swapchain callback ABI"
#endif

template <typename Swapchain>
uint64_t CurrentBackbuffer(Swapchain* swapchain) {
  if constexpr (requires { swapchain->get_current_back_buffer_index(); }) {
    const uint32_t index = swapchain->get_current_back_buffer_index();
    if (index < swapchain->get_back_buffer_count()) return swapchain->get_back_buffer(index).handle;
  }
  return 0;
}

#if defined(MFGUNLOCK_RESHADE_API) && MFGUNLOCK_RESHADE_API >= 18
void OnFinishPresentUiAlpha(reshade::api::command_queue* queue,
                            reshade::api::swapchain* swapchain) {
  if (!queue || !swapchain || g_primary_swapchain.load(std::memory_order_acquire) != swapchain)
    return;
  // Present is not a release event. Only already-signaled copy fences reclaim slots.
  AcquireSRWLockExclusive(&g_ui_alpha_backend_lock);
  if (g_ui_alpha_backend.queue == queue) ReclaimUiAlphaSlotsLocked();
  ReleaseSRWLockExclusive(&g_ui_alpha_backend_lock);
}
#endif

void ObservePresentationBinding(reshade::api::swapchain* swapchain) {
  uint32_t viewport = 0;
  const uint64_t tagged = mfgunlock::integration::ReadTaggedBackbuffer(viewport);
  bool matched = false;
  if (tagged != 0 && (swapchain->get_device()->get_api() == reshade::api::device_api::d3d12 ||
                      swapchain->get_device()->get_api() == reshade::api::device_api::vulkan)) {
    for (uint32_t i = 0; i < swapchain->get_back_buffer_count(); ++i) {
      if (swapchain->get_back_buffer(i).handle == tagged) { matched = true; break; }
    }
  }
  mfgunlock::presentation::ObserveSourceBinding(viewport, matched);
}

void OnPresentUiOutput(reshade::api::command_queue* queue,
                       reshade::api::swapchain* swapchain,
                       const reshade::api::rect* /*source_rect*/,
                       const reshade::api::rect* /*dest_rect*/,
                       uint32_t /*dirty_rect_count*/,
                       const reshade::api::rect* /*dirty_rects*/) {
  mfgunlock::presentation::passive::CpuScope timing(
      mfgunlock::presentation::passive::CpuKind::kPresent);
  static std::atomic_uint32_t maintenance_presents{0};
  const uint32_t present_index =
      maintenance_presents.fetch_add(1, std::memory_order_relaxed);
  const bool trace = mfgunlock::diagnostic::g_verbose.load(std::memory_order_relaxed) &&
      (present_index < 16 || present_index % 120 == 0);
  LARGE_INTEGER before_attach{}, after_attach{}, after_refresh{};
  if (trace) QueryPerformanceCounter(&before_attach);
  TryAttachRuntime();
  if (trace) QueryPerformanceCounter(&after_attach);
  const bool periodic_maintenance = present_index % 120 == 0;
  if (periodic_maintenance && g_bootstrap_complete.load(std::memory_order_acquire))
    PollRuntimeModuleChanges(false);
  const bool provider_pending = g_provider_scan_pending.load(std::memory_order_acquire);
  const bool streamline_pending = g_streamline_scan_pending.load(std::memory_order_acquire);
  const bool ngx_pending = g_ngx_scan_pending.load(std::memory_order_acquire);
  const bool refresh = g_bootstrap_complete.load(std::memory_order_acquire) &&
      (provider_pending || streamline_pending || ngx_pending);
  if (refresh) RefreshRuntime();
  if (trace) {
    QueryPerformanceCounter(&after_refresh);
    LARGE_INTEGER frequency{};
    if (QueryPerformanceFrequency(&frequency) && frequency.QuadPart > 0) {
      const auto elapsed_us = [&](LARGE_INTEGER start, LARGE_INTEGER end) {
        return (end.QuadPart - start.QuadPart) * 1000000LL / frequency.QuadPart;
      };
      std::stringstream message;
      message << "Stage2PresentTiming present=" << (present_index + 1)
              << " refresh=" << (refresh ? 1 : 0)
              << " periodic=" << (periodic_maintenance ? 1 : 0)
              << " provider=" << (provider_pending ? 1 : 0)
              << " streamline=" << (streamline_pending ? 1 : 0)
              << " ngx=" << (ngx_pending ? 1 : 0)
              << " attach_us=" << elapsed_us(before_attach, after_attach)
              << " refresh_us=" << elapsed_us(after_attach, after_refresh);
      mfgunlock::provider::Log(message.str());
    }
  }
  if (swapchain == nullptr) return;
  static std::atomic_uint32_t diag_presents{0};
  const uint32_t diag_present = diag_presents.fetch_add(1, std::memory_order_relaxed) + 1;
  if (diag_present <= 8 && mfgunlock::diagnostic::g_verbose.load(std::memory_order_relaxed)) {
    std::stringstream message;
    message << "TuringDiagV2 ReShade Present#" << diag_present
            << " thread=" << GetCurrentThreadId() << " swapchain=" << swapchain;
    const DWORD last_error = GetLastError();
    mfgunlock::provider::Log(message.str());
    SetLastError(last_error);
  }
  if (g_primary_swapchain.load(std::memory_order_acquire) == nullptr)
    HandleInitSwapchain(swapchain);
  if (g_primary_swapchain.load(std::memory_order_acquire) != swapchain) return;
  if (queue != nullptr && swapchain->get_device() != nullptr &&
      (swapchain->get_device()->get_api() == reshade::api::device_api::d3d12 ||
       swapchain->get_device()->get_api() == reshade::api::device_api::vulkan)) {
    g_primary_present_queue.store(queue, std::memory_order_release);
  }
  mfgunlock::integration::ObserveQueueInit(reinterpret_cast<uint64_t>(queue));
  mfgunlock::integration::ObserveSwapchainPresent(
      reinterpret_cast<uint64_t>(swapchain), GetCurrentThreadId());
  const uint64_t resource = CurrentBackbuffer(swapchain);
  ObservePresentationBinding(swapchain);
  mfgunlock::presentation::ObserveApplicationPresent();
  static std::atomic_uint32_t presentation_probe_counter{0};
  if ((presentation_probe_counter.fetch_add(1, std::memory_order_relaxed) & 63u) == 0)
    ObservePresentationDxgiState(swapchain);

  auto* device = swapchain->get_device();
  if (device != nullptr && swapchain->get_back_buffer_count() != 0) {
    const auto desc = device->get_resource_desc(swapchain->get_back_buffer(0));
    const uint32_t old_width = g_primary_output_width.exchange(
        desc.texture.width, std::memory_order_relaxed);
    const uint32_t old_height = g_primary_output_height.exchange(
        desc.texture.height, std::memory_order_relaxed);
    if ((old_width != 0 && old_width != desc.texture.width) ||
        (old_height != 0 && old_height != desc.texture.height)) {
      mfgunlock::integration::InvalidateFrameEvidence();
      mfgunlock::composition::Reset();
      mfgunlock::framecount::internal::InvalidateLiveOptions();
      mfgunlock::presentation::ClearCorrelation();
      ResetUiCandidateSelection();
    }
  }
  if (device != nullptr && swapchain->get_back_buffer_count() != 0) {
    mfgunlock::composition::Bind(device->get_api() == reshade::api::device_api::d3d12
        ? mfgunlock::composition::Api::kD3D12 : mfgunlock::composition::Api::kVulkan,
        reinterpret_cast<uint64_t>(device), reinterpret_cast<uint64_t>(swapchain),
        swapchain->get_back_buffer(0).handle);
    mfgunlock::composition::ObservePresent(reinterpret_cast<uint64_t>(swapchain), resource);
  }
  ObserveUiOutput(swapchain);
  mfgunlock::framecount::ApplyLiveOptionsOnPresent();
}

// Compare resource identity instead of assuming effect_runtime is a swapchain.
template <typename Runtime>
uint64_t CompositionBackbuffer(Runtime* runtime) {
  if (runtime == nullptr) return 0;
  if constexpr (requires { runtime->get_back_buffer_count(); runtime->get_back_buffer(0); }) {
    if (runtime->get_back_buffer_count() != 0) return runtime->get_back_buffer(0).handle;
  }
  return 0;
}

void ObserveEffects(bool begin, reshade::api::effect_runtime* runtime,
                    reshade::api::resource_view rtv) {
  if (runtime == nullptr || runtime->get_device() == nullptr) return;
  auto* device = runtime->get_device();
  const auto resource = rtv.handle != 0 ? device->get_resource_from_view(rtv).handle : 0;
  mfgunlock::composition::ObserveEffects(begin, reinterpret_cast<uint64_t>(device),
      CompositionBackbuffer(runtime), reinterpret_cast<uint64_t>(runtime), resource);
}

void OnBeginEffects(reshade::api::effect_runtime* runtime, reshade::api::command_list*,
                    reshade::api::resource_view rtv, reshade::api::resource_view) {
  ObserveEffects(true, runtime, rtv);
}

void OnFinishEffects(reshade::api::effect_runtime* runtime, reshade::api::command_list*,
                     reshade::api::resource_view rtv, reshade::api::resource_view) {
  ObserveEffects(false, runtime, rtv);
}

void OnOverlayRendered(reshade::api::effect_runtime* runtime) {
  if (runtime == nullptr) return;
  mfgunlock::composition::ObserveOverlay(reinterpret_cast<uint64_t>(runtime->get_device()),
                                         CompositionBackbuffer(runtime));
}

void OnScreenshotSaved(reshade::api::effect_runtime* runtime, const char* /*path*/) {
  if (runtime == nullptr) return;
  mfgunlock::composition::ObserveScreenshot(reinterpret_cast<uint64_t>(runtime->get_device()),
                                            CompositionBackbuffer(runtime));
}

void OnDestroyEffects(reshade::api::effect_runtime* runtime) {
  mfgunlock::composition::InvalidateEffects(reinterpret_cast<uint64_t>(runtime));
}

template <typename Events = reshade::addon_event>
void RegisterScreenshotObserver(bool attach) {
  if constexpr (requires { Events::reshade_screenshot; }) {
    if (attach) reshade::register_event<Events::reshade_screenshot>(OnScreenshotSaved);
    else reshade::unregister_event<Events::reshade_screenshot>(OnScreenshotSaved);
    mfgunlock::composition::SetScreenshotEventAvailable(attach);
  }
}

// ---------------------------------------------------------------- config

std::atomic_bool g_early_load_restart_required{false};

void EnsureEarlyLoadConfig() {
#if !MFGUNLOCK_RESHADE_HAS_CONFIG_ARRAY
  // ReShade revisions without config-array support cannot safely preserve the
  // NUL-separated EarlyLoading list. Do not overwrite a user's configuration.
  return;
#else
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
#endif
}

// ---------------------------------------------------------------- overlay

void SetWarpMode(int value) {
  const int mode = static_cast<int>(mfgunlock::validatedwarp::ConfiguredMode(value));
  g_validated_warp_mode.store(mode, std::memory_order_relaxed);
  reshade::set_config_value(nullptr, kConfigSection, "WarpDiagnosticMode", mode);
}

void ReserveDebugRows(float start_y, float rows) {
  const float minimum_y = start_y + ImGui::GetTextLineHeightWithSpacing() * rows;
  if (ImGui::GetCursorPosY() < minimum_y) ImGui::SetCursorPosY(minimum_y);
}

void DrawWarpStatus(bool requested) {
  // UI-only last complete status; lock contention is not a feature transition.
  static std::string last_status = "Warp: INACTIVE";
  if (!TryAcquireSRWLockShared(&g_provider_maintenance_lock)) {
    ImGui::TextUnformatted(last_status.c_str());
    return;
  }
  struct Unlock {
    ~Unlock() { ReleaseSRWLockShared(&g_provider_maintenance_lock); }
  } unlock;
  const auto mode = mfgunlock::validatedwarp::ConfiguredMode(
      g_validated_warp_mode.load(std::memory_order_relaxed));
  if (!g_validated_warp_modules.empty()) {
    const bool matches = std::all_of(g_validated_warp_modules.begin(), g_validated_warp_modules.end(),
                                    [mode](const auto& patch) { return patch.mode == mode; });
    if (!requested) {
      last_status = "Warp: ACTIVE | REQUESTED_OFF";
    } else if (!matches) {
      last_status = "Warp: ACTIVE | MODE_PENDING";
    } else if (mode == mfgunlock::validatedwarp::Mode::kValidatedWarp) {
      last_status = "Warp: ACTIVE";
    } else {
      last_status = "Warp: ACTIVE | " + mfgunlock::validatedwarp::ModeLabel(
          mode, g_validated_warp_modules.front().target_sm);
    }
  } else if (requested) {
    if (!g_validated_warp_detail.empty()) {
      last_status = "Warp: UNAVAILABLE";
    } else {
      last_status = "Warp: PENDING";
    }
  } else {
    last_status = "Warp: DISABLED";
  }
  ImGui::TextUnformatted(last_status.c_str());
}

const char* DlssgModeName(uint32_t mode) {
  switch (mode) {
    case 0: return "off";
    case 1: return "on";
    case 2: return "auto";
    case 3: return "dynamic";
    default: return "unknown";
  }
}

void DrawMfgRuntimeDiagnostics() {
  namespace fc = mfgunlock::framecount;
  constexpr uint32_t kUnknown = mfgunlock::diagnostic::kUnknown32;

  const uint32_t requested_mode = fc::g_last_requested_mode.load(std::memory_order_relaxed);
  const uint32_t forwarded_mode = fc::g_last_forwarded_mode.load(std::memory_order_relaxed);
  const uint32_t accepted_mode = fc::g_last_accepted_mode.load(std::memory_order_relaxed);
  const uint32_t requested_generated =
      fc::g_last_requested_generated.load(std::memory_order_relaxed);
  const uint32_t forwarded_generated =
      fc::g_last_forwarded_generated.load(std::memory_order_relaxed);
  const uint32_t accepted_generated =
      fc::g_last_accepted_generated.load(std::memory_order_relaxed);

  ImGui::Text("MFG gate: %s | candidates: %u | ceiling: %u",
      g_mfg_gate_status.load(std::memory_order_acquire),
      g_mfg_gate_candidates.load(std::memory_order_relaxed),
      g_mfg_gate_ceiling.load(std::memory_order_acquire));
  ImGui::Text("MFG native capability: %s", fc::g_state_seen.load(std::memory_order_acquire)
      ? (fc::g_runtime_max_generated.load(std::memory_order_relaxed) > 1 ? "available" : "unavailable")
      : "--");
  ImGui::TextUnformatted("MFG runtime contract");
  ImGui::Separator();
  ImGui::Text("Mode: requested %s, forwarded %s, accepted %s.",
      requested_mode == kUnknown ? "unknown" : DlssgModeName(requested_mode),
      forwarded_mode == kUnknown ? "unknown" : DlssgModeName(forwarded_mode),
      accepted_mode == kUnknown ? "unknown" : DlssgModeName(accepted_mode));

  if (requested_generated != kUnknown && forwarded_generated != kUnknown &&
      accepted_generated != kUnknown) {
    ImGui::Text("Generated-frame request: game %u, forwarded %u, accepted %u.",
                requested_generated, forwarded_generated, accepted_generated);
  } else {
    ImGui::TextDisabled("Generated-frame request: --");
  }

  const uint32_t structural = fc::g_streamline_max_generated.load(std::memory_order_relaxed);
  const uint32_t runtime = fc::g_runtime_max_generated.load(std::memory_order_relaxed);
  const uint32_t effective = fc::g_effective_max_generated.load(std::memory_order_relaxed);
  const std::string structural_text = structural != 0 ? std::to_string(structural) : "unknown";
  const std::string runtime_text = fc::g_state_seen.load(std::memory_order_relaxed)
      ? std::to_string(runtime) : "unknown";
  const std::string effective_text = fc::g_effective_max_seen.load(std::memory_order_acquire)
      ? std::to_string(effective) : "unknown";
  ImGui::Text("Maximum generated frames: structural %s, runtime %s, effective %s.",
      structural_text.c_str(), runtime_text.c_str(), effective_text.c_str());

  if (fc::g_dynamic_support_seen.load(std::memory_order_acquire)) {
    ImGui::Text("Dynamic MFG: runtime support %s, addon request %s, SetOptions accepted %s.",
        fc::g_dynamic_supported.load(std::memory_order_relaxed) ? "yes" : "no",
        fc::g_dynamic_mfg_enabled.load(std::memory_order_relaxed) ? "yes" : "no",
        fc::g_dynamic_applied.load(std::memory_order_relaxed) ? "yes" : "no");
    ImGui::TextDisabled("Dynamic provider mode: --");
  } else {
    ImGui::TextDisabled("Dynamic MFG support: --");
  }

  const auto output_state = mfgunlock::outputstate::Read();
  if (output_state.known) {
    if (output_state.color_space_known) {
      ImGui::Text("Output: %s, format %u, swapchain color-space value %u.",
          mfgunlock::outputstate::EncodingName(output_state.encoding),
          output_state.format_known ? output_state.format : mfgunlock::diagnostic::kUnknown32,
          output_state.color_space);
    } else {
      ImGui::Text("Output: %s, swapchain color space unknown.",
          mfgunlock::outputstate::EncodingName(output_state.encoding));
    }
    if (!output_state.dlssg_supported)
      ImGui::TextDisabled("Output: unsupported color format");
  } else {
    ImGui::TextDisabled("Output encoding/color space: unknown.");
  }

  namespace passive = mfgunlock::presentation::passive;
  bool capture = passive::g_enabled.load(std::memory_order_relaxed);
  if (ImGui::Checkbox("Passive CPU/cadence capture", &capture)) passive::SetEnabled(capture);
  if (capture) {
    const auto metrics = passive::Read();
    ImGui::Text("Metrics generation %llu | lost %llu | invalid %llu",
        static_cast<unsigned long long>(metrics.generation),
        static_cast<unsigned long long>(metrics.lost_events),
        static_cast<unsigned long long>(metrics.invalid_events));
    if (metrics.source_callbacks.valid) ImGui::Text("Source callbacks: %.3f /s (%llu intervals)",
        metrics.source_callbacks.hz, static_cast<unsigned long long>(metrics.source_callbacks.intervals));
    else ImGui::TextDisabled("Source callbacks/s: --");
    if (metrics.native_presented.valid) ImGui::Text("SL-reported presented: %.3f /s (%llu intervals)",
        metrics.native_presented.hz, static_cast<unsigned long long>(metrics.native_presented.intervals));
    else ImGui::TextDisabled("SL-reported presented/s: --");
    for (size_t index = 0; index < passive::kCpuKinds; ++index) {
      const auto& stat = metrics.cpu[index];
      if (stat.valid) ImGui::Text("%s own CPU us: p50 %.2f | p95 %.2f | p99 %.2f | n=%llu",
          passive::Name(static_cast<passive::CpuKind>(index)),
          stat.p50_ns / 1000.0, stat.p95_ns / 1000.0, stat.p99_ns / 1000.0,
          static_cast<unsigned long long>(stat.retained));
    }
    ImGui::TextDisabled("Display scan-out / generated / dropped / duplicate: --");
    if (ImGui::Button("Write passive snapshot to log")) {
      std::stringstream row;
      row << "F2Metrics generation=" << metrics.generation
          << " source_callbacks_hz=" << (metrics.source_callbacks.valid ? metrics.source_callbacks.hz : -1.0)
          << " native_presented_hz=" << (metrics.native_presented.valid ? metrics.native_presented.hz : -1.0)
          << " source_intervals=" << metrics.source_callbacks.intervals
          << " native_intervals=" << metrics.native_presented.intervals
          << " lost=" << metrics.lost_events << " invalid=" << metrics.invalid_events;
      for (size_t index=0; index<passive::kCpuKinds; ++index) {
        const auto& stat=metrics.cpu[index];
        row << " cpu" << index << "_n=" << stat.retained
            << " cpu" << index << "_p50_ns=" << stat.p50_ns
            << " cpu" << index << "_p95_ns=" << stat.p95_ns
            << " cpu" << index << "_p99_ns=" << stat.p99_ns;
      }
      row << " timestamp_ns=" << metrics.timestamp_ns
          << " source_span_ns=" << metrics.source_callbacks.span_ns
          << " native_span_ns=" << metrics.native_presented.span_ns
          << " source_age_ns=" << metrics.source_callbacks.age_ns
          << " native_age_ns=" << metrics.native_presented.age_ns
          << " output_revision=" << output_state.revision;
#if defined(MFGUNLOCK_RESHADE_API) && MFGUNLOCK_RESHADE_API >= 18
      if (TryAcquireSRWLockShared(&g_ui_alpha_backend_lock)) {
        unsigned int pending = 0, poisoned = 0;
        for (const auto& slot : g_ui_alpha_backend.slots) {
          pending += slot.state.gpu_pending || slot.state.pending.prepared ? 1u : 0u;
          poisoned += slot.state.poisoned ? 1u : 0u;
        }
        row << " alpha_pool_pending=" << pending << " alpha_pool_poisoned=" << poisoned;
        ReleaseSRWLockShared(&g_ui_alpha_backend_lock);
      } else row << " alpha_pool=BUSY";
#endif
      row << " alpha_prepare=" << g_ui_alpha_prepare_successes.load()
          << " alpha_prepare_fail=" << g_ui_alpha_prepare_failures.load()
          << " alpha_copy_signaled=" << g_ui_alpha_extractions.load()
          << " alpha_signal_fail=" << g_ui_alpha_extraction_failures.load()
          << " ui_history_policy=NATIVE_CONSTANTS"
          << " ui_constants_calls_total=" << mfgunlock::framecount::g_ui_constants_calls.load()
          << " ui_native_reset_true_total=" << mfgunlock::framecount::g_ui_native_resets_seen.load()
          << " ui_reset_requests_total=" << mfgunlock::framecount::g_ui_resets_requested.load()
          << " ui_reset_requests_retired_total=" << mfgunlock::framecount::g_ui_reset_requests_retired.load()
          << " ui_forced_reset_total=" << mfgunlock::framecount::g_ui_resets_injected.load()
          << " ui_composition_requested=" << mfgunlock::framecount::g_ui_composition_enabled.load()
          << " ui_composition_applied=" << mfgunlock::framecount::g_ui_composition_applied.load()
          << " ui_helper_injected_total=" << mfgunlock::framecount::g_ui_candidate_tags_injected.load();
      mfgunlock::provider::Log(row.str());
      const auto vk = mfgunlock::ngx::internal::ReadVulkanObservation();
      auto* current_device = g_primary_device.load(std::memory_order_acquire);
      if (current_device && current_device->get_api() == reshade::api::device_api::vulkan) {
        std::stringstream detail;
        detail << "F3Vulkan api=Vulkan generation=" << vk.generation
               << " create_ok=" << vk.create_ok << " evaluate_ok=" << vk.evaluate_ok
               << " release_ok=" << vk.release_ok << " failures=" << vk.failures
               << " snapshot_current=" << vk.snapshot_current
               << " last_result=" << vk.last_result
               << " commands=" << vk.last_evaluate.commands
               << " frame_known=" << vk.last_evaluate.backbuffer_frame_id_known
               << " frame=" << vk.last_evaluate.backbuffer_frame_id
               << " generated_count_known=" << vk.last_evaluate.generated_count_known
               << " requested_generated=" << vk.last_evaluate.generated_count
               << " output=" << mfgunlock::outputstate::EncodingName(output_state.encoding)
               << " output_format=" << output_state.format
               << " completion=UNKNOWN scanout=UNKNOWN";
        for (size_t i=0; i<vk.last_evaluate.resources.size(); ++i) {
          const auto& resource=vk.last_evaluate.resources[i];
          detail << " r" << i << "_known=" << resource.known
                 << " r" << i << "_image=" << resource.object
                 << " r" << i << "_vkformat=" << resource.format
                 << " r" << i << "_view=" << vk.images[i].view
                 << " r" << i << "_width=" << vk.images[i].width
                 << " r" << i << "_height=" << vk.images[i].height
                 << " r" << i << "_aspect=" << vk.images[i].aspect
                 << " r" << i << "_first_mip=" << vk.images[i].first_mip
                 << " r" << i << "_mips=" << vk.images[i].mip_count
                 << " r" << i << "_first_layer=" << vk.images[i].first_layer
                 << " r" << i << "_layers=" << vk.images[i].layer_count;
        }
        mfgunlock::provider::Log(detail.str());
      }
      DumpDlssStartupEvidence("manual-passive-snapshot");

    }
  }
  const auto presentation = mfgunlock::presentation::Read();
  if (presentation.effective_multiplier_known) {
    ImGui::Text("Presentation: effective %.3fx (%s), source presents %llu, provider-presented %llu.",
        static_cast<double>(presentation.effective_multiplier_milli) / 1000.0,
        mfgunlock::presentation::ConfidenceName(presentation.confidence),
        static_cast<unsigned long long>(presentation.correlated_source_presents),
        static_cast<unsigned long long>(presentation.correlated_provider_presented));
  } else {
    ImGui::TextDisabled("Presentation multiplier: --");
  }
  if (presentation.accepted_mode_known &&
      presentation.accepted_mode == mfgunlock::presentation::ProviderMode::kFixed &&
      presentation.accepted_multiplier_known) {
    ImGui::Text("Provider accepted fixed multiplier: %ux. Presentation health: %s.",
        presentation.accepted_multiplier,
        mfgunlock::presentation::HealthName(presentation.health));
  } else if (presentation.accepted_mode_known) {
    ImGui::TextDisabled("Provider accepted mode: %s",
        mfgunlock::presentation::ProviderModeName(presentation.accepted_mode));
  } else {
    ImGui::TextDisabled("Provider accepted mode/multiplier: unknown.");
  }
  ImGui::TextDisabled(
      "Generated multiplier: --");
  if (presentation.swap_effect_known || presentation.window_mode_known) {
    ImGui::Text("DXGI: swap effect %s, mode %s.",
        presentation.swap_effect_known
            ? mfgunlock::presentation::SwapEffectName(presentation.swap_effect) : "unknown",
        presentation.window_mode_known
            ? mfgunlock::presentation::WindowModeName(presentation.window_mode) : "unknown");
  } else {
    ImGui::TextDisabled("DXGI presentation model: unknown.");
  }
  if (presentation.waitable_flag_known) {
    ImGui::Text("Waitable swapchain: flag %s, object %s, max frame latency %s.",
        presentation.waitable_flag ? "yes" : "no",
        presentation.waitable_object_known
            ? (presentation.waitable_object_available ? "available" : "not available") : "unknown",
        presentation.max_frame_latency_known
            ? std::to_string(presentation.max_frame_latency).c_str() : "unknown");
  } else {
    ImGui::TextDisabled("Waitable swapchain/max frame latency: unknown.");
  }
  if (presentation.allow_tearing_flag_known)
    ImGui::Text("ALLOW_TEARING creation flag: %s.", presentation.allow_tearing_flag ? "yes" : "no");
  ImGui::TextDisabled(
      "Present flags / SyncInterval / Independent Flip: --");

  const auto ui_debug = fc::ReadUiDebugSnapshot();
  const auto candidate = ReadUiCandidateDebugSnapshot();
  const bool native_uir = ui_debug.hudless_seen &&
      (ui_debug.ui_color_alpha_seen || ui_debug.ui_alpha_seen);
  const bool automatic_applied = fc::g_ui_candidate_active.load(std::memory_order_relaxed);
  uint32_t reject = candidate.present
      ? candidate.reject_reasons
      : mfgunlock::diagnostic::kUiAutoNoCandidate;
  if (!fc::g_ui_candidate_injection_enabled.load(std::memory_order_relaxed))
    reject |= mfgunlock::diagnostic::kUiAutoDisabled;
  if (candidate.present && !mfgunlock::outputstate::IsUiEncodingCompatible(
          output_state,
          static_cast<mfgunlock::outputstate::UiEncoding>(candidate.ui_encoding), false))
    reject |= mfgunlock::diagnostic::kUiAutoEncodingMismatch;
  if (!fc::g_ui_candidate_options_synced.load(std::memory_order_acquire))
    reject |= mfgunlock::diagnostic::kUiAutoOptionsUnsynced;
  if (fc::g_ui_candidate_runtime_declined.load(std::memory_order_acquire))
    reject |= mfgunlock::diagnostic::kUiAutoRuntimeDeclined;
  ImGui::Text("UIR: native observed %s, automatic eligible %s, automatic applied %s.",
      native_uir ? "yes" : "no", candidate.safe && reject == 0 ? "yes" : "no",
      automatic_applied ? "yes" : "no");
  if (!automatic_applied && reject != 0)
    ImGui::TextDisabled("Automatic UIR rejection mask: 0x%x.", reject);
}

void OnRegisterOverlay(reshade::api::effect_runtime* /*runtime*/) {
  TryAttachRuntime();
  bool advanced = g_show_debug_options.load(std::memory_order_relaxed);
  if (ImGui::Checkbox("Advanced", &advanced)) {
    g_show_debug_options.store(advanced, std::memory_order_relaxed);
    reshade::set_config_value(nullptr, kConfigSection, "ShowDebugOptions", advanced ? 1 : 0);
  }
  if (advanced) {
    if (g_early_load_restart_required.load(std::memory_order_acquire))
      ImGui::Text("Restart game to finish setup");
    mfgunlock::streamline::Draw();
  const bool warp_lock = TryAcquireSRWLockShared(&g_provider_maintenance_lock) != 0;
  static bool warp_active = false;
  if (warp_lock) {
    warp_active = !g_validated_warp_modules.empty();
    ReleaseSRWLockShared(&g_provider_maintenance_lock);
  }
  ImGui::Text("Temporal: %s | Warp: %s",
      (g_midpoint_patched.load(std::memory_order_acquire) ||
       g_blackwell_temporal_patched.load(std::memory_order_acquire)) ? "ACTIVE" : "INACTIVE",
      warp_active ? "ACTIVE" : "INACTIVE");
  }
  bool enabled = g_enabled.load(std::memory_order_relaxed);
  if (ImGui::Checkbox("Enable MFG Unlock", &enabled)) {
    g_enabled.store(enabled, std::memory_order_relaxed);
    mfgunlock::framecount::NotifyLiveOptionsChanged();
    reshade::set_config_value(nullptr, kConfigSection, "Enabled", enabled ? 1 : 0);
    if (enabled) InstallPresetForLoadedProviders();
  }

  // Commit one selected value after the slider is released.
  const int configured_force = static_cast<int>(
      mfgunlock::framecount::g_force_multiplier.load(std::memory_order_relaxed));
  const int selected_force = configured_force >= 2 ? (std::min)(configured_force, 6) : 1;
  static int pending_force = selected_force;
  static bool multiplier_editing = false;
  if (!multiplier_editing) pending_force = selected_force;
  ImGui::SliderInt("FG multiplier", &pending_force, 1, 6,
                   pending_force <= 1 ? "Game / Auto" : "%dx");
  const bool commit_multiplier = ImGui::IsItemDeactivatedAfterEdit();
  multiplier_editing = ImGui::IsItemActive();
  if (commit_multiplier) {
    const int force = pending_force <= 1 ? 0 : (std::min)(pending_force, 6);
    if (force != configured_force) {
      mfgunlock::framecount::g_force_multiplier.store(
          static_cast<unsigned int>(force), std::memory_order_relaxed);
      mfgunlock::framecount::NotifyFixedMultiplierChanged();
      reshade::set_config_value(nullptr, kConfigSection, "ForceMultiplier", force);
    }
    pending_force = force == 0 ? 1 : force;
  }
  const int force = configured_force;
  const auto sl_native = mfgunlock::framecount::g_native_maximum.load(std::memory_order_acquire);
  const auto ngx_native = mfgunlock::ngx::g_status.native_maximum.load(std::memory_order_acquire);
  const uint64_t native_maximum = mfgunlock::runtimecontrols::MaximumKnown(sl_native)
      ? sl_native : ngx_native;
  if (mfgunlock::runtimecontrols::MaximumKnown(native_maximum)) {
    const unsigned long long native =
        static_cast<unsigned long long>(mfgunlock::runtimecontrols::MaximumGenerated(native_maximum)) + 1;
    if (force >= 2 && mfgunlock::runtimecontrols::AboveMaximum(
        static_cast<uint32_t>(force - 1), native_maximum)) {
      ImGui::TextColored(ImVec4(1.0f, 0.72f, 0.22f, 1.0f),
                         "Library max: %llux | Selected: %dx", native, force);
    } else {
      ImGui::Text("Library max: %llux", native);
    }
  } else {
    ImGui::TextDisabled("Library max: --");
  }
  if (force >= 2 && !mfgunlock::framecount::g_dynamic_applied.load(std::memory_order_relaxed)) {
    const auto fixed_status = mfgunlock::framecount::g_fixed_override_status.load(
        std::memory_order_relaxed);
    const unsigned int requested =
        mfgunlock::framecount::g_last_requested.load(std::memory_order_relaxed);
    const unsigned int effective =
        mfgunlock::framecount::g_last_effective_generated.load(std::memory_order_relaxed);
    using Status = mfgunlock::framecount::FixedOverrideStatus;
    switch (fixed_status) {
      case Status::kPending:
        ImGui::TextDisabled("FG: pending");
        break;
      case Status::kApplied:
        ImGui::Text("FG: %ux", effective + 1);
        break;
      case Status::kAppliedAfterRetry:
        ImGui::Text("FG: %ux", effective + 1);
        break;
      case Status::kAlreadyMatched:
        ImGui::Text("FG: %ux", requested + 1);
        break;
      case Status::kNativeRejected:
        ImGui::Text("FG: rejected by runtime");
        break;
      case Status::kRejectedNativeFallback:
        ImGui::Text("FG: fallback %ux", effective + 1);
        break;
      case Status::kFallbackRejected:
        ImGui::Text("FG: rejected");
        break;
      case Status::kBlockedBackend:
        ImGui::Text("FG: unavailable");
        break;
      case Status::kBlockedPacing:
        ImGui::Text("FG: pacing unavailable");
        break;
      case Status::kBlockedStructuralCeiling: {
        const unsigned int ceiling =
            mfgunlock::framecount::g_streamline_max_generated.load(std::memory_order_relaxed);
        ImGui::Text("FG: ceiling %ux", ceiling + 1);
        break;
      }
      case Status::kUnknownStructuralCeiling:
        ImGui::Text("FG: ceiling unknown");
        break;
      case Status::kUnknownOptionsAbi:
        ImGui::Text("FG: unsupported API");
        break;
      default:
        break;
    }
  }

  static const char* kFgPresets[] = {"Application / driver default", "Preset A", "Preset B", "Preset C"};
  int preset = static_cast<int>(mfgunlock::fgpreset::g_requested.load(std::memory_order_relaxed));
  if (ImGui::Combo("Frame Generation model", &preset, kFgPresets, ARRAYSIZE(kFgPresets))) {
    mfgunlock::fgpreset::g_requested.store(mfgunlock::fgpreset::Parse(preset), std::memory_order_relaxed);
    reshade::set_config_value(nullptr, kConfigSection, "FrameGenerationPreset", preset);
    mfgunlock::fgpreset::g_last_supplied.store(-1, std::memory_order_relaxed);
    mfgunlock::fgpreset::g_last_applied.store(-1, std::memory_order_relaxed);
    InstallPresetForLoadedProviders();
  }

  if (preset != 0 && enabled) {
    const int supplied = mfgunlock::fgpreset::g_last_supplied.load(std::memory_order_relaxed);
    if (supplied == preset) {
      ImGui::Text("Preset supplied: %s", mfgunlock::fgpreset::Name(
          mfgunlock::fgpreset::Parse(preset)));
    } else if (!mfgunlock::fgpreset::HasHook()) {
      ImGui::TextDisabled("Preset: waiting for DLSS-G");
    } else {
      ImGui::TextDisabled("Preset: next FG model");
    }
  }

  bool dynamic_mfg = mfgunlock::framecount::g_dynamic_mfg_enabled.load(
      std::memory_order_relaxed);
  if (ImGui::Checkbox("NVIDIA Dynamic MFG", &dynamic_mfg)) {
    mfgunlock::framecount::g_dynamic_mfg_enabled.store(dynamic_mfg,
                                                       std::memory_order_relaxed);
    mfgunlock::framecount::NotifyDynamicModeChanged();
    reshade::set_config_value(nullptr, kConfigSection, "DynamicMFG",
                              dynamic_mfg ? 1 : 0);
  }

  if (dynamic_mfg && mfgunlock::framecount::g_dynamic_applied.load(std::memory_order_relaxed)) {
    ImGui::TextUnformatted("Dynamic MFG: active");
  } else if (dynamic_mfg && mfgunlock::framecount::g_dynamic_fell_back.load(std::memory_order_relaxed)) {
    ImGui::Text("Dynamic MFG: rejected (0x%x)",
                mfgunlock::framecount::g_dynamic_result.load(std::memory_order_relaxed));
  } else if (!dynamic_mfg) {
    // Nothing to report while disabled.
  } else if (!mfgunlock::framecount::DynamicStackReady()) {
    ImGui::TextDisabled("Dynamic MFG: pending");
  } else {
    ImGui::TextDisabled("Dynamic MFG: waiting for FG");
  }


  // Settings: visible only in Advanced; hidden settings keep their values.
  if (advanced && ImGui::CollapsingHeader("Image quality", ImGuiTreeNodeFlags_DefaultOpen)) {
  bool warp = g_validated_warp_blend.load(std::memory_order_relaxed);
  if (ImGui::Checkbox("Validated Warp Blend", &warp)) {
    g_validated_warp_blend.store(warp, std::memory_order_relaxed);
    reshade::set_config_value(nullptr, kConfigSection, "ValidatedWarpBlend", warp ? 1 : 0);
  }
  ImGui::Indent();
  // Keep the existing WarpDiagnosticMode INI values (2, 1, 0); only the
  // presentation order changes, with the normal quality mode first.
  static const char* kWarpModes[] = {
      "Validated Warp - improved thin geometry",
      "Blackwell baseline - NVIDIA blending, retargeted",
      "Redirect control - unchanged shader copy (test)",
  };
  static constexpr std::array<int, 3> kWarpValues = {
      static_cast<int>(mfgunlock::validatedwarp::Mode::kValidatedWarp),
      static_cast<int>(mfgunlock::validatedwarp::Mode::kBlackwellBaseline),
      static_cast<int>(mfgunlock::validatedwarp::Mode::kRedirectControl),
  };
  const int saved_warp_mode = g_validated_warp_mode.load(std::memory_order_relaxed);
  int warp_mode_index = 0;
  for (size_t i = 0; i < kWarpValues.size(); ++i) {
    if (kWarpValues[i] == saved_warp_mode) {
      warp_mode_index = static_cast<int>(i);
      break;
    }
  }
  ImGui::TextUnformatted("Warp blend mode");
  ImGui::PushItemWidth(-1.0f);
  if (ImGui::Combo("##mfg_warp_blend_mode", &warp_mode_index, kWarpModes,
                   ARRAYSIZE(kWarpModes))) {
    SetWarpMode(kWarpValues[static_cast<size_t>(warp_mode_index)]);
  }
  ImGui::PopItemWidth();
  DrawWarpStatus(warp);
  ImGui::Unindent();

  ImGui::Spacing();
  bool scatter = g_intermediate_scatter_retention.load(std::memory_order_relaxed);
  if (ImGui::Checkbox("Intermediate Scatter Retention", &scatter)) {
    g_intermediate_scatter_retention.store(scatter, std::memory_order_relaxed);
    reshade::set_config_value(nullptr, kConfigSection, "IntermediateScatterRetention",
                              scatter ? 1 : 0);
  }
  ImGui::TextDisabled("Preserves thin details in motion.");
  ImGui::Indent();
  // Display stronger edge protection first while preserving INI values (2, 1, 0).
  static const char* kBoundaryModes[] = {
      "Aggressive - prioritizes clean boundaries",
      "Balanced - detail and cleaner edges",
      "Off - original ISR, no edge protection",
  };
  static constexpr std::array<int, 3> kBoundaryValues = {
      static_cast<int>(mfgunlock::blackwelltemporal::BoundaryArtifactMode::kAggressive),
      static_cast<int>(mfgunlock::blackwelltemporal::BoundaryArtifactMode::kBalanced),
      static_cast<int>(mfgunlock::blackwelltemporal::BoundaryArtifactMode::kOff),
  };
  const int saved_boundary_mode = static_cast<int>(g_boundary_artifact_mode.load(std::memory_order_relaxed));
  int boundary_mode_index = 2;  // Off on an unrecognized saved value.
  for (size_t i = 0; i < kBoundaryValues.size(); ++i) {
    if (kBoundaryValues[i] == saved_boundary_mode) {
      boundary_mode_index = static_cast<int>(i);
      break;
    }
  }
  // BAM is meaningful only with ISR. Keep the saved mode for re-enabling ISR.
  ImGui::BeginDisabled(!scatter);
  ImGui::TextUnformatted("Boundary Artifact Mitigation (ISR only)");
  ImGui::PushItemWidth(-1.0f);
  if (ImGui::Combo("##mfg_isr_boundary_mode", &boundary_mode_index,
                   kBoundaryModes, ARRAYSIZE(kBoundaryModes))) {
    const auto parsed = mfgunlock::blackwelltemporal::ParseBoundaryArtifactMode(
        kBoundaryValues[static_cast<size_t>(boundary_mode_index)]);
    g_boundary_artifact_mode.store(static_cast<unsigned int>(parsed), std::memory_order_relaxed);
    reshade::set_config_value(nullptr, kConfigSection, "BoundaryArtifactMitigationMode",
                              static_cast<int>(parsed));
  }
  ImGui::PopItemWidth();
  ImGui::EndDisabled();
  // A failed try-lock is not a change of feature state. Keep exactly one row.
  static std::string isr_status = "ISR: INACTIVE";
  if (TryAcquireSRWLockShared(&g_provider_maintenance_lock)) {
    if (!g_blackwell_temporal_modules.empty()) {
      isr_status = std::string("ISR: ACTIVE | Boundary: ") +
          mfgunlock::blackwelltemporal::BoundaryArtifactModeName(
              g_blackwell_temporal_modules.front().boundary_mode);
      if (!scatter) isr_status += " | REQUESTED_OFF";
    } else if (!scatter) {
      isr_status = "ISR: DISABLED";
    } else if (!g_temporal_fix.load(std::memory_order_relaxed)) {
      isr_status = "ISR: DEPENDENCY_TEMPORAL";
    } else if (!g_blackwell_temporal_detail.empty()) {
      isr_status = g_midpoint_patched.load(std::memory_order_acquire)
          ? "ISR: UNAVAILABLE | MIDPOINT_ACTIVE" : "ISR: INACTIVE";
    } else {
      isr_status = "ISR: PENDING";
    }
    ReleaseSRWLockShared(&g_provider_maintenance_lock);
  }
  ImGui::TextUnformatted(isr_status.c_str());
  if (scatter && !g_temporal_fix.load(std::memory_order_relaxed))
    ImGui::TextDisabled("Requires Temporal correction.");
  ImGui::Unindent();

  {
    ImGui::Spacing();
    bool temporal = g_temporal_fix.load(std::memory_order_relaxed);
    if (ImGui::Checkbox("Temporal correction", &temporal)) {
      g_temporal_fix.store(temporal, std::memory_order_relaxed);
      reshade::set_config_value(nullptr, kConfigSection, "TemporalFix", temporal ? 1 : 0);
    }
    static std::string temporal_status = "Temporal: INACTIVE";
    if (TryAcquireSRWLockShared(&g_provider_maintenance_lock)) {
      if (!g_blackwell_temporal_modules.empty()) {
        temporal_status = "Temporal: BLACKWELL_SM" +
            std::to_string(g_blackwell_temporal_modules.front().target_sm) +
            " | providers " + std::to_string(g_blackwell_temporal_modules.size());
      } else if (!g_midpoint_modules.empty()) {
        temporal_status = "Temporal: MIDPOINT | providers " + std::to_string(g_midpoint_modules.size());
      } else {
        temporal_status = temporal ? "Temporal: NATIVE / no addon correction" : "Temporal: DISABLED";
      }
      ReleaseSRWLockShared(&g_provider_maintenance_lock);
    }
    ImGui::TextUnformatted(temporal_status.c_str());
  }
  }
  SaveQualityRequest();

  if (advanced && ImGui::CollapsingHeader("UI Composition")) {
  bool ui_composition = mfgunlock::framecount::g_ui_composition_enabled.load(
      std::memory_order_relaxed);
  if (ImGui::Checkbox("Automatic UI Composition", &ui_composition)) {
    mfgunlock::framecount::g_ui_composition_enabled.store(
        ui_composition, std::memory_order_relaxed);
    mfgunlock::framecount::NotifyUiCompositionChanged();
    reshade::set_config_value(nullptr, kConfigSection, "UIComposition",
                              ui_composition ? 1 : 0);
  }

  const auto ui_debug = mfgunlock::framecount::ReadUiDebugSnapshot();
  if (!ui_composition) {
    // No status row when disabled.
  } else if (mfgunlock::framecount::g_ui_composition_fell_back.load(
                 std::memory_order_relaxed)) {
    ImGui::Text("UI Composition: REJECTED 0x%x | NATIVE_FALLBACK",
                mfgunlock::framecount::g_ui_composition_result.load(
                    std::memory_order_relaxed));
  } else if (mfgunlock::framecount::g_ui_composition_applied.load(
                 std::memory_order_relaxed)) {
    if (mfgunlock::framecount::g_ui_pair_eligible.load(std::memory_order_relaxed)) {
      ImGui::TextUnformatted("UI Composition: ACTIVE_PAIR");
    } else if (mfgunlock::framecount::g_ui_native_hudless_fallback.load(
                   std::memory_order_relaxed)) {
      ImGui::TextDisabled(
          "UI Composition: HUDLESS_ONLY");
    } else if (ui_debug.suppression_active) {
      ImGui::TextDisabled("UI Composition: GUARDED");
    } else {
      ImGui::TextDisabled("UI Composition: WAIT_PAIR");
    }
  }
  bool inject_ui_candidate =
      mfgunlock::framecount::g_ui_candidate_injection_enabled.load(
          std::memory_order_relaxed);
  if (ImGui::Checkbox("Inject detected UI for DLSS-G", &inject_ui_candidate)) {
    mfgunlock::framecount::g_ui_candidate_injection_enabled.store(
        inject_ui_candidate, std::memory_order_relaxed);
    mfgunlock::framecount::NotifyUiCandidateInjectionChanged();
    reshade::set_config_value(nullptr, kConfigSection, "UICandidateInjection",
                              inject_ui_candidate ? 1 : 0);
  }

  }

  if (advanced && ImGui::CollapsingHeader("Runtime & compatibility")) {
  static const char* kRuntimeModes[] = {
      "Game default", "Prefer local runtime", "Force NVIDIA OTA runtime"};
  int runtime_mode = static_cast<int>(
      mfgunlock::framecount::g_runtime_selection_mode.load(std::memory_order_relaxed));
  bool runtime_changed = false;
  runtime_changed = ImGui::Combo("Streamline runtime", &runtime_mode, kRuntimeModes, ARRAYSIZE(kRuntimeModes));
  if (runtime_changed) {
    mfgunlock::framecount::g_runtime_selection_mode.store(
        static_cast<unsigned int>(runtime_mode), std::memory_order_relaxed);
    reshade::set_config_value(nullptr, kConfigSection, "RuntimeSelectionMode", runtime_mode);
  }

  {
    int count = static_cast<int>(g_max_count.load(std::memory_order_relaxed));
    if (ImGui::InputInt("Reported MultiFrameCountMax (0 = native)", &count,
                        0, 0, ImGuiInputTextFlags_EnterReturnsTrue)) {
      count = (std::max)(0, (std::min)(count, 5));
      g_max_count.store(static_cast<unsigned int>(count), std::memory_order_relaxed);
      mfgunlock::framecount::NotifyLiveOptionsChanged();
      reshade::set_config_value(nullptr, kConfigSection, "MaxCount", count);
    }

    bool flip_off = g_force_flip_meter_off.load(std::memory_order_relaxed);
    if (ImGui::Checkbox("Force legacy software pacing", &flip_off)) {
      g_force_flip_meter_off.store(flip_off, std::memory_order_relaxed);
      mfgunlock::framecount::NotifyLiveOptionsChanged();
      reshade::set_config_value(nullptr, kConfigSection, "ForceFlipMeteringOff", flip_off ? 1 : 0);
    }

    if (g_flip_meter_rollback_pending.load(std::memory_order_acquire)) {
      ImGui::TextDisabled("Software pacing: reverting.");
    } else if (g_flip_meter_patched.load(std::memory_order_acquire)) {
      ImGui::TextUnformatted("Software pacing patch: applied");
    } else if (g_flip_meter_failed.load(std::memory_order_acquire)) {
      ImGui::TextDisabled("Software pacing: unavailable.");
    } else {
      ImGui::TextDisabled("Software pacing: inactive.");
    }
  }
  if (mfgunlock::framecount::g_state_seen.load(std::memory_order_relaxed)) {
    const unsigned int status =
        mfgunlock::framecount::g_dlssg_status.load(std::memory_order_relaxed);
    if (status != 0) ImGui::Text("DLSS-G runtime status: 0x%x.", status);
  }

  float dynamic_target = mfgunlock::framecount::g_dynamic_target_fps.load(std::memory_order_relaxed);
  static bool invalid_dynamic_target = false;
  if (ImGui::InputFloat("Dynamic output target (0 = Auto)", &dynamic_target, 0.0f, 0.0f, "%.9g",
                        ImGuiInputTextFlags_EnterReturnsTrue)) {
    invalid_dynamic_target = !mfgunlock::runtimecontrols::ValidDynamicTarget(dynamic_target);
    if (!invalid_dynamic_target) {
      mfgunlock::framecount::g_dynamic_target_fps.store(dynamic_target, std::memory_order_relaxed);
      mfgunlock::framecount::NotifyDynamicModeChanged();
      const auto encoded = mfgunlock::runtimecontrols::FormatDynamicTarget(dynamic_target);
      reshade::set_config_value(nullptr, kConfigSection, "DynamicTargetFPS", encoded.c_str());
    }
  }
  if (invalid_dynamic_target)
    ImGui::Text("Invalid FPS target");

  }

  if (advanced && ImGui::CollapsingHeader("Diagnostics")) {
    // Advanced header details remain available here (H4A, H6A).
    mfgunlock::streamline::DrawAbiDiagnostics();
    const auto runtime_snapshot = mfgunlock::runtime::Read();
    HMODULE interposer = mfgunlock::streamline::internal::InterposerModule();
    const auto contract = mfgunlock::streamline::internal::GetInterposerContract(interposer);
    const auto compat_ngx = mfgunlock::ngx::internal::LifecycleSnapshot();
    ImGui::Text("Provider preparation: %s | identity %016llx",
      mfgunlock::runtime::ProviderReady() ? "STRUCTURAL_PREPARED" :
          mfgunlock::runtime::FallbackReasonName(runtime_snapshot.fallback_reason),
      static_cast<unsigned long long>(runtime_snapshot.provider_identity));
    ImGui::Text("Admission: %s | NGX query: %s",
      mfgunlock::compatibility::Name(contract.admission.state),
      mfgunlock::ngx::g_status.capabilities_seen.load(std::memory_order_acquire)
          ? "yes" : "--");
    const bool mfg_callable = mfgunlock::framecount::internal::RealSetOptions() != nullptr;
    ImGui::Text("Native MFG request: %s | Preset observer: %s",
      !g_enabled.load(std::memory_order_relaxed) ? "DISABLED" :
          mfg_callable ? "CALLABLE" : "WAITING FOR SETTER",
      mfgunlock::fgpreset::HasObserver() ? "AVAILABLE" : "UNAVAILABLE");
    ImGui::Text("Lifecycle: %s | slInit: %s | NGX Creates: %u",
      mfgunlock::streamline::internal::LifecycleName(
          mfgunlock::streamline::internal::g_lifecycle_state.load(std::memory_order_acquire)),
      mfgunlock::streamline::internal::g_init_observed.load(std::memory_order_acquire)
          ? "yes" : "--", compat_ngx.creates);
  bool verbose_diagnostics = mfgunlock::diagnostic::g_verbose.load(std::memory_order_relaxed);
  if (ImGui::Checkbox("Verbose diagnostics", &verbose_diagnostics)) {
    mfgunlock::diagnostic::g_verbose.store(verbose_diagnostics, std::memory_order_relaxed);
    reshade::set_config_value(nullptr, kConfigSection, "VerboseDiagnostics",
                              verbose_diagnostics ? 1 : 0);
  }
    const auto ui_debug = mfgunlock::framecount::ReadUiDebugSnapshot();
    const bool inject_ui_candidate =
        mfgunlock::framecount::g_ui_candidate_injection_enabled.load(std::memory_order_relaxed);
  if (mfgunlock::framecount::g_dynamic_support_seen.load(std::memory_order_acquire)) {
    ImGui::Text("Native capability report: %s",
        mfgunlock::framecount::g_dynamic_supported.load(std::memory_order_relaxed)
            ? "supported" : "unsupported");
  } else if (mfgunlock::framecount::g_dynamic_probe_attempted.load(std::memory_order_acquire)) {
    ImGui::Text("Native capability probe: 0x%x",
        mfgunlock::framecount::g_dynamic_probe_result.load(std::memory_order_relaxed));
  } else {
    ImGui::TextDisabled("Native capability report: not yet observed");
  }
  {
    ImGui::Text("R6 trials: GetState %llu | Dynamic %llu | repeats avoided %llu / %llu",
        static_cast<unsigned long long>(mfgunlock::framecount::g_dynamic_probe_calls.load()),
        static_cast<unsigned long long>(mfgunlock::framecount::g_dynamic_request_calls.load()),
        static_cast<unsigned long long>(mfgunlock::framecount::g_dynamic_suppressed_repeats.load()),
        static_cast<unsigned long long>(mfgunlock::framecount::g_fixed_suppressed_repeats.load()));
  }

  {
    const float ui_debug_height = ImGui::GetTextLineHeightWithSpacing() * 10.0f;
    const bool pair_now =
        mfgunlock::framecount::g_ui_pair_eligible.load(std::memory_order_relaxed);
    const bool native_fallback =
        mfgunlock::framecount::g_ui_native_hudless_fallback.load(std::memory_order_relaxed);
    const bool composition_applied =
        mfgunlock::framecount::g_ui_composition_applied.load(std::memory_order_relaxed);
    const bool composition_fell_back =
        mfgunlock::framecount::g_ui_composition_fell_back.load(std::memory_order_relaxed);
    const auto output_state = mfgunlock::outputstate::Read();
    const bool output_known = output_state.known;
    const uint32_t issue_mask =
        mfgunlock::framecount::g_ui_issue_mask.load(std::memory_order_relaxed);
    const uint64_t suppressed_tags =
        mfgunlock::framecount::g_ui_optional_tags_suppressed.load(std::memory_order_relaxed);

    ImGui::BeginChild("##mfgunlock_ui_debug", ImVec2(0.0f, ui_debug_height), true,
                      ImGuiWindowFlags_AlwaysVerticalScrollbar);
    ImGui::PushTextWrapPos(0.0f);

    ImGui::TextUnformatted("Composition & inputs");
    ImGui::Separator();
    float slot_y = ImGui::GetCursorPosY();
    ImGui::TextWrapped(
        "Output: %s. SetOptions accepted: %s. Options observed: %s. Viewports: %u.",
        !output_known ? "unknown" : mfgunlock::outputstate::EncodingName(output_state.encoding),
        composition_applied ? "yes" : "no", ui_debug.options_seen ? "yes" : "no",
        ui_debug.viewport_count);
    ReserveDebugRows(slot_y, 2.0f);
    slot_y = ImGui::GetCursorPosY();
    ImGui::TextWrapped(
        "Pair now: %s. Pair seen: %s. Native HUD-less fallback: %s.",
        pair_now ? "yes" : "no", ui_debug.pair_eligible ? "yes" : "no",
        native_fallback ? "yes" : "no");
    ReserveDebugRows(slot_y, 2.0f);
    slot_y = ImGui::GetCursorPosY();
    ImGui::TextWrapped(
        "Inputs: HUD-less %s (%llu), UI Color+Alpha %s (%llu), UI Alpha %s (%llu).",
        ui_debug.hudless_seen ? "yes" : "no",
        mfgunlock::framecount::g_ui_hudless_tags_seen.load(std::memory_order_relaxed),
        ui_debug.ui_color_alpha_seen ? "yes" : "no",
        mfgunlock::framecount::g_ui_color_alpha_tags_seen.load(std::memory_order_relaxed),
        ui_debug.ui_alpha_seen ? "yes" : "no",
        mfgunlock::framecount::g_ui_alpha_tags_seen.load(std::memory_order_relaxed));
    ReserveDebugRows(slot_y, 3.0f);
    slot_y = ImGui::GetCursorPosY();
    if (composition_fell_back || ui_debug.invalid || ui_debug.suppression_active ||
        issue_mask != 0 || suppressed_tags != 0) {
      ImGui::TextWrapped(
          "Guard: runtime fallback %s, invalid %s, suppression %s, issue mask 0x%x, "
          "suppressed optional tags %llu.",
          composition_fell_back ? "yes" : "no", ui_debug.invalid ? "yes" : "no",
          ui_debug.suppression_active ? "yes" : "no", issue_mask, suppressed_tags);
    } else {
      ImGui::TextDisabled("Guard: clear.");
    }
    ReserveDebugRows(slot_y, 3.0f);

    const auto candidate = ReadUiCandidateDebugSnapshot();
    ImGui::Spacing();
    ImGui::TextUnformatted("Detected UI target");
    ImGui::Separator();
    slot_y = ImGui::GetCursorPosY();
    if (candidate.present) {
      const auto candidate_encoding = static_cast<mfgunlock::outputstate::UiEncoding>(
          candidate.ui_encoding);
      const bool encoding_compatible = mfgunlock::outputstate::IsUiEncodingCompatible(
          output_state, candidate_encoding, false);
      const auto alpha_convention = static_cast<mfgunlock::uialpha::AlphaConvention>(
          candidate.alpha_convention);
      ImGui::TextWrapped(
          "Candidate: %ux%u %s (format %u), encoding %s. Stable: %s (%u frames). "
          "Safe: %s. Output-compatible: %s. Alpha: %s (%u draws, %u unproven). "
          "Age: %llums.",
          candidate.width, candidate.height, UiCandidateFormatName(candidate.format),
          candidate.format, mfgunlock::outputstate::UiEncodingName(candidate_encoding),
          candidate.stable ? "yes" : "no", candidate.stable_frames,
          candidate.safe ? "yes" : "no", encoding_compatible ? "yes" : "no",
          mfgunlock::uialpha::AlphaConventionName(alpha_convention),
          candidate.alpha_draws, candidate.alpha_unproven_draws,
          static_cast<unsigned long long>(candidate.age_ms));
    } else {
      ImGui::TextWrapped("Candidate: UNAVAILABLE");
    }
    ReserveDebugRows(slot_y, 3.0f);
    slot_y = ImGui::GetCursorPosY();
    if (candidate.family_valid) {
      ImGui::TextWrapped(
          "Family: %ux%u, format %u; handovers %llu; age %llums.",
          candidate.family_width, candidate.family_height, candidate.family_format,
          static_cast<unsigned long long>(candidate.family_handoffs),
          static_cast<unsigned long long>(candidate.family_age_ms));
    } else {
      ImGui::TextWrapped("Family: UNKNOWN | handovers %llu",
          static_cast<unsigned long long>(candidate.family_handoffs));
    }
    ReserveDebugRows(slot_y, 2.0f);
    const bool candidate_guard = candidate.present &&
        (candidate.reject_reasons != mfgunlock::uicandidate::kAccept ||
         candidate.late_writes != 0 ||
         candidate.swapchain || candidate.already_tagged);
    slot_y = ImGui::GetCursorPosY();
    if (candidate.ambiguous || candidate.overflow || candidate_guard) {
      ImGui::TextWrapped(
          "Candidate guard: ambiguous %s, overflow %s, reject mask 0x%x, late writes %u, "
          "swapchain %s, native-tagged %s.",
          candidate.ambiguous ? "yes" : "no", candidate.overflow ? "yes" : "no",
          candidate.reject_reasons, candidate.late_writes,
          candidate.swapchain ? "yes" : "no", candidate.already_tagged ? "yes" : "no");
    } else {
      ImGui::TextDisabled("Candidate guard: clear.");
    }
    ReserveDebugRows(slot_y, 3.0f);

    const auto submission_path = static_cast<mfgunlock::uisubmission::Path>(
        mfgunlock::framecount::g_ui_candidate_path.load(std::memory_order_relaxed));
    slot_y = ImGui::GetCursorPosY();
    ImGui::TextWrapped(
        "Automatic path: %s. Alpha backend prepare %llu ok / %llu failed; extraction %llu ok / %llu failed.",
        mfgunlock::uisubmission::PathName(submission_path),
        g_ui_alpha_prepare_successes.load(std::memory_order_relaxed),
        g_ui_alpha_prepare_failures.load(std::memory_order_relaxed),
        g_ui_alpha_extractions.load(std::memory_order_relaxed),
        g_ui_alpha_extraction_failures.load(std::memory_order_relaxed));
    ReserveDebugRows(slot_y, 3.0f);

    const bool injection_active =
        mfgunlock::framecount::g_ui_candidate_active.load(std::memory_order_relaxed);
    const bool options_synced =
        mfgunlock::framecount::g_ui_candidate_options_synced.load(std::memory_order_acquire);
    const bool hard_declined =
        mfgunlock::framecount::g_ui_candidate_runtime_declined.load(std::memory_order_acquire);
    const uint32_t retry_frames =
        mfgunlock::framecount::g_ui_candidate_retry_frames.load(std::memory_order_acquire);
    const uint32_t consecutive_failures =
        mfgunlock::framecount::g_ui_candidate_consecutive_failures.load(std::memory_order_acquire);
    const uint64_t injection_failures =
        mfgunlock::framecount::g_ui_candidate_injection_failures.load(std::memory_order_relaxed);
    const uint64_t missing_command_buffer =
        mfgunlock::framecount::g_ui_candidate_missing_command_buffer.load(
            std::memory_order_relaxed);

    ImGui::Spacing();
    ImGui::TextUnformatted("Injection & recovery");
    ImGui::Separator();
    slot_y = ImGui::GetCursorPosY();
    ImGui::TextWrapped(
        "Injection: enabled %s, active %s, options synced %s, candidate format %u, hard declined %s.",
        inject_ui_candidate ? "yes" : "no", injection_active ? "yes" : "no",
        options_synced ? "yes" : "no",
        mfgunlock::framecount::g_ui_candidate_format.load(std::memory_order_relaxed),
        hard_declined ? "yes" : "no");
    ReserveDebugRows(slot_y, 3.0f);
    slot_y = ImGui::GetCursorPosY();
    ImGui::TextWrapped(
        "Tags: injected %llu, failures %llu, native fallbacks %llu, full batches %llu, last result 0x%x.",
        mfgunlock::framecount::g_ui_candidate_tags_injected.load(std::memory_order_relaxed),
        injection_failures,
        mfgunlock::framecount::g_ui_candidate_native_fallbacks.load(std::memory_order_relaxed),
        mfgunlock::framecount::g_ui_candidate_full_batches.load(std::memory_order_relaxed),
        mfgunlock::framecount::g_ui_candidate_last_result.load(std::memory_order_relaxed));
    ReserveDebugRows(slot_y, 3.0f);
    slot_y = ImGui::GetCursorPosY();
    if (retry_frames != 0 || consecutive_failures != 0 || injection_failures != 0 ||
        missing_command_buffer != 0 || hard_declined) {
      ImGui::TextWrapped(
          "Recovery: cooldown %u frames, consecutive failures %u, recoveries %llu, "
          "missing command buffer %llu.",
          retry_frames, consecutive_failures,
          mfgunlock::framecount::g_ui_candidate_recoveries.load(std::memory_order_relaxed),
          missing_command_buffer);
    } else {
      ImGui::TextDisabled("Recovery: idle.");
    }
    ReserveDebugRows(slot_y, 3.0f);
    ImGui::PopTextWrapPos();
    ImGui::EndChild();
  }

    const auto lifecycle = mfgunlock::ngx::internal::LifecycleSnapshot();
    namespace qc = mfgunlock::qualityconfig;
    const auto requested_quality = qc::g_requested.load(std::memory_order_acquire);
    const auto prepared_quality = qc::g_prepared.load(std::memory_order_acquire);
    ImGui::Text("Quality: %s | epoch %llu",
        qc::Pending(requested_quality, prepared_quality) ? "PENDING" : "UP_TO_DATE",
        static_cast<unsigned long long>(lifecycle.epoch));
    ImGui::Text("Runtime options: %s | revision %llu",
        mfgunlock::framecount::LiveOptionsActionText() ? "PENDING" : "UP_TO_DATE",
        static_cast<unsigned long long>(mfgunlock::framecount::g_options_revision.load()));
  {
    const float backend_debug_height = ImGui::GetTextLineHeightWithSpacing() * 7.0f;
    ImGui::BeginChild("##mfgunlock_backend_debug",
                      ImVec2(0.0f, backend_debug_height), true,
                      ImGuiWindowFlags_AlwaysVerticalScrollbar);
    ImGui::PushTextWrapPos(0.0f);

    DrawMfgRuntimeDiagnostics();
    ImGui::Spacing();
    ImGui::TextUnformatted("Lifecycle & quality");
    ImGui::Separator();
    float backend_slot_y = ImGui::GetCursorPosY();
    ImGui::TextWrapped(
        "NGX: handles %u, Evaluate %u, Create %u, Release %u, epoch %llu%s.",
        lifecycle.handles, lifecycle.evaluates, lifecycle.creates, lifecycle.releases,
        lifecycle.epoch, lifecycle.uncertain ? " | TRACKING_INCOMPLETE" : "");
    ReserveDebugRows(backend_slot_y, 2.0f);
    backend_slot_y = ImGui::GetCursorPosY();
    ImGui::TextWrapped(
        "Quality: requested 0x%x | prepared 0x%x",
        requested_quality, prepared_quality);
    ReserveDebugRows(backend_slot_y, 2.0f);

    ImGui::Spacing();
    ImGui::TextUnformatted("Provider redirects");
    ImGui::Separator();
    if (TryAcquireSRWLockShared(&g_provider_maintenance_lock)) {
      auto draw_redirects = [](const auto& redirects) {
        for (const auto& redirect : redirects) {
          ImGui::Text("Allocation: %p | descriptors %zu", redirect.allocation,
                      redirect.descriptors.size());
          for (const auto& descriptor : redirect.descriptors)
            ImGui::Text("Slot: %p | original 0x%llx | protection 0x%lx",
                static_cast<void*>(descriptor.slot),
                static_cast<unsigned long long>(descriptor.original),
                static_cast<unsigned long>(descriptor.protection));
        }
      };
      for (const auto& patch : g_blackwell_temporal_modules) {
        ImGui::Text("Temporal: BLACKWELL_SM%u | module %p | boundary %s",
            patch.target_sm, static_cast<void*>(patch.module),
            mfgunlock::blackwelltemporal::BoundaryArtifactModeName(patch.boundary_mode));
        draw_redirects(patch.redirects);
      }
      for (const auto& patch : g_midpoint_modules) {
        ImGui::Text("Temporal: MIDPOINT | module %p | allocation %p | slots %zu",
            static_cast<void*>(patch.module), patch.allocation, patch.patches.size());
        for (const auto& descriptor : patch.patches)
          ImGui::Text("Slot: %p | original 0x%llx", static_cast<void*>(descriptor.slot),
              static_cast<unsigned long long>(descriptor.original));
      }
      for (const auto& patch : g_validated_warp_modules) {
        ImGui::Text("Warp: %s | SM%u | module %p | provider %s",
            mfgunlock::validatedwarp::ModeName(patch.mode), patch.target_sm,
            static_cast<void*>(patch.module), patch.provider_version.c_str());
        draw_redirects(patch.redirects);
      }
      ImGui::Text("Providers: temporal %zu | midpoint %zu | warp %zu",
          g_blackwell_temporal_modules.size(), g_midpoint_modules.size(),
          g_validated_warp_modules.size());
      ReleaseSRWLockShared(&g_provider_maintenance_lock);
    } else {
      ImGui::TextDisabled("Provider redirects: UPDATING");
    }

    ImGui::PopTextWrapPos();
    ImGui::EndChild();
  }  }
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
    const int normalized = (std::max)(0, (std::min)(value, 5));
    g_max_count.store(static_cast<unsigned int>(normalized), std::memory_order_relaxed);
    if (normalized != value)
      reshade::set_config_value(nullptr, kConfigSection, "MaxCount", normalized);
  }
  if (reshade::get_config_value(nullptr, kConfigSection, "ForceFlipMeteringOff", value)) {
    g_force_flip_meter_off.store(value != 0, std::memory_order_relaxed);
  }
  if (reshade::get_config_value(nullptr, kConfigSection, "TemporalFix", value)) {
    g_temporal_fix.store(value != 0, std::memory_order_relaxed);
  }
  const bool has_scatter = reshade::get_config_value(
      nullptr, kConfigSection, "IntermediateScatterRetention", value);
  if (has_scatter) {
    g_intermediate_scatter_retention.store(value != 0, std::memory_order_relaxed);
  }
  int boundary_mode = 0;
  const bool has_boundary_mode = reshade::get_config_value(
      nullptr, kConfigSection, "BoundaryArtifactMitigationMode", boundary_mode);
  const auto parsed_boundary_mode =
      mfgunlock::blackwelltemporal::ParseBoundaryArtifactMode(boundary_mode);
  g_boundary_artifact_mode.store(
      static_cast<unsigned int>(parsed_boundary_mode), std::memory_order_relaxed);
  if (has_boundary_mode && boundary_mode != static_cast<int>(parsed_boundary_mode)) {
    reshade::set_config_value(nullptr, kConfigSection, "BoundaryArtifactMitigationMode",
                              static_cast<int>(parsed_boundary_mode));
  } else if (!has_boundary_mode && has_scatter &&
             g_intermediate_scatter_retention.load(std::memory_order_relaxed)) {
    reshade::set_config_value(nullptr, kConfigSection, "BoundaryArtifactMitigationMode",
                              static_cast<int>(mfgunlock::blackwelltemporal::BoundaryArtifactMode::kOff));
  }
  if (reshade::get_config_value(nullptr, kConfigSection, "ValidatedWarpBlend", value)) {
    g_validated_warp_blend.store(value != 0, std::memory_order_relaxed);
  }
  int debug_options = 0;
  reshade::get_config_value(nullptr, kConfigSection, "ShowDebugOptions", debug_options);
  g_show_debug_options.store(debug_options != 0, std::memory_order_relaxed);
  int verbose_diagnostics = 0;
  reshade::get_config_value(nullptr, kConfigSection, "VerboseDiagnostics", verbose_diagnostics);
  mfgunlock::diagnostic::g_verbose.store(verbose_diagnostics != 0, std::memory_order_relaxed);
  int ui_candidate_injection = 1;
  const bool has_ui_candidate_injection = reshade::get_config_value(
      nullptr, kConfigSection, "UICandidateInjection", ui_candidate_injection);
  mfgunlock::framecount::g_ui_candidate_injection_enabled.store(
      ui_candidate_injection != 0, std::memory_order_relaxed);
  if (!has_ui_candidate_injection)
    reshade::set_config_value(nullptr, kConfigSection, "UICandidateInjection", 1);
  int warp_mode = static_cast<int>(mfgunlock::validatedwarp::Mode::kValidatedWarp);
  const bool has_mode = reshade::get_config_value(nullptr, kConfigSection, "WarpDiagnosticMode", warp_mode);
  const int configured_mode = static_cast<int>(mfgunlock::validatedwarp::ConfiguredMode(warp_mode));
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
  std::array<char, 128> dynamic_target_text{};
  size_t dynamic_target_size = dynamic_target_text.size() - 1;
  if (reshade::get_config_value(nullptr, kConfigSection, "DynamicTargetFPS",
                               dynamic_target_text.data(), &dynamic_target_size)) {
    float target = 0.0f;
    if (mfgunlock::runtimecontrols::ParseDynamicTarget(dynamic_target_text.data(), target))
      mfgunlock::framecount::g_dynamic_target_fps.store(target, std::memory_order_relaxed);
    else
      reshade::log::message(reshade::log::level::warning,
          "mfgunlock: invalid DynamicTargetFPS; expected 0 (Auto) or a finite positive float. Previous value retained.");
  }
  if (reshade::get_config_value(nullptr, kConfigSection, "UIComposition", value)) {
    mfgunlock::framecount::g_ui_composition_enabled.store(
        value != 0, std::memory_order_relaxed);
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
    const int normalized = value < 2 ? 0 : (std::min)(value, 6);
    if (normalized != value)
      reshade::set_config_value(nullptr, kConfigSection, "ForceMultiplier", normalized);
    mfgunlock::framecount::g_force_multiplier.store(static_cast<unsigned int>(normalized),
                                                    std::memory_order_relaxed);
    mfgunlock::framecount::NotifyFixedMultiplierChanged();
  }
  mfgunlock::qualityconfig::g_requested.store(mfgunlock::qualityconfig::Encode(RequestedQuality()));
}

void TryAttachRuntime() {
  if (g_bootstrap_complete.load(std::memory_order_acquire) ||
      g_bootstrap_shutting_down.load(std::memory_order_acquire)) return;
  bool expected = false;
  if (!g_bootstrap_busy.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) return;
  struct Release {
    ~Release() { g_bootstrap_busy.store(false, std::memory_order_release); }
  } release;
  try {
    mfgunlock::provider::Log("DLSS_STARTUP_D2 BOOTSTRAP_BEGIN");
    LoadConfig();
    mfgunlock::runtime::Attach(mfgunlock::architecture::ActiveProfile());
    EnsureEarlyLoadConfig();
    mfgunlock::streamline::Initialize(g_self_module);
    mfgunlock::loadhook::TryInstall();
    g_streamline_scan_pending.store(false, std::memory_order_release);
    g_ngx_scan_pending.store(false, std::memory_order_release);
    mfgunlock::ngx::EnsureEntryHooks();
    mfgunlock::streamline::internal::InstallLegacyInitHook();
    mfgunlock::streamline::internal::InstallSupportHook();
    RunProviderMaintenance();
    if (g_force_flip_meter_off.load(std::memory_order_relaxed)) TryPatchFlipMetering();
    mfgunlock::framecount::TryInstall();
    PollRuntimeModuleChanges(true);
    g_bootstrap_complete.store(true, std::memory_order_release);
    mfgunlock::provider::Log("runtime attach complete; startup missed before attach remains unknown");
  } catch (...) {
    mfgunlock::provider::Log("runtime attach failed; next safe callback may retry", true);
  }
}

DWORD WINAPI DeferredBootstrap(LPVOID held_module) {
  TryAttachRuntime();
  DumpDlssStartupEvidence("bootstrap-worker");
  FreeLibraryAndExitThread(static_cast<HMODULE>(held_module), 0);
}

}  // namespace

extern "C" __declspec(dllexport) constexpr const char* NAME = "MFG Unlock";
extern "C" __declspec(dllexport) constexpr const char* DESCRIPTION =
    "Native NVIDIA DLSS frame generation compatibility and multiplier control";

// Observer registration is symmetric. Callback emission holds a shared guard;
// unregister takes the exclusive guard, so a diagnostic module can disconnect
// without permanently pinning either addon in the process.
extern "C" __declspec(dllexport) bool MfgUnlockSetCountObserver(
    uint32_t version, mfgunlock::countobserver::Callback callback) {
  if (version != mfgunlock::countobserver::kVersion) return false;
  return mfgunlock::countobserver::SetCallback(callback);
}

extern "C" __declspec(dllexport) bool MfgUnlockSetEvaluateObserver(
    uint32_t version, mfgunlock::diagnostic::EvaluateCallback callback) {
  if (version != mfgunlock::diagnostic::kEvaluateVersion) return false;
  return mfgunlock::diagnostic::SetEvaluateCallback(callback);
}

extern "C" __declspec(dllexport) bool MfgUnlockSetLifecycleObserver(
    uint32_t version, mfgunlock::diagnostic::LifecycleCallback callback) {
  if (version != mfgunlock::diagnostic::kLifecycleVersion) return false;
  return mfgunlock::diagnostic::SetLifecycleCallback(callback);
}

extern "C" __declspec(dllexport) bool MfgUnlockGetDiagnosticState(
    uint32_t version, void* output) {
  if ((version != mfgunlock::diagnostic::kStateVersion1 &&
       version != mfgunlock::diagnostic::kStateVersion2 &&
       version != mfgunlock::diagnostic::kStateVersion) ||
      output == nullptr ||
      mfgunlock::provider::internal::g_registry_failed.load(std::memory_order_acquire)) {
    return false;
  }

  mfgunlock::diagnostic::DiagnosticState state;
  const auto* profile = mfgunlock::architecture::ActiveProfile();
  if (profile != nullptr) {
    state.architecture = static_cast<uint32_t>(profile->architecture);
    state.target_sm = profile->target_sm;
  }
  if (!TryAcquireSRWLockShared(&g_provider_maintenance_lock)) return false;
  if (!g_blackwell_temporal_modules.empty()) {
    state.temporal_backend = mfgunlock::diagnostic::TemporalBackend::kBlackwell;
    state.temporal_target_sm = g_blackwell_temporal_modules.front().target_sm;
    state.boundary_mode = static_cast<uint32_t>(g_blackwell_temporal_modules.front().boundary_mode);
  } else if (g_midpoint_patched.load(std::memory_order_acquire)) {
    state.temporal_backend = mfgunlock::diagnostic::TemporalBackend::kMidpoint;
    state.temporal_target_sm = profile ? profile->target_sm : 0;
  }
  if (!g_validated_warp_modules.empty()) {
    state.warp_applied = 1;
    state.warp_target_sm = g_validated_warp_modules.front().target_sm;
  }
  ReleaseSRWLockShared(&g_provider_maintenance_lock);

  state.dynamic_requested = mfgunlock::framecount::g_dynamic_mfg_enabled.load(std::memory_order_relaxed);
  state.dynamic_applied = mfgunlock::framecount::g_dynamic_applied.load(std::memory_order_relaxed);
  state.fixed_multiplier = mfgunlock::framecount::g_force_multiplier.load(std::memory_order_relaxed);
  state.effective_generated = mfgunlock::framecount::g_last_effective_generated.load(std::memory_order_relaxed);
  state.preset_requested = static_cast<int32_t>(mfgunlock::fgpreset::g_requested.load(std::memory_order_relaxed));
  state.preset_supplied = mfgunlock::fgpreset::g_last_supplied.load(std::memory_order_relaxed);
  state.preset_applied = mfgunlock::fgpreset::g_last_applied.load(std::memory_order_relaxed);
  state.ui_composition_requested = mfgunlock::framecount::g_ui_composition_enabled.load(std::memory_order_relaxed);
  state.ui_composition_applied = mfgunlock::framecount::g_ui_composition_applied.load(std::memory_order_relaxed);
  state.ui_composition_fallback = mfgunlock::framecount::g_ui_composition_fell_back.load(std::memory_order_relaxed);
  const auto provider_status = mfgunlock::provider::GetProviderStatus();
  if (provider_status.busy) return false;
  state.prepared_provider_count = provider_status.QualifiedCount();

  const uint32_t requested_mode =
      mfgunlock::framecount::g_last_requested_mode.load(std::memory_order_relaxed);
  const uint32_t requested_generated =
      mfgunlock::framecount::g_last_requested_generated.load(std::memory_order_relaxed);
  const uint32_t forwarded_mode =
      mfgunlock::framecount::g_last_forwarded_mode.load(std::memory_order_relaxed);
  const uint32_t forwarded_generated =
      mfgunlock::framecount::g_last_forwarded_generated.load(std::memory_order_relaxed);
  const uint32_t accepted_mode =
      mfgunlock::framecount::g_last_accepted_mode.load(std::memory_order_relaxed);
  const uint32_t accepted_generated =
      mfgunlock::framecount::g_last_accepted_generated.load(std::memory_order_relaxed);
  state.requested_mode_known = requested_mode != mfgunlock::diagnostic::kUnknown32;
  state.requested_mode = requested_mode;
  state.requested_generated_known = requested_generated != mfgunlock::diagnostic::kUnknown32;
  state.requested_generated = state.requested_generated_known ? requested_generated : 0;
  state.forwarded_mode_known = forwarded_mode != mfgunlock::diagnostic::kUnknown32;
  state.forwarded_mode = forwarded_mode;
  state.forwarded_generated_known = forwarded_generated != mfgunlock::diagnostic::kUnknown32;
  state.forwarded_generated = state.forwarded_generated_known ? forwarded_generated : 0;
  state.accepted_mode_known = accepted_mode != mfgunlock::diagnostic::kUnknown32;
  state.accepted_mode = accepted_mode;
  state.accepted_generated_known = accepted_generated != mfgunlock::diagnostic::kUnknown32;
  state.accepted_generated = state.accepted_generated_known ? accepted_generated : 0;
  // SetOptions eOk proves acceptance, not presentation. No public observed state
  // in this integration identifies the provider-applied multiplier or mode.
  state.provider_applied_generated_known = 0;
  state.provider_applied_generated = 0;
  state.observed_mode_known = 0;
  state.observed_mode = mfgunlock::diagnostic::kUnknown32;

  state.structural_max_generated =
      mfgunlock::framecount::g_streamline_max_generated.load(std::memory_order_relaxed);
  state.structural_max_known = state.structural_max_generated != 0;
  state.runtime_max_generated =
      mfgunlock::framecount::g_runtime_max_generated.load(std::memory_order_relaxed);
  state.runtime_max_known = mfgunlock::framecount::g_state_seen.load(std::memory_order_relaxed);
  state.effective_max_generated =
      mfgunlock::framecount::g_effective_max_generated.load(std::memory_order_relaxed);
  state.effective_max_known =
      mfgunlock::framecount::g_effective_max_seen.load(std::memory_order_acquire);
  state.dynamic_support_seen =
      mfgunlock::framecount::g_dynamic_support_seen.load(std::memory_order_acquire);
  state.dynamic_supported = state.dynamic_support_seen &&
      mfgunlock::framecount::g_dynamic_supported.load(std::memory_order_relaxed);

  const auto output_state = mfgunlock::outputstate::Read();
  state.hdr_state_seen = output_state.known;
  state.hdr_active = output_state.known && output_state.hdr;
  state.swapchain_color_space_known = output_state.color_space_known;
  state.swapchain_color_space = output_state.color_space_known
      ? output_state.color_space : mfgunlock::diagnostic::kUnknown32;
  // Streamline exposes buffer formats but no separate observed DLSS-G output
  // color-space state. Keep it unknown rather than infer it from the swapchain.
  state.dlssg_color_space_known = 0;
  state.dlssg_color_space = mfgunlock::diagnostic::kUnknown32;

  const auto ui_debug = mfgunlock::framecount::ReadUiDebugSnapshot();
  state.native_uir_observed = ui_debug.hudless_seen &&
      (ui_debug.ui_color_alpha_seen || ui_debug.ui_alpha_seen);
  const auto candidate = ReadUiCandidateDebugSnapshot();
  uint32_t auto_reject = candidate.present
      ? candidate.reject_reasons
      : mfgunlock::diagnostic::kUiAutoNoCandidate;
  if (!mfgunlock::framecount::g_ui_candidate_injection_enabled.load(std::memory_order_relaxed))
    auto_reject |= mfgunlock::diagnostic::kUiAutoDisabled;
  if (candidate.present && !mfgunlock::outputstate::IsUiEncodingCompatible(
          output_state,
          static_cast<mfgunlock::outputstate::UiEncoding>(candidate.ui_encoding), false))
    auto_reject |= mfgunlock::diagnostic::kUiAutoEncodingMismatch;
  if (!mfgunlock::framecount::g_ui_candidate_options_synced.load(std::memory_order_acquire))
    auto_reject |= mfgunlock::diagnostic::kUiAutoOptionsUnsynced;
  if (mfgunlock::framecount::g_ui_candidate_runtime_declined.load(std::memory_order_acquire))
    auto_reject |= mfgunlock::diagnostic::kUiAutoRuntimeDeclined;
  state.automatic_uir_reject_reasons = auto_reject;
  state.automatic_uir_eligible = candidate.safe && auto_reject == 0;
  state.automatic_uir_applied =
      mfgunlock::framecount::g_ui_candidate_active.load(std::memory_order_relaxed);
  state.automatic_uir_rejected = state.ui_composition_requested &&
      !state.automatic_uir_applied && auto_reject != 0;

  const auto contract = mfgunlock::integration::Read();
  state.frame_contract = static_cast<uint32_t>(contract.frame_index);
  state.resource_contract = static_cast<uint32_t>(contract.resources);
  state.dynamic_resolution_contract = static_cast<uint32_t>(contract.dynamic_resolution);
  state.queue_contract = static_cast<uint32_t>(contract.queue_contract);
  state.swapchain_contract = static_cast<uint32_t>(contract.swapchain);
  state.viewport_contract = static_cast<uint32_t>(contract.viewport_ownership);
  state.constants_frame_known = contract.constants_frame_known;
  state.constants_frame = contract.constants_frame;
  state.present_start_known = contract.present_start_known;
  state.present_start_frame = contract.present_start_frame;
  state.present_end_known = contract.present_end_known;
  state.present_end_frame = contract.present_end_frame;
  state.present_marker_mismatches = contract.present_marker_mismatches;
  state.constants_marker_mismatches = contract.constants_marker_mismatches;
  for (size_t i = 0; i < contract.resource.size() && i < 32; ++i) {
    const uint32_t bit = 1u << static_cast<uint32_t>(i);
    if (contract.resource[i].seen) state.resource_seen_mask |= bit;
    if (contract.resource[i].active) state.resource_active_mask |= bit;
    if (contract.resource[i].clears) state.resource_clear_mask |= bit;
    if (contract.resource[i].lifecycle_known) state.resource_lifecycle_known_mask |= bit;
    if (contract.resource[i].extent_known) state.resource_extent_known_mask |= bit;
    if (contract.resource[i].state_known) state.resource_state_known_mask |= bit;
    if (contract.resource[i].format_known) state.resource_format_known_mask |= bit;
  }
  state.dynamic_resolution_enabled = contract.dynamic_resolution_enabled;
  state.dynamic_res_width = contract.dynamic_res_width;
  state.dynamic_res_height = contract.dynamic_res_height;
  state.mvec_depth_width = contract.mvec_depth_width;
  state.mvec_depth_height = contract.mvec_depth_height;
  state.color_width = contract.color_width;
  state.color_height = contract.color_height;
  state.num_back_buffers = contract.num_back_buffers;
  state.queue_parallelism_known = contract.queue_parallelism_known;
  state.queue_parallelism_mode = contract.queue_parallelism_mode;
  state.completion_fence_known = contract.completion_fence_known;
  state.completion_fence = contract.completion_fence;
  state.completion_fence_value = contract.completion_fence_value;
  state.presenting_queue_seen = contract.presenting_queue_seen;
  state.presenting_queue = contract.presenting_queue;
  state.queue_destroy_seen = contract.queue_destroy_seen;
  state.swapchain_init_count = contract.swapchain_init_count;
  state.swapchain_destroy_count = contract.swapchain_destroy_count;
  state.swapchain_present_count = contract.swapchain_present_count;
  state.swapchain_recreation_count = contract.swapchain_recreation_count;
  state.swapchain_resize_count = contract.swapchain_resize_count;
  state.swapchain_buffer_count_known = contract.swapchain_buffer_count_known;
  state.swapchain_buffer_count = contract.swapchain_buffer_count;
  state.fullscreen_transition_known = contract.fullscreen_transition_known;
  state.waitable_object_ownership_known = contract.waitable_object_ownership_known;
  state.iflip_known = contract.iflip_known;
  state.dlssg_status_known = contract.status_known;
  state.dlssg_status_raw = contract.status_raw;
  state.dlssg_status_unknown_bits = contract.status_unknown_bits;
  state.fail_resolution_too_low = contract.fail_resolution_too_low;
  state.fail_reflex_missing = contract.fail_reflex_missing;
  state.fail_hdr_unsupported = contract.fail_hdr_unsupported;
  state.fail_constants_invalid = contract.fail_constants_invalid;
  state.fail_backbuffer_index_missing = contract.fail_backbuffer_index_missing;
  state.options_viewport_known = contract.options_viewport_known;
  state.options_viewport = contract.options_viewport;
  state.constants_viewport_known = contract.constants_viewport_known;
  state.constants_viewport = contract.constants_viewport;
  state.tag_viewport_known = contract.tag_viewport_known;
  state.tag_viewport = contract.tag_viewport;
  state.state_viewport_known = contract.state_viewport_known;
  state.state_viewport = contract.state_viewport;
  state.viewport_mismatches = contract.viewport_mismatches;
  state.set_options_thread_known = contract.set_options_thread_known;
  state.set_options_thread = contract.set_options_thread;
  state.present_thread_known = contract.present_thread_known;
  state.present_thread = contract.present_thread;
  state.set_options_on_present_thread = contract.set_options_on_present_thread;
  state.native_vsync_support_known = contract.native_vsync_support_known;
  state.native_vsync_support = contract.native_vsync_support;

  if (version == mfgunlock::diagnostic::kStateVersion1) {
    mfgunlock::diagnostic::DiagnosticStateV1 legacy;
    legacy.architecture = state.architecture;
    legacy.target_sm = state.target_sm;
    legacy.temporal_backend = state.temporal_backend;
    legacy.temporal_target_sm = state.temporal_target_sm;
    legacy.boundary_mode = state.boundary_mode;
    legacy.warp_applied = state.warp_applied;
    legacy.warp_target_sm = state.warp_target_sm;
    legacy.dynamic_requested = state.dynamic_requested;
    legacy.dynamic_applied = state.dynamic_applied;
    legacy.fixed_multiplier = state.fixed_multiplier;
    legacy.effective_generated = state.effective_generated;
    legacy.preset_requested = state.preset_requested;
    legacy.preset_supplied = state.preset_supplied;
    legacy.preset_applied = state.preset_applied;
    legacy.ui_composition_requested = state.ui_composition_requested;
    legacy.ui_composition_applied = state.ui_composition_applied;
    legacy.ui_composition_fallback = state.ui_composition_fallback;
    legacy.prepared_provider_count = state.prepared_provider_count;
    *static_cast<mfgunlock::diagnostic::DiagnosticStateV1*>(output) = legacy;
    return true;
  }
  if (version == mfgunlock::diagnostic::kStateVersion2) {
    mfgunlock::diagnostic::DiagnosticStateV2 legacy;
    std::memcpy(&legacy, &state, sizeof(legacy));
    legacy.bytes = sizeof(legacy);
    legacy.version = mfgunlock::diagnostic::kStateVersion2;
    *static_cast<mfgunlock::diagnostic::DiagnosticStateV2*>(output) = legacy;
    return true;
  }

  *static_cast<mfgunlock::diagnostic::DiagnosticState*>(output) = state;
  return true;
}

extern "C" __declspec(dllexport) bool MfgUnlockGetCompositionState(
    uint32_t version, void* output) {
  if (version != mfgunlock::composition::kVersion || output == nullptr) return false;
  *static_cast<mfgunlock::composition::Snapshot*>(output) = mfgunlock::composition::Read();
  return true;
}

extern "C" __declspec(dllexport) bool MfgUnlockGetPresentationState(
    uint32_t version, void* output) {
  if (version != mfgunlock::presentation::kVersion || output == nullptr) return false;
  *static_cast<mfgunlock::presentation::Snapshot*>(output) =
      mfgunlock::presentation::Read();
  return true;
}

extern "C" __declspec(dllexport) bool MfgUnlockGetPassiveMetrics(
    uint32_t version, void* output) {
  if (version != mfgunlock::presentation::passive::kVersion || output == nullptr) return false;
  *static_cast<mfgunlock::presentation::passive::Snapshot*>(output) =
      mfgunlock::presentation::passive::Read();
  return true;
}

extern "C" __declspec(dllexport) void MfgUnlockSetPassiveMetricsEnabled(bool enabled) {
  mfgunlock::presentation::passive::SetEnabled(enabled);
}

extern "C" __declspec(dllexport) bool MfgUnlockGetRuntimeLifecycleState(
    uint32_t version, void* output) {
  if (version != mfgunlock::runtime::kVersion || output == nullptr) return false;
  SyncRuntimeLifecycleEvidence();
  *static_cast<mfgunlock::runtime::Snapshot*>(output) = mfgunlock::runtime::Read();
  return true;
}

extern "C" __declspec(dllexport) bool MfgUnlockGetAdvancedFeatureState(
    uint32_t version, void* output) {
  if (version != mfgunlock::advanced::kVersion || output == nullptr) return false;

  SyncRuntimeLifecycleEvidence();
  const auto runtime = mfgunlock::runtime::Read();
  const auto* architecture_profile = mfgunlock::architecture::ActiveProfile();
  const mfgunlock::profiles::ProviderProfile* provider_profile = nullptr;
  if (runtime.provider_profile_index > 0 &&
      runtime.provider_profile_index <= mfgunlock::profiles::kProfiles.size()) {
    provider_profile = &mfgunlock::profiles::kProfiles[runtime.provider_profile_index - 1];
  }

  mfgunlock::advanced::AppliedBackends applied;
  if (!TryAcquireSRWLockShared(&g_provider_maintenance_lock)) return false;
  applied.full_temporal = !g_blackwell_temporal_modules.empty();
  applied.midpoint = g_midpoint_patched.load(std::memory_order_acquire);
  applied.validated_warp = std::any_of(
      g_validated_warp_modules.begin(), g_validated_warp_modules.end(),
      [](const auto& patch) {
        return patch.mode == mfgunlock::validatedwarp::Mode::kValidatedWarp;
      });
  applied.scatter_retention = std::any_of(
      g_blackwell_temporal_modules.begin(), g_blackwell_temporal_modules.end(),
      [](const auto& patch) { return patch.intermediate_scatter; });
  applied.boundary_mitigation = std::any_of(
      g_blackwell_temporal_modules.begin(), g_blackwell_temporal_modules.end(),
      [](const auto& patch) {
        return patch.boundary_mode !=
            mfgunlock::blackwelltemporal::BoundaryArtifactMode::kOff;
      });
  ReleaseSRWLockShared(&g_provider_maintenance_lock);

  mfgunlock::advanced::Inputs input;
  input.architecture = architecture_profile;
  input.provider_profile = provider_profile;
  input.provider_profile_index = runtime.provider_profile_index;
  input.provider_exact = runtime.provider_exact_qualified != 0;
  input.provider_ready = runtime.restart_required == 0 && runtime.allowed_capability_bits != 0;
  input.applied = applied;
  input.contract = mfgunlock::integration::Read();
  *static_cast<mfgunlock::advanced::Snapshot*>(output) =
      mfgunlock::advanced::Evaluate(input);
  return true;
}

namespace {

mfgunlock::addonentry::State g_addon_entry;
mfgunlock::addonentry::HostLifetime g_reshade_host_lifetime;

// Keep the D1 observation journal. R2 additionally refreshes host registration
// at AddonInit; runtime/provider setup remains one-time. Diagnostics must not
// initialize SDK module caches before the real registration call.
mfgunlock::registrationtrace::Journal<> g_registration_trace;
// Separate from verbose registration entries: keep actual SL/NGX startup
// evidence even when temporary Vulkan instances reopen/truncate ReShade.log.
// Bounded memory only on the producer path; file I/O is explicit and off-frame.
mfgunlock::registrationtrace::Journal<1024, 4096> g_dlss_startup_trace;
std::atomic_flag g_dlss_dump_busy = ATOMIC_FLAG_INIT;
struct RegistrationTraceLastError {
  DWORD value = GetLastError();
  ~RegistrationTraceLastError() { SetLastError(value); }
};

void TraceDlssStartupLog(const char* message, bool warning) noexcept {
  RegistrationTraceLastError saved;
  g_dlss_startup_trace.Append("tick=%llu tid=%lu severity=%s %s",
      static_cast<unsigned long long>(GetTickCount64()),
      static_cast<unsigned long>(GetCurrentThreadId()), warning ? "WARN" : "INFO",
      message ? message : "<null>");
}

void DumpDlssStartupEvidence(const char* phase) {
  RegistrationTraceLastError saved;
  if (g_dlss_dump_busy.test_and_set(std::memory_order_acquire)) return;
  struct Unlock { ~Unlock() { g_dlss_dump_busy.clear(std::memory_order_release); } } unlock;
  wchar_t path[1024]{};
  const DWORD length = GetModuleFileNameW(g_self_module, path, 1024);
  if (!length || length >= 1024) return;
  auto* slash = std::wcsrchr(path, L'\\');
  constexpr wchar_t name[] = L"MFGAmpereUnlock.dlss-startup.log";
  if (!slash || static_cast<size_t>(slash + 1 - path) + std::size(name) > std::size(path)) return;
  std::copy(std::begin(name), std::end(name), slash + 1);
  HANDLE file = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) {
    char error[128]{};
    std::snprintf(error, sizeof(error), "DLSS_STARTUP_D2 file_error=%lu\n",
                  static_cast<unsigned long>(GetLastError()));
    OutputDebugStringA(error);
    return;
  }
  auto emit = [&](const char* line) {
    DWORD written = 0;
    WriteFile(file, line, static_cast<DWORD>(std::strlen(line)), &written, nullptr);
    WriteFile(file, "\r\n", 2, &written, nullptr);
  };
  char line[4608]{};
  std::snprintf(line, sizeof(line),
      "DLSS_STARTUP_D2 phase=%s pid=%lu tick=%llu dropped=%llu observations_only=1",
      phase, static_cast<unsigned long>(GetCurrentProcessId()),
      static_cast<unsigned long long>(GetTickCount64()),
      static_cast<unsigned long long>(g_dlss_startup_trace.Dropped()));
  emit(line);
  std::snprintf(line, sizeof(line),
      "STATE registration_attempts=%u host_ready=%u runtime_initialized=%u bootstrap_complete=%u bootstrap_busy=%u loadhook_installed=%u slinit_hook_installed=%u slinit_observed=%u sl_fg_requested=%d sl_lifecycle=%s ngx_capabilities_seen=%u ngx_vulkan_capabilities=%u ngx_fg_available=%u ngx_fg_created=%u enabled=%u runtime_selection=%u",
      g_addon_entry.Attempts(), static_cast<unsigned>(g_addon_entry.Ready()),
      static_cast<unsigned>(g_addon_entry.Initialized()),
      static_cast<unsigned>(g_bootstrap_complete.load()), static_cast<unsigned>(g_bootstrap_busy.load()),
      static_cast<unsigned>(mfgunlock::loadhook::g_hooked.load()),
      static_cast<unsigned>(mfgunlock::framecount::g_init_hooked.load()),
      static_cast<unsigned>(mfgunlock::streamline::internal::g_init_observed.load()),
      mfgunlock::streamline::internal::g_feature_requested.load(),
      mfgunlock::streamline::internal::LifecycleName(mfgunlock::streamline::internal::g_lifecycle_state.load()),
      static_cast<unsigned>(mfgunlock::ngx::g_status.capabilities_seen.load()),
      static_cast<unsigned>(mfgunlock::ngx::g_status.vulkan_capabilities.load()),
      static_cast<unsigned>(mfgunlock::ngx::g_status.available.load()),
      static_cast<unsigned>(mfgunlock::ngx::g_status.feature_created.load()),
      static_cast<unsigned>(g_enabled.load()), mfgunlock::framecount::g_runtime_selection_mode.load());
  emit(line);
  std::snprintf(line, sizeof(line), "HOST_LIFETIME_R3 retained_host=%p policy=PROCESS_LIFETIME",
      g_reshade_host_lifetime.Host());
  emit(line);
  std::snprintf(line, sizeof(line), "COMMON_ROLE_R3 resolved_common=%p",
      static_cast<void*>(mfgunlock::streamline::internal::CommonModule()));
  emit(line);
  // GetModuleHandle does not load a library or call any vendor API.
  for (const wchar_t* module_name : {L"sl.interposer.dll", L"sl.common.dll", L"sl.dlss.dll",
          L"sl.dlss_g.dll", L"sl.dlss_d.dll", L"_nvngx.dll", L"nvngx.dll", L"nvngx_dlss.dll",
          L"nvngx_dlssg.dll", L"nvngx_dlssd.dll"}) {
    const auto module = std::wcscmp(module_name, L"sl.common.dll") == 0
        ? mfgunlock::streamline::internal::CommonModule() : GetModuleHandleW(module_name);
    std::snprintf(line, sizeof(line), "MODULE name=%ls handle=%p", module_name,
                  static_cast<void*>(module));
    emit(line);
  }
  g_dlss_startup_trace.Visit([&](size_t index, const char* message) {
    std::snprintf(line, sizeof(line), "seq=%llu %s",
                  static_cast<unsigned long long>(index), message);
    emit(line);
  });
  CloseHandle(file);
}

void TraceRegistrationModule(const char* label, HMODULE module) {
  RegistrationTraceLastError saved;
  char path[1024]{};
  const DWORD n = module ? GetModuleFileNameA(module, path, sizeof(path)) : 0;
  const DWORD error = n ? 0 : GetLastError();
  path[sizeof(path) - 1] = '\0';
  g_registration_trace.Append("module=%s handle=%p path=%s path_error=%lu truncated=%u",
      label, static_cast<void*>(module), path, static_cast<unsigned long>(error),
      static_cast<unsigned>(n >= sizeof(path)));
}

void TraceRegistrationExports(const char* label, HMODULE module) {
  RegistrationTraceLastError saved;
  if (!module) return;
  for (const char* name : {"ReShadeRegisterAddon", "ReShadeUnregisterAddon",
                           "ReShadeLogMessage", "ReShadeGetImGuiFunctionTable"}) {
    const auto function = GetProcAddress(module, name);
    HMODULE owner = nullptr;
    if (function)
      GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
          GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
          reinterpret_cast<LPCWSTR>(function), &owner);
    unsigned char bytes[32]{};
    SIZE_T read = 0;
    char head[65]{};
    if (function && ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void*>(function),
                                      bytes, sizeof(bytes), &read)) {
      read = (std::min)(read, sizeof(bytes));
      for (SIZE_T i = 0; i < read; ++i)
        std::snprintf(head + i * 2, sizeof(head) - i * 2, "%02x", bytes[i]);
    }
    g_registration_trace.Append("exports=%s name=%s address=%p owner=%p entry_bytes=%s",
        label, name, reinterpret_cast<void*>(function), static_cast<void*>(owner), head);
    if (function && owner != module) TraceRegistrationModule("forwarded-export-owner", owner);
  }
}

void TraceRegistrationState(const char* phase, HMODULE self, HMODULE given_host,
                            bool sdk_was_called) {
  RegistrationTraceLastError saved;
  g_registration_trace.Append(
      "phase=%s pid=%lu tid=%lu tick=%llu self=%p given_host=%p local_ready=%u attempts=%u state_host=%p bootstrap_complete=%u bootstrap_busy=%u shutting_down=%u",
      phase, static_cast<unsigned long>(GetCurrentProcessId()),
      static_cast<unsigned long>(GetCurrentThreadId()),
      static_cast<unsigned long long>(GetTickCount64()), static_cast<void*>(self),
      static_cast<void*>(given_host), static_cast<unsigned>(g_addon_entry.Ready()),
      g_addon_entry.Attempts(), g_addon_entry.Host(),
      static_cast<unsigned>(g_bootstrap_complete.load()),
      static_cast<unsigned>(g_bootstrap_busy.load()),
      static_cast<unsigned>(g_bootstrap_shutting_down.load()));
  TraceRegistrationModule("self", self);
  TraceRegistrationModule("given-host", given_host);
  if (!sdk_was_called) return; // Reading uninitialized SDK caches changes them.
  const HMODULE sdk_self = reshade::internal::get_current_module_handle();
  const HMODULE sdk_host = reshade::internal::get_reshade_module_handle();
  g_registration_trace.Append("sdk_cache self_match=%u host_match=%u sdk_self=%p sdk_host=%p",
      static_cast<unsigned>(sdk_self == self), static_cast<unsigned>(sdk_host == given_host),
      static_cast<void*>(sdk_self), static_cast<void*>(sdk_host));
  TraceRegistrationModule("sdk-self", sdk_self);
  TraceRegistrationModule("sdk-host", sdk_host);
  TraceRegistrationExports("sdk-host", sdk_host);
  if (sdk_host != given_host) TraceRegistrationExports("given-host", given_host);
}

void DumpRegistrationTrace(HMODULE self, HMODULE host, const char* phase) {
  RegistrationTraceLastError saved;
  // Called ONLY from AddonInit (not DllMain). Independent file keeps early
  // records even if ReShade's logger/registration does not work at first load.
  wchar_t path[1024]{};
  HANDLE file = INVALID_HANDLE_VALUE;
  const DWORD length = GetModuleFileNameW(self, path, 1024);
  if (length && length < 1024) {
    if (auto* slash = std::wcsrchr(path, L'\\')) {
      constexpr wchar_t name[] = L"MFGAmpereUnlock.registration.log";
      if (static_cast<size_t>(slash + 1 - path) + sizeof(name) / sizeof(wchar_t) <= 1024) {
        std::copy(name, name + sizeof(name) / sizeof(wchar_t), slash + 1);
        file = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
      }
    }
  }
  const DWORD file_error = file == INVALID_HANDLE_VALUE ? GetLastError() : 0;
  using LogFn = void (*)(void*, int, const char*);
  const auto log = host ? reinterpret_cast<LogFn>(GetProcAddress(host, "ReShadeLogMessage")) : nullptr;
  auto emit = [&](const char* line) {
    if (file != INVALID_HANDLE_VALUE) {
      DWORD written = 0;
      WriteFile(file, line, static_cast<DWORD>(std::strlen(line)), &written, nullptr);
      WriteFile(file, "\r\n", 2, &written, nullptr);
    }
    if (log) log(nullptr, 3, line);
    OutputDebugStringA(line);
  };
  char line[1800]{};
  std::snprintf(line, sizeof(line), "[MFG Unlock] REGDIAG_D1 dump=%s file_error=%lu dropped=%llu",
      phase, static_cast<unsigned long>(file_error),
      static_cast<unsigned long long>(g_registration_trace.Dropped()));
  emit(line);
  // The bounded D1 journal retains the first records. Always include a fresh
  // summary as well, even after repeated host initialization fills that journal.
  std::snprintf(line, sizeof(line),
      "[MFG Unlock] registration R2 snapshot phase=%s host_ready=%u runtime_initialized=%u attempts=%u bootstrap_complete=%u",
      phase, static_cast<unsigned>(g_addon_entry.Ready()),
      static_cast<unsigned>(g_addon_entry.Initialized()), g_addon_entry.Attempts(),
      static_cast<unsigned>(g_bootstrap_complete.load(std::memory_order_acquire)));
  emit(line);
  g_registration_trace.Visit([&](size_t index, const char* text) {
    std::snprintf(line, sizeof(line), "[MFG Unlock] REGDIAG_D1 seq=%llu %s",
        static_cast<unsigned long long>(index), text);
    emit(line);
  });
  if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
}

HMODULE FindRegistrationHost() {
  // Do not initialize the SDK's cached handle to nullptr during an early load.
  // AddonInit supplies the authoritative handle later when discovery is too early.
  HMODULE modules[1024]{};
  DWORD bytes = 0;
  if (!K32EnumProcessModules(GetCurrentProcess(), modules, sizeof(modules), &bytes))
    return nullptr;
  const size_t count = (std::min)(static_cast<size_t>(bytes), sizeof(modules)) /
      sizeof(HMODULE);
  for (size_t i = 0; i < count; ++i) {
    if (GetProcAddress(modules[i], "ReShadeRegisterAddon") &&
        GetProcAddress(modules[i], "ReShadeUnregisterAddon")) return modules[i];
  }
  return nullptr;
}

bool RetainRegistrationHost(HMODULE host) {
  RegistrationTraceLastError saved;
  return g_reshade_host_lifetime.Ensure(host, [](void* candidate) {
    HMODULE pinned = nullptr;
    const bool ok = GetModuleHandleExW(
        GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
        reinterpret_cast<LPCWSTR>(candidate), &pinned) && pinned == candidate;
    const DWORD error = ok ? ERROR_SUCCESS : GetLastError();
    char text[192]{};
    std::snprintf(text, sizeof(text),
        "HOST_LIFETIME_R3 host=%p retained=%u win32_error=%lu policy=PROCESS_LIFETIME",
        candidate, static_cast<unsigned>(ok), static_cast<unsigned long>(error));
    g_registration_trace.Append("%s", text);
    TraceDlssStartupLog(text, !ok);
    OutputDebugStringA(text);
    return ok;
  });
}

void LogRegistration(HMODULE host, const char* where, const char* result, int severity = 3) {
  // ReShade's logging wrapper must not cache a null function pointer before the
  // first successful registration. Resolve the optional logger from this host.
  char path[MAX_PATH]{};
  if (host) GetModuleFileNameA(host, path, MAX_PATH - 1);
  char text[768]{};
  std::snprintf(text, sizeof(text),
      "[MFG Unlock] registration R2 entry=%s result=%s attempts=%u api=%u imgui=%u host=%p path=%s",
      where, result, g_addon_entry.Attempts(),
      static_cast<unsigned>(RESHADE_API_VERSION), static_cast<unsigned>(IMGUI_VERSION_NUM),
      static_cast<void*>(host), path);
  using LogFn = void (*)(void*, int, const char*);
  const auto log = host ? reinterpret_cast<LogFn>(GetProcAddress(host, "ReShadeLogMessage")) : nullptr;
  g_registration_trace.Append("tick=%llu tid=%lu %s", static_cast<unsigned long long>(GetTickCount64()),
      static_cast<unsigned long>(GetCurrentThreadId()), text);
  TraceDlssStartupLog(text, severity <= 2);
  if (log) log(nullptr, severity, text);
  OutputDebugStringA(text);
}

void RegisterAddonCallbacks() {
  reshade::register_overlay("MFG Unlock", OnRegisterOverlay);
  reshade::register_event<reshade::addon_event::init_device>(OnInitDevice);
  reshade::register_event<reshade::addon_event::destroy_device>(OnDestroyDevice);
  reshade::register_event<reshade::addon_event::reshade_begin_effects>(OnBeginEffects);
  reshade::register_event<reshade::addon_event::reshade_finish_effects>(OnFinishEffects);
  reshade::register_event<reshade::addon_event::reshade_present>(OnOverlayRendered);
  reshade::register_event<reshade::addon_event::destroy_effect_runtime>(OnDestroyEffects);
  RegisterScreenshotObserver(true);
  reshade::register_event<reshade::addon_event::init_command_queue>(OnInitCommandQueue);
  reshade::register_event<reshade::addon_event::destroy_command_queue>(OnDestroyCommandQueue);
  reshade::register_event<reshade::addon_event::execute_command_list>(OnExecuteUiCommandList);
  reshade::register_event<reshade::addon_event::init_command_list>(OnInitUiCommandList);
  reshade::register_event<reshade::addon_event::reset_command_list>(OnResetUiCommandList);
  reshade::register_event<reshade::addon_event::destroy_command_list>(OnDestroyUiCommandList);
  reshade::register_event<reshade::addon_event::init_pipeline>(OnInitUiBlendPipeline);
  reshade::register_event<reshade::addon_event::destroy_pipeline>(OnDestroyUiBlendPipeline);
  reshade::register_event<reshade::addon_event::bind_pipeline>(OnBindUiBlendPipeline);
  reshade::register_event<reshade::addon_event::draw>(OnDrawUiCandidate);
  reshade::register_event<reshade::addon_event::draw_indexed>(OnDrawIndexedUiCandidate);
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
#if defined(MFGUNLOCK_RESHADE_API) && MFGUNLOCK_RESHADE_API >= 18
  reshade::register_event<reshade::addon_event::finish_present>(OnFinishPresentUiAlpha);
#endif
}

bool InitializeAddon(HMODULE self, HMODULE requested_host, const char* where) {
  mfgunlock::provider::g_startup_log_observer.store(&TraceDlssStartupLog, std::memory_order_relaxed);
  TraceRegistrationState(where, self, requested_host, g_addon_entry.Ready());
  HMODULE observed_host = requested_host;
  const auto register_with_host = [&](void* addon, void* host) {
    observed_host = static_cast<HMODULE>(host);
    if (!GetProcAddress(observed_host, "ReShadeRegisterAddon") ||
        !GetProcAddress(observed_host, "ReShadeUnregisterAddon")) {
      LogRegistration(observed_host, where, "HOST_API_EXPORTS_MISSING", 1);
      return false;
    }
    // Do this before ANY SDK registration/callback caches or runtime workers.
    // ReShade may otherwise unload when a temporary Vulkan probe is destroyed.
    if (!RetainRegistrationHost(observed_host)) {
      observed_host = nullptr;
      LogRegistration(nullptr, where, "HOST_LIFETIME_UNAVAILABLE", 1);
      return false;
    }
    LogRegistration(observed_host, where, "REGISTER_BEGIN");
    const bool registered = mfgunlock::addonentry::RegisterWithHost(
        &reshade::register_addon, &reshade::internal::get_reshade_module_handle,
        static_cast<HMODULE>(addon), observed_host);
    g_registration_trace.Append("sdk_register_return=%u last_error=%lu",
        static_cast<unsigned>(registered), static_cast<unsigned long>(GetLastError()));
    TraceRegistrationState("after-sdk-register", static_cast<HMODULE>(addon), observed_host, true);
    if (!registered) {
      // SDK registration can fail after the host has registered the addon
      // (e.g. ImGui mismatch). Remove only our incomplete registration.
      reshade::unregister_addon(static_cast<HMODULE>(addon));
      return false;
    }
    return true;
  };
  mfgunlock::addonentry::Result result;
  if (requested_host && g_addon_entry.Initialized()) {
    result = g_addon_entry.Refresh(self, requested_host,
        [&](void* addon, void* host) {
          // ReShade's public unregister removes only this module's entry and
          // its callbacks; it is a no-op when the current host has lost them.
          // Removing before re-registering also prevents duplicate callbacks
          // on the normal early-load -> AddonInit path.
          using UnregisterFn = void (*)(void*);
          const auto unregister = reinterpret_cast<UnregisterFn>(GetProcAddress(
              static_cast<HMODULE>(host), "ReShadeUnregisterAddon"));
          if (!unregister) {
            LogRegistration(static_cast<HMODULE>(host), where,
                            "HOST_API_EXPORTS_MISSING", 1);
            return false;
          }
          LogRegistration(static_cast<HMODULE>(host), where,
                          "REFRESH_HOST_REGISTRATION");
          unregister(addon);
          return register_with_host(addon, host);
        }, []() { RegisterAddonCallbacks(); });
  } else {
    result = g_addon_entry.Attach(self, requested_host,
        [&]() -> void* { observed_host = FindRegistrationHost(); return observed_host; },
        register_with_host, [&]() {
      const HMODULE h_module = self;
      g_self_module = h_module;
      mfgunlock::ngx::g_before_fg_create = BeforeFgCreate;
      mfgunlock::framecount::internal::g_mode_observer = ObserveFgMode;
      mfgunlock::framecount::g_observe_streamline_ui_tags = ObserveNativeStreamlineTags;
      mfgunlock::framecount::g_observe_ui_candidate = ObserveUiCandidate;
      mfgunlock::framecount::g_acquire_ui_candidate = AcquireUiCandidate;
      mfgunlock::framecount::g_release_ui_candidate = ReleaseUiCandidate;
#if defined(MFGUNLOCK_RESHADE_API) && MFGUNLOCK_RESHADE_API >= 18
      mfgunlock::framecount::g_prepare_ui_alpha_candidate = PrepareUiAlphaCandidate;
      mfgunlock::framecount::g_finish_ui_alpha_candidate = FinishUiAlphaCandidate;
#else
      mfgunlock::framecount::g_prepare_ui_alpha_candidate = nullptr;
      mfgunlock::framecount::g_finish_ui_alpha_candidate = nullptr;
#endif
      mfgunlock::streamline::g_on_dlssg_plugin_bound = ObserveFrameCountCeiling;
      mfgunlock::streamline::g_on_dlssg_plugin_unloaded = OnDlssgPluginUnloaded;
      mfgunlock::ngx::g_prepare_loaded_providers = RunProviderMaintenance;
      mfgunlock::streamline::g_on_interposer_loaded = mfgunlock::framecount::TryInstall;
      mfgunlock::loadhook::g_on_get_proc_address = mfgunlock::streamline::Resolve;
      mfgunlock::loadhook::g_on_load_result = TraceDlssLoadResult;
      mfgunlock::framecount::g_on_init = mfgunlock::streamline::OnInit;
      mfgunlock::framecount::g_init_compatible = mfgunlock::streamline::CanHookInit;
      mfgunlock::framecount::g_find_interposer = mfgunlock::streamline::internal::InterposerModule;
      mfgunlock::framecount::g_interposer_contract_ready =
          mfgunlock::streamline::FullModernContractReady;
      mfgunlock::framecount::g_provider_mfg_ceiling = +[]() -> unsigned int {
        return g_enabled.load(std::memory_order_acquire) ? g_max_count.load(std::memory_order_acquire) : 0u;
      };
      mfgunlock::framecount::g_on_dlssg_entry_observed = ObserveDlssgFeatureEntry;
      mfgunlock::framecount::g_multi_frame_ready = []() {
        const auto* profile = mfgunlock::architecture::ActiveProfile();
        if (!g_enabled.load() || !profile || !mfgunlock::runtime::ProviderReady()) return false;
        if (!profile->RequiresProviderRetarget()) {
          return profile->provider_backend != mfgunlock::ProviderBackend::kNativeSm89 ||
                 g_gate_patched.load(std::memory_order_acquire);
        }
        return mfgunlock::provider::PreparedProviderCount() == 1 &&
               g_gate_patched.load(std::memory_order_acquire);
      };
      mfgunlock::framecount::g_dynamic_stack_ready = DynamicRuntimeStackReady;
      mfgunlock::ngx::g_capability_limit = []() -> unsigned int {
        // The reported menu range permits a real native trial. Structural and
        // prior reported maxima must not silently reduce a user-selected request.
        return g_enabled.load() ? g_max_count.load(std::memory_order_relaxed) : 0;
      };
      mfgunlock::ngx::g_streamline_ceiling = []() -> unsigned int {
        return mfgunlock::framecount::g_streamline_max_generated.load(std::memory_order_acquire);
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
        mfgunlock::framecount::TryInstall();
        g_streamline_scan_pending.store(true, std::memory_order_release);
      };
      mfgunlock::loadhook::g_on_ngx_loaded = [](HMODULE) {
        g_ngx_scan_pending.store(true, std::memory_order_release);
      };
      mfgunlock::loadhook::g_on_dlssg_loaded = ProcessLoadedDlssgModule;

      RegisterAddonCallbacks();
      // A retained module reference lets the worker finish before an external
      // unload. It releases that reference atomically with thread exit.
      HMODULE held = nullptr;
      if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                             reinterpret_cast<LPCWSTR>(h_module), &held)) {
        HANDLE thread = CreateThread(nullptr, 0, DeferredBootstrap, held, 0, nullptr);
        if (thread) CloseHandle(thread);
        else FreeLibrary(held);  // the first safe ReShade callback will retry
      }
      });
  }
  if (!observed_host) observed_host = static_cast<HMODULE>(g_addon_entry.Host());
  LogRegistration(observed_host, where, mfgunlock::addonentry::Name(result),
                  mfgunlock::addonentry::Succeeded(result) ? 3 : 2);
  return mfgunlock::addonentry::Succeeded(result);
}

}  // namespace

// ReShade calls this with its own module handle, including for a module already
// mapped by LoadFromDllMain. Always reconcile the current host registration;
// this deliberately does NOT restart the already initialized MFG runtime.
extern "C" __declspec(dllexport) bool AddonInit(HMODULE addon_module, HMODULE reshade_module) {
  TraceRegistrationState("AddonInit-enter", addon_module, reshade_module, g_addon_entry.Ready());
  DumpRegistrationTrace(addon_module, reshade_module, "before-AddonInit");
  const bool result = InitializeAddon(addon_module, reshade_module, "AddonInit");
  TraceRegistrationState("AddonInit-exit", addon_module, reshade_module, g_addon_entry.Ready());
  DumpRegistrationTrace(addon_module, reshade_module, "after-AddonInit");
  DumpDlssStartupEvidence("after-AddonInit");
  return result;
}

BOOL APIENTRY DllMain(HMODULE h_module, DWORD fdw_reason, LPVOID reserved) {
  // At process termination CUDA may still retain cached replacement pointers.
  // Leave allocations and mapped-image patches to process teardown.
  if (fdw_reason == DLL_PROCESS_DETACH && reserved != nullptr) return TRUE;
  switch (fdw_reason) {
    case DLL_PROCESS_ATTACH:
      g_self_module = h_module;
      // Keep the early path required by Streamline. If registration is too
      // early, keep the DLL inert so the normal AddonInit path can retry.
      InitializeAddon(h_module, nullptr, "DllMain");
      break;
    case DLL_PROCESS_DETACH:
      if (!g_addon_entry.Initialized()) break;
      g_bootstrap_shutting_down.store(true, std::memory_order_release);
      mfgunlock::runtime::BeginShutdown();
      mfgunlock::ngx::g_before_fg_create = nullptr;
      mfgunlock::framecount::internal::g_mode_observer = nullptr;
      mfgunlock::countobserver::SetCallback(nullptr);
      mfgunlock::diagnostic::SetEvaluateCallback(nullptr);
      mfgunlock::diagnostic::SetLifecycleCallback(nullptr);
#if defined(MFGUNLOCK_RESHADE_API) && MFGUNLOCK_RESHADE_API >= 18
      reshade::unregister_event<reshade::addon_event::finish_present>(OnFinishPresentUiAlpha);
#endif
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
      reshade::unregister_event<reshade::addon_event::draw_indexed>(OnDrawIndexedUiCandidate);
      reshade::unregister_event<reshade::addon_event::draw>(OnDrawUiCandidate);
      reshade::unregister_event<reshade::addon_event::bind_pipeline>(OnBindUiBlendPipeline);
      reshade::unregister_event<reshade::addon_event::destroy_pipeline>(OnDestroyUiBlendPipeline);
      reshade::unregister_event<reshade::addon_event::init_pipeline>(OnInitUiBlendPipeline);
      reshade::unregister_event<reshade::addon_event::destroy_command_list>(OnDestroyUiCommandList);
      reshade::unregister_event<reshade::addon_event::reset_command_list>(OnResetUiCommandList);
      reshade::unregister_event<reshade::addon_event::init_command_list>(OnInitUiCommandList);
      reshade::unregister_event<reshade::addon_event::execute_command_list>(OnExecuteUiCommandList);
      reshade::unregister_event<reshade::addon_event::destroy_command_queue>(OnDestroyCommandQueue);
      reshade::unregister_event<reshade::addon_event::init_command_queue>(OnInitCommandQueue);
      reshade::unregister_event<reshade::addon_event::init_device>(OnInitDevice);
      reshade::unregister_event<reshade::addon_event::destroy_device>(OnDestroyDevice);
      reshade::unregister_event<reshade::addon_event::reshade_begin_effects>(OnBeginEffects);
      reshade::unregister_event<reshade::addon_event::reshade_finish_effects>(OnFinishEffects);
      reshade::unregister_event<reshade::addon_event::reshade_present>(OnOverlayRendered);
      reshade::unregister_event<reshade::addon_event::destroy_effect_runtime>(OnDestroyEffects);
      RegisterScreenshotObserver(false);
      reshade::unregister_overlay("MFG Unlock", OnRegisterOverlay);
      mfgunlock::loadhook::Uninstall();
      // Detach entry hooks before restoring mapped-image patches.
      mfgunlock::loadhook::g_on_get_proc_address = nullptr;
      mfgunlock::loadhook::g_on_load_result = nullptr;
      mfgunlock::loadhook::g_on_dlssg_loaded = nullptr;
      mfgunlock::loadhook::g_on_interposer_loaded = nullptr;
      mfgunlock::loadhook::g_on_ngx_loaded = nullptr;
      mfgunlock::ngx::g_capability_limit = nullptr;
      mfgunlock::ngx::g_streamline_ceiling = nullptr;
      mfgunlock::framecount::g_on_init = nullptr;
      mfgunlock::framecount::g_init_compatible = nullptr;
      mfgunlock::framecount::g_interposer_contract_ready = nullptr;
      mfgunlock::framecount::g_multi_frame_ready = nullptr;
      mfgunlock::framecount::g_on_dlssg_entry_observed = nullptr;
      mfgunlock::framecount::g_dynamic_stack_ready = nullptr;
      mfgunlock::framecount::g_find_interposer = nullptr;
      mfgunlock::ngx::g_prepare_loaded_providers = nullptr;
      mfgunlock::streamline::Shutdown();
      mfgunlock::framecount::Uninstall();
      mfgunlock::framecount::g_observe_streamline_ui_tags = nullptr;
      mfgunlock::framecount::g_observe_ui_candidate = nullptr;
      mfgunlock::framecount::g_acquire_ui_candidate = nullptr;
      mfgunlock::framecount::g_release_ui_candidate = nullptr;
      mfgunlock::framecount::g_prepare_ui_alpha_candidate = nullptr;
      mfgunlock::framecount::g_finish_ui_alpha_candidate = nullptr;
      ClearUiRuntimeCandidates();
      g_ui_candidate_d3d12.store(false, std::memory_order_relaxed);
      mfgunlock::fgpreset::Shutdown();
      RestoreBlackwellTemporal();
      RestoreValidatedWarp();
      RestoreMidpoint();
      if (!mfgunlock::validatedwarp::Restore(g_failed_quality_redirects))
        mfgunlock::provider::Log("incomplete quality rollback still owns replacement memory", true);
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
