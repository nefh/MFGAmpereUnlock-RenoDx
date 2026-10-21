// SPDX-License-Identifier: MIT
#include "framecount.hpp"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <vector>

using namespace mfgunlock;
namespace fc = mfgunlock::framecount;

namespace {
unsigned int g_checks = 0;
unsigned int g_received = 0;
unsigned int g_calls = 0;
unsigned int g_pacing_calls = 0;
unsigned int g_state_calls = 0;
unsigned int g_native_max = 5;
unsigned int g_presented_per_query = 0;
bool g_probe_extension_seen = false;
bool g_backend_ready = false;
bool g_software_pacing = false;
bool g_dynamic_supported = false;
sl::DLSSGMode g_received_mode = sl::DLSSGMode::eOn;
uint32_t g_received_version = 0;
float g_received_dynamic_target = 0.0f;
sl::Result g_result = sl::Result::eOk;
std::vector<sl::Result> g_set_results;
std::vector<unsigned int> g_received_counts;
std::vector<sl::DLSSGMode> g_received_modes;
std::vector<uint32_t> g_state_versions;
std::vector<const void*> g_state_chains;
std::vector<const void*> g_state_options_chains;
std::vector<sl::Result> g_get_results;
bool g_write_dynamic_flag = true;
bool g_probe_options_extension_seen = false;
bool g_invalidate_probe_context = false;
uint32_t g_seen_flags = 0;
std::array<countobserver::Event, 32> g_events;
size_t g_event_count = 0;
const sl::DLSSGOptions* g_caller_options = nullptr;
bool g_caller_untouched = true;

struct LegacyConstantsForHistory {
  uint32_t structVersion = 1;
  sl::Boolean reset = sl::Boolean::eFalse;
};

// Synthetic optional future layout, not a field claimed to exist in the SDK.
struct ModernConstantsForHistory {
  uint32_t structVersion = 2;
  sl::Boolean reset = sl::Boolean::eFalse;
  sl::Boolean renderingGameFrames = sl::Boolean::eFalse;
};
void ObserveCount(const countobserver::Event* event) noexcept {
  if (g_event_count < g_events.size()) g_events[g_event_count++] = *event;
}

void Check(bool value, const char* name) {
  ++g_checks;
  if (value) return;
  std::fprintf(stderr, "FAIL %s\n", name);
  std::exit(1);
}
sl::Result SetOptions(const sl::ViewportHandle&, const sl::DLSSGOptions& options) {
  ++g_calls;
  if (g_caller_options) g_caller_untouched &= g_caller_options->numFramesToGenerate == 1;
  g_received = options.numFramesToGenerate;
  g_received_counts.push_back(g_received);
  g_received_modes.push_back(options.mode);
  g_received_mode = options.mode;
  g_received_version = options.structVersion;
  g_received_dynamic_target = options.dynamicTargetFrameRate;
  if (g_calls <= g_set_results.size()) return g_set_results[g_calls - 1];
  return g_result;
}
sl::Result GetState(const sl::ViewportHandle&, sl::DLSSGState& state, const sl::DLSSGOptions* options) {
  ++g_state_calls;
  g_state_versions.push_back(state.structVersion);
  g_state_chains.push_back(state.next);
  g_state_options_chains.push_back(options ? options->next : nullptr);
  if (g_state_calls > 1 && options && options->next) g_probe_options_extension_seen = true;
  state.numFramesActuallyPresented = g_presented_per_query;
  if (g_state_calls > 1 && state.next != nullptr) g_probe_extension_seen = true;
  if (state.structVersion >= sl::kStructVersion2) state.numFramesToGenerateMax = g_native_max;
  if (state.structVersion >= sl::kStructVersion4 && g_write_dynamic_flag) {
    state.bIsDynamicMFGSupported = g_dynamic_supported
        ? sl::Boolean::eTrue : sl::Boolean::eFalse;
  }
  if (g_invalidate_probe_context) {
    g_invalidate_probe_context = false;
    fc::internal::InvalidateLiveOptions();
  }
  if (g_state_calls <= g_get_results.size()) return g_get_results[g_state_calls - 1];
  return g_result;
}
sl::Result Init(const sl::Preferences& pref, uint64_t) {
  g_seen_flags = static_cast<uint32_t>(pref.flags);
  return sl::Result::eOk;
}
sl::Result ObserveInit(const sl::Preferences& pref, uint64_t sdk,
                       sl::Result (*next)(const sl::Preferences&, uint64_t)) {
  return next(pref, sdk);
}
sl::Result SetConstantsForEvidence(const sl::Constants&, const sl::FrameToken&,
                                     const sl::ViewportHandle&) { return g_result; }
sl::Result SetTagsForEvidence(const sl::FrameToken&, const sl::ViewportHandle&,
                              const sl::ResourceTag*, uint32_t, sl::CommandBuffer*) { return g_result; }
sl::Result SetMarkerForEvidence(sl::PCLMarker, const sl::FrameToken&) { return g_result; }

void Reset(Architecture mode) {
  fc::internal::InvalidateLiveOptions();
  fc::g_request_retry_revision.fetch_add(1);
  fc::internal::g_entry_shutting_down = false;
  outputstate::Reset();
  reshade::log::lines.clear();
  diagnostic::g_verbose = false;
  countobserver::g_callback = nullptr;
  g_event_count = 0;
  presentation::Reset();
  g_caller_options = nullptr;
  g_caller_untouched = true;
  fc::g_ui_composition_enabled = false;
  architecture::Configure(mode);
  g_enabled = true;
  fc::g_force_multiplier = 0;
  fc::g_runtime_selection_mode = static_cast<unsigned int>(fc::RuntimeSelectionMode::kGameDefault);
  fc::g_streamline_plugin_seen = true;
  fc::g_streamline_max_generated = 5;
  fc::g_fixed_override_status = fc::FixedOverrideStatus::kNative;
  fc::g_last_requested = 0;
  fc::g_last_effective_generated = 0;
  fc::g_declined_no_pacing = false;
  fc::g_force_failed_for = 0;
  fc::g_ceiling_blocked_for = 0;
  fc::g_unknown_ceiling_logged = false;
  fc::g_unknown_fixed_abi_logged = false;
  fc::g_last_requested_mode = mfgunlock::diagnostic::kUnknown32;
  fc::g_last_requested_generated = mfgunlock::diagnostic::kUnknown32;
  fc::g_last_forwarded_mode = mfgunlock::diagnostic::kUnknown32;
  fc::g_last_forwarded_generated = mfgunlock::diagnostic::kUnknown32;
  fc::g_last_accepted_mode = mfgunlock::diagnostic::kUnknown32;
  fc::g_last_accepted_generated = mfgunlock::diagnostic::kUnknown32;
  fc::g_effective_max_seen = false;
  fc::g_effective_max_generated = 0;
  fc::g_state_seen = false;
  fc::g_runtime_max_generated = 0;
  fc::internal::g_real_set_options = SetOptions;
  fc::internal::g_real_get_state = GetState;
  fc::g_provider_mfg_ceiling = nullptr;
  fc::g_multi_frame_ready = [] { return g_backend_ready; };
  fc::g_dynamic_stack_ready = [] { return true; };
  fc::g_dynamic_d3d12 = false;
  fc::g_dynamic_mfg_enabled = false;
  fc::g_dynamic_target_fps = 0;
  fc::g_dynamic_support_seen = false;
  fc::g_dynamic_supported = false;
  fc::g_dynamic_probe_attempted = false;
  fc::g_dynamic_applied = false;
  fc::g_dynamic_fell_back = false;
  fc::g_dynamic_runtime_declined = false;
  fc::g_dynamic_result = 0;
  fc::g_dynamic_probe_result = 0;
  fc::g_dynamic_probe_calls = 0;
  fc::g_dynamic_request_calls = 0;
  fc::g_dynamic_suppressed_repeats = 0;
  fc::g_fixed_suppressed_repeats = 0;
  fc::g_ensure_pacing = [] { ++g_pacing_calls; };
  fc::g_pacing_ready = [] { return g_software_pacing; };
  g_calls = 0;
  g_received_counts.clear();
  g_received_modes.clear();
  g_state_versions.clear();
  g_state_chains.clear();
  g_state_options_chains.clear();
  g_get_results.clear();
  g_write_dynamic_flag = true;
  g_probe_options_extension_seen = false;
  g_invalidate_probe_context = false;
  g_set_results.clear();
  g_state_calls = 0;
  g_presented_per_query = 0;
  g_probe_extension_seen = false;
  g_pacing_calls = 0;
  g_backend_ready = false;
  g_software_pacing = false;
  g_dynamic_supported = false;
  g_received_mode = sl::DLSSGMode::eOn;
  g_received_version = 0;
  g_received_dynamic_target = 0.0f;
  g_native_max = 5;
  g_result = sl::Result::eOk;
}
}  // namespace

