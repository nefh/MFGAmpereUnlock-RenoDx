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
unsigned int g_native_max = 5;
bool g_backend_ready = false;
bool g_software_pacing = false;
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
  return g_result;
}
sl::Result GetState(const sl::ViewportHandle&, sl::DLSSGState& state, const sl::DLSSGOptions*) {
  state.numFramesToGenerateMax = g_native_max;
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
  fc::g_advertised_max_generated = 5;
  fc::internal::g_real_set_options = SetOptions;
  fc::internal::g_real_get_state = GetState;
  fc::g_multi_frame_ready = [] { return g_backend_ready; };
  fc::g_ensure_pacing = [] { ++g_pacing_calls; };
  fc::g_pacing_ready = [] { return g_software_pacing; };
  g_calls = 0;
  g_pacing_calls = 0;
  g_backend_ready = false;
  g_software_pacing = false;
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
    Check(g_received == 3 && options.numFramesToGenerate == 1, "common multiplier implementation");
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
  fc::g_force_ota = true;
  for (auto mode : {Architecture::kAuto, Architecture::kUnknown}) {
    Reset(mode);
    fc::internal::InitWithPreferences(preferences, sl::kSDKVersion);
    Check(g_seen_flags == 0, "unresolved profile preserves preferences");
  }
  Reset(Architecture::kAmpere);
  g_enabled = false;
  fc::internal::InitWithPreferences(preferences, sl::kSDKVersion);
  Check(g_seen_flags == 0, "disabled preserves preferences");
  g_enabled = true;
  fc::internal::InitWithPreferences(preferences, sl::kSDKVersion);
  Check(g_seen_flags != 0 && static_cast<uint32_t>(preferences.flags) == 0,
        "existing OTA option applies only during the native call");

  // Streamline 1.x can expose slInit without slGetFeatureFunction. That still
  // has to arm the capability preflight instead of rejecting the host.
  const auto legacy_module = reinterpret_cast<HMODULE>(0x5151);
  mock::modules[L"sl.interposer.dll"] = legacy_module;
  mock::exports[{legacy_module, "slInit"}] = reinterpret_cast<FARPROC>(Init);
  fc::g_on_init = ObserveInit;
  fc::g_init_compatible = [](HMODULE) { return true; };
  fc::g_force_ota = false;
  fc::g_hooked = false;
  fc::g_feature_function_hooked = false;
  fc::g_init_hooked = false;
  fc::internal::g_real_get_feature_function = nullptr;
  fc::internal::g_real_init = nullptr;
  mock::install_ok = true;
  fc::TryInstall();
  Check(fc::g_hooked.load() && fc::g_init_hooked.load() &&
            !fc::g_feature_function_hooked.load() && fc::internal::g_real_init == Init,
        "legacy Streamline installs slInit without slGetFeatureFunction");
  fc::Uninstall();
  Check(!fc::g_hooked.load() && !fc::g_init_hooked.load(),
        "legacy Streamline hook unloads cleanly");

  std::printf("PASS frame-count profiles: %u checks\n", g_checks);
}
