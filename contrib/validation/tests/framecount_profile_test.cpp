// SPDX-License-Identifier: MIT
#include "framecount.hpp"

#include <cstdio>
#include <cstdlib>

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
uint32_t g_seen_flags = 0;

void Check(bool value, const char* name) {
  ++g_checks;
  if (value) return;
  std::fprintf(stderr, "FAIL %s\n", name);
  std::exit(1);
}
sl::Result SetOptions(const sl::ViewportHandle&, const sl::DLSSGOptions& options) {
  ++g_calls;
  g_received = options.numFramesToGenerate;
  g_received_mode = options.mode;
  g_received_version = options.structVersion;
  g_received_dynamic_target = options.dynamicTargetFrameRate;
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
  architecture::Configure(mode);
  g_enabled = true;
  fc::g_force_multiplier = 0;
  fc::g_runtime_selection_mode = static_cast<unsigned int>(fc::RuntimeSelectionMode::kGameDefault);
  fc::g_advertised_max_generated = 5;
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

  Reset(Architecture::kAda);
  fc::g_multi_frame_ready = [] { return true; };
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

  std::printf("PASS frame-count profiles: %u checks\n", g_checks);
}
