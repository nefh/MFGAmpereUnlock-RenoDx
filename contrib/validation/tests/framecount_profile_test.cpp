// SPDX-License-Identifier: MIT
#include "framecount.hpp"

#include <array>
#include <cstdio>
#include <cstdlib>
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
bool g_backend_ready = false;
bool g_software_pacing = false;
bool g_dynamic_supported = false;
sl::DLSSGMode g_received_mode = sl::DLSSGMode::eOn;
uint32_t g_received_version = 0;
float g_received_dynamic_target = 0.0f;
sl::Result g_result = sl::Result::eOk;
std::vector<sl::Result> g_set_results;
std::vector<unsigned int> g_received_counts;
uint32_t g_seen_flags = 0;
std::array<countobserver::Event, 32> g_events;
size_t g_event_count = 0;
const sl::DLSSGOptions* g_caller_options = nullptr;
bool g_caller_untouched = true;
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
  g_received_mode = options.mode;
  g_received_version = options.structVersion;
  g_received_dynamic_target = options.dynamicTargetFrameRate;
  if (g_calls <= g_set_results.size()) return g_set_results[g_calls - 1];
  return g_result;
}
sl::Result GetState(const sl::ViewportHandle&, sl::DLSSGState& state, const sl::DLSSGOptions*) {
  ++g_state_calls;
  state.numFramesToGenerateMax = g_native_max;
  if (state.structVersion >= sl::kStructVersion4) {
    state.bIsDynamicMFGSupported = g_dynamic_supported
        ? sl::Boolean::eTrue : sl::Boolean::eFalse;
  }
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
void Reset(Architecture mode) {
  countobserver::g_callback = nullptr;
  g_event_count = 0;
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
  fc::internal::g_real_set_options = SetOptions;
  fc::internal::g_real_get_state = GetState;
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
  fc::g_ensure_pacing = [] { ++g_pacing_calls; };
  fc::g_pacing_ready = [] { return g_software_pacing; };
  g_calls = 0;
  g_received_counts.clear();
  g_set_results.clear();
  g_state_calls = 0;
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
  sl::ViewportHandle viewport{};
  sl::DLSSGOptions options{};
  options.numFramesToGenerate = 3;
  sl::DLSSGState state{};

  for (auto mode : {Architecture::kAmpere, Architecture::kTuring}) {
    Reset(mode);
    Check(fc::internal::HookedSetOptions(viewport, options) == sl::Result::eOk, "backport forwards result");
    Check(g_calls == 1 && g_received == 1 && options.numFramesToGenerate == 3,
          "incomplete backport limits request without changing caller state");
    fc::internal::HookedGetState(viewport, state, nullptr);
    Check(state.numFramesToGenerateMax == 1, "backport capacity waits for preparation");
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
  Check(state.numFramesToGenerateMax == 3,
        "Streamline structural ceiling caps a larger runtime maximum");
  g_native_max = 1;
  fc::internal::HookedGetState(viewport, state, nullptr);
  Check(state.numFramesToGenerateMax == 3,
        "Streamline structural ceiling can replace stale runtime lowering");

  Reset(Architecture::kAmpere);
  g_backend_ready = true;
  g_software_pacing = true;
  fc::g_streamline_max_generated = 3;
  fc::g_force_multiplier = 6;
  options.structVersion = sl::kStructVersion1;
  options.mode = sl::DLSSGMode::eOn;
  options.numFramesToGenerate = 1;
  fc::internal::HookedSetOptions(viewport, options);
  Check(g_calls == 1 && g_received_counts == std::vector<unsigned int>{1} &&
            fc::g_fixed_override_status.load() == fc::FixedOverrideStatus::kBlockedStructuralCeiling &&
            fc::g_last_effective_generated.load() == 1,
        "6x override is not submitted above a 4x Streamline structural ceiling");

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

  Reset(Architecture::kAmpere);
  g_backend_ready = true;
  g_software_pacing = true;
  fc::g_streamline_max_generated = 0;
  fc::g_force_multiplier = 6;
  options.numFramesToGenerate = 1;
  fc::internal::HookedSetOptions(viewport, options);
  Check(g_calls == 1 && g_received_counts == std::vector<unsigned int>{1} &&
            fc::g_fixed_override_status.load() == fc::FixedOverrideStatus::kUnknownStructuralCeiling,
        "unknown active Streamline ceiling preserves the native request");

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
  Check(g_calls == 1 && g_received == 1 &&
            fc::g_fixed_override_status.load() == fc::FixedOverrideStatus::kBlockedPacing &&
            fc::g_last_effective_generated.load() == 1,
        "pacing block preserves and reports the native request");

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
  g_dynamic_supported = false;
  state.structVersion = sl::kStructVersion4;
  fc::internal::HookedGetState(viewport, state, nullptr);
  fc::g_dynamic_mfg_enabled = true;
  options.mode = sl::DLSSGMode::eOn;
  options.numFramesToGenerate = 1;
  fc::internal::HookedSetOptions(viewport, options);
  Check(fc::g_dynamic_support_seen.load() && !fc::g_dynamic_supported.load() &&
            g_received_mode == sl::DLSSGMode::eOn && !fc::g_dynamic_applied.load(),
        "Turing keeps fixed mode when runtime reports Dynamic unsupported");

  Reset(Architecture::kTuring);
  fc::g_dynamic_d3d12 = true;
  fc::g_dynamic_support_seen = true;
  fc::g_dynamic_supported = true;
  fc::g_dynamic_mfg_enabled = true;
  options.mode = sl::DLSSGMode::eOn;
  options.numFramesToGenerate = 1;
  fc::internal::HookedSetOptions(viewport, options);
  Check(g_received_mode == sl::DLSSGMode::eOn && !fc::g_dynamic_applied.load(),
        "Turing Dynamic never bypasses incomplete backend readiness");

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
  Check(g_received_mode == sl::DLSSGMode::eOn && !fc::g_dynamic_applied.load(),
        "Turing Dynamic rejects an unvalidated runtime stack");

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
  Check(g_received_mode == sl::DLSSGMode::eOn && !fc::g_dynamic_applied.load(),
        "Turing Dynamic waits for the structural Streamline ceiling");

  Reset(Architecture::kTuring);
  g_backend_ready = true;
  fc::g_dynamic_d3d12 = true;
  g_dynamic_supported = true;
  fc::g_streamline_max_generated = 3;
  state.structVersion = sl::kStructVersion4;
  g_native_max = 5;
  fc::internal::HookedGetState(viewport, state, nullptr);
  Check(state.numFramesToGenerateMax == 3,
        "Turing Dynamic keeps the Streamline structural ceiling authoritative");
  fc::g_dynamic_mfg_enabled = true;
  options.mode = sl::DLSSGMode::eOn;
  options.numFramesToGenerate = 1;
  fc::internal::HookedSetOptions(viewport, options);
  Check(g_received_mode == sl::DLSSGMode::eDynamic && fc::g_dynamic_applied.load(),
        "Turing Dynamic can run within the plugin structural ceiling");

  Reset(Architecture::kAda);
  g_backend_ready = true;
  fc::g_dynamic_d3d12 = true;
  fc::g_dynamic_support_seen = true;
  fc::g_dynamic_supported = true;
  fc::g_dynamic_mfg_enabled = true;
  options.mode = sl::DLSSGMode::eOn;
  options.numFramesToGenerate = 1;
  fc::internal::HookedSetOptions(viewport, options);
  Check(g_received_mode == sl::DLSSGMode::eOn && !fc::g_dynamic_applied.load(),
        "addon Dynamic policy does not replace the native Ada path");

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
  Check(g_received_mode == sl::DLSSGMode::eOn && !fc::g_dynamic_applied.load(),
        "Dynamic never bypasses unknown backport readiness");

  Reset(Architecture::kAmpere);
  g_backend_ready = true;
  fc::g_dynamic_d3d12 = true;
  g_dynamic_supported = true;
  state.structVersion = sl::kStructVersion2;
  fc::internal::HookedGetState(viewport, state, nullptr);
  Check(g_state_calls == 2 && fc::g_dynamic_support_seen.load() &&
            fc::g_dynamic_supported.load(),
        "old caller state gets one addon-owned v4 capability probe");

  Reset(Architecture::kAmpere);
  g_backend_ready = true;
  fc::g_dynamic_d3d12 = true;
  fc::g_dynamic_stack_ready = [] { return false; };
  g_dynamic_supported = true;
  state.structVersion = sl::kStructVersion2;
  fc::internal::HookedGetState(viewport, state, nullptr);
  Check(g_state_calls == 1 && !fc::g_dynamic_support_seen.load(),
        "unvalidated runtime never probes or trusts Dynamic v4 state");

  options.numFramesToGenerate = 3;
  options.mode = sl::DLSSGMode::eOn;
  for (auto mode : {Architecture::kAuto, Architecture::kUnknown}) {
    Reset(mode);
    fc::g_force_multiplier = 6;
    fc::internal::HookedSetOptions(viewport, options);
    fc::internal::HookedGetState(viewport, state, nullptr);
    Check(g_received == 3 && g_pacing_calls == 0 && state.numFramesToGenerateMax == 5,
          "unresolved profile leaves native MFG alone");
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
            g_events[1].value == 3,
        "MfgProbe separates native capability read from advertised structural bound");
  state.structVersion = sl::kStructVersion4 + 1;
  fc::internal::HookedGetState(viewport, state, nullptr);
  Check(state.numFramesToGenerateMax == 5 && !g_events[2].value_known && !g_events[3].value_known,
        "unknown state ABI is returned unchanged and count is not guessed");
  countobserver::g_callback = nullptr;

  std::printf("PASS frame-count profiles: %u checks\n", g_checks);
}