int main() {
  integration::Reset();
  LegacyConstantsForHistory legacy_constants{};
  legacy_constants.reset = sl::Boolean::eTrue;
  fc::internal::ObserveStreamlineHistory(legacy_constants);
  auto history = integration::Read();
  Check(history.history_reset_known && history.history_reset,
        "legacy Constants preserves reset evidence");
  Check(!history.rendering_game_frames_known,
        "legacy Constants keeps renderingGameFrames unknown");

  integration::Reset();
  ModernConstantsForHistory modern_constants{};
  modern_constants.renderingGameFrames = sl::Boolean::eTrue;
  fc::internal::ObserveStreamlineHistory(modern_constants);
  history = integration::Read();
  Check(history.history_reset_known && !history.history_reset &&
            history.rendering_game_frames_known && history.rendering_game_frames,
        "modern Constants observes renderingGameFrames");

  modern_constants.structVersion = 1;
  integration::Reset();
  fc::internal::ObserveStreamlineHistory(modern_constants);
  history = integration::Read();
  Check(!history.rendering_game_frames_known,
        "older Constants ABI does not read newer renderingGameFrames member");

  sl::ViewportHandle viewport{};
  sl::DLSSGOptions options{};
  options.numFramesToGenerate = 3;
  sl::DLSSGState state{};

  for (uint32_t version = 1; version <= 5; ++version) {
    sl::DLSSGOptions input{};
    input.structVersion = version;
    input.next = &options;
    input.mode = sl::DLSSGMode::eAuto;
    input.numFramesToGenerate = 4;
    input.flags = static_cast<decltype(input.flags)>(0x1f);
    input.dynamicResWidth = 1280;
    input.dynamicResHeight = 720;
    input.numBackBuffers = 3;
    input.mvecDepthWidth = 640;
    input.mvecDepthHeight = 360;
    input.colorWidth = 2560;
    input.colorHeight = 1440;
    input.colorBufferFormat = 24;
    input.mvecBufferFormat = 34;
    input.depthBufferFormat = 40;
    input.hudLessBufferFormat = 10;
    input.uiBufferFormat = 28;
    input.onErrorCallback = reinterpret_cast<decltype(input.onErrorCallback)>(uintptr_t{0x1234});
    input.bReserved15 = static_cast<decltype(input.bReserved15)>(1);
    input.queueParallelismMode = static_cast<decltype(input.queueParallelismMode)>(1);
    input.enableUserInterfaceRecomposition = sl::Boolean::eTrue;
    input.dynamicTargetFrameRate = 144.0f;
    sl::DLSSGOptions copy{};
    Check(fc::internal::CopyOptions(input, copy) && copy.structVersion == version &&
              copy.next == input.next && copy.mode == input.mode && copy.numFramesToGenerate == 4 &&
              static_cast<uint32_t>(copy.flags) == 0x1f &&
              copy.dynamicResWidth == 1280 && copy.dynamicResHeight == 720 && copy.numBackBuffers == 3 &&
              copy.mvecDepthWidth == 640 && copy.mvecDepthHeight == 360 &&
              copy.colorWidth == 2560 && copy.colorHeight == 1440 &&
              copy.colorBufferFormat == 24 && copy.mvecBufferFormat == 34 &&
              copy.depthBufferFormat == 40 && copy.hudLessBufferFormat == 10 && copy.uiBufferFormat == 28 &&
              copy.onErrorCallback == input.onErrorCallback &&
              (version < 2 || copy.bReserved15 == input.bReserved15) &&
              (version < 3 || copy.queueParallelismMode == input.queueParallelismMode) &&
              (version < 4 || copy.enableUserInterfaceRecomposition == sl::Boolean::eTrue) &&
              (version < 5 || copy.dynamicTargetFrameRate == 144.0f),
          "all public Options v1-v5 fields/flags preserved within known ABI");
  }


  for (auto mode : {Architecture::kAmpere, Architecture::kTuring}) {
    Reset(mode);
    Check(fc::internal::HookedSetOptions(viewport, options) == sl::Result::eOk, "backport forwards result");
    Check(g_calls == 1 && g_received == 3 && options.numFramesToGenerate == 3,
          "incomplete pacing preparation still submits the native count without changing caller state");
    fc::internal::HookedGetState(viewport, state, nullptr);
    Check(state.numFramesToGenerateMax == 5, "native maximum is not lowered by unrelated addon preparation");
    g_backend_ready = true;
    g_software_pacing = true;
    fc::internal::HookedSetOptions(viewport, options);
    Check(g_received == 3 && options.numFramesToGenerate == 3, "ready backport forwards native 4x");
    fc::internal::HookedGetState(viewport, state, nullptr);
    Check(state.numFramesToGenerateMax == 5, "ready backport preserves native capacity");

    fc::g_force_multiplier = 4;
    options.numFramesToGenerate = 1;
    fc::internal::HookedSetOptions(viewport, options);
    Check(g_received == 3 && options.numFramesToGenerate == 1, "fixed multiplier can raise request");
    fc::g_force_multiplier = 2;
    options.numFramesToGenerate = 3;
    fc::internal::HookedSetOptions(viewport, options);
    Check(g_received == 1 && options.numFramesToGenerate == 3, "fixed multiplier can lower request");
    options.numFramesToGenerate = 3;
    g_enabled = false;
    g_backend_ready = false;
    fc::internal::HookedSetOptions(viewport, options);
    fc::internal::HookedGetState(viewport, state, nullptr);
    Check(g_received == 3 && state.numFramesToGenerateMax == 5, "disabled does not clamp game requests");
  }

  Reset(Architecture::kAmpere);
  g_backend_ready = true;
  g_software_pacing = true;
  fc::g_streamline_max_generated = 3;
  state.structVersion = sl::kStructVersion2;
  g_native_max = 5;
  fc::internal::HookedGetState(viewport, state, nullptr);
  Check(state.numFramesToGenerateMax == 5,
        "larger native maximum takes precedence over scanner estimate");
  g_native_max = 1;
  fc::internal::HookedGetState(viewport, state, nullptr);
  Check(state.numFramesToGenerateMax == 5,
        "configured menu trial is independent of a smaller scanner estimate");

  Reset(Architecture::kAmpere);
  g_backend_ready = true;
  g_software_pacing = true;
  fc::g_streamline_max_generated = 3;
  fc::g_force_multiplier = 6;
  options.structVersion = sl::kStructVersion1;
  options.mode = sl::DLSSGMode::eOn;
  options.numFramesToGenerate = 1;
  fc::internal::HookedSetOptions(viewport, options);
  Check(g_calls == 1 && g_received_counts == std::vector<unsigned int>{5} &&
            fc::g_fixed_override_status.load() == fc::FixedOverrideStatus::kApplied &&
            fc::g_last_effective_generated.load() == 5,
        "6x explicit request is submitted to native setter despite lower scanner estimate");

  Reset(Architecture::kAmpere);
  g_backend_ready = true;
  g_software_pacing = true;
  fc::g_streamline_max_generated = 5;
  fc::g_force_multiplier = 6;
  options.numFramesToGenerate = 1;
  fc::internal::HookedSetOptions(viewport, options);
  Check(g_calls == 1 && g_received_counts == std::vector<unsigned int>{5} &&
            fc::g_fixed_override_status.load() == fc::FixedOverrideStatus::kApplied &&
            fc::g_last_effective_generated.load() == 5,
        "6x override is allowed at a 6x Streamline structural ceiling");

  for (unsigned int multiplier = 2; multiplier <= 6; ++multiplier) {
    Reset(Architecture::kAmpere);
    g_backend_ready = true;
    g_software_pacing = true;
    fc::g_streamline_max_generated = 5;
    fc::g_force_multiplier = multiplier;
    options = sl::DLSSGOptions{};
    options.mode = sl::DLSSGMode::eOn;
    options.numFramesToGenerate = 1;
    fc::internal::HookedSetOptions(viewport, options);
    Check(g_calls == 1 && g_received == multiplier - 1 &&
              fc::g_last_accepted_generated.load() == multiplier - 1,
          "fixed multiplier maps total frames to generated frames 2x through 6x");
  }

  Reset(Architecture::kAmpere);
  g_backend_ready = true;
  g_software_pacing = true;
  fc::g_streamline_max_generated = 0;
  fc::g_force_multiplier = 6;
  options.numFramesToGenerate = 1;
  fc::internal::HookedSetOptions(viewport, options);
  Check(g_calls == 1 && g_received_counts == std::vector<unsigned int>{5} &&
            fc::g_fixed_override_status.load() == fc::FixedOverrideStatus::kApplied,
        "unknown scanner ceiling does not suppress a typed native request");

  Reset(Architecture::kAmpere);
  g_backend_ready = true;
  g_software_pacing = true;
  fc::g_force_multiplier = 4;
  options.numFramesToGenerate = 1;
  g_set_results = {sl::Result::eErrorFeatureNotSupported, sl::Result::eOk};
  fc::internal::HookedSetOptions(viewport, options);
  Check(g_received_counts == std::vector<unsigned int>({3, 3}) &&
            fc::g_fixed_override_status.load() == fc::FixedOverrideStatus::kAppliedAfterRetry &&
            fc::g_last_effective_generated.load() == 3,
        "retry acceptance is reported as the effective fixed count");

  g_calls = 0;
  g_received_counts.clear();
  g_set_results = {sl::Result::eErrorFeatureNotSupported,
                   sl::Result::eErrorFeatureNotSupported,
                   sl::Result::eOk};
  fc::internal::HookedSetOptions(viewport, options);
  Check(g_received_counts == std::vector<unsigned int>({3, 3, 1}) &&
            fc::g_fixed_override_status.load() == fc::FixedOverrideStatus::kRejectedNativeFallback &&
            fc::g_last_effective_generated.load() == 1,
        "rejected override replaces stale success with native fallback state");

  Reset(Architecture::kAmpere);
  g_backend_ready = true;
  g_software_pacing = true;
  fc::g_force_multiplier = 4;
  options.numFramesToGenerate = 3;
  fc::internal::HookedSetOptions(viewport, options);
  Check(g_calls == 1 && g_received == 3 &&
            fc::g_fixed_override_status.load() == fc::FixedOverrideStatus::kAlreadyMatched &&
            fc::g_last_effective_generated.load() == 3,
        "matching native request is reported without an override");

  Reset(Architecture::kAmpere);
  g_backend_ready = true;
  fc::g_force_multiplier = 4;
  options.numFramesToGenerate = 1;
  fc::internal::HookedSetOptions(viewport, options);
  Check(g_calls == 1 && g_received == 3 &&
            fc::g_fixed_override_status.load() == fc::FixedOverrideStatus::kApplied &&
            fc::g_last_effective_generated.load() == 3 && g_pacing_calls == 1,
        "best-effort pacing preparation does not preempt the native fixed request");

  Reset(Architecture::kAmpere);
  g_backend_ready = true;
  g_software_pacing = true;
  fc::g_force_multiplier = 4;
  options.structVersion = sl::kStructVersion5 + 1;
  options.numFramesToGenerate = 1;
  fc::internal::HookedSetOptions(viewport, options);
  Check(g_calls == 1 && g_received == 1 &&
            fc::g_fixed_override_status.load() == fc::FixedOverrideStatus::kUnknownOptionsAbi,
        "unknown options ABI preserves the native request");
  options.structVersion = sl::kStructVersion1;

  Reset(Architecture::kAda);
  fc::g_multi_frame_ready = [] { return true; };
  options.numFramesToGenerate = 3;
  fc::internal::HookedSetOptions(viewport, options);
  Check(g_received == 3 && g_pacing_calls == 0, "Ada native request has no backport preflight");
  fc::internal::HookedGetState(viewport, state, nullptr);
  Check(state.numFramesToGenerateMax == 5, "Ada native capacity preserved");

  Reset(Architecture::kAmpere);
  g_backend_ready = true;
  fc::g_dynamic_d3d12 = true;
  g_dynamic_supported = true;
  state.structVersion = sl::kStructVersion4;
  fc::internal::HookedGetState(viewport, state, nullptr);
  Check(fc::g_dynamic_support_seen.load() && fc::g_dynamic_supported.load(),
        "v4 state enables Dynamic capability only after observation");
  fc::g_dynamic_mfg_enabled = true;
  fc::g_dynamic_target_fps = 240;
  fc::g_force_multiplier = 6;
  options.structVersion = sl::kStructVersion1;
  options.mode = sl::DLSSGMode::eOn;
  options.numFramesToGenerate = 1;
  fc::internal::HookedSetOptions(viewport, options);
  Check(g_received_mode == sl::DLSSGMode::eDynamic &&
            g_received_version == sl::kStructVersion5 &&
            g_received_dynamic_target == 240.0f &&
            options.mode == sl::DLSSGMode::eOn &&
            options.structVersion == sl::kStructVersion1,
        "Dynamic builds addon-owned v5 options and leaves caller ABI untouched");
  Check(fc::g_dynamic_applied.load() && g_pacing_calls == 0,
        "Dynamic has priority over fixed forcing and keeps NVIDIA pacing");

  Reset(Architecture::kTuring);
  g_backend_ready = true;
  fc::g_dynamic_d3d12 = true;
  g_dynamic_supported = true;
  state.structVersion = sl::kStructVersion4;
  fc::internal::HookedGetState(viewport, state, nullptr);
  Check(fc::g_dynamic_support_seen.load() && fc::g_dynamic_supported.load(),
        "Turing observes Dynamic capability on the validated stack");
  fc::g_dynamic_mfg_enabled = true;
  options.structVersion = sl::kStructVersion1;
  options.mode = sl::DLSSGMode::eOn;
  options.numFramesToGenerate = 1;
  fc::internal::HookedSetOptions(viewport, options);
  Check(g_received_mode == sl::DLSSGMode::eDynamic && fc::g_dynamic_applied.load() &&
            g_pacing_calls == 0,
        "Turing Dynamic uses midpoint readiness and keeps NVIDIA pacing");

  Reset(Architecture::kTuring);
  g_backend_ready = true;
  fc::g_dynamic_d3d12 = true;
  g_dynamic_supported = true;
  state.structVersion = sl::kStructVersion4;
  fc::internal::HookedGetState(viewport, state, nullptr);
  fc::g_dynamic_mfg_enabled = true;
  fc::g_dynamic_target_fps = 0;
  options.mode = sl::DLSSGMode::eOn;
  options.numFramesToGenerate = 1;
  fc::internal::HookedSetOptions(viewport, options);
  Check(g_received_mode == sl::DLSSGMode::eDynamic &&
            g_received_dynamic_target == 0.0f && fc::g_dynamic_applied.load(),
        "Dynamic target zero is forwarded as active-display refresh semantics");

  Reset(Architecture::kTuring);
  g_backend_ready = true;
  fc::g_dynamic_d3d12 = true;
  g_dynamic_supported = false;
  state.structVersion = sl::kStructVersion4;
  fc::internal::HookedGetState(viewport, state, nullptr);
  fc::g_dynamic_mfg_enabled = true;
  options.mode = sl::DLSSGMode::eOn;
  options.numFramesToGenerate = 1;
  fc::internal::HookedSetOptions(viewport, options);
  Check(fc::g_dynamic_support_seen.load() && !fc::g_dynamic_supported.load() &&
            g_received_mode == sl::DLSSGMode::eDynamic && fc::g_dynamic_applied.load(),
        "Turing submits Dynamic even when capability reports false and records native acceptance");

  Reset(Architecture::kTuring);
  fc::g_dynamic_d3d12 = true;
  fc::g_dynamic_support_seen = true;
  fc::g_dynamic_supported = true;
  fc::g_dynamic_mfg_enabled = true;
  options.mode = sl::DLSSGMode::eOn;
  options.numFramesToGenerate = 1;
  fc::internal::HookedSetOptions(viewport, options);
  Check(g_received_mode == sl::DLSSGMode::eDynamic && fc::g_dynamic_applied.load(),
        "Turing native Dynamic attempt does not wait for unrelated backend readiness");

  Reset(Architecture::kTuring);
  g_backend_ready = true;
  fc::g_dynamic_d3d12 = true;
  fc::g_dynamic_support_seen = true;
  fc::g_dynamic_supported = true;
  fc::g_dynamic_mfg_enabled = true;
  fc::g_dynamic_stack_ready = [] { return false; };
  options.mode = sl::DLSSGMode::eOn;
  options.numFramesToGenerate = 1;
  fc::internal::HookedSetOptions(viewport, options);
  Check(g_received_mode == sl::DLSSGMode::eDynamic && fc::g_dynamic_applied.load(),
        "Turing native Dynamic attempt ignores a predictive stack veto");

  Reset(Architecture::kTuring);
  g_backend_ready = true;
  fc::g_dynamic_d3d12 = true;
  fc::g_dynamic_support_seen = true;
  fc::g_dynamic_supported = true;
  fc::g_dynamic_mfg_enabled = true;
  fc::g_streamline_max_generated = 0;
  options.mode = sl::DLSSGMode::eOn;
  options.numFramesToGenerate = 1;
  fc::internal::HookedSetOptions(viewport, options);
  Check(g_received_mode == sl::DLSSGMode::eDynamic && fc::g_dynamic_applied.load(),
        "Turing native Dynamic attempt does not wait for a structural ceiling");

  Reset(Architecture::kTuring);
  g_backend_ready = true;
  fc::g_dynamic_d3d12 = true;
  g_dynamic_supported = true;
  fc::g_streamline_max_generated = 3;
  state.structVersion = sl::kStructVersion4;
  g_native_max = 5;
  fc::internal::HookedGetState(viewport, state, nullptr);
  Check(state.numFramesToGenerateMax == 5,
        "Turing Dynamic preserves a larger native maximum");
  fc::g_dynamic_mfg_enabled = true;
  options.mode = sl::DLSSGMode::eOn;
  options.numFramesToGenerate = 1;
  fc::internal::HookedSetOptions(viewport, options);
  Check(g_received_mode == sl::DLSSGMode::eDynamic && fc::g_dynamic_applied.load(),
        "Turing Dynamic can run within the plugin structural ceiling");

  Reset(Architecture::kAda);
  g_backend_ready = true;
  fc::g_dynamic_d3d12 = true;
  g_dynamic_supported = true;
  state.structVersion = sl::kStructVersion4;
  fc::internal::HookedGetState(viewport, state, nullptr);
  Check(fc::g_dynamic_support_seen.load() && fc::g_dynamic_supported.load(),
        "Ada observes Dynamic capability on the validated native stack");
  fc::g_dynamic_mfg_enabled = true;
  options.mode = sl::DLSSGMode::eOn;
  options.numFramesToGenerate = 1;
  fc::internal::HookedSetOptions(viewport, options);
  Check(g_received_mode == sl::DLSSGMode::eDynamic && fc::g_dynamic_applied.load() &&
            g_pacing_calls == 0,
        "Ada Dynamic uses NativeSm89 readiness and keeps NVIDIA pacing");

  Reset(Architecture::kAda);
  g_backend_ready = true;
  fc::g_dynamic_d3d12 = true;
  g_dynamic_supported = false;
  state.structVersion = sl::kStructVersion4;
  fc::internal::HookedGetState(viewport, state, nullptr);
  fc::g_dynamic_mfg_enabled = true;
  options.mode = sl::DLSSGMode::eOn;
  options.numFramesToGenerate = 1;
  fc::internal::HookedSetOptions(viewport, options);
  Check(fc::g_dynamic_support_seen.load() && !fc::g_dynamic_supported.load() &&
            g_received_mode == sl::DLSSGMode::eDynamic && fc::g_dynamic_applied.load(),
        "Ada false capability remains diagnostic while native Dynamic acceptance is observed");

  Reset(Architecture::kAda);
  g_backend_ready = true;
  fc::g_dynamic_d3d12 = true;
  fc::g_dynamic_support_seen = true;
  fc::g_dynamic_supported = true;
  fc::g_dynamic_mfg_enabled = true;
  options.mode = sl::DLSSGMode::eOff;
  options.numFramesToGenerate = 1;
  fc::internal::HookedSetOptions(viewport, options);
  Check(g_received_mode == sl::DLSSGMode::eOff && !fc::g_dynamic_applied.load(),
        "Ada eOff is never reactivated by Dynamic policy");

  Reset(Architecture::kAmpere);
  fc::g_dynamic_d3d12 = true;
  fc::g_dynamic_support_seen = true;
  fc::g_dynamic_supported = true;
  fc::g_dynamic_mfg_enabled = true;
  fc::g_multi_frame_ready = nullptr;
  options.structVersion = sl::kStructVersion1;
  options.mode = sl::DLSSGMode::eOn;
  options.numFramesToGenerate = 1;
  fc::internal::HookedSetOptions(viewport, options);
  Check(g_received_mode == sl::DLSSGMode::eDynamic && fc::g_dynamic_applied.load(),
        "Dynamic is submitted even without a backport-readiness callback");

  Reset(Architecture::kAmpere);
  g_backend_ready = true;
  fc::g_dynamic_d3d12 = true;
  g_dynamic_supported = true;
  state.structVersion = sl::kStructVersion2;
  g_presented_per_query = 7;
  state.next = &options;
  fc::internal::HookedGetState(viewport, state, nullptr);
  Check(g_state_calls == 2 && fc::g_dynamic_support_seen.load() &&
            fc::g_dynamic_supported.load(),
        "old caller state gets one addon-owned v4 capability probe");
  Check(state.numFramesActuallyPresented == 14 && !g_probe_extension_seen && state.next == &options,
        "extended probe preserves consumed presentation counts without replaying caller extensions");
  state.next = nullptr;

  Reset(Architecture::kAmpere);
  g_backend_ready = true;
  fc::g_dynamic_d3d12 = true;
  fc::g_dynamic_stack_ready = [] { return false; };
  g_dynamic_supported = true;
  state.structVersion = sl::kStructVersion2;
  fc::internal::HookedGetState(viewport, state, nullptr);
  Check(g_state_calls == 2 && fc::g_dynamic_support_seen.load() && fc::g_dynamic_supported.load(),
        "typed v4 probe executes despite an unrelated runtime-stack veto");

  Reset(Architecture::kAda);
  g_backend_ready = true;
  g_software_pacing = true;
  options = sl::DLSSGOptions{};
  options.mode = sl::DLSSGMode::eAuto;
  options.numFramesToGenerate = 2;
  fc::internal::HookedSetOptions(viewport, options);
  Check(g_received_mode == sl::DLSSGMode::eAuto && g_received == 2,
        "game-owned Auto remains a distinct native mode");
  Check(fc::g_last_requested_mode.load() == static_cast<uint32_t>(sl::DLSSGMode::eAuto) &&
            fc::g_last_forwarded_mode.load() == static_cast<uint32_t>(sl::DLSSGMode::eAuto) &&
            fc::g_last_accepted_mode.load() == static_cast<uint32_t>(sl::DLSSGMode::eAuto),
        "Auto request forwarding and acceptance are observed without aliasing");
  fc::g_force_multiplier = 4;
  fc::internal::HookedSetOptions(viewport, options);
  Check(g_received_mode == sl::DLSSGMode::eAuto && g_received == 3,
        "fixed count override preserves Auto mode");

  Reset(Architecture::kAda);
  g_backend_ready = true;
  fc::g_dynamic_d3d12 = true;
  fc::g_dynamic_mfg_enabled = true;
  fc::g_dynamic_support_seen = true;
  fc::g_dynamic_supported = true;
  options = sl::DLSSGOptions{};
  options.structVersion = sl::kStructVersion5;
  options.mode = sl::DLSSGMode::eAuto;
  options.numFramesToGenerate = 2;
  fc::internal::HookedSetOptions(viewport, options);
  Check(fc::g_last_requested_mode.load() == static_cast<uint32_t>(sl::DLSSGMode::eAuto) &&
            fc::g_last_forwarded_mode.load() == static_cast<uint32_t>(sl::DLSSGMode::eDynamic) &&
            fc::g_last_accepted_mode.load() == static_cast<uint32_t>(sl::DLSSGMode::eDynamic),
        "explicit addon Dynamic override is distinguishable from the game's Auto request");

  options.numFramesToGenerate = 3;
  options.mode = sl::DLSSGMode::eOn;
  for (auto mode : {Architecture::kAuto, Architecture::kUnknown}) {
    Reset(mode);
    fc::g_force_multiplier = 6;
    fc::internal::HookedSetOptions(viewport, options);
    fc::internal::HookedGetState(viewport, state, nullptr);
    Check(g_received == 5 && state.numFramesToGenerateMax == 5,
          "unresolved profile does not veto an explicit typed MFG request");
  }
  sl::Preferences preferences{};
  fc::internal::g_real_init = Init;
  Reset(Architecture::kAmpere);
  g_enabled = false;
  fc::g_runtime_selection_mode = static_cast<unsigned int>(fc::RuntimeSelectionMode::kForceOta);
  fc::internal::InitWithPreferences(preferences, sl::kSDKVersion);
  Check(g_seen_flags == 0, "disabled preserves preferences");

  g_enabled = true;
  fc::g_runtime_selection_mode = static_cast<unsigned int>(fc::RuntimeSelectionMode::kForceOta);
  fc::internal::InitWithPreferences(preferences, sl::kSDKVersion);
  Check(g_seen_flags != 0 && static_cast<uint32_t>(preferences.flags) == 0,
        "force-OTA applies only to forwarded preferences");

  preferences.flags = static_cast<sl::PreferenceFlags>(
      static_cast<uint32_t>(sl::PreferenceFlags::eAllowOTA) |
      static_cast<uint32_t>(sl::PreferenceFlags::eLoadDownloadedPlugins));
  fc::g_runtime_selection_mode = static_cast<unsigned int>(fc::RuntimeSelectionMode::kPreferLocal);
  fc::internal::InitWithPreferences(preferences, sl::kSDKVersion);
  Check(g_seen_flags == 0 && static_cast<uint32_t>(preferences.flags) != 0,
        "prefer-local clears OTA flags only for forwarded preferences");

  g_seen_flags = 0;
  fc::g_runtime_selection_mode = static_cast<unsigned int>(fc::RuntimeSelectionMode::kGameDefault);
  fc::internal::InitWithPreferences(preferences, sl::kSDKVersion);
  Check(g_seen_flags == static_cast<uint32_t>(preferences.flags),
        "game-default preserves the native runtime policy");

  // Streamline 1.x can expose slInit without slGetFeatureFunction. That still
  // has to arm the capability preflight instead of rejecting the host.
  const auto legacy_module = reinterpret_cast<HMODULE>(0x5151);
  mock::modules[L"sl.interposer.dll"] = legacy_module;
  mock::exports[{legacy_module, "slInit"}] = reinterpret_cast<FARPROC>(Init);
  fc::g_on_init = ObserveInit;
  fc::g_init_compatible = [](HMODULE) { return true; };
  fc::g_runtime_selection_mode = static_cast<unsigned int>(fc::RuntimeSelectionMode::kGameDefault);
  fc::g_feature_function_hooked = false;
  fc::g_init_hooked = false;
  fc::internal::g_real_get_feature_function = nullptr;
  fc::internal::g_real_init = nullptr;
  mock::install_ok = true;
  fc::TryInstall();
  Check(fc::g_init_hooked.load() && !fc::g_feature_function_hooked.load() &&
            fc::internal::g_real_init == Init,
        "legacy-compatible host installs slInit without slGetFeatureFunction");
  fc::Uninstall();
  Check(!fc::g_init_hooked.load() && !fc::g_feature_function_hooked.load(),
        "Streamline hooks unload cleanly");

  Reset(Architecture::kAmpere);
  g_backend_ready = true;
  g_software_pacing = true;
  fc::g_force_multiplier = 4;
  options = sl::DLSSGOptions{};
  options.structVersion = sl::kStructVersion5;
  options.numFramesToGenerate = 1;
  options.enableUserInterfaceRecomposition = sl::Boolean::eTrue;
  options.uiBufferFormat = 29;
  options.dynamicTargetFrameRate = 144.0f;
  g_caller_options = &options;
  g_set_results = {sl::Result::eErrorFeatureNotSupported,
                   sl::Result::eErrorFeatureNotSupported, sl::Result::eOk};
  countobserver::g_callback = ObserveCount;
  fc::internal::HookedSetOptions(viewport, options);
  Check(g_caller_untouched && options.numFramesToGenerate == 1,
        "caller options remain unchanged even while override/retry execute");
  Check(g_event_count == 4 && g_events[0].operation == countobserver::Operation::kRequest &&
            g_events[0].value == 1 && g_events[1].value == 3 && g_events[2].value == 3 &&
            g_events[3].value == 1 && g_events[1].origin == countobserver::Origin::kFixed &&
            g_events[2].origin == countobserver::Origin::kRetry &&
            g_events[3].origin == countobserver::Origin::kNativeFallback &&
            g_events[3].result == static_cast<uint32_t>(sl::Result::eOk),
        "MfgProbe records native request then actual fixed/retry/fallback calls");
  Check(g_events[1].requested == 1 && g_events[2].requested == 1 &&
            g_events[3].requested == 1 && g_events[1].caller == g_events[0].caller &&
            g_events[2].caller == g_events[0].caller && g_events[0].caller != 0,
        "forward records retain original caller and request provenance");
  sl::DLSSGOptions copy{};
  Check(fc::internal::CopyOptions(options, copy) &&
            copy.enableUserInterfaceRecomposition == sl::Boolean::eTrue &&
            copy.uiBufferFormat == 29 && copy.dynamicTargetFrameRate == 144.0f,
        "owned fixed options preserve versioned UI and dynamic fields");

  Reset(Architecture::kAmpere);
  g_backend_ready = true;
  g_software_pacing = true;
  fc::g_force_multiplier = 4;
  options.numFramesToGenerate = 3;
  g_result = sl::Result::eErrorFeatureNotSupported;
  fc::internal::HookedSetOptions(viewport, options);
  Check(fc::g_fixed_override_status == fc::FixedOverrideStatus::kNativeRejected &&
            fc::g_last_effective_generated == 0,
        "matching request rejected by runtime is not reported as accepted");
  options.numFramesToGenerate = 1;
  fc::internal::HookedSetOptions(viewport, options);
  Check(fc::g_fixed_override_status == fc::FixedOverrideStatus::kFallbackRejected &&
            fc::g_last_effective_generated == 0,
        "failed override and failed native fallback never claim restoration");

  Reset(Architecture::kAmpere);
  g_backend_ready = true;
  fc::g_force_multiplier = 4;
  options.mode = sl::DLSSGMode::eDynamic;
  fc::internal::HookedSetOptions(viewport, options);
  Check(g_calls == 1 && g_received_mode == sl::DLSSGMode::eDynamic &&
            g_pacing_calls == 0 && fc::g_last_effective_generated == 0,
        "game-owned Dynamic is not rewritten to a fixed multiplier");
  options.mode = sl::DLSSGMode::eOn;
  fc::g_force_multiplier = 0;
  fc::internal::HookedSetOptions(viewport, options);
  Check(!fc::g_dynamic_applied.load(), "later fixed request clears stale Dynamic acceptance");

  Reset(Architecture::kAmpere);
  g_backend_ready = true;
  countobserver::g_callback = ObserveCount;
  fc::g_streamline_max_generated = 3;
  state.structVersion = sl::kStructVersion2;
  fc::internal::HookedGetState(viewport, state, nullptr);
  Check(g_event_count == 2 && g_events[0].operation == countobserver::Operation::kRead &&
            g_events[0].value == 5 && g_events[1].operation == countobserver::Operation::kAdvertise &&
            g_events[1].value == 5,
        "MfgProbe preserves larger native capability separately from structural estimate");
  state.structVersion = sl::kStructVersion4 + 1;
  fc::internal::HookedGetState(viewport, state, nullptr);
  Check(state.numFramesToGenerateMax == 5 && !g_events[2].value_known && !g_events[3].value_known,
        "unknown state ABI is returned unchanged and count is not guessed");
  countobserver::g_callback = nullptr;

  Reset(Architecture::kAda);
  g_backend_ready = true;
  g_result = sl::Result::eOk;
  options = sl::DLSSGOptions{};
  options.structVersion = sl::kStructVersion5;
  options.mode = sl::DLSSGMode::eOn;
  options.numFramesToGenerate = 3;
  fc::internal::HookedSetOptions(viewport, options);
  auto presentation_state = presentation::Read();
  Check(presentation_state.accepted_mode_known &&
            presentation_state.accepted_mode == presentation::ProviderMode::kFixed &&
            presentation_state.accepted_multiplier_known &&
            presentation_state.accepted_multiplier == 4,
        "presentation observer records accepted fixed mode without claiming generated output");
  Check(!presentation_state.generated_multiplier_known,
        "accepted fixed mode does not fabricate actual generation evidence");

  Reset(Architecture::kAda);
  const auto plugin_module = reinterpret_cast<HMODULE>(0x7777);
  fc::internal::g_set_options_entry.identity.module = plugin_module;
  fc::internal::g_set_options_entry.installed = true;
  fc::internal::g_set_options_entry.trampoline = reinterpret_cast<void*>(&SetOptions);
  fc::internal::g_get_state_entry.identity.module = plugin_module;
  fc::internal::g_get_state_entry.installed = true;
  fc::internal::g_get_state_entry.trampoline = reinterpret_cast<void*>(&GetState);
  fc::g_streamline_plugin_seen = true;
  fc::g_streamline_max_generated = 5;
  fc::g_runtime_max_generated = 5;
  fc::g_dynamic_support_seen = true;
  fc::g_dynamic_supported = true;
  fc::g_dynamic_applied = true;
  mock::module_current[plugin_module] = false;
  Check(fc::internal::RealSetOptions() == nullptr && fc::internal::RealGetState() == nullptr,
        "stale plugin entries are never called after the image disappears");
  fc::NotifyDlssgPluginUnloaded(plugin_module);
  Check(!fc::internal::g_set_options_entry.installed.load() &&
            !fc::internal::g_get_state_entry.installed.load() &&
            fc::internal::g_real_set_options == nullptr &&
            fc::internal::g_real_get_state == nullptr,
        "plugin unload clears SetOptions/GetState entry pointers");
  Check(!fc::g_streamline_plugin_seen.load() && fc::g_streamline_max_generated.load() == 0 &&
            fc::g_runtime_max_generated.load() == 0 && !fc::g_effective_max_seen.load() &&
            fc::g_last_forwarded_mode.load() == mfgunlock::diagnostic::kUnknown32 &&
            fc::g_last_accepted_mode.load() == mfgunlock::diagnostic::kUnknown32 &&
            !fc::g_dynamic_support_seen.load() && !fc::g_dynamic_supported.load() &&
            !fc::g_dynamic_applied.load(),
        "plugin unload clears structural, accepted-mode and Dynamic runtime observations");

  Reset(Architecture::kAda);
  integration::Reset();
  fc::g_ui_composition_enabled = false;
  fc::internal::g_real_set_constants = SetConstantsForEvidence;
  fc::internal::g_real_set_tag_for_frame = SetTagsForEvidence;
  fc::internal::g_real_pcl_set_marker = SetMarkerForEvidence;
  sl::Constants constants{};
  constants.reset = sl::Boolean::eFalse;
  const sl::FrameToken frame = 80;
  g_result = sl::Result::eOk;
  fc::internal::HookedSetConstants(constants, frame, viewport);
  Check(integration::Read().constants_frame_known && integration::Read().history_reset_known &&
        !integration::Read().rendering_game_frames_known,
        "successful real-SDK constants record reset without fabricated gameplay field");
  sl::Resource resource(sl::ResourceType::eTex2d, reinterpret_cast<void*>(0x800), 0x40);
  resource.nativeFormat = 28;
  sl::Extent extent{};
  extent.width = 1920;
  extent.height = 1080;
  sl::ResourceTag tag(&resource, sl::kBufferTypeBackbuffer,
                       sl::ResourceLifecycle::eValidUntilPresent, &extent);
  fc::internal::HookedSetTagForFrame(frame, viewport, &tag, 1, nullptr);
  uint32_t bound_viewport = UINT32_MAX;
  Check(integration::ReadTaggedBackbuffer(bound_viewport) == 0x800 &&
        bound_viewport == static_cast<uint32_t>(viewport),
        "only a current successful per-frame tag can bind presentation source");
  fc::g_ui_pair_eligible = true;
  fc::g_ui_candidate_options_synced = true;
  g_result = sl::Result::eErrorNotInitialized;
  fc::internal::HookedSetTagForFrame(frame, viewport, &tag, 1, nullptr);
  Check(!integration::Read().resource[0].active &&
        integration::ReadTaggedBackbuffer(bound_viewport) == 0,
        "failed tag submission cannot leave a live presentation resource");
  Check(!fc::g_ui_pair_eligible.load() && !fc::g_ui_candidate_options_synced.load(),
        "failed native tag submission also invalidates UIR eligibility");
  integration::ObserveHistory(true, false, true, true);
  fc::internal::HookedSetConstants(constants, frame, viewport);
  Check(!integration::Read().history_reset_known && !integration::Read().constants_frame_known,
        "failed constants invalidate previous history evidence");
  fc::internal::HookedPclSetMarker(sl::PCLMarker::ePresentStart, frame);
  Check(!integration::Read().present_start_known, "failed marker is not reported as accepted");
  g_result = sl::Result::eOk;
  fc::internal::HookedPclSetMarker(sl::PCLMarker::ePresentStart, frame);
  Check(integration::Read().present_start_known, "successful marker is observed without rewriting");
  constants.structVersion = 99;
  fc::internal::HookedSetConstants(constants, frame, viewport);
  Check(!integration::Read().constants_frame_known, "unknown Constants ABI passes through but is not decoded");
  presentation::ObserveProviderOptions(presentation::ProviderMode::kFixed, 4);
  g_result = sl::Result::eErrorNotInitialized;
  state = sl::DLSSGState{};
  fc::internal::HookedGetState(viewport, state, nullptr);
  Check(!presentation::Read().effective_multiplier_known &&
        presentation::Read().confidence == presentation::Confidence::kUnknown,
        "failed native GetState clears current delivery evidence, not last accepted options");
  // Configured menu advertisement and explicit native requests are separate.
  // A smaller advertised value does not become a native-request veto.
  for (auto arch : {Architecture::kAmpere, Architecture::kTuring, Architecture::kAda}) {
    Reset(arch);
    g_backend_ready = true;
    g_software_pacing = true;
    fc::g_provider_mfg_ceiling = [] { return 3u; };
    fc::g_force_multiplier = 5;
    options = sl::DLSSGOptions{};
    options.mode = sl::DLSSGMode::eOn;
    options.numFramesToGenerate = 1;
    fc::internal::HookedSetOptions(viewport, options);
    Check(g_received == 4 && fc::g_fixed_override_status == fc::FixedOverrideStatus::kApplied,
          "native setter decides an explicit request on every architecture");
    fc::g_force_multiplier = 4;
    fc::internal::HookedSetOptions(viewport, options);
    Check(g_received == 3, "native setter receives the next explicit fixed request");
    state = sl::DLSSGState{};
    fc::internal::HookedGetState(viewport, state, nullptr);
    Check(state.numFramesToGenerateMax == 5 && fc::g_runtime_max_generated == 5,
          "native capability is not lowered by a smaller configured advertisement");
    fc::g_provider_mfg_ceiling = [] { return 0u; };
    fc::internal::HookedSetOptions(viewport, options);
    Check(g_received == 3, "native advertisement setting does not block fixed submission");
  }
  // R6: a typed native attempt, not a prediction from API/profile/capability.
  for (auto api : {static_cast<outputstate::ResourceApi>(255), outputstate::ResourceApi::kD3D12,
                   outputstate::ResourceApi::kVulkan}) {
    for (auto arch : {Architecture::kUnknown, Architecture::kAuto, Architecture::kAmpere,
                      Architecture::kTuring, Architecture::kAda}) {
      for (unsigned capability = 0; capability < 3; ++capability) {
        Reset(arch);
        outputstate::SetResourceApi(api);
        fc::g_dynamic_mfg_enabled = true;
        fc::g_dynamic_target_fps = 165;
        fc::g_dynamic_stack_ready = [] { return false; };
        fc::g_multi_frame_ready = nullptr;
        fc::g_streamline_plugin_seen = false;
        fc::g_streamline_max_generated = 0;
        fc::g_provider_mfg_ceiling = [] { return 0u; };
        fc::g_force_multiplier = 6;
        g_dynamic_supported = capability == 1;
        g_write_dynamic_flag = capability != 2;
        options = sl::DLSSGOptions{};
        options.structVersion = sl::kStructVersion3;
        options.mode = sl::DLSSGMode::eOn;
        options.numFramesToGenerate = 3;
        const auto result = fc::internal::HookedSetOptions(viewport, options);
        Check(result == sl::Result::eOk && g_calls == 1 && g_state_calls == 1 &&
              g_state_versions == std::vector<uint32_t>{4} &&
              g_received_modes == std::vector<sl::DLSSGMode>{sl::DLSSGMode::eDynamic} &&
              g_received_version == 5 && g_received_dynamic_target == 165.0f,
              "R6 native v4 probe and v5 Dynamic call execute on every API/profile without readiness predictions");
        Check(options.structVersion == 3 && options.mode == sl::DLSSGMode::eOn &&
              options.numFramesToGenerate == 3 && fc::g_dynamic_applied.load() &&
              g_pacing_calls == 0 && fc::g_dynamic_request_calls.load() == 1,
              "R6 real native acceptance leaves game options and NVIDIA pacing unchanged");
        Check(fc::g_dynamic_support_seen.load() == (capability != 2) &&
              fc::g_dynamic_supported.load() == (capability == 1),
              "R6 untouched capability sentinel remains unobserved rather than fabricated false");
        const auto logs_after_first = reshade::log::lines.size();
        for (unsigned repeat = 0; repeat < 64; ++repeat)
          fc::internal::HookedSetOptions(viewport, options);
        Check(g_state_calls == 1 && g_calls == 65 &&
              reshade::log::lines.size() == logs_after_first,
              "R6 successful Dynamic preserves native calls without repeated probes or success logs");
      }
    }
  }

  Reset(Architecture::kUnknown);
  outputstate::SetResourceApi(outputstate::ResourceApi::kVulkan);
  fc::g_dynamic_mfg_enabled = true;
  fc::internal::g_real_get_state = nullptr;
  options = sl::DLSSGOptions{};
  fc::internal::HookedSetOptions(viewport, options);
  Check(g_calls == 1 && g_received_mode == sl::DLSSGMode::eDynamic &&
        fc::g_dynamic_applied.load() && !fc::g_dynamic_support_seen.load(),
        "R6 missing GetState does not veto an available typed Dynamic setter");

  Reset(Architecture::kUnknown);
  outputstate::SetResourceApi(outputstate::ResourceApi::kVulkan);
  fc::g_dynamic_mfg_enabled = true;
  g_get_results = {sl::Result::eErrorFeatureNotSupported};
  options = sl::DLSSGOptions{};
  fc::internal::HookedSetOptions(viewport, options);
  Check(g_state_calls == 1 && g_calls == 1 && g_received_mode == sl::DLSSGMode::eDynamic &&
        fc::g_dynamic_probe_result.load() == static_cast<uint32_t>(sl::Result::eErrorFeatureNotSupported) &&
        fc::g_dynamic_applied.load(),
        "R6 a rejected v4 query is recorded but does not veto the real Dynamic setter");

  Reset(Architecture::kAmpere);
  fc::g_dynamic_mfg_enabled = true;
  g_presented_per_query = 7;
  options = sl::DLSSGOptions{};
  options.next = &state;
  fc::internal::HookedSetOptions(viewport, options);
  state = sl::DLSSGState{};
  state.structVersion = 3;
  state.next = &options;
  fc::internal::HookedGetState(viewport, state, &options);
  Check(g_state_calls == 2 && state.numFramesActuallyPresented == 14 &&
        state.next == &options && options.next == &state &&
        fc::g_dynamic_probe_calls.load() == 1,
        "R6 probe before setter carries consumed presentation counts into the next native state");
  // The second (native) query may legitimately have extensions; inspect the
  // probe independently to ensure no caller chain was replayed there.
  Reset(Architecture::kAmpere);
  fc::g_dynamic_mfg_enabled = true;
  options = sl::DLSSGOptions{};
  options.next = &state;
  fc::internal::HookedSetOptions(viewport, options);
  Check(g_state_chains == std::vector<const void*>{nullptr} &&
        g_state_options_chains == std::vector<const void*>{nullptr} && options.next == &state,
        "R6 addon-owned capability query does not consume caller-owned extension chains");

  Reset(Architecture::kAmpere);
  outputstate::SetResourceApi(outputstate::ResourceApi::kVulkan);
  fc::g_force_multiplier = 6;
  options = sl::DLSSGOptions{};
  g_set_results = {sl::Result::eErrorFeatureNotSupported, sl::Result::eErrorFeatureNotSupported,
                   sl::Result::eOk};
  fc::internal::HookedSetOptions(viewport, options);
  Check(g_received_counts == std::vector<unsigned>{5, 5, 1},
        "R6 fixed rejection is established by two actual calls before native fallback");
  const auto fixed_logs = reshade::log::lines.size();
  for (unsigned repeat = 0; repeat < 1000; ++repeat)
    fc::internal::HookedSetOptions(viewport, options);
  Check(g_calls == 1003 && fc::g_fixed_suppressed_repeats.load() == 1000 &&
        reshade::log::lines.size() == fixed_logs &&
        fc::g_fixed_override_status.load() == fc::FixedOverrideStatus::kRejectedNativeFallback,
        "R6 thousand identical rejected fixed overrides only forward native request and emit no new logs");
  options.colorWidth = 1920;
  fc::internal::HookedSetOptions(viewport, options);
  Check(g_received == 5 && fc::g_fixed_override_status.load() == fc::FixedOverrideStatus::kApplied,
        "R6 changed native render context retries previously rejected fixed count");
  const auto accepted_logs = reshade::log::lines.size();
  for (unsigned repeat = 0; repeat < 1000; ++repeat)
    fc::internal::HookedSetOptions(viewport, options);
  Check(reshade::log::lines.size() == accepted_logs,
        "R6 fixed success does not cycle Pending and Applied or spam logs");

  for (unsigned change = 0; change < 7; ++change) {
    Reset(Architecture::kUnknown);
    outputstate::SetResourceApi(outputstate::ResourceApi::kVulkan);
    fc::g_dynamic_mfg_enabled = true;
    options = sl::DLSSGOptions{};
    g_set_results = {sl::Result::eErrorFeatureNotSupported, sl::Result::eErrorFeatureNotSupported,
                     sl::Result::eOk};
    fc::internal::HookedSetOptions(viewport, options);
    Check(g_received_modes == std::vector<sl::DLSSGMode>{sl::DLSSGMode::eDynamic,
              sl::DLSSGMode::eDynamic, sl::DLSSGMode::eOn} &&
          fc::g_dynamic_runtime_declined.load() && !fc::g_dynamic_applied.load(),
          "R6 failed Dynamic request has real error evidence and forwards native fallback");
    const auto rejected_logs = reshade::log::lines.size();
    for (unsigned repeat = 0; repeat < 100; ++repeat)
      fc::internal::HookedSetOptions(viewport, options);
    Check(g_calls == 103 && fc::g_dynamic_request_calls.load() == 2 &&
          fc::g_dynamic_suppressed_repeats.load() == 100 &&
          reshade::log::lines.size() == rejected_logs,
          "R6 identical runtime-rejected Dynamic requests wait without repeated setter attempts or logs");
    auto retry_viewport = viewport;
    switch (change) {
      case 0: fc::NotifyDynamicModeChanged(); break;
      case 1: fc::internal::InvalidateLiveOptions(); break;
      case 2: options.colorHeight = 1080; break;
      case 3: retry_viewport = sl::ViewportHandle(1); break;
      case 4: outputstate::SetResourceApi(outputstate::ResourceApi::kD3D12); break;
      case 5: fc::g_dynamic_target_fps = 144; break;
      case 6:
        options.mode = sl::DLSSGMode::eOff;
        fc::internal::HookedSetOptions(viewport, options);
        Check(g_received_mode == sl::DLSSGMode::eOff, "R6 native OFF is never promoted");
        options.mode = sl::DLSSGMode::eOn;
        break;
    }
    fc::internal::HookedSetOptions(retry_viewport, options);
    Check(g_received_mode == sl::DLSSGMode::eDynamic && fc::g_dynamic_applied.load() &&
          !fc::g_dynamic_runtime_declined.load(),
          "R6 setting/provider/viewport/API/native-context/OFF-on change permits a fresh Dynamic attempt");
  }

  Reset(Architecture::kAmpere);
  fc::g_dynamic_mfg_enabled = true;
  options = sl::DLSSGOptions{};
  g_result = sl::Result::eErrorFeatureNotSupported;
  const auto all_failed = fc::internal::HookedSetOptions(viewport, options);
  Check(all_failed == sl::Result::eErrorFeatureNotSupported && !fc::g_dynamic_applied.load() &&
        fc::g_last_accepted_mode.load() == diagnostic::kUnknown32,
        "R6 failed Dynamic and failed native fallback never fabricate active mode or success");

  // These allocations end at the old public ABI boundary. Sanitizers can
  // detect an accidental read/write of the later members during promotion.
  Reset(Architecture::kUnknown);
  outputstate::SetResourceApi(outputstate::ResourceApi::kVulkan);
  fc::g_dynamic_mfg_enabled = true;
  options = sl::DLSSGOptions{};
  options.structVersion = 1;
  constexpr size_t old_options_size = offsetof(sl::DLSSGOptions, bReserved15);
  auto old_options_bytes = std::make_unique<std::byte[]>(old_options_size);
  std::memcpy(old_options_bytes.get(), &options, old_options_size);
  auto& old_options = *reinterpret_cast<sl::DLSSGOptions*>(old_options_bytes.get());
  fc::internal::HookedSetOptions(viewport, old_options);
  Check(g_received_version == 5 && g_received_mode == sl::DLSSGMode::eDynamic &&
        std::memcmp(old_options_bytes.get(), &options, old_options_size) == 0,
        "R6 physically short v1 options safely promote through owned v5 storage");

  Reset(Architecture::kUnknown);
  outputstate::SetResourceApi(outputstate::ResourceApi::kVulkan);
  state = sl::DLSSGState{};
  state.structVersion = 2;
  g_presented_per_query = 4;
  constexpr size_t old_state_size = offsetof(sl::DLSSGState, bReserved4);
  auto old_state_bytes = std::make_unique<std::byte[]>(old_state_size);
  std::memcpy(old_state_bytes.get(), &state, old_state_size);
  auto& old_state = *reinterpret_cast<sl::DLSSGState*>(old_state_bytes.get());
  fc::internal::HookedGetState(viewport, old_state, nullptr);
  Check(g_state_versions == std::vector<uint32_t>{2, 4} && old_state.structVersion == 2 &&
        old_state.numFramesActuallyPresented == 8,
        "R6 physically short v2 state stays within allocation while separate v4 probe executes");

  Reset(Architecture::kUnknown);
  fc::g_dynamic_mfg_enabled = true;
  g_presented_per_query = UINT32_MAX;
  options = sl::DLSSGOptions{};
  fc::internal::HookedSetOptions(viewport, options);
  state = sl::DLSSGState{};
  fc::internal::HookedGetState(viewport, state, nullptr);
  Check(state.numFramesActuallyPresented == UINT32_MAX,
        "R6 adding consumed probe presentations saturates rather than wraps the public counter");

  Reset(Architecture::kUnknown);
  outputstate::SetResourceApi(outputstate::ResourceApi::kVulkan);
  fc::g_streamline_max_generated = 3;
  fc::g_multi_frame_ready = [] { return false; };
  g_native_max = 1;
  state = sl::DLSSGState{};
  fc::internal::HookedGetState(viewport, state, nullptr);
  Check(state.numFramesToGenerateMax == 5 && fc::g_runtime_max_generated.load() == 1,
        "R6 configured and native maximum remain separate without scanner or readiness veto");
  fc::g_provider_mfg_ceiling = [] { return 17u; };
  fc::g_streamline_max_generated = 0;
  fc::internal::HookedGetState(viewport, state, nullptr);
  Check(state.numFramesToGenerateMax == 17 && fc::g_runtime_max_generated.load() == 1,
        "R6 explicit maximum17 is advertised without a scanner observation");
  fc::g_provider_mfg_ceiling = [] { return 0u; };
  fc::internal::HookedGetState(viewport, state, nullptr);
  Check(state.numFramesToGenerateMax == 1,
        "R6 zero advertisement preserves the original native maximum");

  Reset(Architecture::kUnknown);
  g_dynamic_supported = true;
  g_presented_per_query = 7;
  g_invalidate_probe_context = true;
  fc::internal::ProbeDynamicSupport(viewport, nullptr);
  Check(g_state_calls == 1 && !fc::g_dynamic_support_seen.load(),
        "R6 an in-flight v4 result cannot publish capability into a new provider epoch");
  state = sl::DLSSGState{};
  state.structVersion = sl::kStructVersion4;
  fc::internal::HookedGetState(viewport, state, nullptr);
  Check(state.numFramesActuallyPresented == 7 && g_state_calls == 2,
        "R6 consumed presentations from a prior provider epoch never leak into the replacement");

  Reset(Architecture::kAmpere);
  fc::g_ui_composition_enabled = true;
  outputstate::Observe(outputstate::FormatKind::kRgba8Unorm, 28, true,
                       outputstate::kColorSpaceSrgbNonlinear, true);
  options = sl::DLSSGOptions{};
  g_set_results = {sl::Result::eErrorFeatureNotSupported, sl::Result::eOk};
  fc::internal::HookedSetOptions(viewport, options);
  Check(g_calls == 2 && fc::g_ui_composition_fell_back.load() && !fc::g_ui_composition_applied.load(),
        "R6 UI-composition fallback follows an actual native rejection");
  const auto ui_failure_logs = reshade::log::lines.size();
  for (unsigned repeat = 0; repeat < 100; ++repeat)
    fc::internal::HookedSetOptions(viewport, options);
  Check(g_calls == 102 && reshade::log::lines.size() == ui_failure_logs &&
        fc::g_ui_composition_result.load() == static_cast<uint32_t>(sl::Result::eErrorFeatureNotSupported),
        "R6 identical rejected UI options do not add a failed native call or duplicate log per frame");

  Reset(Architecture::kUnknown);
  outputstate::SetResourceApi(outputstate::ResourceApi::kD3D12);
  fc::g_force_multiplier = 4;
  options = sl::DLSSGOptions{};
  for (unsigned repeat = 0; repeat < 1000; ++repeat)
    fc::internal::HookedSetOptions(viewport, options);
  Check(g_pacing_calls == 1 && g_calls == 1000 && g_received == 3,
        "R6 unresolved D3D pacing is prepared once per context while all native requests execute");
  fc::internal::InvalidateLiveOptions();
  fc::internal::HookedSetOptions(viewport, options);
  Check(g_pacing_calls == 2 && g_calls == 1001 && g_received == 3,
        "R6 provider epoch rearms best-effort pacing without count veto");
  fc::NotifyFixedMultiplierChanged();
  fc::internal::HookedSetOptions(viewport, options);
  Check(g_pacing_calls == 3 && g_received == 3,
        "R6 explicit setting revision rearms pacing attempt");

  Reset(Architecture::kUnknown);
  outputstate::SetResourceApi(outputstate::ResourceApi::kD3D12);
  fc::g_force_multiplier = 4;
  fc::g_ensure_pacing = nullptr;
  fc::internal::HookedSetOptions(viewport, options);
  fc::g_ensure_pacing = [] { ++g_pacing_calls; };
  fc::internal::HookedSetOptions(viewport, options);
  Check(g_pacing_calls == 1 && g_calls == 2 && g_received == 3,
        "R6 absence of a pacing callback is never cached as an attempted preparation");

  // Only a measured initialization error gets bounded retry opportunities.
  // Definitive feature rejection above stays cached until a context change.
  Reset(Architecture::kUnknown);
  fc::g_force_multiplier = 17;
  fc::g_streamline_max_generated = 3;
  g_native_max = 1;
  options = sl::DLSSGOptions{};
  const auto extended_fixed = fc::internal::HookedSetOptions(viewport, options);
  Check(extended_fixed == sl::Result::eOk && g_calls == 1 && g_received == 16 &&
        options.numFramesToGenerate == 1 && !fc::g_state_seen.load(),
        "R6 explicit multiplier17 reaches native count16 despite old native/scan maxima and does not invent delivery");

  Reset(Architecture::kAmpere);
  fc::g_dynamic_mfg_enabled = true;
  options = sl::DLSSGOptions{};
  g_set_results = {sl::Result::eErrorNotInitialized, sl::Result::eErrorNotInitialized, sl::Result::eOk};
  fc::internal::HookedSetOptions(viewport, options);
  for (unsigned repeat = 0; repeat < 8; ++repeat)
    fc::internal::HookedSetOptions(viewport, options);
  Check(fc::g_dynamic_request_calls.load() == 2 && !fc::g_dynamic_applied.load(),
        "R6 initialization failure backs off before another real request");
  fc::internal::HookedSetOptions(viewport, options);
  Check(fc::g_dynamic_request_calls.load() == 3 && fc::g_dynamic_applied.load(),
        "R6 startup initialization failure can recover without a user toggle");

  Reset(Architecture::kAmpere);
  fc::g_dynamic_mfg_enabled = true;
  options = sl::DLSSGOptions{};
  g_set_results = {sl::Result::eErrorNotInitialized, sl::Result::eErrorNotInitialized, sl::Result::eOk};
  fc::internal::HookedSetOptions(viewport, options);
  state = sl::DLSSGState{};
  state.structVersion = 3;
  fc::internal::HookedGetState(viewport, state, nullptr);
  fc::internal::HookedSetOptions(viewport, options);
  Check(g_received_mode == sl::DLSSGMode::eDynamic && fc::g_dynamic_applied.load(),
        "R6 first observed healthy runtime state rearms a prior initialization failure");

  Reset(Architecture::kAmpere);
  fc::g_dynamic_mfg_enabled = true;
  options = sl::DLSSGOptions{};
  g_result = sl::Result::eErrorNotInitialized;
  for (unsigned repeat = 0; repeat < 1000; ++repeat)
    fc::internal::HookedSetOptions(viewport, options);
  Check(fc::g_dynamic_request_calls.load() == 8,
        "R6 initialization retries are bounded to initial plus three backed-off double attempts per context");

  Reset(Architecture::kAda);
  g_backend_ready = true; fc::g_dynamic_d3d12 = true;
  state = sl::DLSSGState{}; state.structVersion = 99;
  Check(!fc::internal::ObserveDynamicSupport(state, sl::Result::eOk), "future unqualified State never read as v4");
  state.structVersion = sl::kStructVersion4;
  state.structType = 0;
  Check(!fc::internal::ObserveDynamicSupport(state, sl::Result::eOk), "wrong State type never supplies Dynamic proof");
  for (float target : {0.0f, 0.25f, 600.5f, 1000.0f, 16777216.0f,
                       (std::numeric_limits<float>::max)()}) {
    Reset(Architecture::kAmpere);
    fc::g_dynamic_mfg_enabled = true;
    fc::g_dynamic_target_fps = target;
    options = sl::DLSSGOptions{};
    options.mode = sl::DLSSGMode::eOn;
    const auto original = options;
    Check(fc::internal::HookedSetOptions(viewport, options) == sl::Result::eOk &&
          g_received_mode == sl::DLSSGMode::eDynamic && g_received_dynamic_target == target,
          "R6.3 forwards finite float targets, including fractions and values above 480");
    Check(options.dynamicTargetFrameRate == original.dynamicTargetFrameRate &&
          options.mode == original.mode && !fc::g_state_seen.load(),
          "FPS target forwarding leaves caller options and presentation evidence unchanged");
  }
  Reset(Architecture::kAmpere);
  fc::g_dynamic_mfg_enabled = true;
  fc::g_dynamic_target_fps = 1000.5f;
  options = sl::DLSSGOptions{};
  options.mode = sl::DLSSGMode::eOn;
  options.dynamicTargetFrameRate = 123.5f;
  g_set_results = {sl::Result::eErrorFeatureNotSupported,
                   sl::Result::eErrorFeatureNotSupported, sl::Result::eOk};
  Check(fc::internal::HookedSetOptions(viewport, options) == sl::Result::eOk &&
        fc::g_dynamic_fell_back.load() && !fc::g_dynamic_applied.load() &&
        fc::g_dynamic_result.load() == static_cast<unsigned int>(sl::Result::eErrorFeatureNotSupported),
        "R6.3 native rejection above 480 is not relabelled dynamic success");
  Check(g_received_mode == sl::DLSSGMode::eOn && g_received_dynamic_target == 123.5f &&
        options.dynamicTargetFrameRate == 123.5f,
        "R6.3 native fallback preserves original target and caller options");
  for (float bad : {-1.0f, std::numeric_limits<float>::infinity(),
                   std::numeric_limits<float>::quiet_NaN()}) {
    Reset(Architecture::kAmpere);
    fc::g_dynamic_target_fps = bad;
    sl::DLSSGOptions built{};
    Check(!fc::internal::BuildDynamicOptions(options, built),
          "R6.3 invalid internal target cannot be sent as a valid dynamic request");
  }
  Reset(Architecture::kAmpere);
  fc::g_force_multiplier = 6;
  g_native_max = 3;
  fc::g_provider_mfg_ceiling = [] { return 5u; };
  state = sl::DLSSGState{};
  Check(fc::internal::HookedGetState(viewport, state, nullptr) == sl::Result::eOk &&
        state.numFramesToGenerateMax == 5 &&
        fc::g_native_maximum.load() == runtimecontrols::EncodeMaximum(3),
        "R6.3 keeps original Streamline maximum distinct from the advertised raise");
  options = sl::DLSSGOptions{};
  options.mode = sl::DLSSGMode::eOn;
  Check(fc::internal::HookedSetOptions(viewport, options) == sl::Result::eOk && g_received == 5,
        "R6.3 warning evidence does not block a 6x attempt over native 4x");
  g_result = sl::Result::eErrorNotInitialized;
  fc::internal::HookedGetState(viewport, state, nullptr);
  Check(fc::g_native_maximum.load() == runtimecontrols::kUnknownMaximum,
        "R6.3 a failed GetState cannot publish a stale maximum as current");
  g_result = sl::Result::eOk;
  state.structVersion = sl::kStructVersion1;
  fc::internal::HookedGetState(viewport, state, nullptr);
  Check(fc::g_native_maximum.load() == runtimecontrols::kUnknownMaximum,
        "R6.3 missing maximum field is unknown rather than an invented limit");
  state.structVersion = sl::kStructVersion4;
  g_native_max = 0;
  fc::internal::HookedGetState(viewport, state, nullptr);
  Check(fc::g_native_maximum.load() == runtimecontrols::EncodeMaximum(0),
        "R6.3 native zero is distinct from absence of evidence");
  fc::internal::InvalidateLiveOptions();
  Check(fc::g_native_maximum.load() == runtimecontrols::kUnknownMaximum,
        "R6.3 owner/context retirement invalidates the previous maximum");
  std::printf("PASS frame-count profiles: %u checks\n", g_checks);
}
