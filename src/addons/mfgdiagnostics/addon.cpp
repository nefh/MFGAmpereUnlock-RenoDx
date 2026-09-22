// SPDX-License-Identifier: MIT
// Separate, opt-in observer. The stable MFG Unlock implementation is untouched.
#include <windows.h>
#include <tlhelp32.h>
#include <d3d12.h>
#include <bcrypt.h>
#include <algorithm>
#include <array>
#include <filesystem>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <nlohmann/json.hpp>
#include <deps/imgui/imgui.h>
#include "../mfgunlock/reshade_compat.hpp"
#include "../mfgunlock/ngx_hook.hpp"
#include "../mfgunlock/diagnostic_bridge.hpp"
#include "../mfgunlock/output_state.hpp"
#include "./nvapi_observer.hpp"
#include "./trace.hpp"
#include "./mfg_probe.hpp"
#include "./timeline.hpp"

#pragma comment(lib, "version.lib")
#pragma comment(lib, "bcrypt.lib")

namespace {
using namespace mfgdiagnostics;
using Json = nlohmann::json;
constexpr char kSection[] = "RenoDX.MFGDiagnostics";
constexpr const char* kLabels[] = {"BASELINE", "DLSSG_2x", "DLSSG_3x",
                                 "DLSSG_4x", "DLSSG_5x", "DLSSG_6x"};
Capture<16384> g_capture;
bool g_enabled = false;
bool g_count_bridge_connected = false;
bool g_evaluate_bridge_connected = false;
bool g_state_bridge_connected = false;
bool g_lifecycle_bridge_connected = false;
bool g_capture_had_lifecycle_bridge = false;
bool g_capture_had_count_bridge = false;
bool g_capture_had_evaluate_bridge = false;
bool g_capture_had_state_bridge = false;
mfgunlock::countobserver::Register g_register_count = nullptr;
mfgunlock::diagnostic::RegisterEvaluate g_register_evaluate = nullptr;
mfgunlock::diagnostic::RegisterLifecycle g_register_lifecycle = nullptr;
mfgunlock::diagnostic::QueryState g_query_state = nullptr;
HMODULE g_core_module = nullptr;
timeline::OneShotCapture g_evaluate_capture;
std::atomic<uint64_t> g_last_evaluation_id{0};
std::atomic<uint32_t> g_last_frame_kind{static_cast<uint32_t>(timeline::FrameKind::kUnknown)};
std::atomic<uint32_t> g_last_generated_index{0};
std::atomic<uint32_t> g_last_generated_count{0};
std::atomic_bool g_d3d12{false}, g_installed{false}, g_saving{false};
std::atomic_bool g_capture_pending_export{false};
std::atomic<uint32_t> g_truncated_tag_batches{0};
std::atomic<uint32_t> g_optional_exports{0};
SRWLOCK g_install_lock = SRWLOCK_INIT;
std::vector<mfgunlock::hook::HookItem> g_hooks;
std::mutex g_output_mutex;
std::string g_output_message;
int g_label = 0;
int g_recorded_label = 0;
uint64_t g_frequency = 1;

struct TrackedResourceInfo {
  uint64_t width = 0;
  uint32_t height = 0;
  uint32_t format = 0;
  uint16_t levels = 0;
  uint16_t samples = 0;
  uint32_t flags = 0;
};
struct TrackedResourceViewInfo {
  uint64_t resource = 0;
  uint32_t usage = 0;
  uint32_t format = 0;
};

constexpr uint64_t kMaxCaptureDurationMs = 120000;
constexpr size_t kMaxUiCandidates = 128;

enum class UiCandidateUse : uint32_t {
  kRenderTargetBind,
  kClear,
  kShaderResourcePush,
};

struct UiCandidateStats {
  bool used = false;
  bool seen_as_swapchain = false;
  uint64_t resource = 0;
  uint64_t width = 0;
  uint32_t height = 0;
  uint32_t format = 0;
  uint32_t flags = 0;
  uint32_t view_format = 0;
  uint32_t view_usage = 0;
  uint32_t rtv_bind_count = 0;
  uint32_t clear_count = 0;
  uint32_t transparent_clear_count = 0;
  uint32_t srv_push_count = 0;
  uint32_t post_hudless_rtv_bind_count = 0;
  uint32_t post_hudless_clear_count = 0;
  uint32_t post_hudless_transparent_clear_count = 0;
  uint32_t post_hudless_srv_push_count = 0;
  uint32_t hudless_tag_matches = 0;
  uint64_t first_qpc = 0;
  uint64_t last_qpc = 0;
  uint64_t last_post_hudless_qpc = 0;
  uint64_t first_after_hudless_ticks = 0;
  uint64_t last_before_present_ticks = 0;
  std::array<float, 4> last_clear{};
};

std::shared_mutex g_resources_mutex;
std::unordered_map<uint64_t, TrackedResourceInfo> g_resources;
std::unordered_map<uint64_t, TrackedResourceViewInfo> g_resource_views;
std::mutex g_ui_candidates_mutex;
std::array<UiCandidateStats, kMaxUiCandidates> g_ui_candidates{};
std::atomic_bool g_ui_candidate_overflow{false};
std::atomic_bool g_hudless_window_open{false};
std::atomic<uint64_t> g_last_hudless_qpc{0};
std::atomic<uint64_t> g_last_hudless_resource{0};
std::atomic<uint32_t> g_hudless_tag_count{0};
std::atomic<uint32_t> g_ui_color_alpha_tag_count{0};
std::atomic<uint32_t> g_ui_alpha_tag_count{0};
std::atomic<uint64_t> g_capture_started_ms{0};
std::atomic<uint64_t> g_capture_stopped_ms{0};
std::atomic_bool g_capture_auto_stopped{false};
std::atomic<uint32_t> g_output_width{0};
std::atomic<uint32_t> g_output_height{0};
std::mutex g_device_mutex;
IUnknown* g_native_d3d12_device = nullptr;

PFun_slGetFeatureFunction* g_get_function = nullptr;
PFun_slSetConstants* g_set_constants = nullptr;
using SetTagFn = sl::Result (*)(const sl::ViewportHandle&, const sl::ResourceTag*, uint32_t,
                               sl::CommandBuffer*);
SetTagFn g_set_tag = nullptr;
PFun_slSetTagForFrame* g_set_tag_for_frame = nullptr;
std::atomic<PFun_slDLSSGSetOptions*> g_set_options{nullptr};
std::atomic<PFun_slDLSSGGetState*> g_get_state{nullptr};
std::atomic_bool g_dynamic_probe_armed{false};
std::atomic_bool g_dynamic_probe_attempted{false};
std::atomic_bool g_dynamic_capability_seen{false};
std::atomic_bool g_dynamic_supported{false};
std::atomic_bool g_dynamic_capability_from_probe{false};
std::atomic<uint32_t> g_dynamic_probe_result{UINT32_MAX};
std::atomic<uint32_t> g_dynamic_state_version{0};
using LoadWFn = HMODULE (WINAPI*)(LPCWSTR);
using LoadExWFn = HMODULE (WINAPI*)(LPCWSTR, HANDLE, DWORD);
LoadWFn g_load_w = nullptr;
LoadExWFn g_load_ex_w = nullptr;
bool g_loader_hooked = false;

uint64_t Ticks() {
  LARGE_INTEGER value{};
  QueryPerformanceCounter(&value);
  return static_cast<uint64_t>(value.QuadPart);
}

uint64_t Ticket() {
  if (g_capture.active.load(std::memory_order_acquire) == 0) return 0;
  return g_capture.Ticket(GetTickCount64());
}

void Submit(Event event, uint64_t ticket, uint64_t begin, uint32_t viewport,
            uint32_t frame, sl::Result result) {
  event.generation = ticket;
  event.begin = begin;
  event.end = Ticks();
  event.thread = GetCurrentThreadId();
  event.viewport = viewport;
  event.frame = frame;
  event.result = static_cast<uint32_t>(result);
  g_capture.Submit(event);
}

struct ResolvedView {
  uint64_t resource = 0;
  uint32_t view_format = 0;
  uint32_t view_usage = 0;
  TrackedResourceInfo resource_info{};
};

bool ResolveView(reshade::api::resource_view view, ResolvedView& resolved) {
  if (view.handle == 0) return false;
  std::shared_lock lock(g_resources_mutex);
  const auto view_it = g_resource_views.find(view.handle);
  if (view_it == g_resource_views.end() || view_it->second.resource == 0) return false;
  const auto resource_it = g_resources.find(view_it->second.resource);
  if (resource_it == g_resources.end()) return false;
  resolved.resource = view_it->second.resource;
  resolved.view_format = view_it->second.format;
  resolved.view_usage = view_it->second.usage;
  resolved.resource_info = resource_it->second;
  return true;
}

bool IsFullOutputResource(const TrackedResourceInfo& info) {
  const uint32_t width = g_output_width.load(std::memory_order_relaxed);
  const uint32_t height = g_output_height.load(std::memory_order_relaxed);
  return width != 0 && height != 0 && info.width == width && info.height == height;
}

void ResetUiCandidateCapture() {
  std::lock_guard lock(g_ui_candidates_mutex);
  g_ui_candidates = {};
  g_ui_candidate_overflow.store(false, std::memory_order_relaxed);
  g_hudless_window_open.store(false, std::memory_order_relaxed);
  g_last_hudless_qpc.store(0, std::memory_order_relaxed);
  g_last_hudless_resource.store(0, std::memory_order_relaxed);
  g_hudless_tag_count.store(0, std::memory_order_relaxed);
  g_ui_color_alpha_tag_count.store(0, std::memory_order_relaxed);
  g_ui_alpha_tag_count.store(0, std::memory_order_relaxed);
}

UiCandidateStats* FindUiCandidate(uint64_t resource) {
  UiCandidateStats* empty = nullptr;
  for (auto& candidate : g_ui_candidates) {
    if (candidate.used && candidate.resource == resource) return &candidate;
    if (!candidate.used && empty == nullptr) empty = &candidate;
  }
  return empty;
}

// Tag-call CPU ordering is not a GPU execution proof, so keep all full-output
// RTV candidates and record post-HUD-less activity as an additional signal.
void RecordUiCandidate(reshade::api::resource_view view, UiCandidateUse use,
                       const float* clear_color = nullptr) {
  const uint64_t ticket = Ticket();
  if (ticket == 0) return;
  ResolvedView resolved{};
  if (!ResolveView(view, resolved) || !IsFullOutputResource(resolved.resource_info)) return;

  const uint64_t now = Ticks();
  const bool post_hudless = g_hudless_window_open.load(std::memory_order_acquire);
  const uint64_t hudless = g_last_hudless_qpc.load(std::memory_order_relaxed);
  std::lock_guard lock(g_ui_candidates_mutex);
  if (g_capture.active.load(std::memory_order_acquire) != ticket) return;
  UiCandidateStats* candidate = FindUiCandidate(resolved.resource);
  if (candidate == nullptr) {
    g_ui_candidate_overflow.store(true, std::memory_order_relaxed);
    return;
  }
  if (!candidate->used) {
    if (use == UiCandidateUse::kShaderResourcePush) return;
    candidate->used = true;
    candidate->resource = resolved.resource;
    candidate->width = resolved.resource_info.width;
    candidate->height = resolved.resource_info.height;
    candidate->format = resolved.resource_info.format;
    candidate->flags = resolved.resource_info.flags;
    candidate->view_format = resolved.view_format;
    candidate->view_usage = resolved.view_usage;
    candidate->first_qpc = now;
    if (resolved.resource == g_last_hudless_resource.load(std::memory_order_relaxed))
      candidate->hudless_tag_matches = 1;
  }
  candidate->last_qpc = now;
  if (post_hudless) {
    candidate->last_post_hudless_qpc = now;
    if (candidate->first_after_hudless_ticks == 0 && hudless != 0 && now >= hudless)
      candidate->first_after_hudless_ticks = now - hudless;
  }

  switch (use) {
    case UiCandidateUse::kRenderTargetBind:
      ++candidate->rtv_bind_count;
      if (post_hudless) ++candidate->post_hudless_rtv_bind_count;
      break;
    case UiCandidateUse::kClear:
      ++candidate->clear_count;
      if (post_hudless) ++candidate->post_hudless_clear_count;
      if (clear_color != nullptr) {
        for (size_t i = 0; i < candidate->last_clear.size(); ++i)
          candidate->last_clear[i] = clear_color[i];
        if (clear_color[0] == 0.0f && clear_color[1] == 0.0f &&
            clear_color[2] == 0.0f && clear_color[3] == 0.0f) {
          ++candidate->transparent_clear_count;
          if (post_hudless) ++candidate->post_hudless_transparent_clear_count;
        }
      }
      break;
    case UiCandidateUse::kShaderResourcePush:
      ++candidate->srv_push_count;
      if (post_hudless) ++candidate->post_hudless_srv_push_count;
      break;
  }
}

// Legacy slSetTag has no frame token. The HUD-less-to-Present interval is a
// bounded ordering heuristic only; exported raw tag events remain authoritative.
void ObserveUiTagWindow(const sl::ResourceTag* tags, const Event* samples,
                        uint32_t count, uint64_t ticket) {
  if (tags == nullptr || ticket == 0 ||
      g_capture.active.load(std::memory_order_acquire) != ticket) return;
  for (uint32_t index = 0; index < count; ++index) {
    const auto& tag = tags[index];
    if (tag.resource == nullptr) continue;
    if (tag.type == sl::kBufferTypeHUDLessColor) {
      g_hudless_tag_count.fetch_add(1, std::memory_order_relaxed);
      g_last_hudless_qpc.store(Ticks(), std::memory_order_relaxed);
      g_hudless_window_open.store(true, std::memory_order_release);
      if (samples != nullptr && samples[index].recognized &&
          samples[index].values[4] != 0 && samples[index].values[4] != kUnknown) {
        const uint64_t hudless_resource = samples[index].values[4];
        g_last_hudless_resource.store(hudless_resource, std::memory_order_relaxed);
        std::lock_guard lock(g_ui_candidates_mutex);
        for (auto& candidate : g_ui_candidates) {
          if (candidate.used && candidate.resource == hudless_resource) {
            ++candidate.hudless_tag_matches;
            break;
          }
        }
      }
    } else if (tag.type == sl::kBufferTypeUIColorAndAlpha) {
      g_ui_color_alpha_tag_count.fetch_add(1, std::memory_order_relaxed);
    } else if (tag.type == sl::kBufferTypeUIAlpha) {
      g_ui_alpha_tag_count.fetch_add(1, std::memory_order_relaxed);
    }
  }
}

void FinalizeUiCandidateWindow(reshade::api::swapchain* swapchain, uint64_t present_qpc) {
  if (!g_hudless_window_open.exchange(false, std::memory_order_acq_rel)) return;
  const uint64_t hudless = g_last_hudless_qpc.load(std::memory_order_relaxed);
  std::lock_guard lock(g_ui_candidates_mutex);
  const uint32_t backbuffer_count = swapchain->get_back_buffer_count();
  for (auto& candidate : g_ui_candidates) {
    if (!candidate.used) continue;
    if (candidate.last_post_hudless_qpc >= hudless && hudless != 0) {
      candidate.last_before_present_ticks =
          present_qpc >= candidate.last_post_hudless_qpc
              ? present_qpc - candidate.last_post_hudless_qpc
              : 0;
    }
    for (uint32_t index = 0; index < backbuffer_count; ++index) {
      if (swapchain->get_back_buffer(index).handle == candidate.resource) {
        candidate.seen_as_swapchain = true;
        break;
      }
    }
  }
}

size_t UiCandidateCount() {
  std::lock_guard lock(g_ui_candidates_mutex);
  size_t count = 0;
  for (const auto& candidate : g_ui_candidates) count += candidate.used ? 1u : 0u;
  return count;
}

sl::Result HookOptions(const sl::ViewportHandle& viewport, const sl::DLSSGOptions& options) {
  auto* real = g_set_options.load(std::memory_order_acquire);
  const uint64_t ticket = Ticket();
  if (ticket == 0) return real(viewport, options);
  const uint64_t begin = Ticks();
  auto event = SnapshotOptions(options);
  const auto result = real(viewport, options); // Exactly once; never alter options.
  Submit(event, ticket, begin, static_cast<uint32_t>(viewport), UINT32_MAX, result);
  return result;
}

// Capture owner identity now; export never attributes a reused module address
// to the previous image or dereferences an expired parameter/resource pointer.
void CaptureOwner(Event& event, uint64_t address, size_t offset) {
  MEMORY_BASIC_INFORMATION memory{};
  if (!address || !VirtualQuery(reinterpret_cast<const void*>(address), &memory, sizeof(memory))) return;
  mfgunlock::hook::ModuleIdentity identity;
  if (!mfgunlock::hook::CaptureModule(static_cast<HMODULE>(memory.AllocationBase), identity)) return;
  event.values[offset] = reinterpret_cast<uint64_t>(identity.module);
  event.values[offset + 1] = identity.timestamp;
  event.values[offset + 2] = identity.image_bytes;
}

void ObserveCount(const mfgunlock::countobserver::Event* source) noexcept {
  struct LastError {
    DWORD value = GetLastError();
    ~LastError() { SetLastError(value); }
  } last_error;
  try {
    const uint64_t ticket = Ticket();
    if (!ticket || !source || source->version != mfgunlock::countobserver::kVersion ||
        source->bytes != sizeof(*source) ||
        static_cast<uint32_t>(source->backend) > 3 || static_cast<uint32_t>(source->operation) > 4 ||
        static_cast<uint32_t>(source->origin) > 7 || static_cast<uint32_t>(source->key) > 2) return;
    Event event(Kind::frame_count);
    event.version = source->version;
    event.recognized = true;
    event.values[0] = static_cast<uint32_t>(source->backend);
    event.values[1] = static_cast<uint32_t>(source->operation);
    event.values[2] = static_cast<uint32_t>(source->origin);
    event.values[3] = static_cast<uint32_t>(source->key);
    event.values[4] = static_cast<uint64_t>(source->value);
    event.values[5] = source->value_known;
    event.values[6] = source->requested;
    event.values[7] = source->mode;
    event.values[8] = source->ui_fallback;
    event.values[9] = source->caller;
    event.values[10] = source->callee;
    event.values[11] = source->object;
    CaptureOwner(event, source->caller, 12);
    CaptureOwner(event, source->callee, 15);
    Submit(event, ticket, Ticks(), source->viewport, UINT32_MAX,
           static_cast<sl::Result>(source->result));
  } catch (...) {
    g_capture.dropped.fetch_add(1, std::memory_order_relaxed);
  }
}

void ObserveEvaluate(const mfgunlock::diagnostic::EvaluateEvent* source) noexcept {
  struct LastError {
    DWORD value = GetLastError();
    ~LastError() { SetLastError(value); }
  } last_error;
  try {
    const uint64_t ticket = Ticket();
    if (!ticket) {
      if (g_capture.active.load(std::memory_order_acquire) == 0 && g_register_evaluate)
        g_register_evaluate(mfgunlock::diagnostic::kEvaluateVersion, nullptr);
      return;
    }
    if (!source || source->version != mfgunlock::diagnostic::kEvaluateVersion ||
        source->bytes != sizeof(*source) ||
        static_cast<uint32_t>(source->backend) != 0 ||
        static_cast<uint32_t>(source->phase) > 1) return;

    const auto classification = timeline::Classify(*source);
    g_last_evaluation_id.store(source->evaluation_id, std::memory_order_relaxed);
    g_last_frame_kind.store(static_cast<uint32_t>(classification.kind), std::memory_order_relaxed);
    g_last_generated_index.store(classification.generated_index, std::memory_order_relaxed);
    g_last_generated_count.store(classification.generated_count, std::memory_order_relaxed);
    g_evaluate_capture.Observe(*source);

    Event event(Kind::evaluate);
    event.version = source->version;
    event.recognized = true;
    event.values[0] = static_cast<uint32_t>(source->phase);
    event.values[1] = source->evaluation_id;
    event.values[2] = static_cast<uint32_t>(source->backend);
    event.values[3] = source->caller;
    event.values[4] = source->commands;
    event.values[5] = source->handle;
    event.values[6] = source->parameters;
    event.values[7] = source->generated_count_known;
    event.values[8] = source->generated_count;
    event.values[9] = source->generated_index_known;
    event.values[10] = source->generated_index;
    event.values[11] = source->reset_known;
    event.values[12] = source->reset;
    event.values[13] = source->automode_reset_known;
    event.values[14] = source->automode_reset;
    event.values[15] = source->backbuffer_frame_id_known;
    event.values[16] = source->backbuffer_frame_id;
    for (size_t i = 0; i < source->resources.size(); ++i) {
      const auto& resource = source->resources[i];
      const size_t base = 18 + i * 8;
      event.values[base] = resource.key;
      event.values[base + 1] = resource.known;
      event.values[base + 2] = resource.object;
      event.values[base + 3] = resource.width;
      event.values[base + 4] = resource.height;
      event.values[base + 5] = static_cast<uint64_t>(resource.depth_or_array) |
          (static_cast<uint64_t>(resource.mip_levels) << 16) |
          (static_cast<uint64_t>(resource.sample_count) << 32);
      event.values[base + 6] = resource.format;
      event.values[base + 7] = static_cast<uint64_t>(resource.dimension) |
          (static_cast<uint64_t>(resource.flags) << 32);
    }
    CaptureOwner(event, source->caller, 98);
    const auto result = source->result == mfgunlock::diagnostic::kUnknown32
        ? static_cast<sl::Result>(UINT32_MAX)
        : static_cast<sl::Result>(source->result);
    Submit(event, ticket, Ticks(), UINT32_MAX, UINT32_MAX, result);
  } catch (...) {
    g_capture.dropped.fetch_add(1, std::memory_order_relaxed);
  }
}

void ObserveLifecycle(const mfgunlock::diagnostic::LifecycleEvent* source) noexcept {
  const auto ticket = Ticket();
  if (!ticket || !source) return;
  const DWORD saved_error = GetLastError();
  try {
    auto event = SnapshotLifecycle(*source);
    if (event.recognized) {
      Submit(event, ticket, Ticks(), source->viewport, UINT32_MAX,
             static_cast<sl::Result>(source->result));
    }
  } catch (...) {
    g_capture.dropped.fetch_add(1, std::memory_order_relaxed);
  }
  SetLastError(saved_error);
}

void InstallCoreObservation() {
  // Core-side observer slots serialize callback invocation with unregister, so
  // diagnostics no longer needs a permanent module PIN to protect in-flight callbacks.
  const auto core = GetModuleHandleW(L"renodx-mfgunlock.addon64");
  g_core_module = core;
  if (core) {
    g_register_count = reinterpret_cast<mfgunlock::countobserver::Register>(
        GetProcAddress(core, "MfgUnlockSetCountObserver"));
    g_register_evaluate = reinterpret_cast<mfgunlock::diagnostic::RegisterEvaluate>(
        GetProcAddress(core, "MfgUnlockSetEvaluateObserver"));
    g_register_lifecycle = reinterpret_cast<mfgunlock::diagnostic::RegisterLifecycle>(
        GetProcAddress(core, "MfgUnlockSetLifecycleObserver"));
    g_query_state = reinterpret_cast<mfgunlock::diagnostic::QueryState>(
        GetProcAddress(core, "MfgUnlockGetDiagnosticState"));
  }
  g_count_bridge_connected = g_register_count &&
      g_register_count(mfgunlock::countobserver::kVersion, ObserveCount);
  g_evaluate_bridge_connected = g_register_evaluate &&
      g_register_evaluate(mfgunlock::diagnostic::kEvaluateVersion, ObserveEvaluate);
  g_state_bridge_connected = g_query_state != nullptr;
  g_lifecycle_bridge_connected = g_register_lifecycle &&
      g_register_lifecycle(mfgunlock::diagnostic::kLifecycleVersion, ObserveLifecycle);
  probe::g_sink = ObserveCount;
  probe::g_recording = [] { return Ticket() != 0; };
  HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, GetCurrentProcessId());
  if (snapshot == INVALID_HANDLE_VALUE) return;
  MODULEENTRY32W entry{};
  entry.dwSize = sizeof(entry);
  if (Module32FirstW(snapshot, &entry)) do {
    probe::Install(entry.hModule);
  } while (Module32NextW(snapshot, &entry));
  CloseHandle(snapshot);
}

