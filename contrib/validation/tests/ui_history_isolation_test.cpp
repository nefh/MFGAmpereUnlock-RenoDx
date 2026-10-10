// SPDX-License-Identifier: MIT
// Production SL hooks with API doubles. No vendor DLLs or GPU execution.
#include "framecount.hpp"
#include <array>
#include <cstdio>
#include <cstring>
#include <string>

namespace fc = mfgunlock::framecount;
namespace os = mfgunlock::outputstate;
namespace {
unsigned checks = 0, failures = 0, native_calls = 0, forwarded_resets = 0;
const sl::Constants* expected_constants = nullptr;
sl::Result native_result = sl::Result::eOk;
sl::DLSSGOptions forwarded_options{};

void Check(bool value, const char* why) {
  ++checks;
  if (!value) { ++failures; std::fprintf(stderr, "FAIL: %s\n", why); }
}
sl::Result Constants(const sl::Constants& value, const sl::FrameToken&,
                     const sl::ViewportHandle&) {
  ++native_calls;
  forwarded_resets += value.reset == sl::Boolean::eTrue ? 1u : 0u;
  Check(expected_constants == &value, "common constants forwarded by original reference");
  Check(expected_constants && std::memcmp(expected_constants, &value, sizeof(value)) == 0,
        "common constants (including reset) preserved byte-for-byte");
  return native_result;
}
sl::Result Tags(const sl::FrameToken&, const sl::ViewportHandle&,
                const sl::ResourceTag*, uint32_t, sl::CommandBuffer*) {
  return sl::Result::eOk;
}
sl::Result Options(const sl::ViewportHandle&, const sl::DLSSGOptions& value) {
  forwarded_options = value;
  return sl::Result::eOk;
}
sl::Resource Resource(uint64_t id, uint32_t format) {
  sl::Resource r(sl::ResourceType::eTex2d, reinterpret_cast<void*>(id), 4);
  r.width = 1920; r.height = 1080; r.nativeFormat = format;
  return r;
}

void Reset() {
  native_calls = forwarded_resets = 0;
  native_result = sl::Result::eOk;
  mfgunlock::g_enabled = true;
  fc::g_ui_composition_enabled = true;
  fc::g_dynamic_mfg_enabled = false;
  fc::g_ui_candidate_injection_enabled = false;
  fc::g_observe_ui_candidate = nullptr;
  fc::g_observe_streamline_ui_tags = nullptr;
  fc::internal::g_real_set_constants = Constants;
  fc::internal::g_real_set_tag_for_frame = Tags;
  fc::internal::g_real_set_options = Options;
  fc::internal::ForgetUiOutputs();
  for (auto& state : fc::internal::g_ui_viewports) {
    state.key = fc::internal::kUnusedViewport;
    state.native_options_valid = false;
    state.options_seen = false;
    state.reset_requested = 0;
    state.reset_applied = 0;
  }
  fc::g_ui_resets_requested = 0;
  fc::g_ui_resets_injected = 0;
  os::Observe(os::FormatKind::kRgba8Unorm, 28, true, os::kColorSpaceSrgbNonlinear, true);
  sl::DLSSGOptions options{};
  options.structVersion = sl::kStructVersion5;
  options.mode = sl::DLSSGMode::eOn;
  options.colorWidth = 1920; options.colorHeight = 1080;
  options.colorBufferFormat = 28;
  fc::internal::ObserveUiOptionsTransition(1, options);
}
void SetConstants(uint32_t frame, sl::Constants& c, uint32_t viewport = 1) {
  const auto before = c;
  expected_constants = &c;
  const auto result = fc::internal::HookedSetConstants(c, frame, viewport);
  Check(result == native_result, "native constants result preserved without retry");
  Check(std::memcmp(&before, &c, sizeof(c)) == 0, "caller memory unchanged");
}
void Pair(uint32_t frame, bool split, bool ui_first) {
  auto hud = Resource(0x1000 + (frame % 3) * 0x100, 28);
  auto alpha = Resource(0x2000 + (frame % 3) * 0x100, 61);
  sl::ResourceTag h(&hud, sl::kBufferTypeHUDLessColor, sl::ResourceLifecycle::eValidUntilPresent);
  sl::ResourceTag a(&alpha, sl::kBufferTypeUIAlpha, sl::ResourceLifecycle::eValidUntilPresent);
  if (split) {
    fc::internal::HookedSetTagForFrame(frame, 1, ui_first ? &a : &h, 1, nullptr);
    fc::internal::HookedSetTagForFrame(frame, 1, ui_first ? &h : &a, 1, nullptr);
  } else {
    const std::array<sl::ResourceTag, 2> tags{h, a};
    fc::internal::HookedSetTagForFrame(frame, 1, tags.data(), 2, nullptr);
  }
  Check(fc::internal::GetUiState(1)->ui_recomposition_eligible.load(),
        "native UI/HUDless pair remains eligible, feature not disabled");
}
}