void UninstallHotObservers() {
  // The core can unload before diagnostics during a hot-unload sequence. Only
  // call saved registration exports while the exact module instance that
  // supplied them is still mapped; core detach clears its observer slots too.
  const HMODULE current_core = GetModuleHandleW(L"renodx-mfgunlock.addon64");
  const bool core_current = g_core_module != nullptr && current_core == g_core_module;
  if (core_current && g_register_lifecycle && g_lifecycle_bridge_connected)
    g_register_lifecycle(mfgunlock::diagnostic::kLifecycleVersion, nullptr);
  g_lifecycle_bridge_connected = false;
  if (core_current && g_register_evaluate && g_evaluate_bridge_connected)
    g_register_evaluate(mfgunlock::diagnostic::kEvaluateVersion, nullptr);
  if (core_current && g_register_count && g_count_bridge_connected)
    g_register_count(mfgunlock::countobserver::kVersion, nullptr);
  g_evaluate_bridge_connected = false;
  g_count_bridge_connected = false;
  g_state_bridge_connected = false;
  probe::g_sink.store(nullptr, std::memory_order_release);
  probe::g_recording.store(nullptr, std::memory_order_release);
  if (!probe::Uninstall()) {
    reshade::log::message(reshade::log::level::error,
        "mfgdiagnostics: failed to detach every NGX parameter observer; retained module ownership is preserved");
  }
  g_query_state = nullptr;
  g_register_count = nullptr;
  g_register_evaluate = nullptr;
  g_register_lifecycle = nullptr;
  g_core_module = nullptr;
}