int main(int argc, char** argv) {
  const std::string select = argc > 1 ? argv[1] : "all";
  auto run = [&](const char* name) { return select == "all" || select == name; };
  for (const auto* mode : {"complete", "split_hud_first", "split_ui_first", "constants_first"}) {
    if (!run(mode)) continue;
    Reset();
    sl::Constants c{}; c.reset = sl::Boolean::eFalse;
    const bool split = std::string(mode).rfind("split_", 0) == 0;
    const bool ui_first = std::string(mode) == "split_ui_first";
    const bool constants_first = std::string(mode) == "constants_first";
    for (uint32_t frame = 1; frame <= 120; ++frame) {
      if (constants_first) SetConstants(frame, c);
      Pair(frame, split, ui_first);
      if (!constants_first) SetConstants(frame, c);
    }
    std::printf("%s: frames=120 native_calls=%u forwarded_reset_true=%u ui_requests=%llu injected=%llu\n",
        mode, native_calls, forwarded_resets,
        fc::g_ui_resets_requested.load(), fc::g_ui_resets_injected.load());
    Check(native_calls == 120, "one native constants call per input call");
    Check(forwarded_resets == 0, "steady UI must not reset SR/RR/FG history");
  }
  if (run("native_resets")) {
    Reset();
    for (uint32_t frame = 1; frame <= 120; ++frame) {
      Pair(frame, true, frame % 2 != 0);
      sl::Constants c{};
      c.reset = frame == 3 || frame == 70 ? sl::Boolean::eTrue : sl::Boolean::eFalse;
      SetConstants(frame, c);
    }
    Check(forwarded_resets == 2, "real application camera cuts preserved exactly, no extra resets");
  }
  if (run("transitions")) {
    Reset(); sl::Constants c{};
    for (uint32_t frame = 1; frame <= 12; ++frame) {
      Pair(frame, false, false);
      fc::NotifyUiCompositionChanged();
      fc::NotifyUiCandidateInjectionChanged();
      fc::NotifyOutputStateChanged();
      SetConstants(frame, c);
    }
    Check(forwarded_resets == 0, "addon UI and output invalidation does not reset unrelated SL features");
  }
  if (run("viewport_off_failure")) {
    Reset(); sl::Constants c{};
    fc::internal::RequestUiReset(fc::internal::GetUiState(1));
    SetConstants(1, c, 2);
    fc::g_ui_composition_enabled = false; SetConstants(2, c);
    fc::g_ui_composition_enabled = true;
    native_result = sl::Result::eErrorUnsupportedInterface;
    SetConstants(3, c);
    Check(native_calls == 3 && forwarded_resets == 0, "viewport isolation, off path and native failure passthrough");
  }
  if (run("unknown_abi")) {
    Reset(); sl::Constants c{}; c.structVersion = 99;
    fc::internal::RequestUiReset(fc::internal::GetUiState(1));
    SetConstants(1, c);
    Check(forwarded_resets == 0, "unknown ABI not modified");
  }
  if (run("uir_still_enabled")) {
    Reset(); sl::DLSSGOptions options{}; options.structVersion = sl::kStructVersion5;
    options.mode = sl::DLSSGMode::eOn; options.numFramesToGenerate = 5;
    Check(fc::internal::ForwardSetOptions(1, options) == sl::Result::eOk, "UI-capable options accepted");
    Check(forwarded_options.enableUserInterfaceRecomposition == sl::Boolean::eTrue,
          "fix does not disable UIR");
    Check(forwarded_options.numFramesToGenerate == 5, "MFG count unchanged by UI history fix");
    Check(options.enableUserInterfaceRecomposition == sl::Boolean::eFalse,
          "application options not overwritten");
  }
  std::printf("ui_history_isolation_test: %u checks, %u failures\n", checks, failures);
  return failures ? 1 : 0;
}