bool ObserveDynamicCapability(const sl::DLSSGState& state, sl::Result result,
                              bool from_probe) {
  if (result != sl::Result::eOk || state.structType != sl::DLSSGState::s_structType ||
      state.structVersion != sl::kStructVersion4) return false;
  if (state.bIsDynamicMFGSupported != sl::Boolean::eTrue &&
      state.bIsDynamicMFGSupported != sl::Boolean::eFalse) return false;
  g_dynamic_supported.store(state.bIsDynamicMFGSupported == sl::Boolean::eTrue,
                            std::memory_order_relaxed);
  g_dynamic_state_version.store(static_cast<uint32_t>(state.structVersion),
                                std::memory_order_relaxed);
  g_dynamic_capability_from_probe.store(from_probe, std::memory_order_relaxed);
  g_dynamic_capability_seen.store(true, std::memory_order_release);
  g_dynamic_probe_result.store(static_cast<uint32_t>(result), std::memory_order_relaxed);
  g_dynamic_probe_armed.store(false, std::memory_order_release);
  return true;
}

sl::Result HookState(const sl::ViewportHandle& viewport, sl::DLSSGState& state,
                     const sl::DLSSGOptions* options) {
  auto* real = g_get_state.load(std::memory_order_acquire);
  const uint64_t ticket = Ticket();
  const uint64_t begin = ticket == 0 ? 0 : Ticks();
  const auto result = real(viewport, state, options);
  const bool capability_seen = ObserveDynamicCapability(state, result, false);

  if (!capability_seen && g_dynamic_probe_armed.load(std::memory_order_acquire) &&
      g_d3d12.load(std::memory_order_relaxed)) {
    if (state.structType == sl::DLSSGState::s_structType &&
        state.structVersion >= sl::kStructVersion1 && state.structVersion < sl::kStructVersion4 &&
        g_dynamic_probe_armed.exchange(false, std::memory_order_acq_rel)) {
      sl::DLSSGState extended{};
      extended.next = state.next;
      const auto probe_result = real(viewport, extended, options);
      g_dynamic_probe_attempted.store(true, std::memory_order_release);
      g_dynamic_probe_result.store(static_cast<uint32_t>(probe_result),
                                   std::memory_order_relaxed);
      g_dynamic_state_version.store(static_cast<uint32_t>(extended.structVersion),
                                    std::memory_order_relaxed);
      ObserveDynamicCapability(extended, probe_result, true);
    } else if (g_dynamic_probe_armed.exchange(false, std::memory_order_acq_rel)) {
      g_dynamic_probe_attempted.store(true, std::memory_order_release);
      g_dynamic_probe_result.store(static_cast<uint32_t>(result),
                                   std::memory_order_relaxed);
      g_dynamic_state_version.store(static_cast<uint32_t>(state.structVersion),
                                    std::memory_order_relaxed);
    }
  }

  if (ticket != 0) {
    Submit(SnapshotState(state, result), ticket, begin, static_cast<uint32_t>(viewport),
           UINT32_MAX, result);
  }
  return result;
}

sl::Result HookConstants(const sl::Constants& values, const sl::FrameToken& frame,
                         const sl::ViewportHandle& viewport) {
  const uint64_t ticket = Ticket();
  if (ticket == 0) return g_set_constants(values, frame, viewport);
  const uint64_t begin = Ticks();
  auto event = SnapshotConstants(values);
  const auto result = g_set_constants(values, frame, viewport);
  Submit(event, ticket, begin, static_cast<uint32_t>(viewport),
         static_cast<uint32_t>(frame), result);
  return result;
}

// A legacy call can forward to the frame-based API. Preserve both calls while
// recording the outer observation only; legacy frame identity remains unknown.
thread_local bool g_inside_tag = false;
struct TagScope {
  bool nested = g_inside_tag;
  TagScope() { g_inside_tag = true; }
  ~TagScope() { g_inside_tag = nested; }
};

void FillTrackedDescriptor(Event& event, uint64_t native, uint32_t source_offset,
                           const TrackedResourceInfo& info) {
  event.values[3] = native != 0;
  event.values[4] = native;
  event.values[14] = info.width;
  event.values[15] = info.height;
  event.values[16] = info.format;
  event.values[17] = info.levels;
  event.values[18] = info.samples;
  event.values[19] = info.flags;
  event.values[20] = 1;
  event.values[24] = source_offset;
}

// Some shipped Streamline integrations hand slSetTag a Resource layout that
// does not validate against the public header used to build this observer. Do
// not guess that ABI or invoke COM through a guessed pointer. ReShade tells us
// every live D3D12 resource handle, so scan the bounded Resource storage for a
// pointer that exactly matches that live set. Accept only one unique match.
void ResolveTrackedResource(const sl::Resource& resource, Event& event) {
  if (!g_d3d12.load(std::memory_order_relaxed)) return;
  const auto* bytes = reinterpret_cast<const unsigned char*>(&resource);
  std::shared_lock lock(g_resources_mutex);

  uint64_t matched = 0;
  uint32_t matched_offset = UINT32_MAX;
  const TrackedResourceInfo* matched_info = nullptr;
  uint32_t matches = 0;
  for (uint32_t offset = 0; offset <= 56; offset += sizeof(uint64_t)) {
    uint64_t candidate = 0;
    std::memcpy(&candidate, bytes + offset, sizeof(candidate));
    if (candidate == 0 || candidate == matched) continue;
    const auto found = g_resources.find(candidate);
    if (found == g_resources.end()) continue;
    ++matches;
    matched = candidate;
    matched_offset = offset;
    matched_info = &found->second;
  }
  event.values[25] = matches;
  if (matches == 1)
    FillTrackedDescriptor(event, matched, matched_offset, *matched_info);
}

template <typename Call>
sl::Result ObserveTags(const sl::ViewportHandle& viewport, uint32_t frame,
                       const sl::ResourceTag* tags, uint32_t count,
                       sl::CommandBuffer* commands, Call&& call) {
  const TagScope scope;
  const uint64_t ticket = scope.nested ? 0 : Ticket();
  if (ticket == 0) return call();
  const uint64_t begin = Ticks();
  // No allocations or unbounded traversal on the application's tagging path.
  static thread_local std::array<Event, 64> samples;
  const uint32_t size = tags == nullptr ? 0 : (std::min)(count, uint32_t{64});
  if (count > 64) g_truncated_tag_batches.fetch_add(1, std::memory_order_relaxed);
  for (uint32_t i = 0; i < size; ++i) {
    samples[i] = SnapshotTag(tags[i], commands);
    if (samples[i].recognized && tags[i].resource != nullptr)
      ResolveTrackedResource(*tags[i].resource, samples[i]);
  }
  ObserveUiTagWindow(tags, samples.data(), size, ticket);
  const auto result = call();
  if (tags == nullptr || count > 64) {
    Event batch(Kind::tag_batch);
    batch.recognized = true;
    batch.values[0] = count;
    batch.values[1] = tags != nullptr;
    batch.values[2] = size;
    Submit(batch, ticket, begin, static_cast<uint32_t>(viewport), frame, result);
  }
  for (uint32_t i = 0; i < size; ++i)
    Submit(samples[i], ticket, begin, static_cast<uint32_t>(viewport), frame, result);
  return result;
}

sl::Result HookTag(const sl::ViewportHandle& viewport, const sl::ResourceTag* tags,
                   uint32_t count, sl::CommandBuffer* commands) {
  return ObserveTags(viewport, UINT32_MAX, tags, count, commands,
                      [&]() { return g_set_tag(viewport, tags, count, commands); });
}

sl::Result HookTagForFrame(const sl::FrameToken& frame, const sl::ViewportHandle& viewport,
                           const sl::ResourceTag* tags, uint32_t count,
                           sl::CommandBuffer* commands) {
  return ObserveTags(viewport, static_cast<uint32_t>(frame), tags, count, commands,
      [&]() { return g_set_tag_for_frame(frame, viewport, tags, count, commands); });
}

sl::Result HookFunction(sl::Feature feature, const char* name, void*& function) {
  const auto result = g_get_function(feature, name, function);
  if (result != sl::Result::eOk || feature != sl::kFeatureDLSS_G ||
      name == nullptr || function == nullptr) return result;
  if (std::strcmp(name, "slDLSSGSetOptions") == 0 &&
      function != reinterpret_cast<void*>(&HookOptions)) {
    g_set_options.store(reinterpret_cast<PFun_slDLSSGSetOptions*>(function), std::memory_order_release);
    function = reinterpret_cast<void*>(&HookOptions);
  } else if (std::strcmp(name, "slDLSSGGetState") == 0 &&
             function != reinterpret_cast<void*>(&HookState)) {
    g_get_state.store(reinterpret_cast<PFun_slDLSSGGetState*>(function), std::memory_order_release);
    function = reinterpret_cast<void*>(&HookState);
  }
  return result;
}

void TryInstall() {
  if (!g_enabled || g_installed.load(std::memory_order_acquire)) return;
  if (!TryAcquireSRWLockExclusive(&g_install_lock)) return;
  HMODULE module = GetModuleHandleW(L"sl.interposer.dll");
  if (module != nullptr && !g_installed.load(std::memory_order_relaxed)) {
    g_hooks.clear();
    const mfgunlock::hook::HookItem candidates[] = {
      {"slGetFeatureFunction", reinterpret_cast<void**>(&g_get_function), reinterpret_cast<void*>(&HookFunction)},
      {"slSetConstants", reinterpret_cast<void**>(&g_set_constants), reinterpret_cast<void*>(&HookConstants)},
      {"slSetTag", reinterpret_cast<void**>(&g_set_tag), reinterpret_cast<void*>(&HookTag)},
      {"slSetTagForFrame", reinterpret_cast<void**>(&g_set_tag_for_frame), reinterpret_cast<void*>(&HookTagForFrame)}
    };
    uint32_t exports = 0;
    for (size_t i = 0; i < std::size(candidates); ++i) {
      if (GetProcAddress(module, std::get<0>(candidates[i])) == nullptr) continue;
      g_hooks.push_back(candidates[i]);
      exports |= 1u << i;
    }
    if (!g_hooks.empty() && mfgunlock::hook::Install(module, g_hooks, "SL diagnostic observation")) {
      g_optional_exports.store(exports, std::memory_order_relaxed);
      g_installed.store(true, std::memory_order_release);
    }
  }
  ReleaseSRWLockExclusive(&g_install_lock);
}

HMODULE WINAPI HookLoadW(LPCWSTR path) {
  HMODULE module = g_load_w(path);
  if (module != nullptr) TryInstall();
  return module;
}

HMODULE WINAPI HookLoadExW(LPCWSTR path, HANDLE file, DWORD flags) {
  HMODULE module = g_load_ex_w(path, file, flags);
  if (module != nullptr && (flags & (LOAD_LIBRARY_AS_DATAFILE |
      LOAD_LIBRARY_AS_DATAFILE_EXCLUSIVE | LOAD_LIBRARY_AS_IMAGE_RESOURCE)) == 0) TryInstall();
  return module;
}

const std::vector<mfgunlock::hook::HookItem> kLoaderHooks = {
  {"LoadLibraryW", reinterpret_cast<void**>(&g_load_w), reinterpret_cast<void*>(&HookLoadW)},
  {"LoadLibraryExW", reinterpret_cast<void**>(&g_load_ex_w), reinterpret_cast<void*>(&HookLoadExW)}
};

Json ModuleInfo(HMODULE module) {
  std::wstring path(32768, L'\0');
  const DWORD length = module ? GetModuleFileNameW(module, path.data(), static_cast<DWORD>(path.size())) : 0;
  if (length == 0 || length >= path.size()) return nullptr;
  path.resize(length);
  Json info = {{"file", std::filesystem::path(path).filename().string()}};
  DWORD ignored = 0;
  std::vector<unsigned char> data(GetFileVersionInfoSizeW(path.c_str(), &ignored));
  if (!data.empty() && GetFileVersionInfoW(path.c_str(), 0, static_cast<DWORD>(data.size()), data.data())) {
    VS_FIXEDFILEINFO* version = nullptr;
    UINT size = 0;
    if (VerQueryValueW(data.data(), L"\\", reinterpret_cast<void**>(&version), &size) &&
        size >= sizeof(VS_FIXEDFILEINFO))
      info["file_version"] = {HIWORD(version->dwFileVersionMS), LOWORD(version->dwFileVersionMS),
                              HIWORD(version->dwFileVersionLS), LOWORD(version->dwFileVersionLS)};
  }
  return info;
}

Json FunctionOwner(const void* function) {
  HMODULE module = nullptr;
  if (function != nullptr)
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(function), &module);
  return ModuleInfo(module);
}

Json CountOwner(const Event& event, size_t address_index, size_t identity_index) {
  const uint64_t address = event.values[address_index];
  const uint64_t base = event.values[identity_index];
  if (!address || address == kUnknown || !base || base == kUnknown) return nullptr;
  const mfgunlock::hook::ModuleIdentity identity{
      reinterpret_cast<HMODULE>(base), static_cast<DWORD>(event.values[identity_index + 1]),
      static_cast<DWORD>(event.values[identity_index + 2])};
  Json owner = {{"rva", address >= base ? Json(address - base) : Json(nullptr)},
                {"timestamp", identity.timestamp}, {"image_bytes", identity.image_bytes}};
  owner["module"] = mfgunlock::hook::IsCurrent(identity) ? ModuleInfo(identity.module) : Json(nullptr);
  return owner;
}

Json CountEvent(const Event& event) {
  static constexpr const char* kBackends[] = {"streamline", "ngx_d3d12", "ngx_vulkan", "ngx_c_api"};
  static constexpr const char* kOperations[] = {"request", "forward", "read", "advertise", "write"};
  static constexpr const char* kOrigins[] = {"unknown", "native", "fixed_override", "retry", "native_fallback",
                                          "dynamic", "backend_fallback", "capability_policy"};
  static constexpr const char* kKeys[] = {"generated", "maximum_generated", "generated_index"};
  Json row = {{"backend", kBackends[event.values[0]]}, {"operation", kOperations[event.values[1]]},
              {"origin", kOrigins[event.values[2]]}, {"key", kKeys[event.values[3]]},
              {"value", event.values[5] ? Json(static_cast<int64_t>(event.values[4])) : Json(nullptr)},
              {"requested", event.values[6] == UINT32_MAX ? Json(nullptr) : Json(event.values[6])},
              {"mode", event.values[7] == UINT32_MAX ? Json(nullptr) : Json(event.values[7])},
              {"ui_fallback", event.values[8] != 0},
              {"caller", CountOwner(event, 9, 12)}, {"callee", CountOwner(event, 10, 15)}};
  return row;
}

Json ObserveNvapi() {
  IUnknown* device = nullptr;
  {
    std::lock_guard lock(g_device_mutex);
    device = g_native_d3d12_device;
    if (device != nullptr) device->AddRef();
  }
  const auto snapshot = nvapi::Observe(device, GetCurrentProcessId());
  if (device != nullptr) device->Release();

  Json result = {
      {"library_loaded", snapshot.library_loaded},
      {"initialize_status", snapshot.initialize_status},
      {"sleep_status_result", snapshot.sleep_status},
      {"latency_result", snapshot.latency_status},
      {"ngx_override_result", snapshot.ngx_override_status},
  };
  if (snapshot.sleep_status == nvapi::kOk) {
    result["sleep"] = {
        {"low_latency_mode", snapshot.sleep.low_latency_mode != 0},
        {"fullscreen_vrr", snapshot.sleep.fullscreen_vrr != 0},
        {"control_panel_vsync", snapshot.sleep.control_panel_vsync != 0},
        {"sleep_interval_us", snapshot.sleep.sleep_interval_us},
        {"game_sleep", snapshot.sleep.game_sleep != 0},
        {"fullscreen_independent_flip",
         snapshot.sleep.fullscreen_independent_flip != 0},
        {"frame_generation_multiplier",
         snapshot.sleep.frame_generation_multiplier},
        {"dynamic_frame_generation_control",
         snapshot.sleep.dynamic_frame_generation_control != 0},
        {"dynamic_frame_time_target_us",
         snapshot.sleep.dynamic_frame_time_target_us},
    };
  }
  if (snapshot.latency_status == nvapi::kOk) {
    result["latency_frames"] = Json::array();
    for (const auto& frame : snapshot.latency.frames) {
      if (frame.frame_id == 0) continue;
      result["latency_frames"].push_back({
          {"frame_id", frame.frame_id},
          {"gpu_active_render_time_us", frame.gpu_active_render_time_us},
          {"gpu_frame_time_us", frame.gpu_frame_time_us},
          {"cross_adapter_copy_time_us", frame.cross_adapter_copy_time_us},
          {"ai_frame_time_us", frame.ai_frame_time_us},
          {"input_sample_time", frame.input_sample_time},
          {"simulation_start_time", frame.simulation_start_time},
          {"simulation_end_time", frame.simulation_end_time},
          {"render_submit_start_time", frame.render_submit_start_time},
          {"render_submit_end_time", frame.render_submit_end_time},
          {"present_start_time", frame.present_start_time},
          {"present_end_time", frame.present_end_time},
          {"driver_start_time", frame.driver_start_time},
          {"driver_end_time", frame.driver_end_time},
          {"os_render_queue_start_time", frame.os_render_queue_start_time},
          {"os_render_queue_end_time", frame.os_render_queue_end_time},
          {"gpu_render_start_time", frame.gpu_render_start_time},
          {"gpu_render_end_time", frame.gpu_render_end_time},
          {"camera_constructed_time", frame.camera_constructed_time},
      });
    }
  }
  if (snapshot.ngx_override_status == nvapi::kOk) {
    result["ngx_override"] = {
        {"feedback_super_resolution", snapshot.ngx.feedback_super_resolution},
        {"feedback_ray_reconstruction", snapshot.ngx.feedback_ray_reconstruction},
        {"feedback_frame_generation", snapshot.ngx.feedback_frame_generation},
        {"scaling_ratio", snapshot.ngx.scaling_ratio},
        {"performance_mode", snapshot.ngx.performance_mode},
        {"render_preset", snapshot.ngx.render_preset},
        {"frame_generation_count", snapshot.ngx.frame_generation_count},
        {"frame_generation_preset", snapshot.ngx.frame_generation_preset},
        {"frame_generation_mode", snapshot.ngx.frame_generation_mode},
    };
  }
  return result;
}

uint64_t UiCandidateScore(const UiCandidateStats& candidate) {
  return static_cast<uint64_t>(candidate.post_hudless_srv_push_count) * 1000000 +
         static_cast<uint64_t>(candidate.post_hudless_transparent_clear_count) * 100000 +
         static_cast<uint64_t>(candidate.post_hudless_rtv_bind_count) * 1000 +
         static_cast<uint64_t>(candidate.srv_push_count) * 100 +
         static_cast<uint64_t>(candidate.transparent_clear_count) * 10 +
         candidate.rtv_bind_count;
}


const char* KindName(Kind kind) {
  switch (kind) {
    case Kind::options: return "streamline_options";
    case Kind::constants: return "streamline_constants";
    case Kind::tag: return "resource_tag";
    case Kind::state: return "streamline_state";
    case Kind::output: return "host_present";
    case Kind::tag_batch: return "resource_tag_batch";
    case Kind::frame_count: return "frame_count";
    case Kind::evaluate: return "ngx_evaluate";
    case Kind::lifecycle: return "fg_lifecycle";
    default: return "unknown";
  }
}

Json OptionalValue(uint64_t value) {
  return value == kUnknown ? Json(nullptr) : Json(value);
}

Json EvaluateResourceJson(const Event& event, size_t index,
                          std::unordered_map<uint64_t, uint64_t>& identities) {
  const size_t base = 18 + index * 8;
  const bool known = event.values[base + 1] != 0 && event.values[base + 1] != kUnknown;
  static constexpr const char* kNames[] = {
      "backbuffer", "depth", "motion_vectors", "hudless", "ui", "ui_alpha",
      "bidirectional_distortion_field", "output_interpolated", "output_real",
      "output_disable_interpolation"};
  Json row = {{"key", index < std::size(kNames) ? kNames[index] : "unknown"},
              {"known", known}, {"state", nullptr}, {"content_hash", nullptr},
              {"binary_file", nullptr}, {"readback_available", false},
              {"readback_reason", "resource state and DLSS-G pacer synchronization are not verified"}};
  if (!known) return row;
  const uint64_t object = event.values[base + 2];
  Json object_id = nullptr;
  if (object != 0 && object != kUnknown) {
    auto [it, inserted] = identities.try_emplace(object, identities.size() + 1);
    object_id = it->second;
  }
  const uint64_t packed = event.values[base + 5];
  const uint64_t dimension_flags = event.values[base + 7];
  row.update({{"resource", object_id}, {"width", event.values[base + 3]},
              {"height", event.values[base + 4]},
              {"depth_or_array", static_cast<uint16_t>(packed & 0xffff)},
              {"mip_levels", static_cast<uint16_t>((packed >> 16) & 0xffff)},
              {"sample_count", static_cast<uint32_t>(packed >> 32)},
              {"format", event.values[base + 6]},
              {"dimension", static_cast<uint32_t>(dimension_flags & 0xffffffffu)},
              {"flags", static_cast<uint32_t>(dimension_flags >> 32)}});
  return row;
}

Json EvaluateEventJson(const Event& event, std::unordered_map<uint64_t, uint64_t>& identities) {
  const bool input_reset_known = event.values[11] != kUnknown && event.values[11] != 0;
  const bool automode_reset_known = event.values[13] != kUnknown && event.values[13] != 0;
  const bool input_reset = input_reset_known && event.values[12] != 0;
  const bool automode_reset = automode_reset_known && event.values[14] != 0;
  const bool reset = input_reset || automode_reset;
  const bool reset_known = reset || (input_reset_known && automode_reset_known);
  const bool count_known = event.values[7] != kUnknown && event.values[7] != 0;
  const bool index_known = event.values[9] != kUnknown && event.values[9] != 0;
  const uint32_t count = static_cast<uint32_t>(event.values[8]);
  const uint32_t index = static_cast<uint32_t>(event.values[10]);
  const size_t output_base = 18 + static_cast<size_t>(mfgunlock::diagnostic::ResourceKey::kOutputInterpolated) * 8;
  const bool output_known = event.values[output_base + 1] != 0 &&
      event.values[output_base + 1] != kUnknown && event.values[output_base + 2] != 0 &&
      event.values[output_base + 2] != kUnknown;
  timeline::Classification classification;
  if (reset) classification.kind = timeline::FrameKind::kReset;
  else if (reset_known && count_known && index_known && count != 0 && index != 0 &&
           index <= count && output_known)
    classification = {timeline::FrameKind::kGenerated, index, count};
  Json row = {
      {"evaluation_id", event.values[1]},
      {"phase", event.values[0] == 0 ? "begin" : "end"},
      {"backend", "ngx_d3d12"},
      {"caller", CountOwner(event, 3, 98)},
      {"generated_count", count_known ? Json(count) : Json(nullptr)},
      {"generated_index", index_known ? Json(index) : Json(nullptr)},
      {"reset", reset_known ? Json(reset) : Json(nullptr)},
      {"input_reset", input_reset_known ? Json(input_reset) : Json(nullptr)},
      {"automode_override_reset", automode_reset_known ? Json(automode_reset) : Json(nullptr)},
      {"backbuffer_frame_id", event.values[15] != kUnknown && event.values[15] != 0 ? OptionalValue(event.values[16]) : Json(nullptr)},
      {"frame_kind", timeline::FrameKindName(classification.kind)},
      {"marker_label", timeline::MarkerLabel(classification).empty() ? Json(nullptr) : Json(timeline::MarkerLabel(classification))},
      {"displayed", nullptr},
      {"display_note", "provider-generated candidate; Streamline pacing may drop generated frames"},
      {"resources", Json::array()},
  };
  for (size_t i = 0; i < static_cast<size_t>(mfgunlock::diagnostic::ResourceKey::kCount); ++i)
    row["resources"].push_back(EvaluateResourceJson(event, i, identities));
  return row;
}

Json BridgeResourceJson(const mfgunlock::diagnostic::ResourceSnapshot& resource,
                        std::unordered_map<uint64_t, uint64_t>& identities) {
  static constexpr const char* kNames[] = {
      "backbuffer", "depth", "motion_vectors", "hudless", "ui", "ui_alpha",
      "bidirectional_distortion_field", "output_interpolated", "output_real",
      "output_disable_interpolation"};
  const size_t index = resource.key;
  Json row = {{"key", index < std::size(kNames) ? kNames[index] : "unknown"},
              {"known", resource.known != 0}, {"state", nullptr}, {"content_hash", nullptr},
              {"binary_file", nullptr}, {"readback_available", false},
              {"readback_reason", "resource state and DLSS-G pacer synchronization are not verified"}};
  if (!resource.known) return row;
  Json object = nullptr;
  if (resource.object != 0) {
    auto [it, inserted] = identities.try_emplace(resource.object, identities.size() + 1);
    object = it->second;
  }
  row.update({{"resource", object}, {"width", resource.width}, {"height", resource.height},
              {"depth_or_array", resource.depth_or_array}, {"mip_levels", resource.mip_levels},
              {"sample_count", resource.sample_count}, {"format", resource.format},
              {"dimension", resource.dimension}, {"flags", resource.flags}});
  return row;
}

Json BridgeEvaluateJson(const mfgunlock::diagnostic::EvaluateEvent& event,
                        std::unordered_map<uint64_t, uint64_t>& identities) {
  const auto classification = timeline::Classify(event);
  const bool reset = timeline::ResetTrue(event);
  const bool reset_known = timeline::ResetKnown(event);
  Json row = {{"evaluation_id", event.evaluation_id},
              {"phase", event.phase == mfgunlock::diagnostic::EvaluatePhase::kBegin ? "begin" : "end"},
              {"backend", "ngx_d3d12"}, {"viewport", nullptr},
              {"generated_count", event.generated_count_known ? Json(event.generated_count) : Json(nullptr)},
              {"generated_index", event.generated_index_known ? Json(event.generated_index) : Json(nullptr)},
              {"reset", reset_known ? Json(reset) : Json(nullptr)},
              {"input_reset", event.reset_known ? Json(event.reset != 0) : Json(nullptr)},
              {"automode_override_reset", event.automode_reset_known ? Json(event.automode_reset != 0) : Json(nullptr)},
              {"backbuffer_frame_id", event.backbuffer_frame_id_known ? Json(event.backbuffer_frame_id) : Json(nullptr)},
              {"frame_kind", timeline::FrameKindName(classification.kind)},
              {"marker_label", timeline::MarkerLabel(classification).empty() ? Json(nullptr) : Json(timeline::MarkerLabel(classification))},
              {"result", event.result == mfgunlock::diagnostic::kUnknown32 ? Json(nullptr) : Json(event.result)},
              {"resources", Json::array()}};
  for (const auto& resource : event.resources)
    row["resources"].push_back(BridgeResourceJson(resource, identities));
  return row;
}

Json DiagnosticStateJson() {
  if (!g_query_state) return nullptr;
  mfgunlock::diagnostic::DiagnosticState state;
  if (!g_query_state(mfgunlock::diagnostic::kStateVersion, &state)) return nullptr;
  static constexpr const char* kArchitectures[] = {"auto", "ada", "ampere", "turing", "unknown"};
  static constexpr const char* kTemporal[] = {"none", "midpoint", "blackwell"};
  const uint32_t architecture = state.architecture;
  const uint32_t temporal = static_cast<uint32_t>(state.temporal_backend);
  const auto mode_json = [](uint32_t known, uint32_t mode) -> Json {
    if (!known) return nullptr;
    switch (mode) {
      case 0: return "off";
      case 1: return "on";
      case 2: return "auto";
      case 3: return "dynamic";
      default: return mode;
    }
  };
  const auto multiplier_json = [](uint32_t known, uint32_t generated) -> Json {
    return known ? Json(generated == 0 ? 0u : generated + 1u) : Json(nullptr);
  };
  const auto verdict_json = [](uint32_t verdict) -> Json {
    switch (verdict) {
      case 0: return "UNKNOWN";
      case 1: return "OBSERVED";
      case 2: return "VALID";
      case 3: return "INVALID";
      default: return nullptr;
    }
  };
  const auto output_encoding_json = [&state]() -> Json {
    if (!state.swapchain_color_space_known) return nullptr;
    const auto output = mfgunlock::outputstate::Classify(
        mfgunlock::outputstate::FormatKind::kUnknown,
        mfgunlock::outputstate::kUnknown32, false,
        state.swapchain_color_space, true);
    return output.known
        ? Json(mfgunlock::outputstate::EncodingName(output.encoding))
        : Json(nullptr);
  };
  return {
      {"architecture", architecture < std::size(kArchitectures) ? Json(kArchitectures[architecture]) : Json(nullptr)},
      {"target_sm", state.target_sm ? Json(state.target_sm) : Json(nullptr)},
      {"temporal_backend", temporal < std::size(kTemporal) ? Json(kTemporal[temporal]) : Json(nullptr)},
      {"temporal_target_sm", state.temporal_target_sm ? Json(state.temporal_target_sm) : Json(nullptr)},
      {"boundary_mode", state.boundary_mode == UINT32_MAX ? Json(nullptr) : Json(state.boundary_mode)},
      {"warp_applied", state.warp_applied != 0},
      {"warp_target_sm", state.warp_target_sm ? Json(state.warp_target_sm) : Json(nullptr)},
      {"dynamic_requested", state.dynamic_requested != 0},
      {"dynamic_support_seen", state.dynamic_support_seen != 0},
      {"dynamic_supported", state.dynamic_support_seen ? Json(state.dynamic_supported != 0) : Json(nullptr)},
      {"dynamic_applied", state.dynamic_applied != 0},
      {"dynamic_setoptions_accepted", state.accepted_mode_known
          ? Json(state.accepted_mode == static_cast<uint32_t>(sl::DLSSGMode::eDynamic))
          : Json(nullptr)},
      {"dynamic_effective", state.observed_mode_known
          ? Json(state.observed_mode == static_cast<uint32_t>(sl::DLSSGMode::eDynamic))
          : Json(nullptr)},
      {"fixed_multiplier_override", state.fixed_multiplier},
      {"requested_multiplier", multiplier_json(state.requested_generated_known, state.requested_generated)},
      {"forwarded_multiplier", multiplier_json(state.forwarded_generated_known, state.forwarded_generated)},
      {"accepted_multiplier", multiplier_json(state.accepted_generated_known, state.accepted_generated)},
      {"provider_applied_multiplier", multiplier_json(
          state.provider_applied_generated_known, state.provider_applied_generated)},
      {"requested_mode", mode_json(state.requested_mode_known, state.requested_mode)},
      {"forwarded_mode", mode_json(state.forwarded_mode_known, state.forwarded_mode)},
      {"accepted_mode", mode_json(state.accepted_mode_known, state.accepted_mode)},
      {"observed_mode", mode_json(state.observed_mode_known, state.observed_mode)},
      {"structural_max_multiplier", multiplier_json(state.structural_max_known, state.structural_max_generated)},
      {"runtime_max_multiplier", multiplier_json(state.runtime_max_known, state.runtime_max_generated)},
      {"effective_max_multiplier", multiplier_json(state.effective_max_known, state.effective_max_generated)},
      {"game_ui_max_multiplier", multiplier_json(state.game_ui_max_known, state.game_ui_max_generated)},
      {"effective_generated", state.effective_generated},
      {"preset_requested", state.preset_requested},
      {"preset_supplied", state.preset_supplied < 0 ? Json(nullptr) : Json(state.preset_supplied)},
      {"preset_applied", state.preset_applied < 0 ? Json(nullptr) : Json(state.preset_applied)},
      {"hdr", state.hdr_state_seen ? Json(state.hdr_active != 0) : Json(nullptr)},
      {"output_encoding", output_encoding_json()},
      {"swapchain_color_space", state.swapchain_color_space_known ? Json(state.swapchain_color_space) : Json(nullptr)},
      {"dlssg_color_space", state.dlssg_color_space_known ? Json(state.dlssg_color_space) : Json(nullptr)},
      {"native_uir_observed", state.native_uir_observed != 0},
      {"automatic_uir_eligible", state.automatic_uir_eligible != 0},
      {"automatic_uir_applied", state.automatic_uir_applied != 0},
      {"automatic_uir_rejected", state.automatic_uir_rejected != 0},
      {"automatic_uir_reject_reasons", state.automatic_uir_reject_reasons},
      {"ui_composition_requested", state.ui_composition_requested != 0},
      {"ui_composition_applied", state.ui_composition_applied != 0},
      {"ui_composition_fallback", state.ui_composition_fallback != 0},
      {"prepared_provider_count", state.prepared_provider_count},
      {"integration_contract", {
          {"frame_index", verdict_json(state.frame_contract)},
          {"resources", verdict_json(state.resource_contract)},
          {"dynamic_resolution", verdict_json(state.dynamic_resolution_contract)},
          {"queue", verdict_json(state.queue_contract)},
          {"swapchain", verdict_json(state.swapchain_contract)},
          {"viewport_ownership", verdict_json(state.viewport_contract)},
          {"status_raw", state.dlssg_status_known ? Json(state.dlssg_status_raw) : Json(nullptr)},
          {"status_unknown_bits", state.dlssg_status_known ? Json(state.dlssg_status_unknown_bits) : Json(nullptr)},
          {"fail_reflex_missing", state.dlssg_status_known ? Json(state.fail_reflex_missing != 0) : Json(nullptr)},
          {"fail_hdr_unsupported", state.dlssg_status_known ? Json(state.fail_hdr_unsupported != 0) : Json(nullptr)},
          {"fail_common_constants", state.dlssg_status_known ? Json(state.fail_constants_invalid != 0) : Json(nullptr)},
          {"fail_backbuffer_index", state.dlssg_status_known ? Json(state.fail_backbuffer_index_missing != 0) : Json(nullptr)},
          {"resource_seen_mask", state.resource_seen_mask},
          {"resource_active_mask", state.resource_active_mask},
          {"resource_clear_mask", state.resource_clear_mask},
          {"dynamic_resolution_enabled", state.dynamic_resolution_enabled != 0},
          {"queue_parallelism_mode", state.queue_parallelism_known ? Json(state.queue_parallelism_mode) : Json(nullptr)},
          {"completion_fence", state.completion_fence_known ? Json(state.completion_fence) : Json(nullptr)},
          {"completion_fence_value", state.completion_fence_known ? Json(state.completion_fence_value) : Json(nullptr)},
          {"swapchain_recreations", state.swapchain_recreation_count},
          {"swapchain_resizes", state.swapchain_resize_count},
          {"waitable_object_ownership", state.waitable_object_ownership_known ? Json("OBSERVED") : Json("UNKNOWN")},
          {"fullscreen_transition", state.fullscreen_transition_known ? Json("OBSERVED") : Json("UNKNOWN")},
          {"iflip", state.iflip_known ? Json("OBSERVED") : Json("UNKNOWN")},
      }},
  };
}

std::wstring ModulePath(HMODULE module) {
  std::wstring path(32768, L'\0');
  const DWORD length = module ? GetModuleFileNameW(module, path.data(), static_cast<DWORD>(path.size())) : 0;
  if (length == 0 || length >= path.size()) return {};
  path.resize(length);
  return path;
}

HMODULE FindLoadedModule(const wchar_t* wanted) {
  HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, GetCurrentProcessId());
  if (snapshot == INVALID_HANDLE_VALUE) return nullptr;
  HMODULE found = nullptr;
  MODULEENTRY32W entry{};
  entry.dwSize = sizeof(entry);
  if (Module32FirstW(snapshot, &entry)) do {
    std::wstring name(entry.szModule);
    std::transform(name.begin(), name.end(), name.begin(), towlower);
    std::wstring target(wanted ? wanted : L"");
    std::transform(target.begin(), target.end(), target.begin(), towlower);
    if (name == target) { found = entry.hModule; break; }
  } while (Module32NextW(snapshot, &entry));
  CloseHandle(snapshot);
  return found;
}

std::string Sha256File(const std::wstring& path) {
  if (path.empty()) return {};
  HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                            nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) return {};
  BCRYPT_ALG_HANDLE algorithm = nullptr;
  BCRYPT_HASH_HANDLE hash = nullptr;
  DWORD object_bytes = 0, hash_bytes = 0, returned = 0;
  std::vector<unsigned char> object;
  std::vector<unsigned char> digest;
  bool ok = BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) == 0 &&
      BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&object_bytes),
                        sizeof(object_bytes), &returned, 0) == 0 &&
      BCryptGetProperty(algorithm, BCRYPT_HASH_LENGTH, reinterpret_cast<PUCHAR>(&hash_bytes),
                        sizeof(hash_bytes), &returned, 0) == 0;
  if (ok) {
    object.resize(object_bytes);
    digest.resize(hash_bytes);
    ok = BCryptCreateHash(algorithm, &hash, object.data(), object_bytes, nullptr, 0, 0) == 0;
  }
  std::array<unsigned char, 1 << 16> buffer{};
  while (ok) {
    DWORD read = 0;
    if (!ReadFile(file, buffer.data(), static_cast<DWORD>(buffer.size()), &read, nullptr)) { ok = false; break; }
    if (read == 0) break;
    ok = BCryptHashData(hash, buffer.data(), read, 0) == 0;
  }
  if (ok) ok = BCryptFinishHash(hash, digest.data(), hash_bytes, 0) == 0;
  if (hash) BCryptDestroyHash(hash);
  if (algorithm) BCryptCloseAlgorithmProvider(algorithm, 0);
  CloseHandle(file);
  if (!ok) return {};
  static constexpr char kHex[] = "0123456789abcdef";
  std::string result(digest.size() * 2, '0');
  for (size_t i = 0; i < digest.size(); ++i) {
    result[i * 2] = kHex[digest[i] >> 4];
    result[i * 2 + 1] = kHex[digest[i] & 0xf];
  }
  return result;
}

bool WriteTextFile(const std::filesystem::path& path, const std::string& data) {
  HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                            FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) return false;
  DWORD written = 0;
  const bool saved = WriteFile(file, data.data(), static_cast<DWORD>(data.size()), &written, nullptr) &&
      written == data.size();
  CloseHandle(file);
  return saved;
}

// Formatting, module inventory and file I/O run only on the explicitly requested
// export worker after recording has stopped. Never enumerate modules in Present.
DWORD WINAPI ExportWorker(LPVOID pinned_module) {
  std::string message;
  bool export_succeeded = false;
  try {
    Json root = {{"schema", 1}, {"label", kLabels[g_recorded_label]},
      {"qpc_frequency", g_frequency}, {"dropped_events", g_capture.dropped.load()},
      {"capacity_exhausted", g_capture.full.load()},
      {"truncated_tag_batches", g_truncated_tag_batches.load()},
      {"capture_started_ms", g_capture_started_ms.load(std::memory_order_relaxed)},
      {"capture_stopped_ms", g_capture_stopped_ms.load(std::memory_order_relaxed)},
      {"interposer_export_mask", g_optional_exports.load()},
      {"options_downstream_owner", FunctionOwner(reinterpret_cast<const void*>(g_set_options.load()))},
      {"state_downstream_owner", FunctionOwner(reinterpret_cast<const void*>(g_get_state.load()))},
      {"nvapi_at_export", ObserveNvapi()},
      {"dynamic_mfg_capability", {
          {"seen", g_dynamic_capability_seen.load(std::memory_order_acquire)},
          {"supported", g_dynamic_capability_seen.load(std::memory_order_acquire)
              ? Json(g_dynamic_supported.load(std::memory_order_relaxed)) : Json(nullptr)},
          {"state_version", g_dynamic_state_version.load(std::memory_order_relaxed)},
          {"source", g_dynamic_capability_seen.load(std::memory_order_acquire)
              ? Json(g_dynamic_capability_from_probe.load(std::memory_order_relaxed)
                    ? "one-shot probe" : "game GetState")
              : Json(nullptr)},
          {"probe_attempted", g_dynamic_probe_attempted.load(std::memory_order_acquire)},
          {"probe_result", g_dynamic_probe_result.load(std::memory_order_relaxed) == UINT32_MAX
              ? Json(nullptr) : Json(g_dynamic_probe_result.load(std::memory_order_relaxed))}}},
      {"notes", "Read-only diagnostic boundary. NGX D3D12 Evaluate events are exact provider-call observations, "
                "but generated candidates are not proof of displayed pixels because Streamline pacing may drop them. "
                "Legacy tags have no explicit frame ID. Format does not establish color encoding. "
                "Other addon wrappers may be downstream. The optional one-shot Dynamic MFG probe "
                "issues one extra slDLSSGGetState query on the game's GetState thread; no Streamline "
                "setters, NVAPI setters or latency markers are issued. UI candidates are heuristic: "
                "they are full-output render targets observed during capture, with post-HUD-less timing "
                "and push-descriptor SRV evidence when available; descriptor-table SRV use is not "
                "exhaustive. NVAPI is sampled only at export."}};
    root["events"] = Json::array();
    Json trace_rows = Json::array();
    uint64_t evaluate_begin_count = 0, evaluate_end_count = 0;
    uint64_t generated_count = 0, source_count = 0, reset_count = 0, unknown_evaluate_count = 0;
    uint64_t host_present_count = 0, frame_count_events = 0, options_count = 0, state_count = 0;
    uint64_t dynamic_option_requests = 0, dynamic_transitions = 0;
    int last_dynamic_mode = -1;
    uint64_t fixed_count_events = 0, retry_count_events = 0, fallback_count_events = 0;
    std::unordered_map<uint64_t, uint64_t> ids;
    auto identity = [&](uint64_t value) -> Json {
      if (value == kUnknown || value == 0) return nullptr;
      auto [it, inserted] = ids.try_emplace(value, ids.size() + 1);
      return it->second;
    };
    {
      std::lock_guard lock(g_capture.mutex);
      for (const auto& event : g_capture.events) {
        Json row = {{"kind", static_cast<int>(event.kind)}, {"sequence", event.sequence},
          {"begin_qpc", event.begin}, {"end_qpc", event.end}, {"thread", event.thread},
          {"version", event.version}, {"recognized", event.recognized}};
        Json trace = {{"schema", 2}, {"event", KindName(event.kind)},
          {"sequence", event.sequence}, {"begin_qpc", event.begin}, {"end_qpc", event.end},
          {"thread_id", event.thread}, {"recognized", event.recognized}};
        trace["viewport"] = event.viewport == UINT32_MAX ? Json(nullptr) : Json(event.viewport);
        trace["frame_id"] = event.frame == UINT32_MAX ? Json(nullptr) : Json(event.frame);
        trace["result"] = event.result == UINT32_MAX ? Json(nullptr) : Json(event.result);
        row["viewport"] = event.viewport == UINT32_MAX ? Json(nullptr) : Json(event.viewport);
        row["frame"] = event.frame == UINT32_MAX ? Json(nullptr) : Json(event.frame);
        row["result"] = event.result == UINT32_MAX ? Json(nullptr) : Json(event.result);
        row["values"] = Json::array();
        // Schema 1 exposed exactly 32 value slots. Keep that shape stable;
        // schema 2 carries the appended Evaluate/resource metadata separately.
        for (size_t i = 0; i < 32; ++i) {
          const auto value = event.values[i];
          row["values"].push_back(value == kUnknown ? Json(nullptr) : Json(value));
        }
        row["floats"] = event.floats;
        if (event.kind == Kind::options) {
          trace["options"] = {{"mode", OptionalValue(event.values[0])},
              {"generated_requested", OptionalValue(event.values[1])},
              {"flags", OptionalValue(event.values[2])},
              {"ui_recomposition", OptionalValue(event.values[16])}};
          ++options_count;
          if (event.values[0] != kUnknown) {
            const int dynamic = event.values[0] == static_cast<uint32_t>(sl::DLSSGMode::eDynamic) ? 1 : 0;
            if (dynamic != 0) ++dynamic_option_requests;
            if (last_dynamic_mode >= 0 && dynamic != last_dynamic_mode) ++dynamic_transitions;
            last_dynamic_mode = dynamic;
          }
        } else if (event.kind == Kind::state) {
          trace["state"] = {{"status", OptionalValue(event.values[0])},
              {"frames_actually_presented", OptionalValue(event.values[1])},
              {"generated_max", OptionalValue(event.values[2])},
              {"dynamic_supported", OptionalValue(event.values[4])}};
          ++state_count;
        } else if (event.kind == Kind::constants) {
          trace["constants"] = {{"reset", OptionalValue(event.values[3])}};
        } else if (event.kind == Kind::tag) {
          row["values"][4] = identity(event.values[4]);
          row["values"][13] = identity(event.values[13]);
          trace["resource_tag"] = {{"type", OptionalValue(event.values[0])},
              {"resource", identity(event.values[4])}, {"width", OptionalValue(event.values[5])},
              {"height", OptionalValue(event.values[6])}, {"format", OptionalValue(event.values[7])}};
        } else if (event.kind == Kind::frame_count) {
          row["frame_count"] = CountEvent(event);
          trace["frame_count"] = row["frame_count"];
          ++frame_count_events;
          if (event.values[2] == static_cast<uint32_t>(mfgunlock::countobserver::Origin::kFixed)) ++fixed_count_events;
          if (event.values[2] == static_cast<uint32_t>(mfgunlock::countobserver::Origin::kRetry)) ++retry_count_events;
          if (event.values[2] == static_cast<uint32_t>(mfgunlock::countobserver::Origin::kNativeFallback)) ++fallback_count_events;
          row["values"][11] = identity(event.values[11]);
          for (size_t index : {size_t{9}, size_t{10}, size_t{12}, size_t{15}})
            row["values"][index] = nullptr;
        } else if (event.kind == Kind::output) {
          row["values"][0] = identity(event.values[0]);
          trace["host_present"] = {{"swapchain", identity(event.values[0])},
              {"color_space", OptionalValue(event.values[1])},
              {"format", OptionalValue(event.values[2])}, {"width", OptionalValue(event.values[3])},
              {"height", OptionalValue(event.values[4])}, {"backbuffer_count", OptionalValue(event.values[5])},
              {"output_encoding", event.values[8] == 1
                  ? Json(mfgunlock::outputstate::EncodingName(
                        static_cast<mfgunlock::outputstate::Encoding>(event.values[6])))
                  : Json(nullptr)},
              {"hdr", event.values[9] == kUnknown ? Json(nullptr) : Json(event.values[9] != 0)},
              {"dlssg_output_supported", event.values[8] == 1
                  ? Json(event.values[7] != 0) : Json(nullptr)},
              {"classification", "host_present_boundary_only"}};
          ++host_present_count;
        } else if (event.kind == Kind::lifecycle) {
          trace["event"] = mfgunlock::diagnostic::LifecycleName(
              static_cast<mfgunlock::diagnostic::LifecycleKind>(event.values[0]));
          trace["lifecycle"] = {{"epoch", OptionalValue(event.values[1])}, {"handle", identity(event.values[2])},
              {"handles", OptionalValue(event.values[3])}, {"evaluates_in_flight", OptionalValue(event.values[4])},
              {"creates_in_flight", OptionalValue(event.values[5])}, {"releases_in_flight", OptionalValue(event.values[6])},
              {"tracking_uncertain", event.values[7] == kUnknown ? Json(nullptr) : Json(event.values[7] != 0)},
              {"requested_quality", OptionalValue(event.values[8])},
              {"prepared_quality", OptionalValue(event.values[9])},
              {"mode", OptionalValue(event.values[10])}, {"cuda_modules_retired", nullptr}};
        } else if (event.kind == Kind::evaluate) {
          const Json evaluation = EvaluateEventJson(event, ids);
          row["evaluate"] = evaluation;
          trace["evaluate"] = evaluation;
          if (event.values[0] == 0) {
            ++evaluate_begin_count;
            const std::string kind = evaluation.value("frame_kind", "unknown");
            if (kind == "generated") ++generated_count;
            else if (kind == "source") ++source_count;
            else if (kind == "reset") ++reset_count;
            else ++unknown_evaluate_count;
          } else {
            ++evaluate_end_count;
          }
        }
        if (event.kind != Kind::evaluate && event.kind != Kind::lifecycle) root["events"].push_back(std::move(row));
        trace_rows.push_back(std::move(trace));
      }
    }
    root["mfg_probe"] = {{"version", 1}, {"core_bridge", g_capture_had_count_bridge},
                         {"evaluate_bridge", g_capture_had_evaluate_bridge},
                         {"state_bridge", g_capture_had_state_bridge},
                         {"lifecycle_bridge", g_capture_had_lifecycle_bridge},
                         {"ngx_c_api_hooks", probe::g_hook_count},
                         {"capacity_exhausted", probe::g_capacity_exhausted},
                         {"coverage", "core forwarding/capability calls and exported NGX C integer APIs; not arbitrary C++ vtable traffic"},
                         {"counts_are", "API values, not proof of display cadence"}};
    root["ui_tag_summary"] = {
        {"hudless", g_hudless_tag_count.load(std::memory_order_relaxed)},
        {"ui_color_and_alpha", g_ui_color_alpha_tag_count.load(std::memory_order_relaxed)},
        {"ui_alpha", g_ui_alpha_tag_count.load(std::memory_order_relaxed)},
        {"candidate_overflow", g_ui_candidate_overflow.load(std::memory_order_relaxed)}};
    root["ui_candidates"] = Json::array();
    {
      std::lock_guard lock(g_ui_candidates_mutex);
      std::vector<UiCandidateStats> candidates;
      candidates.reserve(kMaxUiCandidates);
      for (const auto& candidate : g_ui_candidates) {
        if (candidate.used) candidates.push_back(candidate);
      }
      std::sort(candidates.begin(), candidates.end(), [](const auto& left, const auto& right) {
        if (left.seen_as_swapchain != right.seen_as_swapchain)
          return !left.seen_as_swapchain;
        if ((left.hudless_tag_matches != 0) != (right.hudless_tag_matches != 0))
          return left.hudless_tag_matches == 0;
        return UiCandidateScore(left) > UiCandidateScore(right);
      });
      for (const auto& candidate : candidates) {
        const uint64_t score = UiCandidateScore(candidate);
        root["ui_candidates"].push_back({
            {"resource", identity(candidate.resource)},
            {"width", candidate.width}, {"height", candidate.height},
            {"format", candidate.format}, {"view_format", candidate.view_format},
            {"ui_encoding", mfgunlock::outputstate::UiEncodingName(
                mfgunlock::outputstate::DetectedUiEncoding(
                    mfgunlock::outputstate::ClassifyFormat(
                        candidate.view_format, candidate.view_format != 0)))},
            {"view_usage", candidate.view_usage}, {"flags", candidate.flags},
            {"swapchain", candidate.seen_as_swapchain},
            {"hudless_tag_matches", candidate.hudless_tag_matches},
            {"ui_candidate", !candidate.seen_as_swapchain &&
                             candidate.hudless_tag_matches == 0},
            {"rtv_binds", candidate.rtv_bind_count}, {"clears", candidate.clear_count},
            {"transparent_clears", candidate.transparent_clear_count},
            {"srv_push_binds", candidate.srv_push_count},
            {"post_hudless_rtv_binds", candidate.post_hudless_rtv_bind_count},
            {"post_hudless_clears", candidate.post_hudless_clear_count},
            {"post_hudless_transparent_clears",
             candidate.post_hudless_transparent_clear_count},
            {"post_hudless_srv_push_binds", candidate.post_hudless_srv_push_count},
            {"first_qpc", candidate.first_qpc}, {"last_qpc", candidate.last_qpc},
            {"first_after_hudless_ticks", candidate.first_after_hudless_ticks},
            {"last_before_present_ticks", candidate.last_before_present_ticks},
            {"last_clear", candidate.last_clear}, {"rank_score", score}});
      }
    }
    root["loaded_candidates_at_export"] = Json::array();
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, GetCurrentProcessId());
    if (snapshot != INVALID_HANDLE_VALUE) {
      MODULEENTRY32W entry{};
      entry.dwSize = sizeof(entry);
      if (Module32FirstW(snapshot, &entry)) do {
        std::wstring path(entry.szExePath);
        std::transform(path.begin(), path.end(), path.begin(), towlower);
        const auto name = std::filesystem::path(path).filename().wstring();
        if (name.starts_with(L"sl.") || name.find(L"nvngx") != std::wstring::npos ||
            name.find(L"mfg") != std::wstring::npos || path.find(L"\\models\\dlssg\\") != std::wstring::npos)
          root["loaded_candidates_at_export"].push_back(ModuleInfo(entry.hModule));
      } while (Module32NextW(snapshot, &entry));
      CloseHandle(snapshot);
    }
    const HMODULE provider_module = FindLoadedModule(L"nvngx_dlssg.dll");
    const std::wstring provider_path = ModulePath(provider_module);
    const std::string provider_sha256 = Sha256File(provider_path);
    const Json core_state = DiagnosticStateJson();

    Json summary = {
        {"schema", 2}, {"label", kLabels[g_recorded_label]}, {"qpc_frequency", g_frequency},
        {"capture_started_ms", g_capture_started_ms.load(std::memory_order_relaxed)},
        {"capture_stopped_ms", g_capture_stopped_ms.load(std::memory_order_relaxed)},
        {"evaluate_begin", evaluate_begin_count}, {"evaluate_end", evaluate_end_count},
        {"generated_evaluations", generated_count}, {"source_evaluations", source_count},
        {"reset_evaluations", reset_count}, {"unknown_evaluations", unknown_evaluate_count},
        {"host_present_events", host_present_count}, {"streamline_options", options_count},
        {"set_options_events", options_count}, {"streamline_states", state_count},
        {"dynamic_option_requests", dynamic_option_requests},
        {"dynamic_transitions", dynamic_transitions}, {"frame_count_events", frame_count_events},
        {"fixed_override_events", fixed_count_events}, {"retry_events", retry_count_events},
        {"native_fallback_events", fallback_count_events},
        {"dropped_events", g_capture.dropped.load()}, {"capacity_exhausted", g_capture.full.load()},
        {"frame_classification", "provider evaluation only; not proof of display"},
        {"pixel_marker", "unavailable: no verified Streamline pacer presentation surface"},
        {"resource_capture", "metadata only: resource state and pacer synchronization are not verified"},
        {"replay", "gated: raw resources, standalone provider initialization and synchronization are incomplete"},
        {"backend_support", {{"d3d12", "NGX Evaluate metadata/classification + metadata capture"},
                             {"vulkan", "frame-count/parameter trace only"},
                             {"d3d11", "Streamline trace only when observed"}}},
        {"core_state", core_state},
    };

    Json manifest = nullptr;
    mfgunlock::diagnostic::EvaluateEvent capture_begin{}, capture_end{};
    if (g_evaluate_capture.Snapshot(capture_begin, capture_end)) {
      std::unordered_map<uint64_t, uint64_t> capture_ids;
      manifest = {
          {"schema", 2}, {"capture", "one_shot_ngx_d3d12_evaluate_metadata"},
          {"provider", {{"module", ModuleInfo(provider_module)},
                         {"sha256", provider_sha256.empty() ? Json(nullptr) : Json(provider_sha256)}}},
          {"streamline", {{"interposer", ModuleInfo(GetModuleHandleW(L"sl.interposer.dll"))},
                           {"dlss_g", ModuleInfo(GetModuleHandleW(L"sl.dlss_g.dll"))}}},
          {"core_state", core_state},
          {"evaluate_begin", BridgeEvaluateJson(capture_begin, capture_ids)},
          {"evaluate_end", BridgeEvaluateJson(capture_end, capture_ids)},
          {"resource_contents_captured", false},
          {"resource_capture_reason", "D3D12 resource states and Streamline asynchronous pacer ownership are not verified at this observer boundary"},
          {"replay", {{"status", "gated"},
                       {"missing", Json::array({"exact resource contents", "verified standalone DLSS-G initialization context", "verified resource-state/synchronization contract", "Streamline pacer context"})}}},
      };
    }
    summary["evaluate_capture_count"] = manifest.is_null() ? 0 : 1;

    wchar_t temporary[MAX_PATH]{};
    if (GetTempPathW(MAX_PATH, temporary) == 0) throw std::runtime_error("Cannot locate TEMP");
    const auto directory = std::filesystem::path(temporary) / "MFGUnlock-Diagnostics";
    std::filesystem::create_directories(directory);
    const std::string stem = std::string(kLabels[g_recorded_label]) + "-" +
        std::to_string(GetCurrentProcessId()) + "-" + std::to_string(GetTickCount64());
    const auto legacy_path = directory / (stem + ".json");
    const auto bundle = directory / (stem + "-bundle");
    std::filesystem::create_directories(bundle);

    std::string trace_jsonl;
    for (const auto& row : trace_rows) {
      trace_jsonl += row.dump();
      trace_jsonl.push_back('\n');
    }
    const bool saved = WriteTextFile(legacy_path, root.dump()) &&
        WriteTextFile(bundle / "trace.jsonl", trace_jsonl) &&
        WriteTextFile(bundle / "summary.json", summary.dump(2)) &&
        (manifest.is_null() || WriteTextFile(bundle / "capture_manifest.json", manifest.dump(2)));
    export_succeeded = saved;
    message = saved ? "Saved: " + legacy_path.string() + "; bundle: " + bundle.string()
                    : "Capture write failed; bundle may be incomplete.";
  } catch (const std::exception& error) {
    message = std::string("Export failed: ") + error.what();
  }
  {
    std::lock_guard lock(g_output_mutex);
    g_output_message = std::move(message);
  }
  if (export_succeeded) g_capture_pending_export.store(false, std::memory_order_release);
  g_saving.store(false, std::memory_order_release);
  FreeLibraryAndExitThread(static_cast<HMODULE>(pinned_module), 0);
}

void OnDevice(reshade::api::device* device) {
  g_d3d12.store(device->get_api() == reshade::api::device_api::d3d12, std::memory_order_relaxed);
  IUnknown* native = nullptr;
  if (device->get_api() == reshade::api::device_api::d3d12 && device->get_native() != 0) {
    native = reinterpret_cast<IUnknown*>(device->get_native());
    native->AddRef();
  }
  IUnknown* previous = nullptr;
  {
    std::lock_guard lock(g_device_mutex);
    previous = g_native_d3d12_device;
    g_native_d3d12_device = native;
  }
  if (previous != nullptr) previous->Release();
  TryInstall();
}

void OnDestroyDevice(reshade::api::device* device) {
  if (device->get_api() != reshade::api::device_api::d3d12 || device->get_native() == 0) return;
  IUnknown* previous = nullptr;
  {
    std::lock_guard lock(g_device_mutex);
    if (g_native_d3d12_device != reinterpret_cast<IUnknown*>(device->get_native())) return;
    previous = g_native_d3d12_device;
    g_native_d3d12_device = nullptr;
  }
  previous->Release();
}

void OnInitResource(reshade::api::device* device, const reshade::api::resource_desc& desc,
                    const reshade::api::subresource_data*, reshade::api::resource_usage,
                    reshade::api::resource resource) {
  if (device->get_api() != reshade::api::device_api::d3d12 || resource.handle == 0 ||
      desc.type == reshade::api::resource_type::buffer ||
      desc.type == reshade::api::resource_type::unknown) return;
  TrackedResourceInfo info{};
  info.width = desc.texture.width;
  info.height = desc.texture.height;
  info.format = static_cast<uint32_t>(desc.texture.format);
  info.levels = desc.texture.levels;
  info.samples = desc.texture.samples;
  info.flags = static_cast<uint32_t>(desc.flags);
  std::unique_lock lock(g_resources_mutex);
  g_resources[resource.handle] = info;
}

void OnDestroyResource(reshade::api::device* device, reshade::api::resource resource) {
  if (device->get_api() != reshade::api::device_api::d3d12 || resource.handle == 0) return;
  std::unique_lock lock(g_resources_mutex);
  g_resources.erase(resource.handle);
}

void OnInitResourceView(reshade::api::device* device, reshade::api::resource resource,
                        reshade::api::resource_usage usage,
                        const reshade::api::resource_view_desc& desc,
                        reshade::api::resource_view view) {
  if (device->get_api() != reshade::api::device_api::d3d12 || view.handle == 0) return;
  std::unique_lock lock(g_resources_mutex);
  g_resource_views[view.handle] = {
      resource.handle,
      static_cast<uint32_t>(usage),
      static_cast<uint32_t>(desc.format),
  };
}

void OnDestroyResourceView(reshade::api::device* device, reshade::api::resource_view view) {
  if (device->get_api() != reshade::api::device_api::d3d12 || view.handle == 0) return;
  std::unique_lock lock(g_resources_mutex);
  g_resource_views.erase(view.handle);
}

void OnBindRenderTargetsAndDepthStencil(reshade::api::command_list*, uint32_t count,
                                        const reshade::api::resource_view* rtvs,
                                        reshade::api::resource_view) {
  if (rtvs == nullptr) return;
  for (uint32_t index = 0; index < count; ++index)
    RecordUiCandidate(rtvs[index], UiCandidateUse::kRenderTargetBind);
}

bool OnClearRenderTargetView(reshade::api::command_list*, reshade::api::resource_view rtv,
                             const float color[4], uint32_t,
                             const reshade::api::rect*) {
  RecordUiCandidate(rtv, UiCandidateUse::kClear, color);
  return false;
}

void OnPushDescriptors(reshade::api::command_list*, reshade::api::shader_stage,
                       reshade::api::pipeline_layout, uint32_t,
                       const reshade::api::descriptor_table_update& update) {
  if (update.count == 0 || update.descriptors == nullptr) return;
  for (uint32_t index = 0; index < update.count; ++index) {
    reshade::api::resource_view view{};
    switch (update.type) {
      case reshade::api::descriptor_type::sampler_with_resource_view:
        view = static_cast<const reshade::api::sampler_with_resource_view*>(
            update.descriptors)[index].view;
        break;
      case reshade::api::descriptor_type::texture_shader_resource_view:
        view = static_cast<const reshade::api::resource_view*>(update.descriptors)[index];
        break;
      default:
        continue;
    }
    RecordUiCandidate(view, UiCandidateUse::kShaderResourcePush);
  }
}

void OnPresent(reshade::api::command_queue*, reshade::api::swapchain* swapchain,
               const reshade::api::rect*, const reshade::api::rect*, uint32_t,
               const reshade::api::rect*) {
  const uint64_t present_qpc = Ticks();
  const uint32_t backbuffer_count = swapchain->get_back_buffer_count();
  if (backbuffer_count != 0) {
    const auto desc = swapchain->get_device()->get_resource_desc(swapchain->get_back_buffer(0));
    g_output_width.store(static_cast<uint32_t>(desc.texture.width), std::memory_order_relaxed);
    g_output_height.store(static_cast<uint32_t>(desc.texture.height),
                          std::memory_order_relaxed);
  }

  const uint64_t ticket = Ticket();
  if (ticket != 0) FinalizeUiCandidateWindow(swapchain, present_qpc);
  else g_hudless_window_open.store(false, std::memory_order_release);
  if (ticket == 0) return;

  Event event(Kind::output);
  event.recognized = true;
  event.values[0] = reinterpret_cast<uintptr_t>(swapchain);
  uint32_t color_space = 0;
  bool color_space_known = false;
#if MFGUNLOCK_RESHADE_HAS_COLOR_SPACE
  color_space = static_cast<uint32_t>(swapchain->get_color_space());
  color_space_known = color_space != 0;
  event.values[1] = color_space_known ? color_space : kUnknown;
#else
  event.values[1] = kUnknown;
#endif
  event.values[5] = backbuffer_count;
  uint32_t format = 0;
  bool format_known = false;
  if (backbuffer_count != 0) {
    const auto desc = swapchain->get_device()->get_resource_desc(swapchain->get_back_buffer(0));
    format = static_cast<uint32_t>(desc.texture.format);
    format_known = format != 0;
    event.values[2] = format_known ? format : kUnknown;
    event.values[3] = desc.texture.width;
    event.values[4] = desc.texture.height;
  }
  const auto output = mfgunlock::outputstate::Classify(
      mfgunlock::outputstate::ClassifyFormat(format, format_known),
      format, format_known, color_space, color_space_known);
  event.values[6] = output.known ? static_cast<uint32_t>(output.encoding) : kUnknown;
  event.values[7] = output.dlssg_supported ? 1u : 0u;
  event.values[8] = output.known ? 1u : 0u;
  event.values[9] = output.known ? static_cast<uint32_t>(output.hdr) : kUnknown;
  Submit(event, ticket, present_qpc, UINT32_MAX, UINT32_MAX, sl::Result::eOk);
}

void BeginCapture() {
  g_evaluate_capture.Cancel();
  g_recorded_label = g_label;
  g_truncated_tag_batches.store(0, std::memory_order_relaxed);
  ResetUiCandidateCapture();
  const uint64_t now = GetTickCount64();
  g_capture_started_ms.store(now, std::memory_order_relaxed);
  g_capture_stopped_ms.store(0, std::memory_order_relaxed);
  g_capture_auto_stopped.store(false, std::memory_order_relaxed);
  // Arm the bounded capture before publishing callbacks. Otherwise an Evaluate
  // racing observer installation could unregister itself while capture is still idle.
  g_capture.Start(now, kMaxCaptureDurationMs);
  g_capture_pending_export.store(true, std::memory_order_release);
  InstallCoreObservation();
  g_capture_had_count_bridge = g_count_bridge_connected;
  g_capture_had_evaluate_bridge = g_evaluate_bridge_connected;
  g_capture_had_state_bridge = g_state_bridge_connected;
  g_capture_had_lifecycle_bridge = g_lifecycle_bridge_connected;
}

void StopCapture() {
  g_capture.Stop();
  UninstallHotObservers();
  if (!g_evaluate_capture.Complete()) g_evaluate_capture.Cancel();
  g_hudless_window_open.store(false, std::memory_order_release);
  g_capture_auto_stopped.store(false, std::memory_order_relaxed);
  g_capture_stopped_ms.store(GetTickCount64(), std::memory_order_relaxed);
}

void LaunchExport() {
  HMODULE pinned = nullptr;
  if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                          reinterpret_cast<LPCWSTR>(&ExportWorker), &pinned)) {
    return;
  }
  g_saving.store(true, std::memory_order_release);
  HANDLE thread = CreateThread(nullptr, 0, ExportWorker, pinned, 0, nullptr);
  if (thread != nullptr) {
    CloseHandle(thread);
  } else {
    g_saving.store(false, std::memory_order_release);
    FreeLibrary(pinned);
  }
}

void OnOverlay(reshade::api::effect_runtime*) {
  static bool enabled = g_enabled;
  if (ImGui::Checkbox("Enable observation hooks (restart required)", &enabled))
    reshade::set_config_value(nullptr, kSection, "Enabled", enabled ? 1 : 0);
  if (enabled != g_enabled) ImGui::TextUnformatted("Setting saved. Restart the game to apply it.");
  ImGui::TextWrapped("Read-only diagnostic companion. It does not modify MFG, buffers, jitter, "
                    "color space, architecture or pacing. Remove it for benchmarks.");
  ImGui::Text("Hooks: %s; legacy tags: %s; frame tags: %s",
      g_installed.load() ? "installed" : "not installed",
      (g_optional_exports.load() & 4) ? "available" : "unavailable",
      (g_optional_exports.load() & 8) ? "available" : "unavailable");

  if (g_dynamic_capability_seen.load(std::memory_order_acquire)) {
    ImGui::Text("Dynamic MFG capability: %s (state v%u, %s).",
                g_dynamic_supported.load(std::memory_order_relaxed) ? "SUPPORTED" : "UNSUPPORTED",
                g_dynamic_state_version.load(std::memory_order_relaxed),
                g_dynamic_capability_from_probe.load(std::memory_order_relaxed)
                    ? "one-shot probe" : "game GetState");
  } else if (g_dynamic_probe_attempted.load(std::memory_order_acquire)) {
    ImGui::Text("Dynamic MFG probe returned 0x%x without a capability result.",
                g_dynamic_probe_result.load(std::memory_order_relaxed));
  } else {
    ImGui::TextDisabled("Dynamic MFG capability: not observed.");
  }
  const bool can_probe = g_d3d12.load(std::memory_order_relaxed) &&
                         g_get_state.load(std::memory_order_acquire) != nullptr;
  ImGui::BeginDisabled(!can_probe || g_dynamic_probe_armed.load(std::memory_order_acquire));
  if (ImGui::Button("Arm one-shot Dynamic MFG capability probe")) {
    g_dynamic_probe_attempted.store(false, std::memory_order_release);
    g_dynamic_capability_seen.store(false, std::memory_order_release);
    g_dynamic_probe_result.store(UINT32_MAX, std::memory_order_relaxed);
    g_dynamic_probe_armed.store(true, std::memory_order_release);
  }
  ImGui::EndDisabled();
  if (g_dynamic_probe_armed.load(std::memory_order_acquire))
    ImGui::TextDisabled("Armed: waiting for the game's next slDLSSGGetState call.");

  const bool saving = g_saving.load(std::memory_order_acquire);
  const bool active = Ticket() != 0;
  const uint64_t now_ms = GetTickCount64();
  const uint64_t started_ms = g_capture_started_ms.load(std::memory_order_relaxed);
  if (!active && started_ms != 0 &&
      g_capture_stopped_ms.load(std::memory_order_relaxed) == 0) {
    g_capture_stopped_ms.store(now_ms, std::memory_order_relaxed);
    g_capture_auto_stopped.store(true, std::memory_order_relaxed);
    UninstallHotObservers();
    if (!g_evaluate_capture.Complete()) g_evaluate_capture.Cancel();
  }
  const bool pending_export = g_capture_pending_export.load(std::memory_order_acquire);
  ImGui::Combo("Capture label (manual)", &g_label, kLabels, static_cast<int>(std::size(kLabels)));
  ImGui::BeginDisabled(saving || active || pending_export || !g_enabled);
  if (ImGui::Button("Start capture")) BeginCapture();
  ImGui::EndDisabled();
  ImGui::SameLine();
  ImGui::BeginDisabled(saving || !pending_export);
  if (ImGui::Button(active ? "Stop & export" : "Export capture")) {
    StopCapture();
    LaunchExport();
  }
  ImGui::EndDisabled();

  const uint64_t stopped_ms = g_capture_stopped_ms.load(std::memory_order_relaxed);
  const uint64_t end_ms = active || stopped_ms == 0 ? now_ms : stopped_ms;
  const double elapsed_s = started_ms == 0 || end_ms < started_ms
      ? 0.0 : static_cast<double>(end_ms - started_ms) / 1000.0;
  const char* capture_state = active ? "recording"
      : g_capture_auto_stopped.load(std::memory_order_relaxed) ? "auto-stopped" : "stopped";
  ImGui::Text("MfgProbe: count=%s, Evaluate=%s, state=%s; NGX C hooks=%u%s",
              (active ? g_count_bridge_connected : g_capture_had_count_bridge) ? "connected" : "unavailable",
              (active ? g_evaluate_bridge_connected : g_capture_had_evaluate_bridge) ? "connected" : "unavailable",
              g_state_bridge_connected ? "connected" : "unavailable", probe::g_hook_count,
              probe::g_capacity_exhausted ? " (capacity reached)" : "");
  const auto last_kind = static_cast<timeline::FrameKind>(g_last_frame_kind.load(std::memory_order_relaxed));
  const uint32_t last_index = g_last_generated_index.load(std::memory_order_relaxed);
  const uint32_t last_count = g_last_generated_count.load(std::memory_order_relaxed);
  if (last_kind == timeline::FrameKind::kGenerated)
    ImGui::Text("Last provider Evaluate: %llu, FG %u/%u (display not proven).",
                static_cast<unsigned long long>(g_last_evaluation_id.load(std::memory_order_relaxed)),
                last_index, last_count);
  else
    ImGui::Text("Last provider Evaluate: %llu, classification=%s.",
                static_cast<unsigned long long>(g_last_evaluation_id.load(std::memory_order_relaxed)),
                timeline::FrameKindName(last_kind));
  ImGui::TextDisabled("Pixel generated-frame marker: unavailable; Streamline pacer presentation surface is not verified.");
  ImGui::BeginDisabled(!active || !g_evaluate_bridge_connected || g_evaluate_capture.Armed());
  if (ImGui::Button("Capture next provider Evaluate")) g_evaluate_capture.Arm();
  ImGui::EndDisabled();
  if (g_evaluate_capture.Armed()) ImGui::TextDisabled("Evaluate capture armed: waiting for the next tracked DLSS-G Evaluate.");
  else if (g_evaluate_capture.Complete()) ImGui::TextDisabled("Evaluate metadata capture complete; Stop & export to write the bundle.");
  ImGui::Text("Capture: %s, %.1f s; dropped: %u; buffer full: %s",
              capture_state, elapsed_s, g_capture.dropped.load(),
              g_capture.full.load() ? "yes" : "no");
  ImGui::Text("UI observer: HUD-less tags=%u, UI Color+Alpha=%u, UI Alpha=%u, candidates=%zu%s",
              g_hudless_tag_count.load(std::memory_order_relaxed),
              g_ui_color_alpha_tag_count.load(std::memory_order_relaxed),
              g_ui_alpha_tag_count.load(std::memory_order_relaxed), UiCandidateCount(),
              g_ui_candidate_overflow.load(std::memory_order_relaxed) ? " (overflow)" : "");
  ImGui::TextDisabled(
      "Manual stop; automatic stop occurs at 120 s or when the bounded event buffer fills.");

  ImGui::BeginDisabled(saving || active || started_ms == 0);
  if (ImGui::Button("Export stopped capture")) LaunchExport();
  ImGui::EndDisabled();
  std::unique_lock lock(g_output_mutex, std::try_to_lock);
  if (lock.owns_lock()) ImGui::TextWrapped("%s", g_output_message.c_str());
  ImGui::TextWrapped("Close the overlay during capture. For UI discovery, switch native FG off/on and "
                    "leave the game running for a few seconds after HUD-less appears. Observer callbacks "
                    "disconnect symmetrically when capture stops or the addon unloads.");
}
} // namespace

extern "C" __declspec(dllexport) constexpr const char* NAME = "MFG Diagnostics";
extern "C" __declspec(dllexport) constexpr const char* DESCRIPTION =
    "Read-only Streamline/DLSS-G capture; no runtime, kernel or pacing overrides";

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID reserved) {
  if (reason == DLL_PROCESS_DETACH && reserved != nullptr) return TRUE;
  if (reason == DLL_PROCESS_ATTACH) {
    if (!reshade::register_addon(module)) return FALSE;
    int enabled = 0;
    reshade::get_config_value(nullptr, kSection, "Enabled", enabled);
    g_enabled = enabled != 0;
    LARGE_INTEGER frequency{};
    QueryPerformanceFrequency(&frequency);
    g_frequency = static_cast<uint64_t>(frequency.QuadPart);
    if (g_enabled) {
      g_loader_hooked = mfgunlock::hook::Install(GetModuleHandleW(L"kernel32.dll"), kLoaderHooks,
                                                "diagnostic loader observation");
      TryInstall();
      reshade::register_event<reshade::addon_event::init_device>(OnDevice);
      reshade::register_event<reshade::addon_event::destroy_device>(OnDestroyDevice);
      reshade::register_event<reshade::addon_event::init_resource>(OnInitResource);
      reshade::register_event<reshade::addon_event::destroy_resource>(OnDestroyResource);
      reshade::register_event<reshade::addon_event::init_resource_view>(OnInitResourceView);
      reshade::register_event<reshade::addon_event::destroy_resource_view>(OnDestroyResourceView);
      reshade::register_event<reshade::addon_event::bind_render_targets_and_depth_stencil>(
          OnBindRenderTargetsAndDepthStencil);
      reshade::register_event<reshade::addon_event::clear_render_target_view>(OnClearRenderTargetView);
      reshade::register_event<reshade::addon_event::push_descriptors>(OnPushDescriptors);
      reshade::register_event<reshade::addon_event::present>(OnPresent);
    }
    reshade::register_overlay("MFG Diagnostics", OnOverlay);
  } else if (reason == DLL_PROCESS_DETACH) {
    g_capture.Stop();
    UninstallHotObservers();
    reshade::unregister_overlay("MFG Diagnostics", OnOverlay);
    if (g_enabled) {
      reshade::unregister_event<reshade::addon_event::present>(OnPresent);
      reshade::unregister_event<reshade::addon_event::push_descriptors>(OnPushDescriptors);
      reshade::unregister_event<reshade::addon_event::clear_render_target_view>(OnClearRenderTargetView);
      reshade::unregister_event<reshade::addon_event::bind_render_targets_and_depth_stencil>(
          OnBindRenderTargetsAndDepthStencil);
      reshade::unregister_event<reshade::addon_event::destroy_resource_view>(OnDestroyResourceView);
      reshade::unregister_event<reshade::addon_event::init_resource_view>(OnInitResourceView);
      reshade::unregister_event<reshade::addon_event::destroy_resource>(OnDestroyResource);
      reshade::unregister_event<reshade::addon_event::init_resource>(OnInitResource);
      reshade::unregister_event<reshade::addon_event::destroy_device>(OnDestroyDevice);
      reshade::unregister_event<reshade::addon_event::init_device>(OnDevice);
      IUnknown* device = nullptr;
      {
        std::lock_guard lock(g_device_mutex);
        device = g_native_d3d12_device;
        g_native_d3d12_device = nullptr;
      }
      if (device != nullptr) device->Release();
    }
    if (g_loader_hooked) mfgunlock::hook::Uninstall(kLoaderHooks);
    if (g_installed.load()) mfgunlock::hook::Uninstall(g_hooks);
    reshade::unregister_addon(module);
  }
  return TRUE;
}
